/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_retry.h - retry rules by exit code (add-on).
 *
 * A program's exit code often says whether trying again can help. A vendor's
 * convention might be: 0 ok, 1 a lock or a runtime error (retry), 2 bad input
 * (retrying will not help), 3 the licence server (retry, but much later). The core
 * retries every failure the same way - which failures are worth retrying is policy,
 * and policy is an add-on's. This is that add-on.
 *
 *   gptps_retry *r;
 *   gptps_retry_install(e, &r);                          // setup time
 *   gptps_retry_on_exit(r, NULL, 2, GPTPS_RETRY_NEVER);  // any task: 2 is final
 *   gptps_retry_on_exit(r, "mrp", 3, 3600000);           // "mrp": 3 waits an hour
 *   ...
 *   gptps_shutdown(e);
 *   gptps_retry_close(r);                                // AFTER shutdown
 *
 * HOW. It reads the exit.code measurement (ABI 2.7) on each GPTPS_EXEC_PROGRAM
 * attempt's FAILED event (observer seam), and decides that item's NEXT attempt when
 * it comes up for admission (constraint seam):
 *
 *   GPTPS_RETRY_NEVER  the retry is refused: the item is dead-lettered at once with
 *                      GPTPS_E_DENIED - its retries are not spent on work that cannot
 *                      succeed. The dead letter keeps its stderr tail, if the task
 *                      captures one (gptps_task_def.stderr_tail_bytes).
 *   N > 0              the retry waits N ms more, on top of the policy's backoff.
 *   0                  retried as the policy says - an explicit "normal", for a task
 *                      that must not follow an all-tasks rule.
 *
 * It decides only WHEN or WHETHER a retry the policy already allows happens: it
 * never adds one. A task with max_retries = 0 is never retried, whatever its code;
 * give "retry much later" a max_retries of at least 1. A rule for one task wins over
 * one for every task (task_name NULL). An exit code with no rule changes nothing,
 * and so does a failure that has no exit code (a timeout, a cancel, an in-process
 * task).
 *
 * LIFETIME. Install at setup time, while the engine is quiescent, as the seams
 * require; rules may be added later. Close AFTER gptps_shutdown: the engine calls
 * into the add-on until it is gone, and gptps_retry_close does not call the engine.
 *
 * Portable: public C99 API plus addon_compat.h's mutex. No core change.
 */
#ifndef GPTPS_RETRY_H
#define GPTPS_RETRY_H

#include "gptps.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GPTPS_RETRY_NEVER 0xFFFFFFFFu   /* wait_ms: never retry this exit code */

typedef struct gptps_retry gptps_retry;

/* Register the observer and the constraint on `e`. */
gptps_status gptps_retry_install(gptps *e, gptps_retry **out);

/* What a retry does after an attempt that exited with `exit_code`, for the task type
 * `task_name` (NULL = every task type): wait_ms as above. Setting a rule again
 * replaces it. GPTPS_E_INVAL for a NULL `r` or an over-long name (over 128 bytes),
 * GPTPS_E_NOMEM when out of memory. */
gptps_status gptps_retry_on_exit(gptps_retry *r, const char *task_name, uint32_t exit_code,
                                 uint32_t wait_ms);

/* How many retries it refused, and how many it held back. */
uint64_t gptps_retry_refused(const gptps_retry *r);
uint64_t gptps_retry_delayed(const gptps_retry *r);

/* Free it. Call AFTER gptps_shutdown (see LIFETIME). NULL is a no-op. */
void gptps_retry_close(gptps_retry *r);

#ifdef __cplusplus
}
#endif
#endif /* GPTPS_RETRY_H */
