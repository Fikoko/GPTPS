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
 * returns is recoverable. Only the submitting thread waits for the disk: the wait
 * holds no lock the engine's own threads need, and submits made at the same time
 * share one fsync.
 *
 * Guarantee: AT-LEAST-ONCE. After a crash, gptps_dq_recover() re-submits every
 * record that was persisted but never completed, so task bodies MUST be
 * idempotent (same contract as on_failure = requeue).
 *
 * How a record ends. The add-on's observer closes a record when its task FINISHES,
 * is DROPPED, or its body cancels itself (a FAILED / GPTPS_E_CANCELLED marked
 * GPTPS_EV_FLAG_SELF_CANCELLED), and quarantines it when the task is DEAD_LETTERED
 * (see gptps_dq_quarantined) - unless teardown ended it, below - or when its body has
 * killed the process three times (see "Crash loops"). gptps_dq_cancel
 * closes one on request, and gptps_dq_submit closes the record of a submit the
 * engine refused, returning you its error. Everything else leaves the record
 * pending, and the next run's gptps_dq_recover re-submits it:
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
 *     GPTPS_E_CANCELLED); work still queued or in backoff when
 *     limits.shutdown_grace_ms expires, and a REQUEUE item the drain will not start
 *     again (DEAD_LETTERED, or DROPPED under on_failure = drop, marked
 *     GPTPS_EV_FLAG_SHUTDOWN - see gptps_shutdown). Teardown ending work is not a
 *     verdict on it, so it is neither quarantined nor lost.
 *   A body that itself returns GPTPS_E_SHUTDOWN is still judged by its policy:
 *   its dead letter or drop carries that status but not the flag, so it is
 *   quarantined or closed like any other failure.
 * Not for services that exit cleanly: a GPTPS_TASK_SERVICE without
 * GPTPS_TASK_RETIRE_ON_OK emits FINISHED each time its run() returns GPTPS_OK and
 * is restarted, so its first clean exit closes the record and a later crash does
 * not bring the service back. One that runs until it is stopped keeps its record.
 *
 * Crash loops. A body that takes its whole process down - a segfault, an abort, the
 * OOM killer - never lets the engine dead-letter it, so on its own it would be
 * recovered by every run and kill every run. The journal therefore notes when each
 * attempt starts and ends, and gptps_dq_open counts, per record, the deaths of the
 * process while that record was running; an attempt that ends, however it ends,
 * clears the count. One death proves nothing and changes nothing. After two the
 * record is a suspect, and suspects go back one at a time (see gptps_dq_recover), so
 * the next death can be laid at one of them rather than at whatever ran beside the
 * culprit. After three it is quarantined at open instead of recovered, and a warning
 * goes to the core's log sink (gptps_set_log_sink). As the count clears only when an
 * attempt ends, so is work that a process dying for other reasons never lets finish -
 * the honest outcome, since that work cannot complete there.
 *
 * I/O errors. A durable call that returns GPTPS_E_IO - gptps_dq_submit, _submit_batch,
 * gptps_dq_cancel - changed nothing that a restart or a power cut can bring back: the
 * queue cuts what it wrote back out of the journal, and makes the cut durable, before
 * it answers. If even that fails, the queue BREAKS. The calls failing at that moment
 * return GPTPS_E_IO although what they wrote may still be read back by the next
 * gptps_dq_open - a submit's record recovered, a retraction's record closed - every
 * later durable call returns GPTPS_E_IO without writing, and an error goes to the
 * core's log sink. A gptps_dq_compact that returns GPTPS_OK repairs it: it rewrites
 * the journal from what the queue holds, and once it has, nothing that failed can come
 * back. A compaction that replaces the journal but cannot reopen it breaks the queue
 * too. A compacted journal is put in place by a rename, which a sync of its directory
 * makes durable. Until one succeeds, nothing written to the new journal is
 * acknowledged. A directory that cannot be synced at all - the process may write it
 * but not read it, or its file system has no fsync for a directory - is not an error:
 * the queue goes on without the sync, and there a power cut can still undo a
 * compaction, and lose what was acknowledged since.
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
 * thread. So does the hand-over of a suspect (see "Crash loops"), on the thread that
 * delivered the event ending the previous suspect's turn - an engine thread, or the
 * caller of gptps_cancel or gptps_dq_cancel. A callback must not call into this queue
 * for THAT event: it would re-enter a lock its own thread holds - a self-deadlock on
 * POSIX, and on Windows, where the lock is a recursive CRITICAL_SECTION, a nested
 * call that can reallocate the record table under the outer one. Any other event is
 * fine.
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
 * compacts the file to just those. Registers an observer on `e`, so like
 * gptps_register_observer it is a setup-time call. Does NOT re-submit (see _recover).
 *
 * Returns NULL, and leaves the journal as it was, when:
 *   - e or journal_path is NULL, or there is too little memory to open it;
 *   - the journal cannot be read: it exists but cannot be opened for reading (only a
 *     journal that does not exist is a new, empty one), or a read fails;
 *   - its file header is corrupt;
 *   - it is cut short by damage (below) and no copy of it can be made;
 *   - the observer cannot be registered on `e`;
 *   - the compaction fails before it replaces the journal: its new file cannot be
 *     created, written, fsync'd or renamed into place.
 * Once the compaction has replaced the journal the open does not fail. If the journal's
 * directory then cannot be synced, nothing is acknowledged until a sync of it succeeds,
 * and a warning goes to the core's log sink; if the journal cannot be reopened for
 * appending, the queue it returns is broken (see "I/O errors").
 *
 * A damaged journal. The record a crash was writing when it struck - torn, at the end
 * of the file - is dropped silently; that is what a crash leaves. Damage anywhere
 * else is skipped and every valid record after it kept, whatever the damaged bytes
 * held being lost. The original file is first copied to "<journal_path>.corrupt", or
 * the first of ".corrupt.1" to ".corrupt.9" that does not exist - once all ten exist,
 * the oldest of them, by modification time, is replaced - and a warning goes to the
 * core's log sink. A copy identical to the journal already there is not made again.
 * One case is not guessed at: a record whose header claims more bytes than the file
 * holds looks exactly like a torn write, so valid records after it are reported and
 * kept in the copy, but not loaded. Then the copy is the only place they are kept, and
 * if it cannot be made the open fails. Other damage is compacted away even when no
 * copy can be made, with a warning that says so. */
