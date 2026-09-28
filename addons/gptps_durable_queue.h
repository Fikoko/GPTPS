/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_durable_queue.h - crash-durable submission for GPTPS (optional add-on).
 *
 * A thin durability layer built ONLY on the public GPTPS API (gptps_submit,
 * gptps_cancel + the observer seam) plus C stdio and the platform's mutex, fsync,
 * truncate and rename. Submit work through gptps_dq_submit()
 * instead of gptps_submit(): the (task, payload) is written to an append-only
 * journal and fsync'd BEFORE the task is enqueued, so a crash after the call
 * returns is recoverable.
 *
 * Guarantee: AT-LEAST-ONCE. After a crash, gptps_dq_recover() re-submits every
 * record that was persisted but never completed, so task bodies MUST be
 * idempotent (same contract as on_failure = requeue).
 *
 * How a record ends. The add-on's observer closes a record when its task FINISHES
 * or is DROPPED, and quarantines it when the task is DEAD_LETTERED (see
 * gptps_dq_quarantined) - unless teardown ended it, below. gptps_dq_cancel closes
 * one on request, and gptps_dq_submit
 * closes the record of a submit the engine refused, returning you its error.
 * Everything else leaves the record pending, and the next run's gptps_dq_recover
 * re-submits it:
 *   - gptps_cancel(e, handle) stops the current execution only. The engine
 *     reports it as FAILED / GPTPS_E_CANCELLED, which is also how it reports
 *     running work the grace cancels, stopped services and work a MANUAL host
 *     leaves unstepped, so this add-on cannot tell an operator's cancel from a
 *     teardown's and treats both as "not done". The record stays
 *     pending - and counted by gptps_dq_pending - for the rest of this run,
 *     unless gptps_dq_cancel retracts it or the execution FINISHED before the
 *     cancel reached it. To withdraw the work, use gptps_dq_cancel.
 *   - Work that shutdown gives up on: whatever a MANUAL host leaves unstepped,
 *     running work the grace cancels and stopped services (FAILED /
 *     GPTPS_E_CANCELLED); work still in backoff when limits.shutdown_grace_ms
 *     expires, and a REQUEUE item the drain will not start again (DEAD_LETTERED,
 *     or DROPPED under on_failure = drop, with status GPTPS_E_SHUTDOWN - see
 *     gptps_shutdown). Teardown ending work is not a verdict on it, so it is
 *     neither quarantined nor lost.
 *   A body that itself returns GPTPS_E_SHUTDOWN is still judged by its policy:
 *   that status on the attempt's own FAILED marks the dead letter or drop that
 *   follows as the task's own verdict. The observer cannot see the policy, so a
 *   teardown dead letter for an on_failure = requeue item is quarantined if the
 *   latest attempt that returned GPTPS_E_SHUTDOWN ended a cycle rather than being
 *   retried, however later cycles failed - retained, not lost; drain it with
 *   gptps_dq_drain_quarantine. A teardown that cancels the item keeps it pending.
 * Not for services that exit cleanly: a GPTPS_TASK_SERVICE without
 * GPTPS_TASK_RETIRE_ON_OK emits FINISHED each time its run() returns GPTPS_OK and
 * is restarted, so its first clean exit closes the record and a later crash does
 * not bring the service back. One that runs until it is stopped keeps its record.
 *
 * Lifecycle (ordering matters):
 *     dq = gptps_dq_open(e, "queue.journal");   // replays + compacts
 *     gptps_dq_recover(dq);                      // re-submit prior-run survivors
 *     gptps_dq_submit(dq, "task", buf, len, &h); // ... durable submits ...
 *     gptps_shutdown(e);                         // drain + join (no more events)
 *     gptps_dq_close(dq);                        // AFTER shutdown
 *
 * gptps_dq_close MUST be called after gptps_shutdown: gptps_unregister_observer
 * is a setup-time call, unsafe while the engine may still emit, so the queue must
 * outlive event delivery. gptps_dq_submit, gptps_dq_recover and gptps_dq_cancel
 * call into the engine and must not be called once gptps_shutdown has returned: it
 * frees the engine.
 *
 * Re-entrancy: gptps_dq_submit and gptps_dq_recover hold the queue's lock while
 * they call gptps_submit, which delivers the item's QUEUED event on the calling
 * thread. A callback must not call into this queue for THAT event: it would
 * re-enter a lock its own thread holds - a self-deadlock on POSIX, and on Windows,
 * where the lock is a recursive CRITICAL_SECTION, a nested call that can
 * reallocate the record table under the outer one. Any other event is fine.
 *
 * Portable (Linux/macOS/Windows) via the header-only addons/addon_compat.h shim.
 * Build: cc ... gptps_durable_queue.c
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
 * not enqueue. Returns GPTPS_E_INVAL for a NULL dq/task_name, a task_name longer
 * than 4096 bytes, or len above 256 MiB - the journal format's limits, which are
 * rejected here rather than written as a record the replayer would discard. */
