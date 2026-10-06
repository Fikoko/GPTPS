/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
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
#include <errno.h>
#include <float.h>
#include <stdarg.h>

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
    uint32_t         *cancel;        /* the item's cancel word, shared with the ctx */
    void             *result;
    size_t            result_len;
    void            (*result_free)(void *);
    bool              result_is_copy;    /* true => core allocated a copy; free it */
    bool              result_set;
    bool              bounded;           /* bounded engine: _set copies into result_buf */
    unsigned char    *result_buf;        /* bounded: this executing thread's buffer */
    size_t            result_cap;        /* bounded: its size, max_result_bytes */
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
    uint64_t              intake_seq;    /* order it last ENTERED intake: breaks score ties oldest-first */
    uint64_t              deadline_ms;   /* 0 = no timeout */
    uint64_t              not_before_ms; /* backoff gate for delayed retries */
    uint32_t              cancel;    /* 1 once a cancel, deadline or removal asks it to stop. Kept in
                                      * the item, so a submit allocates no flag: written with release,
                                      * read with acquire (cancel_raise / cancel_raised) */
    gptps_status          outcome;   /* effective status of the last attempt */
    int                   cancelled; /* gptps_cancel(handle) requested: never retry/dead-letter */
    int                   started;   /* 1 once execute() ran this item's most recent attempt, so
                                      * a STARTED and a FINISHED/FAILED were emitted for it. While
                                      * the item is parked in `delayed` that is the PREVIOUS
                                      * attempt (cleared when step 2 promotes it). It says an
                                      * attempt ran, not how it ended: whether the handle is
                                      * already closed is terminal_reported(), which also reads
                                      * `outcome` (never-ran vs cancelled-while-running is this
                                      * flag's half of that question). */
    uint32_t              timeout_ms_override; /* per-submit sub-second deadline (0 = use policy.timeout_seconds) */
    uint64_t             *res_reserved; /* named-resource amounts reserved at admit (length res_n); freed+NULLed at release (done-drain), else at item_free */
    size_t                res_n;
    struct gptps_item    *next;
    struct gptps_item    *prev;      /* every queue is doubly linked: see "finding an item" */
    unsigned char         where;     /* GPTPS_Q_*: the engine queue holding it, NONE in a local list */
    unsigned char         indexed;   /* in the handle index: gptps_cancel can still reach it */
    unsigned char         pooled;    /* a bounded engine's: freeing returns it to the pool */
} gptps_item;

/* One instance value of a generic per-task setting (see gptps_define_task_setting).
 * Materialized per (task, schema); the settings entry's target points here. */
typedef struct gptps_task_local {
    const struct gptps_task_schema *schema;       /* borrowed: shared schema (leaf/type/range) */
    struct gptps_reg               *reg;          /* owning task (locks reg->engine->m) */
    char                            value[GPTPS_SETTINGS_VALUE_MAX];
    gptps_setting_prep             *prep;         /* its setting, made and not yet published */
    struct gptps_task_local        *next;
} gptps_task_local;

typedef struct gptps_reg {
    gptps_task_def     def;
    char              *name;
    char             **argv_copy;  /* owned NULL-terminated copy for EXEC_PROGRAM */
    int32_t            priority;   /* scheduling priority for this task type (default 0) */
    bool               enabled;    /* false => reject new submits (paused, reversible) */
    bool               settling;   /* true => gptps_register_task is still adding its settings: hidden from other threads */
    bool               published;  /* its settings are in the registry: until then (settling), a definition leaves its setting to the registration */
    bool               removed;    /* true => tombstoned, draining toward removal */
    bool               cancelling; /* true => removal is CANCEL: drop in-flight items rather than dead-letter */
    bool               service;    /* true => GPTPS_TASK_SERVICE: supervised long-running instances (restart-on-exit) */
    bool               retire_on_ok;/* service only: a clean GPTPS_OK return retires the instance instead of restarting it */
    const void        *load_tag;   /* the add-on load whose setup registered it (e->setup_tag), else NULL */
    uint64_t           settling_tid;/* while settling: the thread registering it, which does see it */
    gptps_task_local  *locals;     /* owned generic per-task setting cells */
    uint64_t          *res_cost;   /* per-item cost per named resource (length engine->nres; NULL if nres==0) */
    struct gptps      *engine;     /* back-pointer (settings write_fns lock engine->m) */
    struct gptps_res_cell *res_cells; /* owned: its tasks.<name>.resources.<r> settings' cells */
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
    char                *path;    /* owned copy, for gptps_addon_get_info */
    int                  enabled; /* 0 once gptps_addon_disable succeeded */
    struct gptps_loaded *next;
} gptps_loaded;

typedef struct gptps_observer {
    gptps_event_cb         fn;
    void                  *ud;
    const void            *load_tag;   /* the add-on load whose setup registered it, else NULL */
    struct gptps_observer *next;
} gptps_observer;

typedef struct gptps_constraint {
    gptps_constraint_fn      fn;
    void                    *ud;
    const void              *load_tag; /* the add-on load whose setup registered it, else NULL */
    struct gptps_constraint *next;
} gptps_constraint;

/* `id` is the GPTPS_Q_* value an item takes on while it is in this queue; a local
 * list's is GPTPS_Q_NONE. */
typedef struct { gptps_item *head, *tail; size_t count; unsigned char id; } gptps_fifo;
#define GPTPS_Q_NONE    0
#define GPTPS_Q_INTAKE  1
#define GPTPS_Q_DELAYED 2
#define GPTPS_Q_READY   3
#define GPTPS_Q_RUNNING 4
#define GPTPS_Q_DONE    5
#define GPTPS_Q_DEAD    6
#define GPTPS_Q_POOL    7      /* bounded: back in the pool, free */

/* What a worker thread starts with: its engine, and its index - which picks its
 * result buffer on a bounded engine. */
typedef struct gptps_worker { struct gptps *e; unsigned idx; } gptps_worker;

/* One slot of the handle index: h == 0 empty. Deletion leaves no tombstone (see hidx_del). */
typedef struct { gptps_handle h; gptps_item *it; } gptps_hslot;

/* One per thread id that is, or has been, inside a callback the engine made on it
 * (see gptps.cb_threads). The chains and `tid` are under e->m. `depth` is written
 * with no lock by the thread `tid` names, on leaving; the record passes to another
 * thread only when that one has the same id (the OS reused it) or takes the record
 * over idle (depth 0, below), so depth is loaded acquire and stored release. */
typedef struct gptps_cb_thread {
    uint64_t                tid;
    struct gptps_cb_thread *next;
    char                    pad_[64];   /* keep depth, which its thread writes with no  */
    uint32_t                depth;      /* lock, off the line others read under e->m    */
    char                    pad2_[60];
} gptps_cb_thread;

/* Records are found by hashing the thread id, so entering touches one short chain
 * rather than every record - it is walked with e->m held, on every watched
 * gptps_submit. A new id takes over an idle record on its own chain before a new
 * one is allocated, and at most GPTPS_CB_THREADS_MAX are; after that, an idle
 * record on any chain. Only past that many threads inside such callbacks at once
 * (or escaped from one), or with no memory for a record, does a callback run
 * unguarded. */
#define GPTPS_CB_BUCKETS 64
#ifndef GPTPS_CB_THREADS_MAX            /* overridable only so a test build can reach it */
#define GPTPS_CB_THREADS_MAX 1024
#endif
static size_t cb_bucket(uint64_t tid)
{
    tid ^= tid >> 33; tid *= 0xff51afd7ed558ccdull; tid ^= tid >> 33;
    return (size_t)(tid & (GPTPS_CB_BUCKETS - 1));
}

/* Equal-score runs cached to keep an ordered intake insert O(1); see "intake
 * ordering". Advisory, so this bounds memory, never correctness. */
#define GPTPS_INTAKE_RUNS 32

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
    uint32_t         flags;         /* GPTPS_EV_FLAG_* */
} gptps_pending_ev;

static void ev_set_name(char *dst, const char *src)
{ snprintf(dst, GPTPS_EV_NAME_MAX, "%s", src ? src : "?"); }

/* A generic named admission resource: a total budget and the amount currently
 * reserved by in-flight items (DISPATCHER-only, like reserved_mem). */
typedef struct {
    char    *name;       /* owned */
    uint64_t budget;
    uint64_t reserved;   /* DISPATCHER-ONLY */
    int      addon;      /* defined inside an add-on's setup: the add-on exposes it, not the registry */
} gptps_resource;

/* The cell a resource setting's accessors get (see "resources as settings"): the
 * engine, the resource's index - stable: a resource is published with all its
 * settings made, and never removed before shutdown - and, for a cost, the task. */
typedef struct gptps_res_cell {
    struct gptps          *e;
    struct gptps_reg      *r;       /* NULL: the budget */
    size_t                 ri;
    gptps_setting_prep    *prep;    /* its setting, made and not yet published */
    struct gptps_res_cell *next;
} gptps_res_cell;

struct gptps {
    gptps_limits   limits;
    gptps_reg     *registry;

    gptps_mutex   *m;
    gptps_cond    *cv_disp;
    gptps_cond    *cv_work;
    gptps_cond    *cv_drain;        /* signalled after each pass; a blocked unregister re-checks drain */

    /* intake is held in ADMISSION order, not submission order, and intake_runs
     * indexes it - see "intake ordering" below for both invariants. */
    gptps_fifo     intake;
    struct { int64_t score; gptps_item *tail; } intake_runs[GPTPS_INTAKE_RUNS];
    size_t         n_intake_runs;
    gptps_fifo     ready;
    gptps_fifo     done;
    gptps_fifo     delayed;        /* retries waiting for backoff */
    gptps_fifo     running_items;  /* in-flight, scanned for deadlines */
    gptps_fifo     dead_letter;    /* terminal failures retained */
    uint32_t       dead_letter_count;
    /* Cap on the retained list. It is the ONLY queue the host is not required to
     * drain, so an undrained one grew without bound - each entry pinning its
     * original payload - which is a memory leak with extra steps in an engine whose
     * entire contract is bounded admission (and DEAD_LETTER is the default policy).
     * Past the cap the OLDEST entry is evicted; `dead_evicted` counts how many, so
     * the truncation is never silent (settings key stats.dead_letters_evicted).
     * 0 => unbounded (the pre-1.0 behaviour, now opt-in). */
    uint32_t       max_dead_letters;
    uint64_t       dead_evicted;

    uint64_t       reserved_mem;   /* DISPATCHER-ONLY */
    uint32_t       running;        /* DISPATCHER-ONLY */

    gptps_resource *resources;     /* generic named admission budgets (gptps_define_resource) */
    size_t          nres, rescap;
    gptps_res_cell *res_cells;     /* owned: the resources.<name> settings' cells */
    uint64_t        bnext_items;   /* bounded.* as written live or by a reload: the next open's */
    uint32_t        bnext_payload, bnext_result;

    gptps_thread  *dispatcher;
    gptps_thread **workers;
    unsigned       nworkers;
    struct gptps_worker *worker_args;   /* one per worker: the engine, and the worker's index */

    /* BOUNDED MODE (docs/BOUNDED.md). max_items == 0: the classic engine. Otherwise
     * the first submit (bounded_seal) allocates the working set below, and nothing on
     * the work path allocates after it. */
    uint64_t       max_items;
    uint32_t       max_payload, max_result;
    uint32_t       sealed;            /* stored with release once the working set exists */
    gptps_mutex   *pool_m;            /* guards free_items; made at open */
    gptps_item    *pool;              /* max_items items */
    gptps_item    *free_items;        /* the free ones, linked through ->next */
    unsigned char *payload_arena;     /* a payload slot per item, payload_stride apart */
    size_t         payload_stride;
    uint64_t      *snap_arena;        /* a named-resource snapshot slot per item, pool_nres wide */
    size_t         pool_nres;
    unsigned char *result_arena;      /* a result buffer per executing thread, result_stride apart */
    size_t         result_stride;
    struct gptps_cb_thread *cb_spare; /* callback-thread records made at the seal, unused yet */

    bool           stopping;
    /* Shutdown drain bound. gptps_shutdown waits for in-flight work to finish; an
     * OOP/PROGRAM task with no timeout whose child never exits, or a cooperative
     * in-process body that ignores its deadline, would otherwise hang teardown
     * FOREVER - and since this is an in-process library, that hangs the host's exit
     * path and leaves external children orphaned when the supervisor kills it. Once
     * the grace elapses the dispatcher raises every in-flight item's cancel flag.
     * 0 => wait forever (the pre-1.0 behaviour, now opt-in). */
    uint32_t       shutdown_grace_ms;
    uint64_t       stop_deadline_ms;   /* monotonic; 0 = not shutting down / no bound */
    /* measure.sample_ms (docs/MEASUREMENTS.md): how often a running process job emits
     * a GPTPS_EV_SAMPLE; 0 = never. Read by the executing thread with no lock held,
     * so it is accessed through the HAL's acquire/release pair. */
    uint32_t       sample_ms;
    bool           workers_exit;
    bool           manual;         /* MANUAL mode: no threads; driven by gptps_step() */

    /* Re-entrancy detection. gptps_shutdown joins the dispatcher + every worker, so
     * calling it from a task body or an event callback makes a thread join ITSELF -
     * a deadlock in THREADED mode, and in MANUAL mode a free of the engine that
     * gptps_step is still standing on. Each owned thread records its id here at
     * startup, and gptps_step publishes the id of whoever is pumping it, so those
     * calls can be refused with GPTPS_E_BUSY instead. */
    uint64_t      *owned_tids;     /* dispatcher + workers; NULL in MANUAL mode */
    unsigned       n_owned_tids, cap_owned_tids;
    uint64_t       step_tid;       /* thread currently inside gptps_step (0 = none) */
    /* Fork generation this engine was CREATED in. If the process forks, the child's
     * generation advances, so an engine carried across the fork no longer matches and
     * every entry point refuses it - the mutex it holds may be locked by a thread that
     * did not survive. An engine opened fresh in the child matches and works normally,
     * which is what the fork-a-worker-process pattern (addons/gptps_xport) needs. */
    uint64_t       fork_gen;

    gptps_handle   next_handle;
    gptps_event_cb ev_cb;
    void          *ev_ud;
    gptps_sched_fn sched_fn;      /* swappable admission ordering (NULL => built-in priority) */
    void          *sched_ud;
    /* Who installed it. COPIED, not borrowed: an add-on passes its namespace token,
     * which lives in the .so and outlives the engine - but a host can perfectly
     * reasonably pass a stack buffer, and gptps_scheduler_owner would then hand back
     * a dangling pointer. 32 bytes is enough by construction: the namespace grammar
     * caps a token at 31 characters. NULL owner / released seam => empty string. */
    char           sched_owner[32];
    /* The seam as it was before the add-on load that changed it last, if one did
     * (sched_tag is that load's tag; NULL once anyone else changes it): what a failed
     * setup puts back - and only then, so a seam the host set meanwhile stays. */
    const void    *sched_tag;
    gptps_sched_fn sched_undo_fn;
    void          *sched_undo_ud;
    char           sched_undo_owner[32];

    /* Namespace enforcement window (ABI 2.1). Set under e->m immediately before an
     * add-on's setup() runs and cleared immediately after. The tid pin means only
     * the thread INSIDE setup() is policed, so a host thread registering
     * concurrently is never caught by someone else's namespace.
     *
     * There is exactly ONE window, so two concurrent loads would clobber each
     * other's - the second's namespace would police the first, and enforcement
     * would then FAIL OPEN when the window was cleared early. `loading` serialises
     * the whole load, which closes that and the claim TOCTOU (two add-ons both
     * passing the "is this token free?" scan before either is recorded) with one
     * flag rather than two fixes. */
    const char    *cur_ns;
    size_t         cur_ns_len;
    uint64_t       cur_ns_tid;
    int            loading;       /* a gptps_load_addon is in progress */

    gptps_loaded  *addons;        /* dlopen'd add-ons, torn down at shutdown */
    gptps_observer *observers;    /* extra event sinks (registered before submit) */
    gptps_constraint *constraints;/* admission hooks consulted by the dispatcher */

    gptps_toml    *toml;          /* parsed config file (NULL if opened without one) */
    uint32_t       toml_gen;      /* bumped each time a reload swaps `toml` */
    int            toml_reloading;/* a reload is applying a file not yet in `toml` */
    int            toml_saving;   /* saves copying `toml`'s file: a reload waits for them */
    unsigned       cfg_late_errors;   /* file values found invalid after open (gptps_config_check) */
    int            cfg_reported;  /* the unclaimed-key report was made (first submit, or the check) */
    int            cfg_opening;   /* gptps_open_ex is still reading the file: no report yet */
    int            cfg_report_due;/* the first submit asked for the report; a thread that holds no
                                   * settings lock makes it (the dispatcher, or gptps_step) */
    int            setup_on;      /* an add-on's setup is running, on setup_tid: its file values */
    uint64_t       setup_tid;     /* wait until it returns (gptps_load_addon), and what it */
    const void    *setup_tag;     /* registers is tagged with the load, to be undone if it fails */
    uintptr_t      load_seq;      /* makes each load's tag unique: a library handle is not */
    uint32_t       reserve_after_skips; /* starvation guard: reserve a budget-blocked top task after this many backfill skips */
    gptps_settings *settings;     /* unified settings registry */
    char          *config_path;   /* the path opened with (NULL if none); default for save/reload */
    gptps_owned_setting *owned_settings; /* engine-stored generic global knobs (gptps_define_global) */
    gptps_task_schema   *task_schemas;   /* generic per-task setting schemas (gptps_define_task_setting) */
    unsigned             active_defines; /* gptps_define_task_setting / _resource calls between their
                                          * snapshot of the registry and their publish: a reg must not
                                          * be freed while >0 (one may hold it), nor a new one linked */
    /* A record per thread the engine has called back on from one of these calls (a
     * host thread, or one of its own that called in, e.g. a task body's gptps_submit),
     * kept until shutdown (idle ones are taken over by new thread ids), whose depth is
     * above 0 while that thread is inside one of these callbacks: the QUEUED that
     * gptps_submit emits, the FAILED that gptps_cancel, gptps_unregister_task and
     * gptps_shutdown emit, a dead-letter drain callback, a settings watcher or a
     * write accessor gptps_settings_set / _reload runs, an event an add-on emits,
     * an add-on's setup, teardown and disable. Neither owned_tids nor step_tid
     * knows about those threads, and a gptps_shutdown from such a callback freed
     * the engine under the call still using it. Records the engine owns, rather
     * than frames linked through the callers' stacks, so a callback that never
     * returns normally (a longjmp, a C++ throw) leaves its thread's depth above 0 -
     * the thread is refused from then on - and not a dangling pointer. Entered
     * under e->m, which every such call site but gptps_settings_set and _reload
     * already holds (those take it, in cb_enter), and left with no lock. */
    gptps_cb_thread *cb_threads[GPTPS_CB_BUCKETS];   /* under m; freed at shutdown */
    unsigned         n_cb_threads;
    /* Fields added after the hot ones above, at the end so they do not move them
     * (measurably: gptps_submit throughput is sensitive to this struct's layout). */
    uint64_t       intake_seq;     /* last intake_seq stamped (intake_insert) */
    /* limits.max_concurrent_tasks as last SET. The setting is restart-only - the
     * pool is sized once, at open - so a live write lands here, for reads and
     * gptps_settings_save, and never in e->limits, which admission reads. */
    uint32_t       conc_next;
    /* handle -> item for every item gptps_cancel can still reach (see "finding an
     * item"). Open addressing, a load of at most a half, under m. */
    gptps_hslot   *hidx;
    size_t         nhidx, hidx_live;
    /* Unregistered types kept until shutdown, through `next`: a dead letter of one that
     * could not have its own copy of the name made still names its type through it
     * (see detach_dead_letter). Under m. */
    gptps_reg     *retired;
};

/* ------------------------------------------------------------------------- */
/* fifo helpers                                                              */
/* ------------------------------------------------------------------------- */

static void fifo_push(gptps_fifo *q, gptps_item *it)
{
    it->next = NULL;
    it->prev = q->tail;
    if (q->tail) q->tail->next = it; else q->head = it;
    q->tail = it;
    q->count += 1;
    it->where = q->id;
}
static gptps_item *fifo_pop(gptps_fifo *q)
{
    gptps_item *it = q->head;
    if (it) {
        q->head = it->next;
        if (q->head) q->head->prev = NULL; else q->tail = NULL;
        it->next = it->prev = NULL;
        q->count -= 1;
        it->where = GPTPS_Q_NONE;
    }
    return it;
}
/* Unlink `target`, which must be in `q`, in O(1). */
static void fifo_remove(gptps_fifo *q, gptps_item *target)
{
    if (target->prev) target->prev->next = target->next; else q->head = target->next;
    if (target->next) target->next->prev = target->prev; else q->tail = target->prev;
    target->next = target->prev = NULL;
    q->count -= 1;
    target->where = GPTPS_Q_NONE;
}

/* ------------------------------------------------------------------------- */
/* finding an item                                                           */
/* ------------------------------------------------------------------------- */
/* gptps_cancel used to find its item by walking running_items, ready, done, intake
 * and delayed in turn, and to unlink it from a singly linked list by walking again;
 * a cancel from intake then wiped intake's run cache, so the next submit walked the
 * whole queue for its place. Cancelling a deep queue newest-first was O(n^2) - 4.5s
 * for 40,000 queued items - and so was cancelling and resubmitting at depth: every
 * cancel held the engine lock for its walk, stalling the dispatcher and every
 * submitter, and gptps_dq_cancel, orch, balance and a 06:30 "cancel what is left" all
 * end up here. Admission was made O(1) in queue depth for the same reason
 * (tests/test_admission_perf.c), and leaving intake unbounded is only "memory, never
 * throughput" if every path that touches it agrees.
 *
 * So every queue is doubly linked (any item unlinks in O(1)), each item records which
 * queue holds it (`where`, kept by fifo_push / fifo_pop / fifo_remove and the intake
 * pair, so no caller sets it), and an index maps a handle to its item for as long as
 * gptps_cancel may act on it: from submit until the item is freed, dead-lettered (a
 * dead letter is not cancellable), or detached into a local list to be drained
 * without the lock. Those three exits are the only ways out of the live queues, and
 * each removes the entry: item_drop, dead_letter_push, and index_drop_list at the
 * detach sites. A missed one would leave a pointer to freed memory behind, which
 * the cancel-after-every-ending tests and ASan exist to catch.
 * tests/test_cancel_perf.c gates the shape of the curve. */
static size_t hidx_slot(gptps_handle h, size_t n)
{
    h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33;
    return (size_t)(h & (n - 1));
}

/* The table for `live` entries: a power of two, at least 64, a quarter full. */
static size_t hidx_size_for(size_t live)
{
    size_t nn;
    for (nn = 64; nn < live * 4; nn <<= 1) { }
    return nn;
}

/* Rebuild the index at nn slots. 0 on success; on failure the old table stands.
 * e->m held. */
static int hidx_rebuild(gptps *e, size_t nn)
{
    gptps_hslot *nt = (gptps_hslot *)gptps_calloc(nn, sizeof *nt);
    size_t i;
    if (!nt) return -1;
    for (i = 0; i < e->nhidx; ++i) {
        size_t j;
        if (!e->hidx[i].h) continue;
        for (j = hidx_slot(e->hidx[i].h, nn); nt[j].h; j = (j + 1) & (nn - 1)) { }
        nt[j] = e->hidx[i];
    }
    gptps_free(e->hidx);
    e->hidx = nt; e->nhidx = nn;
    return 0;
}

/* Room for one more entry at a load of at most a half, so a probe always meets an
 * empty slot. 0 on success. e->m held. */
static int hidx_reserve(gptps *e)
{
    if ((e->hidx_live + 1) * 2 <= e->nhidx) return 0;
    if (e->hidx_live + 1 > ((size_t)-1) / (8 * sizeof(gptps_hslot))) return -1;
    return hidx_rebuild(e, hidx_size_for(e->hidx_live + 1));
}

static void hidx_put(gptps *e, gptps_item *it)   /* hidx_reserve first; e->m held */
{
    size_t j = hidx_slot(it->handle, e->nhidx);
    while (e->hidx[j].h) j = (j + 1) & (e->nhidx - 1);
    e->hidx[j].h = it->handle; e->hidx[j].it = it;
    e->hidx_live += 1;
    it->indexed = 1;
}

static gptps_hslot *hidx_find(gptps *e, gptps_handle h)   /* e->m held */
{
    size_t j, k;
    if (!e->nhidx || h == 0) return NULL;
    for (j = hidx_slot(h, e->nhidx), k = 0; k < e->nhidx && e->hidx[j].h;
         ++k, j = (j + 1) & (e->nhidx - 1))
        if (e->hidx[j].h == h) return &e->hidx[j];
    return NULL;
}

/* Drop `it` from the index - it can no longer be cancelled. Idempotent. e->m held.
 *
 * No tombstone is left behind. The gap is closed by moving back each later entry of
 * its probe run whose home slot does not lie between the gap and itself (Knuth's
 * Algorithm R for linear probing), so a lookup never meets a hole inside a run. A
 * table therefore never has to be rebuilt just to clear deleted slots, and a bounded
 * engine's index can keep one size for its whole life. */
static void hidx_del(gptps *e, gptps_item *it)
{
    gptps_hslot *s;
    size_t mask, i, j;
    if (!it->indexed) return;
    it->indexed = 0;
    s = hidx_find(e, it->handle);
    if (!s) return;
    mask = e->nhidx - 1;
    i = (size_t)(s - e->hidx);
    for (j = (i + 1) & mask; e->hidx[j].h; j = (j + 1) & mask) {
        size_t k = hidx_slot(e->hidx[j].h, e->nhidx);   /* the entry's home slot */
        if ((j > i) ? (k <= i || k > j) : (k <= i && k > j)) {   /* home not in (i, j]: move it back */
            e->hidx[i] = e->hidx[j];
            i = j;
        }
    }
    e->hidx[i].h = 0; e->hidx[i].it = NULL;
    e->hidx_live -= 1;
}

/* The same for every item of a local list about to be drained without the lock. */
static void index_drop_list(gptps *e, gptps_fifo *q)
{
    gptps_item *it;
    for (it = q->head; it; it = it->next) hidx_del(e, it);
}

/* Shrink the index as the queues do, or a burst would hold its memory - 32-64 bytes an
 * item at 64-bit - until gptps_shutdown. Once an eighth of it or less is live, it is
 * rebuilt a quarter full, by the dispatcher's next pass rather than in gptps_cancel:
 * one rebuild however far the queues fell, amortized O(1) per submit and drop as
 * growing is. One that cannot allocate leaves the old table. e->m held. */
static void hidx_trim(gptps *e)
{
    if (e->max_items) return;               /* bounded: made at its final size, never rebuilt */
    if (e->nhidx > 64 && e->hidx_live * 8 < e->nhidx)
        (void)hidx_rebuild(e, hidx_size_for(e->hidx_live));
}

/* ------------------------------------------------------------------------- */
/* intake ordering                                                           */
/* ------------------------------------------------------------------------- */
/* The intake queue is kept in ADMISSION order - sched_score DESCENDING, ties
 * oldest-first - rather than submission order. The dispatcher then reads the item
 * it wants off the head instead of searching for it.
 *
 * WHY. Admission used to scan the whole queue twice per admitted item: once to find
 * the highest-scoring item that fits the live budget, once more to unlink it. Since
 * limits.max_intake_depth defaults to 0 - unbounded, and deliberately so, because a
 * host that submits its own work should not have gptps_submit start failing (see
 * docs/SECURITY.md) - a producer that outruns the dispatcher grows the queue to
 * O(n), which made admitting n items O(n^2). Measured with a no-op task, throughput
 * collapsed from ~110k items/s at a depth of 20k to ~4.4k/s at 160k; ordered intake
 * holds ~250k/s flat across the same range (tests/test_admission_perf.c).
 *
 * Sorting moves the cost to insertion, and an ordered insert that walks the list is
 * the same quadratic in a different place - so intake_runs caches the TAIL of each
 * equal-score run. Real workloads use a handful of distinct priorities, so the cache
 * answers virtually every insert and the splice is O(1).
 *
 * THE CACHE IS ADVISORY. A miss just walks for the insertion point, and
 * intake_forget() may be called at any time. The single hard rule is that no cached
 * tail may dangle, so every removal from intake either goes through intake_unlink()
 * - which repairs the affected run - or is followed by intake_forget().
 *
 * ORDER IS NOT A NEW POLICY. It is the order the old double scan already picked,
 * made explicit: highest score first, and within one score the oldest item, so FIFO
 * still holds inside a priority. tests/test_admission_order.c pins that. */

static void intake_forget(gptps *e) { e->n_intake_runs = 0; }

/* Index of the cached run for `score`, or where a new one belongs (runs are sorted
 * by score DESCENDING). *found says which of the two it is. */
