/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_await.h - block until a handle reaches a terminal state (add-on).
 *
 * The core is deliberately event-driven: result delivery is an event, and
 * "futures / promises / async in the engine" is a stated NON-GOAL, on the grounds
 * that a blocking wait(handle) is a small amount of code on the observer seam and
 * does not need to be in the mechanism. This file is that code. It exists so the
 * claim is backed by something you can link rather than by an assertion.
 *
 *   aw = gptps_await_install(e);              // BEFORE you submit anything
 *   gptps_submit(e, "resize", buf, len, &h);
 *   gptps_await_wait(aw, h, 5000, &res, &rlen, &task_status);
 *   free(res);
 *   ... gptps_shutdown(e); gptps_await_close(aw);   // close AFTER shutdown
 *
 * WHY IT MUST BE INSTALLED BEFORE YOU SUBMIT
 *   In THREADED mode the dispatcher can admit an item and a worker can finish it
 *   BEFORE gptps_submit returns the handle to you. A wait that registered interest
 *   only when called would miss that completion and block until its timeout. So the
 *   observer is registered at install time and completions are retained for handles
 *   nobody is waiting on yet - which closes the race in the only place it can be
 *   closed. There is a test for exactly this.
 *
 * RETENTION IS BOUNDED, AND THAT IS A REAL CONTRACT
 *   Retaining every completion forever is the leak in addons/gptps_orch (whose own
 *   header admits its `done` set grows for the process lifetime). This add-on keeps
 *   a fixed-size ring instead - gptps_await_install_ex lets you size it - and evicts
 *   the OLDEST unclaimed completion when it is full.
 *
 *   Consequence, stated plainly: if you submit more than `cap` items and only then
 *   start waiting on the earliest handles, their completions are gone and those
 *   waits block until their timeout. Wait reasonably promptly, size the ring for
 *   your in-flight count, or use gptps_await_quiesce when you only need "all done"
 *   rather than a specific result. Always pass a timeout you can live with.
 *
 * TWO THINGS THIS IS NOT
 *   - Not a future: nothing is chained, and there is no callback composition.
 *     Dependencies between tasks are addons/gptps_orch's job.
 *   - Not usable from inside a task body or an event callback. Blocking a worker
 *     thread on another item's completion invites deadlock - with a small pool you
 *     can occupy every worker with waiters and have nobody left to run the work.
 *     Wait from the submitting thread. (gptps_shutdown and gptps_step already refuse
 *     re-entrant use for a related reason.)
 *
 * Portable: public C99 API plus addon_compat.h's mutex/condvar. No core change.
 */
#ifndef GPTPS_AWAIT_H
#define GPTPS_AWAIT_H

#include "gptps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gptps_await gptps_await;

/* Install on an engine. Registers one observer; call BEFORE submitting work.
 * Returns NULL on allocation failure or if the observer could not be registered. */
gptps_await *gptps_await_install(gptps *e);

/* As above, with an explicit retention ring size (0 = the default, 256). */
gptps_await *gptps_await_install_ex(gptps *e, size_t cap);

/* Block until `h` reaches a terminal state, or `timeout_ms` elapses.
 *
 * Returns:
 *   GPTPS_OK          the handle reached a terminal state. *out_status carries the
 *                     TASK's outcome (GPTPS_OK, or why it failed); *out_result /
 *                     *out_len carry a malloc'd copy of the result, which the CALLER
 *                     frees. Pass NULL for any out-param you do not want.
 *   GPTPS_E_TIMEOUT   no terminal event arrived in time - the task may still be
 *                     running, or its completion was evicted from the ring.
 *   GPTPS_E_INVAL     bad arguments.
 *   GPTPS_E_NOMEM     could not register the wait.
 *
 * Note the two statuses: the RETURN says whether the wait succeeded, *out_status
 * says whether the TASK did. Do not conflate them - a task that failed is a
 * successful wait. (The same split addons/gptps_xport uses for transport vs task.)
 *
 * timeout_ms == 0 polls: it returns immediately, with GPTPS_E_TIMEOUT if the handle
 * has not already completed. */
gptps_status gptps_await_wait(gptps_await *aw, gptps_handle h, unsigned timeout_ms,
                              void **out_result, size_t *out_len,
                              gptps_status *out_status);

/* Total terminal events this observer has seen since install. Monotonic. */
unsigned long gptps_await_count(gptps_await *aw);

/* Block until gptps_await_count() >= n, or timeout. Returns GPTPS_OK or
 * GPTPS_E_TIMEOUT. This is the "I submitted N things, tell me when they are all
 * done" case - it needs no retention at all, so it has none of the ring's limits
 * and is the right call for bulk work and benchmarks. */
gptps_status gptps_await_quiesce(gptps_await *aw, unsigned long n, unsigned timeout_ms);

/* Free. Call AFTER gptps_shutdown, like the other observer add-ons: the contract is
 * that no observer can still be firing, and that any thread blocked in
 * gptps_await_wait has already returned.
 *
 * The ordering is not a style preference. gptps_shutdown frees the engine, which
 * takes the observer registration with it - so there is nothing left to unregister
 * and nothing that can still call back into this object. Closing FIRST would leave a
 * live registration pointing at freed memory, which is the reverse hazard and a much
 * worse one. */
void gptps_await_close(gptps_await *aw);

#ifdef __cplusplus
}
#endif

#endif /* GPTPS_AWAIT_H */
