/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_stats.c - counters, gauges and latency on the observer seam.
 *
 * Structure: one observer, one lock. Per event: a flag read (the runtime switch), lock,
 * one hash lookup on the handle (for the gauges and latencies), a hashed task-row lookup
 * on a handle's first event, two counter bumps (engine row + task row), unlock. The
 * event path allocates nothing: the task rows, their name index, the handle table and
 * the measurement tables are all made at install (1.12), and what does not fit is
 * counted, never grown into. The handle table grows only outside the event path (see
 * "growth" below) - unless the host opts in to GPTPS_STATS_GROW_ON_EVENT (1.13), which
 * lets the observer grow it, allocating with the lock released.
 *
 * Accounting, per handle, driven by the core's event contract (a ONE-SHOT handle
 * reaches exactly one terminal event; FAILED is per attempt):
 *
 *   QUEUED         -> PENDING            pending++
 *   STARTED        -> RUNNING            pending--  in_flight++   wait sample
 *   FINISHED       -> terminal           in_flight--              run sample
 *   FAILED         -> LIMBO              in_flight-- (or pending-- if it never
 *                                        started: cancelled in queue)  run sample
 *                     ...unless status == CANCELLED, which is terminal
 *   RETRIED        -> PENDING            pending++   (re-stamps the queue time)
 *                     ...unless the attempt it announces already started or ended:
 *                     a LATE RETRIED moves nothing (see "Event ORDER" below)
 *   DEAD_LETTERED  -> terminal           pending-- if still PENDING (denied /
 *   DROPPED                              shutdown-terminated in queue), else none
 *
 * An event for a handle this observer never saw QUEUED (work submitted before install)
 * bumps the totals only: with no state to move, adjusting a gauge would be a guess.
 *
 * The two handle shapes outside the one-terminal rule are safe here, but they make the
 * TOTALS mean something different from the gauges, which is worth knowing before you
 * graph them. A GPTPS_TASK_SERVICE handle emits a terminal event per RUN, so `finished`
 * counts service runs rather than service instances, and one long-lived instance can
 * dominate a totals column while `in_flight` correctly shows one. A
 * GPTPS_ON_FAILURE_REQUEUE item emits a per-attempt FAILED each time round and no
 * terminal event until shutdown dead-letters it, so it sits in LIMBO between attempts
 * and its `failed` count grows without a matching terminal. Neither corrupts a gauge -
 * every transition above is still driven by the state the handle is actually in - but
 * `queued == terminal` is an identity for one-shot work only.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "gptps_stats.h"
#include "addon_compat.h"   /* portable mutex */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The one word the event path reads without the lock (the runtime switch): the
 * compiler's atomic builtins where there are some - not the HAL, so the drop-in build
 * has them too - else a volatile access (MSVC gives volatile acquire/release semantics
 * on its targets). One writer at a time (under the lock), a single word: a stale read
 * costs one event counted or skipped at the instant the switch flips, and the event
 * path re-reads it under the lock before it counts anything. */
#if defined(__GNUC__) || defined(__clang__)
#  define ST_FLAG_GET(p)    __atomic_load_n((p), __ATOMIC_ACQUIRE)
#  define ST_FLAG_SET(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#else
#  define ST_FLAG_GET(p)    (*(p))
#  define ST_FLAG_SET(p, v) (*(p) = (v))
#endif

/* ---- sizing (gptps_stats_config) ---- */
#define ST_DEF_TASKS    256u
#define ST_MAX_TASKS    65536u
#define ST_DEF_HANDLES  1024u
#define ST_MAX_HANDLES  (1u << 24)
#define ST_DEF_HMAX     (1u << 20)
#define ST_DEF_MTABS    8u
#define ST_MAX_MTABS    65536u
#define ST_ORPHANS      GPTPS_STATS_ORPHANS
/* Task types folded into '(other)' are counted distinct, by name hash, up to this many. */
#define ST_FOLD_SLOTS   256u
#define ST_FOLD_MAX     192u
/* A WARN condition speaks at most once in this long, on ONE clock: gptps_now_ms, read
 * only when a condition fires. Never an event's or a host's ts_ms - a host may fold with
 * a wall-clock stamp, and two threads' stamps invert - so mixed clocks cannot make a
 * condition speak more often. (A test may supply its own clock.) */
#define ST_WARN_MS      60000u
#ifndef ST_NOW_MS
#  define ST_NOW_MS() gptps_now_ms(NULL)
#endif
/* Late QUEUED / RETRIED events to absorb (see "the absorb set"): the last ST_ABSORB handles
 * whose marker was dropped or whose events the full table did not track. */
#define ST_ABSORB       256u
#define ST_ABSORB_SLOTS 512u

enum { ST_NONE = 0, ST_PENDING, ST_RUNNING, ST_LIMBO, ST_DONE };
enum { W_UNTRACKED = 0, W_FOLD, W_ORPHAN, W_NOMEM, W_MEASURE, W_NEEDED, W_N };

typedef struct {
    gptps_handle handle;     /* 0 = empty slot */
    uint64_t     queued_ms;  /* last QUEUED / RETRIED */
    uint64_t     started_ms; /* last STARTED */
    uint64_t     q_started_ms; /* owe_q: the wait is measured from here */
    uint32_t     task;       /* row index (s->rows; max_tasks is '(other)') */
    /* Retry bookkeeping, so a RETRIED is judged by the attempt it announces rather
     * than by when it arrives (see "Event ORDER" above stats_observe). */
    uint32_t     att;        /* highest attempt started or ended in this cycle; an
                              * attempt-1 STARTED (a REQUEUE) resets it */
    uint32_t     ann;        /* highest attempt a RETRIED has announced, this cycle */
    uint32_t     owe_r;      /* attempt whose STARTED outran its RETRIED: wait owed */
    uint8_t      state;
    uint8_t      seen_queued;/* this observer saw the handle's QUEUED */
    uint8_t      have_queued_ms, have_started_ms;
    uint8_t      owe_q;      /* attempt 1's STARTED outran QUEUED: wait owed */
} stats_slot;

/* An event of attempt N > 1 has arrived and the RETRIED announcing attempt N has not:
 * it is still on its way, so the slot must outlive a terminal event to meet it. */
#define OWES_RETRIED(sl) ((sl)->att > 1 && (sl)->ann < (sl)->att)

/* One measurement row (docs/MEASUREMENTS.md): a name and a method, and what was
 * folded into it. A task type takes a table of them (from the pool made at install)
 * with the first measurement it reports, so an in-process task never holds one. */
typedef struct {
    char                name[GPTPS_STATS_NAME_MAX];
    char                method[GPTPS_STATS_NAME_MAX];
    gptps_stats_measure m;
} stats_mrow;

typedef struct {
    stats_mrow *rows;              /* GPTPS_STATS_MEASURE_ROWS of them, or NULL */
    size_t      n;
    int         owned;             /* rows is its own allocation (not a pool slice) */
    int         refused;           /* its first measurement found the pool spent (counted once) */
} stats_mtab;

typedef struct {
    char                 name[GPTPS_STATS_TASK_NAME];
    uint64_t             hash;
    gptps_stats_counters c;
    stats_mtab           mt;
} stats_task;

struct gptps_stats {
    gptps                *e;
    apx_mutex             mu;
    volatile int          on;      /* the runtime switch: read by the observer WITHOUT mu */
    gptps_stats_counters  total;
    stats_mtab            mtotal;  /* the engine's measurement rows */
    /* task rows: rows[0..ntasks) in first-seen order, rows[max_tasks] is '(other)' */
    stats_task           *rows;
    uint32_t              ntasks, max_tasks;
    int                   other_used;
    int32_t              *tidx;    /* name index: row ids by FNV-1a, -1 empty, <= half full */
    uint32_t              tidx_mask;
    uint64_t              fold[ST_FOLD_SLOTS];   /* name hashes of the folded types, 0 empty */
    uint32_t              nfold;
    int                   fold_more;
    /* measurement-table pool */
    stats_mrow           *mpool;
    uint32_t              mpool_cap, mpool_used;
    /* handle table: open addressing, power-of-two, keyed by handle */
    stats_slot           *tab;
    size_t                tabcap;      /* slots */
    size_t                live_cap;    /* live entries it takes (markers come on top) */
    size_t                tabn, ndone; /* entries; of them markers (ST_DONE) */
    size_t                handles_max;
    gptps_handle          orph[ST_ORPHANS];   /* the newest markers, oldest first */
    unsigned              orph_head, orph_n;
    /* handles whose late QUEUED / RETRIED must move no gauge: a ring (eviction order)
     * over a set (lookup), both fixed */
    gptps_handle          ab_ring[ST_ABSORB];
    unsigned              ab_head, ab_n;
    gptps_handle          ab_set[ST_ABSORB_SLOTS];
    /* the span of handles the full table refused: their later events stay untracked */
    gptps_handle          rf_lo, rf_hi;
    /* (1.13) sizing figures: handles refused and not yet ended (a refused handle's
     * terminal event takes one off), and the peak of live entries plus those - the
     * table a burst needed. GPTPS_STATS_GROW_ON_EVENT: the observer grows the table. */
    uint64_t              untracked_live, peak;
    int                   grow_ev;
    /* health */
    uint64_t              untracked, untracked_seen, orphans_dropped, grows, nomem, grows_event;
    uint64_t              mtab_refused, resumes, resumed_ms, late_absorbed;
    /* WARN rate limit: when each condition last spoke */
    uint64_t              w_last[W_N];
    uint8_t               w_any[W_N];
};