static size_t intake_run_slot(gptps *e, int64_t score, int *found)
{
    size_t lo = 0, hi = e->n_intake_runs;
    *found = 0;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (e->intake_runs[mid].score == score) { *found = 1; return mid; }
        if (e->intake_runs[mid].score > score) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static void intake_run_cache(gptps *e, size_t at, int64_t score, gptps_item *tail)
{
    size_t i;
    if (e->n_intake_runs == GPTPS_INTAKE_RUNS) {
        if (at == e->n_intake_runs) return;     /* lower than everything cached: skip it */
        e->n_intake_runs -= 1;                  /* evict the lowest-scoring run */
    }
    for (i = e->n_intake_runs; i > at; --i) e->intake_runs[i] = e->intake_runs[i - 1];
    e->intake_runs[at].score = score;
    e->intake_runs[at].tail  = tail;
    e->n_intake_runs += 1;
}

static void intake_run_drop(gptps *e, size_t at)
{
    size_t i;
    for (i = at + 1; i < e->n_intake_runs; ++i) e->intake_runs[i - 1] = e->intake_runs[i];
    e->n_intake_runs -= 1;
}

/* Splice `it` in after the last item scoring >= it, i.e. onto the tail of its own
 * score's run - which is what keeps equal scores in submission order. */
static void intake_insert(gptps *e, gptps_item *it)
{
    gptps_fifo *q = &e->intake;
    gptps_item *prev;
    size_t at;
    int found;

    it->intake_seq = ++e->intake_seq;                    /* the tie-break intake_sort uses */
    at = intake_run_slot(e, it->sched_score, &found);
    if (found) {
        prev = e->intake_runs[at].tail;                  /* cache hit: O(1) */
    } else {
        gptps_item *cur;                                 /* miss: walk for the point */
        for (prev = NULL, cur = q->head;
             cur && cur->sched_score >= it->sched_score;
             prev = cur, cur = cur->next) { }
    }

    it->next = prev ? prev->next : q->head;
    it->prev = prev;
    if (it->next) it->next->prev = it; else q->tail = it;
    if (prev) prev->next = it; else q->head = it;
    q->count += 1;
    it->where = GPTPS_Q_INTAKE;

    if (found) e->intake_runs[at].tail = it;             /* it is the run's new tail */
    else       intake_run_cache(e, at, it->sched_score, it);
}

/* Unlink `it` from intake, in O(1), and repair the run cache if it was pointing at
 * `it` as a run tail - so a removal never costs the next insert a walk. */
static void intake_unlink(gptps *e, gptps_item *it)
{
    gptps_fifo *q = &e->intake;
    gptps_item *prev = it->prev;
    size_t at;
    int found;

    if (prev) prev->next = it->next; else q->head = it->next;
    if (it->next) it->next->prev = prev; else q->tail = prev;
    q->count -= 1;

    at = intake_run_slot(e, it->sched_score, &found);
    if (found && e->intake_runs[at].tail == it) {
        if (prev && prev->sched_score == it->sched_score) e->intake_runs[at].tail = prev;
        else intake_run_drop(e, at);                     /* that run is empty now */
    }
    it->next = it->prev = NULL;
    it->where = GPTPS_Q_NONE;
}

/* Bottom-up merge sort of an item list by sched_score DESCENDING, equal scores
 * oldest-first by intake_seq. The tie-break is explicit because stability alone
 * only keeps whatever order the list arrived in, and with a hook that was not
 * submission order: an item enters intake at sched_score = priority and is placed
 * by it against items the hook has already scored, so a hook returning, say,
 * -deadline (below the default priority 0) put every newcomer AHEAD of the equal-
 * score items already waiting, and a stable sort kept it there - newest first.
 * Iterative because it runs with the engine lock held. */
static gptps_item *intake_sort(gptps_item *list)
{
    gptps_item *p, *q, *pick, *tail;
    size_t insize = 1, nmerges, psize, qsize, i;

    if (!list || !list->next) return list;
    for (;;) {
        p = list; list = NULL; tail = NULL; nmerges = 0;
        while (p) {
            ++nmerges;
            q = p; psize = 0;
            for (i = 0; i < insize; ++i) { ++psize; q = q->next; if (!q) break; }
            qsize = insize;
            while (psize > 0 || (qsize > 0 && q)) {
                if (psize == 0)                            { pick = q; q = q->next; --qsize; }
                else if (qsize == 0 || !q)                 { pick = p; p = p->next; --psize; }
                else if (q->sched_score > p->sched_score ||
                         (q->sched_score == p->sched_score &&
                          q->intake_seq < p->intake_seq))  { pick = q; q = q->next; --qsize; }
                else                                       { pick = p; p = p->next; --psize; }
                if (tail) tail->next = pick; else list = pick;
                tail = pick;
            }
            p = q;
        }
        tail->next = NULL;
        if (nmerges <= 1) return list;
        insize <<= 1;
    }
}

/* Re-establish admission order after a scheduler hook restamped every score. The sort
 * relinks `next` only, so the back links are rebuilt on the way to the tail. */
static void intake_resort(gptps *e)
{
    gptps_item *it, *prev = NULL;
    e->intake.head = intake_sort(e->intake.head);
    for (it = e->intake.head; it; prev = it, it = it->next) it->prev = prev;
    e->intake.tail = prev;
    intake_forget(e);                 /* every cached tail is now meaningless */
}

/* An engine created before a fork() must not be used in the child: its mutex may be
 * held by a thread that did not survive, so TAKING the lock is itself the hazard -
 * the check has to happen before it, not inside it. include/gptps.h promises this on
 * every entry point; this macro is how that promise is kept, and keeps it greppable.
 * `ret` is what the entry point returns to say "engine unusable" for its own type. */
#define GPTPS_REFUSE_AFTER_FORK(e, ret) \
    do { if ((e)->fork_gen != gptps_hal_fork_generation()) return ret; } while (0)

/* The tag of the add-on load whose setup is running on this thread, or NULL. What
 * such a setup registers - a task type, an observer, a constraint, a scheduler -
 * carries it, so that if the setup fails the unwind removes exactly that, and
 * nothing a host thread registered meanwhile. Caller holds e->m. */
static const void *setup_tag_here(const gptps *e)
{
    return (e->setup_on && e->setup_tid == gptps_hal_thread_id()) ? e->setup_tag : NULL;
}

/* The live (non-tombstoned) task that holds a name, whether or not its registration
 * is done. A draining task is logically gone: its name is free to re-register. One
 * still being registered already holds its name, so this is what a new registration
 * of the name collides with (GPTPS_E_DUP). */
static gptps_reg *registry_holder(const gptps *e, const char *name)
{
    gptps_reg *r;
    for (r = e->registry; r; r = r->next)
        if (!r->removed && strcmp(r->name, name) == 0) return r;
    return NULL;
}

/* A type still being registered, seen from another thread. gptps_register_task links
 * a type in before it adds its settings, and writes into it until it returns: until
 * then the type is not there yet for other threads - a submit is refused, and so is a
 * pause, a clone from it, a change of its priority or costs, and an unregister, which
 * would free it under that call; each answers GPTPS_E_NOTFOUND, as before the call.
 * The registering thread sees it: an add-on's settings watcher hears the type's file
 * values on that thread, as they are applied, and sets what they mean by its name
 * (gptps_gpu_quota_plugin.c sets the type's cost). Caller holds e->m. */
static int reg_hidden(const gptps_reg *r)
{
    return r->settling && r->settling_tid != gptps_hal_thread_id();
}

/* Find a task by name, as every operation that names one sees it: not one draining
 * toward removal, and not one another thread is still registering. */
static gptps_reg *registry_find(const gptps *e, const char *name)
{
    gptps_reg *r = registry_holder(e, name);
    return (r && !reg_hidden(r)) ? r : NULL;
}

/* Resolved name for an item (owned copy once detached from a removed reg). */
static const char *item_name(const gptps_item *it)
{
    if (it->name_owned) return it->name_owned;
    if (it->reg)        return it->reg->name;
    return it->def ? it->def->name : "?";
}

/* A bounded engine past its first submit: setup that would allocate is refused. */
static int bounded_sealed(const gptps *e)
{ return e->max_items && gptps_hal_load_acquire_u32(&e->sealed); }

/* The cancel word (gptps_item.cancel): raised by a cancel, a deadline or a removal,
 * polled by the body through gptps_is_cancelled and by the process executors. */
static void cancel_raise(gptps_item *it)        { gptps_hal_store_release_u32(&it->cancel, 1u); }
static void cancel_clear(gptps_item *it)        { gptps_hal_store_release_u32(&it->cancel, 0u); }
static int  cancel_raised(const gptps_item *it) { return gptps_hal_load_acquire_u32(&it->cancel) != 0; }

static void item_free(gptps *e, gptps_item *it)
{
    if (!it) return;
    if (it->pooled) {                       /* bounded: its storage is the pool's */
        gptps_free(it->name_owned);         /* never made once sealed; never leaked either */
        it->name_owned = NULL;
        gptps_mutex_lock(e->pool_m);
        /* A second free must not link the item in twice: two submits would then share
         * it. Where the classic engine has ASan to catch a double free, this keeps the
         * pool whole. */
        if (it->where != GPTPS_Q_POOL) {
            it->where = GPTPS_Q_POOL;
            it->next = e->free_items;
            e->free_items = it;
        }
        gptps_mutex_unlock(e->pool_m);
        return;
    }
    gptps_free(it->payload);
    gptps_free(it->name_owned);
    gptps_free(it->res_reserved);
    gptps_free(it);
}

/* Free an item gptps_cancel could still reach - one of the three ways out of the live
 * queues (see "finding an item"). e->m held. */
static void item_drop(gptps *e, gptps_item *it)
{
    hidx_del(e, it);
    item_free(e, it);
}

/* Emit `p`, with the measurements an attempt reported (docs/MEASUREMENTS.md): only
 * a process job's FINISHED / FAILED and its SAMPLEs carry any. They live on the
 * executing thread's stack, which outlives this synchronous emit. */
static void emit_with(gptps *e, gptps_event_cb cb, void *ud, const gptps_pending_ev *p,
                      const gptps_measure *m, size_t nm)
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
    ev.flags = p->flags;
    ev.measures = nm ? m : NULL; ev.n_measures = nm;
    if (cb) cb(&ev, ud);
    for (o = e->observers; o; o = o->next) o->fn(&ev, o->ud); /* extra sinks */
}

static void emit_now(gptps *e, gptps_event_cb cb, void *ud, const gptps_pending_ev *p)
{
    emit_with(e, cb, ud, p, NULL, 0);
}

void gptps_meter_put(gptps_exec_meter *mt, const char *name, uint64_t value,
                     unsigned unit, unsigned kind, const char *method)
{
    size_t i;
    if (!mt || !name || !method) return;
    for (i = 0; i < mt->n; ++i)
        if (strcmp(mt->m[i].name, name) == 0) return;   /* one value per name: the first,
                                                         * best method wins */
    if (mt->n >= GPTPS_EXEC_MEASURES_MAX) return;
    mt->m[mt->n].name = name; mt->m[mt->n].value = value;
    mt->m[mt->n].unit = (uint16_t)unit; mt->m[mt->n].kind = (uint16_t)kind;
    mt->m[mt->n].flags = 0; mt->m[mt->n].method = method;
    mt->n += 1;
}

const gptps_measure *gptps_event_measure(const gptps_event *ev, const char *name)
{
    size_t i;
    if (!ev || !name || !GPTPS_STRUCT_HAS(gptps_event, ev, n_measures) || !ev->measures) return NULL;
    for (i = 0; i < ev->n_measures; ++i)
        if (ev->measures[i].name && strcmp(ev->measures[i].name, name) == 0) return &ev->measures[i];
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* ctx accessors                                                             */
/* ------------------------------------------------------------------------- */

bool        gptps_is_cancelled(const gptps_ctx *ctx) { return ctx && ctx->cancel && gptps_hal_load_acquire_u32(ctx->cancel) != 0; }
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
    if (ctx->bounded) {                     /* bounded: the thread's buffer; nothing allocates */
        if (len > ctx->result_cap || !ctx->result_buf) return GPTPS_E_INVAL;
        memcpy(ctx->result_buf, bytes, len);
        ctx->result = ctx->result_buf; ctx->result_len = len; ctx->result_free = NULL;
        ctx->result_is_copy = false; ctx->result_set = true;
        return GPTPS_OK;
    }
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
    /* Hand the task's own result buffer straight out instead of duplicating it, and
     * do NOT clear the ctx. This runs in a FORKED CHILD of a threaded process: if the
     * host installed a lock-guarded allocator via gptps_set_allocator, its mutex may
     * have been held by a thread that did not survive the fork, so any malloc/free
     * here can deadlock the child (and hang its parent's worker). The caller writes
     * these bytes to the pipe and then _exit()s, which reclaims everything - so the
     * skipped free is not a leak. Callers must therefore NOT free *out_result. */
    if (ctx.result_set && ctx.result_len) { *out_result = ctx.result; *out_len = ctx.result_len; }
    return st;
}

/* ------------------------------------------------------------------------- */
/* worker                                                                    */
/* ------------------------------------------------------------------------- */

/* What a running process job's SAMPLE needs to reach the event sinks: the engine,
 * the callback pair execute() snapshotted, and the attempt's event. */
typedef struct {
    gptps_exec_meter        meter;      /* first: the executor hands back &meter */
    gptps                  *e;
    gptps_event_cb          cb;
    void                   *ud;
    const gptps_pending_ev *p;
} exec_sampler;

/* Called by a process executor, on the executing thread with no lock held, about
 * every measure.sample_ms while the job runs (docs/MEASUREMENTS.md). */
static void emit_sample(gptps_exec_meter *mt, const gptps_measure *cur, size_t n)
{
    exec_sampler *sp = (exec_sampler *)(void *)mt;
    gptps_pending_ev q = *sp->p;
    if (!n) return;
    q.kind = GPTPS_EV_SAMPLE; q.status = GPTPS_OK;
    q.result = NULL; q.result_len = 0; q.flags = 0;
    emit_with(sp->e, sp->cb, sp->ud, &q, cur, n);
}

/* cb/ud are snapshotted under the lock by the caller so a concurrent
 * gptps_set_event_cb cannot pair a new callback with a stale user_data. */
/* `slot` is the executing thread's: a worker's index, or nworkers for gptps_step. It
 * picks the thread's result buffer on a bounded engine. */
static gptps_status execute(gptps *e, gptps_item *it, gptps_event_cb cb, void *ud, unsigned slot)
{
    gptps_pending_ev p;
    gptps_status st;
    struct gptps_ctx ctx;          /* used only on the in-process path */
    void *oop_res = NULL;
    size_t oop_len = 0;
    bool inproc = (it->def->exec == GPTPS_EXEC_INPROC);
    bool raised = false;           /* the cancel flag, read ONCE after the attempt */
    exec_sampler sp;               /* a process job's measurements (docs/MEASUREMENTS.md) */

    p.handle = it->handle; ev_set_name(p.name, item_name(it)); p.attempt = it->attempt; p.mem = it->cost.mem_bytes;
    p.result = NULL; p.result_len = 0; p.flags = 0;
    p.kind = GPTPS_EV_STARTED; p.status = GPTPS_OK; emit_now(e, cb, ud, &p);

    memset(&sp, 0, sizeof sp);
    sp.e = e; sp.cb = cb; sp.ud = ud; sp.p = &p;
    sp.meter.sample_ms = gptps_hal_load_acquire_u32(&e->sample_ms);
    sp.meter.sample = emit_sample;

    if (inproc) {
        /* in-process path: cooperative cancel via the deadline flag */
        memset(&ctx, 0, sizeof ctx);
        ctx.engine = e; ctx.reg = it->reg; ctx.handle = it->handle; ctx.task_name = it->def->name;
        ctx.payload = it->payload; ctx.payload_len = it->payload_len;
        ctx.deadline_ms = it->deadline_ms; ctx.cancel = &it->cancel;
        if (e->max_items) {                /* bounded: results go into this thread's buffer */
            ctx.bounded = true;
            ctx.result_cap = e->max_result;
            ctx.result_buf = e->result_arena ? e->result_arena + (size_t)slot * e->result_stride : NULL;
        }
        st = it->def->run(&ctx, it->def->user_data);
        raised = cancel_raised(it);
        if (raised) {
            /* Tell a deadline breach apart from an explicit stop. The dispatcher's
             * watchdog raises this flag only once the deadline has passed, so a flag
             * raised with no deadline at all - or before it - came from gptps_cancel,
             * shutdown, or task removal. Inferred from the deadline rather than read
             * from it->cancelled, which the engine writes under e->m while this runs
             * with the lock RELEASED (reading it here would be a data race). */
            st = (ctx.deadline_ms && gptps_hal_monotonic_ms() >= ctx.deadline_ms)
               ? GPTPS_E_TIMEOUT : GPTPS_E_CANCELLED;
        }
    } else if (it->def->exec == GPTPS_EXEC_OOP) {
        /* enforced path: run the in-process fn in a forked child, OS-capped, hard-killed.
         * it->cancel lets gptps_cancel / shutdown / removal hard-kill the child. */
        st = gptps_oop_execute(it->def, it->payload, it->payload_len,
                               it->cost.mem_bytes, it->policy.timeout_seconds, &it->cancel, &oop_res, &oop_len,
                               &sp.meter);
    } else if (it->def->exec == GPTPS_EXEC_PROGRAM) {
        /* enforced path: fork+exec an external program; payload->stdin, stdout->result */
        st = gptps_program_execute(it->def, it->payload, it->payload_len,
                                   it->cost.mem_bytes, it->policy.timeout_seconds, &it->cancel, &oop_res, &oop_len,
                                   &sp.meter);
    } else {
        /* Unreachable: gptps_register_task rejects an out-of-range exec kind. This is a
         * hard stop rather than the fallthrough it replaces, and the difference matters
         * for FORWARD compatibility, not for today's three kinds.
         *
         * The old code let the PROGRAM branch catch every value that was not INPROC or
         * OOP. So a def carrying an unknown kind was RUN AS A PROGRAM - with argv NULL,
         * which fails deep inside the executor and burns the item's whole retry budget
         * before dead-lettering it. A misconfiguration was diagnosed as a task failure.
         *
         * If a future ABI MINOR ever appends a fourth kind, an older core loading a
         * newer add-on MUST refuse work it cannot run, never silently run it as
         * something else. Rejecting here is what makes appending a kind safe later. */
        st = GPTPS_E_INVAL;
    }

    /* deliver the result on the FINISHED event (valid for the callback's duration) */
    p.kind = (st == GPTPS_OK) ? GPTPS_EV_FINISHED : GPTPS_EV_FAILED; p.status = st;
    /* A GPTPS_E_CANCELLED with the flag down is the body's own (or the child's):
     * nothing stopped it. One read decides both this and the TIMEOUT/CANCELLED split
     * above - a second read could see the flag the deadline watchdog raised in
     * between, and report a body's own cancel as neither. A stop that raises the flag
     * after the body returned counts as a stop, the conservative reading for a
     * consumer that keeps stopped work. */
    if (!inproc) raised = cancel_raised(it);
    if (st == GPTPS_E_CANCELLED && !raised)
        p.flags = GPTPS_EV_FLAG_SELF_CANCELLED;
    if (st == GPTPS_OK) {
        if (inproc) { if (ctx.result_set) { p.result = ctx.result; p.result_len = ctx.result_len; } }
        else        { p.result = oop_res; p.result_len = oop_len; }
    }
    emit_with(e, cb, ud, &p, sp.meter.m, sp.meter.n);   /* an in-process attempt has none */

    if (inproc) ctx_clear_result(&ctx);
    else        gptps_free(oop_res);
    return st;
}

/* Record the calling thread as one this engine owns, so a re-entrant
 * gptps_shutdown from a task body / event callback can be refused instead of
 * joining the caller's own thread. Caller must NOT hold e->m. */
static void engine_note_own_thread(gptps *e)
{
    gptps_mutex_lock(e->m);
    if (e->owned_tids && e->n_owned_tids < e->cap_owned_tids)
        e->owned_tids[e->n_owned_tids++] = gptps_hal_thread_id();
    gptps_mutex_unlock(e->m);
}

/* Is `tid` one of this engine's own threads, or the one pumping gptps_step?
 * Caller holds e->m. */
static int engine_is_reentrant(const gptps *e, uint64_t tid)
{
    unsigned i;
    if (e->step_tid == tid) return 1;
    for (i = 0; i < e->n_owned_tids; ++i)
        if (e->owned_tids[i] == tid) return 1;
    return 0;
}

/* An idle record (depth 0: no thread is inside a callback on it) for `tid` to take
 * over: from its own chain, and - only once GPTPS_CB_THREADS_MAX records exist, when
 * this is the only way in - from any chain, moved to tid's. NULL if none. Caller
 * holds e->m. A record whose thread escaped a callback is never idle, so it is
 * never taken. */
static gptps_cb_thread *cb_take_idle(gptps *e, uint64_t tid, int any)
{
    size_t home = cb_bucket(tid), k;
    for (k = 0; k < GPTPS_CB_BUCKETS; ++k) {
        size_t b = (home + k) & (GPTPS_CB_BUCKETS - 1);
        gptps_cb_thread **pp = &e->cb_threads[b], *t;
        for (; (t = *pp) != NULL; pp = &t->next) {
            if (gptps_hal_load_acquire_u32(&t->depth) != 0) continue;
            if (b != home) {                     /* relink onto tid's chain */
                *pp = t->next;
                t->next = e->cb_threads[home];
                e->cb_threads[home] = t;
            }
            t->tid = tid;
            return t;
        }
        if (!any && e->n_cb_threads < GPTPS_CB_THREADS_MAX) break;   /* own chain only, then allocate */
    }
    return NULL;
}

/* Bracket a callback the engine makes on a thread it does not own, so a
 * gptps_shutdown or gptps_step from inside it can be refused. Caller holds e->m.
 * Returns the thread's record for cb_leave, or NULL when there is none to be had -
 * out of memory, or GPTPS_CB_THREADS_MAX threads inside callbacks at once: the
 * callback then runs unguarded. */
static gptps_cb_thread *cb_enter_locked(gptps *e)
{
    uint64_t tid = gptps_hal_thread_id();
    gptps_cb_thread **head = &e->cb_threads[cb_bucket(tid)];
    gptps_cb_thread *t;
    for (t = *head; t && t->tid != tid; t = t->next) { }
    if (!t) t = cb_take_idle(e, tid, 0);
    if (!t && e->max_items && gptps_hal_load_acquire_u32(&e->sealed)) {
        /* Bounded and sealed: nothing allocates. A record made at the seal, else any
         * idle one; with neither, the callback runs unguarded (docs/BOUNDED.md). */
        if ((t = e->cb_spare) != NULL) {
            e->cb_spare = t->next;
            t->tid = tid;
            t->next = *head;
            *head = t;
            e->n_cb_threads += 1;
        } else {
            t = cb_take_idle(e, tid, 1);
        }
    }
    else if (!t && e->n_cb_threads < GPTPS_CB_THREADS_MAX &&
        (t = (gptps_cb_thread *)gptps_calloc(1, sizeof *t)) != NULL) {
        t->tid = tid;
        t->next = *head;
        *head = t;
        e->n_cb_threads += 1;
    }
    if (!t) return NULL;
    gptps_hal_store_release_u32(&t->depth, gptps_hal_load_acquire_u32(&t->depth) + 1);
    return t;
}

/* The same, for a caller not holding e->m. */
static gptps_cb_thread *cb_enter(gptps *e)
{
    gptps_cb_thread *t;
    gptps_mutex_lock(e->m);
    t = cb_enter_locked(e);
    gptps_mutex_unlock(e->m);
    return t;
}

/* No lock: while it is inside, only the thread that entered touches its record. */
static void cb_leave(gptps_cb_thread *t)
{
    if (t) gptps_hal_store_release_u32(&t->depth, gptps_hal_load_acquire_u32(&t->depth) - 1);
}

/* engine_is_reentrant, or `tid` is inside a callback bracketed above - the test
 * gptps_shutdown and gptps_step refuse on. Only ever asked about the calling
 * thread, so reading its record's depth is its own. Caller holds e->m. */
static int engine_in_callback(const gptps *e, uint64_t tid)
{
    const gptps_cb_thread *t;
    if (engine_is_reentrant(e, tid)) return 1;
    for (t = e->cb_threads[cb_bucket(tid)]; t; t = t->next)
        if (t->tid == tid) return gptps_hal_load_acquire_u32(&t->depth) > 0;
    return 0;
}

/* The deadline of an attempt starting now, for the worker and gptps_step alike.
 * The deadline flag is the in-process cooperative path; OOP enforces its own
 * deadline in the worker (poll + hard-kill), so it gets no flag deadline. A
 * per-submit timeout_ms_override (gptps_submit_ex) wins over the task type's
 * timeout_seconds and allows a sub-second deadline - gptps_step used to read only
 * the latter, so in MANUAL mode GPTPS_SUBMIT_TIMEOUT_MS did nothing. */
static uint64_t attempt_deadline(const gptps_item *it)
{
    if (it->def->exec != GPTPS_EXEC_INPROC) return 0;
    if (it->timeout_ms_override) return gptps_hal_monotonic_ms() + (uint64_t)it->timeout_ms_override;
    return it->policy.timeout_seconds
        ? gptps_hal_monotonic_ms() + (uint64_t)it->policy.timeout_seconds * 1000u : 0;
}

