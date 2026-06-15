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
 *   submit ─► [intake] ─► dispatcher admits (running<conc &&
 *                          reserved+cost<=max) ─► [ready] ─► worker runs
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

struct gptps_ctx {
    gptps        *engine;
    gptps_handle  handle;
    const char   *task_name;
    const void   *payload;
    size_t        payload_len;
    uint64_t      deadline_ms;
    gptps_flag   *cancel;            /* owned by the item, shared with the ctx */
    void        *result;
    size_t        result_len;
    void       (*result_free)(void *);
    bool          result_is_copy;    /* true => core malloc'd a copy; free() it */
    bool          result_set;
};

typedef struct gptps_item {
    gptps_handle          handle;
    const gptps_task_def *def;       /* points into the registry (stable) */
    void                 *payload;
    size_t                payload_len;
    gptps_cost            cost;
    gptps_failure_policy  policy;
    uint32_t              attempt;   /* 1 = first try */
    uint64_t              deadline_ms;   /* 0 = no timeout */
    uint64_t              not_before_ms; /* backoff gate for delayed retries */
    gptps_flag           *cancel;
    gptps_status          outcome;   /* effective status of the last attempt */
    struct gptps_item    *next;
} gptps_item;

typedef struct gptps_reg {
    gptps_task_def     def;
    char              *name;
    struct gptps_reg  *next;
} gptps_reg;

typedef struct gptps_loaded {
    gptps_dl            *dl;
    const gptps_addon   *addon;
    struct gptps_loaded *next;
} gptps_loaded;

typedef struct { gptps_item *head, *tail; } gptps_fifo;

/* pending event emitted after the lock is released */
typedef struct {
    gptps_event_kind kind;
    gptps_handle     handle;
    const char      *name;
    gptps_status     status;
    uint32_t         attempt;
    uint64_t         mem;
} gptps_pending_ev;

struct gptps {
    gptps_limits   limits;
    gptps_reg     *registry;

    gptps_mutex   *m;
    gptps_cond    *cv_disp;
    gptps_cond    *cv_work;

    gptps_fifo     intake;
    gptps_fifo     ready;
    gptps_fifo     done;
    gptps_fifo     delayed;        /* retries waiting for backoff */
    gptps_fifo     running_items;  /* in-flight, scanned for deadlines */
    gptps_fifo     dead_letter;    /* terminal failures retained */
    uint32_t       dead_letter_count;

    uint64_t       reserved_mem;   /* DISPATCHER-ONLY */
    uint32_t       running;        /* DISPATCHER-ONLY */

    gptps_thread  *dispatcher;
    gptps_thread **workers;
    unsigned       nworkers;

    bool           stopping;
    bool           workers_exit;

    gptps_handle   next_handle;
    gptps_event_cb ev_cb;
    void          *ev_ud;

    gptps_loaded  *addons;        /* dlopen'd add-ons, torn down at shutdown */
};

/* ------------------------------------------------------------------------- */
/* fifo helpers                                                              */
/* ------------------------------------------------------------------------- */

static void fifo_push(gptps_fifo *q, gptps_item *it)
{
    it->next = NULL;
    if (q->tail) q->tail->next = it; else q->head = it;
    q->tail = it;
}
static gptps_item *fifo_pop(gptps_fifo *q)
{
    gptps_item *it = q->head;
    if (it) { q->head = it->next; if (!q->head) q->tail = NULL; it->next = NULL; }
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
            return;
        }
        prev = cur; cur = cur->next;
    }
}

static const gptps_reg *registry_find(const gptps *e, const char *name)
{
    const gptps_reg *r;
    for (r = e->registry; r; r = r->next)
        if (strcmp(r->name, name) == 0) return r;
    return NULL;
}

static void item_free(gptps_item *it)
{
    if (!it) return;
    if (it->cancel) gptps_flag_destroy(it->cancel);
    free(it->payload);
    free(it);
}

static void emit_now(gptps *e, gptps_event_cb cb, void *ud, const gptps_pending_ev *p)
{
    gptps_event ev;
    (void)e;
    if (!cb) return;
    memset(&ev, 0, sizeof ev);
    ev.struct_size = sizeof ev;
    ev.kind = p->kind; ev.handle = p->handle; ev.task_name = p->name;
    ev.ts_ms = gptps_hal_monotonic_ms(); ev.status = p->status;
    ev.attempt = p->attempt; ev.mem_bytes = p->mem;
    cb(&ev, ud);
}

