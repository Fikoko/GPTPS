/*
 * gptps.h - GPTPS: General Purpose Task Processing System
 *
 * An embeddable, in-process C99 task-processing library ("the SQLite of task
 * processors"): link libgptps, register a task once, submit work, and the
 * engine runs it under per-task resource budgets and a failure policy,
 * auto-tuned to the host hardware. Task logic is supplied directly (in-process
 * C function pointers) or by add-ons loaded via the stable host-table ABI.
 *
 * STATUS: implemented and tested (Linux + macOS full; Windows core). This public
 * header is the one can't-reverse decision; everything else hangs off it - grow
 * it by APPENDING only (see ABI DISCIPLINE below).
 *
 * ============================================================================
 * DATA FLOW (M1)
 * ============================================================================
 *
 *   host program                 engine (single-writer dispatcher)
 *   ------------                 ---------------------------------
 *   gptps_open(cfg) ───────────► [config model + auto-tune + HAL + pool]
 *   gptps_register_task() ─────► [task registry]
 *   gptps_submit(name,bytes) ──► [intake queue] ─┐
 *                                                │  dispatcher (owns ledger):
 *                                                ▼  admit iff declared cost
 *                                          [admission ledger]   fits live budget
 *                                                │              AND constraints
 *                                   fits ────────┼──── no ──► defer(retry_after)
 *                                                ▼
 *                                          [executor]
 *                                          ├─ IN-PROCESS  (fast, advisory)
 *                                          └─ OUT-OF-PROC (OS-capped, enforced)
 *                                                │
 *                                   run() ───────┤   watchdog: deadline → cancel
 *                                                ▼            flag (in-proc) /
 *                                          [failure engine]   hard-kill (oop)
 *                                          retries/backoff →
 *                                          requeue | drop | dead_letter
 *                                                │
 *                                   release budget; emit events
 *
 * ABI DISCIPLINE (read before touching any struct below)
 *   - All handles are OPAQUE; never expose struct layout to add-ons.
 *   - Every caller-extensible struct's FIRST field is `size_t struct_size`,
 *     set by the caller to sizeof(that struct). The core uses it to stay
 *     compatible across versions.
 *   - Structs may ONLY grow by APPENDING fields. Never reorder/remove/retype.
 *   - Bump GPTPS_ABI_VERSION_MINOR on additive change; MAJOR on incompatible.
 */
#ifndef GPTPS_H
#define GPTPS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- ABI version (semantic; loader refuses MAJOR mismatch) --------------- */
#define GPTPS_ABI_VERSION_MAJOR 1u
#define GPTPS_ABI_VERSION_MINOR 11u /* additive: result fields, argv/PROGRAM, constraints/observers, task priority, dead-letter drain, settings registry, settings change-watch, manual/single-threaded mode (gptps_step), allocator hook (gptps_set_allocator), generic task management (unregister/clone/enumerate/enable), generic global + per-task settings; v1.9: per-item constraint context (gptps_constraint_input: handle+payload), cancel-by-handle (gptps_cancel), bounded intake (max_intake_depth/E_FULL), submit_ex overrides, unregister constraint/observer, log sink, child_setup, EV_DROPPED; v1.10: generic named-resource budgets (define_resource/set_task_resource_cost/resource_usage); v1.11: long-running SERVICE tasks (gptps_task_def.flags + GPTPS_TASK_SERVICE): supervised restart-on-exit instances stopped cooperatively at cancel/unregister/shutdown */
#define GPTPS_ABI_MAGIC         0x47505450u /* "GPTP" */

/* --- release version (distinct from the ABI version above) ----------------
 * GPTPS_VERSION_* track the project RELEASE; GPTPS_ABI_VERSION_* track the binary
 * add-on contract (host-table + structs) and bump only when that changes. They
 * move INDEPENDENTLY. Versioning policy: until 1.0 the release version may break
 * compatibility between minors; from 1.0 the project follows semantic versioning
 * (a breaking change bumps MAJOR) and deprecated API is kept for one MAJOR with a
 * documented replacement. */
#define GPTPS_VERSION_MAJOR 0
#define GPTPS_VERSION_MINOR 2
#define GPTPS_VERSION_PATCH 0
#define GPTPS_VERSION_STRING "0.2.0"

/* --- export / visibility ------------------------------------------------- */
/* Default build is a STATIC library, so GPTPS_API is undecorated. Define
 * GPTPS_BUILD_DLL when building gptps as a DLL, or GPTPS_USE_DLL when linking
 * against one. Add-on .so/.dll always export their init symbol. */
#if defined(_WIN32)
#  if defined(GPTPS_BUILD_DLL)
#    define GPTPS_API __declspec(dllexport)
#  elif defined(GPTPS_USE_DLL)
#    define GPTPS_API __declspec(dllimport)
#  else
#    define GPTPS_API            /* static library: no decoration */
#  endif
#  define GPTPS_ADDON_EXPORT __declspec(dllexport)
#else
#  define GPTPS_API __attribute__((visibility("default")))
#  define GPTPS_ADDON_EXPORT __attribute__((visibility("default")))
#endif

/* --- opaque handles ------------------------------------------------------ */
typedef struct gptps      gptps;      /* engine instance */
typedef struct gptps_ctx  gptps_ctx;  /* per-run context, passed to a task */
typedef uint64_t          gptps_handle; /* identifies one submitted work item */

/* --- status: uniform return convention, no silent failures --------------- */
typedef enum {
    GPTPS_OK = 0,
    GPTPS_E_NOMEM,        /* allocation failed */
    GPTPS_E_INVAL,        /* bad argument / struct_size mismatch */
    GPTPS_E_NOTFOUND,     /* unknown task name */
    GPTPS_E_DUP,          /* task name already registered */
    GPTPS_E_BUDGET,       /* declared cost can never fit max budget (reject at submit) */
    GPTPS_E_FULL,         /* queue full */
    GPTPS_E_TIMEOUT,      /* task exceeded timeout_seconds */
    GPTPS_E_CANCELLED,    /* task observed cancellation */
    GPTPS_E_ABI,          /* add-on ABI magic/version/size mismatch */
    GPTPS_E_CONFIG,       /* config parse / validation error */
    GPTPS_E_IO,           /* transport / child-process I/O error */
    GPTPS_E_TASK,         /* task returned a non-OK application error */
    GPTPS_E_SHUTDOWN,     /* engine is shutting down */
    GPTPS_E_DENIED,       /* a constraint hook rejected admission */
    GPTPS_E_BUSY          /* task removal refused while work is queued / in-flight (REJECT_IF_BUSY, or DRAIN in MANUAL mode) */
} gptps_status;

