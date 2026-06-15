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
 * path cannot provide. mem_cap==0 or below a floor => no AS cap; timeout_s==0
 * => no timeout (a hanging task with no timeout will block its worker). */
gptps_status gptps_oop_execute(const gptps_task_def *def, const void *payload, size_t plen,
                               uint64_t mem_cap, uint32_t timeout_s,
                               void **out_result, size_t *out_len);

#endif /* GPTPS_INTERNAL_H */
