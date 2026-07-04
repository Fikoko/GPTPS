/*
 * engine.c - GPTPS engine: lifecycle, registry, queue, single-writer
 * dispatcher + worker pool, in-process executor, failure engine
 * (T2 + T4 + T5 + the in-process half of the executor seam).
 *
 * CONCURRENCY MODEL
 * -----------------
 * One mutex `m` guards all shared state. The DISPATCHER thread is the only
 * writer of the admission ledger (reserved_mem, running) and the only authority
 * for timing (deadline enforcement + retry backoff). Workers run tasks with the
 * lock released and post finished items to `done`; they never touch the ledger.
 *
 *   submit ─► [intake] ─► dispatcher admits the highest-PRIORITY item that fits
 *                          (running<conc && reserved+cost<=max), skipping a
 *                          too-large item to backfill smaller work behind it
 *                          (bounded reservation guards against starvation)
 *                                            ─► [ready] ─► worker runs
 *   worker  ─► [done] ─► dispatcher releases budget, then decides:
 *                          ok                -> free
 *                          fail & retries    -> [delayed] (re-admit after backoff)
 *                          fail & exhausted  -> on_failure: dead_letter|drop|requeue
 *
 * Timing: the dispatcher flips a running task's cancel flag when its deadline
 * passes (cooperative; in-process tasks must poll gptps_is_cancelled), and wakes
 * via cond_timedwait at the nearest deadline / backoff time. Event callbacks are
 * invoked with the lock RELEASED (never re-entrant under the engine lock).
 */
#include "gptps.h"
#include "gptps_hal.h"
#include "gptps_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------------- */
/* internal types                                                            */
/* ------------------------------------------------------------------------- */

struct gptps_reg;   /* forward: ctx carries the running item's registry slot */

struct gptps_ctx {
    gptps            *engine;
    struct gptps_reg *reg;           /* registry slot of the running task (for per-task setting reads) */
    gptps_handle      handle;
    const char       *task_name;
    const void       *payload;
    size_t            payload_len;
    uint64_t          deadline_ms;
    gptps_flag       *cancel;        /* owned by the item, shared with the ctx */
    void             *result;
    size_t            result_len;
    void            (*result_free)(void *);
    bool              result_is_copy;    /* true => core allocated a copy; free it */
    bool              result_set;
};

typedef struct gptps_item {
    gptps_handle          handle;
    const gptps_task_def *def;       /* points into the registry (stable while it->reg lives) */
    struct gptps_reg     *reg;       /* owning registry slot (NULL once detached for dead-letter) */
    char                 *name_owned;/* owned name copy, set only when detached from a removed reg */
    void                 *payload;
    size_t                payload_len;
    gptps_cost            cost;
    gptps_failure_policy  policy;
    int32_t               priority;  /* higher = admitted first (default 0) */
    int64_t               sched_score;/* admission ordering key: priority by default, or the scheduler hook's score (stamped per pass) */
    uint32_t              skips;     /* times a backfill admission jumped ahead while budget-blocked */
    uint32_t              attempt;   /* 1 = first try */
    uint64_t              enqueue_ms;    /* monotonic ms when first submitted (for the scheduler seam: age/deadline/FIFO) */
    uint64_t              deadline_ms;   /* 0 = no timeout */
    uint64_t              not_before_ms; /* backoff gate for delayed retries */
    gptps_flag           *cancel;
    gptps_status          outcome;   /* effective status of the last attempt */
    int                   cancelled; /* gptps_cancel(handle) requested: never retry/dead-letter */
    uint32_t              timeout_ms_override; /* per-submit sub-second deadline (0 = use policy.timeout_seconds) */
    uint64_t             *res_reserved; /* named-resource amounts reserved at admit (length res_n); freed+NULLed at release (done-drain), else at item_free */
    size_t                res_n;
    struct gptps_item    *next;
} gptps_item;

/* One instance value of a generic per-task setting (see gptps_define_task_setting).
 * Materialized per (task, schema); the settings entry's target points here. */
typedef struct gptps_task_local {
    const struct gptps_task_schema *schema;       /* borrowed: shared schema (leaf/type/range) */
    struct gptps_reg               *reg;          /* owning task (locks reg->engine->m) */
    char                            value[GPTPS_SETTINGS_VALUE_MAX];
    struct gptps_task_local        *next;
} gptps_task_local;

typedef struct gptps_reg {
    gptps_task_def     def;
    char              *name;
    char             **argv_copy;  /* owned NULL-terminated copy for EXEC_PROGRAM */
    int32_t            priority;   /* scheduling priority for this task type (default 0) */
    bool               enabled;    /* false => reject new submits (paused, reversible) */
    bool               removed;    /* true => tombstoned, draining toward removal */
    bool               cancelling; /* true => removal is CANCEL: drop in-flight items rather than dead-letter */
    bool               service;    /* true => GPTPS_TASK_SERVICE: supervised long-running instances (restart-on-exit) */
    bool               retire_on_ok;/* service only: a clean GPTPS_OK return retires the instance instead of restarting it */
    gptps_task_local  *locals;     /* owned generic per-task setting cells */
    uint64_t          *res_cost;   /* per-item cost per named resource (length engine->nres; NULL if nres==0) */
    struct gptps      *engine;     /* back-pointer (settings write_fns lock engine->m) */
    struct gptps_reg  *next;
} gptps_reg;

/* A generic per-task setting schema: materialized as tasks.<name>.<leaf> on every
 * task (existing + future). The choices array (enum) is owned here. */
typedef struct gptps_task_schema {
    char                     *leaf;     /* owned bare key (no dots) */
    gptps_setting_type        type;
    char                     *defval;   /* owned default rendering */
    int                       hot, has_range;
    double                    min, max;
    char                    **choices;  /* owned NULL-terminated (enum only) */
    struct gptps_task_schema *next;
} gptps_task_schema;

/* A generic GLOBAL setting the engine stores for you (see gptps_define_global):
 * a self-contained value cell with optional owned enum choices. */
typedef struct gptps_owned_setting {
    char                        *key;      /* owned full dotted key */
    char                         value[GPTPS_SETTINGS_VALUE_MAX];
    char                       **choices;  /* owned NULL-terminated (enum only) */
    struct gptps_owned_setting  *next;
} gptps_owned_setting;

typedef struct gptps_loaded {
    gptps_dl            *dl;
    const gptps_addon   *addon;
    struct gptps_loaded *next;
} gptps_loaded;

typedef struct gptps_observer {
    gptps_event_cb         fn;
    void                  *ud;
    struct gptps_observer *next;
} gptps_observer;

typedef struct gptps_constraint {
    gptps_constraint_fn      fn;
    void                    *ud;
    struct gptps_constraint *next;
} gptps_constraint;

typedef struct { gptps_item *head, *tail; size_t count; } gptps_fifo;

/* pending event emitted after the lock is released. `name` is an OWNED inline copy
 * (not a borrowed pointer): a task type can be unregistered and freed during the
 * lock-released emit window, so the buffered event must not alias reg/def memory. */
#define GPTPS_EV_NAME_MAX 128
typedef struct {
    gptps_event_kind kind;
    gptps_handle     handle;
    char             name[GPTPS_EV_NAME_MAX];
    gptps_status     status;
    uint32_t         attempt;
    uint64_t         mem;
    const void      *result;
    size_t           result_len;
} gptps_pending_ev;

static void ev_set_name(char *dst, const char *src)
{ snprintf(dst, GPTPS_EV_NAME_MAX, "%s", src ? src : "?"); }

/* A generic named admission resource: a total budget and the amount currently
 * reserved by in-flight items (DISPATCHER-only, like reserved_mem). */
typedef struct {
    char    *name;       /* owned */
    uint64_t budget;
    uint64_t reserved;   /* DISPATCHER-ONLY */
} gptps_resource;

struct gptps {
    gptps_limits   limits;
    gptps_reg     *registry;

    gptps_mutex   *m;
    gptps_cond    *cv_disp;
    gptps_cond    *cv_work;
    gptps_cond    *cv_drain;        /* signalled after each pass; a blocked unregister re-checks drain */

    gptps_fifo     intake;
    gptps_fifo     ready;
    gptps_fifo     done;
    gptps_fifo     delayed;        /* retries waiting for backoff */
    gptps_fifo     running_items;  /* in-flight, scanned for deadlines */
    gptps_fifo     dead_letter;    /* terminal failures retained */
    uint32_t       dead_letter_count;

    uint64_t       reserved_mem;   /* DISPATCHER-ONLY */
    uint32_t       running;        /* DISPATCHER-ONLY */

    gptps_resource *resources;     /* generic named admission budgets (gptps_define_resource) */
    size_t          nres, rescap;

    gptps_thread  *dispatcher;
    gptps_thread **workers;
    unsigned       nworkers;

    bool           stopping;
    bool           workers_exit;
    bool           manual;         /* MANUAL mode: no threads; driven by gptps_step() */

    gptps_handle   next_handle;
    gptps_event_cb ev_cb;
    void          *ev_ud;
    gptps_sched_fn sched_fn;      /* swappable admission ordering (NULL => built-in priority) */
    void          *sched_ud;

    gptps_loaded  *addons;        /* dlopen'd add-ons, torn down at shutdown */
    gptps_observer *observers;    /* extra event sinks (registered before submit) */
    gptps_constraint *constraints;/* admission hooks consulted by the dispatcher */

    gptps_toml    *toml;          /* parsed config file (NULL if opened without one) */
    uint32_t       reserve_after_skips; /* starvation guard: reserve a budget-blocked top task after this many backfill skips */
    gptps_settings *settings;     /* unified settings registry */
    char          *config_path;   /* the path opened with (NULL if none); default for save/reload */
    gptps_owned_setting *owned_settings; /* engine-stored generic global knobs (gptps_define_global) */
    gptps_task_schema   *task_schemas;   /* generic per-task setting schemas (gptps_define_task_setting) */
    unsigned             active_defines; /* in-flight gptps_define_task_setting materializations; a reg
                                          * must not be freed while >0 (it may hold a snapshotted reg ptr) */
};

/* ------------------------------------------------------------------------- */
/* fifo helpers                                                              */
/* ------------------------------------------------------------------------- */

static void fifo_push(gptps_fifo *q, gptps_item *it)
{
    it->next = NULL;
    if (q->tail) q->tail->next = it; else q->head = it;
    q->tail = it;
    q->count += 1;
}
static gptps_item *fifo_pop(gptps_fifo *q)
{
    gptps_item *it = q->head;
    if (it) { q->head = it->next; if (!q->head) q->tail = NULL; it->next = NULL; q->count -= 1; }
    return it;
}
static void fifo_remove(gptps_fifo *q, gptps_item *target)
{
    gptps_item *prev = NULL, *cur = q->head;
    while (cur) {
        if (cur == target) {
            if (prev) prev->next = cur->next; else q->head = cur->next;
            if (q->tail == cur) q->tail = prev;
            cur->next = NULL;
            q->count -= 1;
            return;
        }
        prev = cur; cur = cur->next;
    }
}

/* Find a live (non-tombstoned) task by name. A draining task is logically gone:
 * its name is free to submit-reject / re-register. */
static gptps_reg *registry_find(const gptps *e, const char *name)
{
    gptps_reg *r;
    for (r = e->registry; r; r = r->next)
        if (!r->removed && strcmp(r->name, name) == 0) return r;
    return NULL;
}

/* Resolved name for an item (owned copy once detached from a removed reg). */
static const char *item_name(const gptps_item *it)
{
    if (it->name_owned) return it->name_owned;
    if (it->reg)        return it->reg->name;
    return it->def ? it->def->name : "?";
}

static void item_free(gptps_item *it)
{
    if (!it) return;
    if (it->cancel) gptps_flag_destroy(it->cancel);
    gptps_free(it->payload);
    gptps_free(it->name_owned);
    gptps_free(it->res_reserved);
    gptps_free(it);
}

static void emit_now(gptps *e, gptps_event_cb cb, void *ud, const gptps_pending_ev *p)
{
    gptps_event ev;
    gptps_observer *o;
    if (!cb && !e->observers) return;
    memset(&ev, 0, sizeof ev);
    ev.struct_size = sizeof ev;
    ev.kind = p->kind; ev.handle = p->handle; ev.task_name = p->name;
    ev.ts_ms = gptps_hal_monotonic_ms(); ev.status = p->status;
    ev.attempt = p->attempt; ev.mem_bytes = p->mem;
    ev.result = p->result; ev.result_len = p->result_len;
    if (cb) cb(&ev, ud);
    for (o = e->observers; o; o = o->next) o->fn(&ev, o->ud); /* extra sinks */
}

/* ------------------------------------------------------------------------- */
/* ctx accessors                                                             */
/* ------------------------------------------------------------------------- */

bool        gptps_is_cancelled(const gptps_ctx *ctx) { return ctx && ctx->cancel && gptps_flag_get(ctx->cancel); }
uint64_t    gptps_deadline_ms(const gptps_ctx *ctx)  { return ctx ? ctx->deadline_ms : 0; }
uint64_t    gptps_now_ms(const gptps_ctx *ctx)       { (void)ctx; return gptps_hal_monotonic_ms(); }

const void *gptps_payload(const gptps_ctx *ctx, size_t *out_len)
{
    if (out_len) *out_len = ctx ? ctx->payload_len : 0;
    return ctx ? ctx->payload : NULL;
}

/* process-wide diagnostic sink (NULL => stderr default); see gptps_set_log_sink */
static gptps_log_sink_fn g_log_sink = NULL;
static void             *g_log_sink_ud = NULL;

void gptps_set_log_sink(gptps_log_sink_fn fn, void *user_data)
{
    g_log_sink = fn; g_log_sink_ud = user_data;
}

void gptps_log(gptps_ctx *ctx, gptps_log_level lvl, const char *msg)
{
    (void)ctx;
    if (lvl >= GPTPS_LOG_WARN && msg) {
        if (g_log_sink) g_log_sink(lvl, msg, g_log_sink_ud);
        else fprintf(stderr, "[gptps] %s\n", msg);
    }
}

static void ctx_clear_result(gptps_ctx *c)
{
    if (c->result_set) {
        if (c->result_is_copy)       gptps_free(c->result);          /* core-owned copy */
        else if (c->result_free)     c->result_free(c->result); /* transferred w/ free_cb */
        /* else: borrowed (nocopy + NULL free_cb) -> caller owns it, do not free */
        c->result = NULL; c->result_len = 0; c->result_free = NULL;
        c->result_is_copy = false; c->result_set = false;
    }
}

gptps_status gptps_result_set(gptps_ctx *ctx, const void *bytes, size_t len)
{
    void *copy;
    if (!ctx) return GPTPS_E_INVAL;
    ctx_clear_result(ctx);
    if (len == 0) { ctx->result = NULL; ctx->result_len = 0; ctx->result_is_copy = false; ctx->result_set = true; return GPTPS_OK; }
    copy = gptps_malloc(len);
    if (!copy) return GPTPS_E_NOMEM;
    memcpy(copy, bytes, len);
    ctx->result = copy; ctx->result_len = len; ctx->result_free = NULL;
    ctx->result_is_copy = true; ctx->result_set = true;
    return GPTPS_OK;
}