/* WARNs are composed under the lock and logged after it is dropped. */
#define ST_WARN_LEN 512
typedef struct { int n; char m[3][ST_WARN_LEN]; } st_warns;

static int st_warn_due(gptps_stats *s, int k)
{
    uint64_t now = ST_NOW_MS();
    if (s->w_any[k] && now >= s->w_last[k] && now - s->w_last[k] < ST_WARN_MS) return 0;
    s->w_any[k] = 1; s->w_last[k] = now;
    return 1;
}
static char *st_warn_slot(st_warns *w)
{
    if (!w || w->n >= 3) return NULL;
    return w->m[w->n++];
}
static void st_warn_flush(const st_warns *w)
{
    int i;
    for (i = 0; i < w->n; ++i) gptps_log(NULL, GPTPS_LOG_WARN, w->m[i]);
}

static size_t st_pow2(size_t n) { size_t p = 1; while (p < n) p <<= 1; return p; }

/* FNV-1a of a task name (the name index and the fold set). */
static uint64_t st_name_hash(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}

/* ---- handle table (handles are never 0: the core starts at 1) ----
 * At most live_cap live entries and GPTPS_STATS_ORPHANS markers, in a table sized so it
 * is never more than 3/4 full; nothing on the event path ever grows it. */
static size_t st_slots_for(size_t live) { return st_pow2((live + ST_ORPHANS + 1) * 4 / 3 + 1); }

static size_t slot_hash(gptps_handle h, size_t cap)
{
    uint64_t x = (uint64_t)h;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;   /* murmur3 finalizer */
    return (size_t)x & (cap - 1);
}
static stats_slot *tab_find(gptps_stats *s, gptps_handle h)
{
    size_t i, cap = s->tabcap;
    if (!cap || !h) return NULL;
    for (i = slot_hash(h, cap); ; i = (i + 1) & (cap - 1)) {
        if (s->tab[i].handle == h) return &s->tab[i];
        if (s->tab[i].handle == 0) return NULL;
    }
}
/* Move a slot to `st`, keeping the marker count true. */
static void st_state(gptps_stats *s, stats_slot *sl, uint8_t st)
{
    if (sl->state == ST_DONE) s->ndone--;
    if (st == ST_DONE) s->ndone++;
    sl->state = st;
}
/* Tombstone-free deletion (backward shift) so lookups stay bounded. Moves entries: any
 * slot pointer taken before this call is stale after it. */
static void tab_del(gptps_stats *s, stats_slot *slot)
{
    size_t cap = s->tabcap, i = (size_t)(slot - s->tab), j = i;
    if (slot->state == ST_DONE) s->ndone--;
    for (;;) {
        size_t k;
        j = (j + 1) & (cap - 1);
        if (s->tab[j].handle == 0) break;
        k = slot_hash(s->tab[j].handle, cap);
        /* can the entry at j move to i? yes iff its home k is not in (i, j] cyclically */
        if ((i <= j) ? (k <= i || k > j) : (k <= i && k > j)) { s->tab[i] = s->tab[j]; i = j; }
    }
    memset(&s->tab[i], 0, sizeof s->tab[i]);
    s->tabn -= 1;
}
/* A new entry for h (not in the table). NULL only if the table is full, which the
 * bounds above rule out - defensive. */
static stats_slot *tab_insert(gptps_stats *s, gptps_handle h)
{
    size_t i;
    if (!s->tabcap || (s->tabn + 1) * 4 > s->tabcap * 3) return NULL;
    for (i = slot_hash(h, s->tabcap); s->tab[i].handle; i = (i + 1) & (s->tabcap - 1)) { }
    memset(&s->tab[i], 0, sizeof s->tab[i]);
    s->tab[i].handle = h;
    s->tabn += 1;
    return &s->tab[i];
}
static size_t st_live(const gptps_stats *s) { return s->tabn - s->ndone; }

/* ---- the absorb set: handles whose late QUEUED / RETRIED moves no gauge ----
 * A marker dropped from the FIFO below, or a handle whose events the full table did not
 * track, may still get the late QUEUED (or RETRIED) it was owed. With no slot, that
 * event would open a PENDING entry nothing ever closes: pending stuck at +1 and a live
 * entry lost for good. So the last ST_ABSORB such handles are remembered (fixed arrays,
 * nothing allocated) and their late QUEUED / RETRIED bumps the totals only, counted in
 * health.late_absorbed. A handle that ages out of the ring before its late event comes
 * is not caught: that is the residual case the header documents. */
static int st_ab_has(const gptps_stats *s, gptps_handle h)
{
    size_t i;
    for (i = slot_hash(h, ST_ABSORB_SLOTS); s->ab_set[i]; i = (i + 1) & (ST_ABSORB_SLOTS - 1))
        if (s->ab_set[i] == h) return 1;
    return 0;
}
static void st_ab_del(gptps_stats *s, gptps_handle h)
{
    size_t i, j;
    for (i = slot_hash(h, ST_ABSORB_SLOTS); s->ab_set[i] != h; i = (i + 1) & (ST_ABSORB_SLOTS - 1))
        if (!s->ab_set[i]) return;
    for (j = i;;) {                               /* backward shift, as tab_del */
        size_t k;
        j = (j + 1) & (ST_ABSORB_SLOTS - 1);
        if (!s->ab_set[j]) break;
        k = slot_hash(s->ab_set[j], ST_ABSORB_SLOTS);
        if ((i <= j) ? (k <= i || k > j) : (k <= i && k > j)) { s->ab_set[i] = s->ab_set[j]; i = j; }
    }
    s->ab_set[i] = 0;
}
static void st_ab_add(gptps_stats *s, gptps_handle h)
{
    size_t i;
    if (!h || st_ab_has(s, h)) return;
    if (s->ab_n == ST_ABSORB) {                   /* the oldest ages out */
        st_ab_del(s, s->ab_ring[s->ab_head]);
        s->ab_ring[s->ab_head] = h;
        s->ab_head = (s->ab_head + 1) % ST_ABSORB;
    } else {
        s->ab_ring[(s->ab_head + s->ab_n) % ST_ABSORB] = h;
        s->ab_n++;
    }
    for (i = slot_hash(h, ST_ABSORB_SLOTS); s->ab_set[i]; i = (i + 1) & (ST_ABSORB_SLOTS - 1)) { }
    s->ab_set[i] = h;
}
/* In the span of handles the full table refused (see st_untracked). */
static int st_refused(const gptps_stats *s, gptps_handle h)
{
    return s->rf_hi && h >= s->rf_lo && h <= s->rf_hi;
}

/* Remember a new marker; drop the oldest when the FIFO is full (it may delete a slot,
 * which moves entries: no slot pointer survives this call). */
static void st_orphan(gptps_stats *s, gptps_handle h, st_warns *w)
{
    if (s->orph_n == ST_ORPHANS) {
        gptps_handle old = s->orph[s->orph_head];
        stats_slot *o = old != h ? tab_find(s, old) : NULL;
        if (o && o->state == ST_DONE) {
            char *m;
            tab_del(s, o);
            s->orphans_dropped++;
            st_ab_add(s, old);                   /* its late QUEUED / RETRIED moves no gauge */
            if (st_warn_due(s, W_ORPHAN) && (m = st_warn_slot(w)) != NULL)
                snprintf(m, ST_WARN_LEN, "gptps_stats: %llu handle marker(s) dropped so far, oldest first: they waited for a "
                         "late event that never came (work submitted before install, a service's later runs); "
                         "at most %d are kept (gptps_stats_health.orphans_dropped)",
                         (unsigned long long)s->orphans_dropped, ST_ORPHANS);
        }
        s->orph[s->orph_head] = h;
        s->orph_head = (s->orph_head + 1) % ST_ORPHANS;
    } else {
        s->orph[(s->orph_head + s->orph_n) % ST_ORPHANS] = h;
        s->orph_n++;
    }
}