gptps_dq *gptps_dq_open(gptps *e, const char *journal_path);

/* Durable submit: persist (task_name, payload) to the journal and fsync it
 * BEFORE enqueuing via gptps_submit. Returns gptps_submit's status (and its
 * handle via out_handle); on a journal write error, or an fsync that fails,
 * returns GPTPS_E_IO, does not enqueue, and the record does not come back (see
 * "I/O errors" for the one exception). Calls from several threads share
 * fsyncs, and none of them stalls the engine while it waits. Returns GPTPS_E_INVAL for a NULL dq/task_name, a task_name longer
 * than 4096 bytes, or len above 256 MiB - the journal format's limits, which are
 * rejected here rather than written as a record the replayer would discard. */
gptps_status gptps_dq_submit(gptps_dq *dq, const char *task_name,
                             const void *payload, size_t len, gptps_handle *out_handle);

/* One item of gptps_dq_submit_batch: fill task_name, payload and len; the call fills
 * handle and status. */
typedef struct {
    const char   *task_name;
    const void   *payload;
    size_t        len;
    gptps_handle  handle;   /* out: the engine's handle, 0 unless status is GPTPS_OK */
    gptps_status  status;   /* out: what gptps_submit returned for it, or the batch's error */
} gptps_dq_item;

/* gptps_dq_submit for n items with ONE fsync: all of them are written to the journal and
 * made durable together, then each is enqueued, in order. Journaled one at a time, work
 * costs an fsync per item - about a thousand items a second from one thread on a fast
 * disk; a batch pays one fsync for the lot.
 * Returns GPTPS_OK once the batch is durable. Each item's status then says whether the
 * engine took it, and an item it refused is closed in the journal, as gptps_dq_submit
 * closes one. GPTPS_E_IO if the batch could not be made durable: nothing is enqueued,
 * and every status is GPTPS_E_IO. GPTPS_E_INVAL (an item past gptps_dq_submit's limits)
 * or GPTPS_E_NOMEM before anything is written, with every status set to it - a batch
 * is journaled whole or not at all. n == 0 is GPTPS_OK. The QUEUED events arrive on
 * this thread with the queue's lock held, as gptps_dq_submit's do. */
gptps_status gptps_dq_submit_batch(gptps_dq *dq, gptps_dq_item *items, size_t n);