gptps_status gptps_result_set_nocopy(gptps_ctx *ctx, void *bytes, size_t len, void (*free_cb)(void *))
{
    if (!ctx) return GPTPS_E_INVAL;
    ctx_clear_result(ctx);
    /* free_cb == NULL => buffer is borrowed (caller-owned); the core won't free it. */
    ctx->result = bytes; ctx->result_len = len; ctx->result_free = free_cb;
    ctx->result_is_copy = false; ctx->result_set = true;
    return GPTPS_OK;
}

/* Build a ctx, run the task in THIS process, return a malloc'd copy of the
 * result (caller frees). Used directly by the in-process path's logic and by
 * the OOP child (see exec_oop_posix.c). */
gptps_status gptps_run_capture(const gptps_task_def *def, const void *payload, size_t plen,
                               void **out_result, size_t *out_len)
{
    struct gptps_ctx ctx;
    gptps_status st;

    *out_result = NULL; *out_len = 0;
    memset(&ctx, 0, sizeof ctx);
    ctx.payload = payload; ctx.payload_len = plen;
    ctx.cancel = NULL; /* OOP enforcement is hard-kill, not the cooperative flag */

    st = def->run(&ctx, def->user_data);
    if (ctx.result_set && ctx.result_len) {
        void *copy = gptps_malloc(ctx.result_len);
        if (copy) { memcpy(copy, ctx.result, ctx.result_len); *out_result = copy; *out_len = ctx.result_len; }
    }
    ctx_clear_result(&ctx);
    return st;
}

/* ------------------------------------------------------------------------- */
/* worker                                                                    */
/* ------------------------------------------------------------------------- */

/* cb/ud are snapshotted under the lock by the caller so a concurrent
 * gptps_set_event_cb cannot pair a new callback with a stale user_data. */
static gptps_status execute(gptps *e, gptps_item *it, gptps_event_cb cb, void *ud)
{
    gptps_pending_ev p;
    gptps_status st;
    struct gptps_ctx ctx;          /* used only on the in-process path */
    void *oop_res = NULL;
    size_t oop_len = 0;
    bool inproc = (it->def->exec == GPTPS_EXEC_INPROC);

    p.handle = it->handle; ev_set_name(p.name, item_name(it)); p.attempt = it->attempt; p.mem = it->cost.mem_bytes;
    p.result = NULL; p.result_len = 0;
    p.kind = GPTPS_EV_STARTED; p.status = GPTPS_OK; emit_now(e, cb, ud, &p);

    if (inproc) {
        /* in-process path: cooperative cancel via the deadline flag */
        memset(&ctx, 0, sizeof ctx);
        ctx.engine = e; ctx.reg = it->reg; ctx.handle = it->handle; ctx.task_name = it->def->name;
        ctx.payload = it->payload; ctx.payload_len = it->payload_len;
        ctx.deadline_ms = it->deadline_ms; ctx.cancel = it->cancel;
        st = it->def->run(&ctx, it->def->user_data);
        if (gptps_flag_get(it->cancel)) st = GPTPS_E_TIMEOUT;
    } else if (it->def->exec == GPTPS_EXEC_OOP) {
        /* enforced path: run the in-process fn in a forked child, OS-capped, hard-killed.
         * it->cancel lets gptps_cancel / shutdown / removal hard-kill the child. */
        st = gptps_oop_execute(it->def, it->payload, it->payload_len,
                               it->cost.mem_bytes, it->policy.timeout_seconds, it->cancel, &oop_res, &oop_len);
    } else {
        /* enforced path: fork+exec an external program; payload->stdin, stdout->result */
        st = gptps_program_execute(it->def, it->payload, it->payload_len,
                                   it->cost.mem_bytes, it->policy.timeout_seconds, it->cancel, &oop_res, &oop_len);
    }

    /* deliver the result on the FINISHED event (valid for the callback's duration) */
    p.kind = (st == GPTPS_OK) ? GPTPS_EV_FINISHED : GPTPS_EV_FAILED; p.status = st;
    if (st == GPTPS_OK) {
        if (inproc) { if (ctx.result_set) { p.result = ctx.result; p.result_len = ctx.result_len; } }
        else        { p.result = oop_res; p.result_len = oop_len; }
    }
    emit_now(e, cb, ud, &p);

    if (inproc) ctx_clear_result(&ctx);
    else        gptps_free(oop_res);
    return st;
}

