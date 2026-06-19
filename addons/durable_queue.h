/*
 * durable_queue.h - crash-durable submission for GPTPS (optional add-on).
 *
 * A thin durability layer built ONLY on the public GPTPS API (gptps_submit +
 * the observer seam) plus POSIX file I/O. Submit work through gptps_dq_submit()
 * instead of gptps_submit(): the (task, payload) is written to an append-only
 * journal and fsync'd BEFORE the task is enqueued, so a crash after the call
 * returns is recoverable. The add-on observes lifecycle events and marks a
 * record complete when its task FINISHES or is DEAD_LETTERED.
 *
 * Guarantee: AT-LEAST-ONCE. After a crash, gptps_dq_recover() re-submits every
 * record that was persisted but never completed, so task bodies MUST be
 * idempotent (same contract as on_failure = requeue). Tasks using
 * on_failure = drop are not observable as terminal, so a dropped failure stays
 * in the journal and will be replayed on recover.
 *
 * Lifecycle (ordering matters):
 *     dq = gptps_dq_open(e, "queue.journal");   // replays + compacts
 *     gptps_dq_recover(dq);                      // re-submit prior-run survivors
 *     gptps_dq_submit(dq, "task", buf, len, &h); // ... durable submits ...
 *     gptps_shutdown(e);                         // drain + join (no more events)
 *     gptps_dq_close(dq);                        // AFTER shutdown
 *
 * gptps_dq_close MUST be called after gptps_shutdown: the engine has no
 * unregister-observer call, so the queue must outlive event delivery.
 *
 * Portable (Linux/macOS/Windows) via the addon_compat shim. Build: cc ... durable_queue.c addons/addon_compat.h is header-only
 */
#ifndef GPTPS_DURABLE_QUEUE_H
#define GPTPS_DURABLE_QUEUE_H

#include "gptps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gptps_dq gptps_dq;

/* Open (or create) a durable queue backed by `journal_path`, attached to engine
 * `e`. Replays the journal (loading records persisted but not completed) and
 * compacts the file to just those. Registers an observer on `e`. Returns NULL on
 * I/O error or a corrupt journal header. Does NOT re-submit (see _recover). */
gptps_dq *gptps_dq_open(gptps *e, const char *journal_path);

/* Durable submit: persist (task_name, payload) to the journal and fsync it
 * BEFORE enqueuing via gptps_submit. Returns gptps_submit's status (and its
 * handle via out_handle); on a journal write error returns GPTPS_E_IO and does
 * not enqueue. */
gptps_status gptps_dq_submit(gptps_dq *dq, const char *task_name,
                             const void *payload, size_t len, gptps_handle *out_handle);

/* Re-submit every record that survived from a previous run/crash (persisted but
 * never completed). Returns the count re-submitted. Idempotent task bodies
 * required (at-least-once). Safe to call once after open. */
size_t gptps_dq_recover(gptps_dq *dq);

/* Number of records currently persisted-but-not-completed. */
size_t gptps_dq_pending(gptps_dq *dq);

/* Rewrite the journal to contain only still-pending records, bounding its growth
 * within a long-running process. Returns GPTPS_OK or GPTPS_E_IO. */
gptps_status gptps_dq_compact(gptps_dq *dq);

/* Close the queue and free it. Call AFTER gptps_shutdown(e). */
void gptps_dq_close(gptps_dq *dq);

#ifdef __cplusplus
}
#endif
#endif /* GPTPS_DURABLE_QUEUE_H */
