/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_orch.h - task orchestration (run-after / fan-in join) as a pure add-on.
 *
 * Built ENTIRELY on the public seams - an observer for terminal events plus
 * gptps_submit - with no core changes. It is the proof that runtime task
 * dependencies are buildable on top of GPTPS rather than baked into the engine:
 * gate a submission on a set of handles, and release it (submit it) once they
 * have all reached a terminal state.
 *
 *   o = gptps_orch_install(e);
 *   gptps_submit(e, "A", ..., &hA);
 *   gptps_submit(e, "B", ..., &hB);
 *   gptps_handle deps[2] = { hA, hB };
 *   gptps_orch_after(o, "C", ..., deps, 2, NULL);   // C runs after A and B finish
 *   ... gptps_shutdown(e); gptps_orch_close(o);      // close AFTER shutdown
 *
 * Contract: deps are engine handles; a gate must be created before its deps can
 * terminate OR while the orchestrator is still tracking them (it remembers
 * completed handles for the process lifetime - prune in a long-running host).
 * Deps within one gate must be distinct.
 *
 * WHAT COUNTS AS "TERMINAL" - and what never gets there:
 *   A dependency is satisfied by the LAST event its handle will ever produce:
 *   FINISHED, DROPPED, DEAD_LETTERED, or FAILED carrying GPTPS_E_CANCELLED.
 *   A plain FAILED is NOT terminal - it is emitted after every failed ATTEMPT, and
 *   the engine then decides retry / drop / dead-letter. A dep that merely retries
 *   must not release your gate, and a FAILED carrying GPTPS_E_TIMEOUT is a failed
 *   attempt like any other.
 *
 *   Two dependency shapes NEVER reach a terminal state, so a gate on one waits
 *   forever - correctly, but surprisingly:
 *     - a task type with on_failure = GPTPS_ON_FAILURE_REQUEUE, which is
 *       re-admitted instead of ending (until shutdown, which dead-letters it);
 *     - a GPTPS_TASK_SERVICE instance, which is supervised to stay up by design.
 *   Depend on those only if you will cancel them, which IS terminal.
 */
#ifndef GPTPS_ORCH_H
#define GPTPS_ORCH_H

#include "gptps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gptps_orch gptps_orch;

/* Install the orchestrator on an engine (registers one observer). NULL on OOM.
 * Remembers completed handles for the process lifetime; see gptps_orch_install_ex
 * and gptps_orch_prune if that is too much for your host. */
gptps_orch *gptps_orch_install(gptps *e);

/* Install with a ceiling on how many completed handles are remembered.
 * done_cap == 0 is exactly gptps_orch_install.
 *
 * The remembered set exists to answer one question: "had this handle already
 * terminated when gptps_orch_after named it?" Nothing else reads it, so it can be
 * dropped wholesale at any time - the ONLY consequence is that a gate created
 * afterwards which names a handle that finished before the drop will wait forever,
 * which is the same outcome this header already documents for a gate created too
 * late. Past done_cap entries the set is dropped automatically, so memory is
 * bounded and the cost is that contract, applied on a schedule you did not pick.
 *
 * Use it when gates are created promptly after their dependencies are submitted -
 * the normal pattern. If your host creates gates long after the fact, leave it
 * unbounded and call gptps_orch_prune() at a point where YOU know no future gate
 * will name an older handle. */
gptps_orch *gptps_orch_install_ex(gptps *e, size_t done_cap);

/* Forget every remembered completed handle; returns how many were forgotten.
 * Safe at any time - no unreleased gate depends on the set (a dependency present
 * when the gate was created was resolved immediately, and one that terminates later
 * is resolved in the same call that records it). See gptps_orch_install_ex for the
 * one consequence. */
size_t gptps_orch_prune(gptps_orch *o);

/* Submit `task` to run only after EVERY handle in deps[0..ndeps) reaches a
 * terminal state (finished, failed, dropped, or dead-lettered). If all deps are
 * already terminal, submits immediately and *out (may be NULL) gets the engine
 * handle; otherwise the gate is held and *out is set to 0 (it is submitted later
 * from the observer, so its handle is not returned here). ndeps == 0 submits now. */
gptps_status gptps_orch_after(gptps_orch *o, const char *task,
                              const void *payload, size_t len,
                              const gptps_handle *deps, size_t ndeps,
                              gptps_handle *out);

/* Number of gates not yet released: still waiting on a dependency, or waiting on a
 * retry of a submission the engine transiently rejected.
 *
 * This always converges to 0, which is what makes it usable as a drain predicate. A
 * gate whose dependencies are all terminal is submitted immediately; if the engine
 * rejects that submit transiently (the task type is PAUSED, or the intake queue is
 * full) the gate is retried on each subsequent terminal event, a bounded number of
 * times, and then abandoned. A permanent rejection - an unregistered task name, a
 * cost that can never fit the budget, a cost hook that refuses - ends the gate at
 * once. In both of those cases the task never runs and no event is emitted for it,
 * so do not treat pending() reaching 0 as proof every gated task ran. */
size_t gptps_orch_pending(gptps_orch *o);

/* STALLED GATES.
 *
 * A gate whose dependencies are all satisfied is submitted immediately. That submit
 * can be refused - the task type is paused, the intake queue is full, the name was
 * never registered, the cost can never fit - and the orchestrator will not retry
 * forever (see gptps_orch_pending). A gate it gives up on is ABANDONED: it stops
 * being pending, and its task never ran and never emitted an event. Without these,
 * that outcome is indistinguishable from success and a host has no way to notice.
 *
 * gptps_orch_stalled() counts them. gptps_orch_stalled_at() reports one by index
 * (0..count-1): `buf` receives a COPY of the task name, truncated to `cap`, and
 * *out_last (may be NULL) receives the status the engine last refused it with -
 * GPTPS_E_NOTFOUND for a paused or unregistered type, GPTPS_E_FULL for a full
 * intake, GPTPS_E_BUDGET for a cost that can never fit. GPTPS_E_NOTFOUND is
 * returned for an index past the end. The index is only stable while you hold no
 * other orchestrator call in flight. */
size_t       gptps_orch_stalled(gptps_orch *o);
gptps_status gptps_orch_stalled_at(gptps_orch *o, size_t index,
                                   char *buf, size_t cap, gptps_status *out_last);

/* Re-arm and immediately re-submit every abandoned gate; returns how many were
 * accepted this time. Call it after fixing the condition that caused the refusal -
 * resuming a paused task type, draining the intake queue, registering the missing
 * name. It submits at once rather than waiting for the next terminal event, because
 * on an idle engine there may never be another one. Gates that are refused again
 * are simply abandoned again. */
size_t gptps_orch_retry(gptps_orch *o);

/* Free the orchestrator. Call AFTER gptps_shutdown(e) so no observer fires. */
void gptps_orch_close(gptps_orch *o);

#ifdef __cplusplus
}
#endif
#endif /* GPTPS_ORCH_H */
