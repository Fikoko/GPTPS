/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_stats.h - counters, gauges and latency on the OBSERVER seam.
 *
 * The core emits events and never aggregates (see "A metrics format" in the README's
 * non-goals). This module is the aggregation that row promises: it attaches one
 * observer to one engine and keeps totals (how many queued / started / finished /
 * failed / retried / dead-lettered / dropped), live gauges (how many are pending in
 * the queue, how many are in flight) and latency (queue wait, and run time per
 * attempt) - for the engine as a whole and per task type.
 *
 * It binds to NO wire format. A snapshot is a plain struct the host reads whenever it
 * wants; exporting it to Prometheus / statsd / OTel / a log line is the host's job, and
 * a ten-line one. That keeps this module from dating the library to whatever is
 * fashionable, which is exactly why the core refused to do it.
 *
 * Scaling: one gptps_stats per engine. With gptps_pool, install one on each
 * gptps_pool_shard(p, i) and fold them with gptps_stats_merge(); the sum is the pool.
 *
 * Contract (the same one every observer add-on here follows - see gptps_await.h):
 *   - Install BEFORE submitting work (observer registration is a setup-time operation
 *     in the core). Work submitted before install still counts in the totals, but its
 *     queue wait is unknown, so it gets no wait sample.
 *   - Close AFTER gptps_shutdown. Shutdown frees the engine and takes the observer
 *     registration with it, so close only frees this object; closing first would
 *     leave a live registration pointing at freed memory.
 *   - Every counter is a monotonically increasing total since install (or the last
 *     gptps_stats_reset); every gauge is the live value. Latencies use the event
 *     timestamps the core stamps (monotonic ms), so they measure the engine, not the
 *     observer.
 *   - Thread-safe: snapshot from any thread while the engine runs.
 */
#ifndef GPTPS_STATS_H
#define GPTPS_STATS_H

#include "gptps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gptps_stats gptps_stats;

typedef struct {
    size_t   struct_size;          /* = sizeof(gptps_stats_counters) */

    /* totals: one increment per event of that kind */
    uint64_t queued;
    uint64_t started;              /* attempts started (a retried item starts again) */
    uint64_t finished;
    uint64_t failed;               /* attempts that failed (per attempt, like the event) */
    uint64_t retried;
    uint64_t dead_lettered;
    uint64_t dropped;
    uint64_t cancelled;            /* FAILED events carrying GPTPS_E_CANCELLED */
    uint64_t terminal;             /* finished + dead_lettered + dropped + cancelled */

    /* gauges: live now */
    uint64_t pending;              /* queued (or retry-parked) and not yet started */
    uint64_t in_flight;            /* started and not yet finished/failed */

    /* latency, in ms, over events whose QUEUED/STARTED this observer saw */
    uint64_t wait_samples;         /* attempts with a known queue time (see wait_ms_sum) */
    uint64_t wait_ms_sum;          /* QUEUED (or RETRIED) -> STARTED. When the STARTED callback
                                    * outran the QUEUED one - event order is not guaranteed across
                                    * threads - the queue time is only BOUNDED, and the sample is
                                    * that lower bound (0 if even the bound inverts). Recovering it
                                    * beats dropping it: the items that lose that race are the ones
                                    * that waited least, so dropping them skews the mean up. */
    uint64_t wait_ms_max;
    uint64_t run_samples;          /* FINISHED/FAILED events with a known start time */
    uint64_t run_ms_sum;           /* STARTED -> FINISHED/FAILED */
    uint64_t run_ms_max;
} gptps_stats_counters;

/* Install on an engine: registers one observer. NULL on allocation failure or if
 * the core refuses the observer. Call BEFORE submitting work. */
gptps_stats *gptps_stats_install(gptps *e);

/* Free. Call AFTER gptps_shutdown (see the contract above). */
void gptps_stats_close(gptps_stats *s);

/* Whole-engine counters. */
gptps_status gptps_stats_total(gptps_stats *s, gptps_stats_counters *out);

/* Counters for one task type by name; GPTPS_E_NOTFOUND if no event for that name
 * has been seen. */
gptps_status gptps_stats_task(gptps_stats *s, const char *task, gptps_stats_counters *out);

/* Enumerate task types seen so far (order is first-seen; stable while attached). */
size_t       gptps_stats_task_count(gptps_stats *s);
gptps_status gptps_stats_task_at(gptps_stats *s, size_t index,
                                 char *name_buf, size_t name_cap,
                                 gptps_stats_counters *out);   /* any of the outputs may be NULL */

/* Zero the totals and latencies (engine-wide and per task). The gauges are the live
 * truth and are left alone. */
void gptps_stats_reset(gptps_stats *s);

/* dst += src, for folding shards or task rows into one view. Totals and gauges add;
 * latency sums and sample counts add; maxima take the larger. */
void gptps_stats_merge(gptps_stats_counters *dst, const gptps_stats_counters *src);

#ifdef __cplusplus
}
#endif
#endif /* GPTPS_STATS_H */
