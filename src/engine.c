/*
 * engine.c - GPTPS engine: lifecycle, registry, queue, single-writer
 * dispatcher + worker pool, in-process executor, ctx accessors (T2 + T4 + the
 * in-process half of the executor seam).
 *
 * CONCURRENCY MODEL (honors the single-writer-ledger decision)
 * ------------------------------------------------------------
 * One mutex `m` guards all shared state. The DISPATCHER thread is the ONLY
 * writer of the admission ledger (reserved_mem, running). Workers never touch
 * the ledger; they post finished items to `done` and let the dispatcher
 * release budget. This removes the admit/release race by construction.
 *
 *   submit ─► [intake FIFO] ─► dispatcher: admit iff (running<conc &&
 *                                          reserved+cost.mem <= max_mem)
 *                                          ├─ reserve, push ─► [ready FIFO]
 *                                          └─ else wait (re-check on completion)
 *   worker: pop [ready] ─► run task ─► emit events ─► push [done] ─► signal
 *   dispatcher: drain [done] ─► release budget ─► admit more
 *
 * v1 milestone admission is strict-FIFO-head (admit head when it fits; never
 * starves because submit rejects any task whose cost can't fit max_mem, so the
 * head always fits once running tasks drain). skip-to-fit + reservation/drain
 * and the deadline watchdog are the next refinements (T4 cont. / T5).
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
    uint64_t      deadline_ms;     /* 0 = none (watchdog lands in T5) */
    gptps_flag   *cancel;
    /* result, delivered via the accessors below */
    void        *result;
    size_t       result_len;
    void       (*result_free)(void *);
    bool         result_owned;     /* true => we must free `result` */
    bool         result_set;
};

typedef struct gptps_item {
    gptps_handle        handle;
    const gptps_task_def *def;     /* points into the registry (stable) */
    void               *payload;   /* owned copy */
    size_t              payload_len;
    gptps_cost          cost;      /* resolved declared cost */
    struct gptps_item  *next;
} gptps_item;

typedef struct gptps_reg {
    gptps_task_def     def;        /* copied; def.name points at `name` below */
    char              *name;       /* owned */
    struct gptps_reg  *next;
} gptps_reg;

typedef struct { gptps_item *head, *tail; } gptps_fifo;

struct gptps {
    gptps_limits   limits;
    gptps_reg     *registry;

    gptps_mutex   *m;
    gptps_cond    *cv_disp;        /* dispatcher waits here */
    gptps_cond    *cv_work;        /* workers wait here */

    gptps_fifo     intake;         /* submitted, not yet admitted */
    gptps_fifo     ready;          /* admitted, awaiting a worker */
    gptps_fifo     done;           /* finished, awaiting budget release */

    /* admission ledger - DISPATCHER-ONLY writes */
    uint64_t       reserved_mem;
    uint32_t       running;

    gptps_thread  *dispatcher;
    gptps_thread **workers;
    unsigned       nworkers;

    bool           stopping;       /* shutdown requested */
    bool           workers_exit;   /* set by dispatcher once all work is drained */

    gptps_handle   next_handle;
    gptps_event_cb ev_cb;
    void          *ev_ud;
};

/* ------------------------------------------------------------------------- */
/* small helpers                                                             */
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
    free(it->payload);
    free(it);
}

static void emit(gptps *e, gptps_event_kind kind, gptps_handle h,
                 const char *name, gptps_status st, uint32_t attempt, uint64_t mem)
{
    gptps_event ev;
    gptps_event_cb cb = e->ev_cb;   /* benign read; set before tasks run */
    void *ud = e->ev_ud;
    if (!cb) return;
    memset(&ev, 0, sizeof ev);
    ev.struct_size = sizeof ev;
    ev.kind = kind; ev.handle = h; ev.task_name = name;
    ev.ts_ms = gptps_hal_monotonic_ms(); ev.status = st; ev.attempt = attempt;
    ev.mem_bytes = mem;
    cb(&ev, ud);
}

/* ------------------------------------------------------------------------- */
/* ctx accessors (public surface a task body touches)                        */
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
        if (c->result_owned) {
            if (c->result_free) c->result_free(c->result); else free(c->result);
        }
        c->result = NULL; c->result_len = 0; c->result_free = NULL;
        c->result_owned = false; c->result_set = false;
    }
}

