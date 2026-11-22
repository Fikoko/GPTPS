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
 *   - Thread-safe: snapshot from any thread while the engine runs. Separate getter calls
 *     are each consistent but not with one another; gptps_stats_snapshot reads the
 *     totals, every task row and the health under ONE lock acquisition.
 *
 * Bounded (1.12): everything the event path touches is allocated at install - the task
 * rows (cfg.max_tasks, then an '(other)' row), the handle table (cfg.handles live
 * entries plus GPTPS_STATS_ORPHANS markers), the measurement tables
 * (cfg.measure_tables). The observer never allocates, rehashes or grows anything (unless
 * GPTPS_STATS_GROW_ON_EVENT, below); it counts what does not fit (gptps_stats_health)
 * and WARNs, at most once a minute per condition. The handle table grows only OUTSIDE
 * the event path by default: gptps_stats_reserve,
 * or a reader (total / task / task_at / snapshot / health) that finds it more than half
 * full, up to cfg.handles_max - allocated with the lock released, swapped (rehashed) in
 * under it, the old one freed after. A host that never reads should size it at install.
 * When it did not fit, the untracked WARN and gptps_stats_health give the figure
 * (1.13): handles_peak, the most handles live at once (tracked plus untracked, each
 * refused handle counted once - see the field), and handles_needed, the
 * gptps_stats_reserve argument that would have held them. With the flag on and the table
 * stopped at cfg.handles_max, the WARN names a larger handles_max instead of the flag.
 *
 * Growth on the event path (1.13, opt-in): GPTPS_STATS_GROW_ON_EVENT in cfg.flags lets
 * the observer itself grow the full table, by doubling up to cfg.handles_max, so a burst
 * before the first read is tracked exactly. It TRADES the zero-allocation event path for
 * exact counts: the event that finds the table full unlocks, allocates, relocks,
 * re-checks (another thread may have grown it meanwhile), swaps the bigger table in and
 * frees the old one after unlocking - the readers' protocol, so the lock is never held
 * across calloc / free - and that one event pays a rehash. If the allocation fails it is
 * counted (health.nomem) and WARNed, and the handle stays untracked as without the flag;
 * nothing tracked is lost. Default off.
 *
 * Runtime switch (1.12): gptps_stats_set_enabled(s, 0) makes the observer return before
 * it takes its lock (a flag read, nothing else). Events while off are not counted
 * anywhere - the totals miss them. Turning it back on drops the handle table and zeroes
 * the gauges, because the work in flight across the pause cannot be accounted for: from
 * then on the gauges (and wait / run samples) cover only work queued after the resume,
 * which gptps_stats_health reports (resumes, gauges_since_ms). Both transitions WARN.
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
 * counters stay 0. Free it with gptps_stats_close. NULL on allocation failure. It has
 * the default task rows; its measurement tables are allocated as folds need them. */
gptps_stats *gptps_stats_open(void);

/* Install on an engine: registers one observer. NULL on allocation failure or if
 * the core refuses the observer. Call BEFORE submitting work. Same as
 * gptps_stats_install_ex(e, NULL). */
gptps_stats *gptps_stats_install(gptps *e);

/* ---- 1.12: sizing, the switch, health, bulk snapshot ----------------------- */
#define GPTPS_STATS_ORPHANS     64     /* markers kept for late events (see health.orphans_dropped) */
#define GPTPS_STATS_TASK_NAME   128    /* a task row's name, with its NUL (= GPTPS_TASK_NAME_MAX + 1) */
#define GPTPS_STATS_OTHER_NAME  "(other)"

/* Install-time sizing. Every field 0 means its default; a value out of range makes
 * install return NULL with a WARN (nothing is clamped). struct_size is required: at
 * least sizeof this 1.12 layout; fields appended later are read only when it covers
 * them. */
#define GPTPS_STATS_START_DISABLED 0x1u  /* flags: install with the observer off (see set_enabled) */
#define GPTPS_STATS_GROW_ON_EVENT  0x2u  /* flags (1.13): the observer grows the full handle table
                                          * itself, up to handles_max, allocating with its lock
                                          * released (see "Growth on the event path"). Exact counts
                                          * for a pre-read burst, at the price of an event path that
                                          * may allocate. A 1.12 library refuses this flag (install
                                          * returns NULL with a WARN). */
