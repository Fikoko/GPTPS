/*
 * gptps.h - GPTPS: General Purpose Task Processing System
 *
 * An embeddable, in-process C99 task-processing library ("the SQLite of task
 * processors"): link libgptps, register a task once, submit work, and the
 * engine runs it under per-task resource budgets and a failure policy,
 * auto-tuned to the host hardware. Task logic is supplied directly (in-process
 * C function pointers) or by add-ons loaded via the stable host-table ABI.
 *
 * STATUS: v1 frozen-on-paper public header (no implementation yet). This file
 * is the one can't-reverse decision; everything else hangs off it.
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
#define GPTPS_ABI_VERSION_MINOR 0u
#define GPTPS_ABI_MAGIC         0x47505450u /* "GPTP" */

/* --- export / visibility ------------------------------------------------- */
#if defined(_WIN32)
#  define GPTPS_API __declspec(dllimport)
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
    GPTPS_E_SHUTDOWN      /* engine is shutting down */
} gptps_status;

GPTPS_API const char *gptps_strerror(gptps_status s);

/* --- on-failure policy --------------------------------------------------- */
typedef enum {
    GPTPS_ON_FAILURE_REQUEUE = 0, /* at-least-once: task bodies MUST be idempotent */
    GPTPS_ON_FAILURE_DROP,
    GPTPS_ON_FAILURE_DEAD_LETTER  /* retained in the in-memory dead-letter list */
} gptps_on_failure;

/* --- executor kind (modular seam; enforcement is an executor property) ---- */
typedef enum {
    GPTPS_EXEC_INPROC = 0, /* fast, advisory self-throttling; cooperative cancel only */
    GPTPS_EXEC_OOP         /* child process, OS memory-capped; hard-kill enforced */
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
 *                 escape hatch for large results). Exactly one of these per run. */
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

typedef struct {
    size_t                struct_size;   /* = sizeof(gptps_task_def) */
    const char           *name;          /* unique task-type id (borrowed) */
    gptps_run_fn          run;           /* required */
    gptps_cost_fn         cost;          /* optional (NULL => use default_cost) */
    void                 *user_data;     /* passed back to run/cost */
    gptps_exec_kind       exec;          /* GPTPS_EXEC_INPROC or _OOP */
    gptps_cost            default_cost;
    gptps_failure_policy  default_policy;
} gptps_task_def;

/* ============================================================================
 * ENGINE LIFECYCLE
 * ==========================================================================*/
typedef struct {
    size_t   struct_size;          /* = sizeof(gptps_limits) */
    uint32_t max_concurrent_tasks; /* 0 => auto (detected cores); 1 => strictly sequential */
    uint64_t max_memory_bytes;     /* 0 => auto (fraction of detected RAM) */
} gptps_limits;

typedef struct {
    size_t        struct_size;     /* = sizeof(gptps_config) */
    const char   *config_path;     /* optional TOML path; NULL => limits below + defaults */
    gptps_limits  limits;          /* explicit values win over auto-tune & file */
} gptps_config;

GPTPS_API gptps_status gptps_open(const char *config_path, gptps **out_engine);
GPTPS_API gptps_status gptps_open_ex(const gptps_config *cfg, gptps **out_engine);

/* Registration paths (BOTH covered by the ABI regression test):
 *   - direct  : gptps_register_task() with in-process function pointers (the
 *               `cc gptps.c yourapp.c` headline path; does not cross the ABI).
 *   - dlopen  : add-ons attach via the host-table ABI (see below). */
GPTPS_API gptps_status gptps_register_task(gptps *e, const gptps_task_def *def);

/* Enqueue work. Rejects with GPTPS_E_BUDGET at submit time if the task's
 * declared cost can NEVER fit max_memory_bytes (it would otherwise starve). */
GPTPS_API gptps_status gptps_submit(gptps *e, const char *task_name,
                                    const void *payload, size_t len,
                                    gptps_handle *out_handle);

GPTPS_API gptps_status gptps_shutdown(gptps *e); /* drain in-flight, join, free */

/* ============================================================================
 * EVENTS (observer surface; the core never aggregates - that's an add-on)
 * ==========================================================================*/
typedef enum {
    GPTPS_EV_QUEUED, GPTPS_EV_STARTED, GPTPS_EV_FINISHED,
    GPTPS_EV_FAILED, GPTPS_EV_RETRIED, GPTPS_EV_DEAD_LETTERED
} gptps_event_kind;

typedef struct {
    size_t           struct_size;   /* = sizeof(gptps_event) */
    gptps_event_kind kind;
    gptps_handle     handle;
    const char      *task_name;
    uint64_t         ts_ms;         /* monotonic */
    gptps_status     status;        /* terminal status for FINISHED/FAILED */
    uint32_t         attempt;
    /* memory: DECLARED cost for in-process tasks (RSS is not measurable in a
     * shared address space); MEASURED RSS for out-of-process tasks. */
    uint64_t         mem_bytes;
} gptps_event;

typedef void (*gptps_event_cb)(const gptps_event *ev, void *user_data);
GPTPS_API gptps_status gptps_set_event_cb(gptps *e, gptps_event_cb cb, void *user_data);

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
    GPTPS_SEAM_CONSTRAINT, /* EXPERIMENTAL: admit/deny/defer hook; MUST be non-blocking */
    GPTPS_SEAM_OBSERVER    /* EXPERIMENTAL: analytics / dead-letter sink */
} gptps_seam_kind;

/* Constraint hook result (constraint seam, experimental). retry_after_ms is
 * honored on DEFER so the dispatcher schedules a wake at T (no busy-spin). */
typedef enum { GPTPS_ADMIT = 0, GPTPS_DENY, GPTPS_DEFER } gptps_admit_decision;

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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GPTPS_H */