/* ---- growth -----------------------------------------------------------------------
 * The 1.11 dashboard's storage protocol: the new table is allocated with the lock
 * RELEASED, the entries are moved and the pointer swapped under the lock, and the old
 * table is freed after it is dropped. Never on the event path: gptps_stats_reserve, and
 * readers that find the table more than half full (or a handle refused since the last
 * look), up to handles_max. */
/* mu held: a failed growth, counted and WARNed. */
static void st_grow_nomem(gptps_stats *s, size_t live_cap, st_warns *w)
{
    char *m;
    s->nomem++;
    if (st_warn_due(s, W_NOMEM) && (m = st_warn_slot(w)) != NULL)
        snprintf(m, ST_WARN_LEN, "gptps_stats: no memory to grow the handle table to %lu entries; it stays at %lu, and "
                 "handles past it are not tracked (gptps_stats_health.nomem = %llu)",
                 (unsigned long)live_cap, (unsigned long)s->live_cap, (unsigned long long)s->nomem);
}
/* mu held: rehash every entry into nt (ncap zeroed slots) and swap it in. Returns the
 * old table, for the caller to free once the lock is dropped. */
static stats_slot *st_swap_in(gptps_stats *s, stats_slot *nt, size_t ncap, size_t live_cap)
{
    stats_slot *old;
    size_t i;
    for (i = 0; i < s->tabcap; ++i) {
        const stats_slot *o = &s->tab[i];
        if (o->handle) {
            size_t j;
            for (j = slot_hash(o->handle, ncap); nt[j].handle; j = (j + 1) & (ncap - 1)) { }
            nt[j] = *o;
        }
    }
    old = s->tab; s->tab = nt; s->tabcap = ncap; s->live_cap = live_cap; s->grows++;
    return old;
}
static gptps_status st_grow(gptps_stats *s, size_t live_cap)
{
    size_t ncap = st_slots_for(live_cap);
    stats_slot *nt = (stats_slot *)calloc(ncap, sizeof *nt), *old;
    st_warns w; w.n = 0;
    apx_mutex_lock(&s->mu);
    if (!nt) {
        st_grow_nomem(s, live_cap, &w);
        apx_mutex_unlock(&s->mu);
        st_warn_flush(&w);
        return GPTPS_E_NOMEM;
    }
    if (s->live_cap >= live_cap) { apx_mutex_unlock(&s->mu); free(nt); return GPTPS_OK; }   /* another grew it */
    old = st_swap_in(s, nt, ncap, live_cap);
    apx_mutex_unlock(&s->mu);
    free(old);
    return GPTPS_OK;
}
/* (1.13) The gptps_stats_reserve figure that would have tracked every handle: the
 * next power of two at or above the peak of live handles (tracked plus untracked), at
 * most 1<<24. */
static uint64_t st_needed(const gptps_stats *s)
{
    size_t p = st_pow2(s->peak ? (size_t)s->peak : 1);
    return p > ST_MAX_HANDLES ? ST_MAX_HANDLES : p;
}
/* mu held: note the live handles now (tracked entries plus refused ones not yet over). */
static void st_peak(gptps_stats *s)
{
    uint64_t d = (uint64_t)st_live(s) + s->untracked_live;
    if (d > s->peak) s->peak = d;
}
/* mu held, by a reader: the live capacity to grow to now, or 0. A reader that finds
 * handles refused since the last look also says, once a minute, the exact figure the
 * burst needed - known only now, after it (the event path's WARN spoke at its start,
 * when the burst's size was not known yet). */
static size_t st_grow_want(gptps_stats *s, st_warns *w)
{
    size_t want;
    int refused = s->untracked != s->untracked_seen;
    char *m;
    s->untracked_seen = s->untracked;
    if (refused && st_warn_due(s, W_NEEDED) && (m = st_warn_slot(w)) != NULL)
        snprintf(m, ST_WARN_LEN, "gptps_stats: handles went untracked: a burst of %llu live handles past a table of %lu: "
                 "gptps_stats_reserve(s, %llu) (the next power of two >= the peak live handles) or "
                 "GPTPS_STATS_GROW_ON_EVENT; %llu event(s) not tracked so far, the pending / in-flight gauges "
                 "undercount (gptps_stats_health.handles_needed)",
                 (unsigned long long)s->peak, (unsigned long)s->live_cap, (unsigned long long)st_needed(s),
                 (unsigned long long)s->untracked);
    if (s->live_cap >= s->handles_max) return 0;
    if (!refused && st_live(s) * 2 <= s->live_cap) return 0;
    want = s->live_cap * 2;
    return want > s->handles_max ? s->handles_max : want;
}
/* After a reader drops the lock: its WARN, then the growth it asked for. */
static void st_grow_after(gptps_stats *s, size_t want, const st_warns *w)
{
    st_warn_flush(w);
    if (want) (void)st_grow(s, want);
}

/* ---- task rows ---- */
static stats_task *st_row(gptps_stats *s, uint32_t ti) { return &s->rows[ti]; }

/* A type with no row: note it in the fold set. 1 if it is a new one. */
static int st_fold_note(gptps_stats *s, uint64_t h)
{
    uint32_t i;
    if (!h) h = 1;                            /* 0 marks an empty slot */
    for (i = (uint32_t)h & (ST_FOLD_SLOTS - 1); s->fold[i]; i = (i + 1) & (ST_FOLD_SLOTS - 1))
        if (s->fold[i] == h) return 0;
    if (s->nfold >= ST_FOLD_MAX) { s->fold_more = 1; return 0; }
    s->fold[i] = h; ++s->nfold;
    return 1;
}

/* The row of `name`, -1 if it has none. Caller holds mu. */
static int64_t st_row_lookup(const gptps_stats *s, const char *name, uint64_t h)
{
    uint32_t i;
    for (i = (uint32_t)h & s->tidx_mask; ; i = (i + 1) & s->tidx_mask) {
        int32_t r = s->tidx[i];
        if (r < 0) return -1;
        if (s->rows[r].hash == h && strcmp(s->rows[r].name, name) == 0) return r;
    }
}

/* The row an event of task type `name` counts in: its own, made on first sight, or
 * '(other)' once every row is taken. Never fails, never allocates. */
static uint32_t task_index(gptps_stats *s, const char *name, st_warns *w)
{
    uint64_t h;
    int64_t r;
    size_t n;
    uint32_t i;
    if (!name) name = "";
    h = st_name_hash(name);
    if ((r = st_row_lookup(s, name, h)) >= 0) return (uint32_t)r;
    n = strlen(name);
    if (s->ntasks < s->max_tasks && n < GPTPS_STATS_TASK_NAME) {
        stats_task *k = &s->rows[s->ntasks];
        for (i = (uint32_t)h & s->tidx_mask; s->tidx[i] >= 0; i = (i + 1) & s->tidx_mask) { }
        memset(k, 0, sizeof *k);
        memcpy(k->name, name, n + 1);
        k->hash = h;
        k->c.struct_size = sizeof k->c;
        s->tidx[i] = (int32_t)s->ntasks;
        return s->ntasks++;
    }
    s->other_used = 1;
    if (st_fold_note(s, h)) {
        char *m;
        if (st_warn_due(s, W_FOLD) && (m = st_warn_slot(w)) != NULL)
            snprintf(m, ST_WARN_LEN, "gptps_stats: task type '%.64s' has no row of its own (%s); it counts in the '%s' row "
                     "(%u type(s) folded there so far%s; cfg.max_tasks = %u)",
                     name, n >= GPTPS_STATS_TASK_NAME ? "its name is too long" : "every row is taken",
                     GPTPS_STATS_OTHER_NAME, (unsigned)s->nfold, s->fold_more ? " or more" : "",
                     (unsigned)s->max_tasks);
    }
    return s->max_tasks;
}

/* The row of `task` by name for a reader: a real row, else '(other)' by its name when
 * it counted something and no real type has that name. NULL if none. */
static stats_task *st_row_find(gptps_stats *s, const char *task)
{
    int64_t r = st_row_lookup(s, task, st_name_hash(task));
    if (r >= 0) return &s->rows[r];
    if (s->other_used && strcmp(task, GPTPS_STATS_OTHER_NAME) == 0) return &s->rows[s->max_tasks];
    return NULL;
}
/* Rows a reader enumerates: the real ones, then '(other)' once used. */
static size_t st_nrows(const gptps_stats *s) { return (size_t)s->ntasks + (s->other_used ? 1u : 0u); }
static stats_task *st_row_at(gptps_stats *s, size_t i)
{
    if (i < s->ntasks) return &s->rows[i];
    if (i == s->ntasks && s->other_used) return &s->rows[s->max_tasks];
    return NULL;
}