typedef struct {
    size_t   struct_size;          /* = sizeof(gptps_stats_config) */
    uint32_t max_tasks;            /* task rows (0 => 256; 1..65536). Rows are found by an FNV-1a
                                    * hash of the name - one probe and one compare per lookup,
                                    * whatever the number of types - and a real row keeps its index
                                    * for the life of the object. A type that arrives once every row
                                    * is taken (or with a name longer than GPTPS_TASK_NAME_MAX) counts
                                    * in one '(other)' row, always enumerated LAST, so its index is
                                    * task_count - 1 and moves up while real rows are still being
                                    * added (find it by GPTPS_STATS_ROW_OTHER or by name, not by a
                                    * cached index); the distinct types folded there are counted
                                    * (health.types_folded). About 312 bytes a row (64-bit). */
    uint32_t handles;              /* live handle-table entries at install (0 => 1024; 1..1<<24).
                                    * A slot is 56 bytes, the table a power of two at most 3/4 full. A
                                    * handle that finds every live entry taken is NOT tracked for the
                                    * rest of its life - its gauges and samples are skipped, never
                                    * another's evicted - and counted once (health.handles_untracked);
                                    * the gauges then undercount. Only a read grows the table, so a
                                    * host that submits more than this many items before it first
                                    * reads the stats (a burst at start-up, a MANUAL engine filled
                                    * then stepped) must size it here or with gptps_stats_reserve -
                                    * health.handles_needed says to what - or set
                                    * GPTPS_STATS_GROW_ON_EVENT. Before 1.12 the table grew on the
                                    * event path without bound; with that flag it grows there up to
                                    * handles_max. */
    uint32_t handles_max;          /* how far reads (and GPTPS_STATS_GROW_ON_EVENT) may grow the table
                                    * (0 => max(handles, 1<<20); handles..1<<24; = handles: never grow
                                    * by itself) */
    uint32_t measure_tables;       /* task types that may keep measurement rows (0 => 8; 1..65536),
                                    * GPTPS_STATS_MEASURE_ROWS rows (about 6 KiB) each, allocated at
                                    * install. A type whose first measurement finds none left keeps no
                                    * rows: its values count in measures_dropped and
                                    * health.measure_tables_refused (the engine's rows still fold
                                    * them). gptps_stats_measure_fold, a host call, allocates one
                                    * instead when they run out. */
    uint32_t flags;                /* GPTPS_STATS_START_DISABLED, GPTPS_STATS_GROW_ON_EVENT */
} gptps_stats_config;

/* Install with sizing (cfg NULL: every default). */
gptps_stats *gptps_stats_install_ex(gptps *e, const gptps_stats_config *cfg);

/* The runtime switch (see "Runtime switch" above). on = 0: the observer returns before
 * its lock until switched on again. on != 0 after off: the handle table is dropped and
 * the gauges zeroed; they count work queued from now. GPTPS_E_INVAL for a NULL s. */
gptps_status gptps_stats_set_enabled(gptps_stats *s, int on);
int          gptps_stats_enabled(gptps_stats *s);         /* 1 when counting; 0 when off or NULL */

/* Grow the handle table to hold `handles` live entries, now, with the lock held only
 * for the swap (never shrinks; may exceed cfg.handles_max). GPTPS_E_INVAL for a NULL s
 * or past 1<<24, GPTPS_E_NOMEM (counted in health.nomem) if it cannot be allocated. */
gptps_status gptps_stats_reserve(gptps_stats *s, size_t handles);

/* What the object could not count, and how full it is. Never silent: each loss here
 * is also a WARN (at most once a minute per condition). */
typedef struct {
    size_t   struct_size;          /* = sizeof(gptps_stats_health_info); at least the 1.12 size */
    uint32_t enabled;              /* 1 while the observer counts */
    uint32_t types_folded_more;    /* 1: more distinct types were folded than types_folded counts */
    uint64_t resumes;              /* times switched on again after off */
    uint64_t gauges_since_ms;      /* 0: the gauges cover all work queued since install; else the
                                    * gptps_now_ms of the last resume - work admitted before it is
                                    * not in them */
    uint64_t handles_cap;          /* live entries the handle table holds now */
    uint64_t handles_max;          /* how far reads may grow it (cfg.handles_max) */
    uint64_t handles_live;         /* entries for queued / running / retry-parked handles */
    uint64_t markers;              /* entries kept for a late event (<= GPTPS_STATS_ORPHANS) */
    uint64_t handles_untracked;    /* events of a handle the full table could not take */
    uint64_t orphans_dropped;      /* markers dropped, oldest first, to bound them (work submitted
                                    * before install, a service's later runs, an out-of-order end
                                    * whose QUEUED / RETRIED is still on its way). The last 256
                                    * dropped (and untracked) handles are remembered: if one's late
                                    * QUEUED / RETRIED still comes it bumps the totals only
                                    * (late_absorbed). Past those 256 it is taken for a new handle's
                                    * and leaves pending at +1 and one live entry held until the next
                                    * resume - the residual cost of bounding the markers. */
    uint64_t grows;                /* handle-table growths */
    uint64_t tasks_cap;            /* cfg.max_tasks */
    uint64_t tasks;                /* rows in use, '(other)' not counted */
    uint64_t types_folded;         /* distinct task types counted in '(other)' (up to 192) */
    uint64_t measure_tables_cap;   /* cfg.measure_tables */
    uint64_t measure_tables_used;
    uint64_t measure_tables_refused; /* first measurements of a type that found no table left */
    uint64_t nomem;                /* allocations that failed: a growth (a read's, a reserve's or,
                                    * with GPTPS_STATS_GROW_ON_EVENT, the observer's), a host
                                    * fold's table */
    uint64_t memory_bytes;         /* what this object holds */
    uint64_t late_absorbed;        /* late QUEUED / RETRIED events of a dropped marker or of an
                                    * untracked handle: counted in the totals, no gauge moved */
    /* ---- 1.13 (read only if struct_size covers them) ---- */
    uint64_t handles_peak;         /* the most handles live at once since install: table entries
                                    * plus handles the full table refused that had not ended yet
                                    * (a refused handle's terminal event takes it off). Each
                                    * refused handle counts once, however its events interleave
                                    * (STARTED before QUEUED, retries). Exact while at most 256
                                    * refused handles are live at once; past that, a span handle
                                    * never refused can stand in for an older refused one for a
                                    * while (the count comes back), and a refused STARTED whose
                                    * late QUEUED comes after 256 newer refusals counts twice */
    uint64_t handles_needed;       /* the gptps_stats_reserve(s, n) that would have tracked them all:
                                    * the next power of two >= handles_peak (at most 1<<24) */
    uint64_t grows_on_event;       /* of `grows`, those made by the observer (GPTPS_STATS_GROW_ON_EVENT) */
} gptps_stats_health_info;
gptps_status gptps_stats_health(gptps_stats *s, gptps_stats_health_info *out);

