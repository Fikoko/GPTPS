/*
 * gptps_internal.h - internal prototypes shared across core translation units.
 * Not installed; not part of the public ABI.
 */
#ifndef GPTPS_INTERNAL_H
#define GPTPS_INTERNAL_H

#include "gptps.h"

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

/* Out-of-process EXTERNAL PROGRAM executor (POSIX): fork + exec argv[0] under an
 * OS memory cap, feed `payload` on the child's stdin, read its stdout as the
 * result, hard-kill on the deadline. Exit 0 => OK, non-zero => GPTPS_E_TASK. */
gptps_status gptps_program_execute(const char *const *argv, const void *payload, size_t plen,
                                   uint64_t mem_cap, uint32_t timeout_s,
                                   void **out_result, size_t *out_len);

#endif /* GPTPS_INTERNAL_H */