/* ---- struct_size: copy what the caller has room for ---- */
/* gptps_stats_counters: the 1.2 layout ended before measures_dropped (1.7). */
#define ST_CNT_V1     (offsetof(gptps_stats_counters, measures_dropped))
#define ST_CNT_FLOOR  (offsetof(gptps_stats_counters, measures_dropped) + sizeof(uint64_t))
static size_t st_cnt_size(size_t asked)
{
    if (asked == ST_CNT_V1) return ST_CNT_V1;
    if (asked < ST_CNT_FLOOR) return ST_CNT_FLOOR;        /* never set: every caller since 1.7 has this */
    return asked < sizeof(gptps_stats_counters) ? asked : sizeof(gptps_stats_counters);
}
static void st_cnt_out(gptps_stats_counters *out, const gptps_stats_counters *src)
{
    size_t n = st_cnt_size(out->struct_size);
    memcpy(out, src, n);
    out->struct_size = n;
}
/* gptps_stats_measure: one layout so far (1.7), which is its floor. */
#define ST_MEAS_FLOOR (offsetof(gptps_stats_measure, last_ms) + sizeof(uint64_t))
static void st_meas_out(gptps_stats_measure *out, const gptps_stats_measure *src)
{
    size_t n = out->struct_size;
    if (n < ST_MEAS_FLOOR) n = ST_MEAS_FLOOR;
    if (n > sizeof *out) n = sizeof *out;
    memcpy(out, src, n);
    out->struct_size = n;
}
/* The 1.12 layouts: new structs, so their callers must say their size. */
#define ST_HEALTH_MIN (offsetof(gptps_stats_health_info, late_absorbed) + sizeof(uint64_t))
#define ST_ROW_MIN    (sizeof(gptps_stats_row))
#define ST_CFG_MIN    (offsetof(gptps_stats_config, flags) + sizeof(uint32_t))

/* ---- measurement rows ---- */
static void mrow_fold(gptps_stats_measure *m, uint64_t v, uint64_t ts)
{
    if (m->count == 0 || v < m->min) m->min = v;
    if (m->count == 0 || v > m->max) m->max = v;
    m->sum = (m->sum > UINT64_MAX - v) ? UINT64_MAX : m->sum + v;
    m->count += 1;
    if (m->count == 1 || ts >= m->last_ms) { m->last = v; m->last_ms = ts; }
}

/* A table for a task type's first measurement: a slice of the pool, or 0 if the pool
 * is spent (counted; the caller counts the value as dropped). */
static int st_mtab_take(gptps_stats *s, stats_mtab *mt, st_warns *w)
{
    char *m;
    if (mt->rows) return 1;
    if (s->mpool_used < s->mpool_cap) {
        mt->rows = &s->mpool[(size_t)s->mpool_used++ * GPTPS_STATS_MEASURE_ROWS];
        mt->n = 0; mt->owned = 0;
        return 1;
    }
    if (mt->refused) return 0;                  /* counted once per task type */
    mt->refused = 1;
    s->mtab_refused++;
    if (st_warn_due(s, W_MEASURE) && (m = st_warn_slot(w)) != NULL)
        snprintf(m, ST_WARN_LEN, "gptps_stats: every measurement table is taken (cfg.measure_tables = %u): a task type's "
                 "measurements are dropped and counted in measures_dropped (%llu type(s) refused so far)",
                 (unsigned)s->mpool_cap, (unsigned long long)s->mtab_refused);
    return 0;
}

/* Fold one value into a table: its row by name and method, made if there is room.
 * 0 if it could not be folded (the caller counts it as dropped). */
static int mtab_fold(stats_mtab *mt, const gptps_measure *x, uint64_t ts)
{
    size_t i;
    stats_mrow *r;
    for (i = 0; i < mt->n; ++i) {
        r = &mt->rows[i];
        if (strcmp(r->name, x->name) == 0 && strcmp(r->method, x->method) == 0) {
            if (r->m.unit != x->unit || r->m.kind != x->kind) return 0;   /* never mix units */
            mrow_fold(&r->m, x->value, ts);
            return 1;
        }
    }
    if (!mt->rows || mt->n >= GPTPS_STATS_MEASURE_ROWS) return 0;
    r = &mt->rows[mt->n];
    memset(r, 0, sizeof *r);
    strcpy(r->name, x->name);              /* lengths checked by the caller */
    strcpy(r->method, x->method);
    r->m.struct_size = sizeof r->m;
    r->m.unit = x->unit; r->m.kind = x->kind;
    mrow_fold(&r->m, x->value, ts);
    mt->n += 1;
    return 1;
}

/* Fold an array of measurements into the engine's rows and a task type's. */
static void fold_array(gptps_stats *s, stats_task *k, const gptps_measure *arr, size_t n,
                       uint64_t ts, st_warns *w)
{
    size_t i;
    int have = n ? st_mtab_take(s, &k->mt, w) : 0;
    for (i = 0; i < n; ++i) {
        const gptps_measure *x = &arr[i];
        int ok = x->name && x->method &&
                 strlen(x->name) < GPTPS_STATS_NAME_MAX && strlen(x->method) < GPTPS_STATS_NAME_MAX;
        /* Each table counts what it could not take: the engine's and the task type's
         * fill up independently. */
        if (!ok || !mtab_fold(&s->mtotal, x, ts)) s->total.measures_dropped += 1;
        if (!ok || !have || !mtab_fold(&k->mt, x, ts)) k->c.measures_dropped += 1;
    }
}

/* Everything an event measured. Its array is read only if its struct_size says it
 * has one. */
static void fold_measures(gptps_stats *s, stats_task *k, const gptps_event *ev, st_warns *w)
{
    if (ev->struct_size < offsetof(gptps_event, n_measures) + sizeof(size_t) || !ev->measures) return;
    fold_array(s, k, ev->measures, ev->n_measures, ev->ts_ms, w);
}

/* ---- the observer ---- */
#define BUMP(field) do { s->total.field += 1; t->field += 1; } while (0)
#define GAUGE(field, d) do { s->total.field += (uint64_t)(d); t->field += (uint64_t)(d); } while (0)

static void sample_wait(gptps_stats *s, gptps_stats_counters *t, uint64_t ms)
{
    s->total.wait_samples += 1; s->total.wait_ms_sum += ms;
    if (ms > s->total.wait_ms_max) s->total.wait_ms_max = ms;
    t->wait_samples += 1; t->wait_ms_sum += ms; if (ms > t->wait_ms_max) t->wait_ms_max = ms;
}
static void sample_run(gptps_stats *s, gptps_stats_counters *t, uint64_t ms)
{
    s->total.run_samples += 1; s->total.run_ms_sum += ms;
    if (ms > s->total.run_ms_max) s->total.run_ms_max = ms;
    t->run_samples += 1; t->run_ms_sum += ms; if (ms > t->run_ms_max) t->run_ms_max = ms;
}

/* The item's life ends with this event: FINISHED, DEAD_LETTERED, DROPPED, or a FAILED
 * carrying GPTPS_E_CANCELLED. */
static int st_terminal(const gptps_event *ev)
{
    return ev->kind == GPTPS_EV_FINISHED || ev->kind == GPTPS_EV_DEAD_LETTERED || ev->kind == GPTPS_EV_DROPPED ||
           (ev->kind == GPTPS_EV_FAILED && ev->status == GPTPS_E_CANCELLED);
}

/* A handle the full table could not take: counted, and WARNed at most once a minute.
 * It joins the refused span, so its later events stay untracked too - counted once, here,
 * never again as a marker dropped. */
static void st_untracked(gptps_stats *s, gptps_handle h, st_warns *w)
{
    char *m;
    s->untracked++;
    if (h) {
        if (!s->rf_hi || h < s->rf_lo) s->rf_lo = h;
        if (h > s->rf_hi) s->rf_hi = h;
        s->untracked_live++;                     /* live, but not in the table: the peak counts it */
        st_peak(s);
    }
    if (st_warn_due(s, W_UNTRACKED) && (m = st_warn_slot(w)) != NULL)
        snprintf(m, ST_WARN_LEN, "gptps_stats: the handle table is full: a burst of %llu live handles (so far) past a table "
                 "of %lu: gptps_stats_reserve(s, %llu) (the next power of two >= the peak live handles) or "
                 "GPTPS_STATS_GROW_ON_EVENT; %llu event(s) not tracked so far and the pending / in-flight gauges "
                 "undercount (reads grow it up to cfg.handles_max = %lu; the figure once the burst is over: "
                 "gptps_stats_health.handles_needed)",
                 (unsigned long long)s->peak, (unsigned long)s->live_cap, (unsigned long long)st_needed(s),
                 (unsigned long long)s->untracked, (unsigned long)s->handles_max);
}