static void *worker_main(void *arg)
{
    gptps_worker *w = (gptps_worker *)arg;
    gptps *e = w->e;
    engine_note_own_thread(e);
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
        it->deadline_ms = attempt_deadline(it);
        cancel_clear(it);
        it->started = 1;                   /* execute() will emit STARTED + a terminal event */
        fifo_push(&e->running_items, it);
        gptps_cond_signal(e->cv_disp);     /* let dispatcher track the new deadline */
        gptps_mutex_unlock(e->m);

        eff = execute(e, it, cb, ud, w->idx);

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
 * These are TERMINAL events - the observer seam's whole reconciliation contract
 * ("every submitted one-shot handle reaches exactly one terminal event") rests on
 * them, and so does the REQUEUE drain's own dead-letter emit below - so
 * a full buffer must never silently drop one. Both producers instead stop early and
 * report saturation through engine_pass's `out_more`, leaving the remaining work
 * queued for the next pass; both pumps re-run immediately while more is due. The
 * done-drain is bounded by max_concurrent_tasks, but a DENYing constraint hook is
 * bounded only by intake depth, which is unbounded by default. */
#define GPTPS_PENDING_CAP 256

/* Default starvation guard: a budget-blocked highest-priority task may be skipped
 * by at most this many backfill admissions before the dispatcher reserves for it
 * (suspends backfill and drains running tasks until it fits). Override per engine
 * via the config file's [scheduler] reserve_after_skips. */
#define GPTPS_RESERVE_AFTER 8u

/* Minimum delay before re-admitting an item under a policy that re-enqueues
 * INDEFINITELY - a service restart, or GPTPS_ON_FAILURE_REQUEUE. Both reset
 * `attempt`, so unlike a bounded retry there is no max_retries ceiling to stop
 * them: with retry_backoff_seconds left at its zero default (what a memset-zero
 * task_def gives you), a body that fails immediately would be re-admitted as fast
 * as the dispatcher can loop and peg a core. A BOUNDED retry is deliberately not
 * floored - a zero backoff there means "retry now" and max_retries ends it. */
#define GPTPS_REQUEUE_MIN_BACKOFF_MS 100u

/* Default shutdown drain bound (see gptps.shutdown_grace_ms). Long enough that a
 * normal drain finishes untouched, short enough that a stuck child cannot wedge the
 * host's exit path. Tunable live via the "limits.shutdown_grace_ms" setting or the
 * config file's [limits] shutdown_grace_ms. */
#define GPTPS_SHUTDOWN_GRACE_MS_DEFAULT 30000u

/* Default cap on retained dead-lettered items (see gptps.max_dead_letters). */
#define GPTPS_MAX_DEAD_LETTERS_DEFAULT 1024u

/* Retain a terminal failure, evicting the OLDEST first if the list is at its cap.
 * Caller holds e->m. An evicted item holds no admission budget (it was released by
 * the done-drain before it got here), so freeing it needs no ledger bookkeeping. */
static void dead_letter_push(gptps *e, gptps_item *it)
{
    hidx_del(e, it);                  /* a dead letter is drained, never cancelled */
    while (e->max_dead_letters && e->dead_letter_count >= e->max_dead_letters) {
        gptps_item *old = fifo_pop(&e->dead_letter);
        if (!old) break;
        e->dead_letter_count -= 1;
        e->dead_evicted += 1;
        item_free(e, old);
    }
    fifo_push(&e->dead_letter, it);
    e->dead_letter_count += 1;
}

static uint64_t requeue_at(uint64_t now, uint32_t backoff_seconds)
{
    uint64_t ms = (uint64_t)backoff_seconds * 1000u;
    if (ms < GPTPS_REQUEUE_MIN_BACKOFF_MS) ms = GPTPS_REQUEUE_MIN_BACKOFF_MS;
    return now + ms;
}

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

/* Can this item EVER fit, i.e. is its declared cost within the ABSOLUTE budget?
 * submit() rejects such an item with GPTPS_E_BUDGET, but a budget lowered at runtime
 * (limits.max_memory_bytes, gptps_define_resource re-budget) can strand an item that
 * was admissible when queued: it never fits, so it is never admitted, never reaches a
 * terminal event, and - via the reserve-for-`top` starvation guard - blocks everything
 * behind it and holds gptps_shutdown forever (the dispatcher exits only on an empty
 * intake). The admission scan uses this to dead-letter such an item in place, with the
 * same E_BUDGET the submit-time check would have given. DISPATCHER context, e->m held. */
static int res_never_fits(const gptps *e, const gptps_item *it)
{
    size_t i;
    const uint64_t *cost;
    if (it->cost.mem_bytes > e->limits.max_memory_bytes) return 1;
    cost = (it->reg ? it->reg->res_cost : NULL);
    if (!e->nres || !cost) return 0;
    for (i = 0; i < e->nres; ++i)
        if (cost[i] > e->resources[i].budget) return 1;
    return 0;
}

static int terminal_reported(const gptps_item *it);   /* defined with drain_cancelled */

static void engine_pass(gptps *e, gptps_pending_ev *pend, int *out_npend,
                        uint64_t *out_next_wake, int *out_more)
{
    uint64_t now = gptps_hal_monotonic_ms();
    uint64_t next_wake = 0; /* 0 = none */
    int npend = 0;
    int more = 0;           /* work is still owed, re-run at once: pend[] filled up, or
                             * a zero-backoff retry is due once its RETRIED is out (2b) */
    gptps_item *it;
    gptps_fifo announced = { NULL, NULL, 0, GPTPS_Q_NONE };   /* retries decided this pass: see 2b */
    uint32_t held = 0;      /* DUE retries 2b parked: step 4 keeps a slot free for each */
    uint64_t held_mem = 0;  /* ... and their memory (a saturating sum) from any work */
    int64_t held_score = 0; /* ... the highest of them outranks (its sched_score) */
    int past_grace = 0;     /* shutting down and the grace is over: admit nothing (3b) */

        /* 1) drain completed: release budget, then retry / terminal decision.
         * Bounded by the event buffer: an item left in `done` is picked up by the
         * next pass with its event intact, which is strictly better than handling
         * it now and losing the event. */
        while (npend < GPTPS_PENDING_CAP && (it = fifo_pop(&e->done)) != NULL) {
            e->reserved_mem -= it->cost.mem_bytes;
            e->running      -= 1;
            if (it->res_reserved) {                          /* release named-resource reservations */
                size_t ri;
                for (ri = 0; ri < it->res_n && ri < e->nres; ++ri)
                    e->resources[ri].reserved -= it->res_reserved[ri];
                /* free the snapshot NOW (symmetric with the admit-time alloc) so a
                 * re-admitted item - a retry, or a service's REQUEUE restart - cannot
                 * overwrite a live pointer and leak it. item_free tolerates NULL. */
                if (!it->pooled) gptps_free(it->res_reserved);   /* bounded: a slot, not a block */
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
                    it->not_before_ms = requeue_at(now, it->policy.retry_backoff_seconds);
                    fifo_push(&e->delayed, it);
                } else {
                    /* An always-up service's FINISHED ends one RUN, not the handle, so
                     * one stopped here - cancelled, removed or shut down after its run
                     * returned OK - still owes the FAILED / GPTPS_E_CANCELLED that ends
                     * the handle. Freed silently, it closed with no terminal event:
                     * gptps_cancel landing in the window after execute() read the
                     * flag, or shutdown finding the run in `done`. */
                    if (!terminal_reported(it) && npend < GPTPS_PENDING_CAP) {
                        pend[npend].kind = GPTPS_EV_FAILED; pend[npend].handle = it->handle;
                        ev_set_name(pend[npend].name, item_name(it)); pend[npend].status = GPTPS_E_CANCELLED;
                        pend[npend].attempt = it->attempt; pend[npend].mem = it->cost.mem_bytes;
                        pend[npend].result = NULL; pend[npend].result_len = 0; pend[npend].flags = 0; ++npend;
                    }
                    item_drop(e, it);
                }
                continue;
            }
            if (it->cancelled) {
                /* per-handle gptps_cancel: terminal, never retried or dead-lettered.
                 * It still owes a terminal event unless this attempt already sent
                 * one. An item whose attempt never started has had none. One that ran
                 * and saw the flag got FAILED / GPTPS_E_CANCELLED from execute(), and
                 * emitting here too would double-count. But a cancel can land after
                 * execute() read the flag: the attempt then reported its own FAILED,
                 * which is per-attempt, not terminal - and testing `started` alone
                 * freed that item with no terminal event at all. */
                if (!terminal_reported(it) && npend < GPTPS_PENDING_CAP) {
                    pend[npend].kind = GPTPS_EV_FAILED; pend[npend].handle = it->handle;
                    ev_set_name(pend[npend].name, item_name(it)); pend[npend].status = GPTPS_E_CANCELLED;
                    pend[npend].attempt = it->attempt; pend[npend].mem = it->cost.mem_bytes;
                    pend[npend].result = NULL; pend[npend].result_len = 0; pend[npend].flags = 0; ++npend;
                }
                item_drop(e, it);
                continue;
            }
            if (it->started && it->outcome == GPTPS_E_CANCELLED) {
                /* The attempt ended with GPTPS_E_CANCELLED without a per-handle cancel
                 * (it->cancelled, above): the body returned it, or a REMOVE_CANCEL
                 * raised the flag mid-run and execute() reported the stop. Either way
                 * its FAILED carried that status, which observers reconciling handles
                 * count as terminal, so the item ends here as a cancel would: no retry,
                 * no dead letter, no requeue. Retrying reopened a handle they had closed.
                 * (`started` matters: an item a REMOVE_CANCEL discarded before it ran
                 * has the same outcome and no event at all - the removal branch below
                 * owes it one.) */
                item_drop(e, it);
                continue;
            }
            if (it->reg && it->reg->removed) {
                /* task is being removed: never retry (keeps the drain bounded).
                 * CANCEL discards in-flight work; DROP frees; otherwise (DRAIN) a
                 * genuine failure is preserved in the dead-letter list. */
                if (it->reg->cancelling || it->policy.on_failure == GPTPS_ON_FAILURE_DROP) {
                    /* Still terminal - so it still owes a terminal event. Freeing these
                     * silently used to make a REMOVE_CANCEL destroy submitted items with
                     * no event at all, which breaks the reconciliation contract every
                     * observer-seam add-on is built on (gpu_quota, for one, releases its
                     * reservation only when it sees a terminal event, so a silent free
                     * leaked its budget permanently).
                     *   CANCEL: FAILED/E_CANCELLED is itself the terminal event, so it is
                     *     emitted unless this attempt already sent it - see the
                     *     cancelled branch above for why that is not `started`.
                     *   DROP:   EV_DROPPED after the attempt's FAILED, matching the
                     *     ordinary (non-removal) DROP path. */
                    int owes = it->reg->cancelling ? !terminal_reported(it) : 1;
                    if (owes && npend < GPTPS_PENDING_CAP) {
                        pend[npend].kind = it->reg->cancelling ? GPTPS_EV_FAILED : GPTPS_EV_DROPPED;
                        pend[npend].handle = it->handle;
                        ev_set_name(pend[npend].name, item_name(it));
                        pend[npend].status = it->reg->cancelling ? GPTPS_E_CANCELLED : it->outcome;
                        pend[npend].attempt = it->attempt; pend[npend].mem = it->cost.mem_bytes;
                        pend[npend].result = NULL; pend[npend].result_len = 0; pend[npend].flags = 0; ++npend;
                    }
                    item_drop(e, it);
                } else {
                    if (npend < GPTPS_PENDING_CAP) {
                        pend[npend].kind = GPTPS_EV_DEAD_LETTERED; pend[npend].handle = it->handle;
                        ev_set_name(pend[npend].name, item_name(it)); pend[npend].status = it->outcome;
                        pend[npend].attempt = it->attempt; pend[npend].mem = it->cost.mem_bytes;
                        pend[npend].result = NULL; pend[npend].result_len = 0; pend[npend].flags = 0; ++npend;
                    }
                    dead_letter_push(e, it);
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
                    pend[npend].result = NULL; pend[npend].result_len = 0; pend[npend].flags = 0; ++npend;
                }
                fifo_push(&announced, it);   /* joins `delayed` after step 2's scan */
            } else {
                switch (it->policy.on_failure) {
                    case GPTPS_ON_FAILURE_REQUEUE:
                        if (e->stopping) {
                            /* never schedule another cycle during drain: an always-
                             * failing REQUEUE task would otherwise hang shutdown
                             * forever. (A cycle already parked in `delayed` is not
                             * touched here: it runs if its backoff ends before the
                             * grace does - or at all, with the grace off - and step
                             * 3b ends it otherwise.)
                             *
                             * The handle is still owed its terminal event. What
                             * happens here IS the dead-letter disposition, reached by
                             * a different road: an observer that only reconciles
                             * handles sees an ordinary DEAD_LETTERED and need not
                             * tell the two apart. Until this emit existed a REQUEUE
                             * item was the one shape that could reach shutdown and
                             * close in silence: gptps_await on it never returned, and
                             * every add-on that counts terminal events leaked a slot
                             * per item.
                             *
                             * Its status - on the event and the retained dead
                             * letter - is GPTPS_E_SHUTDOWN, as in step 3b, not the
                             * attempt's own: its FAILED already reported that, and
                             * what ends the item HERE is teardown refusing another
                             * cycle, not the policy running out. An observer that
                             * keeps work for the next run (durable_queue does) has
                             * to be able to tell this from a dead letter. */
                            it->outcome = GPTPS_E_SHUTDOWN;
                            if (npend < GPTPS_PENDING_CAP) {
                                pend[npend].kind = GPTPS_EV_DEAD_LETTERED; pend[npend].handle = it->handle;
                                ev_set_name(pend[npend].name, item_name(it)); pend[npend].status = GPTPS_E_SHUTDOWN;
                                pend[npend].attempt = it->attempt; pend[npend].mem = it->cost.mem_bytes;
                                pend[npend].result = NULL; pend[npend].result_len = 0;
                                pend[npend].flags = GPTPS_EV_FLAG_SHUTDOWN; ++npend;
                            }
                            dead_letter_push(e, it);
                        } else {
                            /* re-enqueue via delayed so retry_backoff is honored,
                             * floored so a zero backoff cannot busy-loop a core */
                            it->attempt = 1;
                            it->not_before_ms = requeue_at(now, it->policy.retry_backoff_seconds);
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
                            pend[npend].result = NULL; pend[npend].result_len = 0; pend[npend].flags = 0; ++npend;
                        }
                        item_drop(e, it);
                        break;
                    case GPTPS_ON_FAILURE_DEAD_LETTER:
                    default:
                        if (npend < GPTPS_PENDING_CAP) {
                            pend[npend].kind = GPTPS_EV_DEAD_LETTERED; pend[npend].handle = it->handle;
                            ev_set_name(pend[npend].name, item_name(it)); pend[npend].status = it->outcome;
                            pend[npend].attempt = it->attempt; pend[npend].mem = it->cost.mem_bytes;
                    pend[npend].result = NULL; pend[npend].result_len = 0; pend[npend].flags = 0; ++npend;
                        }
                        dead_letter_push(e, it);
                        break;
                }
            }
        }
        if (e->done.head) more = 1;      /* buffer filled before the queue emptied */

        /* (pending events from step 1 + admission below are emitted together,
         * after the admit step, with the lock released — see step 5) */

        /* 2) move backoff-ready delayed items back to intake (single scan) */
        {
            gptps_item *cur = e->delayed.head;
            while (cur) {
                gptps_item *nxt = cur->next;
                if (cur->not_before_ms <= now) {
                    /* A new attempt owes its own terminal event. `started` records
                     * that execute() already emitted STARTED + FINISHED/FAILED for
                     * THIS attempt; leaving it set across a re-admission makes the
                     * done-drain's `!it->started` test read attempt N-1's state, so
                     * an item cancelled while sitting in `ready` was freed silently.
                     * Every re-admission path - bounded retry, REQUEUE, service
                     * restart, constraint DEFER - funnels through `delayed`, so this
                     * is the one place that has to clear it. */
                    cur->started = 0;
                    fifo_remove(&e->delayed, cur);
                    intake_insert(e, cur);   /* back into ADMISSION order, not at the tail */
                } else {
                    next_wake = min_nonzero(next_wake, cur->not_before_ms);
                }
                cur = nxt;
            }
        }
        /* 2b) park the retries step 1 decided - only now, AFTER the promotion scan.
         * Their RETRIED sits in pend[], which is emitted with the lock released once
         * this pass returns, and both pumps (dispatcher_main, gptps_step) finish that
         * emit before they run another pass. So a retry is never promotable in the
         * pass that announced it, and with retry_backoff_seconds = 0 its next
         * attempt can no longer be admitted, started and reported ahead of the
         * RETRIED. A retry already due sets `more`, so the next pass promotes it at
         * once - that is what keeps gptps_step's pass B admitting it within the
         * same step, exactly as when it was promoted in this pass. Promoted here
         * it would also have been admitted in this pass, ahead of everything it
         * outranks, taking a slot and its memory: the due ones are tallied so
         * step 4 can keep back just that much and refill every other slot freed. */
        while ((it = fifo_pop(&announced)) != NULL) {
            fifo_push(&e->delayed, it);
            if (it->not_before_ms <= now) {                  /* zero backoff: due next pass */
                if (!held || it->sched_score > held_score) held_score = it->sched_score;
                held_mem = (it->cost.mem_bytes > UINT64_MAX - held_mem)
                         ? UINT64_MAX : held_mem + it->cost.mem_bytes;
                held += 1; more = 1;                         /* <= GPTPS_PENDING_CAP (step 1) */
            } else {
                next_wake = min_nonzero(next_wake, it->not_before_ms);
            }
        }

        /* 3) enforce deadlines on running tasks (cooperative cancel) */
        for (it = e->running_items.head; it; it = it->next) {
            if (it->deadline_ms) {
                if (now >= it->deadline_ms) cancel_raise(it);
                else next_wake = min_nonzero(next_wake, it->deadline_ms);
            }
        }

        /* 3b) bound the shutdown drain. Without this, one in-flight item that never
         * ends - a PROGRAM child ignoring its (absent) deadline, or a cooperative
         * body that never polls - keeps running_items non-empty and gptps_shutdown
         * never returns. Once the grace expires, ask everything still in flight to
         * stop: the enforced executors poll this flag and SIGKILL their child within
         * a 200ms slice, and a cooperative in-process body sees gptps_is_cancelled().
         * A body that ignores the flag entirely is unchanged - nothing can preempt
         * it in-process - but it is no longer the common case that hangs teardown. */
        if (e->stopping && e->stop_deadline_ms) {
            if (now >= e->stop_deadline_ms) {
                /* Retries parked in `delayed` are not "in flight", but the dispatcher
                 * refuses to exit while the queue is non-empty and step 2 only
                 * promotes an item once its backoff elapses - so a task with
                 * retry_backoff_seconds = 300 held teardown for five minutes no
                 * matter what the grace said. Past the deadline the backoff is moot:
                 * terminate the queue by policy, giving every item the terminal event
                 * it still owes (a retried item has only seen EV_RETRIED so far).
                 *   So is admission, and `intake` goes the same way. Step 2 above
                 * has just moved every parked item that fell due into it: one a
                 * constraint keeps DEFERring is due at every wake once the deadline
                 * has passed, since its own re-check is the only wake still armed,
                 * so it was never in `delayed` when this ran, and step 4 deferred it
                 * again - gptps_shutdown never returned. And queued work admitted
                 * now would only be started to be cancelled on the next pass, or,
                 * an in-process body that never polls, run to completion one item
                 * after another, unbounding the bound. Step 4 admits nothing past
                 * the deadline (`past_grace`). */
                while (npend < GPTPS_PENDING_CAP &&
                       ((it = fifo_pop(&e->delayed)) != NULL ||
                        (it = fifo_pop(&e->intake)) != NULL)) {
                    int drop = (it->policy.on_failure == GPTPS_ON_FAILURE_DROP);
                    it->outcome = GPTPS_E_SHUTDOWN;
                    pend[npend].kind = drop ? GPTPS_EV_DROPPED : GPTPS_EV_DEAD_LETTERED;
                    pend[npend].handle = it->handle;
                    ev_set_name(pend[npend].name, item_name(it));
                    pend[npend].status = GPTPS_E_SHUTDOWN;
                    pend[npend].attempt = it->attempt; pend[npend].mem = it->cost.mem_bytes;
                    pend[npend].result = NULL; pend[npend].result_len = 0;
                    pend[npend].flags = GPTPS_EV_FLAG_SHUTDOWN; ++npend;
                    if (drop) item_drop(e, it); else dead_letter_push(e, it);
                }
                intake_forget(e);                /* popped from the head: every cached tail may be gone */
                if (e->delayed.head || e->intake.head) more = 1;
                past_grace = 1;
                for (it = e->running_items.head; it; it = it->next) {
                    /* `cancelled` as well as the flag, exactly as gptps_cancel and
                     * stop_services do. Without it the forced stop is just another
                     * failed attempt: the done-drain takes the ordinary retry branch
                     * and re-admits the item with a freshly cleared cancel flag, so
                     * the grace made teardown (max_retries + 1) times LONGER instead
                     * of bounding it - and discarded a result the body had already
                     * produced. The grace is a bound only if it is terminal. */
                    it->cancelled = 1;
                    cancel_raise(it);
                }
            } else {
                next_wake = min_nonzero(next_wake, e->stop_deadline_ms);
            }
        }

        /* 4) admit in SCHEDULER order with skip-to-fit backfill + starvation guard.
         *    intake is already in admission order (see "intake ordering"), so `top`
         *    - the highest-score item overall - is simply the head, and `best` - the
         *    highest-score item that FITS the live budget - is the first item from
         *    the head that fits. Ties resolved to the older item, so FIFO holds
         *    within a score. When `best != top` we are about to skip the higher-score
         *    `top` because it does not fit yet: allowed (backfill) until `top` has
         *    been skipped reserve_after_skips times, after which we reserve for it -
         *    admit nothing and let running tasks drain until it fits (bounded). The
         *    ORDERING KEY (sched_score) is the one swappable policy: priority by
         *    default, or a scheduler hook's score; the skip-to-fit / budget /
         *    starvation MECHANISM stays fixed. */
        /* scheduler seam: with a custom ordering installed, (re)score every pending
         * item for this pass (scores may depend on time). The default ordering left
         * sched_score == priority, stamped at submit, so it needs no rescoring. */
        if (!past_grace && e->sched_fn && e->intake.head && e->running < e->limits.max_concurrent_tasks) {
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
            intake_resort(e);   /* the keys just changed, so the order has to be rebuilt */
        }
        while (!past_grace && e->intake.head && e->running < e->limits.max_concurrent_tasks) {
            gptps_item *best = NULL, *cur;
            gptps_item *top = e->intake.head;            /* ordered intake: the head IS `top` */
            uint64_t *snap = NULL;                       /* named-resource reservation snapshot */
            uint32_t retry_after = 0;
            gptps_admit_decision dec;

            /* Because intake is in admission order, the FIRST item that fits the live
             * budget is by construction the highest-scoring one that fits (and the
             * oldest at that score) - the item the old full-queue scan picked. Walking
             * only as far as that item is what makes admission O(1) in the common case
             * where the head fits, instead of O(queue depth) per admitted item. */
            {
                gptps_item *stranded = NULL;
                for (cur = e->intake.head; cur; cur = cur->next) {
                    if (res_never_fits(e, cur)) { stranded = cur; break; }
                    if (e->reserved_mem + cur->cost.mem_bytes <= e->limits.max_memory_bytes &&
                        res_fits(e, cur)) { best = cur; break; }
                }
                if (stranded) {
                    /* Never-fits after a runtime budget shrink: give it the terminal
                     * event submit() would have, with the lock still held so it is
                     * gone before the next scan. Same shape as the DENY path below;
                     * like it, stop while the item is still queued if pend[] is full. */
                    if (npend >= GPTPS_PENDING_CAP) { more = 1; break; }
                    intake_unlink(e, stranded);
                    stranded->outcome = GPTPS_E_BUDGET;
                    pend[npend].kind = GPTPS_EV_DEAD_LETTERED; pend[npend].handle = stranded->handle;
                    ev_set_name(pend[npend].name, item_name(stranded)); pend[npend].status = GPTPS_E_BUDGET;
                    pend[npend].attempt = stranded->attempt; pend[npend].mem = stranded->cost.mem_bytes;
                    pend[npend].result = NULL; pend[npend].result_len = 0; pend[npend].flags = 0; ++npend;
                    dead_letter_push(e, stranded);
                    continue;                            /* rescan: the head may have changed */
                }
            }

            if (!best) break;                            /* nothing fits the live budget now */
            /* Keep back for 2b's due retries what they would have taken had they
             * been promoted in this pass. In intake they would sit ahead of every
             * item they outrank (intake_insert puts an item after all of score >=
             * its own) and be admitted first, a slot and their memory each. So work
             * at or above the top retry's score goes ahead unconditionally - it was
             * queued in front of the retry anyway - and work below it goes ahead
             * while it still fits BESIDE them: the lower work that would have
             * started now either way. Only when it does not fit does admission
             * stop, and only for this pass: the next one runs as soon as the
             * RETRIED is out (`more`) and decides with the retries back in intake.
             * MANUAL mode always stops: nothing runs until gptps_step's pass loop
             * is over, so no slot can idle through the emit, and the next pass
             * then admits the retries ahead of the work they outrank - the order
             * the step runs `ready` in, as on a pass that promoted them itself.
             *   Best effort, and any error lasts one pass (`held` is rebuilt from
             * zero every pass). Conservative: every held retry is reserved against
             * anything the TOP one outranks, even one that would have queued behind
             * `best`; and a retry is reserved for even where it would not have been
             * admitted now (budget taken by work above it, a runtime budget cut, a
             * constraint hook that would DEFER or DENY it). Permissive: named
             * resource budgets are not reserved, so `best` can take units a retry
             * needs and delay it. The key is the retries' last-stamped score, exact
             * for the built-in priority ordering; a scheduler hook rescores them
             * next pass. The memory test is headroom, not a sum: `best` fits, so
             * reserved_mem + its bytes <= the budget and the subtraction cannot wrap. */
            if (held && best->sched_score < held_score &&
                (e->manual || held >= e->limits.max_concurrent_tasks - e->running ||
                 held_mem > e->limits.max_memory_bytes - e->reserved_mem - best->cost.mem_bytes))
                break;
            if (best != top && top->skips >= e->reserve_after_skips)
                break;                                   /* reserve for `top`: drain, admit nothing */

            dec = run_constraints(e, best, &retry_after); /* consulted only on the chosen item */
            if (dec == GPTPS_DEFER) {
                intake_unlink(e, best);
                best->not_before_ms = now + retry_after; /* re-check after the delay */
                fifo_push(&e->delayed, best);
                next_wake = min_nonzero(next_wake, best->not_before_ms);
                continue;
            }
            if (dec == GPTPS_DENY) {
                /* DENY does not raise e->running, so the admission loop can deny an
                 * entire (unbounded) intake queue in one pass. Stop while the item is
                 * still queued rather than dead-letter it with no terminal event. */
                if (npend >= GPTPS_PENDING_CAP) { more = 1; break; }
                intake_unlink(e, best);
                best->outcome = GPTPS_E_DENIED;          /* recorded for dead-letter drain */
                if (npend < GPTPS_PENDING_CAP) {
                    pend[npend].kind = GPTPS_EV_DEAD_LETTERED; pend[npend].handle = best->handle;
                    ev_set_name(pend[npend].name, item_name(best)); pend[npend].status = GPTPS_E_DENIED;
                    pend[npend].attempt = best->attempt; pend[npend].mem = best->cost.mem_bytes;
                    pend[npend].result = NULL; pend[npend].result_len = 0; pend[npend].flags = 0; ++npend;
                }
                dead_letter_push(e, best);               /* denied -> retained */
                continue;
            }

            /* Take the release snapshot BEFORE committing to the admission. It used
             * to be allocated after, and a NULL simply skipped BOTH the snapshot and
             * the `reserved +=` accounting - so under memory pressure the named
             * resource budget stopped being enforced at all while the item ran
             * anyway. Fail closed instead: leave the item queued and retry shortly. */
            if (e->nres && best->reg && best->reg->res_cost) {
                if (best->pooled) {        /* bounded: its own slot (no resource is new since the seal) */
                    snap = e->snap_arena + (size_t)(best - e->pool) * e->pool_nres;
                } else {
                    snap = (uint64_t *)gptps_malloc(e->nres * sizeof(uint64_t));
                    if (!snap) { next_wake = min_nonzero(next_wake, now + 50); break; }
                }
            }
            if (best != top) top->skips += 1;            /* charge the skipped higher-priority task */
            intake_unlink(e, best);
            e->reserved_mem += best->cost.mem_bytes;
            e->running      += 1;
            if (snap) {                                  /* reserve named resources + snapshot for release */
                size_t ri;
                best->res_reserved = snap;
                best->res_n = e->nres;
                for (ri = 0; ri < e->nres; ++ri) {
                    best->res_reserved[ri] = best->reg->res_cost[ri];
                    e->resources[ri].reserved += best->reg->res_cost[ri];
                }
            }
            fifo_push(&e->ready, best);
            gptps_cond_signal(e->cv_work);
        }

        /* 5) let the handle index shrink with the queues */
        hidx_trim(e);

        /* (the caller emits pend[] with the lock released, then re-runs a pass) */

    *out_npend = npend;
    *out_next_wake = next_wake;
    if (out_more) *out_more = more;
}

/* ------------------------------------------------------------------------- */
/* dispatcher thread (THREADED mode): engine_pass in a loop, emit, then sleep */
/* ------------------------------------------------------------------------- */

static void cfg_report_first_submit(gptps *e, int hints);

