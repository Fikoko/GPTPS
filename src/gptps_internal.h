/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_internal.h - internal prototypes shared across core translation units.
 * Not installed; not part of the public ABI.
 */
#ifndef GPTPS_INTERNAL_H
#define GPTPS_INTERNAL_H

#include "gptps.h"
#include "gptps_hal.h"   /* gptps_flag (executor cancel), gptps_hal_monotonic_ms */

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

/* Frozen minimums for the other caller-supplied INPUT structs: the point just past
 * the current LAST field. This does NOT track sizeof (unlike `< sizeof *x`), so
 * appending a field later leaves the minimum where it is and a caller compiled
 * against today's header keeps validating - i.e. it is the append-safe floor. A
 * field appended past here is read via GPTPS_STRUCT_HAS and, to stay unambiguous,
 * must GROW sizeof (not hide in trailing padding - see the task_def note above). */
#define GPTPS_LAST_FIELD_END(type, last) (offsetof(type, last) + sizeof(((type *)0)->last))
#define GPTPS_CONFIG_MIN_SIZE         GPTPS_LAST_FIELD_END(gptps_config, mode)
#define GPTPS_SUBMIT_OPTIONS_MIN_SIZE GPTPS_LAST_FIELD_END(gptps_submit_options, timeout_ms)
#define GPTPS_ALLOCATOR_MIN_SIZE      GPTPS_LAST_FIELD_END(gptps_allocator, user_data)
#define GPTPS_ADDON_MIN_SIZE          GPTPS_LAST_FIELD_END(gptps_addon, teardown)
/* gptps_addon_info is an OUTPUT struct, but the same rule applies for the same
 * reason: validating it with `< sizeof *out` would pin it to today's size, so the
 * day a field is appended every already-compiled caller starts getting E_INVAL. That
 * is exactly the trap this whole section exists to avoid, and it is free to avoid
 * only while the struct is unreleased. (gptps_task_info and gptps_setting_info carry
 * the older `< sizeof` check; those shipped in 1.0.0 and are frozen as they are.) */
#define GPTPS_ADDON_INFO_MIN_SIZE     GPTPS_LAST_FIELD_END(gptps_addon_info, enabled)

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

/* Build a ctx, run the task in THIS process, and hand out a BORROWED pointer to its
 * result bytes (NULL/0 if none). The caller must NOT free them: this is called only
 * in the forked OOP child, which must not touch the allocator (a host allocator's
 * lock may have been held by a thread that did not survive fork()) and which _exit()s
 * immediately after writing the bytes to its pipe. */
gptps_status gptps_run_capture(const gptps_task_def *def, const void *payload, size_t plen,
                               void **out_result, size_t *out_len);

/* Out-of-process executor (POSIX): fork, apply an OS memory cap in the child,
 * run the task there, and stream the result back. The parent hard-kills the
 * child on timeout (returns GPTPS_E_TIMEOUT) - real enforcement the in-process
 * path cannot provide. The memory cap is accurate cgroup v2 (memory.max +
 * swap.max=0, exceeding it => GPTPS_E_NOMEM) when GPTPS_CGROUP_PARENT names a
 * memory-delegated cgroup; otherwise a coarse RLIMIT_AS fallback. mem_cap==0 or
 * below a floor => no cap. `cancel` (may be NULL) is the running item's cooperative
 * cancel flag: the parent waits in bounded slices and hard-kills the child when it is
 * raised, so a cancel / shutdown / task-removal stops even a no-timeout (timeout_s==0)
 * child instead of blocking its worker forever.
 * The kill REASON is reported distinctly: GPTPS_E_CANCELLED for the flag,
 * GPTPS_E_TIMEOUT for the deadline, GPTPS_E_IO for a pump failure - so an operator's
 * cancel is never mistaken for a deadline breach. A child that stops talking without
 * exiting is reaped with a bounded grace period, never an unbounded waitpid(). */
gptps_status gptps_oop_execute(const gptps_task_def *def, const void *payload, size_t plen,
                               uint64_t mem_cap, uint32_t timeout_s, gptps_flag *cancel,
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
 * result, hard-kill on the deadline. Exit 0 => OK, non-zero => GPTPS_E_TASK. The
 * parent pumps stdin and stdout CONCURRENTLY (a single poll loop), so a large
 * payload through a streaming child does not deadlock. `cancel` (may be NULL) is
 * the item's cooperative cancel flag - the pump kills the child when it is raised,
 * so a no-timeout program can still be cancelled / shut down (reported as
 * GPTPS_E_CANCELLED, distinct from a deadline's GPTPS_E_TIMEOUT). Stdout EOF means the
 * child closed its output, NOT that it exited, so the reap is bounded: a program that
 * goes silent and keeps running is SIGKILLed after a grace period. */
gptps_status gptps_program_execute(const gptps_task_def *def, const void *payload, size_t plen,
                                   uint64_t mem_cap, uint32_t timeout_s, gptps_flag *cancel,
                                   void **out_result, size_t *out_len);

#endif /* GPTPS_INTERNAL_H */
