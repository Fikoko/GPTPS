/*
 * gptps_internal.h - internal prototypes shared across core translation units.
 * Not installed; not part of the public ABI.
 */
#ifndef GPTPS_INTERNAL_H
#define GPTPS_INTERNAL_H

#include "gptps.h"

#include <stddef.h>   /* offsetof */

/* --- append-safe ABI struct guards ---
 * A caller-extensible struct only ever GROWS by appending (see gptps.h "ABI
 * DISCIPLINE"), so validating an INPUT struct with `struct_size < sizeof(current)`
 * would reject a caller compiled against an OLDER header the moment the core adds a
 * field. Instead validate against a FROZEN minimum (the layout at the point the
 * field became required) and read any later-appended field only when struct_size
 * proves the caller actually has it.
 *
 *   GPTPS_STRUCT_HAS(type, ptr, field): true iff `ptr` is large enough to contain
 *   `field` (guard before reading an appended field).
 *   GPTPS_<STRUCT>_MIN_SIZE: the frozen minimum a caller must supply. Growing a
 *   struct must NOT move its MIN_SIZE (that would be an incompatible change). */
#define GPTPS_STRUCT_HAS(type, ptr, field) \
    ((ptr)->struct_size >= offsetof(type, field) + sizeof((ptr)->field))
/* gptps_task_def froze at the v1.11 prefix: everything through child_setup is
 * required; `flags` (v1.11) and anything appended later is optional. `flags` is a
 * uint64_t and the struct is 8-aligned, so offsetof(flags) == the pre-v1.11 padded
 * sizeof on every ABI (the field sits exactly at the old size boundary, never inside
 * old trailing padding) - so this equals the pre-v1.11 sizeof(gptps_task_def) and
 * every existing caller still validates, while GPTPS_STRUCT_HAS(flags) stays a true
 * distinguisher (a pre-v1.11 struct_size is strictly below its +8 threshold). */
#define GPTPS_TASK_DEF_MIN_SIZE (offsetof(gptps_task_def, flags))
/* Compile-time guard against the tail-padding hazard: `flags` must stay the struct's
 * widest (hence most-aligned) member, so it lands exactly at the pre-v1.11 padded
 * size boundary and never inside old trailing padding (which would let a pre-v1.11
 * struct_size be misread as "has flags"). Enforced portably by keeping it 8 bytes: a
 * uint64_t is the widest scalar in this struct, so on EVERY ABI - i386 (4-aligned
 * uint64) through ARM32/AAPCS (8-aligned) and s390x - it sits at offsetof == the
 * predecessor sizeof. A uint32_t would drop this to 4 and reintroduce the hazard.
 * (Checking `offsetof(flags) % 8` would be wrong: it false-fails on i386, where the
 * struct is only 4-aligned yet perfectly safe.) C99 negative-array-size assertion. */
typedef char gptps__task_def_flags_is_widest[(sizeof(((gptps_task_def *)0)->flags) == 8u) ? 1 : -1];

/* --- core allocator seam (alloc.c) ---
 * Every CORE allocation goes through these; they default to the C library and
 * are redirected process-wide by the public gptps_set_allocator(). gptps_free
 * tolerates NULL; gptps_calloc guards size overflow. (The HAL uses libc directly
 * - it is replaced wholesale on exotic targets.) */
void *gptps_malloc(size_t size);
void *gptps_calloc(size_t n, size_t size);
void *gptps_realloc(void *ptr, size_t size);
void  gptps_free(void *ptr);

/* config model + auto-tune (T6).
 * Resolves a caller's limits against detected hardware:
 *   - max_concurrent_tasks == 0  => detected CPU count
 *   - max_memory_bytes     == 0  => 0.75 * detected RAM (floor if RAM unknown)
 * Any explicit non-zero value is passed through unchanged (explicit wins).
 * `in` may be NULL (treated as "all auto"). */