static void *dispatcher_main(void *arg)
{
    gptps *e = (gptps *)arg;
    gptps_pending_ev pend[GPTPS_PENDING_CAP];
    int npend, i;
    uint64_t next_wake;

    engine_note_own_thread(e);
    gptps_mutex_lock(e->m);
    for (;;) {
        int more = 0;
        if (e->cfg_report_due) {             /* asked for by the first submit */
            e->cfg_report_due = 0;
            gptps_mutex_unlock(e->m);
            cfg_report_first_submit(e, 0);
            gptps_mutex_lock(e->m);
            continue;
        }
        engine_pass(e, pend, &npend, &next_wake, &more);
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

        if (more) continue;          /* pend[] saturated: more terminal events are owed */

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
        case GPTPS_E_BUSY:      return "busy: refused rather than wait (work is outstanding, or the wait would need the calling thread)";
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
/* "0" means auto here as it does at open and in a config file (~0.75 of detected
 * RAM): a live 0 used to become a budget of zero bytes, so reloading a file that
 * says `max_memory_bytes = 0` dead-lettered every queued item that declares memory. */
static gptps_status sc_wr_maxmem(void *t, const char *v)
{
    gptps *e = (gptps *)t;
    uint64_t mem = (uint64_t)strtoull(v, NULL, 10);
    if (mem == 0) {
        gptps_limits l;
        if (gptps_config_resolve(NULL, &l) != GPTPS_OK) return GPTPS_E_CONFIG;
        mem = l.max_memory_bytes;
    }
    gptps_mutex_lock(e->m); e->limits.max_memory_bytes = mem; gptps_cond_signal(e->cv_disp); gptps_mutex_unlock(e->m);
    return GPTPS_OK;
}
/* Restart-only, and only that: the write is kept for reads and gptps_settings_save
 * (conc_next) and never reaches e->limits. It used to, and admission reads e->limits
 * - so a lowered value throttled the running engine, and a raised one admitted more
 * items than the pool has threads, which then sat in `ready` holding their memory and
 * named-resource budget, ahead of anything submitted later. "0" is kept as 0, auto
 * at the next open, as in a config file. */
static size_t       sc_rd_conc(void *t, char *b, size_t c) { gptps *e = (gptps *)t; size_t n; gptps_mutex_lock(e->m); n = rd_u32(b, c, e->conc_next); gptps_mutex_unlock(e->m); return n; }
static gptps_status sc_wr_conc(void *t, const char *v) { gptps *e = (gptps *)t; gptps_mutex_lock(e->m); e->conc_next = (uint32_t)strtoul(v, NULL, 10); gptps_mutex_unlock(e->m); return GPTPS_OK; }
static size_t       sc_rd_intake(void *t, char *b, size_t c) { gptps *e = (gptps *)t; size_t n; gptps_mutex_lock(e->m); n = rd_u32(b, c, e->limits.max_intake_depth); gptps_mutex_unlock(e->m); return n; }
static gptps_status sc_wr_intake(void *t, const char *v) { gptps *e = (gptps *)t; gptps_mutex_lock(e->m); e->limits.max_intake_depth = (uint32_t)strtoul(v, NULL, 10); gptps_cond_signal(e->cv_disp); gptps_mutex_unlock(e->m); return GPTPS_OK; }
static size_t       sc_rd_grace(void *t, char *b, size_t c) { gptps *e = (gptps *)t; size_t n; gptps_mutex_lock(e->m); n = rd_u32(b, c, e->shutdown_grace_ms); gptps_mutex_unlock(e->m); return n; }
static gptps_status sc_wr_grace(void *t, const char *v) { gptps *e = (gptps *)t; gptps_mutex_lock(e->m); e->shutdown_grace_ms = (uint32_t)strtoul(v, NULL, 10); gptps_mutex_unlock(e->m); return GPTPS_OK; }
static size_t       sc_rd_sample(void *t, char *b, size_t c) { gptps *e = (gptps *)t; return rd_u32(b, c, gptps_hal_load_acquire_u32(&e->sample_ms)); }
static gptps_status sc_wr_sample(void *t, const char *v) { gptps *e = (gptps *)t; gptps_hal_store_release_u32(&e->sample_ms, (uint32_t)strtoul(v, NULL, 10)); return GPTPS_OK; }
static size_t       sc_rd_dlcap(void *t, char *b, size_t c) { gptps *e = (gptps *)t; size_t n; gptps_mutex_lock(e->m); n = rd_u32(b, c, e->max_dead_letters); gptps_mutex_unlock(e->m); return n; }
static gptps_status sc_wr_dlcap(void *t, const char *v) { gptps *e = (gptps *)t; gptps_mutex_lock(e->m); e->max_dead_letters = (uint32_t)strtoul(v, NULL, 10); gptps_mutex_unlock(e->m); return GPTPS_OK; }
/* Count of dead-letter entries evicted by the cap. Writable so an operator can zero
 * it after acting on it; the point is that a capped list never truncates silently. */
static size_t       sc_rd_devict(void *t, char *b, size_t c) { gptps *e = (gptps *)t; size_t n; gptps_mutex_lock(e->m); n = rd_u64(b, c, e->dead_evicted); gptps_mutex_unlock(e->m); return n; }
static gptps_status sc_wr_devict(void *t, const char *v) { gptps *e = (gptps *)t; gptps_mutex_lock(e->m); e->dead_evicted = (uint64_t)strtoull(v, NULL, 10); gptps_mutex_unlock(e->m); return GPTPS_OK; }
/* bounded.*: restart-only. The pool is made once, so a write is for the next open
 * (and for gptps_settings_save), like limits.max_concurrent_tasks. */
static size_t       sb_rd_items(void *t, char *b, size_t c)   { gptps *e = (gptps *)t; size_t n; gptps_mutex_lock(e->m); n = rd_u64(b, c, e->bnext_items); gptps_mutex_unlock(e->m); return n; }
static gptps_status sb_wr_items(void *t, const char *v)        { gptps *e = (gptps *)t; gptps_mutex_lock(e->m); e->bnext_items = (uint64_t)strtoull(v, NULL, 10); gptps_mutex_unlock(e->m); return GPTPS_OK; }
static size_t       sb_rd_payload(void *t, char *b, size_t c) { gptps *e = (gptps *)t; size_t n; gptps_mutex_lock(e->m); n = rd_u32(b, c, e->bnext_payload); gptps_mutex_unlock(e->m); return n; }
static gptps_status sb_wr_payload(void *t, const char *v)      { gptps *e = (gptps *)t; gptps_mutex_lock(e->m); e->bnext_payload = (uint32_t)strtoul(v, NULL, 10); gptps_mutex_unlock(e->m); return GPTPS_OK; }
static size_t       sb_rd_result(void *t, char *b, size_t c)  { gptps *e = (gptps *)t; size_t n; gptps_mutex_lock(e->m); n = rd_u32(b, c, e->bnext_result); gptps_mutex_unlock(e->m); return n; }
static gptps_status sb_wr_result(void *t, const char *v)       { gptps *e = (gptps *)t; gptps_mutex_lock(e->m); e->bnext_result = (uint32_t)strtoul(v, NULL, 10); gptps_mutex_unlock(e->m); return GPTPS_OK; }

/* Resources as settings: resources.<name> is the budget, tasks.<task>.resources.<name>
 * what one run of the task costs. A budget shrink may strand queued work that the
 * admission scan must dead-letter, and a raise may admit it, so both wake the
 * dispatcher - as gptps_define_resource's re-budget does. */
static size_t rs_rd_budget(void *t, char *b, size_t c)
{ gptps_res_cell *x = (gptps_res_cell *)t; size_t n; gptps_mutex_lock(x->e->m); n = rd_u64(b, c, x->e->resources[x->ri].budget); gptps_mutex_unlock(x->e->m); return n; }
static gptps_status rs_wr_budget(void *t, const char *v)
{ gptps_res_cell *x = (gptps_res_cell *)t; gptps_mutex_lock(x->e->m); x->e->resources[x->ri].budget = (uint64_t)strtoull(v, NULL, 10); gptps_cond_signal(x->e->cv_disp); gptps_mutex_unlock(x->e->m); return GPTPS_OK; }
static size_t rs_rd_cost(void *t, char *b, size_t c)
{ gptps_res_cell *x = (gptps_res_cell *)t; size_t n; gptps_mutex_lock(x->e->m); n = rd_u64(b, c, x->r->res_cost ? x->r->res_cost[x->ri] : 0); gptps_mutex_unlock(x->e->m); return n; }
static gptps_status rs_wr_cost(void *t, const char *v)
{ gptps_res_cell *x = (gptps_res_cell *)t; gptps_mutex_lock(x->e->m); if (x->r->res_cost) x->r->res_cost[x->ri] = (uint64_t)strtoull(v, NULL, 10); gptps_cond_signal(x->e->cv_disp); gptps_mutex_unlock(x->e->m); return GPTPS_OK; }

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

/* 1 if the setting could not be registered. An engine without one of its own settings
 * refuses that key in every config file and every live set, so open fails instead. */
static int reg_core_setting(gptps *e, const char *key, gptps_setting_type type, int hot,
                            int has_range, double mn, double mx, const char *desc,
                            size_t (*rd)(void *, char *, size_t), gptps_status (*wr)(void *, const char *))
{
    gptps_setting_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.key = key; d.type = type; d.hot = hot;
    d.has_range = has_range; d.min = mn; d.max = mx; d.desc = desc;
    d.target = e; d.read = rd; d.write = wr;
    return gptps_settings_add(e->settings, &d) != GPTPS_OK;
}

/* The per-task keys, with the ranges of the fields behind them: one table for the
 * live settings and the config file, so whatever either accepts fits its field. The
 * live settings used to declare no range at all, so tasks.<t>.max_retries =
 * 99999999999 was accepted, then truncated by the cast into a uint32_t. */
typedef struct {
    const char        *leaf;
    gptps_setting_type type;
    int                has_range;
    double             min, max;
    const char *const *choices;
    const char        *desc;
    size_t       (*rd)(void *, char *, size_t);
    gptps_status (*wr)(void *, const char *);
} task_key;

static const task_key TASK_KEYS[] = {
    { "timeout_seconds",       GPTPS_SETTING_UINT, 1, 0, 4294967295.0, NULL,
      "seconds one attempt may run before it is timed out; 0 = no limit", st_rd_timeout, st_wr_timeout },
    { "max_retries",           GPTPS_SETTING_UINT, 1, 0, 4294967295.0, NULL,
      "attempts after the first, before on_failure applies; 0 = none", st_rd_retries, st_wr_retries },
    { "retry_backoff_seconds", GPTPS_SETTING_UINT, 1, 0, 4294967295.0, NULL,
      "seconds from a failed attempt to the next; 0 = at once (a requeue still waits 100 ms)", st_rd_backoff, st_wr_backoff },
    { "mem_bytes",             GPTPS_SETTING_UINT, 0, 0, 0, NULL,
      "memory one run declares, in bytes: admission budgets it, the process executors cap it; "
      "0 = none declared: not budgeted, not capped", st_rd_mem, st_wr_mem },
    { "priority",              GPTPS_SETTING_INT,  1, -2147483648.0, 2147483647.0, NULL,
      "admission order: higher runs first; may be negative", st_rd_prio, st_wr_prio },
    { "on_failure",            GPTPS_SETTING_ENUM, 0, 0, 0, ONFAIL_CHOICES,
      "once the retries are spent: dead_letter keeps the item, requeue starts it over, drop discards it",
      st_rd_onfail, st_wr_onfail },
};
#define N_TASK_KEYS (sizeof TASK_KEYS / sizeof TASK_KEYS[0])

static const task_key *task_key_find(const char *leaf)
{
    size_t i;
    for (i = 0; i < N_TASK_KEYS; ++i) if (strcmp(TASK_KEYS[i].leaf, leaf) == 0) return &TASK_KEYS[i];
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* generic settings: engine-stored global knobs + per-task setting schemas    */
/* ------------------------------------------------------------------------- */

/* A default as given, less any white space around it - for anything but a string,
 * as a live set reads what is typed (settings.c): " 5" is 5. */
static const char *trim_default(gptps_setting_type type, const char *v, char *buf, size_t cap)
{
    size_t n;
    if (type == GPTPS_SETTING_STRING || !v || strlen(v) >= cap) return v;
    while (*v && strchr(" \t\r\n\v\f", *v)) ++v;
    n = strlen(v);
    while (n && strchr(" \t\r\n\v\f", v[n - 1])) --n;
    memcpy(buf, v, n); buf[n] = 0;
    return buf;
}

/* Validate a value against a schema the same way the registry does, so a bad
 * default_val is rejected at define time (returns 1 ok, 0 invalid). */
static int gval_ok(gptps_setting_type type, int has_range, double mn, double mx,
                   const char *const *choices, const char *v)
{
    char *end;
    /* Every engine-owned type stores its textual representation in one fixed
     * cell, not just STRING. Reject input loss before accepting a default. */
    if (!v || strlen(v) >= GPTPS_SETTINGS_VALUE_MAX) return 0;
    /* the number grammar valid_value() holds every write to: no spaces, hex, inf
     * or nan - nothing a save would write that the file could not read back */
    if ((type == GPTPS_SETTING_INT || type == GPTPS_SETTING_UINT || type == GPTPS_SETTING_DOUBLE) &&
        !gptps_settings_plain_number(v, type != GPTPS_SETTING_DOUBLE))
        return 0;
    switch (type) {
        /* errno, not just the end pointer: strtoll/strtoull SATURATE at their
         * limits and report it only through ERANGE, so without this a nonsense
         * default was accepted and silently became LLONG_MAX / ULLONG_MAX. Mirrors
         * valid_value() in settings.c, which validates the same grammar at write
         * time - the two must agree or a default is accepted that no later write to
         * the same key could reproduce. */
        case GPTPS_SETTING_INT: {
            long long x;
            errno = 0;
            x = strtoll(v, &end, 10);
            if (end == v || *end) return 0;
            if (errno == ERANGE) return 0;
            return !(has_range && ((double)x < mn || (double)x > mx));
        }
        case GPTPS_SETTING_UINT: {
            unsigned long long x; const char *p = v;
            while (*p == ' ' || *p == '\t') ++p;
            if (*p == '-') return 0;
            errno = 0;
            x = strtoull(v, &end, 10);
            if (end == v || *end) return 0;
            if (errno == ERANGE) return 0;
            return !(has_range && ((double)x < mn || (double)x > mx));
        }
        case GPTPS_SETTING_DOUBLE: {
            double x = gptps_strtod_c(v, &end);
            if (end == v || *end) return 0;
            if (!(x == x) || x > DBL_MAX || x < -DBL_MAX) return 0;   /* 1e999 is inf */
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
    *mn = gptps_strtod_c(c, NULL);
    *mx = gptps_strtod_c(dd + 2, NULL);
    *has_range = 1;
    return 1;
}

/* parse "a|b|c" into a fresh NULL-terminated, gptps_malloc'd choices array
 * (caller owns). Returns NULL on empty/none, and out of memory. */
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
static gptps_status os_wr(void *t, const char *v)
{
    gptps_owned_setting *o = (gptps_owned_setting *)t;
    if (strlen(v) >= sizeof o->value) return GPTPS_E_CONFIG;
    snprintf(o->value, sizeof o->value, "%s", v);
    return GPTPS_OK;
}

/* ---- generic PER-TASK setting instance (target = gptps_task_local*) ----
 * Locks the engine mutex (consistent with the built-in per-task knobs). */
static size_t       stl_rd(void *t, char *b, size_t c) { gptps_task_local *L = (gptps_task_local *)t; size_t n; gptps_mutex_lock(L->reg->engine->m); n = (size_t)snprintf(b, c, "%s", L->value); gptps_mutex_unlock(L->reg->engine->m); return n; }
static gptps_status stl_wr(void *t, const char *v)
{
    gptps_task_local *L = (gptps_task_local *)t;
    if (strlen(v) >= sizeof L->value) return GPTPS_E_CONFIG;
    gptps_mutex_lock(L->reg->engine->m);
    snprintf(L->value, sizeof L->value, "%s", v);
    gptps_mutex_unlock(L->reg->engine->m);
    return GPTPS_OK;
}

/* ---- a call's settings: all made, then published at once ----------------------
 * A task's registration, gptps_define_resource and gptps_define_task_setting each make
 * settings - one per task, or several per task. Added to the registry one at a time,
 * a call that ran out of memory part-way either returned GPTPS_OK without the rest -
 * a setting that was missing took no value from the config file and refused every
 * live set - or had to take back what other threads could already see and use. So
 * each call makes every setting it needs first (gptps_settings_prepare: nothing can see
 * one yet), and publishes them all in one critical section, under the settings lock
 * and then e->m, after its last allocation: publishing cannot fail part-way. One that
 * cannot be made frees what was made, and nothing else has changed. A key someone else holds already - a host's own
 * setting under the same name - stays theirs, as it always did, and the cell made
 * for it is freed. */

/* tasks.<r>.<leaf>, a built-in knob. NULL: out of memory. Reads r's value: no lock held. */
static gptps_setting_prep *task_key_prepare(gptps_reg *r, const task_key *k)
{
    gptps_setting_def d;
    gptps_setting_prep *p;
    char key[320];
    memset(&d, 0, sizeof d);
    snprintf(key, sizeof key, "tasks.%s.%s", r->name, k->leaf);
    d.struct_size = sizeof d; d.key = key; d.type = k->type; d.hot = 1; d.desc = k->desc;
    d.has_range = k->has_range; d.min = k->min; d.max = k->max;
    d.choices = k->choices; d.target = r; d.read = k->rd; d.write = k->wr;
    return gptps_settings_prepare(&d, r, NULL, &p) == GPTPS_OK ? p : NULL;
}

/* One generic per-task schema on `r`: tasks.<r>.<leaf>, bound to a cell of its own
 * that holds the value. NULL: out of memory. */
static gptps_task_local *task_local_prepare(gptps_reg *r, const gptps_task_schema *sc)
{
    gptps_task_local *L;
    gptps_setting_def d;
    char key[320];
    L = (gptps_task_local *)gptps_calloc(1, sizeof *L);
    if (!L) return NULL;
    L->schema = sc; L->reg = r;
    snprintf(L->value, sizeof L->value, "%s", sc->defval ? sc->defval : "");
    memset(&d, 0, sizeof d);
    snprintf(key, sizeof key, "tasks.%s.%s", r->name, sc->leaf);
    d.struct_size = sizeof d; d.key = key; d.type = sc->type; d.hot = sc->hot; d.desc = "per-task setting";
    d.has_range = sc->has_range; d.min = sc->min; d.max = sc->max;
    d.choices = (const char *const *)sc->choices;
    d.target = L; d.read = stl_rd; d.write = stl_wr;
    if (gptps_settings_prepare(&d, r, L->value, &L->prep) != GPTPS_OK) { gptps_free(L); return NULL; }
    return L;
}

/* A named resource's setting, so the config file, the dashboard and gptps_settings_set
 * reach it like any other knob: resources.<rname>, its budget (`r` NULL), or
 * tasks.<r>.resources.<rname>, what one run of r holds of it. `defval` NULL reads the
 * value, which needs `ri` to be a resource already published. Not made for a resource
 * an add-on defined: the add-on exposes it its own way (gpu_quota_plugin's
 * gpuq.total_units), and two settings would fight over one budget. NULL: out of
 * memory. */
static gptps_res_cell *res_cell_prepare(gptps *e, gptps_reg *r, size_t ri, const char *rname,
                                        const char *defval)
{
    gptps_res_cell *x;
    gptps_setting_def d;
    char key[384];
    x = (gptps_res_cell *)gptps_calloc(1, sizeof *x);
    if (!x) return NULL;
    x->e = e; x->r = r; x->ri = ri;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.key = key; d.type = GPTPS_SETTING_UINT; d.hot = 1;
    if (r) {
        snprintf(key, sizeof key, "tasks.%s.resources.%s", r->name, rname);
        d.desc = "how much of the named resource one run of this task holds while it runs; 0 = none";
        d.read = rs_rd_cost; d.write = rs_wr_cost;
    } else {
        snprintf(key, sizeof key, "resources.%s", rname);
        d.desc = "a named resource's budget: how much of it the running tasks may hold at once; "
                 "0 = none: a submit that costs any is refused, and queued work that does is dead-lettered "
                 "(GPTPS_E_BUDGET) - not a pause";
        d.read = rs_rd_budget; d.write = rs_wr_budget;
    }
    d.target = x;
    if (gptps_settings_prepare(&d, r, defval, &x->prep) != GPTPS_OK) { gptps_free(x); return NULL; }
    return x;
}

/* Made, and not published: freed, with their settings. */
static void locals_free(gptps_task_local *L)
{
    while (L) { gptps_task_local *n = L->next; gptps_settings_prep_free(L->prep); gptps_free(L); L = n; }
}
static void res_cells_free(gptps_res_cell *x)
{
    while (x) { gptps_res_cell *n = x->next; gptps_settings_prep_free(x->prep); gptps_free(x); x = n; }
}

/* Publish a made cell's setting, and give the cell to its task (or to `*list`) - or,
 * when the key is someone else's, free both. Caller holds the settings lock, then e->m. */
static void local_publish(gptps *e, gptps_task_local *L)
{
    if (gptps_settings_publish_locked(e->settings, L->prep)) {
        L->prep = NULL;
        L->next = L->reg->locals; L->reg->locals = L;
    } else {
        L->next = NULL;
        locals_free(L);
    }
}
static void res_cell_publish(gptps *e, gptps_res_cell *x, gptps_res_cell **list)
{
    if (gptps_settings_publish_locked(e->settings, x->prep)) {
        x->prep = NULL;
        x->next = *list; *list = x;
    } else {
        x->next = NULL;
        res_cells_free(x);
    }
}

/* A registration's settings, made and not yet published. */
typedef struct {
    gptps_setting_prep *keys[N_TASK_KEYS];   /* the built-in knobs */
    gptps_task_local   *locals, **lt;        /* one per generic per-task setting */
    gptps_res_cell     *costs, **xt;         /* one per named resource the host defined */
    const gptps_task_schema *sc_done;        /* the schemas made for: from the list's head here */
    size_t              nres_done;           /* the resources made for: those below it */
} task_prep;

static void task_prep_free(task_prep *tp)
{
    size_t i;
    for (i = 0; i < N_TASK_KEYS; ++i) gptps_settings_prep_free(tp->keys[i]);
    locals_free(tp->locals);
    res_cells_free(tp->costs);
    memset(tp, 0, sizeof *tp);
}

/* Make what a new type needs for the definitions published since tp was last made
 * for: a setting for each generic per-task schema from the list's head `sc` down to
 * tp->sc_done, and a cost for each named resource the host defined from
 * tp->nres_done up to `nres`. No lock held. GPTPS_E_NOMEM if one could not be made;
 * what was made is in *tp either way. */
static gptps_status task_prepare_defs(gptps *e, gptps_reg *r, const gptps_task_schema *sc, size_t nres,
                                      task_prep *tp)
{
    const gptps_task_schema *stop = tp->sc_done;
    size_t i;
    /* Walked without the lock: a schema is prepended to the list, its `next` is set
     * before it is published and never changes, and none is freed before shutdown. */
    for (; sc && sc != stop; sc = sc->next) {
        if (!(*tp->lt = task_local_prepare(r, sc))) return GPTPS_E_NOMEM;
        tp->lt = &(*tp->lt)->next;
    }
    for (i = tp->nres_done; i < nres; ++i) {
        const char *rname;
        int addon;
        gptps_mutex_lock(e->m);       /* e->resources moves when it grows; a name never does */
        rname = e->resources[i].name; addon = e->resources[i].addon;
        gptps_mutex_unlock(e->m);
        if (addon) continue;
        if (!(*tp->xt = res_cell_prepare(e, r, i, rname, NULL))) return GPTPS_E_NOMEM;
        tp->xt = &(*tp->xt)->next;
    }
    return GPTPS_OK;
}

/* Make every setting a new type has, then publish them all in one critical section:
 * its built-in knobs, one for each generic per-task setting, and a cost for each
 * named resource the host defined. No lock held. A definition published while they
 * were made passed this type over (it was not published yet), so before publishing,
 * under both locks, the lists are checked again, and what was defined meanwhile is
 * made too, with the locks released, until nothing new has come. GPTPS_E_NOMEM if one
 * could not be made: then nothing was published, and nothing is left to free. */
static gptps_status task_settings(gptps *e, gptps_reg *r, const gptps_task_schema *sc0, size_t nres0)
{
    task_prep tp;
    gptps_task_local *L;
    gptps_res_cell *x;
    size_t i;
    memset(&tp, 0, sizeof tp);
    tp.lt = &tp.locals; tp.xt = &tp.costs;
    for (i = 0; i < N_TASK_KEYS; ++i)
        if (!(tp.keys[i] = task_key_prepare(r, &TASK_KEYS[i]))) { task_prep_free(&tp); return GPTPS_E_NOMEM; }
    for (;;) {
        if (task_prepare_defs(e, r, sc0, nres0, &tp) != GPTPS_OK) { task_prep_free(&tp); return GPTPS_E_NOMEM; }
        tp.sc_done = sc0; tp.nres_done = nres0;
        gptps_settings_lock(e->settings);
        gptps_mutex_lock(e->m);
        if (e->task_schemas == sc0 && e->nres == nres0) break;   /* both locks held */
        sc0 = e->task_schemas; nres0 = e->nres;
        gptps_mutex_unlock(e->m);
        gptps_settings_unlock(e->settings);
    }
    for (i = 0; i < N_TASK_KEYS; ++i) {
        if (!gptps_settings_publish_locked(e->settings, tp.keys[i])) gptps_settings_prep_free(tp.keys[i]);
        tp.keys[i] = NULL;
    }
    while ((L = tp.locals) != NULL) { tp.locals = L->next; local_publish(e, L); }
    while ((x = tp.costs) != NULL)  { tp.costs = x->next;  res_cell_publish(e, x, &r->res_cells); }
    r->published = true;              /* a definition from now on makes its own setting for it */
    gptps_mutex_unlock(e->m);
    gptps_settings_unlock(e->settings);
    return GPTPS_OK;
}

/* A gptps_config limit whose 0 means "not set" (ABI 2.4): 0 keeps `dflt`,
 * GPTPS_LIMIT_NONE is the engine's 0 ("no limit"), and anything else is the limit. */
static uint32_t open_limit(uint32_t v, uint32_t dflt)
{
    if (v == 0) return dflt;
    return v == GPTPS_LIMIT_NONE ? 0u : v;
}

/* The engine itself, from a config whose file (if any) has already been read. */
static gptps_status open_engine(const gptps_config *cfg, gptps **out_engine)
{
    gptps *e;
    gptps_status s;
    const gptps_limits *in = cfg ? &cfg->limits : NULL;
    unsigned i, lost = 0;           /* core settings that could not be registered */

    if (!out_engine) return GPTPS_E_INVAL;
    if (cfg && cfg->struct_size < GPTPS_CONFIG_MIN_SIZE) return GPTPS_E_INVAL; /* ABI: append-safe floor */
    *out_engine = NULL;

    /* Idempotent; makes a host fork() detectable so the child fails loudly rather
     * than deadlocking on a mutex its parent's threads left locked. */
    gptps_hal_fork_guard_install();

    e = (gptps *)gptps_calloc(1, sizeof *e);
    if (!e) return GPTPS_E_NOMEM;
    e->fork_gen = gptps_hal_fork_generation();   /* refuse this engine after a fork */

    s = gptps_config_resolve(in, &e->limits);
    if (s != GPTPS_OK) { gptps_free(e); return s; }
    e->conc_next = e->limits.max_concurrent_tasks;

    e->next_handle = 1;
    e->intake.id = GPTPS_Q_INTAKE;   e->delayed.id = GPTPS_Q_DELAYED;
    e->ready.id  = GPTPS_Q_READY;    e->running_items.id = GPTPS_Q_RUNNING;
    e->done.id   = GPTPS_Q_DONE;     e->dead_letter.id = GPTPS_Q_DEAD;
    e->reserve_after_skips = GPTPS_RESERVE_AFTER;
    e->shutdown_grace_ms   = GPTPS_SHUTDOWN_GRACE_MS_DEFAULT;
    e->max_dead_letters    = GPTPS_MAX_DEAD_LETTERS_DEFAULT;
    e->m = gptps_mutex_create();
    e->cv_disp = gptps_cond_create();
    e->cv_work = gptps_cond_create();
    e->cv_drain = gptps_cond_create();
    e->settings = gptps_settings_create();
    if (!e->m || !e->cv_disp || !e->cv_work || !e->cv_drain || !e->settings) { s = GPTPS_E_NOMEM; goto fail; }

    /* core settings (read live engine state; hot ones apply immediately) */
    lost += reg_core_setting(e, "limits.max_memory_bytes", GPTPS_SETTING_UINT, 1, 0, 0, 0,
                             "memory the running tasks may declare at once, in bytes (each task's mem_bytes); admission waits for room. 0 = 3/4 of the machine's memory", sc_rd_maxmem, sc_wr_maxmem);
    /* 0 is in range: it means auto, as in the file - and reloading a file that says
     * so (the shipped gptps.example.toml does) used to fail with GPTPS_E_CONFIG. */
    lost += reg_core_setting(e, "limits.max_concurrent_tasks", GPTPS_SETTING_UINT, 0, 1, 0, 65536,
                             "tasks that run at once: the worker pool. 0 = one per logical CPU", sc_rd_conc, sc_wr_conc);
    /* has_range is not decoration on these four: each write callback casts to
     * uint32_t, so without a declared ceiling "4294967296" validated fine and then
     * truncated to 0 - which for max_intake_depth means the bound the operator just
     * set silently became "unbounded". The range makes the setting refuse instead. */
    lost += reg_core_setting(e, "limits.max_intake_depth", GPTPS_SETTING_UINT, 1, 1, 0, 4294967295.0,
                             "items that may wait to be admitted; past it gptps_submit returns GPTPS_E_FULL. 0 = no limit", sc_rd_intake, sc_wr_intake);
    lost += reg_core_setting(e, "limits.shutdown_grace_ms", GPTPS_SETTING_UINT, 1, 1, 0, 4294967295.0,
                             "how long gptps_shutdown lets running work finish before it cancels it, in ms. 0 = wait forever", sc_rd_grace, sc_wr_grace);
    lost += reg_core_setting(e, "limits.max_dead_letters", GPTPS_SETTING_UINT, 1, 1, 0, 4294967295.0,
                             "dead letters kept; past it the oldest is dropped and counted in stats.dead_letters_evicted. 0 = no limit", sc_rd_dlcap, sc_wr_dlcap);
    lost += reg_core_setting(e, "stats.dead_letters_evicted", GPTPS_SETTING_UINT, 1, 0, 0, 0,
                             "dead letters dropped because limits.max_dead_letters was reached; write 0 to reset it", sc_rd_devict, sc_wr_devict);
    gptps_settings_nosave(e->settings, "stats.dead_letters_evicted");   /* a count, not configuration */
    lost += reg_core_setting(e, "measure.sample_ms", GPTPS_SETTING_UINT, 1, 1, 0, 3600000,
                             "how often a running process job reports its current memory, as a GPTPS_EV_SAMPLE event, "
                             "in ms (under 10 counts as 10; applies to jobs started after a change; docs/MEASUREMENTS.md). "
                             "0 = never: no SAMPLE events", sc_rd_sample, sc_wr_sample);
    lost += reg_core_setting(e, "scheduler.reserve_after_skips", GPTPS_SETTING_UINT, 1, 1, 0, 4294967295.0,
                             "times smaller work may pass a waiting top-priority task that does not fit, before the scheduler holds room for it. 0 = strict priority order", sc_rd_resv, sc_wr_resv);
    lost += reg_core_setting(e, "bounded.max_items", GPTPS_SETTING_UINT, 0, 1, 0, 4294967295.0,
                             "items alive at once in bounded mode: queued, retrying, running, dead-lettered "
                             "(docs/BOUNDED.md); 0 = the classic engine", sb_rd_items, sb_wr_items);
    lost += reg_core_setting(e, "bounded.max_payload_bytes", GPTPS_SETTING_UINT, 0, 1, 0, 4294967295.0,
                             "bounded mode: each item's payload slot; a longer payload is refused; 0 = only an empty payload fits",
                             sb_rd_payload, sb_wr_payload);
    lost += reg_core_setting(e, "bounded.max_result_bytes", GPTPS_SETTING_UINT, 0, 1, 0, 4294967295.0,
                             "bounded mode: each executing thread's result buffer; a longer result is refused; "
                             "0 = only an empty result fits",
                             sb_rd_result, sb_wr_result);
    if (lost) { s = GPTPS_E_NOMEM; goto fail; }

    if (cfg && cfg->config_path) {   /* remember the open path for save/reload defaults */
        size_t L = strlen(cfg->config_path) + 1;
        e->config_path = (char *)gptps_malloc(L);
        /* without it, save and reload with no path would refuse with GPTPS_E_INVAL */
        if (!e->config_path) { s = GPTPS_E_NOMEM; goto fail; }
        memcpy(e->config_path, cfg->config_path, L);
    }

    /* Bounded mode (ABI 2.4): read only what the caller's struct has. */
    if (cfg && GPTPS_STRUCT_HAS(gptps_config, cfg, max_items)) e->max_items = cfg->max_items;
    if (e->max_items) {
        if (GPTPS_STRUCT_HAS(gptps_config, cfg, max_payload_bytes)) e->max_payload = cfg->max_payload_bytes;
        if (GPTPS_STRUCT_HAS(gptps_config, cfg, max_result_bytes))  e->max_result  = cfg->max_result_bytes;
        e->pool_m = gptps_mutex_create();
        if (!e->pool_m) { s = GPTPS_E_NOMEM; goto fail; }
    }
    e->bnext_items = e->max_items; e->bnext_payload = e->max_payload; e->bnext_result = e->max_result;

    /* ABI 2.4: the two limits whose 0 means something of its own, read only when the
     * caller's struct has them. With a config file, cfg_open_keys has already left
     * the file's value out if the struct sets one, and cfg_apply sets it otherwise. */
    if (cfg && GPTPS_STRUCT_HAS(gptps_config, cfg, max_dead_letters))
        e->max_dead_letters = open_limit(cfg->max_dead_letters, e->max_dead_letters);
    if (cfg && GPTPS_STRUCT_HAS(gptps_config, cfg, shutdown_grace_ms))
        e->shutdown_grace_ms = open_limit(cfg->shutdown_grace_ms, e->shutdown_grace_ms);

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
        e->worker_args = (gptps_worker *)gptps_calloc(e->nworkers, sizeof *e->worker_args);
        if (!e->worker_args) { s = GPTPS_E_NOMEM; goto fail; }

        /* Room for every thread this engine owns (dispatcher + workers) to record
         * its own id, so gptps_shutdown can refuse a call made from one of them. */
        e->cap_owned_tids = e->nworkers + 1;
        e->owned_tids = (uint64_t *)gptps_calloc(e->cap_owned_tids, sizeof *e->owned_tids);
        if (!e->owned_tids) { s = GPTPS_E_NOMEM; goto fail; }

        e->dispatcher = gptps_thread_start(dispatcher_main, e);
        if (!e->dispatcher) { s = GPTPS_E_NOMEM; goto fail; }
        for (i = 0; i < e->nworkers; ++i) {
            e->worker_args[i].e = e;
            e->worker_args[i].idx = (unsigned)i;
            e->workers[i] = gptps_thread_start(worker_main, &e->worker_args[i]);
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
    gptps_free(e->config_path);   /* copied before the workers/threads that failed */
    if (e->settings) gptps_settings_destroy(e->settings);
    if (e->workers) gptps_free(e->workers);
    gptps_free(e->worker_args);
    if (e->pool_m) gptps_mutex_destroy(e->pool_m);
    if (e->owned_tids) gptps_free(e->owned_tids);
    if (e->cv_drain) gptps_cond_destroy(e->cv_drain);
    if (e->cv_work) gptps_cond_destroy(e->cv_work);
    if (e->cv_disp) gptps_cond_destroy(e->cv_disp);
    if (e->m) gptps_mutex_destroy(e->m);
    gptps_free(e);
    return s;
}

/* Apply a single config table's task overrides onto a task def. Values present
 * in the file override the def's compiled-in defaults (file wins). */
/* ------------------------------------------------------------------------- */
/* the config file (docs/CONFIG.md)                                          */
/* ------------------------------------------------------------------------- */
/* One validation path. A value from the file passes exactly the checks a live
 * gptps_settings_set makes - the same parser, ranges and choices - and every
 * problem names the file, the line, the key and what is wrong, through the log
 * sink. What the engine can judge at open fails the open: a line that does not
 * parse, a value out of range or of the wrong type, a key no engine-owned table
 * has. A key only a later definition can claim - a task registered after open, a
 * plug-in's or the host's own setting - waits for it: gptps_config_check()
 * reports whatever nothing claimed, and the first submit logs it, once.
 *
 * The file used to be read leniently and cast. A line it could not parse was
 * skipped, `max_retries = -1` became 4294967295 retries and `priority =
 * 3000000000` wrapped to -1294967296 - while the same values set live were
 * refused. */

/* One entry, copied out of the file: a reload may swap and free the file while a
 * value from it is being applied. Its key always fits: a longer one is refused before
 * any entry is copied out (cfg_refuse_long_keys). */
#define CFG_KEY_MAX 384           /* a key's bytes, with its NUL; no setting's key is longer */
typedef struct {
    char key[CFG_KEY_MAX];
    char text[GPTPS_SETTINGS_VALUE_MAX];
    char path[256];
    int  line;
    int  usable;      /* a single value whose text fits `text` */
    gptps_toml_kind kind;   /* as the file wrote it: a number, true/false, a "string", a list */
    size_t index;     /* its place in the file it was copied from */
} cfg_item;

static void cfg_item_at(const gptps_toml *t, size_t i, cfg_item *it)
{
    const char *x = gptps_toml_text_at(t, i);
    it->index = i;
    gptps_toml_dotted_at(t, i, it->key, sizeof it->key);
    snprintf(it->path, sizeof it->path, "%s", gptps_toml_path(t));
    it->line = gptps_toml_line_at(t, i);
    it->kind = gptps_toml_kind_at(t, i);
    it->usable = x && strlen(x) < sizeof it->text;
    snprintf(it->text, sizeof it->text, "%s", x ? x : "");
}

static void cfg_say(gptps_log_level lvl, const cfg_item *it, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    int k = snprintf(msg, sizeof msg, "config %s:%d: %s: ", it->path, it->line, it->key);
    if (k < 0 || (size_t)k >= sizeof msg) k = 0;
    va_start(ap, fmt);
    vsnprintf(msg + k, sizeof msg - (size_t)k, fmt, ap);
    va_end(ap);
    gptps_log(NULL, lvl, msg);
}

/* The parser's messages, one per line of `err`, each logged as an error. */
static void cfg_say_parse(const char *err)
{
    const char *p = err;
    while (*p) {
        const char *nl = strchr(p, '\n');
        int n = nl ? (int)(nl - p) : (int)strlen(p);
        char msg[700];
        snprintf(msg, sizeof msg, "config %.*s", n, p);
        gptps_log(NULL, GPTPS_LOG_ERROR, msg);
        p += n + (nl ? 1 : 0);
    }
}

/* Check one value against a key's shape; on refusal, `why` says why. */
/* Whether the file wrote the kind of value the key takes: a number for a number,
 * true or false for a switch, a "string" for text or a choice - so a quoted number
 * is a string, as TOML has it. 1, or 0 with `why`. */
static int cfg_kind_ok(gptps_setting_type type, gptps_toml_kind k, const char *text, char *why, size_t cap)
{
    const char *want = NULL;
    int text_wanted = 0;
    switch (type) {
        case GPTPS_SETTING_INT: case GPTPS_SETTING_UINT: if (k != GPTPS_TOML_INT) want = "a whole number"; break;
        case GPTPS_SETTING_DOUBLE: if (k != GPTPS_TOML_INT && k != GPTPS_TOML_FLOAT) want = "a number"; break;
        case GPTPS_SETTING_BOOL:   if (k != GPTPS_TOML_BOOL) want = "true or false"; break;
        default:                   if (k != GPTPS_TOML_STRING) { want = "a \"string\""; text_wanted = 1; } break;
    }
    if (!want) return 1;
    if (k == GPTPS_TOML_ARRAY) {
        snprintf(why, cap, "expects %s, not a list", want);
    } else if (k == GPTPS_TOML_STRING) {
        /* "4" for a number: say what would do. "four": the value is wrong, not the quotes. */
        int unquoted_ok = type == GPTPS_SETTING_BOOL ? (!strcmp(text, "true") || !strcmp(text, "false"))
                                                     : gptps_settings_plain_number(text, type != GPTPS_SETTING_DOUBLE);
        snprintf(why, cap, "expects %s, not the string \"%s\"%s", want, text, unquoted_ok ? " - drop the quotes" : "");
    } else if (text_wanted) {
        snprintf(why, cap, "expects a \"string\" - put %s in quotes", text);
    } else {
        snprintf(why, cap, "expects %s, not %s", want, text);
    }
    return 0;
}

static int cfg_value_ok(gptps_setting_type type, int has_range, double mn, double mx,
                        const char *const *choices, const cfg_item *it, char *why, size_t cap)
{
    if (!it->usable && it->kind != GPTPS_TOML_ARRAY) {
        snprintf(why, cap, "expects a single value, not a string of %d characters or more", GPTPS_SETTINGS_VALUE_MAX);
        return 0;
    }
    if (!cfg_kind_ok(type, it->kind, it->text, why, cap)) return 0;
    if (gval_ok(type, has_range, mn, mx, choices, it->text)) return 1;
    gptps_settings_explain(type, has_range, mn, mx, choices, it->text, why, cap);
    return 0;
}

/* A file's value for a setting, through the registry as a live set would - once it
 * is the kind of value the setting takes. 0 when no setting has the key (now). */
static int cfg_set_from_file(gptps *e, const cfg_item *it, gptps_status *st, char *why, size_t cap)
{
    gptps_setting_type ty;
    if (!gptps_settings_type_of(e->settings, it->key, &ty)) return 0;
    why[0] = 0;
    if (!it->usable && it->kind != GPTPS_TOML_ARRAY) {
        snprintf(why, cap, "expects a single value, not a string of %d characters or more", GPTPS_SETTINGS_VALUE_MAX);
        *st = GPTPS_E_CONFIG;
    } else if (!cfg_kind_ok(ty, it->kind, it->text, why, cap)) {
        *st = GPTPS_E_CONFIG;
    } else {
        gptps_cb_thread *in = cb_enter(e);  /* it runs a write accessor, and an add-on's watchers:
                                             * a gptps_shutdown from either is refused */
        *st = gptps_settings_set_text(e->settings, it->key, it->text, why, cap);
        cb_leave(in);
        if (*st == GPTPS_E_NOTFOUND) return 0;          /* removed meanwhile: it waits again */
    }
    return 1;
}

/* A validated per-task value, into the def. */
static void task_key_put(const task_key *k, const char *v, gptps_task_def *def, int32_t *priority)
{
    if      (!strcmp(k->leaf, "timeout_seconds"))       def->default_policy.timeout_seconds = (uint32_t)strtoull(v, NULL, 10);
    else if (!strcmp(k->leaf, "max_retries"))           def->default_policy.max_retries = (uint32_t)strtoull(v, NULL, 10);
    else if (!strcmp(k->leaf, "retry_backoff_seconds")) def->default_policy.retry_backoff_seconds = (uint32_t)strtoull(v, NULL, 10);
    else if (!strcmp(k->leaf, "mem_bytes"))             def->default_cost.mem_bytes = (uint64_t)strtoull(v, NULL, 10);
    else if (!strcmp(k->leaf, "priority"))              *priority = (int32_t)strtoll(v, NULL, 10);
    else if (!strcmp(k->leaf, "on_failure"))
        def->default_policy.on_failure = !strcmp(v, "drop")    ? GPTPS_ON_FAILURE_DROP
                                       : !strcmp(v, "requeue") ? GPTPS_ON_FAILURE_REQUEUE
                                       :                         GPTPS_ON_FAILURE_DEAD_LETTER;
}

/* A task's values from the file, at its registration: [task_defaults], then
 * [tasks.<name>] - the most specific wins. Validated when the file was read; here
 * they are only converted, re-checked so nothing unchecked can reach a field. The
 * [tasks.<name>] entries used are claimed. e->m held. */
static void apply_task_config(gptps *e, const char *name, gptps_task_def *def, int32_t *priority)
{
    gptps_toml *t = e->toml;
    char key[400];
    size_t i;
    if (!t) return;
    for (i = 0; i < N_TASK_KEYS; ++i) {
        const task_key *k = &TASK_KEYS[i];
        const char *v;
        long j;
        char no[8];
        snprintf(key, sizeof key, "task_defaults.%s", k->leaf);
        if ((j = gptps_toml_find_dotted(t, key)) >= 0 && (v = gptps_toml_text_at(t, (size_t)j)) != NULL &&
            cfg_kind_ok(k->type, gptps_toml_kind_at(t, (size_t)j), v, no, sizeof no) &&
            gval_ok(k->type, k->has_range, k->min, k->max, k->choices, v))
            task_key_put(k, v, def, priority);
        snprintf(key, sizeof key, "tasks.%s.%s", name, k->leaf);
        if ((j = gptps_toml_find_dotted(t, key)) >= 0 && (v = gptps_toml_text_at(t, (size_t)j)) != NULL &&
            cfg_kind_ok(k->type, gptps_toml_kind_at(t, (size_t)j), v, no, sizeof no) &&
            gval_ok(k->type, k->has_range, k->min, k->max, k->choices, v)) {
            task_key_put(k, v, def, priority);
            gptps_toml_claim_at(t, (size_t)j);
        }
    }
}

/* The keys that size the engine, read before it exists: [limits] and [bounded]. An
 * explicit gptps_config value wins over the file; 0 means "not set" in both.
 * gptps_config.max_dead_letters and .shutdown_grace_ms win the same way, but their 0
 * ("not set") is not the file's 0 ("no limit"): when the struct sets one, the file's
 * value is only checked here; when it does not, cfg_apply sets the file's value as a
 * live set would. Returns the number of errors, each one logged. */
static unsigned cfg_open_keys(gptps_toml *t, gptps_config *c)
{
    static const struct { const char *key; int has_range; double max; } K[] = {
        { "limits.max_concurrent_tasks", 1, 65536.0 },
        { "limits.max_memory_bytes",     0, 0 },
        { "limits.max_intake_depth",     1, 4294967295.0 },
        { "bounded.max_items",           1, 4294967295.0 },
        { "bounded.max_payload_bytes",   1, 4294967295.0 },
        { "bounded.max_result_bytes",    1, 4294967295.0 },
    };
    unsigned bad = 0;
    char why[512];
    cfg_item it;
    size_t i;
    long j;
    for (i = 0; i < sizeof K / sizeof K[0]; ++i) {
        unsigned long long v;
        if ((j = gptps_toml_find_dotted(t, K[i].key)) < 0) continue;
        gptps_toml_claim_at(t, (size_t)j);
        cfg_item_at(t, (size_t)j, &it);
        if (!cfg_value_ok(GPTPS_SETTING_UINT, K[i].has_range, 0, K[i].max, NULL, &it, why, sizeof why)) {
            cfg_say(GPTPS_LOG_ERROR, &it, "%s", why); ++bad; continue;
        }
        v = strtoull(it.text, NULL, 10);
        switch (i) {
            case 0: if (!c->limits.max_concurrent_tasks) c->limits.max_concurrent_tasks = (uint32_t)v; break;
            case 1: if (!c->limits.max_memory_bytes)     c->limits.max_memory_bytes = (uint64_t)v;     break;
            case 2: if (!c->limits.max_intake_depth)     c->limits.max_intake_depth = (uint32_t)v;     break;
            case 3: if (!c->max_items)                   c->max_items = (uint64_t)v;                   break;
            case 4: if (!c->max_payload_bytes)           c->max_payload_bytes = (uint32_t)v;           break;
            default: if (!c->max_result_bytes)           c->max_result_bytes = (uint32_t)v;            break;
        }
    }
    for (i = 0; i < 2; ++i) {      /* the two limits whose 0 is the file's "no limit" */
        if (!(i ? c->shutdown_grace_ms : c->max_dead_letters)) continue;
        if ((j = gptps_toml_find_dotted(t, i ? "limits.shutdown_grace_ms" : "limits.max_dead_letters")) < 0) continue;
        gptps_toml_claim_at(t, (size_t)j);
        cfg_item_at(t, (size_t)j, &it);
        if (!cfg_value_ok(GPTPS_SETTING_UINT, 1, 0, 4294967295.0, NULL, &it, why, sizeof why)) {
            cfg_say(GPTPS_LOG_ERROR, &it, "%s", why); ++bad;
        }
    }
    /* max_memory_gb: the file's other spelling of max_memory_bytes, which wins if both are set */
    if ((j = gptps_toml_find_dotted(t, "limits.max_memory_gb")) >= 0) {
        gptps_toml_claim_at(t, (size_t)j);
        cfg_item_at(t, (size_t)j, &it);
        if (!cfg_value_ok(GPTPS_SETTING_DOUBLE, 1, 0, 1e9, NULL, &it, why, sizeof why)) {
            cfg_say(GPTPS_LOG_ERROR, &it, "%s", why); ++bad;
        } else if (!c->limits.max_memory_bytes) {
            c->limits.max_memory_bytes = (uint64_t)(gptps_strtod_c(it.text, NULL) * 1073741824.0);
        }
    }
    return bad;
}

/* A key is judged by its dotted form, never by how the file spelt it: [limits] x,
 * [limits.y] x and a quoted "limits.x" all start with the table "limits". */
static size_t cfg_first_len(const char *key)
{
    const char *dot = strchr(key, '.');
    return dot ? (size_t)(dot - key) : strlen(key);
}

static int cfg_first_is(const char *key, const char *table)
{
    size_t fl = cfg_first_len(key);
    return strlen(table) == fl && strncmp(key, table, fl) == 0;
}

/* The engine's own tables: a key in one that no setting has is a mistake, not a key
 * some later definition may claim. */
static int cfg_core_key(const char *key)
{
    if (!key[cfg_first_len(key)]) return 0;      /* no table part: see the near misses */
    return cfg_first_is(key, "limits") || cfg_first_is(key, "scheduler") ||
           cfg_first_is(key, "stats") || cfg_first_is(key, "bounded");
}

/* [resources]: the dotted key past "resources." is the resource's name. */
static int cfg_is_resource(const char *key)
{
    return strncmp(key, "resources.", 10) == 0 && key[10];
}

/* Every table the engine reads. */
static const char *const CFG_OWN[] = { "limits", "scheduler", "stats", "bounded", "resources",
                                       "task_defaults", "tasks", 0 };

/* The engine table `first` is a near miss of - one or two letters off, and longer
 * than three - or NULL. With `d`, how far off. */
static const char *cfg_near_table(const char *first, size_t *d)
{
    size_t m, fl = strlen(first), bestd = 3;
    const char *best = NULL;
    if (fl <= 3) return NULL;
    for (m = 0; CFG_OWN[m]; ++m) {                /* the nearest: [taks] is [tasks], not [stats] */
        size_t dist;
        if (!strcmp(first, CFG_OWN[m])) return NULL;
        dist = gptps_edit_distance(first, CFG_OWN[m], 2);
        if (dist >= 1 && dist < bestd) { bestd = dist; best = CFG_OWN[m]; }
    }
    if (best && d) *d = bestd;
    return best;
}

/* Whether `rest`, the key past its first part, is one of `table`'s keys or close to
 * one: what makes a near miss of the table's name a typo - [limit]
 * max_concurrent_tasks - rather than a table of the host's - [status] code. */
static int cfg_typo_evidence(gptps *e, const char *table, const char *rest, size_t d)
{
    const char *leaf = strrchr(rest, '.');
    leaf = leaf ? leaf + 1 : rest;
    if (!strcmp(table, "resources")) return d == 1;      /* any name may be a resource's: [resource] gpu */
    if (!strcmp(table, "tasks") || !strcmp(table, "task_defaults")) {
        size_t m;
        if (strstr(rest, ".resources.")) return 1;
        for (m = 0; m < N_TASK_KEYS; ++m) if (gptps_edit_distance(leaf, TASK_KEYS[m].leaf, 2) <= 2) return 1;
        return 0;
    }
    {
        char fixed[512], guess[384];
        size_t tl = strlen(table);
        snprintf(fixed, sizeof fixed, "%s.%s", table, rest);
        if (gptps_settings_has(e->settings, fixed)) return 1;
        return gptps_settings_closest(e->settings, fixed, guess, sizeof guess) &&
               strncmp(guess, table, tl) == 0 && guess[tl] == '.';
    }
}

static void cfg_claim(gptps *e, gptps_toml *t, size_t i)
{
    gptps_mutex_lock(e->m);
    gptps_toml_claim_at(t, i);
    gptps_mutex_unlock(e->m);
}

static int cfg_claimed(gptps *e, const gptps_toml *t, size_t i)
{
    int c;
    gptps_mutex_lock(e->m);
    c = gptps_toml_claimed_at(t, i);
    gptps_mutex_unlock(e->m);
    return c;
}

/* A value refused, marked in the file it came from: a save that copies the file
 * writes the engine's own value in its place (cfg_refused). */
static void cfg_refuse(gptps *e, gptps_toml *t, const cfg_item *it)
{
    gptps_mutex_lock(e->m);
    gptps_toml_refuse_at(t, it->index);
    gptps_mutex_unlock(e->m);
}

/* A key the engine cannot hold whole, refused before anything looks at the file's keys
 * by their names: cut to fit a cfg_item, it matched any setting whose key it starts
 * with, and a value meant for one key was applied to another. The settings the engine
 * makes for a task or a resource are built in buffers of this size, so none has a
 * longer key; a host's could, and a file cannot set it. Claimed, so no later pass
 * reads it, and refused, so a save that copies the file leaves it out. Returns the
 * number refused, each one logged. */
static unsigned cfg_refuse_long_keys(gptps *e, gptps_toml *t)
{
    unsigned bad = 0;
    size_t i, n = gptps_toml_count(t);
    for (i = 0; i < n; ++i) {
        const char *sec = gptps_toml_section_at(t, i), *key = gptps_toml_key_at(t, i);
        size_t len = (*sec ? strlen(sec) + 1 : 0) + strlen(key);
        char shown[GPTPS_TOML_SHOWN], msg[GPTPS_TOML_SHOWN + 400];
        cfg_item it;
        if (len < CFG_KEY_MAX || cfg_claimed(e, t, i)) continue;
        cfg_claim(e, t, i);
        cfg_item_at(t, i, &it);
        cfg_refuse(e, t, &it);
        gptps_toml_key_shown(sec, key, shown, sizeof shown);
        snprintf(msg, sizeof msg, "config %s:%d: %s: the key is %lu bytes long - a file may set one of "
                 "at most %d", it.path, it.line, shown, (unsigned long)len, CFG_KEY_MAX - 1);
        gptps_log(NULL, GPTPS_LOG_ERROR, msg);
        ++bad;
    }
    return bad;
}

/* Apply a parsed file to a live engine - at open, after cfg_open_keys took the keys
 * that size it, or at a reload. Returns the number of errors, each one logged, and
 * sets *oom when one of them was memory running out. No engine lock is held across a
 * set: the registry's write accessors take it. */
static unsigned cfg_apply(gptps *e, gptps_toml *t, int at_open, int *oom)
{
    unsigned bad = 0;
    size_t i, n = gptps_toml_count(t);
    char why[512], hint[448];   /* " (did you mean <a key>?)" */
    cfg_item it;

    /* At a reload, the file's other spelling of max_memory_bytes; at open,
     * cfg_open_keys took it. And addons: they load at open, so a reload only checks
     * the key's shape - a changed list takes effect at the next start. */
    if (!at_open) {
        long j = gptps_toml_find_dotted(t, "addons");
        if (j >= 0) {
            cfg_claim(e, t, (size_t)j);
            if (gptps_toml_text_at(t, (size_t)j)) {
                cfg_item_at(t, (size_t)j, &it);
                cfg_refuse(e, t, &it);
                cfg_say(GPTPS_LOG_ERROR, &it, "expects a [\"list\"] of add-on paths"); ++bad;
            }
        }
        j = gptps_toml_find_dotted(t, "limits.max_memory_gb");
        if (j >= 0) {
            cfg_claim(e, t, (size_t)j);
            cfg_item_at(t, (size_t)j, &it);
            if (!cfg_value_ok(GPTPS_SETTING_DOUBLE, 1, 0, 1e9, NULL, &it, why, sizeof why)) {
                cfg_refuse(e, t, &it);
                cfg_say(GPTPS_LOG_ERROR, &it, "%s", why); ++bad;
            } else if (gptps_toml_find_dotted(t, "limits.max_memory_bytes") < 0) {
                char v[32];
                snprintf(v, sizeof v, "%llu", (unsigned long long)(gptps_strtod_c(it.text, NULL) * 1073741824.0));
                if (gptps_settings_set_text(e->settings, "limits.max_memory_bytes", v, why, sizeof why) != GPTPS_OK) {
                    cfg_refuse(e, t, &it);
                    cfg_say(GPTPS_LOG_ERROR, &it, "%s", why); ++bad;
                }
            }
        }
    }
    /* [resources]: the operator names them. Each key defines one; a name already
     * defined is re-budgeted. */
    for (i = 0; i < n; ++i) {
        gptps_status st;
        if (cfg_claimed(e, t, i)) continue;
        cfg_item_at(t, i, &it);
        if (!cfg_is_resource(it.key)) continue;
        cfg_claim(e, t, i);
        if (!cfg_value_ok(GPTPS_SETTING_UINT, 0, 0, 0, NULL, &it, why, sizeof why)) {
            cfg_refuse(e, t, &it);
            cfg_say(GPTPS_LOG_ERROR, &it, "a resource's budget: %s", why); ++bad; continue;
        }
        st = gptps_define_resource(e, it.key + 10, (uint64_t)strtoull(it.text, NULL, 10));
        if (st == GPTPS_E_NOMEM) *oom = 1;
        if (st != GPTPS_OK) {
            cfg_refuse(e, t, &it);
            cfg_say(GPTPS_LOG_ERROR, &it, "the resource could not be defined: %s", gptps_strerror(st)); ++bad;
        }
    }
    /* every key that names a setting, through the registry, as a live set would */
    for (i = 0; i < n; ++i) {
        gptps_status st;
        if (cfg_claimed(e, t, i)) continue;
        cfg_item_at(t, i, &it);
        if (!cfg_set_from_file(e, &it, &st, why, sizeof why)) continue;
        cfg_claim(e, t, i);
        if (st == GPTPS_E_NOMEM) *oom = 1;  /* a write accessor's: the value may be fine */
        if (st != GPTPS_OK) {
            cfg_refuse(e, t, &it);
            cfg_say(GPTPS_LOG_ERROR, &it, "%s", why); ++bad;
        }
    }
    /* per-task keys: checked now, applied when the task registers (or above, through
     * the registry, if it already has). By the dotted key, which is the same however
     * the file spells it: [tasks.x] max_retries, or a quoted "tasks.x.max_retries". */
    for (i = 0; i < n; ++i) {
        const task_key *k;
        const char *leaf;
        int defaults;
        if (cfg_claimed(e, t, i)) continue;
        cfg_item_at(t, i, &it);
        defaults = !strncmp(it.key, "task_defaults.", 14);
        if (!defaults && strncmp(it.key, "tasks.", 6) != 0) continue;
        if (!defaults) {                    /* a leaf a plug-in or the host defined: np.priority */
            const gptps_task_schema *sc, *best = NULL;
            size_t kl = strlen(it.key), bl = 0;
            gptps_setting_type ty = GPTPS_SETTING_UINT;
            int hr = 0; double mn = 0, mx = 0;
            const char *const *ch = NULL;
            gptps_mutex_lock(e->m);         /* schemas are freed only at shutdown */
            for (sc = e->task_schemas; sc; sc = sc->next) {
                size_t ll = strlen(sc->leaf);
                if (ll > bl && kl > 6 + ll + 1 && it.key[kl - ll - 1] == '.' && !strcmp(it.key + kl - ll, sc->leaf))
                    { best = sc; bl = ll; }
            }
            if (best) { ty = best->type; hr = best->has_range; mn = best->min; mx = best->max;
                        ch = (const char *const *)best->choices; }
            gptps_mutex_unlock(e->m);
            if (best) {     /* the longest defined leaf wins: np.priority over the built-in priority */
                /* checked now against its definition; applied when the task registers */
                if (!cfg_value_ok(ty, hr, mn, mx, ch, &it, why, sizeof why)) {
                    cfg_claim(e, t, i);
                    cfg_refuse(e, t, &it);
                    cfg_say(GPTPS_LOG_ERROR, &it, "%s", why); ++bad;
                }
                continue;
            }
        }
        if (!defaults && strstr(it.key + 6, ".resources.")) {          /* what a run costs of a resource */
            if (!cfg_value_ok(GPTPS_SETTING_UINT, 0, 0, 0, NULL, &it, why, sizeof why)) {
                cfg_claim(e, t, i);
                cfg_refuse(e, t, &it);
                cfg_say(GPTPS_LOG_ERROR, &it, "a resource cost: %s", why); ++bad;
            }
            continue;
        }
        leaf = defaults ? it.key + 14 : strrchr(it.key, '.') + 1;
        k = task_key_find(leaf);
        if (k && !defaults && (size_t)(leaf - 1 - (it.key + 6)) != strcspn(it.key + 6, ".")) {
            /* tasks.<a.b>.priority: a task named a.b, or a plug-in leaf b.priority that
             * is not defined yet - which, only a later definition says. It waits, and is
             * checked when it is applied. */
            continue;
        }
        if (!k) {
            if (defaults) {
                size_t m, best = 99;
                hint[0] = 0;
                for (m = 0; m < N_TASK_KEYS; ++m) {
                    size_t d = gptps_edit_distance(leaf, TASK_KEYS[m].leaf, 3);
                    if (d <= 3 && d < best) { best = d; snprintf(hint, sizeof hint, " (did you mean %s?)", TASK_KEYS[m].leaf); }
                }
                cfg_claim(e, t, i);
                cfg_refuse(e, t, &it);
                cfg_say(GPTPS_LOG_ERROR, &it, "[task_defaults] has no such key%s", hint); ++bad;
            }
            continue;       /* [tasks.<name>]: a host's or plug-in's per-task setting, claimed when it is defined */
        }
        if (defaults) cfg_claim(e, t, i);
        if (!cfg_value_ok(k->type, k->has_range, k->min, k->max, k->choices, &it, why, sizeof why)) {
            if (!defaults) cfg_claim(e, t, i);
            cfg_refuse(e, t, &it);
            cfg_say(GPTPS_LOG_ERROR, &it, "%s", why); ++bad;
        }
    }
    /* the engine's own tables: a key no setting has is a typo, so it fails now */
    for (i = 0; i < n; ++i) {
        char guess[384];               /* not "near": a macro in <windows.h> */
        if (cfg_claimed(e, t, i)) continue;
        cfg_item_at(t, i, &it);
        if (!cfg_core_key(it.key)) continue;
        cfg_claim(e, t, i);
        hint[0] = 0;
        if (gptps_settings_closest(e->settings, it.key, guess, sizeof guess)) snprintf(hint, sizeof hint, " (did you mean %s?)", guess);
        cfg_refuse(e, t, &it);
        cfg_say(GPTPS_LOG_ERROR, &it, "[%.*s] has no such key%s", (int)cfg_first_len(it.key), it.key, hint); ++bad;
    }
    /* A value named like one of the engine's tables is a mistake: those hold keys.
     * And a key whose first part is a letter or two from one of them is a typo when
     * the rest of it is that table's - [limit] max_concurrent_tasks, [task.resize]
     * priority, [resource] gpu. Any other table may be the host's or a plug-in's -
     * [status], [tags] - and waits for its definition; if none comes, the report of
     * unused keys names the near miss. */
    for (i = 0; i < n; ++i) {
        char first[64];
        const char *own;
        size_t fl, d = 0, m;
        if (cfg_claimed(e, t, i)) continue;
        cfg_item_at(t, i, &it);
        fl = cfg_first_len(it.key);
        if (fl >= sizeof first) continue;
        memcpy(first, it.key, fl); first[fl] = 0;
        if (!it.key[fl]) {
            for (m = 0; CFG_OWN[m]; ++m)
                if (!strcmp(first, CFG_OWN[m])) {
                    cfg_claim(e, t, i);
                    cfg_refuse(e, t, &it);
                    cfg_say(GPTPS_LOG_ERROR, &it, "is one of the engine's tables, not a key - its keys go under [%s]", first);
                    ++bad;
                    break;
                }
            continue;
        }
        if ((own = cfg_near_table(first, &d)) == NULL || !cfg_typo_evidence(e, own, it.key + fl + 1, d)) continue;
        cfg_claim(e, t, i);
        cfg_refuse(e, t, &it);
        cfg_say(GPTPS_LOG_ERROR, &it, "there is no [%s] table (did you mean [%s]?)", first, own);
        ++bad;
    }
    return bad;
}

/* Apply the file's values for settings that exist now - a task just registered, a
 * plug-in's or the host's setting just defined. `prefix` is one key, a subtree when
 * it ends in '.', or every key when it is "". An entry that names no setting yet
 * keeps waiting. Each one applied is claimed; one that fails is logged, and
 * gptps_config_check reports it. Takes e->m and the registry in turn, never both. */
static void cfg_apply_pending(gptps *e, const char *prefix)
{
    size_t from = 0, pl = strlen(prefix);
    int all = pl == 0, subtree = pl && prefix[pl - 1] == '.';
    uint32_t seen = 0;
    int started = 0;
    for (;;) {
        cfg_item it;
        char why[512];
        size_t i = 0, n;
        uint32_t gen;
        int found = 0;
        gptps_status st;
        gptps_mutex_lock(e->m);
        if (e->toml_reloading) { gptps_mutex_unlock(e->m); return; }   /* the reload applies its file */
        /* Inside an add-on's setup, its file values wait: the loader applies them when
         * it returns, so the watcher the add-on registers hears them. Only on the setup
         * thread - another thread's definitions keep their values, as a task's costs
         * must (setup-time calls on other threads meanwhile are outside the contract,
         * gptps.h THREADING). */
        if (e->setup_on && e->setup_tid == gptps_hal_thread_id()) { gptps_mutex_unlock(e->m); return; }
        n = gptps_toml_count(e->toml);
        gen = e->toml_gen;
        if (started && gen != seen) from = 0;      /* a reload swapped the file: its indexes are new */
        seen = gen; started = 1;
        for (i = from; i < n; ++i) {
            if (gptps_toml_claimed_at(e->toml, i)) continue;
            cfg_item_at(e->toml, i, &it);
            if (all || (subtree ? strncmp(it.key, prefix, pl) == 0 : strcmp(it.key, prefix) == 0)) { found = 1; break; }
        }
        gptps_mutex_unlock(e->m);
        if (!found) return;
        from = i + 1;
        if (!cfg_set_from_file(e, &it, &st, why, sizeof why)) continue;
        gptps_mutex_lock(e->m);
        if (e->toml_gen == gen && i < gptps_toml_count(e->toml)) {
            gptps_toml_claim_at(e->toml, i);
            if (st != GPTPS_OK) {
                e->cfg_late_errors += 1;                    /* the current file's, not the one before */
                gptps_toml_refuse_at(e->toml, i);
            }
        }
        gptps_mutex_unlock(e->m);
        if (st != GPTPS_OK) cfg_say(GPTPS_LOG_ERROR, &it, "%s", why);
    }
}

/* Every entry nothing has claimed, through the log sink, with a guess at what was
 * meant. Returns how many there were. */
static unsigned cfg_report(gptps *e, gptps_log_level lvl, const char *tail, int hints)
{
    unsigned count = 0;
    size_t from = 0;
    for (;;) {
        cfg_item it;
        char what[640], guess[384];
        size_t i = 0, n;
        int found = 0;
        what[0] = 0;
        gptps_mutex_lock(e->m);
        n = gptps_toml_count(e->toml);
        for (i = from; i < n; ++i) if (!gptps_toml_claimed_at(e->toml, i)) { found = 1; break; }
        if (found) {
            cfg_item_at(e->toml, i, &it);
            if (!strncmp(it.key, "tasks.", 6)) {
                /* Whose table is it? A registered task whose name the key starts
                 * with - or else the registered name nearest its first part. */
                gptps_reg *r, *mine = NULL, *best = NULL;
                size_t bd = 4, nl;
                char cand[256];
                for (r = e->registry; r; r = r->next) {
                    nl = strlen(r->name);
                    if (!strncmp(it.key + 6, r->name, nl) && it.key[6 + nl] == '.') { mine = r; break; }
                }
                if (mine && !strncmp(it.key + 6 + strlen(mine->name), ".resources.", 11)) {
                    /* a cost of a resource nobody defined - or one an add-on owns, whose
                     * costs the add-on sets: it has no settings to take a file's value */
                    const char *rn = it.key + 6 + strlen(mine->name) + 11;
                    size_t ri;
                    int addon_res = 0;
                    for (ri = 0; ri < e->nres; ++ri)
                        if (!strcmp(e->resources[ri].name, rn)) { addon_res = e->resources[ri].addon; break; }
                    if (addon_res)
                        snprintf(what, sizeof what, "the resource %s belongs to an add-on, which sets what a task "
                                 "costs of it - a config file cannot", rn);
                    else
                        snprintf(what, sizeof what, "no resource named %s is defined - add it under [resources], "
                                 "or define it with gptps_define_resource", rn);
                }
                if (!mine) {
                    const char *dot = strchr(it.key + 6, '.');
                    size_t cl = dot ? (size_t)(dot - (it.key + 6)) : strlen(it.key + 6);
                    if (cl >= sizeof cand) cl = sizeof cand - 1;
                    memcpy(cand, it.key + 6, cl); cand[cl] = 0;
                    for (r = e->registry; r; r = r->next) {
                        size_t d = gptps_edit_distance(cand, r->name, 3);
                        if (d < bd) { bd = d; best = r; }
                    }
                    if (best) snprintf(what, sizeof what, "no task named %s is registered (did you mean %s?)", cand, best->name);
                    else      snprintf(what, sizeof what, "no task named %s is registered", cand);
                }
            }
        }
        gptps_mutex_unlock(e->m);
        if (!found) return count;
        from = i + 1;
        ++count;
        if (!what[0]) {
            char first[64];
            const char *own = NULL;
            size_t fl = cfg_first_len(it.key);
            if (fl < sizeof first && it.key[fl]) { memcpy(first, it.key, fl); first[fl] = 0; own = cfg_near_table(first, NULL); }
            if (hints && gptps_settings_closest(e->settings, it.key, guess, sizeof guess))
                snprintf(what, sizeof what, "nothing has used this key (did you mean %s?)", guess);
            else if (own)
                snprintf(what, sizeof what, "nothing has used this key (is [%s] meant to be [%s]?)", first, own);
            else
                snprintf(what, sizeof what, "nothing has used this key");
        }
        cfg_say(lvl, &it, "%s%s", what, tail);
    }
}

/* The report the first submit asked for (cfg_report_due). `hints` would consult the
 * settings registry for a near miss; none of its callers does - the dispatcher, the
 * stepper standing in for it, shutdown - since a setting's write accessor may hold
 * the registry's lock while it waits for a task one of them has to start.
 * gptps_config_check gives every hint. */
static void cfg_report_first_submit(gptps *e, int hints)
{
    cfg_report(e, GPTPS_LOG_WARN, " - seen at the first submit; gptps_config_check() after setup fails on it", hints);
}

gptps_status gptps_config_check(gptps *e)
{
    unsigned n, late;
    if (!e) return GPTPS_E_INVAL;
    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
    gptps_mutex_lock(e->m);
    e->cfg_reported = 1;                 /* the first submit need not repeat it */
    gptps_mutex_unlock(e->m);
    n = cfg_report(e, GPTPS_LOG_ERROR, "", 1);
    gptps_mutex_lock(e->m);
    late = e->cfg_late_errors;
    gptps_mutex_unlock(e->m);
    return (n || late) ? GPTPS_E_CONFIG : GPTPS_OK;
}

/* A config file is read here, whichever entry point named it. gptps_open_ex used
 * to take cfg->config_path only as the default path for gptps_settings_save and
 * _reload, although the header calls it the "optional TOML path": a host that
 * opened with it got none of the file's [limits], [scheduler], [tasks.*] or add-ons
 * - until its first gptps_settings_reload applied part of the same file. */
gptps_status gptps_open_ex(const gptps_config *cfg, gptps **out_engine)
{
    gptps_config c;
    gptps_toml *t;
    gptps_status s;
    gptps *e;
    const char *const *addons;
    char err[4096];                      /* the parser's messages, a line each */
    unsigned bad = 0;
    long j;
    int n, k, why = 0, oom = 0;          /* oom: memory ran out - not a mistake in the file */

    if (!out_engine) return GPTPS_E_INVAL;
    if (cfg && cfg->struct_size < GPTPS_CONFIG_MIN_SIZE) return GPTPS_E_INVAL; /* ABI: append-safe floor */
    if (!cfg || !cfg->config_path) return open_engine(cfg, out_engine);

    memset(&c, 0, sizeof c);
    memcpy(&c, cfg, cfg->struct_size < sizeof c ? cfg->struct_size : sizeof c);
    c.struct_size = sizeof c;
    t = gptps_toml_parse_file_ex(c.config_path, err, sizeof err, &why);
    if (!t) {
        char msg[400];
        *out_engine = NULL;
        if (why == GPTPS_TOML_NOMEM) return GPTPS_E_NOMEM;   /* the file may be fine: nothing to say */
        cfg_say_parse(err);              /* why: "cannot open the file (...)", or each bad line */
        snprintf(msg, sizeof msg, "config %s: the file %s - the engine was not opened", c.config_path,
                 why == GPTPS_TOML_UNREAD ? "cannot be read" : "does not parse");
        gptps_log(NULL, GPTPS_LOG_ERROR, msg);
        return GPTPS_E_CONFIG;
    }
    /* [limits] and [bounded]: they size the engine. A value refused here is left out
     * and the engine opens all the same - only so that the rest of the file is
     * checked too, and every mistake in it reported at once rather than one per
     * attempt. The open is refused below. */
    bad = cfg_open_keys(t, &c);

    s = open_engine(&c, out_engine);
    if (s != GPTPS_OK) { gptps_toml_free(t); return s; }
    e = *out_engine;
    e->toml = t;                         /* retained for register-time task overrides */
    e->cfg_opening = 1;                  /* no other thread has the engine yet */
    bad += cfg_refuse_long_keys(e, t);   /* before an add-on's setup applies the file's values */

    /* top-level addons = ["lib1.so", ...]. One that does not load fails the open,
     * like any other mistake in the file: an add-on is a policy carrier (a
     * constraint that enforces a quota, say), and "ran without it" is exactly the
     * outcome an operator must not discover from behaviour alone. */
    if ((j = gptps_toml_find_dotted(t, "addons")) >= 0) {
        cfg_item it;
        gptps_toml_claim_at(t, (size_t)j);
        cfg_item_at(t, (size_t)j, &it);
        if (gptps_toml_text_at(t, (size_t)j)) {
            cfg_say(GPTPS_LOG_ERROR, &it, "expects a [\"list\"] of add-on paths");
            ++bad;
        }
        n = gptps_toml_str_array(t, "", "addons", &addons);
        for (k = 0; k < n; ++k) {
            gptps_status as = gptps_load_addon(e, addons[k]);
            if (as == GPTPS_E_NOMEM) oom = 1;
            if (as != GPTPS_OK) {
                cfg_say(GPTPS_LOG_ERROR, &it, "the add-on %s did not load (%s)", addons[k], gptps_strerror(as));
                ++bad;
            }
        }
    }
    bad += cfg_apply(e, t, 1, &oom);     /* everything else: the same checks as a live set */
    gptps_mutex_lock(e->m);
    bad += e->cfg_late_errors;           /* a plug-in's key, refused as the add-on defined it */
    e->cfg_late_errors = 0;
    e->cfg_opening = 0;
    gptps_mutex_unlock(e->m);
    if (!bad) return GPTPS_OK;
    gptps_shutdown(e);                   /* frees the file with the engine */
    t = NULL;
    {
        char msg[400];
        snprintf(msg, sizeof msg, "config %s: %u error%s - the engine was not opened",
                 c.config_path, bad, bad == 1 ? "" : "s");
        gptps_log(NULL, GPTPS_LOG_ERROR, msg);
    }
    gptps_toml_free(t);
    *out_engine = NULL;
    return oom ? GPTPS_E_NOMEM : GPTPS_E_CONFIG;
}

gptps_status gptps_open(const char *config_path, gptps **out_engine)
{
    gptps_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.config_path = config_path;
    cfg.limits.struct_size = sizeof cfg.limits;
    return gptps_open_ex(&cfg, out_engine);
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

/* ------------------------------------------------------------------------- */
/* add-on namespaces (ABI 2.1)                                               */
/*                                                                            */
/* A namespaced add-on must register everything under "<ns>.". Enforced only   */
/* inside its own setup(), pinned to that thread - attribution, not a sandbox. */
/* ------------------------------------------------------------------------- */

/* Is `name` acceptable under the namespace window currently in force?
 * Caller holds e->m. True when no window is active, when this is not the thread
 * inside setup(), or when the name carries the "<ns>." prefix. */
static bool ns_ok(const gptps *e, const char *name)
{
    if (!e->cur_ns || e->cur_ns_tid != gptps_hal_thread_id()) return true;
    if (!name) return false;
    return strncmp(name, e->cur_ns, e->cur_ns_len) == 0 && name[e->cur_ns_len] == '.';
}

/* Say WHY a registration was refused. A namespace violation is a mistake in an
 * add-on the operator did not write, so an unexplained GPTPS_E_INVAL would be
 * close to undiagnosable. Caller holds e->m (cur_ns is read here). */
static void ns_reject(const gptps *e, const char *what, const char *name)
{
    char msg[GPTPS_EV_NAME_MAX * 2 + 96];
    snprintf(msg, sizeof msg,
             "add-on namespace '%s': %s \"%s\" must be prefixed \"%s.\" - rejected",
             e->cur_ns, what, name ? name : "(null)", e->cur_ns);
    gptps_log(NULL, GPTPS_LOG_ERROR, msg);
}

/* A namespace token: [a-z][a-z0-9_]{0,30}. No dots - a dot would make the "<ns>."
 * boundary ambiguous against the dotted settings-key grammar. */
static bool ns_token_valid(const char *ns)
{
    size_t i;
    if (!ns || !*ns) return false;
    if (ns[0] < 'a' || ns[0] > 'z') return false;
    for (i = 0; ns[i]; ++i) {
        char c = ns[i];
        if (i >= 31) return false;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

static void register_task_undo(gptps *e, gptps_reg *r);   /* beside reg_destroy, below */

gptps_status gptps_register_task(gptps *e, const gptps_task_def *def)
{
    gptps_reg *r;
    char *name;
    char **argv_copy = NULL;
    uint64_t svc_flags;
    const gptps_task_schema *sc0;
    size_t nres0;

    if (!e || !def || !def->name) return GPTPS_E_INVAL;
    if (bounded_sealed(e)) return GPTPS_E_BUSY;   /* bounded: setup ended at the first submit */
    if (e->max_items && def->exec != GPTPS_EXEC_INPROC) return GPTPS_E_INVAL;   /* bounded: in-process only */
    if (def->struct_size < GPTPS_TASK_DEF_MIN_SIZE) return GPTPS_E_INVAL; /* ABI: below the frozen minimum */
    /* Bound the name HERE, where it can still be reported. Past this length the
     * fixed-size "tasks.<name>.<leaf>" buffers truncate: the six per-task settings
     * collide into one key (five silently lost to E_DUP) and gptps_unregister_task,
     * which refuses an over-long name, can never remove the type again. */
    if (strlen(def->name) > GPTPS_TASK_NAME_MAX) return GPTPS_E_INVAL;
    /* Reject an exec kind this core does not know, at REGISTRATION - the only place it
     * can be reported as what it is. Without this the value flowed through to execute(),
     * which used to treat "not INPROC and not OOP" as PROGRAM: a typo'd or
     * newer-than-us kind registered cleanly, then failed once per attempt with argv
     * NULL, consumed its retries and dead-lettered. That turns a setup mistake into a
     * runtime mystery. Kept as an explicit range test (not a switch) so appending a
     * fourth enumerator later is a one-line change here and in execute(). */
    if ((unsigned)def->exec > (unsigned)GPTPS_EXEC_PROGRAM) return GPTPS_E_INVAL;
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

    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
    gptps_mutex_lock(e->m);
    /* Not while a definition makes its settings (gptps_define_task_setting,
     * gptps_define_resource). It makes one for each task there was when it began, and
     * publishes what it defines only once they are all made. A task linked meanwhile
     * would get it from neither, if this call publishes its own first: not from the
     * definition, which does not know the task, nor from this call, which makes a
     * setting for what is defined - and that is not, yet. No host code runs inside a
     * definition's window, so the wait is short and cannot be on this thread. */
    while (e->active_defines > 0)
        gptps_cond_wait(e->cv_drain, e->m);
    if (!ns_ok(e, def->name)) {                    /* namespaced add-on, unprefixed name */
        ns_reject(e, "task", def->name);
        gptps_mutex_unlock(e->m);
        if (argv_copy) { char **a = argv_copy; while (*a) gptps_free(*a++); gptps_free(argv_copy); }
        return GPTPS_E_INVAL;
    }
    if (registry_holder(e, def->name)) {     /* taken, by a type registered or still registering */
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
    /* Linked in below, under this lock, so the name is taken at once - but its settings
     * are added after the lock goes (settings->m comes first), and until they are, an
     * item of it would run without them: a per-task setting its body reads came back
     * GPTPS_E_NOTFOUND. And this call writes into r until it returns, so another
     * thread's unregister must not free it meanwhile. Settling, it is found by no call
     * from another thread that names it (reg_hidden): it takes no work from one, and
     * none can pause, clone, re-cost or remove it, until this call is done with it. */
    r->settling = true;
    r->settling_tid = gptps_hal_thread_id();
    /* Registered by an add-on's setup, on its thread: if that setup fails, this is how
     * the unwind tells its types from those other threads registered meanwhile. */
    r->load_tag = setup_tag_here(e);
    r->engine = e;
    apply_task_config(e, name, &r->def, &r->priority); /* config file overrides compiled-in defaults */
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
    sc0 = e->task_schemas;               /* what is defined as it is linked */
    nres0 = e->nres;
    gptps_mutex_unlock(e->m);

    /* This task's settings in the registry: its built-in knobs, one for each generic
     * per-task setting defined, and what one run costs of each named resource - all
     * made, then published at once (task_settings). A type without one of them took
     * no value the file gives it - a resource cost among them, so that budget went
     * unenforced - and refused every live set of it, while this returned GPTPS_OK.
     * One that cannot be made takes the registration back, and no setting of the type
     * was ever seen. Made with e->m released: the settings lock comes first. */
    if (task_settings(e, r, sc0, nres0) != GPTPS_OK) {
        register_task_undo(e, r);
        return GPTPS_E_NOMEM;
    }
    {   /* the file's values for this task's settings that only exist now */
        char pre[320];
        snprintf(pre, sizeof pre, "tasks.%s.", name);
        cfg_apply_pending(e, pre);
    }
    gptps_mutex_lock(e->m);
    r->settling = false;                  /* set up: from here on it is there for every call */
    gptps_mutex_unlock(e->m);
    return GPTPS_OK;
}

gptps_status gptps_set_task_priority(gptps *e, const char *task_name, int priority)
{
    gptps_reg *r;
    if (!e || !task_name) return GPTPS_E_INVAL;
    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
    gptps_mutex_lock(e->m);
    r = registry_find(e, task_name);
    if (r) r->priority = (int32_t)priority;   /* applies to subsequently-submitted items */
    gptps_mutex_unlock(e->m);
    return r ? GPTPS_OK : GPTPS_E_NOTFOUND;
}

/* ---- named resource budgets (generic admission limits) ---- */
/* A resource already defined: re-budgeted. 1 if it was. Caller holds e->m. */
static int res_rebudget(gptps *e, const char *name, uint64_t budget)
{
    size_t i;
    for (i = 0; i < e->nres; ++i)
        if (strcmp(e->resources[i].name, name) == 0) {
            e->resources[i].budget = budget;
            /* Wake the dispatcher: a raise may admit waiting work now, and a shrink
             * may strand queued items that the admission scan must dead-letter. */
            gptps_cond_signal(e->cv_disp);
            return 1;
        }
    return 0;
}

/* Room for one more resource, in e->resources and in every type's cost vector. Made
 * before the resource is published, so it can fail with nothing changed: a vector
 * grown for a resource that then is not published is only longer. Caller holds e->m. */
static gptps_status res_room(gptps *e)
{
    gptps_reg *r;
    if (e->nres == e->rescap) {
        size_t nc = e->rescap ? e->rescap * 2 : 4;
        gptps_resource *nr = (gptps_resource *)gptps_realloc(e->resources, nc * sizeof *nr);
        if (!nr) return GPTPS_E_NOMEM;
        e->resources = nr; e->rescap = nc;
    }
    for (r = e->registry; r; r = r->next) {
        uint64_t *nc = (uint64_t *)gptps_realloc(r->res_cost, (e->nres + 1) * sizeof(uint64_t));
        if (!nc) return GPTPS_E_NOMEM;
        r->res_cost = nc;
    }
    return GPTPS_OK;
}

gptps_status gptps_define_resource(gptps *e, const char *name, uint64_t budget)
{
    size_t i, ri, nsnap = 0, cap = 0;
    gptps_reg *r, **snap = NULL;
    gptps_res_cell *bx = NULL, *costs = NULL, **ct = &costs, *x;
    gptps_status st = GPTPS_OK;
    char *nm, dv[32];
    int addon, again = 0;
    if (!e || !name || !*name) return GPTPS_E_INVAL;
    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
    gptps_mutex_lock(e->m);
    if (e->stopping) { gptps_mutex_unlock(e->m); return GPTPS_E_SHUTDOWN; }
    /* The one surface where namespacing is load-bearing rather than tidy: a
     * duplicate resource name is NOT an error here, it silently RE-BUDGETS. So two
     * add-ons both defining "gpu" would each believe they owned the budget, and the
     * second would quietly resize the first's. A claimed namespace makes that
     * collision impossible instead of undetectable. */
    if (!ns_ok(e, name)) {
        ns_reject(e, "resource", name);
        gptps_mutex_unlock(e->m);
        return GPTPS_E_INVAL;
    }
    if (res_rebudget(e, name, budget)) { gptps_mutex_unlock(e->m); return GPTPS_OK; }
    if (bounded_sealed(e)) {                /* bounded: a NEW resource would need a snapshot slot */
        gptps_mutex_unlock(e->m);
        return GPTPS_E_BUSY;
    }
    /* A new one: its settings are the budget, and its cost to each task registered so
     * far. The tasks are taken now and pinned until it is published (active_defines),
     * which also holds back a registration that would link a task the snapshot lacks.
     * At the publication, a task being unregistered is passed over - its unregister's
     * sweep of owned settings counts on no definition adding one after it - and so is
     * one whose registration has not published its settings yet: that registration
     * finds the resource there, and makes the cost itself (task_settings). */
    addon = (e->cur_ns && e->cur_ns_tid == gptps_hal_thread_id());
    for (r = e->registry; r && !addon; r = r->next) {
        if (r->removed) continue;
        if (nsnap == cap) {
            size_t nc = cap ? cap * 2 : 8;
            gptps_reg **ns = (gptps_reg **)gptps_realloc(snap, nc * sizeof *ns);
            if (!ns) { gptps_mutex_unlock(e->m); gptps_free(snap); return GPTPS_E_NOMEM; }
            snap = ns; cap = nc;
        }
        snap[nsnap++] = r;
    }
    if (!addon) e->active_defines += 1;
    gptps_mutex_unlock(e->m);

    /* All of it or none, and none of it seen until all of it is made: the name and
     * every setting are made first; the slot, the cost vectors' room and the
     * publication follow under both locks, in one critical section. A resource that
     * returned GPTPS_OK without its settings never took the file's value for what a
     * task costs of it - so that budget went unenforced - and refused every live set
     * of them. One taken back once published would have been seen meanwhile: another
     * thread could read its name, set a cost on it, or admit work against its slot. */
    nm = (char *)gptps_malloc(strlen(name) + 1);
    if (!nm) st = GPTPS_E_NOMEM;
    else strcpy(nm, name);
    if (st == GPTPS_OK && !addon) {
        snprintf(dv, sizeof dv, "%llu", (unsigned long long)budget);
        if (!(bx = res_cell_prepare(e, NULL, 0, name, dv))) st = GPTPS_E_NOMEM;
        for (i = 0; i < nsnap && st == GPTPS_OK; ++i) {   /* a new resource costs every task 0 */
            if (!(*ct = res_cell_prepare(e, snap[i], 0, name, "0"))) st = GPTPS_E_NOMEM;
            else ct = &(*ct)->next;
        }
    }
    if (st == GPTPS_OK) {
        gptps_settings_lock(e->settings);
        gptps_mutex_lock(e->m);
        if (e->stopping) st = GPTPS_E_SHUTDOWN;
        else if (res_rebudget(e, name, budget)) again = 1;   /* defined meanwhile: this re-budgets it */
        else if (bounded_sealed(e)) st = GPTPS_E_BUSY;
        else st = res_room(e);
        if (st == GPTPS_OK && !again) {
            ri = e->nres;
            e->resources[ri].name = nm;
            e->resources[ri].budget = budget;
            e->resources[ri].reserved = 0;
            e->resources[ri].addon = addon;
            for (r = e->registry; r; r = r->next) r->res_cost[ri] = 0;
            e->nres += 1;
            nm = NULL;                      /* the engine's, until shutdown */
            if (bx) { bx->ri = ri; res_cell_publish(e, bx, &e->res_cells); bx = NULL; }
            while ((x = costs) != NULL) {
                costs = x->next;
                if (x->r->removed || !x->r->published) { x->next = NULL; res_cells_free(x); continue; }
                x->ri = ri;
                res_cell_publish(e, x, &x->r->res_cells);
            }
        }
        gptps_mutex_unlock(e->m);
        gptps_settings_unlock(e->settings);
    }
    gptps_free(nm);                         /* what was made and not published */
    res_cells_free(bx);
    res_cells_free(costs);
    if (!addon) {
        gptps_mutex_lock(e->m);
        e->active_defines -= 1;
        gptps_cond_broadcast(e->cv_drain);
        gptps_mutex_unlock(e->m);
    }
    gptps_free(snap);
    if (st != GPTPS_OK || again || addon) return st;
    {   /* the file's values for the settings just made */
        char key[320];
        snprintf(key, sizeof key, "resources.%s", name);
        cfg_apply_pending(e, key);
        cfg_apply_pending(e, "tasks.");
    }
    return GPTPS_OK;
}

gptps_status gptps_set_task_resource_cost(gptps *e, const char *task_name,
                                          const char *resource, uint64_t amount)
{
    size_t i;
    gptps_reg *r;
    if (!e || !task_name || !resource) return GPTPS_E_INVAL;
    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
    gptps_mutex_lock(e->m);
    r = registry_find(e, task_name);
    if (!r) { gptps_mutex_unlock(e->m); return GPTPS_E_NOTFOUND; }
    for (i = 0; i < e->nres; ++i)
        if (strcmp(e->resources[i].name, resource) == 0) {
            if (r->res_cost) r->res_cost[i] = amount;
            gptps_cond_signal(e->cv_disp);   /* queued items of the type are judged by it now */
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
    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
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

/* Unlink every item in q referencing r and move it to `out` (caller holds e->m).
 * The items are NOT freed here: they are still submitted handles that owe their
 * observer a terminal event, and events must be emitted with the lock RELEASED.
 * The caller drains `out` after unlocking (see gptps_unregister_task). Only used
 * on queues whose items have NOT reserved admission budget (intake / delayed). */
static unsigned fifo_detach_reg(gptps_fifo *q, const gptps_reg *r, gptps_fifo *out)
{
    gptps_item *it, *next; unsigned n = 0;
    for (it = q->head; it; it = next) {
        next = it->next;
        if (it->reg != r) continue;
        fifo_remove(q, it);
        fifo_push(out, it); ++n;
    }
    return n;
}

/* fifo_detach_reg's sibling for queues whose items HAVE reserved admission budget
 * (`ready` / `done`). Detaching those without releasing the ledger would leak
 * e->running and e->reserved_mem for the life of the engine - the counters are only
 * ever decremented by the done-drain, which will never see these items again.
 * Mirrors engine_pass step 1's release. Caller holds e->m. */
static unsigned fifo_detach_reg_admitted(gptps *e, gptps_fifo *q, const gptps_reg *r,
                                         gptps_fifo *out)
{
    gptps_item *it, *next; unsigned n = 0;
    for (it = q->head; it; it = next) {
        next = it->next;
        if (it->reg == r) {
            fifo_remove(q, it);
            e->reserved_mem -= it->cost.mem_bytes;
            e->running      -= 1;
            if (it->res_reserved) {
                size_t ri;
                for (ri = 0; ri < it->res_n && ri < e->nres; ++ri)
                    e->resources[ri].reserved -= it->res_reserved[ri];
                if (!it->pooled) gptps_free(it->res_reserved);
                it->res_reserved = NULL;
                it->res_n = 0;
            }
            fifo_push(out, it); ++n;
        }
    }
    return n;
}

/* Has this handle already had the terminal event that closes it? Only if its current
 * attempt RAN and execute() reported that attempt as one: a FAILED stamped
 * GPTPS_E_CANCELLED, or a FINISHED - which closes a one-shot, but only one RUN of an
 * always-up service, whose instance is closed by the FAILED/E_CANCELLED its stop
 * produces (see GPTPS_TASK_SERVICE in gptps.h). A plain FAILED is per attempt: the
 * retry / dead-letter decision that would have closed the handle never came.
 *   `started` alone is not the test. It records that execute() ran the attempt, not
 * how that attempt ended, and it is cleared only when an item leaves `delayed`, so
 * a bounded retry, a REQUEUE or a service restart parked there still carries the
 * last attempt's 1. Testing `started` alone freed every such item in silence - a
 * REMOVE_CANCEL of a type with a retry in backoff, a service in restart backoff at
 * shutdown, a MANUAL host's teardown with a retry parked - and every observer
 * reconciling terminal events leaked that handle for good. The item's reg must be
 * alive (the caller's contract below). */
static int terminal_reported(const gptps_item *it)
{
    if (!it->started) return 0;                          /* this attempt never ran */
    if (it->outcome == GPTPS_E_CANCELLED) return 1;
    if (it->outcome != GPTPS_OK) return 0;               /* a plain, per-attempt FAILED */
    return !(it->reg && it->reg->service && !it->reg->retire_on_ok);
}

/* Emit a terminal cancelled event for every item in `q` that is still owed one, and
 * free it. Caller must NOT hold e->m (observers may re-enter the engine), and must
 * call this while the items' reg is still alive so item_name() stays valid.
 * An item that terminal_reported() says is already closed is freed silently:
 * emitting another would break the exactly-one-terminal-event invariant. */
static void drain_cancelled(gptps *e, gptps_fifo *q, gptps_event_cb cb, void *ud)
{
    gptps_item *it;
    while ((it = fifo_pop(q)) != NULL) {
        gptps_pending_ev p;
        if (terminal_reported(it)) { item_free(e, it); continue; }
        p.kind = GPTPS_EV_FAILED; p.handle = it->handle;
        ev_set_name(p.name, item_name(it));
        p.status = GPTPS_E_CANCELLED; p.attempt = it->attempt; p.mem = it->cost.mem_bytes;
        p.result = NULL; p.result_len = 0; p.flags = 0;
        emit_now(e, cb, ud, &p);
        item_free(e, it);
    }
}

/* Detach every SERVICE item in q into `out` (caller holds e->m). Only for queues
 * whose items have NOT reserved admission budget (intake / delayed); running/ready
 * items must flow through `done` so their budget is released.
 * These used to be freed outright, which silently broke the exactly-one-terminal-
 * event invariant for any service instance that was queued or in restart backoff at
 * shutdown - the steady state for a service, given the REQUEUE floor. The caller
 * drains `out` with the lock released so each still gets its terminal event. */
static unsigned fifo_detach_services(gptps_fifo *q, gptps_fifo *out)
{
    gptps_item *it, *next; unsigned n = 0;
    for (it = q->head; it; it = next) {
        next = it->next;
        if (!it->reg || !it->reg->service) continue;
        fifo_remove(q, it);
        fifo_push(out, it); ++n;
    }
    return n;
}

/* Give a retained item its own copy of its task's name, and sever both its pointers
 * into the registry, so it outlives its task type: `def` is &r->def, interior to the
 * same allocation (see the gptps_item comment), and item_name falls back to it. 0 if
 * the copy could not be made: the item is then left as it was, still naming its type
 * through `reg`. Caller holds e->m, and the item's reg is alive. */
static int item_own_name(gptps_item *it)
{
    if (!it->name_owned) {
        const char *nm = item_name(it);
        size_t L = strlen(nm) + 1;
        char *cp = (char *)gptps_malloc(L);
        if (!cp) return 0;
        memcpy(cp, nm, L);
        it->name_owned = cp;
    }
    it->reg = NULL; it->def = NULL;   /* only name_owned is read hereafter */
    return 1;
}

/* Before r is freed, each retained dead letter of it gets its own copy of the name. 0
 * if one could not: that one still points at r, so r must outlive it - the caller
 * keeps it on e->retired until shutdown rather than free it. A dead letter used to be
 * left named "?" instead, and handed to the drain's callback so. Caller holds e->m. */
static int detach_dead_letter(gptps *e, gptps_reg *r)
{
    gptps_item *it;
    int all = 1;
    for (it = e->dead_letter.head; it; it = it->next)
        if (it->reg == r && !item_own_name(it)) all = 0;
    return all;
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
    gptps_res_cell *x = r->res_cells;
    while (L) { gptps_task_local *n = L->next; gptps_free(L); L = n; }
    while (x) { gptps_res_cell *n = x->next; gptps_free(x); x = n; }
    if (r->argv_copy) { char **a = r->argv_copy; while (*a) gptps_free(*a++); gptps_free(r->argv_copy); }
    gptps_free(r->res_cost);
    gptps_free(r->name);
    gptps_free(r);
}

/* Take back a registration whose settings could not all be made, so the call can be
 * made again. The type is still settling: no other thread has found it by its name,
 * and this one has run no host code since it linked it, so nothing was submitted to
 * it. None of its settings was published, and no definition published one for it -
 * each passes over a type whose settings are not out yet (task_settings) - so there
 * is no setting to take back: a host's own key under "tasks.<name>." is not touched.
 * A definition may still hold it in its snapshot, though (see active_defines), so it
 * is unlinked once those are done, in the same critical section as the wait, which
 * keeps a new one from taking it in. Its name stays held until then: a registration
 * of it on another thread gets GPTPS_E_DUP, as while any registration of it runs.
 * What a failed add-on load unwinds is found by its load's tag, not by a place in the
 * registry (addon_unwind), so a type leaving the list here does not disturb an unwind
 * running meanwhile. */
static void register_task_undo(gptps *e, gptps_reg *r)
{
    gptps_mutex_lock(e->m);
    while (e->active_defines > 0)
        gptps_cond_wait(e->cv_drain, e->m);
    registry_unlink(e, r);
    gptps_mutex_unlock(e->m);
    reg_destroy(r);
}

size_t gptps_task_count(gptps *e)
{
    size_t n = 0; gptps_reg *r;
    if (!e) return 0;
    GPTPS_REFUSE_AFTER_FORK(e, 0);
    gptps_mutex_lock(e->m);
    for (r = e->registry; r; r = r->next)
        if (!reg_hidden(r)) ++n;   /* includes draining types; not one another thread is registering */
    gptps_mutex_unlock(e->m);
    return n;
}

gptps_status gptps_task_get_info(gptps *e, size_t index, gptps_task_info *out)
{
    gptps_reg *r; size_t i = 0;
    if (!e || !out) return GPTPS_E_INVAL;
    /* Frozen 1.0.0 floor, NOT sizeof: validating against sizeof would pin this struct
     * to today's size, so appending `flags` in ABI 2.2 would have started returning
     * E_INVAL to every caller already compiled. See gptps_internal.h. */
    if (out->struct_size < GPTPS_TASK_INFO_MIN_SIZE) return GPTPS_E_INVAL;
    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
    gptps_mutex_lock(e->m);
    for (r = e->registry; r; r = r->next) {      /* as gptps_task_count counts them */
        if (reg_hidden(r)) continue;
        if (i++ == index) break;
    }
    if (!r) { gptps_mutex_unlock(e->m); return GPTPS_E_NOTFOUND; }
    out->name = r->name; out->exec = r->def.exec; out->priority = r->priority;
    out->default_cost = r->def.default_cost; out->default_policy = r->def.default_policy;
    out->enabled = r->enabled ? 1 : 0; out->removed = r->removed ? 1 : 0;
    out->queued  = fifo_count_reg(&e->intake, r) + fifo_count_reg(&e->delayed, r);
    out->running = fifo_count_reg(&e->ready, r) + fifo_count_reg(&e->running_items, r) + fifo_count_reg(&e->done, r);
    out->dead    = fifo_count_reg(&e->dead_letter, r);
    /* appended past the floor: only write it if the caller's struct actually has it */
    if (GPTPS_STRUCT_HAS(gptps_task_info, out, flags))
        out->flags = GPTPS_STRUCT_HAS(gptps_task_def, &r->def, flags) ? r->def.flags : 0u;
    gptps_mutex_unlock(e->m);
    return GPTPS_OK;
}

gptps_status gptps_task_flags(gptps *e, const char *task_name, uint64_t *out_flags)
{
    gptps_reg *r; uint64_t f = 0;
    if (!e || !task_name || !out_flags) return GPTPS_E_INVAL;
    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
    gptps_mutex_lock(e->m);
    r = registry_find(e, task_name);          /* skips draining types, and one still registering elsewhere */
    if (r) f = GPTPS_STRUCT_HAS(gptps_task_def, &r->def, flags) ? r->def.flags : 0u;
    gptps_mutex_unlock(e->m);
    if (!r) return GPTPS_E_NOTFOUND;
    *out_flags = f;
    return GPTPS_OK;
}

int gptps_task_exists(gptps *e, const char *task_name)
{
    gptps_reg *r; int yes;
    if (!e || !task_name) return 0;
    GPTPS_REFUSE_AFTER_FORK(e, 0);
    gptps_mutex_lock(e->m);
    r = registry_find(e, task_name);          /* skips draining types, and one still registering elsewhere */
    yes = (r && r->enabled) ? 1 : 0;
    gptps_mutex_unlock(e->m);
    return yes;
}

gptps_status gptps_set_task_enabled(gptps *e, const char *task_name, int enabled)
{
    gptps_reg *r;
    if (!e || !task_name) return GPTPS_E_INVAL;
    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
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

    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
    gptps_mutex_lock(e->m);
    r = registry_find(e, src_name);
    if (!r) { gptps_mutex_unlock(e->m); return GPTPS_E_NOTFOUND; }
    if (registry_holder(e, dst_name)) { gptps_mutex_unlock(e->m); return GPTPS_E_DUP; }
    def = r->def;                       /* shares run/cost/user_data; copies exec/cost/policy */
    /* A service's items run with no timeout whatever its timeout_seconds says (each
     * submit normalizes them), but a live set of that setting lands here in r->def -
     * and registration refuses a service with a timeout, so the copy takes the value a
     * service runs with. */
    if (r->service) def.default_policy.timeout_seconds = 0;
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

/* gptps_unregister_task, and the unwind of a failed add-on load, which passes its
 * load's tag: then only a type that load registered is removed - not one a host
 * thread registered under the same name after removing the add-on's. */
static gptps_status unregister_task(gptps *e, const char *task_name, unsigned flags, const void *only_tag)
{
    gptps_reg *r;
    unsigned mode = flags & GPTPS_REMOVE_MODE_MASK;
    gptps_fifo dropped;              /* items cancelled by the removal, freed after unlock */
    gptps_event_cb cb; void *ud;
    gptps_cb_thread *in;
    int named;                       /* every dead letter of it has its own copy of the name */

    if (!e || !task_name) return GPTPS_E_INVAL;
    if (bounded_sealed(e)) return GPTPS_E_BUSY;   /* bounded: setup ended at the first submit */
    if (strlen(task_name) > GPTPS_TASK_NAME_MAX) return GPTPS_E_INVAL;   /* never registrable */

    dropped.head = dropped.tail = NULL; dropped.count = 0; dropped.id = GPTPS_Q_NONE;

    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
    gptps_mutex_lock(e->m);
    if (e->stopping) { gptps_mutex_unlock(e->m); return GPTPS_E_SHUTDOWN; }
    r = registry_find(e, task_name);
    if (r && only_tag && r->load_tag != only_tag) r = NULL;
    if (!r) { gptps_mutex_unlock(e->m); return GPTPS_E_NOTFOUND; }
    /* Its registration is running on this very thread - an add-on's watcher hearing
     * the type's file values, say - and writes into r once this returns: removing it
     * now would free it under that call. */
    if (r->settling) { gptps_mutex_unlock(e->m); return GPTPS_E_BUSY; }

    /* A service's instances run until stopped, so a DRAIN (wait for work to finish)
     * would block forever in THREADED mode / refuse forever in MANUAL. Upgrade it to
     * CANCEL, which cooperatively stops the running instances. REJECT_IF_BUSY is left
     * as-is: "remove only if idle" is still a meaningful request for a service. */
    if (r->service && mode == GPTPS_REMOVE_DRAIN) mode = GPTPS_REMOVE_CANCEL;

    if (e->manual) {
        /* MANUAL: no worker threads, but "nothing is in-flight between gptps_step
         * calls" is only true of running_items. gptps_step's pass B ADMITS work at
         * the end of the step, so `ready` (and `done`) routinely still hold items of
         * this type when the host calls in - and those items keep both it->reg and
         * it->def, which is interior to the reg. Freeing r here left them dangling:
         * the next gptps_step read the freed slot. THREADED never had this because
         * it blocks on reg_live_refs, which counts ready/done/running. */
        if (mode == GPTPS_REMOVE_CANCEL) {
            /* running_items non-empty in MANUAL can only mean this call came from
             * inside gptps_step - a task body or an event callback on this very
             * thread. The body is executing out of r->def right now, so the slot
             * cannot be freed. Refuse, exactly as a re-entrant gptps_shutdown does,
             * and leave the type untouched so a retry after the step is clean. */
            if (fifo_count_reg(&e->running_items, r) > 0) {
                gptps_mutex_unlock(e->m);
                return GPTPS_E_BUSY;
            }
            r->removed = true; r->cancelling = true;
            fifo_detach_reg(&e->intake, r, &dropped);
            intake_forget(e);
            fifo_detach_reg(&e->delayed, r, &dropped);
            /* admitted but not yet run (or run and awaiting accounting): these hold
             * admission budget, so detaching them must release the ledger too */
            fifo_detach_reg_admitted(e, &e->ready, r, &dropped);
            fifo_detach_reg_admitted(e, &e->done,  r, &dropped);
            index_drop_list(e, &dropped);   /* drained below, without the lock */
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
        /* ...unless this IS one of the threads the drain needs. Every completion is
         * accounted by a dispatcher pass and every admitted item is run by a worker,
         * so a removal that has to wait, called from the dispatcher (an event
         * callback for RETRIED / DEAD_LETTERED / DROPPED), waits for a pass it is
         * itself holding up; called from a worker (a task body, or the STARTED /
         * FINISHED / FAILED callback it emits) it waits for the very item that
         * worker is running whenever that item is of this type. Neither ever
         * returns. Refuse, exactly as a re-entrant gptps_shutdown does, before
         * anything is changed, so the caller can retry from a thread of its own.
         * The rule is "an engine thread never waits on the engine", not "only where
         * it would deadlock": a worker removing another type whose work runs
         * elsewhere could finish, but two workers each removing the other's type
         * would wait on each other, and nothing here could tell. A removal with
         * nothing to wait for - an idle type, or a CANCEL of work that is only
         * queued, which it detaches below - still completes here. */
        if (engine_is_reentrant(e, gptps_hal_thread_id())) {
            unsigned owed = fifo_count_reg(&e->ready, r) + fifo_count_reg(&e->done, r)
                          + fifo_count_reg(&e->running_items, r);
            if (mode != GPTPS_REMOVE_CANCEL)
                owed += fifo_count_reg(&e->intake, r) + fifo_count_reg(&e->delayed, r);
            if (owed > 0) { gptps_mutex_unlock(e->m); return GPTPS_E_BUSY; }
        }
        r->removed = true;               /* reject new submits + stop retries (bounded drain) */
        if (mode == GPTPS_REMOVE_CANCEL) {
            gptps_item *it;
            r->cancelling = true;             /* in-flight items are discarded, not dead-lettered */
            fifo_detach_reg(&e->intake, r, &dropped);   /* queued backlog (no budget reserved yet) */
            intake_forget(e);
            fifo_detach_reg(&e->delayed, r, &dropped);
            index_drop_list(e, &dropped);   /* drained below, without the lock */
            for (it = e->running_items.head; it; it = it->next)
                if (it->reg == r) cancel_raise(it);   /* cooperative cancel in-flight */
            gptps_cond_broadcast(e->cv_work);
        }
        gptps_cond_signal(e->cv_disp);        /* wake the dispatcher to drive the drain */
    }

    /* Tear the per-task settings down NOW, not after the drain.
     *
     * registry_find deliberately hides a tombstoned reg, so from the instant
     * r->removed was set above the NAME is free to re-register - while this call may
     * still be blocked for seconds waiting for the type to drain. A re-registration
     * inside that window used to succeed with every one of its "tasks.<name>.*"
     * settings silently rejected as E_DUP against the predecessor's still-live
     * entries, and then this function's prefix removal deleted the successor's keys
     * too: a live, submittable task type with no tunable settings at all.
     *
     * Removing them here shrinks that window from "the whole drain" to the few
     * instructions between the tombstone and this unlock. The engine's own entries
     * go by owner - this reg - not by the "tasks.<name>." prefix, which took a
     * successor's keys and a sibling's: a type named "<name>.x" lives under the same
     * prefix. A host's unowned keys under the prefix still go, as they always did,
     * except those under a live sibling's own "tasks.<sibling>." prefix. What is left
     * of the window is a re-registration between the tombstone and this call, whose
     * settings are rejected as duplicates of the predecessor's.
     *
     * The lock order is settings->m -> e->m (see include/gptps.h), so e->m must be
     * released across the call. Re-acquiring is safe without re-resolving `r`: only
     * this function frees a reg, and a concurrent unregister of the same name cannot
     * have found it - the tombstone hides it from registry_find. */
    {
        char prefix[GPTPS_TASK_NAME_MAX + 8];
        char **keep = NULL; size_t nkeep = 0, cap = 0, nl = strlen(task_name), i;
        gptps_reg *s;
        snprintf(prefix, sizeof prefix, "tasks.%s.", task_name);
        for (s = e->registry; s; s = s->next) {
            char **grown;
            if (s->removed || strncmp(s->name, task_name, nl) != 0 || s->name[nl] != '.') continue;
            if (nkeep == cap) {
                cap = cap ? cap * 2 : 4;
                grown = (char **)gptps_realloc(keep, cap * sizeof *keep);
                if (!grown) break;               /* OOM: keep what we have; see below */
                keep = grown;
            }
            keep[nkeep] = (char *)gptps_malloc(strlen(s->name) + 8);
            if (!keep[nkeep]) break;
            snprintf(keep[nkeep], strlen(s->name) + 8, "tasks.%s.", s->name);
            ++nkeep;
        }
        gptps_mutex_unlock(e->m);
        /* On OOM the sibling list may be short. A sibling left off it then loses its
         * host keys, as every sibling did before - but no host key of THIS type is
         * left bound to state its host may now free. */
        gptps_settings_remove_task(e->settings, r, prefix, (const char *const *)keep, nkeep);
        for (i = 0; i < nkeep; ++i) gptps_free(keep[i]);
        gptps_free(keep);
        gptps_mutex_lock(e->m);
    }

    if (!e->manual)                           /* THREADED: block until the type drains */
        while (reg_live_refs(e, r) > 0)
            gptps_cond_wait(e->cv_drain, e->m);

    /* A concurrent gptps_define_task_setting or gptps_define_resource may hold this reg
     * in its snapshot, and reads its name as it makes the reg's setting; wait for it to
     * finish before freeing the slot (it broadcasts cv_drain, so this terminates even
     * in MANUAL). It publishes under e->m, and nothing for a tombstoned reg: what it
     * published for this one came before the tombstone, and the sweep above took it. */
    while (e->active_defines > 0)
        gptps_cond_wait(e->cv_drain, e->m);

    /* teardown (still holding e->m): make any retained dead-letter items self-owning,
     * then unlink the slot from the registry - and keep it, out of the registry, if a
     * dead letter still names its type through it. */
    named = detach_dead_letter(e, r);
    registry_unlink(e, r);
    if (!named) { r->next = e->retired; e->retired = r; }
    cb = e->ev_cb; ud = e->ev_ud;         /* snapshot under the lock */
    in = cb_enter_locked(e);
    gptps_mutex_unlock(e->m);

    /* Terminal events for the backlog this removal cancelled. Emitted here, with
     * e->m released (observers may re-enter) but BEFORE reg_destroy below, so
     * item_name() still resolves against the live reg. */
    drain_cancelled(e, &dropped, cb, ud);
    cb_leave(in);

    if (named) reg_destroy(r);                /* settings already removed above */
    return GPTPS_OK;
}

gptps_status gptps_unregister_task(gptps *e, const char *task_name, unsigned flags)
{
    return unregister_task(e, task_name, flags, NULL);
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
    char dvbuf[GPTPS_SETTINGS_VALUE_MAX];
    gptps_status st;
    size_t klen;

    if (!e || !key || !*key) return GPTPS_E_INVAL;
    if (bounded_sealed(e)) return GPTPS_E_BUSY;   /* bounded: setup ended at the first submit */
    {   /* namespaced add-on: globals must live under "<ns>." too */
        int bad;
    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
        gptps_mutex_lock(e->m);
        bad = !ns_ok(e, key);
        if (bad) ns_reject(e, "global setting", key);
        gptps_mutex_unlock(e->m);
        if (bad) return GPTPS_E_INVAL;
    }
    if (type == GPTPS_SETTING_ENUM) {
        if (!constraint || !*constraint) return GPTPS_E_CONFIG;   /* enum needs a choice set */
        choices = parse_choices(constraint);
        if (!choices) return GPTPS_E_NOMEM;             /* a set was given: the copy failed */
    } else if (type == GPTPS_SETTING_INT || type == GPTPS_SETTING_UINT || type == GPTPS_SETTING_DOUBLE) {
        if (!parse_range(constraint, &has_range, &mn, &mx)) return GPTPS_E_CONFIG;
    }
    dv = default_val ? default_val : gtype_zero(type, (const char *const *)choices);
    dv = trim_default(type, dv, dvbuf, sizeof dvbuf);
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
    char dvbuf[GPTPS_SETTINGS_VALUE_MAX];
    gptps_reg **snap = NULL; size_t nsnap = 0, cap = 0, i;
    gptps_reg *r;
    gptps_task_local *made = NULL, **lt = &made, *L;
    gptps_status st = GPTPS_OK;

    if (!e || !leaf || !*leaf) return GPTPS_E_INVAL;
    if (bounded_sealed(e)) return GPTPS_E_BUSY;   /* bounded: setup ended at the first submit */
    {   /* A leaf is normally a BARE key - no dots - because it is materialized as
         * "tasks.<task>.<leaf>". A namespaced add-on is the one exception: it must
         * prefix, so it gets exactly one dot, as "<ns>.<bare>". That round-trips
         * cleanly because the settings layer splits a key at its LAST dot, so
         * "tasks.resize.gpuq.units" yields section "tasks.resize.gpuq", leaf
         * "units" - valid TOML, and read back by the add-on as "gpuq.units". */
        int inside_ns, bad;
    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
        gptps_mutex_lock(e->m);
        inside_ns = (e->cur_ns && e->cur_ns_tid == gptps_hal_thread_id());
        if (inside_ns) {
            bad = !ns_ok(e, leaf) || strchr(leaf + e->cur_ns_len + 1, '.') != NULL;
            if (bad) ns_reject(e, "per-task setting leaf", leaf);
        } else {
            bad = (strchr(leaf, '.') != NULL);       /* unchanged rule for everyone else */
        }
        gptps_mutex_unlock(e->m);
        if (bad) return GPTPS_E_INVAL;
    }
    {   /* reject collisions with the six built-in per-task leaves (they are not in
         * task_schemas, so they would otherwise pass the dup check yet fail to materialize) */
        static const char *const BUILTIN[] = { "timeout_seconds", "max_retries", "retry_backoff_seconds",
                                               "mem_bytes", "priority", "on_failure", 0 };
        const char *const *b;
        for (b = BUILTIN; *b; ++b) if (strcmp(*b, leaf) == 0) return GPTPS_E_DUP;
    }
    if (type == GPTPS_SETTING_ENUM) {
        if (!constraint || !*constraint) return GPTPS_E_CONFIG;
        choices = parse_choices(constraint);
        if (!choices) return GPTPS_E_NOMEM;
    } else if (type == GPTPS_SETTING_INT || type == GPTPS_SETTING_UINT || type == GPTPS_SETTING_DOUBLE) {
        if (!parse_range(constraint, &has_range, &mn, &mx)) return GPTPS_E_CONFIG;
    }
    dv = default_val ? default_val : gtype_zero(type, (const char *const *)choices);
    dv = trim_default(type, dv, dvbuf, sizeof dvbuf);
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
    for (r = e->registry; r; r = r->next) {           /* snapshot existing live tasks */
        if (r->removed) continue;
        if (nsnap == cap) {
            size_t nc = cap ? cap * 2 : 8;
            gptps_reg **ns = (gptps_reg **)gptps_realloc(snap, nc * sizeof *ns);
            if (!ns) {
                gptps_mutex_unlock(e->m);
                gptps_free(snap);
                gptps_free(sc->leaf); gptps_free(sc->defval); gptps_free(sc); free_choices(choices);
                return GPTPS_E_NOMEM;
            }
            snap = ns; cap = nc;
        }
        snap[nsnap++] = r;
    }
    e->active_defines += 1;          /* pin: no reg is freed, and none linked, until it is published */
    gptps_mutex_unlock(e->m);

    /* All of it or none, and none of it seen until all of it is made. A definition that
     * returned GPTPS_OK with its setting missing from some tasks left their bodies
     * reading GPTPS_E_NOTFOUND, and the file's value for those tasks unapplied: it
     * published the schema, then made the settings, and went on past one it could not
     * make. So the setting is made for every task first, with e->m released (the
     * settings lock comes first), and the schema - for the tasks registered from now on
     * - is published with all of them, in one critical section. That is also where a
     * second definition of the leaf, made on another thread meanwhile, is found: the
     * first to publish defines it, and the other returns GPTPS_E_DUP having published
     * nothing, as if the leaf had been defined before it began. */
    for (i = 0; i < nsnap && st == GPTPS_OK; ++i) {
        if (!(*lt = task_local_prepare(snap[i], sc))) st = GPTPS_E_NOMEM;
        else lt = &(*lt)->next;
    }
    if (st == GPTPS_OK) {
        gptps_settings_lock(e->settings);
        gptps_mutex_lock(e->m);
        for (it = e->task_schemas; it && strcmp(it->leaf, leaf) != 0; it = it->next) { }
        if (it) st = GPTPS_E_DUP;                       /* defined meanwhile, on another thread */
        else {
            sc->next = e->task_schemas; e->task_schemas = sc;
            while ((L = made) != NULL) {
                made = L->next;
                if (L->reg->removed || !L->reg->published) { L->next = NULL; locals_free(L); continue; }
                local_publish(e, L);
            }
        }
        gptps_mutex_unlock(e->m);
        gptps_settings_unlock(e->settings);
    }
    locals_free(made);                   /* made and not published */

    gptps_mutex_lock(e->m);
    e->active_defines -= 1;
    gptps_cond_broadcast(e->cv_drain);   /* wake any unregister or registration waiting on the pin */
    gptps_mutex_unlock(e->m);

    gptps_free(snap);
    if (st != GPTPS_OK) {
        gptps_free(sc->leaf); gptps_free(sc->defval); gptps_free(sc); free_choices(choices);
        return st;
    }
    cfg_apply_pending(e, "tasks.");      /* the file's values for the settings it made */
    return GPTPS_OK;
}

gptps_status gptps_task_setting_str(gptps_ctx *ctx, const char *key, char *buf, size_t cap)
{
    gptps_task_local *L = NULL;
    gptps *e;
    if (!ctx || !ctx->engine || !ctx->reg || !key || !buf || cap == 0) return GPTPS_E_INVAL;
    e = ctx->engine;
    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
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
    errno = 0;
    v = strtol(b, &end, 10);
    if (end == b || *end) return GPTPS_E_INVAL;   /* not an integer setting */
    /* strtol SATURATES at LONG_MIN/LONG_MAX and only says so through errno. Without
     * this the task silently computed with a value it was never configured with -
     * and because `long` is 32-bit on Windows and on the 32-bit CI leg, an ordinary
     * operator value like 3000000000 clamped there while working fine on 64-bit
     * Linux. Reporting the range error keeps one config meaning one thing. */
    if (errno == ERANGE) return GPTPS_E_INVAL;
    *out = v;
    return GPTPS_OK;
}

/* --- settings: public forwarders onto the registry --- */
gptps_status gptps_register_setting(gptps *e, const gptps_setting_def *def)
{
    if (!e || !def) return GPTPS_E_INVAL;
    if (bounded_sealed(e)) return GPTPS_E_BUSY;   /* bounded: setup ended at the first submit */
    {   /* namespaced add-on: its settings keys must live under "<ns>." */
        int bad;
    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
        gptps_mutex_lock(e->m);
        bad = !ns_ok(e, def->key);
        if (bad) ns_reject(e, "setting", def->key);
        gptps_mutex_unlock(e->m);
        if (bad) return GPTPS_E_INVAL;
    }
    {
        gptps_status st = gptps_settings_add(e->settings, def);
        const void *tag = NULL;
        if (st == GPTPS_OK) {
            gptps_mutex_lock(e->m);         /* an add-on's, inside its setup: undone if that fails */
            if (e->setup_on && e->setup_tid == gptps_hal_thread_id()) tag = e->setup_tag;
            gptps_mutex_unlock(e->m);
            if (tag) gptps_settings_set_tag(e->settings, def->key, tag);
            cfg_apply_pending(e, def->key);   /* the file's value for it, if any */
        }
        return st;
    }
}
/* These forward straight into the settings registry, which carries its OWN mutex -
 * also inherited across a fork, also possibly held by a thread that did not
 * survive - so they refuse in a forked child for the same reason the e->m ones do. */
size_t gptps_settings_count(gptps *e)
{ if (!e) return 0; GPTPS_REFUSE_AFTER_FORK(e, 0); return gptps_settings_size(e->settings); }
gptps_status gptps_settings_get_info(gptps *e, size_t index, gptps_setting_info *out)
{ if (!e) return GPTPS_E_INVAL; GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN); return gptps_settings_info_at(e->settings, index, out); }
gptps_status gptps_settings_get(gptps *e, const char *key, char *buf, size_t cap)
{ if (!e) return GPTPS_E_INVAL; GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN); return gptps_settings_get_by(e->settings, key, buf, cap); }
/* A settings watcher is a callback on the caller's thread (engine_in_callback):
 * a gptps_shutdown from one would free the registry this call is still walking. */
gptps_status gptps_settings_set(gptps *e, const char *key, const char *value)
{
    return gptps_settings_set_ex(e, key, value, NULL, 0);
}

gptps_status gptps_settings_set_ex(gptps *e, const char *key, const char *value, char *why, size_t cap)
{
    gptps_status st;
    gptps_cb_thread *in;
    if (why && cap) why[0] = 0;
    if (!e) return GPTPS_E_INVAL;
    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
    in = cb_enter(e);
    st = gptps_settings_set_live(e->settings, key, value, why, cap);
    cb_leave(in);
    if (st == GPTPS_E_NOTFOUND && why && cap) {
        char guess[384];
        if (gptps_settings_closest(e->settings, key, guess, sizeof guess))
            snprintf(why, cap, "no setting is named %s (did you mean %s?)", key, guess);
        else
            snprintf(why, cap, "no setting is named %s", key);
    }
    return st;
}

/* The value the loaded config file gives a setting: by its key, as max_memory_gb,
 * or for a task through [task_defaults]. 1 and `val`; 2 when the engine refused the
 * value there, which a save then does not write (see gptps_settings_save_to); or 0. A
 * save to a new file writes these. The registry calls it with its lock held, and
 * settings->m -> e->m is the order. */
static int cfg_in_file(const char *key, void *ud, char *val, size_t cap)
{
    gptps *e = (gptps *)ud;
    const gptps_toml *t;
    const char *x = NULL;
    long j = -1;
    int got = 0;
    gptps_mutex_lock(e->m);
    if ((t = e->toml) != NULL) {
        if ((j = gptps_toml_find_dotted(t, key)) >= 0) x = gptps_toml_text_at(t, (size_t)j);
        else if (!strcmp(key, "limits.max_memory_bytes")) {
            if ((j = gptps_toml_find_dotted(t, "limits.max_memory_gb")) >= 0 && (x = gptps_toml_text_at(t, (size_t)j)) != NULL) {
                /* In bytes, and only a value the engine took. 1e20 GiB in bytes is past
                 * any 64-bit count, a conversion C leaves undefined, which on x86-64 wrote
                 * 0, "auto", into the new file; the check is the one open and reload make. */
                cfg_item it;
                char why[160];
                cfg_item_at(t, (size_t)j, &it);
                if (gptps_toml_refused_at(t, (size_t)j) ||
                    !cfg_value_ok(GPTPS_SETTING_DOUBLE, 1, 0, 1e9, NULL, &it, why, sizeof why)) {
                    got = 2;
                } else {
                    snprintf(val, cap, "%llu", (unsigned long long)(gptps_strtod_c(x, NULL) * 1073741824.0));
                    got = 1;
                }
                gptps_mutex_unlock(e->m);
                return got;
            }
        } else if (!strncmp(key, "tasks.", 6) && !strstr(key + 6, ".resources.")) {
            const char *leaf = strrchr(key, '.') + 1;
            char d[400];
            if (task_key_find(leaf)) {
                snprintf(d, sizeof d, "task_defaults.%s", leaf);
                if ((j = gptps_toml_find_dotted(t, d)) >= 0) x = gptps_toml_text_at(t, (size_t)j);
            }
        }
        if (j >= 0 && gptps_toml_refused_at(t, (size_t)j)) got = 2;
        else if (x) { snprintf(val, cap, "%s", x); got = 1; }
    }
    gptps_mutex_unlock(e->m);
    return got;
}

/* Whether the engine refused the value `text` (NULL: a list) the loaded config file
 * gives `key`: then a copy of that file writes the setting's own value there, or leaves
 * the key out. A file changed since it was loaded may give the key another value,
 * which the engine never judged: that one is copied as it is. Called as cfg_in_file. */
static int cfg_refused(const char *key, const char *text, void *ud)
{
    gptps *e = (gptps *)ud;
    const char *x;
    long j;
    int r = 0;
    gptps_mutex_lock(e->m);
    if (e->toml && (j = gptps_toml_find_dotted(e->toml, key)) >= 0 && gptps_toml_refused_at(e->toml, (size_t)j)) {
        x = gptps_toml_text_at(e->toml, (size_t)j);
        r = (x == NULL) == (text == NULL) && (!x || !strcmp(x, text));
    }
    gptps_mutex_unlock(e->m);
    return r;
}

gptps_status gptps_settings_save(gptps *e, const char *path)
{
    char base[1024];
    gptps_status st;
    if (!e) return GPTPS_E_INVAL;
    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
    if (!path) path = e->config_path;
    if (!path) return GPTPS_E_INVAL;
    gptps_mutex_lock(e->m);                  /* the file last loaded, at open or a reload */
    if (e->toml_reloading) {
        /* A reload is applying a file whose values are not all in yet: a save now
         * would write a live value it is about to replace over the file it reads. */
        gptps_mutex_unlock(e->m);
        return GPTPS_E_BUSY;
    }
    base[0] = 0;
    if (e->toml) snprintf(base, sizeof base, "%s", gptps_toml_path(e->toml));
    /* Until the save is done, `toml` stays the file named in `base`: cfg_refused asks
     * it which of the copied values the engine refused. A reload that swapped in
     * another file meanwhile had it answer for the wrong file, so a value the engine
     * refused was copied as it was, and the new file did not open again. */
    e->toml_saving += 1;
    gptps_mutex_unlock(e->m);
    st = gptps_settings_save_to(e->settings, path, base[0] ? base : NULL, cfg_in_file, cfg_refused, e);
    gptps_mutex_lock(e->m);
    e->toml_saving -= 1;
    gptps_mutex_unlock(e->m);
    return st;
}

gptps_status gptps_settings_reload(gptps *e, const char *path)
{
    gptps_toml *t, *old;
    gptps_cb_thread *in;
    char err[4096];                      /* the parser's messages, a line each */
    unsigned bad;
    int why = 0, oom = 0;                /* oom: memory ran out - not a mistake in the file */
    if (!e) return GPTPS_E_INVAL;
    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
    if (!path) path = e->config_path;
    if (!path) return GPTPS_E_INVAL;
    gptps_mutex_lock(e->m);
    /* One at a time; not while an add-on's setup runs, whose settings must take their
     * values when it returns, for its watcher to hear them; and not while a save copies
     * the file this one would replace (see gptps_settings_save). Marked before the
     * file is read, so a save meanwhile is refused rather than rewrite it under us. */
    if (e->toml_reloading || e->setup_on || e->toml_saving) {
        gptps_mutex_unlock(e->m);
        return GPTPS_E_BUSY;
    }
    e->toml_reloading = 1;               /* definitions meanwhile leave this file to us; a save waits */
    gptps_mutex_unlock(e->m);
    t = gptps_toml_parse_file_ex(path, err, sizeof err, &why);
    if (!t) {
        char msg[400];
        if (why != GPTPS_TOML_NOMEM) {   /* out of memory, the file may be fine: nothing to say */
            cfg_say_parse(err);          /* why: "cannot open the file (...)", or each bad line */
            snprintf(msg, sizeof msg, "config %s: the file %s - nothing was reloaded", path,
                     why == GPTPS_TOML_UNREAD ? "cannot be read" : "does not parse");
            gptps_log(NULL, GPTPS_LOG_ERROR, msg);
        }
        gptps_mutex_lock(e->m);
        e->toml_reloading = 0;
        gptps_mutex_unlock(e->m);
        cfg_apply_pending(e, "");        /* what was defined meanwhile, from the file that stands */
        return why == GPTPS_TOML_NOMEM ? GPTPS_E_NOMEM : GPTPS_E_CONFIG;
    }
    gptps_mutex_lock(e->m);
    e->cfg_late_errors = 0;              /* the old file's: this one replaces it */
    gptps_mutex_unlock(e->m);
    in = cb_enter(e);                    /* runs host write accessors */
    bad = cfg_refuse_long_keys(e, t);
    bad += cfg_apply(e, t, 0, &oom);     /* the same checks as at open, and as a live set */
    cb_leave(in);
    gptps_mutex_lock(e->m);              /* swap so future task registrations see it */
    old = e->toml; e->toml = t; e->toml_gen += 1; e->toml_reloading = 0;
    e->cfg_late_errors += bad;           /* gptps_config_check reports them while this file stands */
    gptps_mutex_unlock(e->m);
    gptps_toml_free(old);
    cfg_apply_pending(e, "");            /* anything defined while it was being applied */
    if (bad) {
        char msg[400];
        snprintf(msg, sizeof msg, "config %s: %u error%s in the reload; the valid values were applied",
                 path, bad, bad == 1 ? "" : "s");
        gptps_log(NULL, GPTPS_LOG_ERROR, msg);
    }
    return bad ? (oom ? GPTPS_E_NOMEM : GPTPS_E_CONFIG) : GPTPS_OK;
}

gptps_status gptps_settings_watch(gptps *e, gptps_settings_cb cb, void *user_data)
{
    if (!e) return GPTPS_E_INVAL;
    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);   /* settings->m is inherited too */
    return gptps_settings_watch_add(e->settings, cb, user_data, 0, NULL);
}

/* The host table's settings_watch: an add-on's watcher hears a config file's values
 * as well as live sets, since that is how a plug-in learns its configuration. It
 * cannot reload, save or shut down from the host table, so hearing a reload cannot
 * recurse into one. */
static gptps_status api_settings_watch(gptps *e, gptps_settings_cb cb, void *user_data)
{
    const void *tag = NULL;
    if (!e) return GPTPS_E_INVAL;
    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
    gptps_mutex_lock(e->m);
    if (e->setup_on && e->setup_tid == gptps_hal_thread_id()) tag = e->setup_tag;
    gptps_mutex_unlock(e->m);
    return gptps_settings_watch_add(e->settings, cb, user_data, 1, tag);
}

/* ------------------------------------------------------------------------- */
/* add-on loader (host-table ABI)                                            */
/* ------------------------------------------------------------------------- */

/* host-table entry: lets an add-on emit an event through the engine */
static gptps_status api_emit_event(gptps *e, const gptps_event *ev)
{
    gptps_event_cb cb; void *ud;
    gptps_cb_thread *in = NULL;
    if (!e || !ev) return GPTPS_E_INVAL;
    gptps_mutex_lock(e->m);
    cb = e->ev_cb; ud = e->ev_ud;
    if (cb) in = cb_enter_locked(e);   /* an add-on may emit from a host thread */
    gptps_mutex_unlock(e->m);
    if (cb) cb(ev, ud);
    cb_leave(in);
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
    gptps_set_scheduler,
    /* --- ABI 2.1: the ctx surface a dlopen'd task body could not reach before.
     * Without is_cancelled a binary plugin could not honour a timeout, a cancel or
     * shutdown - i.e. could not meet the liveness guarantees the core makes
     * contractual. See the rationale on gptps_api_routines in the public header. */
    gptps_is_cancelled,
    gptps_deadline_ms,
    gptps_now_ms,
    gptps_result_set_nocopy,
    gptps_task_setting_int,
    gptps_task_setting_str,
    /* work flow: observers run with the lock released and MAY re-enter */
    gptps_submit,
    gptps_submit_ex,
    /* configuration + diagnostics: what a purely config-driven add-on needs */
    gptps_settings_get,
    gptps_settings_set,
    api_settings_watch,
    gptps_set_task_priority,
    gptps_strerror,
    gptps_version,
    /* seam ownership: an add-on must pass flags == 0 and fail setup() on E_BUSY */
    gptps_set_scheduler_ex
};

/* Undo whatever a FAILED addon setup() managed to register before it gave up.
 * Without this, a setup that registered an observer and then returned E_DUP on its
 * second task left a live function pointer on a list the engine walks on the very
 * next event. Matches gptps_unregister_observer's discipline: unlink under e->m,
 * free after.
 *
 * What the setup registered is told by the load's tag, which the setup's thread
 * stamps on every task type, observer and constraint it registers, and on a
 * scheduler it sets (setup_tag_here). Host threads may register and unregister while
 * a setup runs. The unwind used to take everything ahead of each list's head, as the
 * head was when the setup began, to be the setup's own: an observer, constraint or
 * task type a host thread registered meanwhile was removed with the add-on's - a
 * host's admission constraint, a quota, gone without a word - and once that old head
 * was unregistered, the scan never met it again and removed everything in the list.
 * It put the scheduler it found back unconditionally too, reverting one the host set
 * meanwhile. */
static void addon_unwind(gptps *e, const void *tag)
{
    gptps_observer   *odead = NULL, **op;
    gptps_constraint *cdead = NULL, **cp;
    gptps_reg *r;
    gptps_status st;

    gptps_mutex_lock(e->m);
    for (op = &e->observers; *op; ) {
        gptps_observer *o = *op;
        if (o->load_tag != tag) { op = &o->next; continue; }
        *op = o->next; o->next = odead; odead = o;
    }
    for (cp = &e->constraints; *cp; ) {
        gptps_constraint *c = *cp;
        if (c->load_tag != tag) { cp = &c->next; continue; }
        *cp = c->next; c->next = cdead; cdead = c;
    }
    /* The seam, if this setup was the last to change it: put back what it found,
     * function AND owner label. Restoring only the function left a live mismatch
     * whenever a scheduler already existed: the incumbent's fn came back under the
     * FAILED add-on's label, so gptps_scheduler_owner named an add-on that is not
     * installed, and the real owner could no longer release its own seam
     * (gptps_set_scheduler_ex's `self` test compares owner strings, so it saw a
     * stranger and returned E_BUSY) - the "two parties, both believe they hold it"
     * failure this seam's ownership rules exist to eliminate. A seam someone else set
     * since is theirs, and stays. */
    if (e->sched_tag == tag) {
        e->sched_fn = e->sched_undo_fn; e->sched_ud = e->sched_undo_ud;
        memcpy(e->sched_owner, e->sched_undo_owner, sizeof e->sched_owner);
        e->sched_tag = NULL;
        gptps_cond_signal(e->cv_disp);   /* re-evaluate ordering on the next pass */
    }
    gptps_mutex_unlock(e->m);

    while (odead) { gptps_observer   *n = odead->next; gptps_free(odead); odead = n; }
    while (cdead) { gptps_constraint *n = cdead->next; gptps_free(cdead); cdead = n; }

    /* Unregister the task types this setup added, one at a time: snapshot the name
     * of the oldest one left under e->m, then unregister it with the lock released
     * (unregister_task takes e->m itself), and rescan.
     *
     * This used to buffer up to 16 names into a fixed array, so an add-on that
     * registered more than that before failing left the surplus types live and
     * submittable while the host had been told the load failed. Rescanning has no
     * cap and needs no allocation, which matters on a path that is often reached
     * BECAUSE memory ran out. Bail out if a removal refuses, rather than spinning on
     * it. The removal checks the tag again under the lock, so a type a host thread
     * registers under the same name, once it has removed the add-on's, stays. */
    for (;;) {
        char name[GPTPS_TASK_NAME_MAX + 1];
        gptps_reg *mine = NULL;
        gptps_mutex_lock(e->m);
        for (r = e->registry; r; r = r->next)            /* newest first: the last one found is the oldest */
            if (r->load_tag == tag && !r->removed) mine = r;
        if (!mine) { gptps_mutex_unlock(e->m); break; }
        snprintf(name, sizeof name, "%s", mine->name);
        gptps_mutex_unlock(e->m);
        st = unregister_task(e, name, GPTPS_REMOVE_CANCEL, tag);
        /* NOTFOUND: another thread removed it first - the next scan passes it over */
        if (st != GPTPS_OK && st != GPTPS_E_NOTFOUND) break;
    }
}

gptps_status gptps_load_addon(gptps *e, const char *path)
{
    gptps_dl *dl;
    void *sym;
    gptps_addon_init_fn init;
    const gptps_addon *addon;
    gptps_loaded *node;
    char *err = NULL;
    gptps_status s;
    const char *ns = NULL;
    gptps_cb_thread *in;

    if (!e || !path) return GPTPS_E_INVAL;
    if (bounded_sealed(e)) return GPTPS_E_BUSY;   /* bounded: setup ended at the first submit */

    /* Claim the loader. Serialising the whole load is what makes the namespace
     * window and the token claim actually hold: there is one window, so two
     * concurrent loads would clobber it and enforcement would fail OPEN, and the
     * "is this token taken?" scan is otherwise a check-then-act with a gap.
     *
     * Also fail fast on the two states every sibling entry point already refuses:
     * a shutting-down engine (its threads are joining, and an add-on registering
     * into that is a race with teardown), and an engine INHERITED across fork,
     * whose mutex may be held by a thread that did not survive. */
    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
    gptps_mutex_lock(e->m);
    if (e->stopping)                                  { gptps_mutex_unlock(e->m); return GPTPS_E_SHUTDOWN; }
    if (e->loading)                                   { gptps_mutex_unlock(e->m); return GPTPS_E_BUSY; }
    e->loading = 1;
    gptps_mutex_unlock(e->m);

#define LOAD_RELEASE() do { gptps_mutex_lock(e->m); e->loading = 0; gptps_mutex_unlock(e->m); } while (0)
#define LOAD_FAIL(st)  do { LOAD_RELEASE(); return (st); } while (0)

    dl = gptps_dl_open(path);
    if (!dl) LOAD_FAIL(GPTPS_E_IO);

    sym = gptps_dl_sym(dl, "gptps_addon_init");
    if (!sym) { gptps_dl_close(dl); LOAD_FAIL(GPTPS_E_ABI); }
    memcpy(&init, &sym, sizeof init); /* portable object-ptr -> function-ptr */

    addon = init(&G_API);
    if (!addon ||
        addon->magic != GPTPS_ABI_MAGIC ||
        addon->abi_version_major != GPTPS_ABI_VERSION_MAJOR ||
        addon->struct_size < GPTPS_ADDON_MIN_SIZE ||   /* ABI: append-safe floor */
        !addon->name) {                                /* reported by introspection */
        gptps_dl_close(dl);
        LOAD_FAIL(GPTPS_E_ABI);
    }
    /* Built against a NEWER minor than this core. Append-only means we can still use
     * it - we simply ignore the fields past our size - but that graceful degradation
     * should not be invisible: an operator debugging "why is my add-on's new feature
     * doing nothing" deserves the hint. */
    if (addon->struct_size > sizeof(gptps_addon)) {
        char msg[160];
        snprintf(msg, sizeof msg,
                 "add-on '%s' was built against a newer ABI minor (descriptor %u bytes vs %u); "
                 "fields past this core's size are ignored",
                 addon->name, (unsigned)addon->struct_size, (unsigned)sizeof(gptps_addon));
        gptps_log(NULL, GPTPS_LOG_WARN, msg);
    }

    /* Claim the namespace token, if it declared one. Claiming is what makes the
     * guarantee real: a second add-on wanting the same token is refused here rather
     * than silently shadowing the first. */
    if (GPTPS_STRUCT_HAS(gptps_addon, addon, ns) && addon->ns) {
        gptps_loaded *it;
        if (!ns_token_valid(addon->ns)) { gptps_dl_close(dl); LOAD_FAIL(GPTPS_E_ABI); }
        gptps_mutex_lock(e->m);
        for (it = e->addons; it; it = it->next) {
            const gptps_addon *o = it->addon;
            if (GPTPS_STRUCT_HAS(gptps_addon, o, ns) && o->ns && strcmp(o->ns, addon->ns) == 0) {
                gptps_mutex_unlock(e->m);
                gptps_dl_close(dl);
                LOAD_FAIL(GPTPS_E_DUP);
            }
        }
        gptps_mutex_unlock(e->m);
        ns = addon->ns;
    }

    /* The bookkeeping is made BEFORE setup() runs. Once setup has succeeded nothing
     * may fail: what it registered - tasks, observers, settings, the scheduler seam -
     * stays live, so a load that failed after it reported GPTPS_E_NOMEM for an add-on
     * that was running all the same, and a second load was refused GPTPS_E_DUP on
     * its own task. */
    node = (gptps_loaded *)gptps_calloc(1, sizeof *node);
    if (node) {
        size_t n = strlen(path) + 1;
        node->path = (char *)gptps_malloc(n);
        if (node->path) memcpy(node->path, path, n);
    }
    if (!node || !node->path) {
        if (node) gptps_free(node->path);
        gptps_free(node);
        gptps_dl_close(dl);                  /* nothing of the add-on has run but its init */
        LOAD_FAIL(GPTPS_E_NOMEM);
    }

    if (addon->setup) {
        /* What setup() registers carries this load's tag, so a PARTIAL setup is
         * undone rather than left live (addon_unwind). A failed load is a status a
         * host reasonably logs and continues past ("running without add-on X"), so it
         * must leave the engine in a consistent state - not one event away from a
         * wild jump. */
        const void       *tag;          /* this load's: what its setup registers carries it */

        gptps_mutex_lock(e->m);
        /* Open the namespace window, pinned to this thread. */
        e->cur_ns = ns; e->cur_ns_len = ns ? strlen(ns) : 0;
        e->cur_ns_tid = gptps_hal_thread_id();
        e->setup_tid = e->cur_ns_tid; e->setup_on = 1;
        e->setup_tag = (const void *)++e->load_seq;   /* never 0, never reused in this engine */
        tag = e->setup_tag;
        in = cb_enter_locked(e);     /* setup() and what it emits run on this thread */
        gptps_mutex_unlock(e->m);

        s = addon->setup(e, &G_API, &err);
        cb_leave(in);

        /* A failed add-on's code must not run again: the setup that would have made it
         * ready did not finish. Its watchers hear nothing more, and the settings it
         * registered go - their accessors and targets are its own. Done while the load
         * still counts as running, so no value applied meanwhile reaches either. */
        if (s != GPTPS_OK) gptps_settings_forget_tag(e->settings, tag);
        gptps_mutex_lock(e->m);
        e->cur_ns = NULL; e->cur_ns_len = 0; e->cur_ns_tid = 0;
        e->setup_on = 0; e->setup_tid = 0; e->setup_tag = NULL;
        gptps_mutex_unlock(e->m);
        /* The file's values that waited: for what the add-on defined, now its setup is
         * done - a plug-in learns its configuration through the watcher it registers
         * in setup (addons/gptps_gpu_quota_plugin.c), and a value applied while setup
         * still ran would have reached no watcher - and for whatever another thread
         * defined meanwhile. */
        cfg_apply_pending(e, "");

        if (s != GPTPS_OK) {
            addon_unwind(e, tag);
            /* An add-on may report WHY it failed through err_out. Surface it - it is
             * the only diagnostic channel a plug-in has, and the header promises it -
             * then free it, since the contract is that the add-on hands ownership over. */
            if (err) {
                char msg[GPTPS_EV_NAME_MAX + 160];
                snprintf(msg, sizeof msg, "add-on '%s' setup failed: %s",
                         addon->name, err);
                gptps_log(NULL, GPTPS_LOG_ERROR, msg);
                gptps_free(err);
                err = NULL;
            }
            /* Deliberately NOT gptps_dl_close(dl). A partially-initialised add-on can
             * still have left pointers the unwind cannot reach - a settings entry's
             * read/write pair, a per-task setting schema, a named resource. Unmapping
             * the library would turn every one of those into a wild jump; retaining a
             * single mapping on a path that has already failed is the safer trade.
             *
             * The MAPPING is what must survive, though - not the handle wrapper, which
             * nothing references once this returns. Releasing it keeps the deliberate
             * decision exact and this path leak-free. */
            gptps_dl_release(dl);
            gptps_free(node->path);
            gptps_free(node);
            LOAD_FAIL(s);
        }
    }

    node->dl = dl; node->addon = addon; node->enabled = 1;
    gptps_mutex_lock(e->m);
    node->next = e->addons; e->addons = node;
    e->loading = 0;                      /* published and released in one critical section */
    gptps_mutex_unlock(e->m);
    return GPTPS_OK;
#undef LOAD_RELEASE
#undef LOAD_FAIL
}

size_t gptps_addon_count(gptps *e)
{
    size_t n = 0;
    gptps_loaded *a;
    if (!e) return 0;
    GPTPS_REFUSE_AFTER_FORK(e, 0);
    gptps_mutex_lock(e->m);
    for (a = e->addons; a; a = a->next) ++n;
    gptps_mutex_unlock(e->m);
    return n;
}

gptps_status gptps_addon_get_info(gptps *e, size_t index, gptps_addon_info *out)
{
    gptps_loaded *a;
    size_t i = 0;
    if (!e || !out) return GPTPS_E_INVAL;
    if (out->struct_size < GPTPS_ADDON_INFO_MIN_SIZE) return GPTPS_E_INVAL; /* frozen floor, not sizeof */
    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
    gptps_mutex_lock(e->m);
    for (a = e->addons; a; a = a->next, ++i) {
        if (i != index) continue;
        out->name              = a->addon->name;
        out->ns                = GPTPS_STRUCT_HAS(gptps_addon, a->addon, ns) ? a->addon->ns : NULL;
        out->path              = a->path;
        out->seam              = a->addon->seam;
        out->abi_version_major = a->addon->abi_version_major;
        out->enabled           = a->enabled;
        gptps_mutex_unlock(e->m);
        return GPTPS_OK;
    }
    gptps_mutex_unlock(e->m);
    return GPTPS_E_NOTFOUND;
}

gptps_status gptps_addon_disable(gptps *e, const char *ns_or_name)
{
    gptps_loaded *a, *hit = NULL;
    gptps_status (*fn)(gptps *) = NULL;
    gptps_status st;
    gptps_cb_thread *in;

    if (!e || !ns_or_name) return GPTPS_E_INVAL;

    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
    gptps_mutex_lock(e->m);
    /* Same two fail-fasts every sibling entry point has: a shutting-down engine is
     * tearing add-ons down already, and an engine inherited across fork may have its
     * mutex held by a thread that did not survive. */
    if (e->stopping)                                { gptps_mutex_unlock(e->m); return GPTPS_E_SHUTDOWN; }
    /* namespace first (the unambiguous identity), then the self-declared name */
    for (a = e->addons; a && !hit; a = a->next) {
        const gptps_addon *ad = a->addon;
        if (GPTPS_STRUCT_HAS(gptps_addon, ad, ns) && ad->ns && strcmp(ad->ns, ns_or_name) == 0)
            hit = a;
    }
    for (a = e->addons; a && !hit; a = a->next)
        if (a->addon->name && strcmp(a->addon->name, ns_or_name) == 0) hit = a;

    if (!hit)          { gptps_mutex_unlock(e->m); return GPTPS_E_NOTFOUND; }
    if (!hit->enabled) { gptps_mutex_unlock(e->m); return GPTPS_OK; }  /* already disabled */
    if (!GPTPS_STRUCT_HAS(gptps_addon, hit->addon, disable) || !hit->addon->disable) {
        gptps_mutex_unlock(e->m);
        return GPTPS_E_INVAL;   /* matched, but it declares no disable hook */
    }
    fn = hit->addon->disable;
    /* Claim the transition BEFORE releasing the lock. Setting enabled = 0 only after
     * the hook returned made the documented idempotence false: two concurrent callers
     * both saw enabled == 1 and both ran the hook, which for a well-written disable()
     * means a double unregister. Claiming here makes the second caller take the
     * already-disabled path above. On failure it is restored below, so a hook that
     * declines leaves the add-on enabled, as it should. */
    hit->enabled = 0;
    in = cb_enter_locked(e);            /* disable() and what it calls run on this thread */
    gptps_mutex_unlock(e->m);

    /* Call OUTSIDE the lock: disable() unregisters its own observers/constraints/
     * tasks, every one of which takes e->m itself.
     *
     * `hit` stays valid across this window by the same contract that makes the whole
     * add-on model safe: an add-on's node and its mapping live for the engine's
     * lifetime, and gptps_shutdown - the only thing that frees them - is refused
     * re-entrantly and is a caller error to run concurrently with setup-time calls
     * like this one. The e->stopping check above is the fail-fast for the honest
     * mistake. */
    st = fn(e);
    cb_leave(in);
    if (st != GPTPS_OK) {
        gptps_mutex_lock(e->m);
        hit->enabled = 1;               /* the hook declined; it is still participating */
        gptps_mutex_unlock(e->m);
        return st;
    }
    return GPTPS_OK;
}

/* ------------------------------------------------------------------------- */
/* bounded mode (docs/BOUNDED.md)                                            */
/* ------------------------------------------------------------------------- */
#define GPTPS_BOUNDED_HOST_THREADS 32u      /* callback records for host threads */

static int size_mul(size_t a, size_t b, size_t *out)    /* a * b, or -1 on overflow */
{
    if (a && b > (size_t)-1 / a) return -1;
    *out = a * b;
    return 0;
}

/* The first submit to a bounded engine allocates its whole working set, here, sized
 * by the config and by what setup defined: max_items items, each owning a payload
 * slot and a named-resource snapshot slot at its own index; the handle index at its
 * final size (at least twice max_items, so its load stays at most a half, and
 * deletion leaves no tombstone - it is never rebuilt); a result buffer per executing
 * thread; and callback-thread records for every engine thread plus
 * GPTPS_BOUNDED_HOST_THREADS host threads. All or nothing: a failure frees what this
 * call made and leaves the engine unsealed. Slots are 16-byte aligned, so a task may
 * read a payload as a struct. e->m held. */
static gptps_status bounded_seal(gptps *e)
{
    size_t n, items_sz, pay_sz, snap_sz, res_sz, nn = 64, i, ncb;
    gptps_hslot *idx = NULL;
    if (e->sealed) return GPTPS_OK;
    if (e->max_items > (uint64_t)((size_t)-1 / 4)) return GPTPS_E_NOMEM;
    n = (size_t)e->max_items;
    e->payload_stride = ((size_t)e->max_payload + 15u) / 16u * 16u;
    e->result_stride  = ((size_t)e->max_result + 15u) / 16u * 16u;
    e->pool_nres = e->nres;
    if (size_mul(n, sizeof *e->pool, &items_sz) || size_mul(n, e->payload_stride, &pay_sz) ||
        size_mul(n, e->pool_nres, &snap_sz) || size_mul(snap_sz, sizeof(uint64_t), &snap_sz) ||
        size_mul((size_t)e->nworkers + 1u, e->result_stride, &res_sz))
        return GPTPS_E_NOMEM;
    while (nn < 2 * n) nn <<= 1;
    ncb = (size_t)e->nworkers + 1u + GPTPS_BOUNDED_HOST_THREADS;

    e->pool          = (gptps_item *)gptps_calloc(n, sizeof *e->pool);
    e->payload_arena = pay_sz  ? (unsigned char *)gptps_malloc(pay_sz)  : NULL;
    e->snap_arena    = snap_sz ? (uint64_t *)gptps_malloc(snap_sz)      : NULL;
    e->result_arena  = res_sz  ? (unsigned char *)gptps_malloc(res_sz)  : NULL;
    idx              = (gptps_hslot *)gptps_calloc(nn, sizeof *idx);
    if (!e->pool || (pay_sz && !e->payload_arena) || (snap_sz && !e->snap_arena) ||
        (res_sz && !e->result_arena) || !idx)
        goto fail;
    for (i = 0; i < ncb; ++i) {
        gptps_cb_thread *t = (gptps_cb_thread *)gptps_calloc(1, sizeof *t);
        if (!t) goto fail;
        t->next = e->cb_spare;
        e->cb_spare = t;
    }
    for (i = n; i-- > 0; ) {
        e->pool[i].next = e->free_items;
        e->free_items = &e->pool[i];
    }
    gptps_free(e->hidx);                    /* none yet: nothing was submitted before the seal */
    e->hidx = idx; e->nhidx = nn; e->hidx_live = 0;
    gptps_hal_store_release_u32(&e->sealed, 1u);
    return GPTPS_OK;

fail:
    while (e->cb_spare) { gptps_cb_thread *t = e->cb_spare; e->cb_spare = t->next; gptps_free(t); }
    gptps_free(idx);
    gptps_free(e->result_arena);  e->result_arena = NULL;
    gptps_free(e->snap_arena);    e->snap_arena = NULL;
    gptps_free(e->payload_arena); e->payload_arena = NULL;
    gptps_free(e->pool);          e->pool = NULL;
    return GPTPS_E_NOMEM;
}

/* A free item from a bounded engine's pool, zeroed, owning its payload slot; NULL
 * when all are in use. Its own lock, not e->m: the caller copies the payload before
 * it takes e->m, as the classic submit copies before it does. */
static gptps_item *pool_take(gptps *e)
{
    gptps_item *it;
    size_t i;
    gptps_mutex_lock(e->pool_m);
    it = e->free_items;
    if (it) e->free_items = it->next;
    gptps_mutex_unlock(e->pool_m);
    if (!it) return NULL;
    i = (size_t)(it - e->pool);
    memset(it, 0, sizeof *it);
    it->pooled = 1;
    it->payload = e->payload_arena ? e->payload_arena + i * e->payload_stride : NULL;
    return it;
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
    if (opts && opts->struct_size < GPTPS_SUBMIT_OPTIONS_MIN_SIZE) return GPTPS_E_INVAL; /* ABI: append-safe floor */
    /* An engine INHERITED across a fork may have its mutex held by a thread that
     * did not survive, so taking it below would block forever. Fail instead of
     * hang. An engine created fresh in the child stamps the new generation and is
     * unaffected. */
    if (e->fork_gen != gptps_hal_fork_generation()) return GPTPS_E_SHUTDOWN;

    /* Copy the payload + allocate the item OUTSIDE the engine lock: none of it needs
     * engine state, and keeping it off-lock shortens the critical section every
     * producer contends on - a real win for large payloads / many concurrent
     * submitters (and for each pool shard). item_free cleans up uniformly if a check
     * below rejects the submit. (A reject now does a wasted copy, but rejects are the
     * rare path; the common accept path wins.) A bounded engine takes the item from
     * its pool instead, and copies into the item's own slot: nothing allocates. */
    if (e->max_items) {
        if (len > e->max_payload) return GPTPS_E_INVAL;
        if (!gptps_hal_load_acquire_u32(&e->sealed)) {
            /* The first submit that names a task allocates it all. One that names
             * no registered task seals nothing: a typo in it must not end setup. */
            gptps_status ss;
            gptps_reg *r0;
            gptps_mutex_lock(e->m);
            r0 = e->stopping ? NULL : registry_find(e, task_name);
            ss = e->stopping            ? GPTPS_E_SHUTDOWN
               : (!r0 || !r0->enabled)  ? GPTPS_E_NOTFOUND
               :                          bounded_seal(e);
            gptps_mutex_unlock(e->m);
            if (ss != GPTPS_OK) return ss;
        }
        it = pool_take(e);
        if (!it) return GPTPS_E_FULL;
        if (len) memcpy(it->payload, payload, len);
        else     it->payload = NULL;
        it->payload_len = len;
    } else {
        if (len) {
            pcopy = gptps_malloc(len);
            if (!pcopy) return GPTPS_E_NOMEM;
            memcpy(pcopy, payload, len);
        }
        it = (gptps_item *)gptps_calloc(1, sizeof *it);   /* zeroed: the cancel word starts clear */
        if (!it) { gptps_free(pcopy); return GPTPS_E_NOMEM; }
        it->payload = pcopy;          /* set now so item_free frees it on any reject below */
        it->payload_len = len;
    }

    gptps_mutex_lock(e->m);
    if (e->stopping) { gptps_mutex_unlock(e->m); item_free(e, it); return GPTPS_E_SHUTDOWN; }

    r = registry_find(e, task_name);
    if (!r || !r->enabled) { gptps_mutex_unlock(e->m); item_free(e, it); return GPTPS_E_NOTFOUND; } /* unknown, draining, paused, or still registering */

    cost = r->def.default_cost;
    if (r->def.cost) {
        gptps_status cs = r->def.cost(payload, len, &cost, r->def.user_data);
        if (cs != GPTPS_OK) { gptps_mutex_unlock(e->m); item_free(e, it); return cs; }
    }
    if (cost.mem_bytes > e->limits.max_memory_bytes) {
        gptps_mutex_unlock(e->m); item_free(e, it);
        return GPTPS_E_BUDGET; /* never-fits: reject at submit */
    }
    if (e->nres && r->res_cost) {           /* a resource cost that can never fit its budget */
        size_t ri;
        for (ri = 0; ri < e->nres; ++ri)
            if (r->res_cost[ri] > e->resources[ri].budget) { gptps_mutex_unlock(e->m); item_free(e, it); return GPTPS_E_BUDGET; }
    }
    /* backpressure: bound the intake queue so an overproducing client cannot grow
     * it without limit (max_memory_bytes bounds only the RUNNING set). 0 = off. */
    if (e->limits.max_intake_depth && e->intake.count >= e->limits.max_intake_depth) {
        gptps_mutex_unlock(e->m); item_free(e, it);
        return GPTPS_E_FULL;
    }
    /* Room in the handle index first (see "finding an item"): past this point the
     * item is reachable, and a failure would leave it uncancellable. */
    if (hidx_reserve(e) != 0) { gptps_mutex_unlock(e->m); item_free(e, it); return GPTPS_E_NOMEM; }

    it->handle = e->next_handle++;
    it->def = &r->def;
    it->reg = r;
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

    intake_insert(e, it);   /* by admission order, so the dispatcher need not search */
    hidx_put(e, it);
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
        gptps_cb_thread *in = NULL;
        p.kind = GPTPS_EV_QUEUED; p.handle = h; ev_set_name(p.name, r->def.name);
        p.status = GPTPS_OK; p.attempt = 0; p.mem = mem;
        p.result = NULL; p.result_len = 0; p.flags = 0;
        /* Once: see "the config file" - and not while the open is still reading it, as
         * when an add-on submits as it loads, since its keys are not claimed yet. The
         * report itself is made elsewhere: it consults the settings registry, and this
         * submit may come from a setting's write accessor, which runs holding the
         * registry's lock. */
        if (e->toml && !e->cfg_opening && !e->cfg_reported) {
            e->cfg_reported = 1;
            e->cfg_report_due = 1;          /* the dispatcher, signalled above, or gptps_step */
        }
        if (cb || e->observers) in = cb_enter_locked(e);   /* else nothing to bracket */
        gptps_mutex_unlock(e->m);
        emit_now(e, cb, ud, &p);
        cb_leave(in);
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
    gptps_hslot *slot;
    gptps_item *it;
    gptps_pending_ev p;
    gptps_event_cb cb = NULL;
    void *ud = NULL;
    gptps_cb_thread *in;

    if (!e || h == 0) return GPTPS_E_INVAL;
    if (e->fork_gen != gptps_hal_fork_generation()) return GPTPS_E_SHUTDOWN; /* see submit_internal */

    gptps_mutex_lock(e->m);
    if (e->stopping) { gptps_mutex_unlock(e->m); return GPTPS_E_SHUTDOWN; }

    /* Straight to the item, wherever it is (see "finding an item"). */
    slot = hidx_find(e, h);
    it = slot ? slot->it : NULL;
    switch (it ? it->where : GPTPS_Q_NONE) {
    case GPTPS_Q_RUNNING:
        /* in flight: mark cancelled and raise the cooperative flag. The worker carries
         * it to terminal (a running in-proc item observes the flag). No budget
         * bookkeeping here - done-processing releases it. */
        it->cancelled = 1; cancel_raise(it);
        gptps_mutex_unlock(e->m); return GPTPS_OK;
    case GPTPS_Q_READY:
        /* admitted but not started: a worker discards it before it starts */
        it->cancelled = 1; cancel_raise(it);
        gptps_cond_broadcast(e->cv_work);   /* wake a worker to discard it */
        gptps_mutex_unlock(e->m); return GPTPS_OK;
    case GPTPS_Q_DONE:
        /* finished-but-not-yet-reaped (in `done`): a worker has posted it and the
         * dispatcher has not made its terminal decision yet. Mark cancelled so that
         * decision frees it instead of retrying / REQUEUEing - without this a crash-
         * restarting service momentarily in `done` would dodge the cancel and restart.
         * Budget is released by the done-drain, so no ledger bookkeeping here. */
        it->cancelled = 1; cancel_raise(it);
        gptps_mutex_unlock(e->m); return GPTPS_OK;
    case GPTPS_Q_INTAKE:
        intake_unlink(e, it);   /* repairs the run cache: the next submit stays O(1) */
        break;
    case GPTPS_Q_DELAYED:
        fifo_remove(&e->delayed, it);
        break;
    default:
        gptps_mutex_unlock(e->m);
        return GPTPS_E_NOTFOUND;   /* unknown handle, or already terminal */
    }

    /* queued (intake) or backoff-delayed: not admitted, no budget reserved -
     * removed above; free it and emit a terminal cancelled event with the lock released. */
    p.kind = GPTPS_EV_FAILED; p.handle = h; ev_set_name(p.name, item_name(it));
    p.status = GPTPS_E_CANCELLED; p.attempt = it->attempt; p.mem = it->cost.mem_bytes;
    p.result = NULL; p.result_len = 0; p.flags = 0;
    cb = e->ev_cb; ud = e->ev_ud;
    item_drop(e, it);
    /* This can be the last live reference to a draining task type, and
     * cv_drain is otherwise only broadcast from a dispatcher pass - so a
     * blocked gptps_unregister_task(DRAIN) would sleep until some
     * unrelated event happened to wake the dispatcher, or forever on an
     * idle engine. Waking cv_disp matters too: the cancelled item may be
     * the reserved `top` that was holding back skip-to-fit backfill. */
    gptps_cond_broadcast(e->cv_drain);
    gptps_cond_signal(e->cv_disp);
    in = cb_enter_locked(e);
    gptps_mutex_unlock(e->m);
    emit_now(e, cb, ud, &p);        /* lock released: observers may re-enter */
    cb_leave(in);
    return GPTPS_OK;
}

gptps_status gptps_set_event_cb(gptps *e, gptps_event_cb cb, void *user_data)
{
    if (!e) return GPTPS_E_INVAL;
    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
    gptps_mutex_lock(e->m);
    e->ev_cb = cb; e->ev_ud = user_data;
    gptps_mutex_unlock(e->m);
    return GPTPS_OK;
}

/* Scheduler seam: swap the admission ORDERING key (fn == NULL resets to the
 * built-in priority ordering). Setup-time; the hook runs under e->m on the
 * dispatcher hot path (must be fast / non-reentrant). */
gptps_status gptps_set_scheduler_ex(gptps *e, gptps_sched_fn fn, void *user_data,
                                    const char *owner, unsigned flags)
{
    if (!e) return GPTPS_E_INVAL;
    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
    gptps_mutex_lock(e->m);
    /* An ordering key is a total order, and a total order has one definition. Two
     * independent scorers is a CONFLICT, not something to silently merge - so unless
     * the caller explicitly asks to take it, an owned seam is reported as busy and
     * the incumbent keeps ordering. Releasing (fn == NULL) is allowed to the current
     * owner without the flag, so an add-on's disable() can hand the seam back. */
    if (!(flags & GPTPS_SCHED_REPLACE) && e->sched_fn) {
        int self = (fn == NULL && owner && e->sched_owner[0] && strcmp(owner, e->sched_owner) == 0);
        if (!self) { gptps_mutex_unlock(e->m); return GPTPS_E_BUSY; }
    }
    {   /* An add-on's setup changing it keeps what to put back if that setup fails -
         * the seam as it was before, unless this load already holds that record. Any
         * other change makes the seam no load's to put back. */
        const void *tag = setup_tag_here(e);
        if (tag && e->sched_tag != tag) {
            e->sched_undo_fn = e->sched_fn; e->sched_undo_ud = e->sched_ud;
            memcpy(e->sched_undo_owner, e->sched_owner, sizeof e->sched_undo_owner);
        }
        e->sched_tag = tag;
    }
    e->sched_fn = fn; e->sched_ud = user_data;
    /* Copy, do not borrow - see the field's declaration. */
    if (fn && owner) {
        size_t n = strlen(owner);
        if (n >= sizeof e->sched_owner) n = sizeof e->sched_owner - 1;
        memcpy(e->sched_owner, owner, n);
        e->sched_owner[n] = '\0';
    } else {
        e->sched_owner[0] = '\0';
    }
    gptps_cond_signal(e->cv_disp);   /* re-evaluate ordering on the next pass */
    gptps_mutex_unlock(e->m);
    return GPTPS_OK;
}

const char *gptps_scheduler_owner(gptps *e)
{
    const char *o;
    if (!e) return NULL;
    GPTPS_REFUSE_AFTER_FORK(e, NULL);
    gptps_mutex_lock(e->m);
    o = (e->sched_fn && e->sched_owner[0]) ? e->sched_owner : NULL;
    gptps_mutex_unlock(e->m);
    return o;
}

gptps_status gptps_set_scheduler(gptps *e, gptps_sched_fn fn, void *user_data)
{
    /* Unchanged behaviour: the host takes the seam regardless. Every existing
     * caller, and tests/test_sched_seam.c, keep working exactly as before. */
    return gptps_set_scheduler_ex(e, fn, user_data, "host", GPTPS_SCHED_REPLACE);
}

gptps_status gptps_register_observer(gptps *e, gptps_event_cb fn, void *user_data)
{
    gptps_observer *o;
    if (!e || !fn) return GPTPS_E_INVAL;
    if (bounded_sealed(e)) return GPTPS_E_BUSY;   /* bounded: setup ended at the first submit */
    o = (gptps_observer *)gptps_calloc(1, sizeof *o);
    if (!o) return GPTPS_E_NOMEM;
    o->fn = fn; o->ud = user_data;
    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
    gptps_mutex_lock(e->m);
    o->load_tag = setup_tag_here(e);    /* an add-on's: undone if its setup fails */
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
    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
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
    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
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
    if (bounded_sealed(e)) return GPTPS_E_BUSY;   /* bounded: setup ended at the first submit */
    c = (gptps_constraint *)gptps_calloc(1, sizeof *c);
    if (!c) return GPTPS_E_NOMEM;
    c->fn = fn; c->ud = user_data;
    GPTPS_REFUSE_AFTER_FORK(e, GPTPS_E_SHUTDOWN);
    gptps_mutex_lock(e->m);
    c->load_tag = setup_tag_here(e);    /* an add-on's: undone if its setup fails */
    c->next = e->constraints; e->constraints = c;
    gptps_mutex_unlock(e->m);
    return GPTPS_OK;
}

size_t gptps_dead_letter_count(gptps *e)
{
    size_t n;
    if (!e) return 0;
    GPTPS_REFUSE_AFTER_FORK(e, 0);
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
    gptps_cb_thread *in = NULL;
    int named;

    if (!e) return 0;

    /* Take the list under the lock, then iterate with the lock RELEASED so the callback
     * may re-enter the engine (e.g. re-submit) without deadlock. The items taken are
     * then invisible to detach_dead_letter, which is what would normally give them an
     * owned name before their task type is freed - and the callback is explicitly
     * allowed to call gptps_unregister_task on the very type these items came from. So
     * each gets its own copy of its name NOW, while the regs are still alive. A copy
     * that cannot be made stops the drain at that item: it and the ones after it stay
     * retained, in order, for the next drain, the ones before it reach the callback as
     * usual, and the count returned is short of what gptps_dead_letter_count said.
     * They used to reach the callback named "?". Without a callback no name is needed;
     * and a sealed bounded engine refuses that unregister, so its regs outlive the
     * callback, and the copies - an allocation per item - are not made there. */
    GPTPS_REFUSE_AFTER_FORK(e, 0);
    gptps_mutex_lock(e->m);
    local.head = local.tail = NULL; local.count = 0; local.id = GPTPS_Q_NONE;
    named = !cb || bounded_sealed(e);
    while ((it = e->dead_letter.head) != NULL) {
        if (!named && !item_own_name(it)) break;
        fifo_pop(&e->dead_letter);
        e->dead_letter_count -= 1;
        fifo_push(&local, it);
    }
    if (cb) in = cb_enter_locked(e);
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
        item_free(e, it);
        ++n;
    }
    cb_leave(in);
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
    int npend, more = 0;
    uint64_t next_wake;
    size_t ran = 0;
    gptps_item *it;

    if (out_ran) *out_ran = 0;
    if (!e) return GPTPS_E_INVAL;
    if (!e->manual) return GPTPS_E_INVAL;   /* threaded engines run themselves */
    if (e->fork_gen != gptps_hal_fork_generation()) return GPTPS_E_SHUTDOWN; /* see submit_internal */

    gptps_mutex_lock(e->m);
    /* Refuse a re-entrant pump: a task body or event callback calling gptps_step
     * would recursively drain queues the outer step is still walking - or, from a
     * callback made outside any step (the QUEUED of a gptps_submit, a drain), run
     * the whole engine inside the call that made the callback. */
    if (e->step_tid != 0 || engine_in_callback(e, gptps_hal_thread_id())) {
        gptps_mutex_unlock(e->m); return GPTPS_E_BUSY;
    }
    e->step_tid = gptps_hal_thread_id();
    if (e->cfg_report_due) {                 /* asked for by the first submit; not in a callback here */
        e->cfg_report_due = 0;
        gptps_mutex_unlock(e->m);
        cfg_report_first_submit(e, 0);       /* the stepper is the dispatcher: see dispatcher_main */
        gptps_mutex_lock(e->m);
    }

    /* pass A: complete any prior work, promote backoff-ready retries, admit.
     * Repeat while the pass reports work still owed: a terminal event it could not
     * buffer (a single gptps_step must not leave a handle without one), or a
     * zero-backoff retry it announced, which the next pass promotes and admits once
     * its RETRIED is out (engine_pass 2b). */
    do {
        engine_pass(e, pend, &npend, &next_wake, &more);
        flush_pending(e, pend, &npend);
    } while (more);

run_ready:
    /* run everything admitted into `ready` to completion, inline on THIS thread */
    while ((it = fifo_pop(&e->ready)) != NULL) {
        gptps_status eff;
        gptps_event_cb cb = e->ev_cb; void *ud = e->ev_ud;  /* snapshot under lock */
        /* Same guard as the threaded worker: an event callback may have re-entered
         * and cancelled this admitted-but-unstarted item (or CANCELled its type)
         * during the emit above - starting it anyway would reset its cancel flag
         * and let a cooperative task spin forever. */
        if (it->cancelled || (it->reg && it->reg->removed && it->reg->cancelling)) {
            it->outcome = GPTPS_E_CANCELLED;
            fifo_push(&e->done, it);
            continue;
        }
        it->deadline_ms = attempt_deadline(it);
        cancel_clear(it);
        it->started = 1;                   /* execute() will emit STARTED + a terminal event */
        fifo_push(&e->running_items, it);
        gptps_mutex_unlock(e->m);

        eff = execute(e, it, cb, ud, e->nworkers);   /* STARTED + FINISHED/FAILED emitted here */

        gptps_mutex_lock(e->m);
        fifo_remove(&e->running_items, it);
        it->outcome = eff;
        fifo_push(&e->done, it);
        ++ran;
    }

    /* pass B: account the work just run (release budget, schedule retries / dead-letter). */
    do {
        engine_pass(e, pend, &npend, &next_wake, &more);
        flush_pending(e, pend, &npend);
    } while (more);

    /* Pass B admits too, and can admit what pass A could not: an admission whose
     * named-resource snapshot could not be allocated stops its pass, and the next one
     * may allocate it. That work runs in this step. Left in `ready`, the step returned
     * 0 with work waiting, and the drain idiom - step while a step runs something -
     * stopped there and left it for gptps_shutdown to cancel. */
    if (!ran && e->ready.head) goto run_ready;

    e->step_tid = 0;
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
 * detached into `out`, and the caller gives each its terminal event.
 * Non-service in-flight work is left alone HERE so it drains gracefully - but it is
 * no longer left alone forever: gptps_shutdown arms e->stop_deadline_ms, and once
 * that grace elapses the dispatcher cancels whatever is still running (engine_pass
 * step 3b). Caller holds e->m. */
static void stop_services(gptps *e, gptps_fifo *out)
{
    gptps_item *it;
    for (it = e->running_items.head; it; it = it->next)
        if (it->reg && it->reg->service) { it->cancelled = 1; cancel_raise(it); }
    for (it = e->ready.head; it; it = it->next)
        if (it->reg && it->reg->service) { it->cancelled = 1; cancel_raise(it); }
    /* A run that already returned and waits in `done` for the dispatcher's verdict:
     * a crash would otherwise be judged there like any failure while stopping - a
     * REQUEUE the drain will not restart, DEAD_LETTERED - instead of ending with the
     * FAILED / GPTPS_E_CANCELLED a service's shutdown owes. gptps_cancel covers
     * `done` for the same reason. */
    for (it = e->done.head; it; it = it->next)
        if (it->reg && it->reg->service) { it->cancelled = 1; cancel_raise(it); }
    fifo_detach_services(&e->intake, out);
    intake_forget(e);
    fifo_detach_services(&e->delayed, out);
    index_drop_list(e, out);          /* the caller drains `out` without the lock */
}

gptps_status gptps_shutdown(gptps *e)
{
    unsigned i;
    gptps_reg *r;
    gptps_item *it;
    gptps_fifo dropped;                 /* queued work this teardown terminated */
    gptps_event_cb cb; void *ud;
    gptps_cb_thread *in;

    if (!e) return GPTPS_E_INVAL;
    /* An engine created before a fork() is unusable in the child (see
     * submit_internal), and shutdown is the worst entry point to let through: it
     * joins dispatcher/worker pthread_t's that do not exist here and frees the
     * engine. Check BEFORE taking e->m - the lock itself may be held by a thread
     * that did not survive the fork. Refusing without freeing is correct: the
     * child's contract is exec()/_exit(). */
    if (e->fork_gen != gptps_hal_fork_generation()) return GPTPS_E_SHUTDOWN;

    gptps_mutex_lock(e->m);
    /* Refuse a re-entrant shutdown. Called from a task body or an event callback,
     * this would join the very thread making the call (THREADED) or free the engine
     * that gptps_step is still standing on (MANUAL) - a deadlock and a
     * use-after-free respectively, both from a call that looks perfectly ordinary.
     * The correct pattern is to signal your main thread and shut down from there.
     * A callback the engine makes on a host thread counts too (engine_in_callback):
     * shutdown frees the engine the calling submit / cancel / drain is still in. */
    if (engine_in_callback(e, gptps_hal_thread_id())) {
        gptps_mutex_unlock(e->m);
        return GPTPS_E_BUSY;
    }
    if (e->cfg_report_due) {            /* a MANUAL engine may never have stepped */
        e->cfg_report_due = 0;
        gptps_mutex_unlock(e->m);
        cfg_report_first_submit(e, 0);
        gptps_mutex_lock(e->m);
    }
    /* This thread's own callbacks below (the terminal events of whatever the
     * teardown cancels) are callbacks like any other: a gptps_shutdown from one
     * would run teardown again, inside itself. */
    in = cb_enter_locked(e);            /* until the add-on teardown below is done */
    e->stopping = true;
    /* Arm the drain bound before waking anyone: in-flight work gets until this
     * instant to finish on its own, after which the dispatcher cancels it. */
    e->stop_deadline_ms = e->shutdown_grace_ms
        ? gptps_hal_monotonic_ms() + (uint64_t)e->shutdown_grace_ms : 0;
    dropped.head = dropped.tail = NULL; dropped.count = 0; dropped.id = GPTPS_Q_NONE;
    stop_services(e, &dropped);         /* cooperatively stop long-running service instances */
    cb = e->ev_cb; ud = e->ev_ud;       /* snapshot under the lock */
    gptps_cond_signal(e->cv_disp);
    gptps_cond_broadcast(e->cv_work);   /* wake idle workers to discard cancelled service instances */
    gptps_mutex_unlock(e->m);

    /* Service instances that were queued or in restart backoff: terminal event now,
     * with the lock released, while their regs are still alive for item_name(). */
    drain_cancelled(e, &dropped, cb, ud);

    if (!e->manual) {                       /* MANUAL spawns no threads to join */
        gptps_thread_join(e->dispatcher);
        for (i = 0; i < e->nworkers; ++i) gptps_thread_join(e->workers[i]);
    }

    /* Whatever the pumps did not get to still owes its observer a terminal event -
     * a MANUAL host that stopped stepping mid-drain, or work left when the grace
     * expired. This MUST run before the add-on teardown below: that loop calls
     * gptps_dl_close(), and an observer registered by an add-on lives in the very
     * .so being unmapped, so emitting afterwards would jump into freed code.
     * drain_cancelled skips only items whose terminal event execute() already
     * reported, so nothing is double-counted - and nothing parked between attempts
     * is dropped. */
    gptps_mutex_lock(e->m);
    while ((it = fifo_pop(&e->intake))        != NULL) fifo_push(&dropped, it);
    intake_forget(e);
    while ((it = fifo_pop(&e->delayed))       != NULL) fifo_push(&dropped, it);
    while ((it = fifo_pop(&e->ready))         != NULL) fifo_push(&dropped, it);
    while ((it = fifo_pop(&e->done))          != NULL) fifo_push(&dropped, it);
    while ((it = fifo_pop(&e->running_items)) != NULL) fifo_push(&dropped, it);
    index_drop_list(e, &dropped);
    cb = e->ev_cb; ud = e->ev_ud;
    gptps_mutex_unlock(e->m);
    drain_cancelled(e, &dropped, cb, ud);

    /* tear down add-ons (threads joined => no task code runs; last calls into
     * each .so, then unload) */
    {
        gptps_loaded *a = e->addons;
        while (a) {
            gptps_loaded *n = a->next;
            /* teardown runs once whether or not the add-on was disabled */
            if (a->addon->teardown) a->addon->teardown(e);
            gptps_dl_close(a->dl);
            gptps_free(a->path);
            gptps_free(a);
            a = n;
        }
    }
    cb_leave(in);                       /* the last host code this teardown calls is done */

    /* Retained dead letters are the one queue the host is not required to drain and
     * whose items have ALREADY had their terminal event; free them without another.
     * The work queues were emptied and reported above, before the add-on unload. */
    while ((it = fifo_pop(&e->dead_letter)) != NULL) item_free(e, it);

    r = e->registry;
    while (r) {
        gptps_reg *n = r->next;
        reg_destroy(r);   /* frees locals + argv_copy + name + r */
        r = n;
    }
    r = e->retired;       /* after the dead letters that named their types through them */
    while (r) {
        gptps_reg *n = r->next;
        reg_destroy(r);
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
    { gptps_res_cell *x = e->res_cells; while (x) { gptps_res_cell *n = x->next; gptps_free(x); x = n; } }
    gptps_toml_free(e->toml);
    gptps_free(e->config_path);

    gptps_free(e->hidx);
    gptps_free(e->workers);
    gptps_free(e->worker_args);
    gptps_free(e->owned_tids);
    /* bounded: every item is back in the pool by now (item_free) */
    while (e->cb_spare) { gptps_cb_thread *t = e->cb_spare; e->cb_spare = t->next; gptps_free(t); }
    gptps_free(e->result_arena);
    gptps_free(e->snap_arena);
    gptps_free(e->payload_arena);
    gptps_free(e->pool);
    if (e->pool_m) gptps_mutex_destroy(e->pool_m);
    {   size_t b;
        for (b = 0; b < GPTPS_CB_BUCKETS; ++b) {
            gptps_cb_thread *t = e->cb_threads[b];
            while (t) { gptps_cb_thread *n = t->next; gptps_free(t); t = n; }
        } }
    gptps_cond_destroy(e->cv_drain);
    gptps_cond_destroy(e->cv_work);
    gptps_cond_destroy(e->cv_disp);
    gptps_mutex_destroy(e->m);
    gptps_free(e);
    return GPTPS_OK;
}