GPTPS_API const char *gptps_strerror(gptps_status s);

/* Release version string (== GPTPS_VERSION_STRING the library was built from). */
GPTPS_API const char *gptps_version(void);

/* --- on-failure policy --------------------------------------------------- */
typedef enum {
    GPTPS_ON_FAILURE_DEAD_LETTER = 0, /* DEFAULT (safe): retained in the in-memory dead-letter list */
    GPTPS_ON_FAILURE_REQUEUE,         /* re-enqueue for another retry cycle; bodies MUST be idempotent */
    GPTPS_ON_FAILURE_DROP             /* discard after retries are exhausted */
} gptps_on_failure;

/* --- executor kind (modular seam; enforcement is an executor property) ---- */
typedef enum {
    GPTPS_EXEC_INPROC = 0, /* fast, advisory self-throttling; cooperative cancel only */
    GPTPS_EXEC_OOP,        /* forked child runs the in-process run fn; OS-capped, hard-kill */
    GPTPS_EXEC_PROGRAM     /* fork+exec an external program (argv); payload->stdin, stdout->result */
} gptps_exec_kind;

/* --- declared cost: a FIXED VECTOR so adding dimensions later is additive,
 *     not a behavioral ABI change. Only `mem_bytes` is enforced in M1. ----- */
typedef struct {
    size_t   struct_size;     /* = sizeof(gptps_cost) */
    uint64_t mem_bytes;       /* peak memory the task expects to use */
    uint32_t gpu_units;       /* abstract GPU units (0 = none); enforced via add-on */
    uint64_t est_duration_ms; /* expected wall-clock; scheduling hint */
} gptps_cost;

/* --- per-task failure policy (overridable in config with the same keys) --- */
typedef struct {
    size_t           struct_size;          /* = sizeof(gptps_failure_policy) */
    uint32_t         timeout_seconds;      /* 0 = no timeout */
    uint32_t         max_retries;
    uint32_t         retry_backoff_seconds;
    gptps_on_failure on_failure;
} gptps_failure_policy;

/* ============================================================================
 * TASK CONTEXT - the only surface a task body touches at run time.
 * The cancel flag is NEVER a public field; it is read through the accessor so
 * its synchronization (C11 _Atomic / __atomic / HAL barrier) stays internal
 * and the header remains pure C99 / broadly FFI-able.
 * ==========================================================================*/
typedef enum { GPTPS_LOG_DEBUG, GPTPS_LOG_INFO, GPTPS_LOG_WARN, GPTPS_LOG_ERROR } gptps_log_level;

/* True once the watchdog has signalled the deadline. In-process tasks MUST
 * poll this in long loops; a task that never polls cannot be stopped in-proc
 * (route untrusted/unbounded work to a GPTPS_EXEC_OOP executor). */
GPTPS_API bool        gptps_is_cancelled(const gptps_ctx *ctx);
GPTPS_API uint64_t    gptps_deadline_ms(const gptps_ctx *ctx);  /* monotonic; 0 = none */
GPTPS_API uint64_t    gptps_now_ms(const gptps_ctx *ctx);       /* monotonic */
GPTPS_API const void *gptps_payload(const gptps_ctx *ctx, size_t *out_len); /* input bytes */
GPTPS_API void        gptps_log(gptps_ctx *ctx, gptps_log_level lvl, const char *msg);

/* Result delivery goes through the ctx so ownership is UNIFORM across the
 * in-process and out-of-process paths (the OOP path marshals bytes anyway).
 *   _set        : core copies your bytes (valid only during this call) and frees its copy.
 *   _set_nocopy : you transfer ownership; the core calls free_cb after delivery (zero-copy
 *                 escape hatch for large results). A NULL free_cb means the buffer is
 *                 BORROWED (caller/static-owned) and the core will NOT free it.
 *                 Exactly one of these per run. */
GPTPS_API gptps_status gptps_result_set(gptps_ctx *ctx, const void *bytes, size_t len);
GPTPS_API gptps_status gptps_result_set_nocopy(gptps_ctx *ctx, void *bytes, size_t len,
                                               void (*free_cb)(void *bytes));

/* ============================================================================
 * TASK DEFINITION (direct in-process registration).
 *   run():  do the work. Read input via gptps_payload(); deliver output via
 *           gptps_result_set[_nocopy](); poll gptps_is_cancelled(). Return
 *           GPTPS_OK on success or GPTPS_E_TASK (or a specific code) on failure.
 *   cost(): OPTIONAL. Fill *out from the payload for dynamic per-item cost.
 *           If NULL, `default_cost` is used (config may override).
 * ==========================================================================*/
typedef gptps_status (*gptps_run_fn)(gptps_ctx *ctx, void *user_data);
typedef gptps_status (*gptps_cost_fn)(const void *payload, size_t len,
                                      gptps_cost *out, void *user_data);

/* --- task flags (gptps_task_def.flags; OR together) ----------------------- */
/* SERVICE: a long-running, supervised instance rather than a one-shot task. Its
 * run() is expected to loop until told to stop (poll gptps_is_cancelled()); when
 * it returns for any reason OTHER than a stop request it is automatically
 * RESTARTED after retry_backoff_seconds (crash-restart supervision). The engine
 * normalizes the failure policy for you (on_failure = REQUEUE, max_retries = 0,
 * no timeout). Start an instance with gptps_submit (start several for a pool); the
 * returned handle stays valid across restarts, so gptps_cancel(handle) stops that
 * one instance for good. gptps_unregister_task stops every instance of the type
 * (a DRAIN is auto-upgraded to CANCEL, since a service never drains on its own),
 * and gptps_shutdown stops them all. A service must poll gptps_is_cancelled() to
 * be stoppable - the same cooperative contract as any in-process cancel.
 * v1 restrictions (rejected at registration with GPTPS_E_INVAL): INPROC executor
 * only, THREADED mode only (a service's infinite loop cannot be run to completion
 * by the MANUAL gptps_step pump), and no timeout_seconds. */
