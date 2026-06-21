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
    int32_t               priority;  /* higher = admitted first (default 0) */
    uint32_t              skips;     /* times a backfill admission jumped ahead while budget-blocked */
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
    char             **argv_copy;  /* owned NULL-terminated copy for EXEC_PROGRAM */
    int32_t            priority;   /* scheduling priority for this task type (default 0) */
    struct gptps      *engine;     /* back-pointer (settings write_fns lock engine->m) */
    struct gptps_reg  *next;
} gptps_reg;

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

typedef struct { gptps_item *head, *tail; } gptps_fifo;

/* pending event emitted after the lock is released */
typedef struct {
    gptps_event_kind kind;
    gptps_handle     handle;
    const char      *name;
    gptps_status     status;
    uint32_t         attempt;
    uint64_t         mem;
    const void      *result;
    size_t           result_len;
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
    bool           manual;         /* MANUAL mode: no threads; driven by gptps_step() */

    gptps_handle   next_handle;
    gptps_event_cb ev_cb;
    void          *ev_ud;

    gptps_loaded  *addons;        /* dlopen'd add-ons, torn down at shutdown */
    gptps_observer *observers;    /* extra event sinks (registered before submit) */
    gptps_constraint *constraints;/* admission hooks consulted by the dispatcher */

    gptps_toml    *toml;          /* parsed config file (NULL if opened without one) */
    uint32_t       reserve_after_skips; /* starvation guard: reserve a budget-blocked top task after this many backfill skips */
    gptps_settings *settings;     /* unified settings registry */
    char          *config_path;   /* the path opened with (NULL if none); default for save/reload */
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

/* worker-side emit (already lock-free) */
static void emit(gptps *e, gptps_event_kind kind, gptps_handle h,
                 const char *name, gptps_status st, uint32_t attempt, uint64_t mem)
{
    gptps_pending_ev p;
    p.kind = kind; p.handle = h; p.name = name; p.status = st; p.attempt = attempt; p.mem = mem;
    p.result = NULL; p.result_len = 0;
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
    struct gptps_ctx ctx;          /* used only on the in-process path */
    void *oop_res = NULL;
    size_t oop_len = 0;
    bool inproc = (it->def->exec == GPTPS_EXEC_INPROC);

    p.handle = it->handle; p.name = it->def->name; p.attempt = it->attempt; p.mem = it->cost.mem_bytes;
    p.result = NULL; p.result_len = 0;
    p.kind = GPTPS_EV_STARTED; p.status = GPTPS_OK; emit_now(e, cb, ud, &p);

    if (inproc) {
        /* in-process path: cooperative cancel via the deadline flag */
        memset(&ctx, 0, sizeof ctx);
        ctx.engine = e; ctx.handle = it->handle; ctx.task_name = it->def->name;
        ctx.payload = it->payload; ctx.payload_len = it->payload_len;
        ctx.deadline_ms = it->deadline_ms; ctx.cancel = it->cancel;
        st = it->def->run(&ctx, it->def->user_data);
        if (gptps_flag_get(it->cancel)) st = GPTPS_E_TIMEOUT;
    } else if (it->def->exec == GPTPS_EXEC_OOP) {
        /* enforced path: run the in-process fn in a forked child, OS-capped, hard-killed */
        st = gptps_oop_execute(it->def, it->payload, it->payload_len,
                               it->cost.mem_bytes, it->policy.timeout_seconds, &oop_res, &oop_len);
    } else {
        /* enforced path: fork+exec an external program; payload->stdin, stdout->result */
        st = gptps_program_execute(it->def->argv, it->payload, it->payload_len,
                                   it->cost.mem_bytes, it->policy.timeout_seconds, &oop_res, &oop_len);
    }

    /* deliver the result on the FINISHED event (valid for the callback's duration) */
    p.kind = (st == GPTPS_OK) ? GPTPS_EV_FINISHED : GPTPS_EV_FAILED; p.status = st;
    if (st == GPTPS_OK) {
        if (inproc) { if (ctx.result_set) { p.result = ctx.result; p.result_len = ctx.result_len; } }
        else        { p.result = oop_res; p.result_len = oop_len; }
    }
    emit_now(e, cb, ud, &p);

    if (inproc) ctx_clear_result(&ctx);
    else        free(oop_res);
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
    *retry_after = 0;
    for (c = e->constraints; c; c = c->next) {
        uint32_t ra = 0;
        gptps_admit_decision d = c->fn(it->def->name, &it->cost, &ra, c->ud);
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
                        item_free(it);
                        break;
                    case GPTPS_ON_FAILURE_DEAD_LETTER:
                    default:
                        if (npend < GPTPS_PENDING_CAP) {
                            pend[npend].kind = GPTPS_EV_DEAD_LETTERED; pend[npend].handle = it->handle;
                            pend[npend].name = it->def->name; pend[npend].status = it->outcome;
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

        /* 4) admit in priority order with skip-to-fit backfill + starvation guard.
         *    Each iteration scans intake for `top` (highest-priority item overall)
         *    and `best` (highest-priority item that fits the live budget; ties
         *    resolve to the older item, preserving FIFO within a priority). When
         *    `best != top` we are about to skip the higher-priority `top` because
         *    it does not fit yet: allowed (backfill) until `top` has been skipped
         *    reserve_after_skips times, after which we reserve for it - admit
         *    nothing and let running tasks drain until it fits (bounded). */
        while (e->intake.head && e->running < e->limits.max_concurrent_tasks) {
            gptps_item *best = NULL, *top = NULL, *cur;
            uint32_t retry_after = 0;
            gptps_admit_decision dec;

            for (cur = e->intake.head; cur; cur = cur->next) {
                if (!top || cur->priority > top->priority) top = cur;
                if (e->reserved_mem + cur->cost.mem_bytes <= e->limits.max_memory_bytes &&
                    (!best || cur->priority > best->priority))
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
                    pend[npend].name = best->def->name; pend[npend].status = GPTPS_E_DENIED;
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
    e->reserve_after_skips = GPTPS_RESERVE_AFTER;
    e->m = gptps_mutex_create();
    e->cv_disp = gptps_cond_create();
    e->cv_work = gptps_cond_create();
    e->settings = gptps_settings_create();
    if (!e->m || !e->cv_disp || !e->cv_work || !e->settings) { s = GPTPS_E_NOMEM; goto fail; }

    /* core settings (read live engine state; hot ones apply immediately) */
    reg_core_setting(e, "limits.max_memory_bytes", GPTPS_SETTING_UINT, 1, 0, 0, 0,
                     "admission memory budget in bytes", sc_rd_maxmem, sc_wr_maxmem);
    reg_core_setting(e, "limits.max_concurrent_tasks", GPTPS_SETTING_UINT, 0, 1, 1, 65536,
                     "worker pool size (restart to apply)", sc_rd_conc, sc_wr_conc);
    reg_core_setting(e, "scheduler.reserve_after_skips", GPTPS_SETTING_UINT, 1, 0, 0, 0,
                     "scheduler starvation guard (backfill skips before reserving)", sc_rd_resv, sc_wr_resv);

    if (cfg && cfg->config_path) {   /* remember the open path for save/reload defaults */
        size_t L = strlen(cfg->config_path) + 1;
        e->config_path = (char *)malloc(L);
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
        e->workers = (gptps_thread **)calloc(e->nworkers, sizeof *e->workers);
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
    if (e->workers) free(e->workers);
    if (e->cv_work) gptps_cond_destroy(e->cv_work);
    if (e->cv_disp) gptps_cond_destroy(e->cv_disp);
    if (e->m) gptps_mutex_destroy(e->m);
    free(e);
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
    out = (char **)calloc(n + 1, sizeof *out);
    if (!out) return NULL;
    for (i = 0; i < n; ++i) {
        size_t L = strlen(argv[i]) + 1;
        out[i] = (char *)malloc(L);
        if (!out[i]) { while (i--) free(out[i]); free(out); return NULL; }
        memcpy(out[i], argv[i], L);
    }
    return out;
}

gptps_status gptps_register_task(gptps *e, const gptps_task_def *def)
{
    gptps_reg *r;
    char *name;
    char **argv_copy = NULL;

    if (!e || !def || !def->name) return GPTPS_E_INVAL;
    if (def->struct_size < sizeof *def) return GPTPS_E_INVAL; /* ABI: reject undersized struct */
    if (def->exec == GPTPS_EXEC_PROGRAM) {
        if (!def->argv || !def->argv[0]) return GPTPS_E_INVAL; /* program needs an argv */
    } else if (!def->run) {
        return GPTPS_E_INVAL;                                 /* in-process kinds need a run fn */
    }

    if (def->exec == GPTPS_EXEC_PROGRAM) {
        argv_copy = argv_dup(def->argv);
        if (!argv_copy) return GPTPS_E_NOMEM;
    }

    gptps_mutex_lock(e->m);
    if (registry_find(e, def->name)) {
        gptps_mutex_unlock(e->m);
        if (argv_copy) { char **a = argv_copy; while (*a) free(*a++); free(argv_copy); }
        return GPTPS_E_DUP;
    }

    r = (gptps_reg *)calloc(1, sizeof *r);
    name = (char *)malloc(strlen(def->name) + 1);
    if (!r || !name) {
        free(r); free(name);
        if (argv_copy) { char **a = argv_copy; while (*a) free(*a++); free(argv_copy); }
        gptps_mutex_unlock(e->m);
        return GPTPS_E_NOMEM;
    }
    strcpy(name, def->name);

    r->def = *def;
    r->name = name;
    r->def.name = name;
    r->argv_copy = argv_copy;
    r->def.argv = (const char *const *)argv_copy; /* point at our owned copy (NULL for non-PROGRAM) */
    if (r->def.default_cost.struct_size == 0) r->def.default_cost.struct_size = sizeof(gptps_cost);
    r->priority = 0;
    r->engine = e;
    apply_task_config(e->toml, name, &r->def, &r->priority); /* config file overrides compiled-in defaults */
    r->next = e->registry;
    e->registry = r;
    gptps_mutex_unlock(e->m);

    /* expose this task's knobs in the settings registry (after releasing e->m:
     * registry add takes settings->m then e->m, preserving the lock order) */
    register_task_settings(e, r);
    return GPTPS_OK;
}

gptps_status gptps_set_task_priority(gptps *e, const char *task_name, int priority)
{
    gptps_reg *r;
    if (!e || !task_name) return GPTPS_E_INVAL;
    gptps_mutex_lock(e->m);
    for (r = e->registry; r; r = r->next)
        if (strcmp(r->name, task_name) == 0) break;
    if (r) r->priority = (int32_t)priority;   /* applies to subsequently-submitted items */
    gptps_mutex_unlock(e->m);
    return r ? GPTPS_OK : GPTPS_E_NOTFOUND;
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
    gptps_register_setting
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
    it->priority = r->priority;
    it->skips = 0;
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

gptps_status gptps_register_observer(gptps *e, gptps_event_cb fn, void *user_data)
{
    gptps_observer *o;
    if (!e || !fn) return GPTPS_E_INVAL;
    o = (gptps_observer *)calloc(1, sizeof *o);
    if (!o) return GPTPS_E_NOMEM;
    o->fn = fn; o->ud = user_data;
    gptps_mutex_lock(e->m);
    o->next = e->observers; e->observers = o;
    gptps_mutex_unlock(e->m);
    return GPTPS_OK;
}

gptps_status gptps_register_constraint(gptps *e, gptps_constraint_fn fn, void *user_data)
{
    gptps_constraint *c;
    if (!e || !fn) return GPTPS_E_INVAL;
    c = (gptps_constraint *)calloc(1, sizeof *c);
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
            dl.task_name = it->def->name;   /* registry-owned; stable until shutdown */
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
            free(a);
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
        if (r->argv_copy) { char **a = r->argv_copy; while (*a) free(*a++); free(r->argv_copy); }
        free(r->name); free(r); r = n;
    }
    { gptps_observer  *o = e->observers;  while (o) { gptps_observer  *n = o->next; free(o); o = n; } }
    { gptps_constraint *c = e->constraints; while (c) { gptps_constraint *n = c->next; free(c); c = n; } }
    gptps_settings_destroy(e->settings);  /* entries reference e / regs, which are freed above/after; destroy only frees the schema list */
    gptps_toml_free(e->toml);
    free(e->config_path);

    free(e->workers);
    gptps_cond_destroy(e->cv_work);
    gptps_cond_destroy(e->cv_disp);
    gptps_mutex_destroy(e->m);
    free(e);
    return GPTPS_OK;
}