/* Event ORDER is not guaranteed across threads (see EVENT ORDER in gptps.h). Two
 * inversions reach this observer, and every transition below is keyed on the slot's
 * state and on the attempt an event names - never on the order events arrive in:
 *   - QUEUED comes from the submitting thread, so a fast task can report STARTED -
 *     or even FINISHED - before its own QUEUED. A terminal event that outruns QUEUED
 *     leaves a ST_DONE marker that the late QUEUED then clears.
 *   - RETRIED comes from the dispatcher, and attempt N can be cancelled - its
 *     terminal FAILED/E_CANCELLED emitted at once, on the cancelling thread - while
 *     the RETRIED announcing it is still being delivered. (An older engine could
 *     also re-admit a zero-backoff retry in the same pass, so attempt N could even
 *     start, finish or fail first; the engine no longer does, and nothing below
 *     depends on which.) Taken on arrival order, that late RETRIED reopened a handle
 *     already running or already over as PENDING, and the gauges never came back:
 *     pending and in_flight stuck above zero, a run sample lost, and after a
 *     terminal event a fresh slot leaked as well. So a RETRIED for an attempt this
 *     handle has already started or ended moves nothing, and a terminal event that
 *     outruns the RETRIED it is owed leaves a marker for that RETRIED to clear.
 * An attempt that starts before the event that queued it carries off its wait
 * sample, and that sample is taken when the late event arrives. Markers whose late
 * event never comes - work submitted before install never sends its QUEUED, and a
 * service's later runs have none - are bounded (1.12): the newest GPTPS_STATS_ORPHANS
 * are kept, the oldest dropped and counted. */
static void finish_slot(gptps_stats *s, stats_slot *slot, st_warns *w)
{
    if (slot->seen_queued && !OWES_RETRIED(slot)) { tab_del(s, slot); return; }
    if (slot->state != ST_DONE) {                /* wait for the late QUEUED / RETRIED */
        gptps_handle h = slot->handle;
        st_state(s, slot, ST_DONE);
        st_orphan(s, h, w);                 /* last use of `slot` */
    }
}
static void note_attempt(stats_slot *slot, uint32_t attempt)
{
    if (attempt > slot->att) slot->att = attempt;
}

static void observe_locked(gptps_stats *s, const gptps_event *ev, st_warns *w)
{
    stats_slot *slot;
    stats_task *k;
    gptps_stats_counters *t;

    if (ev->kind == GPTPS_EV_SAMPLE) {          /* a running job's values: no state moves */
        fold_measures(s, st_row(s, task_index(s, ev->task_name, w)), ev, w);
        return;
    }
    slot = tab_find(s, ev->handle);
    /* The task row: from the slot if we have one (the name pointer is only valid for
     * this call, so the row index is what we keep), else by name. */
    k = st_row(s, slot ? slot->task : task_index(s, ev->task_name, w));
    t = &k->c;
    if (ev->kind == GPTPS_EV_FINISHED || ev->kind == GPTPS_EV_FAILED) fold_measures(s, k, ev, w);
    if (!slot && ev->handle) {
        int late = ev->kind == GPTPS_EV_QUEUED || ev->kind == GPTPS_EV_RETRIED;
        if (late && s->ab_n && st_ab_has(s, ev->handle)) {
            /* the late event of a dropped marker or of an untracked handle: totals only */
            s->late_absorbed++;
        } else if (!late && st_refused(s, ev->handle)) {
            /* a refused handle's later events (or one in that span whose QUEUED is
             * still on its way): untracked, already counted; its late QUEUED, if one
             * comes, is absorbed */
            if (ev->kind == GPTPS_EV_STARTED || st_terminal(ev)) st_ab_add(s, ev->handle);
            if (st_terminal(ev) && s->untracked_live) s->untracked_live--;   /* it is over */
        /* A terminal event may always take an entry: it becomes a marker at once, and
         * the orphan FIFO bounds those. Anything else needs a live entry's room. */
        } else if (!st_terminal(ev) && st_live(s) >= s->live_cap) st_untracked(s, ev->handle, w);
        else if ((slot = tab_insert(s, ev->handle)) != NULL) { slot->task = (uint32_t)(k - s->rows); slot->state = ST_NONE; }
        else st_untracked(s, ev->handle, w);
    } else if (slot && slot->state == ST_DONE && st_live(s) >= s->live_cap &&
               (ev->kind == GPTPS_EV_STARTED || (ev->kind == GPTPS_EV_FAILED && ev->status != GPTPS_E_CANCELLED))) {
        /* reviving a marker makes it live again: it needs the room a new entry would */
        st_untracked(s, 0, w);
        slot = NULL;
    }

    switch (ev->kind) {
    case GPTPS_EV_QUEUED:
        BUMP(queued);
        if (slot) {
            slot->seen_queued = 1;
            /* A STARTED that outran this QUEUED left its wait sample owed here: the
             * queue time was not known yet, so the STARTED arm below kept when it
             * started instead (owe_q). Take the sample now rather than drop it. This
             * event's ts was stamped when the submitting thread finally reached the
             * emit, at or after the real enqueue, so started - this is a LOWER bound on
             * the true wait; when even that inverts, the item started before its own
             * notification left the submitter and 0 is the only honest answer. Dropping
             * the sample instead biases the reported mean UP, because the items that
             * lose this race are exactly the ones that waited least. Placement is
             * load-bearing: tab_del() backward-shifts and invalidates `slot`, so the
             * sample must be taken first. */
            if (slot->owe_q) {
                sample_wait(s, t, (slot->q_started_ms > ev->ts_ms) ? slot->q_started_ms - ev->ts_ms : 0);
                slot->owe_q = 0;
            }
            if (slot->state == ST_DONE) {                               /* already over */
                if (!OWES_RETRIED(slot)) tab_del(s, slot);
                break;
            }
            if (slot->state == ST_NONE) { st_state(s, slot, ST_PENDING); GAUGE(pending, 1); }
            if (!slot->have_queued_ms) { slot->queued_ms = ev->ts_ms; slot->have_queued_ms = 1; }
        }
        break;
    case GPTPS_EV_STARTED:
        BUMP(started);
        if (slot) {
            if (slot->state == ST_PENDING) GAUGE(pending, -1);
            if (ev->attempt <= 1) { slot->ann = 0; slot->owe_r = 0; }  /* a new cycle: REQUEUE */
            slot->att = ev->attempt;
            st_state(s, slot, ST_RUNNING); slot->started_ms = ev->ts_ms; slot->have_started_ms = 1;
            GAUGE(in_flight, 1);
            /* The wait is measured from the event that queued THIS attempt. If that
             * event has not arrived yet, the sample is owed to it instead. */
            if (ev->attempt > 1 && slot->ann < ev->attempt) {
                slot->owe_r = ev->attempt;                 /* outran its RETRIED */
            } else if (!slot->have_queued_ms) {
                slot->owe_q = 1; slot->q_started_ms = ev->ts_ms;   /* outran its QUEUED */
            } else {    /* in order, but two threads' stamps can still invert: 0 */
                sample_wait(s, t, (ev->ts_ms > slot->queued_ms) ? ev->ts_ms - slot->queued_ms : 0);
            }
        }
        break;
    case GPTPS_EV_FINISHED:
        BUMP(finished); BUMP(terminal);
        if (slot) {
            note_attempt(slot, ev->attempt);
            if (slot->state == ST_RUNNING) {
                GAUGE(in_flight, -1);
                if (slot->have_started_ms && ev->ts_ms >= slot->started_ms)
                    sample_run(s, t, ev->ts_ms - slot->started_ms);
            }
            finish_slot(s, slot, w);
        }
        break;
    case GPTPS_EV_FAILED:
        BUMP(failed);
        if (ev->status == GPTPS_E_CANCELLED) { BUMP(cancelled); BUMP(terminal); }
        if (slot) {
            note_attempt(slot, ev->attempt);
            if (slot->state == ST_RUNNING) {
                GAUGE(in_flight, -1);
                if (slot->have_started_ms && ev->ts_ms >= slot->started_ms)
                    sample_run(s, t, ev->ts_ms - slot->started_ms);
            } else if (slot->state == ST_PENDING) {
                GAUGE(pending, -1);           /* cancelled before it ever started */
            }
            st_state(s, slot, ST_LIMBO);
            if (ev->status == GPTPS_E_CANCELLED) finish_slot(s, slot, w);
        }
        break;
    case GPTPS_EV_RETRIED:
        BUMP(retried);
        if (slot) {
            if (ev->attempt > slot->ann) slot->ann = ev->attempt;
            if (slot->owe_r && slot->owe_r == ev->attempt) {   /* its STARTED outran it */
                sample_wait(s, t, (slot->started_ms > ev->ts_ms) ? slot->started_ms - ev->ts_ms : 0);
                slot->owe_r = 0;
            }
            /* LATE: the attempt it announces has already started or ended, or the
             * handle is over. It moves no state - reopening the slot as PENDING is
             * what used to leave the gauges stuck - and it clears the marker a
             * terminal event left waiting for it (tab_del invalidates `slot`). */
            if (slot->att >= ev->attempt || slot->state == ST_DONE) {
                if (slot->state == ST_DONE && slot->seen_queued) tab_del(s, slot);
                break;
            }
            if (slot->state != ST_PENDING) GAUGE(pending, 1);
            st_state(s, slot, ST_PENDING); slot->queued_ms = ev->ts_ms; slot->have_queued_ms = 1;
        }
        break;
    case GPTPS_EV_DEAD_LETTERED:
    case GPTPS_EV_DROPPED:
        if (ev->kind == GPTPS_EV_DEAD_LETTERED) BUMP(dead_lettered); else BUMP(dropped);
        BUMP(terminal);
        if (slot) {
            note_attempt(slot, ev->attempt);
            if (slot->state == ST_PENDING) GAUGE(pending, -1);
            else if (slot->state == ST_RUNNING) GAUGE(in_flight, -1);   /* defensive */
            finish_slot(s, slot, w);
        }
        break;
    default:
        break;
    }
    st_peak(s);
}