#define GPTPS_TASK_SERVICE 0x1u

typedef struct {
    size_t                struct_size;   /* = sizeof(gptps_task_def) */
    const char           *name;          /* unique task-type id (borrowed) */
    gptps_run_fn          run;           /* required, EXCEPT for GPTPS_EXEC_PROGRAM */
    gptps_cost_fn         cost;          /* optional (NULL => use default_cost) */
    void                 *user_data;     /* passed back to run/cost */
    gptps_exec_kind       exec;          /* INPROC | OOP | PROGRAM */
    gptps_cost            default_cost;
    gptps_failure_policy  default_policy;
    /* GPTPS_EXEC_PROGRAM only: NULL-terminated argv. argv[0] is the program; a
     * bare name (e.g. "wasmtime") is resolved via PATH, an absolute/relative path
     * is used as-is. The engine feeds the payload on the program's stdin and reads
     * its result from stdout; exit code 0 => success, non-zero => GPTPS_E_TASK.
     * Ignored for INPROC/OOP. Copied by the engine at registration. */
    const char *const    *argv;
    /* GPTPS_EXEC_OOP / GPTPS_EXEC_PROGRAM only (v1.9): optional hook run IN THE
     * CHILD after fork - before exec (PROGRAM) or before the run fn (OOP). Harden
     * the child here: chdir, setenv, setrlimit, drop privileges, install seccomp,
     * close inherited fds. MUST be async-signal-safe (it runs between fork and
     * exec). Receives `user_data`. Ignored for INPROC and on Windows (no fork). */
    void (*child_setup)(void *user_data);
    /* v1.11: OR of GPTPS_TASK_* flags (0 = a normal one-shot task). GPTPS_TASK_SERVICE
     * marks a supervised long-running instance (see the flag's doc above). Appended
     * field: read only when struct_size covers it, so pre-v1.11 callers stay valid.
     * Deliberately uint64_t (not uint32_t): the struct is 8-byte aligned (its cost
     * fields hold uint64_t), so an 8-byte field cannot fall inside a pre-v1.11
     * struct's <8-byte trailing padding - it always grows sizeof, which keeps the
     * struct_size feature-detection unambiguous even on 32-bit ABIs (ARM32/AAPCS,
     * MIPS32) where a uint32_t here would have hidden in that padding. */
    uint64_t flags;
} gptps_task_def;

/* ============================================================================
 * ALLOCATOR (optional) - redirect ALL core allocation process-wide.
 * Install ONCE before the first gptps_open and do not change it while engines
 * exist (the override is configuration, not runtime state). Pass NULL to reset
 * to the C library. All three function pointers are required. Lets a host with a
 * static pool / no libc heap (embedded, bare-metal) own GPTPS's memory. Covers
 * the portable core; the HAL manages its own memory (replace it for exotic RAM).
 * ==========================================================================*/
typedef struct {
    size_t struct_size;                              /* = sizeof(gptps_allocator) */
    void *(*malloc_fn)(size_t size, void *user_data);
    void *(*realloc_fn)(void *ptr, size_t size, void *user_data);
    void  (*free_fn)(void *ptr, void *user_data);
    void  *user_data;                                /* opaque, passed to each call */
} gptps_allocator;

GPTPS_API gptps_status gptps_set_allocator(const gptps_allocator *a); /* NULL => reset to libc */

/* ============================================================================
 * DIAGNOSTIC SINK (optional) - redirect the core's WARN/ERROR diagnostics.
 * By default the core writes them to stderr. A host with no stdio (embedded /
 * bare-metal / freestanding) can install a sink to redirect or silence them
 * (pass a no-op callback to silence; pass NULL to reset to the stderr default).
 * Process-wide, like gptps_set_allocator; set it once before gptps_open.
 * ==========================================================================*/
typedef void (*gptps_log_sink_fn)(gptps_log_level lvl, const char *msg, void *user_data);
GPTPS_API void gptps_set_log_sink(gptps_log_sink_fn fn, void *user_data); /* NULL => stderr default */

/* ============================================================================
 * THREADING & REENTRANCY CONTRACT (read before writing callbacks or bindings)
 *
 * The engine is INTERNALLY SYNCHRONIZED. THREADED mode (default) runs a single
 * dispatcher + a worker pool; MANUAL mode runs entirely on the thread that calls
 * gptps_step().
 *
 *  - Concurrent calls: gptps_submit / gptps_submit_ex / gptps_cancel /
 *    gptps_settings_get / gptps_settings_set / gptps_task_* may be called from
 *    MANY threads at once on the same engine. Registration-style setup
 *    (gptps_register_task, gptps_register/unregister_constraint/observer,
 *    gptps_define_*) is SETUP-time: do it before submitting work or while quiescent.
 *  - Which thread a callback fires on (THREADED mode):
 *      QUEUED                  -> the thread that called gptps_submit;
 *      STARTED/FINISHED/FAILED -> a worker thread;
 *      RETRIED/DEAD_LETTERED   -> the dispatcher thread.
 *    In MANUAL mode every callback fires on the thread that called gptps_step().
 *  - Event callbacks (gptps_event_cb, observers) and the dead-letter drain
 *    callback ALWAYS run with the engine lock RELEASED, so they MAY call back
 *    into the engine (e.g. gptps_submit to retry) without deadlock. Keep them
 *    quick - a slow drain callback holds up the drain.
 *  - Constraint hooks (gptps_constraint_fn) are the EXCEPTION: they run on the
 *    dispatcher thread UNDER the engine lock, so they MUST be fast / non-blocking
 *    and MUST NOT call back into this engine (that would re-enter the lock).
 *  - In-process task bodies run with the lock released and must poll
 *    gptps_is_cancelled() to be stoppable (timeouts and gptps_cancel are
 *    cooperative for INPROC; OOP/PROGRAM are hard-killed at their deadline).
 *  - Settings write callbacks take their own lock; the fixed lock order is
 *    settings-lock -> engine-lock -> add-on lock.
 * ==========================================================================*/

/* ============================================================================
 * ENGINE LIFECYCLE
 * ==========================================================================*/