gptps_status gptps_dq_submit(gptps_dq *dq, const char *task_name,
                             const void *payload, size_t len, gptps_handle *out_handle);

/* Re-submit every record that survived from a previous run/crash (persisted but
 * never completed). Quarantined (dead-lettered) records are terminal-but-retained,
 * NOT incomplete: they are skipped here - recover them out-of-band via
 * gptps_dq_drain_quarantine. Returns the count re-submitted. Idempotent task
 * bodies required (at-least-once). Safe to call once after open. */
size_t gptps_dq_recover(gptps_dq *dq);

/* Retract a durable submit: close the record for `h` so no later gptps_dq_recover
 * re-submits it, then gptps_cancel(e, h) the execution. `h` is an engine handle
 * from THIS run: one gptps_dq_submit returned, or one the engine gave a record
 * gptps_dq_recover re-submitted (gptps_dq_recover returns only a count, so that
 * handle is known only from the engine's events). The retraction is fsync'd
 * before the engine is told, so it survives a crash; if it cannot be made durable
 * the call returns GPTPS_E_IO, leaving the record open and the execution alone.
 *
 * Returns GPTPS_OK once the record is retracted - including for work an earlier
 * gptps_cancel already stopped. A still-live item then ends as gptps_cancel
 * describes; work already completing may still FINISH, as it may under
 * gptps_cancel, but it is never re-submitted.
 * GPTPS_E_SHUTDOWN if the engine is tearing down (a call from an event callback
 * while gptps_shutdown drains): the record is retracted all the same, but the
 * execution was not stopped and may still run.
 * GPTPS_E_NOTFOUND if this queue holds no open record for `h`: unknown, closed
 * already, or quarantined (drain those instead).
 * GPTPS_E_INVAL for a NULL dq or h == 0. */
gptps_status gptps_dq_cancel(gptps_dq *dq, gptps_handle h);

/* Number of records currently persisted-but-not-completed. */
size_t gptps_dq_pending(gptps_dq *dq);

/* Quarantine: a record whose task is DEAD_LETTERED - it exhausted its retries under
 * the dead_letter policy, admission refused it outright, or it failed under any
 * policy but drop while its type was being removed with GPTPS_REMOVE_DRAIN - is
 * RETAINED in the journal (its poison payload survives a crash) rather than
 * silently dropped. Teardown's dead letters stay pending instead, except the
 * requeue case above. Inspect / recover these out-of-band. */
size_t gptps_dq_quarantined(gptps_dq *dq);   /* count of retained dead-lettered records */

/* Drain quarantined records: `cb` is called for each (payload valid only for the
 * call; cb must not re-enter this queue), the records are then cleared and the
 * journal compacted. Returns the number drained. cb may be NULL to discard. */
typedef void (*gptps_dq_quarantine_cb)(const char *task_name, const void *payload,
                                       size_t len, void *user_data);
size_t gptps_dq_drain_quarantine(gptps_dq *dq, gptps_dq_quarantine_cb cb, void *user_data);

/* As above, and reports whether the journal was successfully compacted afterwards.
 *
 * The return value counts records the callback saw, and that is true either way -
 * so a compaction failure cannot be signalled through it. But the consequence
 * matters: if compaction fails the drained records are still in the journal, so a
 * RESTART re-quarantines them and your callback sees them a second time. That is
 * this add-on's at-least-once contract applied to the drain, and it is fine for an
 * idempotent callback and not fine for one that bills, emails, or files a ticket.
 *
 * *out_compact (may be NULL) receives GPTPS_OK if the journal was compacted, the
 * failure status if it was not, and GPTPS_OK when there was nothing to drain.
 * Everything else - including the at-least-once guarantee itself - is unchanged. */
size_t gptps_dq_drain_quarantine_ex(gptps_dq *dq, gptps_dq_quarantine_cb cb,
                                    void *user_data, gptps_status *out_compact);

/* Rewrite the journal to contain only still-pending records, bounding its growth
 * within a long-running process. Returns GPTPS_OK or GPTPS_E_IO.
 *
 * Not optional at scale: nothing else bounds the journal, and both replay on open
 * and the observer's per-completion lookup walk everything accumulated since the
 * last compaction. A process that never compacts pays for it in gptps_dq_open,
 * the one path that must be fast after a crash. */
gptps_status gptps_dq_compact(gptps_dq *dq);

/* Close the queue and free it. Call AFTER gptps_shutdown(e). */
void gptps_dq_close(gptps_dq *dq);

#ifdef __cplusplus
}
#endif
#endif /* GPTPS_DURABLE_QUEUE_H */