gptps_status gptps_result_set(gptps_ctx *ctx, const void *bytes, size_t len)
{
    void *copy;
    if (!ctx) return GPTPS_E_INVAL;
    ctx_clear_result(ctx);
    if (len == 0) { ctx->result = NULL; ctx->result_len = 0; ctx->result_owned = false; ctx->result_set = true; return GPTPS_OK; }
    copy = malloc(len);
    if (!copy) return GPTPS_E_NOMEM;
    memcpy(copy, bytes, len);
    ctx->result = copy; ctx->result_len = len; ctx->result_free = NULL;
    ctx->result_owned = true; ctx->result_set = true;
    return GPTPS_OK;
}

gptps_status gptps_result_set_nocopy(gptps_ctx *ctx, void *bytes, size_t len,
                                     void (*free_cb)(void *))
{
    if (!ctx) return GPTPS_E_INVAL;
    ctx_clear_result(ctx);
    ctx->result = bytes; ctx->result_len = len; ctx->result_free = free_cb;
    ctx->result_owned = true; ctx->result_set = true;
    return GPTPS_OK;
}

/* ------------------------------------------------------------------------- */
/* worker + dispatcher threads                                               */
/* ------------------------------------------------------------------------- */

static void run_one(gptps *e, gptps_item *it)
{
    struct gptps_ctx ctx;
    gptps_status st;

    memset(&ctx, 0, sizeof ctx);
    ctx.engine = e;
    ctx.handle = it->handle;
    ctx.task_name = it->def->name;
    ctx.payload = it->payload;
    ctx.payload_len = it->payload_len;
    ctx.deadline_ms = 0; /* watchdog wires this in T5 */
    ctx.cancel = gptps_flag_create(false);

    emit(e, GPTPS_EV_STARTED, it->handle, it->def->name, GPTPS_OK, 1, it->cost.mem_bytes);
    st = it->def->run(&ctx, it->def->user_data);
    emit(e, (st == GPTPS_OK) ? GPTPS_EV_FINISHED : GPTPS_EV_FAILED,
         it->handle, it->def->name, st, 1, it->cost.mem_bytes);

    ctx_clear_result(&ctx);
    gptps_flag_destroy(ctx.cancel);
}

static void *worker_main(void *arg)
{
    gptps *e = (gptps *)arg;
    gptps_mutex_lock(e->m);
    for (;;) {
        gptps_item *it;
        while (!e->ready.head && !e->workers_exit)
            gptps_cond_wait(e->cv_work, e->m);
        if (!e->ready.head && e->workers_exit) break;
        it = fifo_pop(&e->ready);
        gptps_mutex_unlock(e->m);

        run_one(e, it);                 /* no lock held while task runs */

        gptps_mutex_lock(e->m);
        fifo_push(&e->done, it);
        gptps_cond_signal(e->cv_disp);
    }
    gptps_mutex_unlock(e->m);
    return NULL;
}

static void *dispatcher_main(void *arg)
{
    gptps *e = (gptps *)arg;
    gptps_mutex_lock(e->m);
    for (;;) {
        gptps_item *it;

        /* 1) release budget for everything that finished */
        while ((it = fifo_pop(&e->done)) != NULL) {
            e->reserved_mem -= it->cost.mem_bytes;
            e->running      -= 1;
            item_free(it);
        }

        /* 2) admit while the head fits (strict FIFO) */
        while (e->intake.head &&
               e->running < e->limits.max_concurrent_tasks &&
               e->reserved_mem + e->intake.head->cost.mem_bytes <= e->limits.max_memory_bytes) {
            it = fifo_pop(&e->intake);
            e->reserved_mem += it->cost.mem_bytes;
            e->running      += 1;
            fifo_push(&e->ready, it);
            gptps_cond_signal(e->cv_work);
        }

        /* 3) done? everything drained and shutdown requested */
        if (e->stopping && !e->intake.head && !e->ready.head &&
            !e->done.head && e->running == 0) {
            e->workers_exit = true;
            gptps_cond_broadcast(e->cv_work);
            break;
        }

        /* 4) nothing to do right now: sleep until submit / completion / stop */
        gptps_cond_wait(e->cv_disp, e->m);
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
        case GPTPS_OK:         return "ok";
        case GPTPS_E_NOMEM:    return "out of memory";
        case GPTPS_E_INVAL:    return "invalid argument";
        case GPTPS_E_NOTFOUND: return "task not found";
        case GPTPS_E_DUP:      return "task already registered";
        case GPTPS_E_BUDGET:   return "declared cost cannot fit the budget";
        case GPTPS_E_FULL:     return "queue full";
        case GPTPS_E_TIMEOUT:  return "task timed out";
        case GPTPS_E_CANCELLED:return "task cancelled";
        case GPTPS_E_ABI:      return "add-on ABI mismatch";
        case GPTPS_E_CONFIG:   return "config error";
        case GPTPS_E_IO:       return "I/O error";
        case GPTPS_E_TASK:     return "task error";
        case GPTPS_E_SHUTDOWN: return "engine shutting down";
        default:               return "unknown error";
    }
}