gptps_status gptps_config_resolve(const gptps_limits *in, gptps_limits *out);

/* Build a ctx, run the task in THIS process, and return a malloc'd copy of its
 * result bytes (caller frees; NULL/0 if none). Used by the OOP child. */
gptps_status gptps_run_capture(const gptps_task_def *def, const void *payload, size_t plen,
                               void **out_result, size_t *out_len);

/* Out-of-process executor (POSIX): fork, apply an OS memory cap in the child,
 * run the task there, and stream the result back. The parent hard-kills the
 * child on timeout (returns GPTPS_E_TIMEOUT) - real enforcement the in-process
 * path cannot provide. The memory cap is accurate cgroup v2 (memory.max +
 * swap.max=0, exceeding it => GPTPS_E_NOMEM) when GPTPS_CGROUP_PARENT names a
 * memory-delegated cgroup; otherwise a coarse RLIMIT_AS fallback. mem_cap==0 or
 * below a floor => no cap; timeout_s==0 => no timeout (a hanging task with no
 * timeout will block its worker). */
gptps_status gptps_oop_execute(const gptps_task_def *def, const void *payload, size_t plen,
                               uint64_t mem_cap, uint32_t timeout_s,
                               void **out_result, size_t *out_len);

/* --- minimal TOML-subset config parser (config_toml.c) --- */
typedef struct gptps_toml gptps_toml;
gptps_toml *gptps_toml_parse_file(const char *path, char *errbuf, size_t errlen); /* NULL on error */
void        gptps_toml_free(gptps_toml *t);
int         gptps_toml_int(const gptps_toml *t, const char *section, const char *key, long long *out);
int         gptps_toml_double(const gptps_toml *t, const char *section, const char *key, double *out);
int         gptps_toml_bool(const gptps_toml *t, const char *section, const char *key, int *out);
const char *gptps_toml_str(const gptps_toml *t, const char *section, const char *key);
int         gptps_toml_str_array(const gptps_toml *t, const char *section, const char *key, const char *const **out);

/* --- settings registry (settings.c) --- */
typedef struct gptps_settings gptps_settings;
gptps_settings *gptps_settings_create(void);
void            gptps_settings_destroy(gptps_settings *r);
gptps_status    gptps_settings_add(gptps_settings *r, const gptps_setting_def *def);
size_t          gptps_settings_size(gptps_settings *r);
gptps_status    gptps_settings_get_by(gptps_settings *r, const char *key, char *buf, size_t cap);
gptps_status    gptps_settings_set_by(gptps_settings *r, const char *key, const char *value);
gptps_status    gptps_settings_info_at(gptps_settings *r, size_t index, gptps_setting_info *out);
gptps_status    gptps_settings_save_to(gptps_settings *r, const char *path);
gptps_status    gptps_settings_apply_toml(gptps_settings *r, const gptps_toml *t);
gptps_status    gptps_settings_watch_add(gptps_settings *r, gptps_settings_cb cb, void *ud);
/* Remove every entry whose key begins with `prefix` (e.g. "tasks.resize."); used
 * to tear down a task type's settings when it is unregistered. Returns the count
 * removed. Takes only the settings lock (never the engine lock), so callers must
 * NOT hold the engine lock (preserve the settings->m -> engine->m order). */
size_t          gptps_settings_remove_prefix(gptps_settings *r, const char *prefix);

/* Out-of-process EXTERNAL PROGRAM executor (POSIX): fork + exec argv[0] under an
 * OS memory cap, feed `payload` on the child's stdin, read its stdout as the
 * result, hard-kill on the deadline. Exit 0 => OK, non-zero => GPTPS_E_TASK. */
gptps_status gptps_program_execute(const gptps_task_def *def, const void *payload, size_t plen,
                                   uint64_t mem_cap, uint32_t timeout_s,
                                   void **out_result, size_t *out_len);

#endif /* GPTPS_INTERNAL_H */