typedef struct {
    size_t   struct_size;          /* = sizeof(gptps_limits) */
    uint32_t max_concurrent_tasks; /* 0 => auto (detected cores); 1 => strictly sequential */
    uint64_t max_memory_bytes;     /* 0 => auto (fraction of detected RAM) */
    /* v1.9: backpressure. 0 => unbounded intake (default). Otherwise gptps_submit
     * returns GPTPS_E_FULL once this many items are queued (not yet admitted),
     * bounding memory an overproducing client can pin (max_memory_bytes caps only
     * the RUNNING set). Also tunable live via the "limits.max_intake_depth" setting. */
    uint32_t max_intake_depth;
} gptps_limits;

/* Execution model. THREADED (default, 0) spawns a dispatcher + worker pool and
 * runs itself - needs HAL threads/condvars. MANUAL spawns NO threads; the caller
 * drives the engine cooperatively via gptps_step() on its own thread. MANUAL is
 * the portable/embeddable path for single-threaded hosts and bare-metal: it needs
 * only the HAL mutex/clock/flag primitives, never gptps_thread_start/cond_wait. */
typedef enum {
    GPTPS_RUN_THREADED = 0,        /* default: dispatcher + worker pool */
    GPTPS_RUN_MANUAL   = 1         /* no threads; pump via gptps_step() */
} gptps_run_mode;

typedef struct {
    size_t        struct_size;     /* = sizeof(gptps_config) */
    const char   *config_path;     /* optional TOML path; NULL => limits below + defaults */
    gptps_limits  limits;          /* explicit values win over auto-tune & file */
    gptps_run_mode mode;           /* v1.6: THREADED (default) or MANUAL (gptps_step) */
} gptps_config;

GPTPS_API gptps_status gptps_open(const char *config_path, gptps **out_engine);
GPTPS_API gptps_status gptps_open_ex(const gptps_config *cfg, gptps **out_engine);

/* Registration paths (BOTH covered by the ABI regression test):
 *   - direct  : gptps_register_task() with in-process function pointers (the
 *               `cc gptps.c yourapp.c` headline path; does not cross the ABI).
 *   - dlopen  : add-ons attach via the host-table ABI (see below). */
GPTPS_API gptps_status gptps_register_task(gptps *e, const gptps_task_def *def);

/* Set a task type's scheduling priority (higher runs first; default 0, may be
 * negative). The dispatcher admits the highest-priority pending task that fits
 * the live budget, skipping a too-large task to backfill smaller work behind it
 * (no head-of-line blocking) while a bounded reservation keeps the skipped task
 * from starving. Applies to tasks submitted AFTER this call; also settable per
 * task in the config file ([task_defaults] / [tasks.<name>] `priority`). */
GPTPS_API gptps_status gptps_set_task_priority(gptps *e, const char *task_name, int priority);

/* ============================================================================
 * NAMED RESOURCE BUDGETS (generic admission limits)
 *
 * Declare arbitrary named resources with a total budget (GPUs, I/O bandwidth,
 * license seats, a per-tenant quota - anything). A task type declares how much of
 * each resource one of its items costs; the dispatcher then admits an item only
 * when EVERY resource it costs still fits its budget, reserving on admit and
 * releasing when the item reaches a terminal state. This generalizes admission
 * beyond the dedicated memory budget (limits.max_memory_bytes), which stays its
 * own dimension (it is wired to OS enforcement + auto-tune). Costs are per task
 * TYPE; an item that costs more of a resource than its whole budget is rejected
 * at submit with GPTPS_E_BUDGET. Setup-time calls (define before submitting work).
 * ==========================================================================*/

/* Declare (or, if it already exists, re-budget) a named resource. */
GPTPS_API gptps_status gptps_define_resource(gptps *e, const char *name, uint64_t budget);

/* Set a task type's per-item cost against a named resource (0 = no cost). The
 * resource must already be defined; GPTPS_E_NOTFOUND for an unknown task/resource. */
GPTPS_API gptps_status gptps_set_task_resource_cost(gptps *e, const char *task_name,
                                                    const char *resource, uint64_t amount);

/* Introspect a resource: *out_reserved (currently in flight) and *out_budget
 * (either may be NULL). GPTPS_E_NOTFOUND if the name is not defined. */
GPTPS_API gptps_status gptps_resource_usage(gptps *e, const char *name,
                                            uint64_t *out_reserved, uint64_t *out_budget);

/* ============================================================================
 * TASK MANAGEMENT (enumerate / enable / clone / unregister)
 *
 * The registry is itself introspectable and mutable at runtime, so an operator
 * (or a control-plane / TUI) can manage the whole task lifecycle live - not just
 * submit against compiled-in types. Behavior (the run fn / executor) still arrives
 * via code or an add-on; these calls own the *configuration and lifecycle* of the
 * types that exist.
 * ==========================================================================*/

/* A task type's removal policy (passed as the `flags` argument; the low bits are
 * the mode). The conservative default (0) never drops or blocks on work. */
#define GPTPS_REMOVE_MODE_MASK     0x3u
#define GPTPS_REMOVE_REJECT_IF_BUSY 0x0u /* DEFAULT: fail GPTPS_E_BUSY if any work is queued/in-flight */
#define GPTPS_REMOVE_DRAIN          0x1u /* tombstone (reject new submits), let queued+in-flight finish (no retries), then free */
#define GPTPS_REMOVE_CANCEL         0x2u /* drop queued work, cooperatively cancel in-flight, then free */

/* Snapshot of one registered task type (introspection; struct_size first). */
typedef struct {
    size_t               struct_size;   /* = sizeof(gptps_task_info) */
    const char          *name;          /* borrowed; stable until the registry is next mutated */
    gptps_exec_kind      exec;
    int32_t              priority;
    gptps_cost           default_cost;
    gptps_failure_policy default_policy;
    int                  enabled;        /* 0 => submits rejected (paused); 1 => accepting work */
    int                  removed;        /* 1 => tombstoned and draining toward removal */
    uint32_t             queued;         /* items waiting (intake + backoff) for this type */
    uint32_t             running;        /* items admitted / in-flight for this type */
    uint32_t             dead;           /* dead-lettered items retained for this type */
} gptps_task_info;

/* Number of registered task types (includes types that are draining toward removal). */
GPTPS_API size_t       gptps_task_count(gptps *e);
/* Fill *out for the task at `index` (0-based; order stable until the registry is
 * mutated). GPTPS_E_NOTFOUND past the end. */