gptps_status gptps_open_ex(const gptps_config *cfg, gptps **out_engine)
{
    gptps *e;
    gptps_status s;
    const gptps_limits *in = cfg ? &cfg->limits : NULL;
    unsigned i;

    if (!out_engine) return GPTPS_E_INVAL;
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
    /* tear down already-started threads cleanly */
    gptps_mutex_lock(e->m);
    e->stopping = true;
    gptps_cond_broadcast(e->cv_disp);
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
    /* NOTE: TOML parsing is a later increment; for now config_path is recorded
     * but limits are auto-tuned. gptps_open(NULL) == auto. */
    gptps_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.config_path = config_path;
    cfg.limits.struct_size = sizeof cfg.limits;
    return gptps_open_ex(&cfg, out_engine);
}

gptps_status gptps_register_task(gptps *e, const gptps_task_def *def)
{
    gptps_reg *r;
    char *name;

    if (!e || !def || !def->name || !def->run) return GPTPS_E_INVAL;

    gptps_mutex_lock(e->m);
    if (registry_find(e, def->name)) { gptps_mutex_unlock(e->m); return GPTPS_E_DUP; }

    r = (gptps_reg *)calloc(1, sizeof *r);
    name = (char *)malloc(strlen(def->name) + 1);
    if (!r || !name) { free(r); free(name); gptps_mutex_unlock(e->m); return GPTPS_E_NOMEM; }
    strcpy(name, def->name);

    r->def = *def;          /* struct copy */
    r->name = name;
    r->def.name = name;     /* point the copy at our owned string */
    if (r->def.default_cost.struct_size == 0) r->def.default_cost.struct_size = sizeof(gptps_cost);
    r->next = e->registry;
    e->registry = r;
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

    /* resolve declared cost (cost fn runs under the lock for this milestone;
     * moving it off the lock is a documented follow-up). */
    cost = r->def.default_cost;
    if (r->def.cost) {
        gptps_status cs = r->def.cost(payload, len, &cost, r->def.user_data);
        if (cs != GPTPS_OK) { gptps_mutex_unlock(e->m); return cs; }
    }

    /* never-fits rejection: a task that can't fit max budget would starve. */
    if (cost.mem_bytes > e->limits.max_memory_bytes) {
        gptps_mutex_unlock(e->m);
        return GPTPS_E_BUDGET;
    }

    if (len) {
        pcopy = malloc(len);
        if (!pcopy) { gptps_mutex_unlock(e->m); return GPTPS_E_NOMEM; }
        memcpy(pcopy, payload, len);
    }
    it = (gptps_item *)calloc(1, sizeof *it);
    if (!it) { free(pcopy); gptps_mutex_unlock(e->m); return GPTPS_E_NOMEM; }

    it->handle = e->next_handle++;
    it->def = &r->def;
    it->payload = pcopy;
    it->payload_len = len;
    it->cost = cost;

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

    if (!e) return GPTPS_E_INVAL;

    gptps_mutex_lock(e->m);
    e->stopping = true;
    gptps_cond_signal(e->cv_disp);
    gptps_mutex_unlock(e->m);

    gptps_thread_join(e->dispatcher);          /* drains, then sets workers_exit */
    for (i = 0; i < e->nworkers; ++i) gptps_thread_join(e->workers[i]);

    /* free registry */
    r = e->registry;
    while (r) { gptps_reg *n = r->next; free(r->name); free(r); r = n; }

    free(e->workers);
    gptps_cond_destroy(e->cv_work);
    gptps_cond_destroy(e->cv_disp);
    gptps_mutex_destroy(e->m);
    free(e);
    return GPTPS_OK;
}