/* mu held: would this event need a live entry - a new handle's, or a marker revived -
 * where observe_locked asks for room? The same cases as its room checks, in its order;
 * only asked when every live entry is taken. */
static int st_needs_room(gptps_stats *s, const gptps_event *ev)
{
    stats_slot *slot;
    if (ev->kind == GPTPS_EV_SAMPLE || !ev->handle) return 0;
    slot = tab_find(s, ev->handle);
    if (!slot) {
        int late = ev->kind == GPTPS_EV_QUEUED || ev->kind == GPTPS_EV_RETRIED;
        if (late && s->ab_n && st_ab_has(s, ev->handle)) return 0;
        if (!late && st_refused(s, ev->handle)) return 0;
        return !st_terminal(ev);
    }
    return slot->state == ST_DONE &&
           (ev->kind == GPTPS_EV_STARTED || (ev->kind == GPTPS_EV_FAILED && ev->status != GPTPS_E_CANCELLED));
}

/* GPTPS_STATS_GROW_ON_EVENT (1.13): mu held, every live entry taken, and this event
 * needs one - grow on the event path instead of leaving the handle untracked. The
 * readers' storage protocol: unlock, allocate, relock, re-check (while the lock was down
 * another thread may have grown the table, filled it again, switched the observer off or
 * resumed it), swap, and free the old table only with the lock dropped - the caller
 * frees what this returns. Each pass the capacity rises (this thread's swap or
 * another's) toward handles_max, so the loop ends. A failed allocation is counted (nomem,
 * WARNed) and the event goes on to observe_locked, which leaves the handle untracked
 * exactly as without the flag; no tracked entry is touched. */
static stats_slot *st_grow_on_event(gptps_stats *s, const gptps_event *ev, st_warns *w)
{
    stats_slot *gone = NULL;
    while (s->on && st_live(s) >= s->live_cap && s->live_cap < s->handles_max && st_needs_room(s, ev)) {
        size_t want = s->live_cap * 2, ncap;
        stats_slot *nt;
        if (want > s->handles_max) want = s->handles_max;
        ncap = st_slots_for(want);
        apx_mutex_unlock(&s->mu);
        free(gone);                              /* an earlier pass's old table or loser */
        nt = (stats_slot *)calloc(ncap, sizeof *nt);
        apx_mutex_lock(&s->mu);
        gone = NULL;
        if (!nt) { st_grow_nomem(s, want, w); break; }
        if (!s->on || s->live_cap >= want) { gone = nt; continue; }   /* switched off, or another grew it: look again */
        gone = st_swap_in(s, nt, ncap, want);
        s->grows_event++;
    }
    return gone;
}

static void stats_observe(const gptps_event *ev, void *ud)
{
    gptps_stats *s = (gptps_stats *)ud;
    stats_slot *gone = NULL;
    st_warns w;
    if (!ST_FLAG_GET(&s->on)) return;           /* switched off: not even the lock */
    w.n = 0;
    apx_mutex_lock(&s->mu);
    if (s->on && s->grow_ev && st_live(s) >= s->live_cap) gone = st_grow_on_event(s, ev, &w);
    if (s->on) observe_locked(s, ev, &w);
    apx_mutex_unlock(&s->mu);
    free(gone);                                  /* the table it replaced, with the lock released */
    st_warn_flush(&w);
}