GPTPS_API gptps_status gptps_task_get_info(gptps *e, size_t index, gptps_task_info *out);
/* 1 if a task of this name is registered AND accepting submits (enabled, not draining). */
GPTPS_API int          gptps_task_exists(gptps *e, const char *task_name);

/* Pause / resume a task type without removing it: a disabled type keeps its config
 * and stats but rejects new gptps_submit (GPTPS_E_NOTFOUND), reversibly. */
GPTPS_API gptps_status gptps_set_task_enabled(gptps *e, const char *task_name, int enabled);

/* Duplicate an existing task type under a new name (same run/exec/argv/user_data).
 * Cost + failure policy are copied from `src` and then re-layered from any open
 * config file's [task_defaults]/[tasks.<dst>]; the scheduling priority is carried
 * over from `src` as-is. The most common "tweak a copy" operation - e.g. clone
 * "resize" to "resize_hi" then raise its quality. GPTPS_E_NOTFOUND if `src` is
 * unknown, GPTPS_E_DUP if `dst` already exists. */
GPTPS_API gptps_status gptps_clone_task(gptps *e, const char *src_name, const char *dst_name);

/* Remove a task type. `flags` selects the policy (GPTPS_REMOVE_* above). On a
 * successful return the type is gone: its name is free to re-register and its
 * tasks.<name>.* settings are torn down. Dead-lettered items for the type are
 * retained (they carry the name as data) and remain drainable. GPTPS_E_NOTFOUND if
 * unknown, GPTPS_E_BUSY (REJECT_IF_BUSY only) if work is outstanding.
 *
 * THREADED mode blocks until the drain/cancel completes. DRAIN waits for queued +
 * in-flight work to finish, so it can block indefinitely if that work cannot make
 * progress (e.g. the budget stays saturated or a constraint keeps deferring it) -
 * use CANCEL to force removal in that case. In MANUAL mode there is no in-flight
 * work between gptps_step calls, so DRAIN/REJECT_IF_BUSY refuse with GPTPS_E_BUSY
 * when work is still queued (drain it by stepping first); CANCEL drops the queued
 * backlog and removes immediately. NOTE: a CANCEL/DRAIN of an in-flight in-process
 * task that never polls gptps_is_cancelled() blocks until it returns (same
 * cooperative limit as timeouts). Do not register/re-register the same name
 * concurrently with its removal (registration is a setup-time operation). */
GPTPS_API gptps_status gptps_unregister_task(gptps *e, const char *task_name, unsigned flags);

/* Load a dynamic add-on (shared library) via the host-table ABI below. The
 * add-on must export gptps_addon_init; the core validates magic/version/size
 * before use and tears the add-on down at gptps_shutdown. */
GPTPS_API gptps_status gptps_load_addon(gptps *e, const char *path);

/* Enqueue work. Rejects with GPTPS_E_BUDGET at submit time if the task's
 * declared cost can NEVER fit max_memory_bytes (it would otherwise starve). */
GPTPS_API gptps_status gptps_submit(gptps *e, const char *task_name,
                                    const void *payload, size_t len,
                                    gptps_handle *out_handle);

/* Per-submit overrides for gptps_submit_ex. struct_size first (additive). Only
 * the fields whose GPTPS_SUBMIT_* bit is set in `flags` are applied; the others
 * fall back to the task type's registered defaults. Lets a one-off submit carry
 * its own scheduling priority, failure policy, or sub-second deadline without
 * cloning the task type. */
#define GPTPS_SUBMIT_PRIORITY    0x1u  /* apply `priority`   */
#define GPTPS_SUBMIT_POLICY      0x2u  /* apply `policy`     (timeout/retries/backoff/on_failure) */
#define GPTPS_SUBMIT_TIMEOUT_MS  0x4u  /* apply `timeout_ms` (sub-second deadline; in-process cooperative path) */
typedef struct {
    size_t               struct_size;  /* = sizeof(gptps_submit_options) */
    unsigned             flags;        /* OR of GPTPS_SUBMIT_* selecting which overrides apply */
    int32_t              priority;     /* this item's scheduling priority (higher runs first) */
    gptps_failure_policy policy;       /* this item's failure policy */
    uint32_t             timeout_ms;   /* this item's deadline in ms (in-process cooperative path) */
} gptps_submit_options;

/* Like gptps_submit, but applies the per-item overrides in `opts` (NULL =>
 * identical to gptps_submit). GPTPS_E_INVAL if opts is non-NULL and undersized. */
GPTPS_API gptps_status gptps_submit_ex(gptps *e, const char *task_name,
                                       const void *payload, size_t len,
                                       const gptps_submit_options *opts,
                                       gptps_handle *out_handle);

/* Cancel one submitted work item by its handle. A still-queued item is removed
 * before it runs; an in-flight or admitted item gets the cooperative cancel flag
 * (in-process tasks MUST poll gptps_is_cancelled() to stop; OOP/PROGRAM children
 * stop at their deadline). The item ends terminal (a FAILED event, never a retry
 * or dead-letter). Returns GPTPS_OK if a matching item was found and cancelled,
 * GPTPS_E_NOTFOUND if the handle is unknown or already terminal (cancel-after-
 * completion is a harmless no-op), GPTPS_E_SHUTDOWN during teardown. Safe to call
 * from any thread, including from inside an event callback. */
GPTPS_API gptps_status gptps_cancel(gptps *e, gptps_handle h);

/* Single-threaded pump - MANUAL mode only (GPTPS_E_INVAL otherwise). Runs the
 * engine on the CALLING thread with no dispatcher/worker threads: one call
 * completes finished work, promotes backoff-ready retries, admits within budget,
 * and runs the admitted tasks to completion inline. *out_ran (may be NULL)
 * receives the number of task attempts executed this call - 0 when idle or only
 * waiting on a retry backoff. Drain with `while (gptps_step(e,&n)==GPTPS_OK && n);`,
 * or call periodically as your host's main loop ticks. Because a task runs to
 * completion on this thread, a wall-clock timeout cannot preempt it; cooperative
 * tasks should poll gptps_is_cancelled() / gptps_deadline_ms(). */
GPTPS_API gptps_status gptps_step(gptps *e, size_t *out_ran);

GPTPS_API gptps_status gptps_shutdown(gptps *e); /* drain in-flight, join, free */

/* ============================================================================
 * EVENTS (observer surface; the core never aggregates - that's an add-on)
 * ==========================================================================*/