/* Re-submit every record that survived from a previous run/crash (persisted but
 * never completed). Quarantined (dead-lettered) records are terminal-but-retained,
 * NOT incomplete: they are skipped here - recover them out-of-band via
 * gptps_dq_drain_quarantine. Returns the count re-submitted. Idempotent task
 * bodies required (at-least-once).
 *
 * May be called again: each call offers the engine only the records that have no
 * execution in this run yet, and a record the engine refuses stays pending for the
 * next call. That is how to recover under backpressure - with
 * limits.max_intake_depth set, a call re-submits what fits (the rest is refused with
 * GPTPS_E_FULL), and calling again once work has drained offers the rest. A
 * record whose type is not registered in this run is refused every time, so bound
 * the retries: stop when a call re-submits nothing although the engine has
 * drained.
 *
 * Suspects (see "Crash loops" above) are the exception: they are handed over one at
 * a time, oldest first - the first by this call, each next one when the previous
 * one's turn ends (its attempt ends, it reaches a verdict, or it is cancelled or
 * retracted before it runs). So the count includes at most one of them. */
size_t gptps_dq_recover(gptps_dq *dq);

/* Called for every record this queue hands back to the engine - each one
 * gptps_dq_recover re-submits, and each suspect handed over later (see "Crash loops") -
 * with its task name, payload and NEW handle. Events carry no payload, so this is how a
 * host links the work it journaled (an invoice id in the payload, say) to the handle
 * that work runs under after a restart.
 * It runs with the queue's lock held, on the thread that resubmitted the record: the
 * caller of gptps_dq_recover, or for a suspect the thread that ended the previous
 * suspect's turn. So it must not call into this queue, and the payload is valid only for
 * the call. The record's own events can reach other observers before it runs, as a
 * submit's can before gptps_submit returns: key a ledger on the handle, and attach the
 * id when this arrives. Set it before gptps_dq_recover; NULL clears it. */
typedef void (*gptps_dq_resubmit_cb)(const char *task_name, const void *payload, size_t len,
                                     gptps_handle handle, void *user_data);
gptps_status gptps_dq_set_resubmit_cb(gptps_dq *dq, gptps_dq_resubmit_cb cb, void *user_data);

/* Retract a durable submit: close the record for `h` so no later gptps_dq_recover
 * re-submits it, then gptps_cancel(e, h) the execution. `h` is an engine handle
 * from THIS run: one gptps_dq_submit returned, or one the engine gave a record
 * gptps_dq_recover re-submitted (gptps_dq_recover returns only a count, so that
 * handle is known only from the engine's events). The retraction is fsync'd
 * before the engine is told, so it survives a crash; if it cannot be made durable
 * the call returns GPTPS_E_IO, leaving the record open and the execution alone (see
 * "I/O errors").
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
 * silently dropped. Teardown's dead letters (GPTPS_EV_FLAG_SHUTDOWN) stay pending
 * instead. A record that was running at three of the process's deaths is quarantined
 * too (see "Crash loops"). Inspect / recover these out-of-band. */
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
 * matters: until the compaction is durable, the drained records can still be read
 * back - the old journal is still in place, or a power cut can bring it back while its
 * directory is not synced - so a RESTART re-quarantines them and your callback sees
 * them a second time. That is this add-on's at-least-once contract applied to the
 * drain, and it is fine for an idempotent callback and not fine for one that bills,
 * emails, or files a ticket.
 *
 * *out_compact (may be NULL) receives the compaction's status, as gptps_dq_compact
 * returns it, and GPTPS_OK when there was nothing to drain. GPTPS_OK: the drained
 * records are durably out of the journal. Anything else: the queue cannot vouch for
 * that, and a restart may hand them to your callback again. Everything else -
 * including the at-least-once guarantee itself - is unchanged. */
size_t gptps_dq_drain_quarantine_ex(gptps_dq *dq, gptps_dq_quarantine_cb cb,
                                    void *user_data, gptps_status *out_compact);

/* Rewrite the journal to contain only still-pending records, bounding its growth
 * within a long-running process. Returns GPTPS_OK once the rewritten journal is in
 * place and durable; that also repairs a broken queue (see "I/O errors"). GPTPS_E_INVAL
 * for a NULL dq. GPTPS_E_NOMEM or GPTPS_E_IO if the old journal is still in place, as
 * it was. GPTPS_E_IO too if the new one is in place, but its directory could not be
 * synced - a power cut could still bring back the old one, so submits and retractions
 * made after it are acknowledged only once a sync of the directory succeeds - or it
 * could not be reopened for appending, which breaks the queue.
 *
 * Compact when the engine is quiet. A compaction holds the queue from start to end - it
 * rewrites every pending record and fsyncs the result - and the engine's threads wait
 * at their next event until it is done: a pause about as long as writing the journal's
 * pending records once. gptps_dq_drain_quarantine compacts too.
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