/* worker-side emit (already lock-free) */
static void emit(gptps *e, gptps_event_kind kind, gptps_handle h,
                 const char *name, gptps_status st, uint32_t attempt, uint64_t mem)
{
    gptps_pending_ev p;
    p.kind = kind; p.handle = h; p.name = name; p.status = st; p.attempt = attempt; p.mem = mem;
    emit_now(e, e->ev_cb, e->ev_ud, &p);
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

void gptps_log(gptps_ctx *ctx, gptps_log_level lvl, const char *msg)
{
    (void)ctx;
    if (lvl >= GPTPS_LOG_WARN && msg) fprintf(stderr, "[gptps] %s\n", msg);
}

static void ctx_clear_result(gptps_ctx *c)
{
    if (c->result_set) {
        if (c->result_is_copy)       free(c->result);          /* core-owned copy */
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
    copy = malloc(len);
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
        void *copy = malloc(ctx.result_len);
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

    p.handle = it->handle; p.name = it->def->name; p.attempt = it->attempt; p.mem = it->cost.mem_bytes;
    p.kind = GPTPS_EV_STARTED; p.status = GPTPS_OK; emit_now(e, cb, ud, &p);

    if (it->def->exec == GPTPS_EXEC_OOP) {
        /* enforced path: run in a forked child, OS-capped, hard-killed on timeout */
        void *res = NULL; size_t rlen = 0;
        st = gptps_oop_execute(it->def, it->payload, it->payload_len,
                               it->cost.mem_bytes, it->policy.timeout_seconds, &res, &rlen);
        free(res); /* M1: result-to-caller delivery is a later increment */
    } else {
        /* in-process path: cooperative cancel via the deadline flag */
        struct gptps_ctx ctx;
        bool timed;
        memset(&ctx, 0, sizeof ctx);
        ctx.engine = e; ctx.handle = it->handle; ctx.task_name = it->def->name;
        ctx.payload = it->payload; ctx.payload_len = it->payload_len;
        ctx.deadline_ms = it->deadline_ms; ctx.cancel = it->cancel;
        st = it->def->run(&ctx, it->def->user_data);
        timed = gptps_flag_get(it->cancel);
        if (timed) st = GPTPS_E_TIMEOUT;
        ctx_clear_result(&ctx);
    }

    p.kind = (st == GPTPS_OK) ? GPTPS_EV_FINISHED : GPTPS_EV_FAILED; p.status = st;
    emit_now(e, cb, ud, &p);
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
        cb = e->ev_cb; ud = e->ev_ud;      /* snapshot callback under the lock */
        /* deadline flag is the in-process cooperative path; OOP enforces its own
         * deadline in the worker (poll + hard-kill), so it gets no flag deadline */
        it->deadline_ms = (it->policy.timeout_seconds && it->def->exec == GPTPS_EXEC_INPROC)
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

static uint64_t min_nonzero(uint64_t a, uint64_t b)
{
    if (a == 0) return b;
    if (b == 0) return a;
    return a < b ? a : b;
}

static void *dispatcher_main(void *arg)
{
    gptps *e = (gptps *)arg;
    gptps_pending_ev pend[GPTPS_PENDING_CAP];
    int npend;

    gptps_mutex_lock(e->m);
    for (;;) {
        uint64_t now = gptps_hal_monotonic_ms();
        uint64_t next_wake = 0; /* 0 = none */
        gptps_item *it;
        int i;

        npend = 0;

        /* 1) drain completed: release budget, then retry / terminal decision */
        while ((it = fifo_pop(&e->done)) != NULL) {
            e->reserved_mem -= it->cost.mem_bytes;
            e->running      -= 1;

            if (it->outcome == GPTPS_OK) {
                item_free(it);
                continue;
            }
            if (it->attempt <= it->policy.max_retries) {
                /* schedule a retry after backoff */
                it->attempt += 1;
                it->not_before_ms = now + (uint64_t)it->policy.retry_backoff_seconds * 1000u;
                if (npend < GPTPS_PENDING_CAP) {
                    pend[npend].kind = GPTPS_EV_RETRIED; pend[npend].handle = it->handle;
                    pend[npend].name = it->def->name; pend[npend].status = it->outcome;
                    pend[npend].attempt = it->attempt; pend[npend].mem = it->cost.mem_bytes; ++npend;
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
                        item_free(it);
                        break;
                    case GPTPS_ON_FAILURE_DEAD_LETTER:
                    default:
                        if (npend < GPTPS_PENDING_CAP) {
                            pend[npend].kind = GPTPS_EV_DEAD_LETTERED; pend[npend].handle = it->handle;
                            pend[npend].name = it->def->name; pend[npend].status = it->outcome;
                            pend[npend].attempt = it->attempt; pend[npend].mem = it->cost.mem_bytes; ++npend;
                        }
                        fifo_push(&e->dead_letter, it);
                        e->dead_letter_count += 1;
                        break;
                }
            }
        }

        /* 2) emit retry/dead-letter events with the lock RELEASED */
        if (npend > 0) {
            gptps_event_cb cb = e->ev_cb; void *ud = e->ev_ud;
            gptps_mutex_unlock(e->m);
            for (i = 0; i < npend; ++i) emit_now(e, cb, ud, &pend[i]);
            gptps_mutex_lock(e->m);
            now = gptps_hal_monotonic_ms();
        }

        /* 3) move backoff-ready delayed items back to intake (single scan) */
        {
            gptps_item *cur = e->delayed.head, *prev = NULL;
            while (cur) {
                gptps_item *nxt = cur->next;
                if (cur->not_before_ms <= now) {
                    if (prev) prev->next = nxt; else e->delayed.head = nxt;
                    if (e->delayed.tail == cur) e->delayed.tail = prev;
                    cur->next = NULL;
                    fifo_push(&e->intake, cur);
                } else {
                    next_wake = min_nonzero(next_wake, cur->not_before_ms);
                    prev = cur;
                }
                cur = nxt;
            }
        }

        /* 4) enforce deadlines on running tasks (cooperative cancel) */
        for (it = e->running_items.head; it; it = it->next) {
            if (it->deadline_ms) {
                if (now >= it->deadline_ms) gptps_flag_set(it->cancel, true);
                else next_wake = min_nonzero(next_wake, it->deadline_ms);
            }
        }

        /* 5) admit while the head fits (strict FIFO) */
        while (e->intake.head &&
               e->running < e->limits.max_concurrent_tasks &&
               e->reserved_mem + e->intake.head->cost.mem_bytes <= e->limits.max_memory_bytes) {
            it = fifo_pop(&e->intake);
            e->reserved_mem += it->cost.mem_bytes;
            e->running      += 1;
            fifo_push(&e->ready, it);
            gptps_cond_signal(e->cv_work);
        }

        /* 6) shutdown when everything is drained */
        if (e->stopping && !e->intake.head && !e->ready.head && !e->done.head &&
            !e->delayed.head && !e->running_items.head && e->running == 0) {
            e->workers_exit = true;
            gptps_cond_broadcast(e->cv_work);
            break;
        }

        /* re-drain anything a worker pushed during the step-2 emit window: its
         * cv_disp signal can be lost (no waiter), so never sleep with `done`
         * non-empty or its budget/retry would be stranded (P1 lost-wakeup). */
        if (e->done.head) continue;

        /* 7) sleep until the next deadline/backoff, or until signalled */
        if (next_wake == 0) {
            gptps_cond_wait(e->cv_disp, e->m);
        } else if (next_wake > now) {
            gptps_cond_timedwait(e->cv_disp, e->m, next_wake - now);
        }
        /* else: a deadline/backoff is already due -> loop immediately */
    }
    gptps_mutex_unlock(e->m);
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* public API                                                                */
/* ------------------------------------------------------------------------- */

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
        default:                return "unknown error";
    }
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

    e = (gptps *)calloc(1, sizeof *e);
    if (!e) return GPTPS_E_NOMEM;

    s = gptps_config_resolve(in, &e->limits);
    if (s != GPTPS_OK) { free(e); return s; }

    e->next_handle = 1;
    e->m = gptps_mutex_create();
    e->cv_disp = gptps_cond_create();
    e->cv_work = gptps_cond_create();
    if (!e->m || !e->cv_disp || !e->cv_work) { s = GPTPS_E_NOMEM; goto fail; }

    e->nworkers = e->limits.max_concurrent_tasks;
    e->workers = (gptps_thread **)calloc(e->nworkers, sizeof *e->workers);
    if (!e->workers) { s = GPTPS_E_NOMEM; goto fail; }

    e->dispatcher = gptps_thread_start(dispatcher_main, e);
    if (!e->dispatcher) { s = GPTPS_E_NOMEM; goto fail; }
    for (i = 0; i < e->nworkers; ++i) {
        e->workers[i] = gptps_thread_start(worker_main, e);
        if (!e->workers[i]) { s = GPTPS_E_NOMEM; goto fail_threads; }
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
    if (e->workers) free(e->workers);
    if (e->cv_work) gptps_cond_destroy(e->cv_work);
    if (e->cv_disp) gptps_cond_destroy(e->cv_disp);
    if (e->m) gptps_mutex_destroy(e->m);
    free(e);
    return s;
}

gptps_status gptps_open(const char *config_path, gptps **out_engine)
{
    gptps_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.config_path = config_path; /* TOML parsing is a later increment; limits auto-tune */
    cfg.limits.struct_size = sizeof cfg.limits;
    return gptps_open_ex(&cfg, out_engine);
}

gptps_status gptps_register_task(gptps *e, const gptps_task_def *def)
{
    gptps_reg *r;
    char *name;

    if (!e || !def || !def->name || !def->run) return GPTPS_E_INVAL;
    if (def->struct_size < sizeof *def) return GPTPS_E_INVAL; /* ABI: reject undersized struct */

    gptps_mutex_lock(e->m);
    if (registry_find(e, def->name)) { gptps_mutex_unlock(e->m); return GPTPS_E_DUP; }

    r = (gptps_reg *)calloc(1, sizeof *r);
    name = (char *)malloc(strlen(def->name) + 1);
    if (!r || !name) { free(r); free(name); gptps_mutex_unlock(e->m); return GPTPS_E_NOMEM; }
    strcpy(name, def->name);

    r->def = *def;
    r->name = name;
    r->def.name = name;
    if (r->def.default_cost.struct_size == 0) r->def.default_cost.struct_size = sizeof(gptps_cost);
    r->next = e->registry;
    e->registry = r;
    gptps_mutex_unlock(e->m);
    return GPTPS_OK;
}

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
    gptps_payload
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

    node = (gptps_loaded *)calloc(1, sizeof *node);
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

gptps_status gptps_submit(gptps *e, const char *task_name,
                          const void *payload, size_t len, gptps_handle *out_handle)
{
    const gptps_reg *r;
    gptps_item *it;
    gptps_cost cost;
    void *pcopy = NULL;

    if (!e || !task_name) return GPTPS_E_INVAL;

    gptps_mutex_lock(e->m);
    if (e->stopping) { gptps_mutex_unlock(e->m); return GPTPS_E_SHUTDOWN; }

    r = registry_find(e, task_name);
    if (!r) { gptps_mutex_unlock(e->m); return GPTPS_E_NOTFOUND; }

    cost = r->def.default_cost;
    if (r->def.cost) {
        gptps_status cs = r->def.cost(payload, len, &cost, r->def.user_data);
        if (cs != GPTPS_OK) { gptps_mutex_unlock(e->m); return cs; }
    }
    if (cost.mem_bytes > e->limits.max_memory_bytes) {
        gptps_mutex_unlock(e->m);
        return GPTPS_E_BUDGET; /* never-fits: reject at submit */
    }

    if (len) {
        pcopy = malloc(len);
        if (!pcopy) { gptps_mutex_unlock(e->m); return GPTPS_E_NOMEM; }
        memcpy(pcopy, payload, len);
    }
    it = (gptps_item *)calloc(1, sizeof *it);
    if (it) it->cancel = gptps_flag_create(false);
    if (!it || !it->cancel) {
        if (it) gptps_flag_destroy(it->cancel);
        free(it); free(pcopy);
        gptps_mutex_unlock(e->m);
        return GPTPS_E_NOMEM;
    }

    it->handle = e->next_handle++;
    it->def = &r->def;
    it->payload = pcopy;
    it->payload_len = len;
    it->cost = cost;
    it->policy = r->def.default_policy;
    it->attempt = 1;

    fifo_push(&e->intake, it);
    if (out_handle) *out_handle = it->handle;
    emit(e, GPTPS_EV_QUEUED, it->handle, r->def.name, GPTPS_OK, 0, cost.mem_bytes);
    gptps_cond_signal(e->cv_disp);
    gptps_mutex_unlock(e->m);
    return GPTPS_OK;
}

gptps_status gptps_set_event_cb(gptps *e, gptps_event_cb cb, void *user_data)
{
    if (!e) return GPTPS_E_INVAL;
    gptps_mutex_lock(e->m);
    e->ev_cb = cb; e->ev_ud = user_data;
    gptps_mutex_unlock(e->m);
    return GPTPS_OK;
}

gptps_status gptps_shutdown(gptps *e)
{
    unsigned i;
    gptps_reg *r;
    gptps_item *it;

    if (!e) return GPTPS_E_INVAL;

    gptps_mutex_lock(e->m);
    e->stopping = true;
    gptps_cond_signal(e->cv_disp);
    gptps_mutex_unlock(e->m);

    gptps_thread_join(e->dispatcher);
    for (i = 0; i < e->nworkers; ++i) gptps_thread_join(e->workers[i]);

    /* tear down add-ons (threads joined => no task code runs; last calls into
     * each .so, then unload) */
    {
        gptps_loaded *a = e->addons;
        while (a) {
            gptps_loaded *n = a->next;
            if (a->addon->teardown) a->addon->teardown(e);
            gptps_dl_close(a->dl);
            free(a);
            a = n;
        }
    }

    /* free anything left (dead-letter retained items, etc.) */
    while ((it = fifo_pop(&e->dead_letter)) != NULL) item_free(it);
    while ((it = fifo_pop(&e->intake)) != NULL) item_free(it);
    while ((it = fifo_pop(&e->delayed)) != NULL) item_free(it);

    r = e->registry;
    while (r) { gptps_reg *n = r->next; free(r->name); free(r); r = n; }

    free(e->workers);
    gptps_cond_destroy(e->cv_work);
    gptps_cond_destroy(e->cv_disp);
    gptps_mutex_destroy(e->m);
    free(e);
    return GPTPS_OK;
}