/* One task row of a snapshot. */
#define GPTPS_STATS_ROW_OTHER 0x1u   /* flags: the '(other)' row */
typedef struct {
    size_t               struct_size;   /* written: the bytes of this row filled */
    uint32_t             flags;         /* GPTPS_STATS_ROW_OTHER */
    char                 name[GPTPS_STATS_TASK_NAME];
    gptps_stats_counters c;
} gptps_stats_row;

/* The totals, the health and every task row (first-seen order, then '(other)' if it
 * counted anything), all under ONE lock acquisition, so the rows add up to the totals.
 * Any output may be NULL. rows is an array of `cap` elements of `row_size` bytes each -
 * pass sizeof(gptps_stats_row); a later, larger row is filled as far as row_size
 * covers. *needed (if not NULL) gets the number of rows there are. GPTPS_OK when they
 * all fit; GPTPS_E_FULL when only `cap` were written (totals and health are still
 * written); GPTPS_E_INVAL for a NULL s, rows with cap > 0 and a row_size below the 1.12
 * sizeof(gptps_stats_row), or a health below its 1.12 size. */
gptps_status gptps_stats_snapshot(gptps_stats *s, gptps_stats_counters *total,
                                  gptps_stats_health_info *health,
                                  gptps_stats_row *rows, size_t row_size, size_t cap,
                                  size_t *needed);

/* Free. Call AFTER gptps_shutdown (see the contract above). */
void gptps_stats_close(gptps_stats *s);

/* Whole-engine counters. Every getter fills the caller's struct only as far as its
 * struct_size says (1.12; earlier releases wrote the whole current struct): the 1.2-1.6
 * size gets the 1.2 fields, any other value below the 1.7 size - a caller that never
 * set it - gets the 1.7 fields, which every caller since has room for. The size
 * written is stored back in struct_size. gptps_stats_measure_* do the same with
 * gptps_stats_measure (its floor is its 1.7 size). */
gptps_status gptps_stats_total(gptps_stats *s, gptps_stats_counters *out);

/* Counters for one task type by name; GPTPS_E_NOTFOUND if no event for that name
 * has been seen - and also for a type that was seen but has no row of its own (past
 * cfg.max_tasks, or a name longer than GPTPS_TASK_NAME_MAX): its counts are in the
 * '(other)' row (GPTPS_STATS_OTHER_NAME), and health.types_folded says how many types
 * are there. */
gptps_status gptps_stats_task(gptps_stats *s, const char *task, gptps_stats_counters *out);

/* Enumerate task types seen so far. The real rows are in first-seen order and keep
 * their index while attached. Once anything folds into it, the '(other)' row
 * (GPTPS_STATS_OTHER_NAME) is always LAST: its index is task_count - 1, which moves up
 * each time a real row is added (a long name can fold before max_tasks is reached), so
 * a reader should not cache it. By name it is found only when no real type has that
 * name. */
size_t       gptps_stats_task_count(gptps_stats *s);
gptps_status gptps_stats_task_at(gptps_stats *s, size_t index,
                                 char *name_buf, size_t name_cap,
                                 gptps_stats_counters *out);   /* any of the outputs may be NULL */

/* Zero the totals and latencies (engine-wide and per task), and forget every
 * measurement row. The gauges are the live truth and are left alone, and so are the
 * task rows themselves (names and indexes) and the health counters. */
void gptps_stats_reset(gptps_stats *s);

/* dst += src, for folding shards or task rows into one view. Totals and gauges add;
 * latency sums and sample counts add; maxima take the larger. */
void gptps_stats_merge(gptps_stats_counters *dst, const gptps_stats_counters *src);

#ifdef __cplusplus
}
#endif
#endif /* GPTPS_STATS_H */