static void *worker_main(void *arg)
{
    gptps *e = (gptps *)arg;
    gptps_mutex_lock(e->m);
    for (;;) {
        gptps_item *it;
        gptps_status eff;
        gptps_event_cb cb;
        void *ud;

        while (!e->ready.head && !e->workers_exit)
            gptps_cond_wait(e->cv_work, e->m);
        if (!e->ready.head && e->workers_exit) break;

        it = fifo_pop(&e->ready);
        if (it->cancelled || (it->reg && it->reg->removed && it->reg->cancelling)) {
            /* the item was cancelled (per-handle gptps_cancel) or its type is being
             * CANCELled: don't start an admitted-but-unstarted item (the worker would
             * otherwise reset its cancel flag and run it, letting a cooperative task
             * spin forever and hang the drain). */
            it->outcome = GPTPS_E_CANCELLED;
            fifo_push(&e->done, it);
            gptps_cond_signal(e->cv_disp);
            continue;                       /* lock still held; loop top re-checks ready */
        }
        cb = e->ev_cb; ud = e->ev_ud;      /* snapshot callback under the lock */
        /* deadline flag is the in-process cooperative path; OOP enforces its own
         * deadline in the worker (poll + hard-kill), so it gets no flag deadline.
         * A per-submit timeout_ms_override (gptps_submit_ex) wins over the task
         * type's timeout_seconds and allows a sub-second deadline. */
        if (it->def->exec != GPTPS_EXEC_INPROC)
            it->deadline_ms = 0;
        else if (it->timeout_ms_override)
            it->deadline_ms = gptps_hal_monotonic_ms() + (uint64_t)it->timeout_ms_override;
        else
            it->deadline_ms = it->policy.timeout_seconds
                ? gptps_hal_monotonic_ms() + (uint64_t)it->policy.timeout_seconds * 1000u : 0;
        gptps_flag_set(it->cancel, false);
        fifo_push(&e->running_items, it);
        gptps_cond_signal(e->cv_disp);     /* let dispatcher track the new deadline */
        gptps_mutex_unlock(e->m);

        eff = execute(e, it, cb, ud);

        gptps_mutex_lock(e->m);
        fifo_remove(&e->running_items, it);
        it->outcome = eff;
        fifo_push(&e->done, it);
        gptps_cond_signal(e->cv_disp);
    }
    gptps_mutex_unlock(e->m);
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* dispatcher                                                                */
/* ------------------------------------------------------------------------- */

/* Retry/dead-letter events buffered per dispatch pass for lock-free emit.
 * `done` holds at most `running` (<= max_concurrent_tasks) items per pass, so
 * events are only truncated when max_concurrent_tasks > 256 (observability
 * only; the items themselves are always handled correctly). */
#define GPTPS_PENDING_CAP 256

/* Default starvation guard: a budget-blocked highest-priority task may be skipped
 * by at most this many backfill admissions before the dispatcher reserves for it
 * (suspends backfill and drains running tasks until it fits). Override per engine
 * via the config file's [scheduler] reserve_after_skips. */
#define GPTPS_RESERVE_AFTER 8u

static uint64_t min_nonzero(uint64_t a, uint64_t b)
{
    if (a == 0) return b;
    if (b == 0) return a;
    return a < b ? a : b;
}

/* Consult every constraint hook. Any DENY rejects; otherwise DEFER (with the
 * largest requested retry delay) or ADMIT. Runs on the dispatcher thread, so
 * hooks must be non-blocking. */
static gptps_admit_decision run_constraints(gptps *e, gptps_item *it, uint32_t *retry_after)
{
    gptps_constraint *c;
    gptps_admit_decision result = GPTPS_ADMIT;
    uint32_t max_defer = 0;
    gptps_constraint_input in;
    *retry_after = 0;
    memset(&in, 0, sizeof in);
    in.struct_size = sizeof in;
    in.task_name   = it->def->name;
    in.cost        = &it->cost;
    in.handle      = it->handle;
    in.payload     = it->payload;
    in.payload_len = it->payload_len;
    for (c = e->constraints; c; c = c->next) {
        uint32_t ra = 0;
        gptps_admit_decision d = c->fn(&in, &ra, c->ud);
        if (d == GPTPS_DENY) return GPTPS_DENY;
        if (d == GPTPS_DEFER) { result = GPTPS_DEFER; if (ra > max_defer) max_defer = ra; }
    }
    if (result == GPTPS_DEFER) *retry_after = max_defer ? max_defer : 1u;
    return result;
}

/* One non-blocking scheduling pass, shared by the threaded dispatcher and the
 * MANUAL-mode pump (gptps_step): drain completed work (release budget + retry /
 * terminal decisions), promote backoff-ready retries, enforce running deadlines,
 * and admit within budget. Lock held on entry & exit. Fills pend[0..*out_npend)
 * (cap GPTPS_PENDING_CAP) for the caller to emit with the lock RELEASED; sets
 * *out_next_wake to the nearest deadline/backoff (0 = none). Never sleeps/emits. */
/* Does this item's named-resource cost still fit every resource's budget?
 * (Memory is checked separately.) DISPATCHER context, e->m held. */
static int res_fits(const gptps *e, const gptps_item *it)
{
    size_t i;
    const uint64_t *cost = (it->reg ? it->reg->res_cost : NULL);
    if (!e->nres || !cost) return 1;
    for (i = 0; i < e->nres; ++i)
        if (cost[i] && e->resources[i].reserved + cost[i] > e->resources[i].budget) return 0;
    return 1;
}

static void engine_pass(gptps *e, gptps_pending_ev *pend, int *out_npend, uint64_t *out_next_wake)
{
    uint64_t now = gptps_hal_monotonic_ms();
    uint64_t next_wake = 0; /* 0 = none */
    int npend = 0;
    gptps_item *it;

        /* 1) drain completed: release budget, then retry / terminal decision */
        while ((it = fifo_pop(&e->done)) != NULL) {
            e->reserved_mem -= it->cost.mem_bytes;
            e->running      -= 1;
            if (it->res_reserved) {                          /* release named-resource reservations */
                size_t ri;
                for (ri = 0; ri < it->res_n && ri < e->nres; ++ri)
                    e->resources[ri].reserved -= it->res_reserved[ri];
                /* free the snapshot NOW (symmetric with the admit-time alloc) so a
                 * re-admitted item - a retry, or a service's REQUEUE restart - cannot
                 * overwrite a live pointer and leak it. item_free tolerates NULL. */
                gptps_free(it->res_reserved);
                it->res_reserved = NULL;
                it->res_n = 0;
            }

            if (it->outcome == GPTPS_OK) {
                /* A SERVICE is supervised to stay up: by default a clean return that
                 * was NOT an external stop (cancel / removal / shutdown) is an
                 * unexpected exit, so restart it after backoff - the same crash-restart
                 * contract the failure path gives (a service normally exits only via
                 * the flag, which makes outcome != OK). GPTPS_TASK_RETIRE_ON_OK opts out:
                 * a clean OK return then terminally retires the instance. A normal
                 * task's OK is always terminal. */
                if (it->reg && it->reg->service && !it->reg->retire_on_ok &&
                    !it->cancelled && !it->reg->removed && !e->stopping) {
                    it->attempt = 1;
                    it->not_before_ms = now + (uint64_t)it->policy.retry_backoff_seconds * 1000u;
                    fifo_push(&e->delayed, it);
                } else {
                    item_free(it);
                }
                continue;
            }
            if (it->cancelled) {
                /* per-handle gptps_cancel: terminal, never retried or dead-lettered.
                 * outcome==E_CANCELLED means it was discarded before running, so no
                 * STARTED/terminal event fired yet - emit one so observers reconcile.
                 * If it actually ran, execute() already emitted its FAILED event. */
                if (it->outcome == GPTPS_E_CANCELLED && npend < GPTPS_PENDING_CAP) {
                    pend[npend].kind = GPTPS_EV_FAILED; pend[npend].handle = it->handle;
                    ev_set_name(pend[npend].name, item_name(it)); pend[npend].status = GPTPS_E_CANCELLED;
                    pend[npend].attempt = it->attempt; pend[npend].mem = it->cost.mem_bytes;
                    pend[npend].result = NULL; pend[npend].result_len = 0; ++npend;
                }
                item_free(it);
                continue;
            }
            if (it->reg && it->reg->removed) {
                /* task is being removed: never retry (keeps the drain bounded).
                 * CANCEL discards in-flight work; DROP frees; otherwise (DRAIN) a
                 * genuine failure is preserved in the dead-letter list. */
                if (it->reg->cancelling || it->policy.on_failure == GPTPS_ON_FAILURE_DROP) {
                    item_free(it);
                } else {
                    if (npend < GPTPS_PENDING_CAP) {
                        pend[npend].kind = GPTPS_EV_DEAD_LETTERED; pend[npend].handle = it->handle;
                        ev_set_name(pend[npend].name, item_name(it)); pend[npend].status = it->outcome;
                        pend[npend].attempt = it->attempt; pend[npend].mem = it->cost.mem_bytes;
                        pend[npend].result = NULL; pend[npend].result_len = 0; ++npend;
                    }
                    fifo_push(&e->dead_letter, it);
                    e->dead_letter_count += 1;
                }
                continue;
            }
            if (it->attempt <= it->policy.max_retries) {
                /* schedule a retry after backoff */
                it->attempt += 1;
                it->not_before_ms = now + (uint64_t)it->policy.retry_backoff_seconds * 1000u;
                if (npend < GPTPS_PENDING_CAP) {
                    pend[npend].kind = GPTPS_EV_RETRIED; pend[npend].handle = it->handle;
                    ev_set_name(pend[npend].name, item_name(it)); pend[npend].status = it->outcome;
                    pend[npend].attempt = it->attempt; pend[npend].mem = it->cost.mem_bytes;
                    pend[npend].result = NULL; pend[npend].result_len = 0; ++npend;
                }
                fifo_push(&e->delayed, it);
            } else {
                switch (it->policy.on_failure) {
                    case GPTPS_ON_FAILURE_REQUEUE:
                        if (e->stopping) {
                            /* never re-admit during drain: an always-failing
                             * REQUEUE task would otherwise hang shutdown forever */
                            fifo_push(&e->dead_letter, it);
                            e->dead_letter_count += 1;
                        } else {
                            /* re-enqueue via delayed so retry_backoff is honored
                             * (avoids a zero-backoff busy loop pegging a core) */
                            it->attempt = 1;
                            it->not_before_ms = now + (uint64_t)it->policy.retry_backoff_seconds * 1000u;
                            fifo_push(&e->delayed, it);
                        }
                        break;
                    case GPTPS_ON_FAILURE_DROP:
                        /* emit a terminal event so observers reconcile the item
                         * (DROP retains nothing, but the submitter/observer still
                         * needs to know this handle is finally gone). */
                        if (npend < GPTPS_PENDING_CAP) {
                            pend[npend].kind = GPTPS_EV_DROPPED; pend[npend].handle = it->handle;
                            ev_set_name(pend[npend].name, item_name(it)); pend[npend].status = it->outcome;
                            pend[npend].attempt = it->attempt; pend[npend].mem = it->cost.mem_bytes;
                            pend[npend].result = NULL; pend[npend].result_len = 0; ++npend;
                        }
                        item_free(it);
                        break;
                    case GPTPS_ON_FAILURE_DEAD_LETTER:
                    default:
                        if (npend < GPTPS_PENDING_CAP) {
                            pend[npend].kind = GPTPS_EV_DEAD_LETTERED; pend[npend].handle = it->handle;
                            ev_set_name(pend[npend].name, item_name(it)); pend[npend].status = it->outcome;
                            pend[npend].attempt = it->attempt; pend[npend].mem = it->cost.mem_bytes;
                    pend[npend].result = NULL; pend[npend].result_len = 0; ++npend;
                        }
                        fifo_push(&e->dead_letter, it);
                        e->dead_letter_count += 1;
                        break;
                }
            }
        }

        /* (pending events from step 1 + admission below are emitted together,
         * after the admit step, with the lock released — see step 5) */

        /* 2) move backoff-ready delayed items back to intake (single scan) */
        {
            gptps_item *cur = e->delayed.head, *prev = NULL;
            while (cur) {
                gptps_item *nxt = cur->next;
                if (cur->not_before_ms <= now) {
                    if (prev) prev->next = nxt; else e->delayed.head = nxt;
                    if (e->delayed.tail == cur) e->delayed.tail = prev;
                    cur->next = NULL;
                    e->delayed.count -= 1;
                    fifo_push(&e->intake, cur);
                } else {
                    next_wake = min_nonzero(next_wake, cur->not_before_ms);
                    prev = cur;
                }
                cur = nxt;
            }
        }

        /* 3) enforce deadlines on running tasks (cooperative cancel) */
        for (it = e->running_items.head; it; it = it->next) {
            if (it->deadline_ms) {
                if (now >= it->deadline_ms) gptps_flag_set(it->cancel, true);
                else next_wake = min_nonzero(next_wake, it->deadline_ms);
            }
        }

        /* 4) admit in SCHEDULER order with skip-to-fit backfill + starvation guard.
         *    Each iteration scans intake for `top` (highest-score item overall) and
         *    `best` (highest-score item that fits the live budget; ties resolve to
         *    the older item, preserving FIFO within a score). When `best != top` we
         *    are about to skip the higher-score `top` because it does not fit yet:
         *    allowed (backfill) until `top` has been skipped reserve_after_skips
         *    times, after which we reserve for it - admit nothing and let running
         *    tasks drain until it fits (bounded). The ORDERING KEY (sched_score) is
         *    the one swappable policy: priority by default, or a scheduler hook's
         *    score; the skip-to-fit / budget / starvation MECHANISM stays fixed. */
        /* scheduler seam: with a custom ordering installed, (re)score every pending
         * item for this pass (scores may depend on time). The default ordering left
         * sched_score == priority, stamped at submit, so it needs no rescoring. */
        if (e->sched_fn && e->intake.head && e->running < e->limits.max_concurrent_tasks) {
            gptps_item *cur;
            for (cur = e->intake.head; cur; cur = cur->next) {
                gptps_sched_input si;
                memset(&si, 0, sizeof si);
                si.struct_size = sizeof si;
                si.task_name = item_name(cur);   si.cost = &cur->cost;
                si.handle = cur->handle;         si.priority = cur->priority;
                si.attempt = cur->attempt;       si.enqueue_ms = cur->enqueue_ms;
                si.payload = cur->payload;       si.payload_len = cur->payload_len;
                cur->sched_score = e->sched_fn(&si, e->sched_ud);
            }
        }
        while (e->intake.head && e->running < e->limits.max_concurrent_tasks) {
            gptps_item *best = NULL, *top = NULL, *cur;
            uint32_t retry_after = 0;
            gptps_admit_decision dec;

            for (cur = e->intake.head; cur; cur = cur->next) {
                if (!top || cur->sched_score > top->sched_score) top = cur;
                if (e->reserved_mem + cur->cost.mem_bytes <= e->limits.max_memory_bytes &&
                    res_fits(e, cur) &&
                    (!best || cur->sched_score > best->sched_score))
                    best = cur;
            }

            if (!best) break;                            /* nothing fits the live budget now */
            if (best != top && top->skips >= e->reserve_after_skips)
                break;                                   /* reserve for `top`: drain, admit nothing */

            dec = run_constraints(e, best, &retry_after); /* consulted only on the chosen item */
            if (dec == GPTPS_DEFER) {
                fifo_remove(&e->intake, best);
                best->not_before_ms = now + retry_after; /* re-check after the delay */
                fifo_push(&e->delayed, best);
                next_wake = min_nonzero(next_wake, best->not_before_ms);
                continue;
            }
            if (dec == GPTPS_DENY) {
                fifo_remove(&e->intake, best);
                best->outcome = GPTPS_E_DENIED;          /* recorded for dead-letter drain */
                if (npend < GPTPS_PENDING_CAP) {
                    pend[npend].kind = GPTPS_EV_DEAD_LETTERED; pend[npend].handle = best->handle;
                    ev_set_name(pend[npend].name, item_name(best)); pend[npend].status = GPTPS_E_DENIED;
                    pend[npend].attempt = best->attempt; pend[npend].mem = best->cost.mem_bytes;
                    pend[npend].result = NULL; pend[npend].result_len = 0; ++npend;
                }
                fifo_push(&e->dead_letter, best);        /* denied -> retained */
                e->dead_letter_count += 1;
                continue;
            }

            if (best != top) top->skips += 1;            /* charge the skipped higher-priority task */
            fifo_remove(&e->intake, best);
            e->reserved_mem += best->cost.mem_bytes;
            e->running      += 1;
            if (e->nres && best->reg && best->reg->res_cost) {   /* reserve named resources + snapshot for release */
                best->res_reserved = (uint64_t *)gptps_malloc(e->nres * sizeof(uint64_t));
                if (best->res_reserved) {
                    size_t ri; best->res_n = e->nres;
                    for (ri = 0; ri < e->nres; ++ri) {
                        best->res_reserved[ri] = best->reg->res_cost[ri];
                        e->resources[ri].reserved += best->reg->res_cost[ri];
                    }
                }
            }
            fifo_push(&e->ready, best);
            gptps_cond_signal(e->cv_work);
        }

        /* (the caller emits pend[] with the lock released, then re-runs a pass) */

    *out_npend = npend;
    *out_next_wake = next_wake;
}

/* ------------------------------------------------------------------------- */
/* dispatcher thread (THREADED mode): engine_pass in a loop, emit, then sleep */
/* ------------------------------------------------------------------------- */

static void *dispatcher_main(void *arg)
{
    gptps *e = (gptps *)arg;
    gptps_pending_ev pend[GPTPS_PENDING_CAP];
    int npend, i;
    uint64_t next_wake;

    gptps_mutex_lock(e->m);
    for (;;) {
        engine_pass(e, pend, &npend, &next_wake);
        gptps_cond_broadcast(e->cv_drain);   /* let a blocked gptps_unregister_task re-check its drain */

        /* emit buffered events with the lock RELEASED, then re-run: a submit /
         * completion signal during the emit window may have been missed, so we
         * loop again rather than risk a lost-wakeup sleep. */
        if (npend > 0) {
            gptps_event_cb cb = e->ev_cb; void *ud = e->ev_ud;
            gptps_mutex_unlock(e->m);
            for (i = 0; i < npend; ++i) emit_now(e, cb, ud, &pend[i]);
            gptps_mutex_lock(e->m);
            continue;
        }

        /* shutdown once everything is drained */
        if (e->stopping && !e->intake.head && !e->ready.head && !e->done.head &&
            !e->delayed.head && !e->running_items.head && e->running == 0) {
            e->workers_exit = true;
            gptps_cond_broadcast(e->cv_work);
            break;
        }

        /* sleep until the nearest deadline/backoff or a signal. No unlock happened
         * this pass (npend==0), so no signal between the pass and the wait is lost. */
        {
            uint64_t now = gptps_hal_monotonic_ms();
            if (next_wake == 0)       gptps_cond_wait(e->cv_disp, e->m);
            else if (next_wake > now) gptps_cond_timedwait(e->cv_disp, e->m, next_wake - now);
            /* else: already due -> loop immediately */
        }
    }
    gptps_mutex_unlock(e->m);
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* public API                                                                */
/* ------------------------------------------------------------------------- */

const char *gptps_version(void) { return GPTPS_VERSION_STRING; }

const char *gptps_strerror(gptps_status s)
{
    switch (s) {
        case GPTPS_OK:          return "ok";
        case GPTPS_E_NOMEM:     return "out of memory";
        case GPTPS_E_INVAL:     return "invalid argument";
        case GPTPS_E_NOTFOUND:  return "task not found";
        case GPTPS_E_DUP:       return "task already registered";
        case GPTPS_E_BUDGET:    return "declared cost cannot fit the budget";
        case GPTPS_E_FULL:      return "queue full";
        case GPTPS_E_TIMEOUT:   return "task timed out";
        case GPTPS_E_CANCELLED: return "task cancelled";
        case GPTPS_E_ABI:       return "add-on ABI mismatch";
        case GPTPS_E_CONFIG:    return "config error";
        case GPTPS_E_IO:        return "I/O error";
        case GPTPS_E_TASK:      return "task error";
        case GPTPS_E_SHUTDOWN:  return "engine shutting down";
        case GPTPS_E_DENIED:    return "admission denied by a constraint";
        case GPTPS_E_BUSY:      return "task busy: work is queued or in-flight";
        default:                return "unknown error";
    }
}

/* ------------------------------------------------------------------------- */
/* settings bindings (read/write callbacks for the registry)                 */
/* ------------------------------------------------------------------------- */

static size_t rd_u64(char *b, size_t c, uint64_t v) { return (size_t)snprintf(b, c, "%llu", (unsigned long long)v); }
static size_t rd_u32(char *b, size_t c, uint32_t v) { return (size_t)snprintf(b, c, "%lu", (unsigned long)v); }
static size_t rd_i32(char *b, size_t c, int32_t  v) { return (size_t)snprintf(b, c, "%ld", (long)v); }

/* core settings: target = gptps* */
static size_t       sc_rd_maxmem(void *t, char *b, size_t c) { gptps *e = (gptps *)t; size_t n; gptps_mutex_lock(e->m); n = rd_u64(b, c, e->limits.max_memory_bytes); gptps_mutex_unlock(e->m); return n; }
static gptps_status sc_wr_maxmem(void *t, const char *v) { gptps *e = (gptps *)t; gptps_mutex_lock(e->m); e->limits.max_memory_bytes = (uint64_t)strtoull(v, NULL, 10); gptps_cond_signal(e->cv_disp); gptps_mutex_unlock(e->m); return GPTPS_OK; }
static size_t       sc_rd_conc(void *t, char *b, size_t c) { gptps *e = (gptps *)t; size_t n; gptps_mutex_lock(e->m); n = rd_u32(b, c, e->limits.max_concurrent_tasks); gptps_mutex_unlock(e->m); return n; }
static gptps_status sc_wr_conc(void *t, const char *v) { gptps *e = (gptps *)t; gptps_mutex_lock(e->m); e->limits.max_concurrent_tasks = (uint32_t)strtoul(v, NULL, 10); gptps_mutex_unlock(e->m); return GPTPS_OK; } /* restart-only: pool not resized live */
static size_t       sc_rd_intake(void *t, char *b, size_t c) { gptps *e = (gptps *)t; size_t n; gptps_mutex_lock(e->m); n = rd_u32(b, c, e->limits.max_intake_depth); gptps_mutex_unlock(e->m); return n; }
static gptps_status sc_wr_intake(void *t, const char *v) { gptps *e = (gptps *)t; gptps_mutex_lock(e->m); e->limits.max_intake_depth = (uint32_t)strtoul(v, NULL, 10); gptps_cond_signal(e->cv_disp); gptps_mutex_unlock(e->m); return GPTPS_OK; }
static size_t       sc_rd_resv(void *t, char *b, size_t c) { gptps *e = (gptps *)t; size_t n; gptps_mutex_lock(e->m); n = rd_u32(b, c, e->reserve_after_skips); gptps_mutex_unlock(e->m); return n; }
static gptps_status sc_wr_resv(void *t, const char *v) { gptps *e = (gptps *)t; gptps_mutex_lock(e->m); e->reserve_after_skips = (uint32_t)strtoul(v, NULL, 10); gptps_cond_signal(e->cv_disp); gptps_mutex_unlock(e->m); return GPTPS_OK; }

/* per-task settings: target = gptps_reg* (locks reg->engine->m) */
static const char *const ONFAIL_CHOICES[] = { "dead_letter", "requeue", "drop", 0 };
#define TASK_LOCK(r)   gptps_mutex_lock((r)->engine->m)
#define TASK_UNLOCK(r) gptps_mutex_unlock((r)->engine->m)
static size_t       st_rd_timeout(void *t, char *b, size_t c) { gptps_reg *r = (gptps_reg *)t; size_t n; TASK_LOCK(r); n = rd_u32(b, c, r->def.default_policy.timeout_seconds); TASK_UNLOCK(r); return n; }
static gptps_status st_wr_timeout(void *t, const char *v) { gptps_reg *r = (gptps_reg *)t; TASK_LOCK(r); r->def.default_policy.timeout_seconds = (uint32_t)strtoul(v, NULL, 10); TASK_UNLOCK(r); return GPTPS_OK; }
static size_t       st_rd_retries(void *t, char *b, size_t c) { gptps_reg *r = (gptps_reg *)t; size_t n; TASK_LOCK(r); n = rd_u32(b, c, r->def.default_policy.max_retries); TASK_UNLOCK(r); return n; }
static gptps_status st_wr_retries(void *t, const char *v) { gptps_reg *r = (gptps_reg *)t; TASK_LOCK(r); r->def.default_policy.max_retries = (uint32_t)strtoul(v, NULL, 10); TASK_UNLOCK(r); return GPTPS_OK; }
static size_t       st_rd_backoff(void *t, char *b, size_t c) { gptps_reg *r = (gptps_reg *)t; size_t n; TASK_LOCK(r); n = rd_u32(b, c, r->def.default_policy.retry_backoff_seconds); TASK_UNLOCK(r); return n; }
static gptps_status st_wr_backoff(void *t, const char *v) { gptps_reg *r = (gptps_reg *)t; TASK_LOCK(r); r->def.default_policy.retry_backoff_seconds = (uint32_t)strtoul(v, NULL, 10); TASK_UNLOCK(r); return GPTPS_OK; }
static size_t       st_rd_mem(void *t, char *b, size_t c) { gptps_reg *r = (gptps_reg *)t; size_t n; TASK_LOCK(r); n = rd_u64(b, c, r->def.default_cost.mem_bytes); TASK_UNLOCK(r); return n; }
static gptps_status st_wr_mem(void *t, const char *v) { gptps_reg *r = (gptps_reg *)t; TASK_LOCK(r); r->def.default_cost.mem_bytes = (uint64_t)strtoull(v, NULL, 10); TASK_UNLOCK(r); return GPTPS_OK; }
static size_t       st_rd_prio(void *t, char *b, size_t c) { gptps_reg *r = (gptps_reg *)t; size_t n; TASK_LOCK(r); n = rd_i32(b, c, r->priority); TASK_UNLOCK(r); return n; }
static gptps_status st_wr_prio(void *t, const char *v) { gptps_reg *r = (gptps_reg *)t; TASK_LOCK(r); r->priority = (int32_t)strtol(v, NULL, 10); TASK_UNLOCK(r); return GPTPS_OK; }
static size_t       st_rd_onfail(void *t, char *b, size_t c) { gptps_reg *r = (gptps_reg *)t; const char *s; TASK_LOCK(r);
    s = (r->def.default_policy.on_failure == GPTPS_ON_FAILURE_DROP) ? "drop" : (r->def.default_policy.on_failure == GPTPS_ON_FAILURE_REQUEUE) ? "requeue" : "dead_letter";
    TASK_UNLOCK(r); return (size_t)snprintf(b, c, "%s", s); }
static gptps_status st_wr_onfail(void *t, const char *v) { gptps_reg *r = (gptps_reg *)t; gptps_on_failure of = GPTPS_ON_FAILURE_DEAD_LETTER;
    if (strcmp(v, "drop") == 0) of = GPTPS_ON_FAILURE_DROP; else if (strcmp(v, "requeue") == 0) of = GPTPS_ON_FAILURE_REQUEUE;
    TASK_LOCK(r); r->def.default_policy.on_failure = of; TASK_UNLOCK(r); return GPTPS_OK; }

static void reg_core_setting(gptps *e, const char *key, gptps_setting_type type, int hot,
                             int has_range, double mn, double mx, const char *desc,
                             size_t (*rd)(void *, char *, size_t), gptps_status (*wr)(void *, const char *))
{
    gptps_setting_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.key = key; d.type = type; d.hot = hot;
    d.has_range = has_range; d.min = mn; d.max = mx; d.desc = desc;
    d.target = e; d.read = rd; d.write = wr;
    gptps_settings_add(e->settings, &d);
}

static void reg_task_setting(gptps *e, gptps_reg *r, const char *leaf, gptps_setting_type type,
                             const char *const *choices,
                             size_t (*rd)(void *, char *, size_t), gptps_status (*wr)(void *, const char *))
{
    gptps_setting_def d;
    char key[320];
    memset(&d, 0, sizeof d);
    snprintf(key, sizeof key, "tasks.%s.%s", r->name, leaf);
    d.struct_size = sizeof d; d.key = key; d.type = type; d.hot = 1; d.desc = "per-task policy";
    d.choices = choices; d.target = r; d.read = rd; d.write = wr;
    gptps_settings_add(e->settings, &d);   /* key is copied by add() */
}

/* register the six per-task knobs for a freshly-registered task (after e->m unlocked) */
static void register_task_settings(gptps *e, gptps_reg *r)
{
    reg_task_setting(e, r, "timeout_seconds",       GPTPS_SETTING_UINT, NULL,           st_rd_timeout, st_wr_timeout);
    reg_task_setting(e, r, "max_retries",           GPTPS_SETTING_UINT, NULL,           st_rd_retries, st_wr_retries);
    reg_task_setting(e, r, "retry_backoff_seconds", GPTPS_SETTING_UINT, NULL,           st_rd_backoff, st_wr_backoff);
    reg_task_setting(e, r, "mem_bytes",             GPTPS_SETTING_UINT, NULL,           st_rd_mem,     st_wr_mem);
    reg_task_setting(e, r, "priority",              GPTPS_SETTING_INT,  NULL,           st_rd_prio,    st_wr_prio);
    reg_task_setting(e, r, "on_failure",            GPTPS_SETTING_ENUM, ONFAIL_CHOICES, st_rd_onfail,  st_wr_onfail);
}

/* ------------------------------------------------------------------------- */
/* generic settings: engine-stored global knobs + per-task setting schemas    */
/* ------------------------------------------------------------------------- */

/* Validate a value against a schema the same way the registry does, so a bad
 * default_val is rejected at define time (returns 1 ok, 0 invalid). */
static int gval_ok(gptps_setting_type type, int has_range, double mn, double mx,
                   const char *const *choices, const char *v)
{
    char *end;
    if (!v) return 0;
    switch (type) {
        case GPTPS_SETTING_INT: {
            long long x = strtoll(v, &end, 10);
            if (end == v || *end) return 0;
            return !(has_range && ((double)x < mn || (double)x > mx));
        }
        case GPTPS_SETTING_UINT: {
            unsigned long long x; const char *p = v;
            while (*p == ' ' || *p == '\t') ++p;
            if (*p == '-') return 0;
            x = strtoull(v, &end, 10);
            if (end == v || *end) return 0;
            return !(has_range && ((double)x < mn || (double)x > mx));
        }
        case GPTPS_SETTING_DOUBLE: {
            double x = strtod(v, &end);
            if (end == v || *end) return 0;
            return !(has_range && (x < mn || x > mx));
        }
        case GPTPS_SETTING_BOOL:
            return strcmp(v, "true") == 0 || strcmp(v, "false") == 0;
        case GPTPS_SETTING_ENUM: {
            const char *const *c;
            if (!choices) return 0;
            for (c = choices; *c; ++c)
                if (strcmp(*c, v) == 0) return 1;
            return 0;
        }
        case GPTPS_SETTING_STRING:
            return strlen(v) < GPTPS_SETTINGS_VALUE_MAX;
    }
    return 0;
}

/* a type's zero/default rendering when the caller passes default_val == NULL */
static const char *gtype_zero(gptps_setting_type type, const char *const *choices)
{
    switch (type) {
        case GPTPS_SETTING_BOOL:   return "false";
        case GPTPS_SETTING_ENUM:   return (choices && choices[0]) ? choices[0] : "";
        case GPTPS_SETTING_STRING: return "";
        default:                   return "0";
    }
}

/* parse a "min..max" constraint; *has_range=0 (no range) when c is NULL/empty.
 * returns 1 on success, 0 if c is malformed (has no "..") */
static int parse_range(const char *c, int *has_range, double *mn, double *mx)
{
    const char *dd;
    *has_range = 0; *mn = 0; *mx = 0;
    if (!c || !*c) return 1;
    dd = strstr(c, "..");
    if (!dd) return 0;
    *mn = strtod(c, NULL);
    *mx = strtod(dd + 2, NULL);
    *has_range = 1;
    return 1;
}

/* parse "a|b|c" into a fresh NULL-terminated, gptps_malloc'd choices array
 * (caller owns). Returns NULL on empty/none. */
static char **parse_choices(const char *c)
{
    char **arr; size_t n = 1, i = 0; const char *p;
    if (!c || !*c) return NULL;
    for (p = c; *p; ++p) if (*p == '|') ++n;              /* count tokens */
    arr = (char **)gptps_calloc(n + 1, sizeof *arr);
    if (!arr) return NULL;
    while (*c) {
        const char *bar = strchr(c, '|');
        size_t len = bar ? (size_t)(bar - c) : strlen(c);
        char *tok = (char *)gptps_malloc(len + 1);
        if (!tok) { while (i) gptps_free(arr[--i]); gptps_free(arr); return NULL; }
        memcpy(tok, c, len); tok[len] = 0;
        arr[i++] = tok;
        if (!bar) break;
        c = bar + 1;
    }
    arr[i] = NULL;
    return arr;
}

static void free_choices(char **arr)
{
    char **p;
    if (!arr) return;
    for (p = arr; *p; ++p) gptps_free(*p);
    gptps_free(arr);
}

/* ---- engine-stored GLOBAL setting (target = gptps_owned_setting*) ----
 * Accessed only through the registry (under settings->m), so the cell needs no
 * lock of its own. */
static size_t       os_rd(void *t, char *b, size_t c) { gptps_owned_setting *o = (gptps_owned_setting *)t; return (size_t)snprintf(b, c, "%s", o->value); }
static gptps_status os_wr(void *t, const char *v) { gptps_owned_setting *o = (gptps_owned_setting *)t; snprintf(o->value, sizeof o->value, "%s", v); return GPTPS_OK; }

/* ---- generic PER-TASK setting instance (target = gptps_task_local*) ----
 * Locks the engine mutex (consistent with the built-in per-task knobs). */
static size_t       stl_rd(void *t, char *b, size_t c) { gptps_task_local *L = (gptps_task_local *)t; size_t n; gptps_mutex_lock(L->reg->engine->m); n = (size_t)snprintf(b, c, "%s", L->value); gptps_mutex_unlock(L->reg->engine->m); return n; }
static gptps_status stl_wr(void *t, const char *v) { gptps_task_local *L = (gptps_task_local *)t; gptps_mutex_lock(L->reg->engine->m); snprintf(L->value, sizeof L->value, "%s", v); gptps_mutex_unlock(L->reg->engine->m); return GPTPS_OK; }

/* Materialize one generic per-task schema as tasks.<r->name>.<schema->leaf>,
 * bound to a fresh owned value cell. Called with e->m NOT held (settings add
 * takes settings->m then e->m via the read callback). */
static void materialize_task_local(gptps *e, gptps_reg *r, const gptps_task_schema *sc)
{
    gptps_task_local *L;
    gptps_setting_def d;
    char key[320];
    L = (gptps_task_local *)gptps_calloc(1, sizeof *L);
    if (!L) return;
    L->schema = sc; L->reg = r;
    snprintf(L->value, sizeof L->value, "%s", sc->defval ? sc->defval : "");
    memset(&d, 0, sizeof d);
    snprintf(key, sizeof key, "tasks.%s.%s", r->name, sc->leaf);
    d.struct_size = sizeof d; d.key = key; d.type = sc->type; d.hot = sc->hot; d.desc = "per-task setting";
    d.has_range = sc->has_range; d.min = sc->min; d.max = sc->max;
    d.choices = (const char *const *)sc->choices;
    d.target = L; d.read = stl_rd; d.write = stl_wr;
    if (gptps_settings_add(e->settings, &d) == GPTPS_OK) {
        gptps_mutex_lock(e->m); L->next = r->locals; r->locals = L; gptps_mutex_unlock(e->m);
    } else {
        gptps_free(L);   /* duplicate leaf or OOM: not bound */
    }
}

/* Materialize ALL defined per-task schemas onto a freshly-registered task. The
 * schema list is snapshotted under e->m (schemas are freed only at shutdown, so the
 * pointers stay valid) to avoid racing a concurrent gptps_define_task_setting that
 * prepends to it; materialization itself runs with e->m released (lock order). */
static void register_task_local_settings(gptps *e, gptps_reg *r)
{
    const gptps_task_schema *stack_snap[32];
    const gptps_task_schema **snap = stack_snap;
    size_t n = 0, cap = 32, i;
    const gptps_task_schema *sc;

    gptps_mutex_lock(e->m);
    for (sc = e->task_schemas; sc; sc = sc->next) {
        if (n == cap) {
            size_t nc = cap * 2;
            const gptps_task_schema **ns = (const gptps_task_schema **)gptps_malloc(nc * sizeof *ns);
            if (!ns) break;
            memcpy(ns, snap, n * sizeof *ns);
            if (snap != stack_snap) gptps_free(snap);
            snap = ns; cap = nc;
        }
        snap[n++] = sc;
    }
    gptps_mutex_unlock(e->m);

    for (i = 0; i < n; ++i) materialize_task_local(e, r, snap[i]);
    if (snap != stack_snap) gptps_free(snap);
}

gptps_status gptps_open_ex(const gptps_config *cfg, gptps **out_engine)
{
    gptps *e;
    gptps_status s;
    const gptps_limits *in = cfg ? &cfg->limits : NULL;
    unsigned i;

    if (!out_engine) return GPTPS_E_INVAL;
    if (cfg && cfg->struct_size < sizeof *cfg) return GPTPS_E_INVAL; /* ABI: reject undersized struct */
    *out_engine = NULL;

    e = (gptps *)gptps_calloc(1, sizeof *e);
    if (!e) return GPTPS_E_NOMEM;

    s = gptps_config_resolve(in, &e->limits);
    if (s != GPTPS_OK) { gptps_free(e); return s; }

    e->next_handle = 1;
    e->reserve_after_skips = GPTPS_RESERVE_AFTER;
    e->m = gptps_mutex_create();
    e->cv_disp = gptps_cond_create();
    e->cv_work = gptps_cond_create();
    e->cv_drain = gptps_cond_create();
    e->settings = gptps_settings_create();
    if (!e->m || !e->cv_disp || !e->cv_work || !e->cv_drain || !e->settings) { s = GPTPS_E_NOMEM; goto fail; }

    /* core settings (read live engine state; hot ones apply immediately) */
    reg_core_setting(e, "limits.max_memory_bytes", GPTPS_SETTING_UINT, 1, 0, 0, 0,
                     "admission memory budget in bytes", sc_rd_maxmem, sc_wr_maxmem);
    reg_core_setting(e, "limits.max_concurrent_tasks", GPTPS_SETTING_UINT, 0, 1, 1, 65536,
                     "worker pool size (restart to apply)", sc_rd_conc, sc_wr_conc);
    reg_core_setting(e, "limits.max_intake_depth", GPTPS_SETTING_UINT, 1, 0, 0, 0,
                     "max queued (un-admitted) items before submit returns E_FULL (0 = unbounded)", sc_rd_intake, sc_wr_intake);
    reg_core_setting(e, "scheduler.reserve_after_skips", GPTPS_SETTING_UINT, 1, 0, 0, 0,
                     "scheduler starvation guard (backfill skips before reserving)", sc_rd_resv, sc_wr_resv);

    if (cfg && cfg->config_path) {   /* remember the open path for save/reload defaults */
        size_t L = strlen(cfg->config_path) + 1;
        e->config_path = (char *)gptps_malloc(L);
        if (e->config_path) memcpy(e->config_path, cfg->config_path, L);
    }

    e->manual = (cfg && cfg->mode == GPTPS_RUN_MANUAL);
    if (e->manual) {
        /* MANUAL: no dispatcher/worker threads; the caller drives via gptps_step().
         * max_concurrent_tasks still bounds how many items one step admits at once. */
        e->nworkers = 0;
        e->workers = NULL;
    } else {
        e->nworkers = e->limits.max_concurrent_tasks;
        e->workers = (gptps_thread **)gptps_calloc(e->nworkers, sizeof *e->workers);
        if (!e->workers) { s = GPTPS_E_NOMEM; goto fail; }

        e->dispatcher = gptps_thread_start(dispatcher_main, e);
        if (!e->dispatcher) { s = GPTPS_E_NOMEM; goto fail; }
        for (i = 0; i < e->nworkers; ++i) {
            e->workers[i] = gptps_thread_start(worker_main, e);
            if (!e->workers[i]) { s = GPTPS_E_NOMEM; goto fail_threads; }
        }
    }

    *out_engine = e;
    return GPTPS_OK;

fail_threads:
    gptps_mutex_lock(e->m);
    e->stopping = true;
    gptps_cond_signal(e->cv_disp);
    gptps_mutex_unlock(e->m);
    gptps_thread_join(e->dispatcher);
    for (i = 0; i < e->nworkers; ++i) if (e->workers[i]) gptps_thread_join(e->workers[i]);
fail:
    if (e->settings) gptps_settings_destroy(e->settings);
    if (e->workers) gptps_free(e->workers);
    if (e->cv_drain) gptps_cond_destroy(e->cv_drain);
    if (e->cv_work) gptps_cond_destroy(e->cv_work);
    if (e->cv_disp) gptps_cond_destroy(e->cv_disp);
    if (e->m) gptps_mutex_destroy(e->m);
    gptps_free(e);
    return s;
}

/* Apply a single config table's task overrides onto a task def. Values present
 * in the file override the def's compiled-in defaults (file wins). */
static void apply_task_table(const gptps_toml *t, const char *section,
                             gptps_task_def *def, int32_t *priority)
{
    long long ll;
    const char *s;
    if (gptps_toml_int(t, section, "timeout_seconds", &ll))       def->default_policy.timeout_seconds = (uint32_t)ll;
    if (gptps_toml_int(t, section, "max_retries", &ll))           def->default_policy.max_retries = (uint32_t)ll;
    if (gptps_toml_int(t, section, "retry_backoff_seconds", &ll)) def->default_policy.retry_backoff_seconds = (uint32_t)ll;
    if (gptps_toml_int(t, section, "mem_bytes", &ll))             def->default_cost.mem_bytes = (uint64_t)ll;
    if (gptps_toml_int(t, section, "priority", &ll))              *priority = (int32_t)ll;
    s = gptps_toml_str(t, section, "on_failure");
    if (s) {
        if      (strcmp(s, "drop") == 0)        def->default_policy.on_failure = GPTPS_ON_FAILURE_DROP;
        else if (strcmp(s, "requeue") == 0)     def->default_policy.on_failure = GPTPS_ON_FAILURE_REQUEUE;
        else if (strcmp(s, "dead_letter") == 0) def->default_policy.on_failure = GPTPS_ON_FAILURE_DEAD_LETTER;
    }
}

/* Layer file config over a task def: global [task_defaults] first, then the
 * task-specific [tasks.<name>] table (most specific wins). No-op without a file. */
static void apply_task_config(const gptps_toml *t, const char *name,
                              gptps_task_def *def, int32_t *priority)
{
    char section[300];
    if (!t) return;
    apply_task_table(t, "task_defaults", def, priority);
    if (strlen(name) < sizeof section - 7) {
        strcpy(section, "tasks.");
        strcat(section, name);
        apply_task_table(t, section, def, priority);
    }
}

gptps_status gptps_open(const char *config_path, gptps **out_engine)
{
    gptps_config cfg;
    gptps_toml *t = NULL;
    gptps_status s;

    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.config_path = config_path;
    cfg.limits.struct_size = sizeof cfg.limits;

    if (config_path) {
        char err[128];
        long long ll;
        double gb;
        t = gptps_toml_parse_file(config_path, err, sizeof err);
        if (!t) return GPTPS_E_CONFIG;
        /* [limits]: explicit file values seed the config; 0/absent => auto-tune */
        if (gptps_toml_int(t, "limits", "max_concurrent_tasks", &ll))
            cfg.limits.max_concurrent_tasks = (uint32_t)ll;
        if (gptps_toml_int(t, "limits", "max_memory_bytes", &ll))
            cfg.limits.max_memory_bytes = (uint64_t)ll;
        else if (gptps_toml_double(t, "limits", "max_memory_gb", &gb) && gb > 0.0)
            cfg.limits.max_memory_bytes = (uint64_t)(gb * 1073741824.0);
        if (gptps_toml_int(t, "limits", "max_intake_depth", &ll) && ll >= 0)
            cfg.limits.max_intake_depth = (uint32_t)ll;
    }

    s = gptps_open_ex(&cfg, out_engine);
    if (s != GPTPS_OK) { gptps_toml_free(t); return s; }

    if (t) {
        const char *const *addons;
        long long ll;
        int n, k;
        (*out_engine)->toml = t;            /* retained for register-time task overrides */
        /* [scheduler]: starvation-guard knob (0 => reserve immediately, no backfill) */
        if (gptps_toml_int(t, "scheduler", "reserve_after_skips", &ll) && ll >= 0)
            (*out_engine)->reserve_after_skips = (uint32_t)ll;
        /* top-level addons = ["lib1.so", ...] : auto-load (best-effort) */
        n = gptps_toml_str_array(t, "", "addons", &addons);
        for (k = 0; k < n; ++k) (void)gptps_load_addon(*out_engine, addons[k]);
    }
    return GPTPS_OK;
}

/* deep-copy a NULL-terminated argv; returns NULL on alloc failure or empty */
static char **argv_dup(const char *const *argv)
{
    size_t n = 0, i;
    char **out;
    while (argv[n]) ++n;
    out = (char **)gptps_calloc(n + 1, sizeof *out);
    if (!out) return NULL;
    for (i = 0; i < n; ++i) {
        size_t L = strlen(argv[i]) + 1;
        out[i] = (char *)gptps_malloc(L);
        if (!out[i]) { while (i--) gptps_free(out[i]); gptps_free(out); return NULL; }
        memcpy(out[i], argv[i], L);
    }
    return out;
}

gptps_status gptps_register_task(gptps *e, const gptps_task_def *def)
{
    gptps_reg *r;
    char *name;
    char **argv_copy = NULL;
    uint64_t svc_flags;

    if (!e || !def || !def->name) return GPTPS_E_INVAL;
    if (def->struct_size < GPTPS_TASK_DEF_MIN_SIZE) return GPTPS_E_INVAL; /* ABI: below the frozen minimum */
    if (def->exec == GPTPS_EXEC_PROGRAM) {
        if (!def->argv || !def->argv[0]) return GPTPS_E_INVAL; /* program needs an argv */
    } else if (!def->run) {
        return GPTPS_E_INVAL;                                 /* in-process kinds need a run fn */
    }

    /* v1.11 SERVICE flag (an appended field: read it only if the caller's struct
     * actually carries it, so a pre-v1.11 def stays valid). */
    svc_flags = GPTPS_STRUCT_HAS(gptps_task_def, def, flags) ? def->flags : 0u;
    if (svc_flags & GPTPS_TASK_SERVICE) {
        /* A service is a cooperative in-process loop, supervised by the worker pool,
         * that runs until stopped - so v1 forbids configs that contradict that:
         * an OS-enforced executor (its child-cancel plumbing is a later step), the
         * MANUAL pump (gptps_step runs a task to completion inline, so an infinite
         * loop would wedge the caller's thread), and a wall-clock timeout (would kill
         * the service rather than let it run). */
        if (def->exec != GPTPS_EXEC_INPROC)           return GPTPS_E_INVAL;
        if (e->manual)                                return GPTPS_E_INVAL;
        if (def->default_policy.timeout_seconds != 0) return GPTPS_E_INVAL;
    }

    if (def->exec == GPTPS_EXEC_PROGRAM) {
        argv_copy = argv_dup(def->argv);
        if (!argv_copy) return GPTPS_E_NOMEM;
    }

    gptps_mutex_lock(e->m);
    if (registry_find(e, def->name)) {
        gptps_mutex_unlock(e->m);
        if (argv_copy) { char **a = argv_copy; while (*a) gptps_free(*a++); gptps_free(argv_copy); }
        return GPTPS_E_DUP;
    }

    r = (gptps_reg *)gptps_calloc(1, sizeof *r);
    name = (char *)gptps_malloc(strlen(def->name) + 1);
    if (!r || !name) {
        gptps_free(r); gptps_free(name);
        if (argv_copy) { char **a = argv_copy; while (*a) gptps_free(*a++); gptps_free(argv_copy); }
        gptps_mutex_unlock(e->m);
        return GPTPS_E_NOMEM;
    }
    strcpy(name, def->name);
    if (e->nres) {                            /* per-item cost vector for the defined resources */
        r->res_cost = (uint64_t *)gptps_calloc(e->nres, sizeof(uint64_t));
        if (!r->res_cost) {
            gptps_free(r); gptps_free(name);
            if (argv_copy) { char **a = argv_copy; while (*a) gptps_free(*a++); gptps_free(argv_copy); }
            gptps_mutex_unlock(e->m);
            return GPTPS_E_NOMEM;
        }
    }

    /* append-safe copy: a pre-v1.11 caller's def may be smaller than ours, so
     * zero-fill first, then copy only the bytes it actually supplied (clamped to
     * our size so a future, larger caller cannot overflow r->def). */
    {
        size_t copy = def->struct_size < sizeof r->def ? def->struct_size : sizeof r->def;
        memset(&r->def, 0, sizeof r->def);
        memcpy(&r->def, def, copy);
    }
    r->name = name;
    r->def.name = name;
    r->argv_copy = argv_copy;
    r->def.argv = (const char *const *)argv_copy; /* point at our owned copy (NULL for non-PROGRAM) */
    if (r->def.default_cost.struct_size == 0) r->def.default_cost.struct_size = sizeof(gptps_cost);
    r->priority = 0;
    r->enabled = true;
    r->engine = e;
    apply_task_config(e->toml, name, &r->def, &r->priority); /* config file overrides compiled-in defaults */
    r->service = (svc_flags & GPTPS_TASK_SERVICE) != 0;
    r->retire_on_ok = (svc_flags & GPTPS_TASK_RETIRE_ON_OK) != 0; /* service only; consulted on a clean OK exit */
    if (r->service) {
        /* supervised restart-on-exit. Re-assert the policy AFTER file overrides so a
         * config file cannot un-service the type; each submitted item is normalized
         * again at submit time, so a live settings edit cannot break it either. */
        r->def.default_policy.on_failure      = GPTPS_ON_FAILURE_REQUEUE;
        r->def.default_policy.max_retries     = 0;
        r->def.default_policy.timeout_seconds = 0;
    }
    r->next = e->registry;
    e->registry = r;
    gptps_mutex_unlock(e->m);

    /* expose this task's knobs in the settings registry (after releasing e->m:
     * registry add takes settings->m then e->m, preserving the lock order) */
    register_task_settings(e, r);
    register_task_local_settings(e, r);   /* + any generic per-task settings defined so far */
    return GPTPS_OK;
}

gptps_status gptps_set_task_priority(gptps *e, const char *task_name, int priority)
{
    gptps_reg *r;
    if (!e || !task_name) return GPTPS_E_INVAL;
    gptps_mutex_lock(e->m);
    r = registry_find(e, task_name);
    if (r) r->priority = (int32_t)priority;   /* applies to subsequently-submitted items */
    gptps_mutex_unlock(e->m);
    return r ? GPTPS_OK : GPTPS_E_NOTFOUND;
}

/* ---- named resource budgets (generic admission limits) ---- */
gptps_status gptps_define_resource(gptps *e, const char *name, uint64_t budget)
{
    size_t i, oldn;
    gptps_reg *r;
    char *nm;
    if (!e || !name || !*name) return GPTPS_E_INVAL;
    gptps_mutex_lock(e->m);
    if (e->stopping) { gptps_mutex_unlock(e->m); return GPTPS_E_SHUTDOWN; }
    for (i = 0; i < e->nres; ++i)                       /* existing => just re-budget */
        if (strcmp(e->resources[i].name, name) == 0) {
            e->resources[i].budget = budget;
            gptps_mutex_unlock(e->m);
            return GPTPS_OK;
        }
    if (e->nres == e->rescap) {
        size_t nc = e->rescap ? e->rescap * 2 : 4;
        gptps_resource *nr = (gptps_resource *)gptps_realloc(e->resources, nc * sizeof *nr);
        if (!nr) { gptps_mutex_unlock(e->m); return GPTPS_E_NOMEM; }
        e->resources = nr; e->rescap = nc;
    }
    nm = (char *)gptps_malloc(strlen(name) + 1);
    if (!nm) { gptps_mutex_unlock(e->m); return GPTPS_E_NOMEM; }
    strcpy(nm, name);
    /* grow every task's per-resource cost vector by one (zeroed) slot */
    oldn = e->nres;
    for (r = e->registry; r; r = r->next) {
        uint64_t *nc2 = (uint64_t *)gptps_realloc(r->res_cost, (oldn + 1) * sizeof(uint64_t));
        if (!nc2) { gptps_free(nm); gptps_mutex_unlock(e->m); return GPTPS_E_NOMEM; }
        nc2[oldn] = 0;
        r->res_cost = nc2;
    }
    e->resources[e->nres].name = nm;
    e->resources[e->nres].budget = budget;
    e->resources[e->nres].reserved = 0;
    e->nres += 1;
    gptps_mutex_unlock(e->m);
    return GPTPS_OK;
}

gptps_status gptps_set_task_resource_cost(gptps *e, const char *task_name,
                                          const char *resource, uint64_t amount)
{
    size_t i;
    gptps_reg *r;
    if (!e || !task_name || !resource) return GPTPS_E_INVAL;
    gptps_mutex_lock(e->m);
    r = registry_find(e, task_name);
    if (!r) { gptps_mutex_unlock(e->m); return GPTPS_E_NOTFOUND; }
    for (i = 0; i < e->nres; ++i)
        if (strcmp(e->resources[i].name, resource) == 0) {
            if (r->res_cost) r->res_cost[i] = amount;
            gptps_mutex_unlock(e->m);
            return GPTPS_OK;
        }
    gptps_mutex_unlock(e->m);
    return GPTPS_E_NOTFOUND;                            /* unknown resource */
}

gptps_status gptps_resource_usage(gptps *e, const char *name,
                                  uint64_t *out_reserved, uint64_t *out_budget)
{
    size_t i;
    if (!e || !name) return GPTPS_E_INVAL;
    gptps_mutex_lock(e->m);
    for (i = 0; i < e->nres; ++i)
        if (strcmp(e->resources[i].name, name) == 0) {
            if (out_reserved) *out_reserved = e->resources[i].reserved;
            if (out_budget)   *out_budget   = e->resources[i].budget;
            gptps_mutex_unlock(e->m);
            return GPTPS_OK;
        }
    gptps_mutex_unlock(e->m);
    return GPTPS_E_NOTFOUND;
}

/* ------------------------------------------------------------------------- */
/* task management: enumerate / enable / clone / unregister                   */
/* ------------------------------------------------------------------------- */

/* item bookkeeping helpers (caller holds e->m) */
static unsigned fifo_count_reg(const gptps_fifo *q, const gptps_reg *r)
{ const gptps_item *it; unsigned n = 0; for (it = q->head; it; it = it->next) if (it->reg == r) ++n; return n; }

/* live (non-dead-letter) items still referencing r */
static unsigned reg_live_refs(const gptps *e, const gptps_reg *r)
{
    return fifo_count_reg(&e->intake, r) + fifo_count_reg(&e->delayed, r)
         + fifo_count_reg(&e->ready, r)  + fifo_count_reg(&e->done, r)
         + fifo_count_reg(&e->running_items, r);
}

/* remove + free every item in q referencing r (caller holds e->m). Only used on
 * queues whose items have NOT reserved admission budget (intake / delayed). */
static unsigned fifo_drop_reg(gptps_fifo *q, const gptps_reg *r)
{
    gptps_item *it = q->head, *prev = NULL; unsigned n = 0;
    while (it) {
        gptps_item *next = it->next;
        if (it->reg == r) {
            if (prev) prev->next = next; else q->head = next;
            if (q->tail == it) q->tail = prev;
            it->next = NULL; item_free(it); ++n;
            q->count -= 1;
        } else prev = it;
        it = next;
    }
    return n;
}

/* free every SERVICE item in q (caller holds e->m). Only for queues whose items
 * have NOT reserved admission budget (intake / delayed); running/ready items must
 * flow through `done` so their budget is released. */
static unsigned fifo_drop_services(gptps_fifo *q)
{
    gptps_item *it = q->head, *prev = NULL; unsigned n = 0;
    while (it) {
        gptps_item *next = it->next;
        if (it->reg && it->reg->service) {
            if (prev) prev->next = next; else q->head = next;
            if (q->tail == it) q->tail = prev;
            it->next = NULL; item_free(it); ++n;
            q->count -= 1;
        } else prev = it;
        it = next;
    }
    return n;
}

/* give every retained dead-letter item referencing r its own name copy and sever
 * the reg pointer, so freeing r leaves those items valid (caller holds e->m). */
static void detach_dead_letter(gptps *e, gptps_reg *r)
{
    gptps_item *it;
    for (it = e->dead_letter.head; it; it = it->next) {
        if (it->reg == r) {
            if (!it->name_owned) {
                size_t L = strlen(r->name) + 1;
                char *nm = (char *)gptps_malloc(L);
                if (nm) { memcpy(nm, r->name, L); it->name_owned = nm; }
            }
            it->reg = NULL; it->def = NULL;   /* only name_owned is read hereafter */
        }
    }
}

static void registry_unlink(gptps *e, gptps_reg *r)
{
    gptps_reg *cur = e->registry, *prev = NULL;
    while (cur) {
        if (cur == r) { if (prev) prev->next = cur->next; else e->registry = cur->next; cur->next = NULL; return; }
        prev = cur; cur = cur->next;
    }
}

/* free a reg's owned resources (r already unlinked + its settings removed) */
static void reg_destroy(gptps_reg *r)
{
    gptps_task_local *L = r->locals;
    while (L) { gptps_task_local *n = L->next; gptps_free(L); L = n; }
    if (r->argv_copy) { char **a = r->argv_copy; while (*a) gptps_free(*a++); gptps_free(r->argv_copy); }
    gptps_free(r->res_cost);
    gptps_free(r->name);
    gptps_free(r);
}

size_t gptps_task_count(gptps *e)
{
    size_t n = 0; gptps_reg *r;
    if (!e) return 0;
    gptps_mutex_lock(e->m);
    for (r = e->registry; r; r = r->next) ++n;   /* includes draining types */
    gptps_mutex_unlock(e->m);
    return n;
}

gptps_status gptps_task_get_info(gptps *e, size_t index, gptps_task_info *out)
{
    gptps_reg *r; size_t i = 0;
    if (!e || !out) return GPTPS_E_INVAL;
    if (out->struct_size < sizeof *out) return GPTPS_E_INVAL;   /* ABI: reject undersized struct */
    gptps_mutex_lock(e->m);
    for (r = e->registry; r && i < index; r = r->next) ++i;
    if (!r) { gptps_mutex_unlock(e->m); return GPTPS_E_NOTFOUND; }
    out->name = r->name; out->exec = r->def.exec; out->priority = r->priority;
    out->default_cost = r->def.default_cost; out->default_policy = r->def.default_policy;
    out->enabled = r->enabled ? 1 : 0; out->removed = r->removed ? 1 : 0;
    out->queued  = fifo_count_reg(&e->intake, r) + fifo_count_reg(&e->delayed, r);
    out->running = fifo_count_reg(&e->ready, r) + fifo_count_reg(&e->running_items, r) + fifo_count_reg(&e->done, r);
    out->dead    = fifo_count_reg(&e->dead_letter, r);
    gptps_mutex_unlock(e->m);
    return GPTPS_OK;
}

int gptps_task_exists(gptps *e, const char *task_name)
{
    gptps_reg *r; int yes;
    if (!e || !task_name) return 0;
    gptps_mutex_lock(e->m);
    r = registry_find(e, task_name);          /* skips draining types */
    yes = (r && r->enabled) ? 1 : 0;
    gptps_mutex_unlock(e->m);
    return yes;
}

gptps_status gptps_set_task_enabled(gptps *e, const char *task_name, int enabled)
{
    gptps_reg *r;
    if (!e || !task_name) return GPTPS_E_INVAL;
    gptps_mutex_lock(e->m);
    r = registry_find(e, task_name);
    if (r) r->enabled = enabled ? true : false;
    gptps_mutex_unlock(e->m);
    return r ? GPTPS_OK : GPTPS_E_NOTFOUND;
}

gptps_status gptps_clone_task(gptps *e, const char *src_name, const char *dst_name)
{
    gptps_task_def def;
    gptps_reg *r;
    int32_t prio;
    char **argv_snapshot = NULL;
    gptps_status st;

    if (!e || !src_name || !dst_name) return GPTPS_E_INVAL;

    gptps_mutex_lock(e->m);
    r = registry_find(e, src_name);
    if (!r) { gptps_mutex_unlock(e->m); return GPTPS_E_NOTFOUND; }
    if (registry_find(e, dst_name)) { gptps_mutex_unlock(e->m); return GPTPS_E_DUP; }
    def = r->def;                       /* shares run/cost/user_data; copies exec/cost/policy */
    prio = r->priority;
    if (def.exec == GPTPS_EXEC_PROGRAM && r->argv_copy) {
        argv_snapshot = argv_dup((const char *const *)r->argv_copy);   /* own a copy across the unlock */
        if (!argv_snapshot) { gptps_mutex_unlock(e->m); return GPTPS_E_NOMEM; }
    }
    gptps_mutex_unlock(e->m);

    def.name = dst_name;
    def.argv = (const char *const *)argv_snapshot;   /* register_task deep-copies this */
    st = gptps_register_task(e, &def);               /* re-applies [tasks.<dst>] config too */
    if (argv_snapshot) { char **a = argv_snapshot; while (*a) gptps_free(*a++); gptps_free(argv_snapshot); }
    if (st != GPTPS_OK) return st;
    gptps_set_task_priority(e, dst_name, prio);   /* carry the source priority (incl. 0) over config */
    return GPTPS_OK;
}

gptps_status gptps_unregister_task(gptps *e, const char *task_name, unsigned flags)
{
    gptps_reg *r;
    unsigned mode = flags & GPTPS_REMOVE_MODE_MASK;
    char prefix[320];

    if (!e || !task_name) return GPTPS_E_INVAL;
    if (strlen(task_name) > sizeof prefix - 8) return GPTPS_E_INVAL;

    gptps_mutex_lock(e->m);
    if (e->stopping) { gptps_mutex_unlock(e->m); return GPTPS_E_SHUTDOWN; }
    r = registry_find(e, task_name);
    if (!r) { gptps_mutex_unlock(e->m); return GPTPS_E_NOTFOUND; }

    /* A service's instances run until stopped, so a DRAIN (wait for work to finish)
     * would block forever in THREADED mode / refuse forever in MANUAL. Upgrade it to
     * CANCEL, which cooperatively stops the running instances. REJECT_IF_BUSY is left
     * as-is: "remove only if idle" is still a meaningful request for a service. */
    if (r->service && mode == GPTPS_REMOVE_DRAIN) mode = GPTPS_REMOVE_CANCEL;

    if (e->manual) {
        /* MANUAL: no worker threads => nothing is in-flight between gptps_step calls. */
        if (mode == GPTPS_REMOVE_CANCEL) {
            r->removed = true; r->cancelling = true;
            fifo_drop_reg(&e->intake, r);
            fifo_drop_reg(&e->delayed, r);
        } else if (reg_live_refs(e, r) > 0) {
            gptps_mutex_unlock(e->m);     /* DRAIN/REJECT: step the queue empty first, then remove */
            return GPTPS_E_BUSY;
        } else {
            r->removed = true;
        }
    } else {
        /* THREADED: tombstone, optionally cancel, then block until the type drains. */
        if (mode == GPTPS_REMOVE_REJECT_IF_BUSY && reg_live_refs(e, r) > 0) {
            gptps_mutex_unlock(e->m);
            return GPTPS_E_BUSY;
        }
        r->removed = true;               /* reject new submits + stop retries (bounded drain) */
        if (mode == GPTPS_REMOVE_CANCEL) {
            gptps_item *it;
            r->cancelling = true;             /* in-flight items are discarded, not dead-lettered */
            fifo_drop_reg(&e->intake, r);     /* drop the queued backlog (no budget reserved yet) */
            fifo_drop_reg(&e->delayed, r);
            for (it = e->running_items.head; it; it = it->next)
                if (it->reg == r) gptps_flag_set(it->cancel, true);   /* cooperative cancel in-flight */
            gptps_cond_broadcast(e->cv_work);
        }
        gptps_cond_signal(e->cv_disp);        /* wake the dispatcher to drive the drain */
        while (reg_live_refs(e, r) > 0)
            gptps_cond_wait(e->cv_drain, e->m);
    }

    /* A concurrent gptps_define_task_setting may hold a snapshot of this reg and be
     * materializing onto it; wait for that to finish before freeing the slot (the
     * define broadcasts cv_drain on completion, so this terminates even in MANUAL). */
    while (e->active_defines > 0)
        gptps_cond_wait(e->cv_drain, e->m);

    /* teardown (still holding e->m): make any retained dead-letter items self-owning,
     * then unlink the slot from the registry. */
    detach_dead_letter(e, r);
    registry_unlink(e, r);
    gptps_mutex_unlock(e->m);

    /* tear down its settings with e->m RELEASED (settings->m -> e->m order), then free */
    snprintf(prefix, sizeof prefix, "tasks.%s.", task_name);
    gptps_settings_remove_prefix(e->settings, prefix);
    reg_destroy(r);
    return GPTPS_OK;
}

/* --- generic settings: public entry points --- */
gptps_status gptps_define_global(gptps *e, const char *key, gptps_setting_type type,
                                 const char *default_val, const char *constraint, unsigned flags)
{
    gptps_owned_setting *o;
    gptps_setting_def d;
    int has_range = 0; double mn = 0, mx = 0;
    char **choices = NULL;
    const char *dv;
    gptps_status st;
    size_t klen;

    if (!e || !key || !*key) return GPTPS_E_INVAL;
    if (type == GPTPS_SETTING_ENUM) {
        choices = parse_choices(constraint);
        if (!choices) return GPTPS_E_CONFIG;            /* enum needs a choice set */
    } else if (type == GPTPS_SETTING_INT || type == GPTPS_SETTING_UINT || type == GPTPS_SETTING_DOUBLE) {
        if (!parse_range(constraint, &has_range, &mn, &mx)) return GPTPS_E_CONFIG;
    }
    dv = default_val ? default_val : gtype_zero(type, (const char *const *)choices);
    if (!gval_ok(type, has_range, mn, mx, (const char *const *)choices, dv)) { free_choices(choices); return GPTPS_E_CONFIG; }

    o = (gptps_owned_setting *)gptps_calloc(1, sizeof *o);
    klen = strlen(key) + 1;
    if (o) o->key = (char *)gptps_malloc(klen);
    if (!o || !o->key) { if (o) gptps_free(o->key); gptps_free(o); free_choices(choices); return GPTPS_E_NOMEM; }
    memcpy(o->key, key, klen);
    o->choices = choices;
    snprintf(o->value, sizeof o->value, "%s", dv);

    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.key = key; d.type = type; d.hot = !(flags & GPTPS_SETTING_RESTART);
    d.desc = "custom setting"; d.has_range = has_range; d.min = mn; d.max = mx;
    d.choices = (const char *const *)choices; d.target = o; d.read = os_rd; d.write = os_wr;
    st = gptps_register_setting(e, &d);   /* copies key/desc; takes settings->m */
    if (st != GPTPS_OK) { free_choices(choices); gptps_free(o->key); gptps_free(o); return st; }

    gptps_mutex_lock(e->m); o->next = e->owned_settings; e->owned_settings = o; gptps_mutex_unlock(e->m);
    return GPTPS_OK;
}

gptps_status gptps_define_task_setting(gptps *e, const char *leaf, gptps_setting_type type,
                                       const char *default_val, const char *constraint, unsigned flags)
{
    gptps_task_schema *sc, *it;
    int has_range = 0; double mn = 0, mx = 0;
    char **choices = NULL;
    const char *dv;
    gptps_reg **snap = NULL; size_t nsnap = 0, cap = 0, i;
    gptps_reg *r;

    if (!e || !leaf || !*leaf || strchr(leaf, '.')) return GPTPS_E_INVAL;  /* leaf is a bare key */
    {   /* reject collisions with the six built-in per-task leaves (they are not in
         * task_schemas, so they would otherwise pass the dup check yet fail to materialize) */
        static const char *const BUILTIN[] = { "timeout_seconds", "max_retries", "retry_backoff_seconds",
                                               "mem_bytes", "priority", "on_failure", 0 };
        const char *const *b;
        for (b = BUILTIN; *b; ++b) if (strcmp(*b, leaf) == 0) return GPTPS_E_DUP;
    }
    if (type == GPTPS_SETTING_ENUM) {
        choices = parse_choices(constraint);
        if (!choices) return GPTPS_E_CONFIG;
    } else if (type == GPTPS_SETTING_INT || type == GPTPS_SETTING_UINT || type == GPTPS_SETTING_DOUBLE) {
        if (!parse_range(constraint, &has_range, &mn, &mx)) return GPTPS_E_CONFIG;
    }
    dv = default_val ? default_val : gtype_zero(type, (const char *const *)choices);
    if (!gval_ok(type, has_range, mn, mx, (const char *const *)choices, dv)) { free_choices(choices); return GPTPS_E_CONFIG; }

    sc = (gptps_task_schema *)gptps_calloc(1, sizeof *sc);
    if (sc) { sc->leaf = (char *)gptps_malloc(strlen(leaf) + 1); sc->defval = (char *)gptps_malloc(strlen(dv) + 1); }
    if (!sc || !sc->leaf || !sc->defval) {
        if (sc) { gptps_free(sc->leaf); gptps_free(sc->defval); gptps_free(sc); }
        free_choices(choices); return GPTPS_E_NOMEM;
    }
    strcpy(sc->leaf, leaf); strcpy(sc->defval, dv);
    sc->type = type; sc->hot = !(flags & GPTPS_SETTING_RESTART);
    sc->has_range = has_range; sc->min = mn; sc->max = mx; sc->choices = choices;

    gptps_mutex_lock(e->m);
    for (it = e->task_schemas; it; it = it->next)
        if (strcmp(it->leaf, leaf) == 0) {            /* leaf already defined */
            gptps_mutex_unlock(e->m);
            gptps_free(sc->leaf); gptps_free(sc->defval); gptps_free(sc); free_choices(choices);
            return GPTPS_E_DUP;
        }
    sc->next = e->task_schemas; e->task_schemas = sc;
    for (r = e->registry; r; r = r->next) {           /* snapshot existing live tasks */
        if (r->removed) continue;
        if (nsnap == cap) {
            size_t nc = cap ? cap * 2 : 8;
            gptps_reg **ns = (gptps_reg **)gptps_realloc(snap, nc * sizeof *ns);
            if (!ns) break;                            /* best-effort: materialize what we captured */
            snap = ns; cap = nc;
        }
        snap[nsnap++] = r;
    }
    e->active_defines += 1;          /* pin: gptps_unregister_task must not free a reg while we materialize */
    gptps_mutex_unlock(e->m);

    for (i = 0; i < nsnap; ++i) materialize_task_local(e, snap[i], sc);   /* e->m released */

    gptps_mutex_lock(e->m);
    e->active_defines -= 1;
    gptps_cond_broadcast(e->cv_drain);   /* wake any unregister waiting on the pin */
    gptps_mutex_unlock(e->m);

    gptps_free(snap);
    return GPTPS_OK;
}

gptps_status gptps_task_setting_str(gptps_ctx *ctx, const char *key, char *buf, size_t cap)
{
    gptps_task_local *L = NULL;
    gptps *e;
    if (!ctx || !ctx->engine || !ctx->reg || !key || !buf || cap == 0) return GPTPS_E_INVAL;
    e = ctx->engine;
    gptps_mutex_lock(e->m);
    for (L = ctx->reg->locals; L; L = L->next)
        if (strcmp(L->schema->leaf, key) == 0) { snprintf(buf, cap, "%s", L->value); break; }
    gptps_mutex_unlock(e->m);
    return L ? GPTPS_OK : GPTPS_E_NOTFOUND;
}

gptps_status gptps_task_setting_int(gptps_ctx *ctx, const char *key, long *out)
{
    char b[GPTPS_SETTINGS_VALUE_MAX], *end;
    long v;
    gptps_status st;
    if (!out) return GPTPS_E_INVAL;
    st = gptps_task_setting_str(ctx, key, b, sizeof b);
    if (st != GPTPS_OK) return st;
    v = strtol(b, &end, 10);
    if (end == b || *end) return GPTPS_E_INVAL;   /* not an integer setting */
    *out = v;
    return GPTPS_OK;
}

/* --- settings: public forwarders onto the registry --- */
gptps_status gptps_register_setting(gptps *e, const gptps_setting_def *def)
{ return e ? gptps_settings_add(e->settings, def) : GPTPS_E_INVAL; }
size_t gptps_settings_count(gptps *e)
{ return e ? gptps_settings_size(e->settings) : 0; }
gptps_status gptps_settings_get_info(gptps *e, size_t index, gptps_setting_info *out)
{ return e ? gptps_settings_info_at(e->settings, index, out) : GPTPS_E_INVAL; }
gptps_status gptps_settings_get(gptps *e, const char *key, char *buf, size_t cap)
{ return e ? gptps_settings_get_by(e->settings, key, buf, cap) : GPTPS_E_INVAL; }
gptps_status gptps_settings_set(gptps *e, const char *key, const char *value)
{ return e ? gptps_settings_set_by(e->settings, key, value) : GPTPS_E_INVAL; }

gptps_status gptps_settings_save(gptps *e, const char *path)
{
    if (!e) return GPTPS_E_INVAL;
    if (!path) path = e->config_path;
    if (!path) return GPTPS_E_INVAL;
    return gptps_settings_save_to(e->settings, path);
}

gptps_status gptps_settings_reload(gptps *e, const char *path)
{
    gptps_toml *t, *old;
    gptps_status st;
    if (!e) return GPTPS_E_INVAL;
    if (!path) path = e->config_path;
    if (!path) return GPTPS_E_INVAL;
    t = gptps_toml_parse_file(path, NULL, 0);
    if (!t) return GPTPS_E_CONFIG;
    st = gptps_settings_apply_toml(e->settings, t);   /* re-apply known keys (validated) */
    gptps_mutex_lock(e->m);                            /* swap so future task registrations see it */
    old = e->toml; e->toml = t;
    gptps_mutex_unlock(e->m);
    gptps_toml_free(old);
    return st;
}

gptps_status gptps_settings_watch(gptps *e, gptps_settings_cb cb, void *user_data)
{ return e ? gptps_settings_watch_add(e->settings, cb, user_data) : GPTPS_E_INVAL; }

/* ------------------------------------------------------------------------- */
/* add-on loader (host-table ABI)                                            */
/* ------------------------------------------------------------------------- */

/* host-table entry: lets an add-on emit an event through the engine */
static gptps_status api_emit_event(gptps *e, const gptps_event *ev)
{
    gptps_event_cb cb; void *ud;
    if (!e || !ev) return GPTPS_E_INVAL;
    gptps_mutex_lock(e->m); cb = e->ev_cb; ud = e->ev_ud; gptps_mutex_unlock(e->m);
    if (cb) cb(ev, ud);
    return GPTPS_OK;
}

/* The versioned function-pointer table add-ons call the core through. Add-ons
 * NEVER link core symbols directly; everything routes through this table. */
static const gptps_api_routines G_API = {
    sizeof(gptps_api_routines),
    GPTPS_ABI_VERSION_MAJOR,
    GPTPS_ABI_VERSION_MINOR,
    gptps_register_task,
    api_emit_event,
    gptps_log,
    gptps_result_set,
    gptps_payload,
    gptps_register_constraint,
    gptps_register_observer,
    gptps_register_setting,
    gptps_unregister_task,
    gptps_task_exists,
    gptps_define_global,
    gptps_define_task_setting,
    gptps_cancel,
    gptps_unregister_constraint,
    gptps_unregister_observer,
    gptps_define_resource,
    gptps_set_task_resource_cost,
    gptps_resource_usage,
    gptps_set_scheduler
};

gptps_status gptps_load_addon(gptps *e, const char *path)
{
    gptps_dl *dl;
    void *sym;
    gptps_addon_init_fn init;
    const gptps_addon *addon;
    gptps_loaded *node;
    char *err = NULL;
    gptps_status s;

    if (!e || !path) return GPTPS_E_INVAL;

    dl = gptps_dl_open(path);
    if (!dl) return GPTPS_E_IO;

    sym = gptps_dl_sym(dl, "gptps_addon_init");
    if (!sym) { gptps_dl_close(dl); return GPTPS_E_ABI; }
    memcpy(&init, &sym, sizeof init); /* portable object-ptr -> function-ptr */

    addon = init(&G_API);
    if (!addon ||
        addon->magic != GPTPS_ABI_MAGIC ||
        addon->abi_version_major != GPTPS_ABI_VERSION_MAJOR ||
        addon->struct_size < sizeof *addon) {
        gptps_dl_close(dl);
        return GPTPS_E_ABI;
    }

    if (addon->setup) {
        s = addon->setup(e, &G_API, &err);
        if (s != GPTPS_OK) { gptps_dl_close(dl); return s; }
    }

    node = (gptps_loaded *)gptps_calloc(1, sizeof *node);
    if (!node) {
        if (addon->teardown) addon->teardown(e);
        gptps_dl_close(dl);
        return GPTPS_E_NOMEM;
    }
    node->dl = dl; node->addon = addon;
    gptps_mutex_lock(e->m);
    node->next = e->addons; e->addons = node;
    gptps_mutex_unlock(e->m);
    return GPTPS_OK;
}

static gptps_status submit_internal(gptps *e, const char *task_name,
                                    const void *payload, size_t len,
                                    const gptps_submit_options *opts,
                                    gptps_handle *out_handle)
{
    gptps_reg *r;
    gptps_item *it;
    gptps_cost cost;
    void *pcopy = NULL;

    if (!e || !task_name) return GPTPS_E_INVAL;
    if (opts && opts->struct_size < sizeof *opts) return GPTPS_E_INVAL; /* ABI: reject undersized */

    gptps_mutex_lock(e->m);
    if (e->stopping) { gptps_mutex_unlock(e->m); return GPTPS_E_SHUTDOWN; }

    r = registry_find(e, task_name);
    if (!r || !r->enabled) { gptps_mutex_unlock(e->m); return GPTPS_E_NOTFOUND; } /* unknown, draining, or paused */

    cost = r->def.default_cost;
    if (r->def.cost) {
        gptps_status cs = r->def.cost(payload, len, &cost, r->def.user_data);
        if (cs != GPTPS_OK) { gptps_mutex_unlock(e->m); return cs; }
    }
    if (cost.mem_bytes > e->limits.max_memory_bytes) {
        gptps_mutex_unlock(e->m);
        return GPTPS_E_BUDGET; /* never-fits: reject at submit */
    }
    if (e->nres && r->res_cost) {           /* a resource cost that can never fit its budget */
        size_t ri;
        for (ri = 0; ri < e->nres; ++ri)
            if (r->res_cost[ri] > e->resources[ri].budget) { gptps_mutex_unlock(e->m); return GPTPS_E_BUDGET; }
    }
    /* backpressure: bound the intake queue so an overproducing client cannot grow
     * it without limit (max_memory_bytes bounds only the RUNNING set). 0 = off. */
    if (e->limits.max_intake_depth && e->intake.count >= e->limits.max_intake_depth) {
        gptps_mutex_unlock(e->m);
        return GPTPS_E_FULL;
    }

    if (len) {
        pcopy = gptps_malloc(len);
        if (!pcopy) { gptps_mutex_unlock(e->m); return GPTPS_E_NOMEM; }
        memcpy(pcopy, payload, len);
    }
    it = (gptps_item *)gptps_calloc(1, sizeof *it);
    if (it) it->cancel = gptps_flag_create(false);
    if (!it || !it->cancel) {
        if (it) gptps_flag_destroy(it->cancel);
        gptps_free(it); gptps_free(pcopy);
        gptps_mutex_unlock(e->m);
        return GPTPS_E_NOMEM;
    }

    it->handle = e->next_handle++;
    it->def = &r->def;
    it->reg = r;
    it->payload = pcopy;
    it->payload_len = len;
    it->cost = cost;
    it->policy = r->def.default_policy;
    it->priority = r->priority;
    it->skips = 0;
    it->attempt = 1;
    it->enqueue_ms = gptps_hal_monotonic_ms();   /* age basis for the scheduler seam */

    /* per-submit overrides (gptps_submit_ex): only flagged fields apply */
    if (opts) {
        if (opts->flags & GPTPS_SUBMIT_PRIORITY)   it->priority = opts->priority;
        if (opts->flags & GPTPS_SUBMIT_POLICY)     it->policy = opts->policy;
        if (opts->flags & GPTPS_SUBMIT_TIMEOUT_MS) it->timeout_ms_override = opts->timeout_ms;
    }
    it->sched_score = it->priority;   /* default ordering key; a scheduler hook re-scores per pass */
    if (r->service) {
        /* a service instance is supervised: restart-on-exit, never a wall-clock kill.
         * Re-assert here so a submit_ex override (or a live tasks.<name>.* edit that
         * changed the type default) cannot turn one instance into a one-shot. */
        it->policy.on_failure      = GPTPS_ON_FAILURE_REQUEUE;
        it->policy.max_retries     = 0;
        it->policy.timeout_seconds = 0;
        it->timeout_ms_override    = 0;
    }

    fifo_push(&e->intake, it);
    if (out_handle) *out_handle = it->handle;
    gptps_cond_signal(e->cv_disp);
    {
        /* Emit QUEUED with the lock RELEASED (every other emit site is already
         * off-lock). Holding e->m across an observer let a slow sink stall all
         * admission/dispatch. The name is copied into `p` before unlock, so it
         * stays valid even if the type is unregistered+freed during the emit. */
        gptps_pending_ev p;
        gptps_event_cb cb = e->ev_cb;
        void *ud = e->ev_ud;
        gptps_handle h = it->handle;
        uint64_t mem = cost.mem_bytes;
        p.kind = GPTPS_EV_QUEUED; p.handle = h; ev_set_name(p.name, r->def.name);
        p.status = GPTPS_OK; p.attempt = 0; p.mem = mem;
        p.result = NULL; p.result_len = 0;
        gptps_mutex_unlock(e->m);
        emit_now(e, cb, ud, &p);
    }
    return GPTPS_OK;
}

gptps_status gptps_submit(gptps *e, const char *task_name,
                          const void *payload, size_t len, gptps_handle *out_handle)
{ return submit_internal(e, task_name, payload, len, NULL, out_handle); }

gptps_status gptps_submit_ex(gptps *e, const char *task_name,
                             const void *payload, size_t len,
                             const gptps_submit_options *opts, gptps_handle *out_handle)
{ return submit_internal(e, task_name, payload, len, opts, out_handle); }

/* Cancel one submitted work item by handle. In-flight / admitted items get the
 * cooperative cancel flag (in-proc tasks must poll gptps_is_cancelled) and are
 * carried to a terminal FAILED outcome without retry; a still-queued item is
 * removed and a terminal GPTPS_EV_FAILED/GPTPS_E_CANCELLED event is emitted.
 * Returns GPTPS_E_NOTFOUND if the handle is unknown or already terminal (a
 * no-op cancel-after-completion), GPTPS_E_SHUTDOWN during teardown. */
gptps_status gptps_cancel(gptps *e, gptps_handle h)
{
    gptps_item *it;
    gptps_pending_ev p;
    gptps_event_cb cb = NULL;
    void *ud = NULL;
    int qi;
    gptps_fifo *queues[2];

    if (!e || h == 0) return GPTPS_E_INVAL;

    gptps_mutex_lock(e->m);
    if (e->stopping) { gptps_mutex_unlock(e->m); return GPTPS_E_SHUTDOWN; }

    /* in-flight (running) or admitted-but-unstarted (ready): mark cancelled and
     * raise the cooperative flag. The worker/dispatcher carries it to terminal
     * (a ready item is discarded before start; a running in-proc item observes
     * the flag). No budget bookkeeping here - done-processing releases it. */
    for (it = e->running_items.head; it; it = it->next)
        if (it->handle == h) {
            it->cancelled = 1; gptps_flag_set(it->cancel, true);
            gptps_mutex_unlock(e->m); return GPTPS_OK;
        }
    for (it = e->ready.head; it; it = it->next)
        if (it->handle == h) {
            it->cancelled = 1; gptps_flag_set(it->cancel, true);
            gptps_cond_broadcast(e->cv_work);   /* wake a worker to discard it */
            gptps_mutex_unlock(e->m); return GPTPS_OK;
        }
    /* finished-but-not-yet-reaped (in `done`): a worker has posted it and the
     * dispatcher has not made its terminal decision yet. Mark cancelled so that
     * decision frees it instead of retrying / REQUEUEing - without this a crash-
     * restarting service momentarily in `done` would dodge the cancel and restart.
     * Budget is released by the done-drain, so no ledger bookkeeping here. */
    for (it = e->done.head; it; it = it->next)
        if (it->handle == h) {
            it->cancelled = 1; gptps_flag_set(it->cancel, true);
            gptps_mutex_unlock(e->m); return GPTPS_OK;
        }

    /* queued (intake) or backoff-delayed: not admitted, no budget reserved -
     * remove, free, and emit a terminal cancelled event with the lock released. */
    queues[0] = &e->intake; queues[1] = &e->delayed;
    for (qi = 0; qi < 2; ++qi)
        for (it = queues[qi]->head; it; it = it->next)
            if (it->handle == h) {
                fifo_remove(queues[qi], it);
                p.kind = GPTPS_EV_FAILED; p.handle = h; ev_set_name(p.name, item_name(it));
                p.status = GPTPS_E_CANCELLED; p.attempt = it->attempt; p.mem = it->cost.mem_bytes;
                p.result = NULL; p.result_len = 0;
                cb = e->ev_cb; ud = e->ev_ud;
                item_free(it);
                gptps_mutex_unlock(e->m);
                emit_now(e, cb, ud, &p);        /* lock released: observers may re-enter */
                return GPTPS_OK;
            }

    gptps_mutex_unlock(e->m);
    return GPTPS_E_NOTFOUND;   /* unknown handle, or already terminal */
}

gptps_status gptps_set_event_cb(gptps *e, gptps_event_cb cb, void *user_data)
{
    if (!e) return GPTPS_E_INVAL;
    gptps_mutex_lock(e->m);
    e->ev_cb = cb; e->ev_ud = user_data;
    gptps_mutex_unlock(e->m);
    return GPTPS_OK;
}

/* Scheduler seam: swap the admission ORDERING key (fn == NULL resets to the
 * built-in priority ordering). Setup-time; the hook runs under e->m on the
 * dispatcher hot path (must be fast / non-reentrant). */
gptps_status gptps_set_scheduler(gptps *e, gptps_sched_fn fn, void *user_data)
{
    if (!e) return GPTPS_E_INVAL;
    gptps_mutex_lock(e->m);
    e->sched_fn = fn; e->sched_ud = user_data;
    gptps_cond_signal(e->cv_disp);   /* re-evaluate ordering on the next pass */
    gptps_mutex_unlock(e->m);
    return GPTPS_OK;
}

gptps_status gptps_register_observer(gptps *e, gptps_event_cb fn, void *user_data)
{
    gptps_observer *o;
    if (!e || !fn) return GPTPS_E_INVAL;
    o = (gptps_observer *)gptps_calloc(1, sizeof *o);
    if (!o) return GPTPS_E_NOMEM;
    o->fn = fn; o->ud = user_data;
    gptps_mutex_lock(e->m);
    o->next = e->observers; e->observers = o;
    gptps_mutex_unlock(e->m);
    return GPTPS_OK;
}

/* Remove a previously-registered constraint/observer by (fn, user_data). Like
 * registration, this is a SETUP-time operation: callers must not unregister
 * while the engine is actively emitting events / admitting work (event sinks are
 * iterated lock-free on the hot path), i.e. unregister when quiescent or before
 * submitting. Enables add-on hot-unload. GPTPS_E_NOTFOUND if no match. */
gptps_status gptps_unregister_constraint(gptps *e, gptps_constraint_fn fn, void *user_data)
{
    gptps_constraint *cur, *prev = NULL;
    if (!e || !fn) return GPTPS_E_INVAL;
    gptps_mutex_lock(e->m);
    for (cur = e->constraints; cur; prev = cur, cur = cur->next) {
        if (cur->fn == fn && cur->ud == user_data) {
            if (prev) prev->next = cur->next; else e->constraints = cur->next;
            gptps_mutex_unlock(e->m);
            gptps_free(cur);
            return GPTPS_OK;
        }
    }
    gptps_mutex_unlock(e->m);
    return GPTPS_E_NOTFOUND;
}

gptps_status gptps_unregister_observer(gptps *e, gptps_event_cb fn, void *user_data)
{
    gptps_observer *cur, *prev = NULL;
    if (!e || !fn) return GPTPS_E_INVAL;
    gptps_mutex_lock(e->m);
    for (cur = e->observers; cur; prev = cur, cur = cur->next) {
        if (cur->fn == fn && cur->ud == user_data) {
            if (prev) prev->next = cur->next; else e->observers = cur->next;
            gptps_mutex_unlock(e->m);
            gptps_free(cur);
            return GPTPS_OK;
        }
    }
    gptps_mutex_unlock(e->m);
    return GPTPS_E_NOTFOUND;
}

gptps_status gptps_register_constraint(gptps *e, gptps_constraint_fn fn, void *user_data)
{
    gptps_constraint *c;
    if (!e || !fn) return GPTPS_E_INVAL;
    c = (gptps_constraint *)gptps_calloc(1, sizeof *c);
    if (!c) return GPTPS_E_NOMEM;
    c->fn = fn; c->ud = user_data;
    gptps_mutex_lock(e->m);
    c->next = e->constraints; e->constraints = c;
    gptps_mutex_unlock(e->m);
    return GPTPS_OK;
}

size_t gptps_dead_letter_count(gptps *e)
{
    size_t n;
    if (!e) return 0;
    gptps_mutex_lock(e->m);
    n = e->dead_letter_count;
    gptps_mutex_unlock(e->m);
    return n;
}

size_t gptps_dead_letter_drain(gptps *e, gptps_dead_letter_cb cb, void *user_data)
{
    gptps_fifo local;
    gptps_item *it;
    size_t n = 0;

    if (!e) return 0;

    /* Detach the whole list under the lock, then iterate with the lock RELEASED
     * so the callback may re-enter the engine (e.g. re-submit) without deadlock. */
    gptps_mutex_lock(e->m);
    local = e->dead_letter;
    e->dead_letter.head = e->dead_letter.tail = NULL;
    e->dead_letter_count = 0;
    gptps_mutex_unlock(e->m);

    while ((it = fifo_pop(&local)) != NULL) {
        if (cb) {
            gptps_dead_letter dl;
            memset(&dl, 0, sizeof dl);
            dl.struct_size = sizeof dl;
            dl.handle = it->handle;
            dl.task_name = item_name(it);   /* reg name, or an owned copy if the task was removed */
            dl.status = it->outcome;
            dl.attempts = it->attempt;
            dl.payload = it->payload;
            dl.payload_len = it->payload_len;
            cb(&dl, user_data);
        }
        item_free(it);
        ++n;
    }
    return n;
}

/* Run one buffered batch of events with the lock released, then re-acquire.
 * `*npend` is consumed (set to 0). Caller holds the lock on entry and exit. */
static void flush_pending(gptps *e, gptps_pending_ev *pend, int *npend)
{
    if (*npend > 0) {
        gptps_event_cb cb = e->ev_cb; void *ud = e->ev_ud;
        int i;
        gptps_mutex_unlock(e->m);
        for (i = 0; i < *npend; ++i) emit_now(e, cb, ud, &pend[i]);
        gptps_mutex_lock(e->m);
        *npend = 0;
    }
}

gptps_status gptps_step(gptps *e, size_t *out_ran)
{
    gptps_pending_ev pend[GPTPS_PENDING_CAP];
    int npend;
    uint64_t next_wake;
    size_t ran = 0;
    gptps_item *it;

    if (out_ran) *out_ran = 0;
    if (!e) return GPTPS_E_INVAL;
    if (!e->manual) return GPTPS_E_INVAL;   /* threaded engines run themselves */

    gptps_mutex_lock(e->m);

    /* pass A: complete any prior work, promote backoff-ready retries, admit. */
    engine_pass(e, pend, &npend, &next_wake);
    flush_pending(e, pend, &npend);

    /* run everything admitted into `ready` to completion, inline on THIS thread */
    while ((it = fifo_pop(&e->ready)) != NULL) {
        gptps_status eff;
        gptps_event_cb cb = e->ev_cb; void *ud = e->ev_ud;  /* snapshot under lock */
        it->deadline_ms = (it->policy.timeout_seconds && it->def->exec == GPTPS_EXEC_INPROC)
            ? gptps_hal_monotonic_ms() + (uint64_t)it->policy.timeout_seconds * 1000u : 0;
        gptps_flag_set(it->cancel, false);
        fifo_push(&e->running_items, it);
        gptps_mutex_unlock(e->m);

        eff = execute(e, it, cb, ud);       /* STARTED + FINISHED/FAILED emitted here */

        gptps_mutex_lock(e->m);
        fifo_remove(&e->running_items, it);
        it->outcome = eff;
        fifo_push(&e->done, it);
        ++ran;
    }

    /* pass B: account the work just run (release budget, schedule retries / dead-letter). */
    engine_pass(e, pend, &npend, &next_wake);
    flush_pending(e, pend, &npend);

    gptps_mutex_unlock(e->m);
    if (out_ran) *out_ran = ran;
    return GPTPS_OK;
}

/* Stop every SERVICE instance so the dispatcher can reach its drain condition.
 * A service run() loops until the cancel flag is raised, and its REQUEUE policy
 * would otherwise restart it forever - so without this, a running service would
 * hang shutdown (running_items never empties) or a backing-off one would keep the
 * delayed queue non-empty. Running/ready instances (budget reserved) are marked
 * cancelled + flagged so they exit / are discarded through the normal `done` path
 * (which releases their budget); queued + backing-off instances (no budget) are
 * freed outright. Non-service in-flight work is untouched, so it still drains
 * gracefully. Caller holds e->m. */
static void stop_services(gptps *e)
{
    gptps_item *it;
    for (it = e->running_items.head; it; it = it->next)
        if (it->reg && it->reg->service) { it->cancelled = 1; gptps_flag_set(it->cancel, true); }
    for (it = e->ready.head; it; it = it->next)
        if (it->reg && it->reg->service) { it->cancelled = 1; gptps_flag_set(it->cancel, true); }
    fifo_drop_services(&e->intake);
    fifo_drop_services(&e->delayed);
}

gptps_status gptps_shutdown(gptps *e)
{
    unsigned i;
    gptps_reg *r;
    gptps_item *it;

    if (!e) return GPTPS_E_INVAL;

    gptps_mutex_lock(e->m);
    e->stopping = true;
    stop_services(e);                   /* cooperatively stop long-running service instances */
    gptps_cond_signal(e->cv_disp);
    gptps_cond_broadcast(e->cv_work);   /* wake idle workers to discard cancelled service instances */
    gptps_mutex_unlock(e->m);

    if (!e->manual) {                       /* MANUAL spawns no threads to join */
        gptps_thread_join(e->dispatcher);
        for (i = 0; i < e->nworkers; ++i) gptps_thread_join(e->workers[i]);
    }

    /* tear down add-ons (threads joined => no task code runs; last calls into
     * each .so, then unload) */
    {
        gptps_loaded *a = e->addons;
        while (a) {
            gptps_loaded *n = a->next;
            if (a->addon->teardown) a->addon->teardown(e);
            gptps_dl_close(a->dl);
            gptps_free(a);
            a = n;
        }
    }

    /* free anything left (dead-letter retained items, etc.). The ready/done/
     * running queues are already empty in THREADED mode (the dispatcher drains
     * them before exit); in MANUAL mode they may hold items if the caller stopped
     * stepping mid-drain, so free them here too. */
    while ((it = fifo_pop(&e->dead_letter)) != NULL) item_free(it);
    while ((it = fifo_pop(&e->intake)) != NULL) item_free(it);
    while ((it = fifo_pop(&e->delayed)) != NULL) item_free(it);
    while ((it = fifo_pop(&e->ready)) != NULL) item_free(it);
    while ((it = fifo_pop(&e->done)) != NULL) item_free(it);
    while ((it = fifo_pop(&e->running_items)) != NULL) item_free(it);

    r = e->registry;
    while (r) {
        gptps_reg *n = r->next;
        reg_destroy(r);   /* frees locals + argv_copy + name + r */
        r = n;
    }
    { gptps_observer  *o = e->observers;  while (o) { gptps_observer  *n = o->next; gptps_free(o); o = n; } }
    { gptps_constraint *c = e->constraints; while (c) { gptps_constraint *n = c->next; gptps_free(c); c = n; } }
    /* generic global setting cells + per-task setting schemas (their settings-registry
     * entries are freed by gptps_settings_destroy; these are the owned backing stores) */
    { gptps_owned_setting *o = e->owned_settings; while (o) { gptps_owned_setting *n = o->next; free_choices(o->choices); gptps_free(o->key); gptps_free(o); o = n; } }
    { gptps_task_schema *s = e->task_schemas; while (s) { gptps_task_schema *n = s->next; free_choices(s->choices); gptps_free(s->leaf); gptps_free(s->defval); gptps_free(s); s = n; } }
    gptps_settings_destroy(e->settings);  /* entries reference e / regs, which are freed above/after; destroy only frees the schema list */
    { size_t i; for (i = 0; i < e->nres; ++i) gptps_free(e->resources[i].name); gptps_free(e->resources); }
    gptps_toml_free(e->toml);
    gptps_free(e->config_path);

    gptps_free(e->workers);
    gptps_cond_destroy(e->cv_drain);
    gptps_cond_destroy(e->cv_work);
    gptps_cond_destroy(e->cv_disp);
    gptps_mutex_destroy(e->m);
    gptps_free(e);
    return GPTPS_OK;
}
