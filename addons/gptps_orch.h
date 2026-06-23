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
 */
#ifndef GPTPS_ORCH_H
#define GPTPS_ORCH_H

#include "gptps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gptps_orch gptps_orch;

/* Install the orchestrator on an engine (registers one observer). NULL on OOM. */
gptps_orch *gptps_orch_install(gptps *e);

/* Submit `task` to run only after EVERY handle in deps[0..ndeps) reaches a
 * terminal state (finished, failed, dropped, or dead-lettered). If all deps are
 * already terminal, submits immediately and *out (may be NULL) gets the engine
 * handle; otherwise the gate is held and *out is set to 0 (it is submitted later
 * from the observer, so its handle is not returned here). ndeps == 0 submits now. */
gptps_status gptps_orch_after(gptps_orch *o, const char *task,
                              const void *payload, size_t len,
                              const gptps_handle *deps, size_t ndeps,
                              gptps_handle *out);

/* Number of gates still waiting on dependencies (not yet released). */
size_t gptps_orch_pending(gptps_orch *o);

/* Free the orchestrator. Call AFTER gptps_shutdown(e) so no observer fires. */
void gptps_orch_close(gptps_orch *o);

#ifdef __cplusplus
}
#endif
#endif /* GPTPS_ORCH_H */