typedef enum {
    GPTPS_EV_QUEUED, GPTPS_EV_STARTED, GPTPS_EV_FINISHED,
    GPTPS_EV_FAILED, GPTPS_EV_RETRIED, GPTPS_EV_DEAD_LETTERED,
    /* v1.9: terminal event after retries are exhausted under the DROP policy
     * (the item is discarded, not retained). Lets an observer reconcile every
     * submitted item - DROP previously emitted no terminal event. */
    GPTPS_EV_DROPPED
} gptps_event_kind;

typedef struct {
    size_t           struct_size;   /* = sizeof(gptps_event) */
    gptps_event_kind kind;
    gptps_handle     handle;
    const char      *task_name;
    uint64_t         ts_ms;         /* monotonic */
    gptps_status     status;        /* terminal status for FINISHED/FAILED */
    uint32_t         attempt;
    /* memory: the task's DECLARED cost (cost.mem_bytes), for both in-process and
     * OOP/PROGRAM tasks. RSS is not reported here: in-process it is not observable
     * in a shared address space, and although an OOP/PROGRAM task's cap is ENFORCED
     * (cgroup memory.max on Linux, else RLIMIT_AS), its measured peak RSS is not
     * plumbed back into this field. */
    uint64_t         mem_bytes;
    /* task result bytes, present on GPTPS_EV_FINISHED (NULL/0 otherwise).
     * Valid only for the duration of the callback - copy it if you need it. */
    const void      *result;
    size_t           result_len;
} gptps_event;

typedef void (*gptps_event_cb)(const gptps_event *ev, void *user_data);
GPTPS_API gptps_status gptps_set_event_cb(gptps *e, gptps_event_cb cb, void *user_data);

/* ============================================================================
 * DEAD LETTER (retained terminal failures; inspect / reprocess)
 *
 * Tasks that exhaust retries under the dead_letter policy, and tasks a
 * constraint DENYs, are retained in-memory. Pull them back out to log, audit,
 * or re-submit them. gptps_shutdown() frees any that were never drained.
 * ==========================================================================*/
typedef struct {
    size_t       struct_size;   /* = sizeof(gptps_dead_letter) */
    gptps_handle handle;        /* the original submit handle */
    const char  *task_name;
    gptps_status status;        /* the terminal failure status */
    uint32_t     attempts;      /* attempts made before giving up */
    const void  *payload;       /* original payload (valid only for the callback) */
    size_t       payload_len;
} gptps_dead_letter;

/* Current number of retained dead-lettered tasks (does not drain). */
GPTPS_API size_t gptps_dead_letter_count(gptps *e);

typedef void (*gptps_dead_letter_cb)(const gptps_dead_letter *dl, void *user_data);

/* Drain all retained dead-lettered tasks. For each, `cb` is invoked (the payload
 * is valid only for that call) and the item is then freed. The callback runs
 * with the engine lock RELEASED, so it MAY re-submit (e.g. gptps_submit) to retry
 * the work. Pass cb == NULL to simply discard them. Returns the number drained. */
GPTPS_API size_t gptps_dead_letter_drain(gptps *e, gptps_dead_letter_cb cb, void *user_data);

/* ============================================================================
 * SETTINGS - a unified, typed, introspectable + validated configuration registry.
 *
 * Every knob (core limits/scheduler, per-task policy, and add-on settings) is a
 * registered entry with a dotted key (e.g. "scheduler.reserve_after_skips",
 * "tasks.resize.timeout_seconds", "tui.refresh_ms"). The registry is SCHEMA +
 * ACCESSOR BINDING: the live engine/add-on state stays the source of truth, so
 * reads never go stale. set() parses + validates (range / enum / type) before
 * applying, and applies LIVE when the setting is `hot`.
 * ==========================================================================*/
typedef enum {
    GPTPS_SETTING_INT = 0,   /* signed integer        */
    GPTPS_SETTING_UINT,      /* unsigned integer      */
    GPTPS_SETTING_DOUBLE,
    GPTPS_SETTING_BOOL,      /* "true" / "false"      */
    GPTPS_SETTING_ENUM,      /* one of a fixed choice set */
    GPTPS_SETTING_STRING
} gptps_setting_type;

#define GPTPS_SETTINGS_VALUE_MAX 256   /* cap for any rendered/parsed value string */

/* Flags for the convenience registration helpers below (gptps_define_global /
 * gptps_define_task_setting). Default (0) is a `hot` setting that applies live. */
#define GPTPS_SETTING_HOT      0x0u /* applies live (default) */
#define GPTPS_SETTING_RESTART  0x1u /* effective only after restart (sets info.hot = 0) */

/* Schema + accessor binding for ONE setting, supplied at registration. `key`,
 * `desc` are copied; `choices` (enum only) is BORROWED and must outlive the engine
 * (use a static array). `target` is opaque, passed back to read/write. */
typedef struct {
    size_t              struct_size;   /* = sizeof(gptps_setting_def) */
    const char         *key;
    gptps_setting_type  type;
    const char         *desc;
    int                 hot;           /* 1 = applies live; 0 = effective on restart */
    int                 has_range;     /* 1 => min/max apply (numeric types) */
    double              min, max;
    const char *const  *choices;       /* NULL-terminated; NULL unless ENUM */
    void               *target;
    size_t            (*read)(void *target, char *buf, size_t cap);  /* render current value */
    gptps_status      (*write)(void *target, const char *value);     /* parse + apply (takes its own lock) */
} gptps_setting_def;

/* Introspection record (struct_size first; value/defval rendered inline). */
typedef struct {
    size_t              struct_size;   /* = sizeof(gptps_setting_info) */
    const char         *key;           /* borrowed; stable for engine lifetime */
    gptps_setting_type  type;
    const char         *desc;          /* borrowed */
    int                 hot;
    int                 has_range;
    double              min, max;
    const char *const  *choices;       /* borrowed; NULL unless ENUM */
    char                value[GPTPS_SETTINGS_VALUE_MAX];   /* current, rendered */
    char                defval[GPTPS_SETTINGS_VALUE_MAX];  /* default at registration */
} gptps_setting_info;

/* Register a setting into the engine's registry (GPTPS_E_DUP on a duplicate key). */
GPTPS_API gptps_status gptps_register_setting(gptps *e, const gptps_setting_def *def);

