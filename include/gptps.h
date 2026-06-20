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
#define GPTPS_ABI_VERSION_MINOR 3u  /* additive: result fields, argv/PROGRAM, constraints/observers, task priority, dead-letter drain */
#define GPTPS_ABI_MAGIC         0x47505450u /* "GPTP" */

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
    GPTPS_E_DENIED        /* a constraint hook rejected admission */
} gptps_status;

GPTPS_API const char *gptps_strerror(gptps_status s);

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

/* Set a task type's scheduling priority (higher runs first; default 0, may be
 * negative). The dispatcher admits the highest-priority pending task that fits
 * the live budget, skipping a too-large task to backfill smaller work behind it
 * (no head-of-line blocking) while a bounded reservation keeps the skipped task
 * from starving. Applies to tasks submitted AFTER this call; also settable per
 * task in the config file ([task_defaults] / [tasks.<name>] `priority`). */
GPTPS_API gptps_status gptps_set_task_priority(gptps *e, const char *task_name, int priority);

/* Load a dynamic add-on (shared library) via the host-table ABI below. The
 * add-on must export gptps_addon_init; the core validates magic/version/size
 * before use and tears the add-on down at gptps_shutdown. */
GPTPS_API gptps_status gptps_load_addon(gptps *e, const char *path);

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

/* Introspection (index-based; indices are stable until a new task/add-on registers). */
GPTPS_API size_t       gptps_settings_count(gptps *e);
GPTPS_API gptps_status gptps_settings_get_info(gptps *e, size_t index, gptps_setting_info *out);

/* String get/set by key. set() validates then applies; GPTPS_E_NOTFOUND for an
 * unknown key, GPTPS_E_CONFIG for an invalid value. */
GPTPS_API gptps_status gptps_settings_get(gptps *e, const char *key, char *buf, size_t cap);
GPTPS_API gptps_status gptps_settings_set(gptps *e, const char *key, const char *value);

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

/* A constraint hook is consulted at admission time (in the dispatcher, so it
 * MUST be fast / non-blocking). Return GPTPS_ADMIT to allow, GPTPS_DENY to
 * reject (task is dead-lettered with GPTPS_E_DENIED), or GPTPS_DEFER and set
 * *retry_after_ms to re-check later. */
typedef gptps_admit_decision (*gptps_constraint_fn)(const char *task_name,
                                                    const gptps_cost *cost,
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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GPTPS_H */
