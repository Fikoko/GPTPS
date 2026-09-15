/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_balance.h - load-balanced routing above gptps_pool, by LATE BINDING.
 *
 * gptps_pool routes each submit to a shard as it arrives (round-robin or by key).
 * That is the right default for uniform work and the wrong one for mixed sizes: one
 * shard can end up holding three long items while its neighbours idle, and nothing
 * can move them, because an item inside an engine's queue belongs to that engine.
 *
 * This module keeps the queue where it can be re-routed: in the router. Work waits
 * HERE, in one priority queue, and each shard is handed only what it can run right
 * now plus a small prefetch depth. When a shard finishes something (observer seam),
 * the next item goes to whichever shard has the least outstanding. That is
 * join-shortest-queue with late binding - in effect work-stealing, since idle shards
 * pull and busy ones stop winning the comparison - and it adapts to any task size
 * without knowing sizes in advance: a shard running one long item simply stops being
 * the shortest queue.
 *
 * Why not steal from the engine? The public API deliberately has no "give me back a
 * queued item", and it should not: an engine's queue is its admission ledger. The
 * queue you want to steal from has to be yours. This one is. No core change.
 *
 * The contract (the same shape as pool):
 *
 *   p = gptps_pool_open(4, &cfg); gptps_pool_register_task(p, &def);
 *   b = gptps_balance_open(p, NULL);              // BEFORE submitting anything
 *   gptps_balance_set_event_cb(b, on_event, ud);  // events carry BALANCE handles
 *   gptps_balance_submit(b, "resize", buf, len, &h);
 *   ... gptps_pool_close(p); gptps_balance_close(b);   // close AFTER the pool
 *
 * Events: the host's callback sees the engine's lifecycle events for balanced work
 * with `handle` rewritten to the balance handle - plus a QUEUED emitted by this
 * module at submit time (the item queued HERE), and a terminal DEAD_LETTERED /
 * DROPPED for an item this module could not dispatch (unknown task, shard refused
 * the submit, close with work still queued). Every balance handle reaches exactly
 * one terminal event, the same guarantee the core gives. Work submitted straight to
 * a shard (gptps_pool_submit, gptps_submit) is not seen here and not counted.
 *
 * What it costs: one lock and one hash lookup per event, and a copy of the payload
 * while the item waits in the router (the engine copies again at dispatch).
 *
 * What it is not: not cross-process (that is xport's job; xport in engine mode is a
 * fine thing to put behind a shard) and not a scheduler for the shards' own queues -
 * the engine's admission order still applies to what has been handed over, which is
 * why shard_depth exists: deep enough that skip-to-fit and the starvation guard have
 * something to work with, shallow enough that a late-arriving long item cannot bury
 * a shard. THREADED shards only (dispatch is driven from their
 * dispatcher threads).
 */
#ifndef GPTPS_BALANCE_H
#define GPTPS_BALANCE_H

#include "gptps.h"
#include "gptps_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gptps_balance gptps_balance;
typedef uint64_t gptps_balance_handle;

typedef struct {
    size_t   struct_size;      /* = sizeof(gptps_balance_config) */
    uint32_t shard_depth;      /* how many items a shard may hold at once (running +
                                * waiting in ITS queue). A shard is offered work while its
                                * outstanding count is below this. 0 => 2x that shard's
                                * limits.max_concurrent_tasks. Set it to exactly
                                * max_concurrent_tasks for no prefetch at all. */
} gptps_balance_config;

/* Open above a pool. Registers one observer on every shard and reads each shard's
 * limits.max_concurrent_tasks. Call BEFORE submitting work to the pool (observer
 * registration is a setup-time operation in the core). NULL on failure. */
gptps_balance *gptps_balance_open(gptps_pool *p, const gptps_balance_config *cfg);

/* Lifecycle events for balanced work, handles rewritten (see header). Optional. */
gptps_status gptps_balance_set_event_cb(gptps_balance *b, gptps_event_cb cb, void *user_data);

/* Queue work. GPTPS_E_NOTFOUND if the task is not registered on the pool. */
gptps_status gptps_balance_submit(gptps_balance *b, const char *task,
                                  const void *payload, size_t len, gptps_balance_handle *out);

/* Same, with a priority (higher dispatches first; equal => FIFO). The priority is
 * also passed to the shard on dispatch, so the engine's ordering agrees. */
gptps_status gptps_balance_submit_ex(gptps_balance *b, const char *task,
                                     const void *payload, size_t len, int32_t priority,
                                     gptps_balance_handle *out);

/* Cancel. Still queued here => removed, one terminal FAILED/E_CANCELLED event.
 * Already dispatched => forwarded to the shard (gptps_cancel). */
gptps_status gptps_balance_cancel(gptps_balance *b, gptps_balance_handle h);

/* Items waiting in the router (not yet handed to any shard). */
size_t gptps_balance_queued(gptps_balance *b);

/* Items handed to shard i and not yet terminal. */
size_t gptps_balance_shard_load(gptps_balance *b, size_t i);

/* Free. Call AFTER gptps_pool_close: shutting the shards down takes the observer
 * registrations with it (see gptps_await.h for the reasoning). Anything still
 * queued here gets its terminal DROPPED / E_SHUTDOWN event, then everything is freed. */
void gptps_balance_close(gptps_balance *b);

#ifdef __cplusplus
}
#endif
#endif /* GPTPS_BALANCE_H */