/* ---- generic settings without write-your-own get/set glue --------------------
 * The primitive above binds a setting to YOUR live state via read/write callbacks.
 * The two helpers below let you declare arbitrary typed knobs that the engine
 * stores and validates for you - so a host (or a control plane) can grow the
 * configuration surface at runtime without per-key code.
 *
 *   `constraint` shapes validation by type:
 *     numeric  -> "min..max"  (e.g. "0..4096"); NULL for unbounded
 *     enum     -> "a|b|c"     (the allowed choices; required for GPTPS_SETTING_ENUM)
 *     other    -> ignored (pass NULL)
 *   `default_val` is the initial rendered value (validated; NULL => type's zero).
 *   `flags` is GPTPS_SETTING_HOT (default) or GPTPS_SETTING_RESTART.
 */

/* A GLOBAL knob, engine-stored. Appears under its full dotted `key` (no tasks.
 * prefix), round-trips through TOML, and shows up in the settings editor like any
 * core setting. Read/write it with gptps_settings_get/set. GPTPS_E_DUP on a
 * duplicate key, GPTPS_E_CONFIG if default_val/constraint is invalid. */
GPTPS_API gptps_status gptps_define_global(gptps *e, const char *key, gptps_setting_type type,
                                           const char *default_val, const char *constraint,
                                           unsigned flags);

/* A PER-TASK knob schema. The engine materializes `tasks.<name>.<leaf>` for every
 * registered task (existing and future), each task instance carrying its own value
 * (defaulted from `default_val`, overridable via TOML or the settings API). `leaf`
 * must be a bare key with no dots. GPTPS_E_DUP if the leaf is already defined.
 * Like task registration, this is a SETUP-time call: it walks the registry to
 * apply the schema to existing tasks, so do not run it concurrently with
 * gptps_unregister_task / gptps_clone_task on this engine. */
GPTPS_API gptps_status gptps_define_task_setting(gptps *e, const char *leaf, gptps_setting_type type,
                                                 const char *default_val, const char *constraint,
                                                 unsigned flags);

/* Read THIS task instance's resolved value for a generic per-task setting, from
 * inside its run() (closes the loop opened by gptps_define_task_setting). `key` is
 * the bare leaf. GPTPS_OK on success; GPTPS_E_NOTFOUND if no such per-task setting;
 * GPTPS_E_INVAL for a bad arg or when the value does not parse as the requested
 * type. Available only to in-process (INPROC) tasks - an OOP/PROGRAM body runs in a
 * separate process with no live engine handle and gets GPTPS_E_INVAL. */
GPTPS_API gptps_status gptps_task_setting_int(gptps_ctx *ctx, const char *key, long *out);
GPTPS_API gptps_status gptps_task_setting_str(gptps_ctx *ctx, const char *key, char *buf, size_t cap);

/* Introspection (index-based; indices are stable until a new task/add-on registers). */
GPTPS_API size_t       gptps_settings_count(gptps *e);
GPTPS_API gptps_status gptps_settings_get_info(gptps *e, size_t index, gptps_setting_info *out);

/* String get/set by key. set() validates then applies; GPTPS_E_NOTFOUND for an
 * unknown key, GPTPS_E_CONFIG for an invalid value. */
GPTPS_API gptps_status gptps_settings_get(gptps *e, const char *key, char *buf, size_t cap);
GPTPS_API gptps_status gptps_settings_set(gptps *e, const char *key, const char *value);

/* Persistence. save() regenerates a grouped TOML file atomically (comments are
 * NOT preserved). reload() re-parses and re-applies known keys via set()+validation
 * (best-effort: returns the first error; a parse failure applies nothing). For both,
 * path==NULL uses the path the engine was opened with (GPTPS_E_INVAL if none). */
GPTPS_API gptps_status gptps_settings_save(gptps *e, const char *path);
GPTPS_API gptps_status gptps_settings_reload(gptps *e, const char *path);

/* Watch for changes: `cb` fires (with the settings lock RELEASED) after each
 * successful gptps_settings_set, with the key and its newly-applied value - for
 * audit logs, auto-save, re-rendering, etc. Register watchers before concurrent
 * settings activity. (reload re-applies known keys without firing watchers.) */
typedef void (*gptps_settings_cb)(const char *key, const char *value, void *user_data);
GPTPS_API gptps_status gptps_settings_watch(gptps *e, gptps_settings_cb cb, void *user_data);

/* ============================================================================
 * HOST-TABLE ABI (for dlopen'd add-ons)
 *
 * The add-on calls the core ONLY through a passed, version-stamped table of
 * function pointers and links NO core symbols. This is the permanent contract;
 * grow `gptps_api_routines` by APPENDING only.
 *
 * Add-on .so exports exactly one symbol:
 *     GPTPS_ADDON_EXPORT const gptps_addon *gptps_addon_init(const gptps_api_routines *api);
 * The core checks magic / abi_version_major / struct_size BEFORE using it.
 *
 * NOTE: in the amalgamated `cc gptps.c yourapp.c` build, core symbols live in
 * the host executable; -fvisibility=hidden/RTLD_LOCAL do NOT protect them, so
 * core symbols are namespaced (gptps_/gptps__) to avoid add-on capture.
 * ==========================================================================*/
typedef enum {
    GPTPS_SEAM_TASK = 0,   /* frozen v1.0 */
    GPTPS_SEAM_TRANSPORT,  /* EXPERIMENTAL until M2 (exec-bridge); frozen with first consumer */
    GPTPS_SEAM_CONSTRAINT, /* frozen v1.9 (gptps_constraint_input): admit/deny/defer hook; MUST be non-blocking */
    GPTPS_SEAM_OBSERVER    /* frozen v1.9: analytics / dead-letter sink */
} gptps_seam_kind;

/* Constraint hook result (constraint seam, frozen v1.9). retry_after_ms is
 * honored on DEFER so the dispatcher schedules a wake at T (no busy-spin). */
typedef enum { GPTPS_ADMIT = 0, GPTPS_DENY, GPTPS_DEFER } gptps_admit_decision;

/* Per-item admission context handed to a constraint hook. `struct_size` is
 * first so future per-item fields are additive - the hook signature never has
 * to change again. All pointers are BORROWED and valid only for the duration of
 * the call. The `handle` gives the hook ITEM IDENTITY (not just the task type),
 * which is what makes per-item constraints - dependencies, dedup / idempotency,
 * per-tenant admission - buildable as pure add-ons. */
