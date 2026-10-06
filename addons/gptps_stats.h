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
 *
 * Measurements (docs/MEASUREMENTS.md): what process jobs actually used - mem.peak,
 * cpu.user_ms, ..., and any name an add-on reports - folded per task type and for
 * the engine, into rows keyed by name AND method, because one name measured two ways
 * is two different numbers. A row keeps count, sum, min, max and the latest value;
 * no individual value is kept, so memory follows the vocabulary, not the job count.
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
    /* (1.7) Measurements not folded into a row: the row's table was full
     * (GPTPS_STATS_MEASURE_ROWS), a name or method was longer than
     * GPTPS_STATS_NAME_MAX - 1, or a name came with a different unit or kind than its
     * row has. Counted, never dropped silently. Read it only if struct_size covers it. */
    uint64_t measures_dropped;
} gptps_stats_counters;

/* ---- measurements -------------------------------------------------------- */
#define GPTPS_STATS_MEASURE_ROWS 32   /* rows per task type, and for the engine */
#define GPTPS_STATS_NAME_MAX     64   /* a name or a method, with its NUL */

typedef struct {
    size_t   struct_size;          /* = sizeof(gptps_stats_measure) */
    uint32_t unit;                 /* gptps_measure_unit, as the name reported it */
    uint32_t kind;                 /* gptps_measure_kind: how values combine */
    uint64_t count;                /* values folded in: attempts, or samples for mem.current */
    uint64_t sum;                  /* saturates at UINT64_MAX */
    uint64_t min, max;
    uint64_t last;                 /* the latest value ... */
    uint64_t last_ms;              /* ... and its event's ts_ms (monotonic) */
} gptps_stats_measure;

/* One row: `task` names a task type, or NULL for the engine as a whole. Both `name`
 * and `method` must match ("mem.peak", "cgroup.job.resident"). GPTPS_E_NOTFOUND if no
 * such value has been folded in since install (or the last reset). */
gptps_status gptps_stats_measure_get(gptps_stats *s, const char *task, const char *name,
                                     const char *method, gptps_stats_measure *out);

/* Enumerate a task type's rows (task NULL: the engine's), in first-seen order. The
 * name and method are copied into the buffers, truncated to fit; any output may be
 * NULL. */
size_t       gptps_stats_measure_count(gptps_stats *s, const char *task);
gptps_status gptps_stats_measure_at(gptps_stats *s, const char *task, size_t index,
                                    char *name_buf, size_t name_cap,
                                    char *method_buf, size_t method_cap,
                                    gptps_stats_measure *out);

/* dst += src, for rows of the SAME name and method from several engines (gptps_pool
 * shards): counts and sums add, min and max take the extreme, and the latest value is
 * the one with the later last_ms. A src with count 0 changes nothing. */
void gptps_stats_measure_merge(gptps_stats_measure *dst, const gptps_stats_measure *src);

/* Fold measurements that did not arrive as this engine's events - a gptps_xport
 * reply's, for one - into `task`'s rows and the engine's, exactly as an event's are.
 * ts_ms orders the latest value (gptps_now_ms(NULL) will do). GPTPS_E_INVAL for a
 * NULL stats, task or array with n > 0; what cannot be folded counts in
 * measures_dropped, as above. */
gptps_status gptps_stats_measure_fold(gptps_stats *s, const char *task,
                                      const gptps_measure *m, size_t n, uint64_t ts_ms);

/* A stats object that no engine feeds: only gptps_stats_measure_fold fills it - in a
 * process that sends work through gptps_xport and keeps no engine of its own. Its
 * counters stay 0. Free it with gptps_stats_close. NULL on allocation failure. */
gptps_stats *gptps_stats_open(void);

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

/* Zero the totals and latencies (engine-wide and per task), and forget every
 * measurement row. The gauges are the live truth and are left alone. */
void gptps_stats_reset(gptps_stats *s);

/* dst += src, for folding shards or task rows into one view. Totals and gauges add;
 * latency sums and sample counts add; maxima take the larger. */
void gptps_stats_merge(gptps_stats_counters *dst, const gptps_stats_counters *src);

#ifdef __cplusplus
}
#endif
#endif /* GPTPS_STATS_H */