/* ---- construction ---- */
static gptps_stats *st_new(const gptps_stats_config *cfg, uint32_t def_mtabs)
{
    gptps_stats_config c;
    gptps_stats *s;
    size_t idx, i;
    memset(&c, 0, sizeof c);
    if (cfg) {
        size_t n = cfg->struct_size;
        if (n < ST_CFG_MIN) {
            gptps_log(NULL, GPTPS_LOG_WARN, "gptps_stats: config refused: struct_size is below the 1.12 "
                      "sizeof(gptps_stats_config) (set it to sizeof)");
            return NULL;
        }
        memcpy(&c, cfg, n < sizeof c ? n : sizeof c);
    }
    if (!c.max_tasks) c.max_tasks = ST_DEF_TASKS;
    if (!c.handles) c.handles = ST_DEF_HANDLES;
    if (!c.handles_max) c.handles_max = c.handles > ST_DEF_HMAX ? c.handles : ST_DEF_HMAX;
    if (!c.measure_tables) c.measure_tables = def_mtabs;
    if (c.max_tasks > ST_MAX_TASKS || c.handles > ST_MAX_HANDLES || c.handles_max < c.handles ||
        c.handles_max > ST_MAX_HANDLES || c.measure_tables > ST_MAX_MTABS ||
        (c.flags & ~(GPTPS_STATS_START_DISABLED | GPTPS_STATS_GROW_ON_EVENT))) {
        char m[256];
        snprintf(m, sizeof m, "gptps_stats: config refused: max_tasks %u (1..%u), handles %u (1..%u), handles_max %u "
                 "(handles..%u), measure_tables %u (1..%u), flags 0x%x", (unsigned)c.max_tasks, ST_MAX_TASKS,
                 (unsigned)c.handles, ST_MAX_HANDLES, (unsigned)c.handles_max, ST_MAX_HANDLES,
                 (unsigned)c.measure_tables, ST_MAX_MTABS, (unsigned)c.flags);
        gptps_log(NULL, GPTPS_LOG_WARN, m);
        return NULL;
    }
    s = (gptps_stats *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->total.struct_size = sizeof s->total;
    s->max_tasks = c.max_tasks;
    s->handles_max = c.handles_max;
    s->live_cap = c.handles;
    s->tabcap = st_slots_for(c.handles);
    idx = st_pow2((size_t)c.max_tasks * 2);
    s->tidx_mask = (uint32_t)(idx - 1);
    s->mpool_cap = c.measure_tables;
    s->rows = (stats_task *)calloc((size_t)c.max_tasks + 1, sizeof *s->rows);
    s->tidx = (int32_t *)malloc(idx * sizeof *s->tidx);
    s->tab = (stats_slot *)calloc(s->tabcap, sizeof *s->tab);
    s->mtotal.rows = (stats_mrow *)calloc(GPTPS_STATS_MEASURE_ROWS, sizeof *s->mtotal.rows);
    s->mtotal.owned = 1;
    if (c.measure_tables)
        s->mpool = (stats_mrow *)calloc((size_t)c.measure_tables * GPTPS_STATS_MEASURE_ROWS, sizeof *s->mpool);
    if (!s->rows || !s->tidx || !s->tab || !s->mtotal.rows || (c.measure_tables && !s->mpool)) {
        free(s->rows); free(s->tidx); free(s->tab); free(s->mtotal.rows); free(s->mpool); free(s);
        return NULL;
    }
    for (i = 0; i < idx; ++i) s->tidx[i] = -1;
    memcpy(s->rows[s->max_tasks].name, GPTPS_STATS_OTHER_NAME, sizeof GPTPS_STATS_OTHER_NAME);
    s->rows[s->max_tasks].c.struct_size = sizeof s->rows[s->max_tasks].c;
    s->on = (c.flags & GPTPS_STATS_START_DISABLED) ? 0 : 1;
    s->grow_ev = (c.flags & GPTPS_STATS_GROW_ON_EVENT) ? 1 : 0;
    apx_mutex_init(&s->mu);
    return s;
}

/* ---- public ---- */
gptps_stats *gptps_stats_install_ex(gptps *e, const gptps_stats_config *cfg)
{
    gptps_stats *s;
    if (!e) return NULL;
    if ((s = st_new(cfg, ST_DEF_MTABS)) == NULL) return NULL;
    s->e = e;
    if (gptps_register_observer(e, stats_observe, s) != GPTPS_OK) {
        gptps_stats_close(s); return NULL;
    }
    return s;
}

gptps_stats *gptps_stats_install(gptps *e) { return gptps_stats_install_ex(e, NULL); }

gptps_stats *gptps_stats_open(void)
{
    gptps_stats_config c;
    memset(&c, 0, sizeof c);
    c.struct_size = sizeof c;
    c.measure_tables = 0;   /* folds are host calls: they allocate tables as they need them */
    return st_new(&c, 0);
}

void gptps_stats_close(gptps_stats *s)
{
    size_t i;
    if (!s) return;
    /* No unregister: this runs AFTER gptps_shutdown, which freed the engine and its
     * observer list. See the contract in the header. */
    apx_mutex_destroy(&s->mu);
    for (i = 0; i <= s->max_tasks; ++i) if (s->rows[i].mt.owned) free(s->rows[i].mt.rows);
    free(s->mtotal.rows);
    free(s->mpool);
    free(s->rows);
    free(s->tidx);
    free(s->tab);
    free(s);
}

gptps_status gptps_stats_set_enabled(gptps_stats *s, int on)
{
    int was;
    if (!s) return GPTPS_E_INVAL;
    on = on ? 1 : 0;
    apx_mutex_lock(&s->mu);
    was = s->on;
    if (on && !was) {
        /* The work in flight across the pause cannot be accounted for: its events were
         * not seen. Drop every entry and start the gauges from zero. */
        size_t i;
        memset(s->tab, 0, s->tabcap * sizeof *s->tab);
        s->tabn = s->ndone = 0;
        s->orph_head = s->orph_n = 0;
        memset(s->ab_set, 0, sizeof s->ab_set);
        s->ab_head = s->ab_n = 0;
        s->rf_lo = s->rf_hi = 0;
        s->untracked_live = 0;                   /* the peak stays: a sizing figure since install */
        s->total.pending = s->total.in_flight = 0;
        for (i = 0; i <= s->max_tasks; ++i) s->rows[i].c.pending = s->rows[i].c.in_flight = 0;
        s->resumes++;
        s->resumed_ms = gptps_now_ms(NULL);
        if (!s->resumed_ms) s->resumed_ms = 1;   /* 0 means "since install" */
    }
    ST_FLAG_SET(&s->on, on);
    apx_mutex_unlock(&s->mu);
    if (on && !was)
        gptps_log(NULL, GPTPS_LOG_WARN, "gptps_stats: switched on again: the totals miss every event while it was "
                  "off, and the pending / in-flight gauges and the samples now cover only work queued from now "
                  "(gptps_stats_health.gauges_since_ms)");
    else if (!on && was)
        gptps_log(NULL, GPTPS_LOG_WARN, "gptps_stats: switched off: no event is counted until "
                  "gptps_stats_set_enabled(s, 1), which restarts the gauges from zero");
    return GPTPS_OK;
}

int gptps_stats_enabled(gptps_stats *s)
{
    return s ? (ST_FLAG_GET(&s->on) ? 1 : 0) : 0;
}

gptps_status gptps_stats_reserve(gptps_stats *s, size_t handles)
{
    if (!s || handles > ST_MAX_HANDLES) return GPTPS_E_INVAL;
    return st_grow(s, handles);
}

static size_t st_memory(const gptps_stats *s)
{
    size_t n = sizeof *s, i;
    n += ((size_t)s->max_tasks + 1) * sizeof *s->rows;
    n += ((size_t)s->tidx_mask + 1) * sizeof *s->tidx;
    n += s->tabcap * sizeof *s->tab;
    n += (size_t)GPTPS_STATS_MEASURE_ROWS * sizeof(stats_mrow);              /* the engine's */
    n += (size_t)s->mpool_cap * GPTPS_STATS_MEASURE_ROWS * sizeof(stats_mrow);
    for (i = 0; i <= s->max_tasks; ++i)
        if (s->rows[i].mt.owned) n += (size_t)GPTPS_STATS_MEASURE_ROWS * sizeof(stats_mrow);
    return n;
}

/* mu held: the health, into a caller's struct already checked for its size. */
static void st_health_out(gptps_stats *s, gptps_stats_health_info *out)
{
    gptps_stats_health_info h;
    size_t n = out->struct_size, i, owned = 0;
    memset(&h, 0, sizeof h);
    h.enabled = s->on ? 1u : 0u;
    h.types_folded_more = s->fold_more ? 1u : 0u;
    h.resumes = s->resumes;
    h.gauges_since_ms = s->resumed_ms;
    h.handles_cap = s->live_cap;
    h.handles_max = s->handles_max;
    h.handles_live = st_live(s);
    h.markers = s->ndone;
    h.handles_untracked = s->untracked;
    h.orphans_dropped = s->orphans_dropped;
    h.grows = s->grows;
    h.tasks_cap = s->max_tasks;
    h.tasks = s->ntasks;
    h.types_folded = s->nfold;
    for (i = 0; i <= s->max_tasks; ++i) if (s->rows[i].mt.owned) owned++;
    h.measure_tables_cap = s->mpool_cap;
    h.measure_tables_used = s->mpool_used + owned;
    h.measure_tables_refused = s->mtab_refused;
    h.nomem = s->nomem;
    h.memory_bytes = st_memory(s);
    h.late_absorbed = s->late_absorbed;
    h.handles_peak = s->peak;
    h.handles_needed = st_needed(s);
    h.grows_on_event = s->grows_event;
    if (n > sizeof h) n = sizeof h;
    h.struct_size = n;
    memcpy(out, &h, n);
}

gptps_status gptps_stats_health(gptps_stats *s, gptps_stats_health_info *out)
{
    size_t want;
    st_warns w;
    if (!s || !out || out->struct_size < ST_HEALTH_MIN) return GPTPS_E_INVAL;
    w.n = 0;
    apx_mutex_lock(&s->mu);
    st_health_out(s, out);
    want = st_grow_want(s, &w);
    apx_mutex_unlock(&s->mu);
    st_grow_after(s, want, &w);
    return GPTPS_OK;
}

gptps_status gptps_stats_total(gptps_stats *s, gptps_stats_counters *out)
{
    size_t want;
    st_warns w;
    if (!s || !out) return GPTPS_E_INVAL;
    w.n = 0;
    apx_mutex_lock(&s->mu);
    st_cnt_out(out, &s->total);
    want = st_grow_want(s, &w);
    apx_mutex_unlock(&s->mu);
    st_grow_after(s, want, &w);
    return GPTPS_OK;
}

gptps_status gptps_stats_task(gptps_stats *s, const char *task, gptps_stats_counters *out)
{
    stats_task *k;
    size_t want;
    st_warns w;
    gptps_status st = GPTPS_E_NOTFOUND;
    if (!s || !task || !out) return GPTPS_E_INVAL;
    w.n = 0;
    apx_mutex_lock(&s->mu);
    if ((k = st_row_find(s, task)) != NULL) { st_cnt_out(out, &k->c); st = GPTPS_OK; }
    want = st_grow_want(s, &w);
    apx_mutex_unlock(&s->mu);
    st_grow_after(s, want, &w);
    return st;
}

size_t gptps_stats_task_count(gptps_stats *s)
{
    size_t n;
    if (!s) return 0;
    apx_mutex_lock(&s->mu);
    n = st_nrows(s);
    apx_mutex_unlock(&s->mu);
    return n;
}

static void copy_out(char *buf, size_t cap, const char *src)
{
    size_t n;
    if (!buf || !cap) return;
    n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(buf, src, n); buf[n] = 0;
}

gptps_status gptps_stats_task_at(gptps_stats *s, size_t index,
                                 char *name_buf, size_t name_cap, gptps_stats_counters *out)
{
    stats_task *k;
    size_t want;
    st_warns w;
    gptps_status st = GPTPS_E_NOTFOUND;
    if (!s) return GPTPS_E_INVAL;
    w.n = 0;
    apx_mutex_lock(&s->mu);
    if ((k = st_row_at(s, index)) != NULL) {
        copy_out(name_buf, name_cap, k->name);
        if (out) st_cnt_out(out, &k->c);
        st = GPTPS_OK;
    }
    want = st_grow_want(s, &w);
    apx_mutex_unlock(&s->mu);
    st_grow_after(s, want, &w);
    return st;
}

gptps_status gptps_stats_snapshot(gptps_stats *s, gptps_stats_counters *total,
                                  gptps_stats_health_info *health,
                                  gptps_stats_row *rows, size_t row_size, size_t cap,
                                  size_t *needed)
{
    size_t nrows, i, want, rn, cn;
    st_warns w;
    if (!s || (rows && cap && row_size < ST_ROW_MIN) || (!rows && cap) ||
        (health && health->struct_size < ST_HEALTH_MIN)) return GPTPS_E_INVAL;
    rn = row_size < sizeof(gptps_stats_row) ? row_size : sizeof(gptps_stats_row);
    cn = rn > offsetof(gptps_stats_row, c) ? rn - offsetof(gptps_stats_row, c) : 0;
    w.n = 0;
    apx_mutex_lock(&s->mu);
    nrows = st_nrows(s);
    if (total) st_cnt_out(total, &s->total);
    if (health) st_health_out(s, health);
    for (i = 0; rows && i < cap && i < nrows; ++i) {
        gptps_stats_row r;
        stats_task *k = st_row_at(s, i);
        memset(&r, 0, sizeof r);
        r.struct_size = rn;
        r.flags = (i == s->ntasks) ? GPTPS_STATS_ROW_OTHER : 0u;
        memcpy(r.name, k->name, sizeof r.name);
        memcpy(&r.c, &k->c, sizeof r.c);
        r.c.struct_size = cn;
        memcpy((char *)rows + i * row_size, &r, rn);
    }
    want = st_grow_want(s, &w);
    apx_mutex_unlock(&s->mu);
    st_grow_after(s, want, &w);
    if (needed) *needed = nrows;
    return nrows > cap ? GPTPS_E_FULL : GPTPS_OK;
}

static void reset_row(gptps_stats_counters *c)
{
    uint64_t pending = c->pending, in_flight = c->in_flight;
    memset(c, 0, sizeof *c);
    c->struct_size = sizeof *c;
    c->pending = pending; c->in_flight = in_flight;
}

void gptps_stats_reset(gptps_stats *s)
{
    size_t i;
    if (!s) return;
    apx_mutex_lock(&s->mu);
    reset_row(&s->total);
    s->mtotal.n = 0;
    for (i = 0; i <= s->max_tasks; ++i) { reset_row(&s->rows[i].c); s->rows[i].mt.n = 0; }
    apx_mutex_unlock(&s->mu);
}

void gptps_stats_merge(gptps_stats_counters *d, const gptps_stats_counters *a)
{
    size_t n;
    if (!d || !a) return;
    n = st_cnt_size(d->struct_size);            /* d is written only as far as it has room */
    d->struct_size = n;
    d->queued += a->queued; d->started += a->started; d->finished += a->finished;
    d->failed += a->failed; d->retried += a->retried; d->dead_lettered += a->dead_lettered;
    d->dropped += a->dropped; d->cancelled += a->cancelled; d->terminal += a->terminal;
    d->pending += a->pending; d->in_flight += a->in_flight;
    d->wait_samples += a->wait_samples; d->wait_ms_sum += a->wait_ms_sum;
    if (a->wait_ms_max > d->wait_ms_max) d->wait_ms_max = a->wait_ms_max;
    d->run_samples += a->run_samples; d->run_ms_sum += a->run_ms_sum;
    if (a->run_ms_max > d->run_ms_max) d->run_ms_max = a->run_ms_max;
    if (n >= ST_CNT_FLOOR && a->struct_size >= ST_CNT_FLOOR)
        d->measures_dropped += a->measures_dropped;
}

/* The table for `task` (NULL: the engine's), or NULL if that task type is unknown.
 * Caller holds s->mu. */
static stats_mtab *mtab_for(gptps_stats *s, const char *task)
{
    stats_task *k;
    if (!task) return &s->mtotal;
    k = st_row_find(s, task);
    return k ? &k->mt : NULL;
}

gptps_status gptps_stats_measure_get(gptps_stats *s, const char *task, const char *name,
                                     const char *method, gptps_stats_measure *out)
{
    stats_mtab *mt;
    size_t i;
    gptps_status st = GPTPS_E_NOTFOUND;
    if (!s || !name || !method || !out) return GPTPS_E_INVAL;
    apx_mutex_lock(&s->mu);
    mt = mtab_for(s, task);
    for (i = 0; mt && i < mt->n; ++i)
        if (strcmp(mt->rows[i].name, name) == 0 && strcmp(mt->rows[i].method, method) == 0) {
            st_meas_out(out, &mt->rows[i].m); st = GPTPS_OK; break;
        }
    apx_mutex_unlock(&s->mu);
    return st;
}

size_t gptps_stats_measure_count(gptps_stats *s, const char *task)
{
    stats_mtab *mt;
    size_t n = 0;
    if (!s) return 0;
    apx_mutex_lock(&s->mu);
    if ((mt = mtab_for(s, task)) != NULL) n = mt->n;
    apx_mutex_unlock(&s->mu);
    return n;
}

gptps_status gptps_stats_measure_at(gptps_stats *s, const char *task, size_t index,
                                    char *name_buf, size_t name_cap,
                                    char *method_buf, size_t method_cap,
                                    gptps_stats_measure *out)
{
    stats_mtab *mt;
    gptps_status st = GPTPS_E_NOTFOUND;
    if (!s) return GPTPS_E_INVAL;
    apx_mutex_lock(&s->mu);
    mt = mtab_for(s, task);
    if (mt && index < mt->n) {
        copy_out(name_buf, name_cap, mt->rows[index].name);
        copy_out(method_buf, method_cap, mt->rows[index].method);
        if (out) st_meas_out(out, &mt->rows[index].m);
        st = GPTPS_OK;
    }
    apx_mutex_unlock(&s->mu);
    return st;
}

gptps_status gptps_stats_measure_fold(gptps_stats *s, const char *task,
                                      const gptps_measure *m, size_t n, uint64_t ts_ms)
{
    stats_task *k;
    stats_mrow *lost = NULL;
    st_warns w;
    if (!s || !task || (n && !m)) return GPTPS_E_INVAL;
    w.n = 0;
    apx_mutex_lock(&s->mu);
    k = st_row(s, task_index(s, task, &w));
    if (n && !k->mt.rows && s->mpool_used >= s->mpool_cap) {
        /* A host call, not the event path: the table it needs is allocated here, with
         * the lock released, and installed under it (unless another fold did first). */
        stats_mrow *nr;
        uint32_t ti = (uint32_t)(k - s->rows);
        apx_mutex_unlock(&s->mu);
        nr = (stats_mrow *)calloc(GPTPS_STATS_MEASURE_ROWS, sizeof *nr);
        apx_mutex_lock(&s->mu);
        k = st_row(s, ti);                       /* rows never move: the index still names it */
        if (!nr) {
            char *msg;
            s->nomem++;
            /* counted here, as nomem: not again as a spent pool by fold_array */
            k->mt.refused = 1;
            if (st_warn_due(s, W_NOMEM) && (msg = st_warn_slot(&w)) != NULL)
                snprintf(msg, ST_WARN_LEN, "gptps_stats: no memory for task type '%.64s''s measurement table: its values are "
                         "dropped and counted in measures_dropped (gptps_stats_health.nomem = %llu)",
                         task, (unsigned long long)s->nomem);
        } else if (!k->mt.rows) {
            k->mt.rows = nr; k->mt.n = 0; k->mt.owned = 1; nr = NULL;
        }
        lost = nr;                               /* another fold installed one first */
    }
    fold_array(s, k, m, n, ts_ms, &w);
    apx_mutex_unlock(&s->mu);
    free(lost);                                  /* freed with the lock released */
    st_warn_flush(&w);
    return GPTPS_OK;
}

void gptps_stats_measure_merge(gptps_stats_measure *d, const gptps_stats_measure *a)
{
    if (!d || !a || a->count == 0) return;
    d->struct_size = sizeof *d;
    if (d->count == 0) { *d = *a; d->struct_size = sizeof *d; return; }
    if (a->min < d->min) d->min = a->min;
    if (a->max > d->max) d->max = a->max;
    d->sum = (d->sum > UINT64_MAX - a->sum) ? UINT64_MAX : d->sum + a->sum;
    d->count += a->count;
    if (a->last_ms >= d->last_ms) { d->last = a->last; d->last_ms = a->last_ms; }
}