typedef struct {
    size_t            struct_size;   /* = sizeof(gptps_constraint_input) */
    const char       *task_name;     /* task type id */
    const gptps_cost *cost;          /* this item's declared cost */
    gptps_handle      handle;        /* the submitted work item being admitted */
    const void       *payload;       /* item payload bytes (NULL if none) */
    size_t            payload_len;
} gptps_constraint_input;

/* A constraint hook is consulted at admission time (in the dispatcher, so it
 * MUST be fast / non-blocking). Return GPTPS_ADMIT to allow, GPTPS_DENY to
 * reject (task is dead-lettered with GPTPS_E_DENIED), or GPTPS_DEFER and set
 * *retry_after_ms to re-check later. Inspect the item via `in` (check
 * in->struct_size before reading fields appended in a later minor). */
typedef gptps_admit_decision (*gptps_constraint_fn)(const gptps_constraint_input *in,
                                                    uint32_t *retry_after_ms,
                                                    void *user_data);

typedef struct {
    size_t   struct_size;          /* = sizeof(gptps_api_routines) */
    uint32_t abi_version_major;    /* == GPTPS_ABI_VERSION_MAJOR */
    uint32_t abi_version_minor;
    /* --- v1.0 routines (append below this line in later minors only) --- */
    gptps_status (*register_task)(gptps *e, const gptps_task_def *def);
    gptps_status (*emit_event)(gptps *e, const gptps_event *ev);
    void         (*log)(gptps_ctx *ctx, gptps_log_level lvl, const char *msg);
    /* result/result_nocopy/payload mirror the ctx accessors for OOP shims */
    gptps_status (*result_set)(gptps_ctx *ctx, const void *bytes, size_t len);
    const void  *(*payload)(const gptps_ctx *ctx, size_t *out_len);
    /* --- v1.1 routines (append-only) --- */
    gptps_status (*register_constraint)(gptps *e, gptps_constraint_fn fn, void *user_data);
    gptps_status (*register_observer)(gptps *e, gptps_event_cb fn, void *user_data);
    /* --- v1.4 routines (append-only); guard with `struct_size` before calling --- */
    gptps_status (*register_setting)(gptps *e, const gptps_setting_def *def);
    /* --- v1.8 routines (append-only); guard with `struct_size` before calling --- */
    gptps_status (*unregister_task)(gptps *e, const char *task_name, unsigned flags);
    int          (*task_exists)(gptps *e, const char *task_name);
    gptps_status (*define_global)(gptps *e, const char *key, gptps_setting_type type,
                                  const char *default_val, const char *constraint, unsigned flags);
    gptps_status (*define_task_setting)(gptps *e, const char *leaf, gptps_setting_type type,
                                        const char *default_val, const char *constraint, unsigned flags);
    /* --- v1.9 routines (append-only); guard with `struct_size` before calling --- */
    gptps_status (*cancel)(gptps *e, gptps_handle h);
    gptps_status (*unregister_constraint)(gptps *e, gptps_constraint_fn fn, void *user_data);
    gptps_status (*unregister_observer)(gptps *e, gptps_event_cb fn, void *user_data);
    /* --- v1.10 routines (append-only); guard with `struct_size` before calling --- */
    gptps_status (*define_resource)(gptps *e, const char *name, uint64_t budget);
    gptps_status (*set_task_resource_cost)(gptps *e, const char *task_name, const char *resource, uint64_t amount);
    gptps_status (*resource_usage)(gptps *e, const char *name, uint64_t *out_reserved, uint64_t *out_budget);
} gptps_api_routines;

typedef struct {
    size_t          struct_size;     /* = sizeof(gptps_addon) */
    uint32_t        magic;           /* == GPTPS_ABI_MAGIC */
    uint32_t        abi_version_major;
    const char     *name;
    gptps_seam_kind seam;
    /* Called once after load; register tasks/constraints/etc. via `api`. */
    gptps_status  (*setup)(gptps *e, const gptps_api_routines *api, char **err_out);
    void          (*teardown)(gptps *e);
} gptps_addon;

typedef const gptps_addon *(*gptps_addon_init_fn)(const gptps_api_routines *api);

/* Ergonomic entry-point macro (EXTENSION_INIT-style): keeps the api table
 * stashed so call sites read normally. Use once per add-on translation unit. */
#define GPTPS_ADDON_INIT(name_str, seam_kind, setup_fn, teardown_fn)            \
    static const gptps_api_routines *gptps__api = 0;                            \
    GPTPS_ADDON_EXPORT const gptps_addon *                                      \
    gptps_addon_init(const gptps_api_routines *api) {                           \
        static const gptps_addon a = {                                          \
            sizeof(gptps_addon), GPTPS_ABI_MAGIC, GPTPS_ABI_VERSION_MAJOR,      \
            (name_str), (seam_kind), (setup_fn), (teardown_fn)                  \
        };                                                                      \
        gptps__api = api;                                                       \
        return &a;                                                              \
    }

/* ============================================================================
 * CONSTRAINTS & OBSERVERS (extension points; also reachable by add-ons through
 * the api-routines table above)
 * ==========================================================================*/

/* Consulted at admission for every task (in registration order). Keep it fast. */
GPTPS_API gptps_status gptps_register_constraint(gptps *e, gptps_constraint_fn fn, void *user_data);

/* Additional event sink beyond gptps_set_event_cb (many allowed). Register
 * before submitting work. */
GPTPS_API gptps_status gptps_register_observer(gptps *e, gptps_event_cb fn, void *user_data);

/* Remove a constraint / observer previously registered with the same (fn,
 * user_data). Like registration, this is a SETUP-time operation: do not call it
 * while the engine is actively emitting events or admitting work (sinks are
 * iterated lock-free on the hot path) - unregister when quiescent / before
 * submitting. Enables add-on hot-unload. GPTPS_E_NOTFOUND if no match. */
GPTPS_API gptps_status gptps_unregister_constraint(gptps *e, gptps_constraint_fn fn, void *user_data);
GPTPS_API gptps_status gptps_unregister_observer(gptps *e, gptps_event_cb fn, void *user_data);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GPTPS_H */
