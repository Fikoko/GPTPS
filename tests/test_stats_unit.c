/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_stats_unit.c - gptps_stats' 1.12 hardening, with the add-on compiled in (its
 * statics in reach) and its allocator, lock and strcmp interposed:
 *    1) the runtime switch: off returns before the lock; on again drops the handle
 *       table, zeroes the gauges and says so (health, WARN)
 *    2) the hashed task index: one compare a lookup at 1000 types; the '(other)' row
 *       past cfg.max_tasks, counted and WARNed
 *    3) bounded orphans: markers for handles whose QUEUED never came stay <= 64
 *    4) the event path allocates nothing; a full table refuses (never evicts) and
 *       counts; reads grow it outside the lock; reserve
 *    5) getters honour struct_size; install honours the config's
 *    6) the bulk snapshot: one lock, rows add up to totals, needed / E_FULL
 *    7) OOM counted: growth, a host fold's table, the measurement pool, install
 *    8) WARNs at most once a minute per condition, on one clock whatever ts the events
 *       and host folds carry
 *    9) on a real engine: install_ex, START_DISABLED, the switch; a burst past the table
 *   10) a storm: emitters, a reader and the switch on several threads (TSan)
 *   11) late events: a dropped marker's late QUEUED / RETRIED and an untracked handle's
 *       later events move no gauge; reviving a marker needs room
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* every allocation the add-on makes, counted, and failed on demand */
static int t_allocs;                /* ints: tests/test_atomic_compat.h maps them on MSVC */
static int t_held;                  /* the add-on's lock is held (single-threaded checks) */
static int t_locks;                 /* its lock acquisitions */
static int t_alloc_locked;          /* allocations made while it was held */
static int t_calloc_skip, t_calloc_fail, t_malloc_fail;   /* set by the main thread only */
static int t_free_locked;           /* frees (even of NULL) while it was held */
static void (*t_calloc_hook)(void); /* run once, inside the next calloc (main thread only) */
static uint64_t t_now = 1;          /* the add-on's WARN clock (ST_NOW_MS), set by the main thread */
static void t_note_alloc(void)
{
    __atomic_add_fetch(&t_allocs, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&t_held, __ATOMIC_SEQ_CST) > 0) __atomic_add_fetch(&t_alloc_locked, 1, __ATOMIC_SEQ_CST);
}
/* after `t_calloc_skip` more calls succeed, the next `t_calloc_fail` return NULL */
static void *t_calloc(size_t n, size_t s)
{
    if (t_calloc_hook) { void (*f)(void) = t_calloc_hook; t_calloc_hook = NULL; f(); }
    t_note_alloc();
    if (t_calloc_fail > 0) {
        if (t_calloc_skip > 0) --t_calloc_skip;
        else { --t_calloc_fail; return NULL; }
    }
    return calloc(n, s);
}
static void *t_malloc(size_t n)
{
    t_note_alloc();
    if (t_malloc_fail > 0) { --t_malloc_fail; return NULL; }
    return malloc(n);
}
static void t_free(void *p)
{
    if (__atomic_load_n(&t_held, __ATOMIC_SEQ_CST) > 0) __atomic_add_fetch(&t_free_locked, 1, __ATOMIC_SEQ_CST);
    free(p);
}
static int t_strcmps;               /* the add-on's string compares */
static int t_strcmp(const char *a, const char *b) { __atomic_add_fetch(&t_strcmps, 1, __ATOMIC_SEQ_CST); return strcmp(a, b); }

#include "addon_compat.h"
#define calloc(n, s) t_calloc((n), (s))
#define malloc(n) t_malloc((n))
#define free(p) t_free((p))
#define ST_NOW_MS() (t_now)
#undef strcmp
#define strcmp(a, b) t_strcmp((a), (b))
#define apx_mutex_lock(m) (apx_mutex_lock(m), (void)__atomic_add_fetch(&t_held, 1, __ATOMIC_SEQ_CST), \
                           (void)__atomic_add_fetch(&t_locks, 1, __ATOMIC_SEQ_CST))
#define apx_mutex_unlock(m) ((void)__atomic_sub_fetch(&t_held, 1, __ATOMIC_SEQ_CST), apx_mutex_unlock(m))
#include "../addons/gptps_stats.c"
#undef calloc
#undef malloc
#undef free
#undef strcmp
#undef apx_mutex_lock
#undef apx_mutex_unlock

#if !defined(_WIN32)
#  include <pthread.h>
#endif

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int allocs(void) { return __atomic_load_n(&t_allocs, __ATOMIC_SEQ_CST); }
static int locks(void)  { return __atomic_load_n(&t_locks, __ATOMIC_SEQ_CST); }

/* ---- the log, captured ---- */
static char logbuf[16384];
static size_t loglen;
static int nwarn;
static void cap_sink(gptps_log_level lvl, const char *msg, void *ud)
{
    size_t n = strlen(msg);
    (void)ud;
    if (lvl >= GPTPS_LOG_WARN) ++nwarn;
    if (loglen + n + 2 < sizeof logbuf) { memcpy(logbuf + loglen, msg, n); loglen += n; logbuf[loglen++] = '\n'; logbuf[loglen] = 0; }
}
static void log_begin(void) { loglen = 0; logbuf[0] = 0; nwarn = 0; gptps_set_log_sink(cap_sink, NULL); }
static void drop_sink(gptps_log_level lvl, const char *msg, void *ud) { (void)lvl; (void)msg; (void)ud; }
static void log_end(void)   { gptps_set_log_sink(drop_sink, NULL); }   /* the WARNs not under test: off stderr */
static int log_count(const char *needle)
{
    int n = 0; const char *p = logbuf;
    while ((p = strstr(p, needle)) != NULL) { ++n; p += strlen(needle); }
    return n;
}

/* ---- events, by hand ---- */
static void emit_a(gptps_stats *s, gptps_event_kind k, const char *task, gptps_handle h,
                   gptps_status st, uint64_t ts, uint32_t attempt)
{
    gptps_event ev;
    memset(&ev, 0, sizeof ev);
    ev.struct_size = sizeof ev; ev.kind = k; ev.handle = h; ev.task_name = task;
    ev.ts_ms = ts; ev.status = st; ev.attempt = attempt;
    stats_observe(&ev, s);
}
static void emit(gptps_stats *s, gptps_event_kind k, const char *task, gptps_handle h, uint64_t ts)
{
    emit_a(s, k, task, h, GPTPS_OK, ts, k == GPTPS_EV_QUEUED ? 0 : 1);
}
static void life(gptps_stats *s, const char *task, gptps_handle h, uint64_t ts)
{
    emit(s, GPTPS_EV_QUEUED, task, h, ts);
    emit(s, GPTPS_EV_STARTED, task, h, ts + 1);
    emit(s, GPTPS_EV_FINISHED, task, h, ts + 2);
}
static gptps_stats *mk(uint32_t max_tasks, uint32_t handles, uint32_t handles_max, uint32_t mtabs)
{
    gptps_stats_config c;
    memset(&c, 0, sizeof c);
    c.struct_size = sizeof c; c.max_tasks = max_tasks; c.handles = handles;
    c.handles_max = handles_max; c.measure_tables = mtabs;
    return st_new(&c, ST_DEF_MTABS);
}
static gptps_stats_health_info health(gptps_stats *s)
{
    gptps_stats_health_info h;
    memset(&h, 0, sizeof h); h.struct_size = sizeof h;
    CHECK(gptps_stats_health(s, &h) == GPTPS_OK);
    return h;
}
static gptps_stats_counters total(gptps_stats *s)
{
    gptps_stats_counters c;
    memset(&c, 0, sizeof c); c.struct_size = sizeof c;
    CHECK(gptps_stats_total(s, &c) == GPTPS_OK);
    return c;
}

/* 1) the switch */
static void test_switch(void)
{
    gptps_stats *s = mk(0, 0, 0, 0);
    gptps_stats_counters c;
    gptps_stats_health_info h;
    int l0;
    CHECK(s != NULL); if (!s) return;
    CHECK(gptps_stats_enabled(s) == 1);
    emit(s, GPTPS_EV_QUEUED, "a", 1, 10);
    emit(s, GPTPS_EV_STARTED, "a", 1, 11);           /* 1 in flight across the pause */
    emit(s, GPTPS_EV_QUEUED, "a", 2, 12);            /* 1 pending across it */
    log_begin();
    CHECK(gptps_stats_set_enabled(s, 0) == GPTPS_OK);
    CHECK(gptps_stats_enabled(s) == 0);
    CHECK(log_count("switched off") == 1);
    l0 = locks();
    emit(s, GPTPS_EV_QUEUED, "a", 3, 13);
    emit(s, GPTPS_EV_FINISHED, "a", 1, 14);
    CHECK(locks() == l0);                            /* off: the observer never took the lock */
    c = total(s);
    CHECK(c.queued == 2 && c.finished == 0);         /* ... and counted nothing */
    CHECK(c.pending == 1 && c.in_flight == 1);
    CHECK(gptps_stats_set_enabled(s, 0) == GPTPS_OK && log_count("switched") == 1);   /* no change, no WARN */
    CHECK(gptps_stats_set_enabled(s, 1) == GPTPS_OK);
    CHECK(log_count("switched on again") == 1);
    log_end();
    h = health(s);
    CHECK(h.enabled == 1 && h.resumes == 1 && h.gauges_since_ms != 0);
    CHECK(h.handles_live == 0 && h.markers == 0);    /* the table was dropped */
    /* the default sizing's memory, as addons/README.md states it ("about 0.25 MiB") */
    printf("default object: %llu bytes\n", (unsigned long long)h.memory_bytes);
    CHECK(h.memory_bytes > 128 * 1024 && h.memory_bytes < 384 * 1024);
    c = total(s);
    CHECK(c.pending == 0 && c.in_flight == 0);       /* the gauges restart from zero */
    {   gptps_stats_counters tc; memset(&tc, 0, sizeof tc); tc.struct_size = sizeof tc;
        CHECK(gptps_stats_task(s, "a", &tc) == GPTPS_OK && tc.pending == 0 && tc.in_flight == 0); }
    /* the work from before the pause ends: no gauge wraps below zero */
    emit(s, GPTPS_EV_STARTED, "a", 2, 20);
    emit(s, GPTPS_EV_FINISHED, "a", 2, 21);
    c = total(s);
    CHECK(c.pending == 0 && c.in_flight == 0 && c.finished == 1);
    /* work admitted after the resume is exact */
    emit(s, GPTPS_EV_QUEUED, "a", 9, 30);
    c = total(s);
    CHECK(c.pending == 1);
    emit(s, GPTPS_EV_STARTED, "a", 9, 31);
    emit(s, GPTPS_EV_FINISHED, "a", 9, 32);
    c = total(s);
    CHECK(c.pending == 0 && c.in_flight == 0 && c.wait_samples == 2 && c.run_samples == 2);
    CHECK(gptps_stats_set_enabled(NULL, 1) == GPTPS_E_INVAL && gptps_stats_enabled(NULL) == 0);
    gptps_stats_close(s);
}

/* 2) the hashed index and '(other)' */
static void test_task_index(void)
{
    gptps_stats *s = mk(1000, 0, 0, 0);
    char nm[32];
    int i, before;
    gptps_stats_counters c;
    gptps_stats_health_info h;
    CHECK(s != NULL); if (!s) return;
    for (i = 0; i < 1000; ++i) { snprintf(nm, sizeof nm, "type%04d", i); life(s, nm, (gptps_handle)(i + 1), 0); }
    CHECK(gptps_stats_task_count(s) == 1000);
    /* the last type's first event: one compare, not a walk of 999 names */
    before = t_strcmps;
    emit(s, GPTPS_EV_QUEUED, "type0999", 5000, 1);
    CHECK(t_strcmps - before <= 2);
    before = t_strcmps;
    memset(&c, 0, sizeof c); c.struct_size = sizeof c;
    CHECK(gptps_stats_task(s, "type0500", &c) == GPTPS_OK && c.queued == 1 && c.finished == 1);
    CHECK(t_strcmps - before <= 2);
    CHECK(gptps_stats_task_at(s, 999, nm, sizeof nm, &c) == GPTPS_OK && strcmp(nm, "type0999") == 0 && c.queued == 2);
    gptps_stats_close(s);

    /* past max_tasks: one '(other)' row, last, counted and WARNed once */
    s = mk(3, 0, 0, 0);
    CHECK(s != NULL); if (!s) return;
    log_begin();
    for (i = 0; i < 6; ++i) { snprintf(nm, sizeof nm, "t%d", i); life(s, nm, (gptps_handle)(i + 1), 0); }
    life(s, "t5", 50, 0);                             /* a folded type again: not a new one */
    CHECK(log_count("has no row of its own") == 1);
    log_end();
    CHECK(gptps_stats_task_count(s) == 4);
    CHECK(gptps_stats_task_at(s, 3, nm, sizeof nm, &c) == GPTPS_OK);
    CHECK(strcmp(nm, GPTPS_STATS_OTHER_NAME) == 0 && c.queued == 4 && c.finished == 4);
    CHECK(gptps_stats_task(s, GPTPS_STATS_OTHER_NAME, &c) == GPTPS_OK && c.queued == 4);
    CHECK(gptps_stats_task(s, "t4", &c) == GPTPS_E_NOTFOUND);
    CHECK(gptps_stats_task_at(s, 4, nm, sizeof nm, &c) == GPTPS_E_NOTFOUND);
    h = health(s);
    CHECK(h.tasks == 3 && h.tasks_cap == 3 && h.types_folded == 3 && h.types_folded_more == 0);
    c = total(s);
    CHECK(c.queued == 7 && c.pending == 0 && c.in_flight == 0);
    /* a name longer than any task name folds too */
    {   char longn[200]; memset(longn, 'x', sizeof longn - 1); longn[sizeof longn - 1] = 0;
        s->w_any[W_FOLD] = 0;
        log_begin();
        life(s, longn, 99, 0);
        CHECK(log_count("its name is too long") == 1);
        log_end();
        CHECK(health(s).types_folded == 4); }
    gptps_stats_close(s);
}

/* 3) orphans */
static void test_orphans(void)
{
    gptps_stats *s = mk(0, 64, 64, 0);
    gptps_stats_health_info h;
    gptps_stats_counters c;
    int i;
    CHECK(s != NULL); if (!s) return;
    log_begin();
    /* 1000 handles submitted "before install": STARTED and FINISHED, never QUEUED */
    for (i = 0; i < 1000; ++i) {
        emit(s, GPTPS_EV_STARTED, "svc", (gptps_handle)(i + 1), 5);
        emit(s, GPTPS_EV_FINISHED, "svc", (gptps_handle)(i + 1), 6);
    }
    CHECK(log_count("marker(s) dropped") == 1);      /* once a minute, not once each */
    log_end();
    h = health(s);
    CHECK(h.markers == GPTPS_STATS_ORPHANS);
    CHECK(h.orphans_dropped == 1000 - GPTPS_STATS_ORPHANS);
    CHECK(h.handles_live == 0 && h.handles_untracked == 0);
    CHECK(s->tabn == GPTPS_STATS_ORPHANS);
    c = total(s);
    CHECK(c.in_flight == 0 && c.finished == 1000);
    /* a late QUEUED for a kept marker still clears it */
    emit(s, GPTPS_EV_QUEUED, "svc", 1000, 4);
    CHECK(health(s).markers == GPTPS_STATS_ORPHANS - 1);
    /* an always-up service's later runs (STARTED/FINISHED, no QUEUED) stay bounded */
    for (i = 0; i < 500; ++i) {
        emit(s, GPTPS_EV_STARTED, "svc", 77777, 10 + (uint64_t)i);
        emit(s, GPTPS_EV_FINISHED, "svc", 77777, 10 + (uint64_t)i);
    }
    h = health(s);
    CHECK(h.markers <= GPTPS_STATS_ORPHANS && h.handles_live == 0);
    CHECK(total(s).in_flight == 0);
    gptps_stats_close(s);
}

/* 4) no allocation on the event path; refuse, never evict; growth outside the lock */
static void test_bounded_table(void)
{
    gptps_stats *s = mk(0, 4, 4, 0);
    gptps_stats_counters c;
    gptps_stats_health_info h;
    int a0, i;
    CHECK(s != NULL); if (!s) return;
    a0 = allocs();
    log_begin();
    for (i = 1; i <= 6; ++i) emit(s, GPTPS_EV_QUEUED, "q", (gptps_handle)i, 0);
    CHECK(log_count("handle table is full") == 1);
    log_end();
    c = total(s);
    CHECK(c.queued == 6 && c.pending == 4);          /* 5 and 6 not tracked ... */
    h = health(s);
    CHECK(h.handles_untracked == 2 && h.handles_live == 4 && h.handles_cap == 4 && h.grows == 0);
    for (i = 1; i <= 6; ++i) { emit(s, GPTPS_EV_STARTED, "q", (gptps_handle)i, 1); emit(s, GPTPS_EV_FINISHED, "q", (gptps_handle)i, 2); }
    c = total(s);
    CHECK(c.pending == 0 && c.in_flight == 0 && c.finished == 6);   /* ... and the first 4 kept */
    CHECK(c.run_samples == 4 && c.wait_samples == 4);   /* 5 and 6 stay untracked to their end ... */
    h = health(s);
    CHECK(h.handles_untracked == 2 && h.markers == 0 && h.orphans_dropped == 0);   /* ... counted once, not as markers */
    CHECK(allocs() == a0);                            /* nothing allocated by an event */
    gptps_stats_close(s);

    /* many events, many handles, nothing allocated; reads grow the table outside the lock */
    s = mk(8, 16, 1024, 0);
    CHECK(s != NULL); if (!s) return;
    a0 = allocs();
    for (i = 1; i <= 100000; ++i) {
        char nm[8]; snprintf(nm, sizeof nm, "k%d", i % 8);
        emit(s, GPTPS_EV_QUEUED, nm, (gptps_handle)i, (uint64_t)i);
        if (i > 12) { emit(s, GPTPS_EV_STARTED, "k0", (gptps_handle)(i - 12), (uint64_t)i); emit(s, GPTPS_EV_FINISHED, "k0", (gptps_handle)(i - 12), (uint64_t)i); }
    }
    CHECK(allocs() == a0);
    t_alloc_locked = 0;
    h = health(s);                                    /* 12 live of 16: over half -> grows after this read */
    CHECK(h.handles_live == 12 && h.handles_cap == 16);
    h = health(s);
    CHECK(h.handles_cap == 32 && h.grows == 1 && h.handles_live == 12);
    CHECK(allocs() == a0 + 1);
    CHECK(t_alloc_locked == 0);                       /* allocated with the lock released */
    for (i = 100001; i <= 100030; ++i) emit(s, GPTPS_EV_QUEUED, "k1", (gptps_handle)i, 0);
    CHECK(health(s).handles_untracked == 10);         /* 32 live: 10 refused */
    h = health(s);                                    /* the refusals asked for room */
    CHECK(h.handles_cap == 64 && h.grows == 2);
    c = total(s);
    CHECK(c.pending == 32);                           /* 12 + 20 tracked; the refused ones never counted */
    CHECK(gptps_stats_reserve(s, 500) == GPTPS_OK && health(s).handles_cap == 500);
    CHECK(gptps_stats_reserve(s, 100) == GPTPS_OK && health(s).handles_cap == 500);   /* never shrinks */
    CHECK(gptps_stats_reserve(s, (size_t)ST_MAX_HANDLES + 1) == GPTPS_E_INVAL);
    CHECK(gptps_stats_reserve(NULL, 1) == GPTPS_E_INVAL);
    CHECK(total(s).pending == 32);                    /* entries survive the swaps */
    /* never above handles_max by itself */
    gptps_stats_close(s);
    s = mk(0, 8, 8, 0);
    CHECK(s != NULL); if (!s) return;
    for (i = 1; i <= 20; ++i) emit(s, GPTPS_EV_QUEUED, "q", (gptps_handle)i, 0);
    h = health(s); h = health(s);
    CHECK(h.handles_cap == 8 && h.grows == 0 && h.handles_max == 8);
    gptps_stats_close(s);
}

/* 5) struct_size honoured */
static void test_struct_size(void)
{
    gptps_stats *s = mk(0, 0, 0, 0);
    unsigned char buf[sizeof(gptps_stats_counters) + 32];
    gptps_stats_counters *c = (gptps_stats_counters *)(void *)buf;
    gptps_stats_measure m;
    gptps_measure x;
    size_t i;
    CHECK(s != NULL); if (!s) return;
    life(s, "a", 1, 0);
    memset(&x, 0, sizeof x);
    x.name = "acme.n"; x.method = "acme.m"; x.unit = GPTPS_UNIT_COUNT; x.kind = GPTPS_MEASURE_TOTAL; x.value = 3;
    s->total.measures_dropped = 5;                    /* so a write of the 1.7 field shows */
    /* a 1.2-1.6 caller's size: the 1.2 fields, nothing past them */
    memset(buf, 0xAB, sizeof buf);
    c->struct_size = ST_CNT_V1;
    CHECK(gptps_stats_total(s, c) == GPTPS_OK);
    CHECK(c->struct_size == ST_CNT_V1 && c->queued == 1 && c->finished == 1);
    for (i = ST_CNT_V1; i < sizeof buf; ++i) if (buf[i] != 0xAB) break;
    CHECK(i == sizeof buf);
    memset(buf, 0xAB, sizeof buf);
    c->struct_size = ST_CNT_V1;
    CHECK(gptps_stats_task(s, "a", c) == GPTPS_OK && c->queued == 1);
    for (i = ST_CNT_V1; i < sizeof buf; ++i) if (buf[i] != 0xAB) break;
    CHECK(i == sizeof buf);
    memset(buf, 0xAB, sizeof buf);
    c->struct_size = ST_CNT_V1;
    CHECK(gptps_stats_task_at(s, 0, NULL, 0, c) == GPTPS_OK && c->queued == 1);
    for (i = ST_CNT_V1; i < sizeof buf; ++i) if (buf[i] != 0xAB) break;
    CHECK(i == sizeof buf);
    /* never set (0): the 1.7 fields */
    memset(buf, 0xAB, sizeof buf);
    c->struct_size = 0;
    CHECK(gptps_stats_total(s, c) == GPTPS_OK);
    CHECK(c->struct_size == sizeof(gptps_stats_counters) && c->measures_dropped == 5);
    /* a later, larger caller: what exists now, its tail untouched */
    memset(buf, 0xAB, sizeof buf);
    c->struct_size = sizeof buf;
    CHECK(gptps_stats_total(s, c) == GPTPS_OK && c->struct_size == sizeof(gptps_stats_counters));
    for (i = sizeof(gptps_stats_counters); i < sizeof buf; ++i) if (buf[i] != 0xAB) break;
    CHECK(i == sizeof buf);
    /* merge into a 1.2 struct leaves its tail alone */
    {   gptps_stats_counters a = total(s);
        memset(buf, 0, sizeof buf); memset(buf + ST_CNT_V1, 0xAB, sizeof buf - ST_CNT_V1);
        c->struct_size = ST_CNT_V1;
        gptps_stats_merge(c, &a);
        CHECK(c->queued == 1 && c->struct_size == ST_CNT_V1);
        for (i = ST_CNT_V1; i < sizeof buf; ++i) if (buf[i] != 0xAB) break;
        CHECK(i == sizeof buf);
        memset(buf, 0, sizeof buf);                   /* zeroed (struct_size 0): as before, all of it */
        gptps_stats_merge(c, &a);
        CHECK(c->struct_size == sizeof(gptps_stats_counters) && c->measures_dropped == 5);
        /* a src from a 1.2-1.6 caller: its measures_dropped is not there to read */
        {   unsigned char sb[sizeof(gptps_stats_counters)];
            gptps_stats_counters *src = (gptps_stats_counters *)(void *)sb;
            memset(sb, 0xAB, sizeof sb);
            memset(sb, 0, ST_CNT_V1);
            src->struct_size = ST_CNT_V1; src->queued = 2;
            gptps_stats_merge(c, src);
            CHECK(c->queued == 3 && c->measures_dropped == 5); } }
    /* measurement rows: their 1.7 size is their floor */
    CHECK(gptps_stats_measure_fold(s, "a", &x, 1, 1) == GPTPS_OK);
    memset(&m, 0, sizeof m);
    CHECK(gptps_stats_measure_get(s, "a", "acme.n", "acme.m", &m) == GPTPS_OK && m.count == 1 && m.struct_size == sizeof m);
    /* health: a new struct, its size required; larger is filled as far as it exists */
    {   unsigned char hb[sizeof(gptps_stats_health_info) + 16];
        gptps_stats_health_info *h = (gptps_stats_health_info *)(void *)hb;
        memset(hb, 0xCD, sizeof hb);
        h->struct_size = sizeof hb;
        CHECK(gptps_stats_health(s, h) == GPTPS_OK && h->struct_size == sizeof(gptps_stats_health_info) && h->enabled == 1);
        for (i = sizeof(gptps_stats_health_info); i < sizeof hb; ++i) if (hb[i] != 0xCD) break;
        CHECK(i == sizeof hb);
        h->struct_size = ST_HEALTH_MIN - 1;
        CHECK(gptps_stats_health(s, h) == GPTPS_E_INVAL);
        CHECK(gptps_stats_health(NULL, h) == GPTPS_E_INVAL); }
    gptps_stats_close(s);

    /* install honours the config's struct_size: too small is refused, larger is read
     * only as far as this layout goes */
    {   unsigned char cb[sizeof(gptps_stats_config) + 16];
        gptps_stats_config *cfg = (gptps_stats_config *)(void *)cb;
        memset(cb, 0xEE, sizeof cb);                  /* garbage in a later caller's appended fields */
        memset(cfg, 0, sizeof *cfg);
        cfg->struct_size = sizeof cb; cfg->max_tasks = 5;
        s = st_new(cfg, ST_DEF_MTABS);
        CHECK(s != NULL && s && s->max_tasks == 5);
        gptps_stats_close(s);
        cfg->struct_size = ST_CFG_MIN - 1;
        log_begin();
        CHECK(st_new(cfg, ST_DEF_MTABS) == NULL);
        CHECK(log_count("struct_size") == 1);
        cfg->struct_size = sizeof *cfg; cfg->max_tasks = ST_MAX_TASKS + 1;
        CHECK(st_new(cfg, ST_DEF_MTABS) == NULL);
        cfg->max_tasks = 0; cfg->handles = 100; cfg->handles_max = 50;
        CHECK(st_new(cfg, ST_DEF_MTABS) == NULL);
        cfg->handles_max = 0; cfg->flags = 0x80;
        CHECK(st_new(cfg, ST_DEF_MTABS) == NULL);
        CHECK(log_count("config refused") == 4);
        log_end(); }
}

/* 6) the snapshot */
static void test_snapshot(void)
{
    gptps_stats *s = mk(4, 0, 0, 0);
    struct big { gptps_stats_row r; unsigned char tail[24]; } rows[8];
    gptps_stats_counters tot, sum;
    gptps_stats_health_info h;
    size_t need = 0, i;
    int l0, k;
    CHECK(s != NULL); if (!s) return;
    for (k = 0; k < 6; ++k) {
        char nm[8]; snprintf(nm, sizeof nm, "s%d", k);
        life(s, nm, (gptps_handle)(k + 1), 0);
        emit(s, GPTPS_EV_QUEUED, nm, (gptps_handle)(k + 100), 0);
    }
    (void)health(s);                                  /* settle any growth first */
    memset(rows, 0x5A, sizeof rows);
    memset(&tot, 0, sizeof tot); tot.struct_size = sizeof tot;
    memset(&h, 0, sizeof h); h.struct_size = sizeof h;
    l0 = locks();
    CHECK(gptps_stats_snapshot(s, &tot, &h, &rows[0].r, sizeof rows[0], 8, &need) == GPTPS_OK);
    CHECK(locks() == l0 + 1);                          /* ONE lock for all of it */
    CHECK(need == 5);                                  /* four rows and '(other)' */
    memset(&sum, 0, sizeof sum);
    for (i = 0; i < need; ++i) {
        size_t j;
        CHECK(rows[i].r.struct_size == sizeof(gptps_stats_row));
        CHECK(rows[i].r.c.struct_size == sizeof(gptps_stats_counters));
        gptps_stats_merge(&sum, &rows[i].r.c);
        for (j = 0; j < sizeof rows[i].tail; ++j) if (rows[i].tail[j] != 0x5A) break;
        CHECK(j == sizeof rows[i].tail);              /* the stride is the caller's, the tail untouched */
    }
    CHECK(strcmp(rows[0].r.name, "s0") == 0 && rows[0].r.flags == 0);
    CHECK(strcmp(rows[4].r.name, GPTPS_STATS_OTHER_NAME) == 0 && rows[4].r.flags == GPTPS_STATS_ROW_OTHER);
    CHECK(rows[4].r.c.queued == 4);
    CHECK(sum.queued == tot.queued && sum.terminal == tot.terminal && sum.pending == tot.pending && tot.pending == 6);
    CHECK(h.tasks == 4 && h.types_folded == 2);
    CHECK(rows[5].r.struct_size != sizeof(gptps_stats_row));   /* past the rows there are: untouched */
    /* too small: what fits, E_FULL, and how many there are */
    need = 0;
    CHECK(gptps_stats_snapshot(s, NULL, NULL, &rows[0].r, sizeof rows[0], 2, &need) == GPTPS_E_FULL && need == 5);
    need = 0;
    CHECK(gptps_stats_snapshot(s, NULL, NULL, NULL, 0, 0, &need) == GPTPS_E_FULL && need == 5);
    CHECK(gptps_stats_snapshot(s, NULL, NULL, &rows[0].r, sizeof(gptps_stats_row) - 1, 2, &need) == GPTPS_E_INVAL);
    CHECK(gptps_stats_snapshot(s, NULL, NULL, NULL, sizeof(gptps_stats_row), 2, &need) == GPTPS_E_INVAL);
    CHECK(gptps_stats_snapshot(NULL, NULL, NULL, NULL, 0, 0, &need) == GPTPS_E_INVAL);
    h.struct_size = 8;
    CHECK(gptps_stats_snapshot(s, NULL, &h, NULL, 0, 0, NULL) == GPTPS_E_INVAL);
    gptps_stats_close(s);
    /* nothing seen: no rows, OK */
    s = mk(0, 0, 0, 0);
    if (s) { need = 9; CHECK(gptps_stats_snapshot(s, NULL, NULL, NULL, 0, 0, &need) == GPTPS_OK && need == 0); gptps_stats_close(s); }
}

/* 7) OOM, counted */
static gptps_stats *oom_s;
static const gptps_measure *oom_x;
static void oom_race(void) { (void)gptps_stats_measure_fold(oom_s, "m6", oom_x, 1, 5); }
static void test_oom(void)
{
    gptps_stats *s = mk(0, 4, 64, 2);
    gptps_stats_health_info h;
    gptps_stats_counters c;
    gptps_measure x;
    gptps_event ev;
    int i;
    CHECK(s != NULL); if (!s) return;
    log_begin();
    t_calloc_fail = 1;
    CHECK(gptps_stats_reserve(s, 32) == GPTPS_E_NOMEM);
    CHECK(log_count("no memory to grow") == 1);
    h = health(s);
    CHECK(h.nomem == 1 && h.handles_cap == 4);
    /* a read's own growth failing is counted too */
    for (i = 1; i <= 4; ++i) emit(s, GPTPS_EV_QUEUED, "q", (gptps_handle)i, 0);
    t_calloc_fail = 1;
    h = health(s);
    CHECK(health(s).nomem == 2 && health(s).handles_cap == 8);   /* the 2nd read grew it */
    log_end();
    /* the measurement pool: 2 tables; a 3rd type's values are dropped and counted */
    memset(&x, 0, sizeof x);
    x.name = "acme.n"; x.method = "acme.m"; x.unit = GPTPS_UNIT_COUNT; x.kind = GPTPS_MEASURE_TOTAL; x.value = 1;
    memset(&ev, 0, sizeof ev);
    ev.struct_size = sizeof ev; ev.kind = GPTPS_EV_SAMPLE; ev.measures = &x; ev.n_measures = 1;
    log_begin();
    {   int a0 = allocs();
        ev.task_name = "m1"; stats_observe(&ev, s);
        ev.task_name = "m2"; stats_observe(&ev, s);
        ev.task_name = "m3"; stats_observe(&ev, s); stats_observe(&ev, s);
        CHECK(allocs() == a0); }                     /* the event path took pool tables, allocated none */
    CHECK(log_count("every measurement table is taken") == 1);
    log_end();
    h = health(s);
    CHECK(h.measure_tables_cap == 2 && h.measure_tables_used == 2 && h.measure_tables_refused == 1);
    memset(&c, 0, sizeof c); c.struct_size = sizeof c;
    CHECK(gptps_stats_task(s, "m3", &c) == GPTPS_OK && c.measures_dropped == 2);
    CHECK(gptps_stats_measure_count(s, "m3") == 0 && gptps_stats_measure_count(s, "m1") == 1);
    CHECK(total(s).measures_dropped == 0);           /* the engine's rows folded every one */
    /* a host fold allocates the table it needs (outside the lock); without memory, counted */
    t_alloc_locked = 0; t_free_locked = 0;
    CHECK(gptps_stats_measure_fold(s, "m4", &x, 1, 5) == GPTPS_OK);
    CHECK(t_alloc_locked == 0 && gptps_stats_measure_count(s, "m4") == 1);
    CHECK(t_free_locked == 0);                        /* nor anything freed under it */
    /* another fold installs the table while this one allocates: the loser is freed,
     * with the lock released, and both values land */
    oom_s = s; oom_x = &x;
    t_free_locked = 0;
    t_calloc_hook = oom_race;
    CHECK(gptps_stats_measure_fold(s, "m6", &x, 1, 5) == GPTPS_OK);
    CHECK(t_calloc_hook == NULL && t_free_locked == 0);
    CHECK(gptps_stats_measure_count(s, "m6") == 1);
    memset(&c, 0, sizeof c); c.struct_size = sizeof c;
    {   gptps_stats_measure mm; memset(&mm, 0, sizeof mm); mm.struct_size = sizeof mm;
        CHECK(gptps_stats_measure_get(s, "m6", "acme.n", "acme.m", &mm) == GPTPS_OK && mm.count == 2); }
    t_now += 60000;                                   /* the earlier nomem WARN is a minute old */
    log_begin();
    t_calloc_fail = 1;
    CHECK(gptps_stats_measure_fold(s, "m5", &x, 1, 5) == GPTPS_OK);
    CHECK(log_count("no memory for task type 'm5'") == 1);
    CHECK(log_count("every measurement table is taken") == 0);   /* one loss, one WARN */
    log_end();
    h = health(s);
    CHECK(h.nomem == 3 && h.measure_tables_refused == 1);        /* counted once, as nomem */
    CHECK(gptps_stats_task(s, "m5", &c) == GPTPS_OK && c.measures_dropped == 1);
    CHECK(health(s).measure_tables_used == 4);
    gptps_stats_close(s);
    /* every allocation of construction failing in turn: NULL, nothing leaked (ASan) */
    for (i = 0; i < 5; ++i) {                         /* the object, rows, table, engine rows, pool */
        t_calloc_skip = i; t_calloc_fail = 1;
        s = mk(0, 0, 0, 0);
        CHECK(s == NULL);
        if (s) gptps_stats_close(s);
        t_calloc_skip = 0; t_calloc_fail = 0;
    }
    t_malloc_fail = 1;                                /* the name index */
    s = mk(0, 0, 0, 0);
    CHECK(s == NULL);
    if (s) gptps_stats_close(s);
    t_malloc_fail = 0;
    s = mk(0, 0, 0, 0);                               /* and with memory, fine */
    CHECK(s != NULL);
    gptps_stats_close(s);
}

/* 8) WARN rate limit: once a minute per condition */
static void test_warn_rate(void)
{
    gptps_stats *s = mk(1, 1, 1, 0);
    int i;
    uint64_t t0;
    CHECK(s != NULL); if (!s) return;
    emit(s, GPTPS_EV_QUEUED, "a", 1, 0);              /* the one live entry */
    t0 = t_now = 1000;
    log_begin();
    for (i = 0; i < 1000; ++i) { t_now = t0 + (uint64_t)i * 59; emit(s, GPTPS_EV_QUEUED, "a", (gptps_handle)(i + 2), 0); }   /* 0 .. 58941 ms */
    CHECK(log_count("handle table is full") == 1);
    t_now = t0 + 60000;
    emit(s, GPTPS_EV_QUEUED, "a", 5000, 0);
    CHECK(log_count("handle table is full") == 2);
    emit(s, GPTPS_EV_QUEUED, "b", 5001, 0);           /* another condition speaks on its own */
    CHECK(log_count("has no row of its own") == 1);
    emit(s, GPTPS_EV_QUEUED, "c", 5002, 0);
    CHECK(log_count("has no row of its own") == 1);
    log_end();
    CHECK(health(s).handles_untracked == 1003);
    gptps_stats_close(s);
    /* two clocks mixed - a host folding with wall-clock stamps, events with the engine's
     * monotonic ones, both jumping about - still speak once a minute */
    s = mk(1, 0, 0, 0);
    CHECK(s != NULL); if (!s) return;
    life(s, "only", 1, 0);                            /* the one row */
    t_now += 1;
    log_begin();
    for (i = 0; i < 20; ++i) {
        gptps_measure x; char nm[16];
        memset(&x, 0, sizeof x);
        x.name = "acme.n"; x.method = "acme.m"; x.unit = GPTPS_UNIT_COUNT; x.kind = GPTPS_MEASURE_TOTAL; x.value = 1;
        snprintf(nm, sizeof nm, "h%d", i);
        (void)gptps_stats_measure_fold(s, nm, &x, 1, 1700000000000ULL + (uint64_t)i);
        snprintf(nm, sizeof nm, "e%d", i);
        emit(s, GPTPS_EV_QUEUED, nm, (gptps_handle)(100 + i), 200 + (uint64_t)i);
        t_now += 10;                                  /* 400 ms of the one clock in all */
    }
    CHECK(log_count("has no row of its own") == 1);
    log_end();
    CHECK(health(s).types_folded == 40);
    gptps_stats_close(s);
}

/* 9) on a real engine */
static int runs;
static gptps_status t_ok(gptps_ctx *c, void *u) { (void)c; (void)u; ++runs; return GPTPS_OK; }
static void test_engine(void)
{
    gptps *e = NULL; gptps_config cfg; gptps_task_def d; gptps_stats_config sc;
    gptps_stats *s; gptps_handle h; size_t ran; int i;
    gptps_stats_counters c;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.mode = GPTPS_RUN_MANUAL;
    cfg.limits.struct_size = sizeof cfg.limits; cfg.limits.max_concurrent_tasks = 1; cfg.limits.max_memory_bytes = 1024;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK && e);
    if (!e) return;
    memset(&sc, 0, sizeof sc);
    sc.struct_size = sizeof sc; sc.flags = GPTPS_STATS_START_DISABLED; sc.handles = 8;
    s = gptps_stats_install_ex(e, &sc);
    CHECK(s != NULL && gptps_stats_enabled(s) == 0);
    if (!s) { gptps_shutdown(e); return; }
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "ok"; d.run = t_ok; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = 1;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    for (i = 0; i < 3; ++i) CHECK(gptps_submit(e, "ok", NULL, 0, &h) == GPTPS_OK);
    for (i = 0; i < 3; ++i) { ran = 0; CHECK(gptps_step(e, &ran) == GPTPS_OK); }
    c = total(s);
    CHECK(c.queued == 0 && c.finished == 0);          /* off: nothing counted */
    log_begin();
    CHECK(gptps_stats_set_enabled(s, 1) == GPTPS_OK);
    log_end();
    for (i = 0; i < 2; ++i) CHECK(gptps_submit(e, "ok", NULL, 0, &h) == GPTPS_OK);
    c = total(s);
    CHECK(c.queued == 2 && c.pending == 2);
    for (i = 0; i < 2; ++i) { ran = 0; CHECK(gptps_step(e, &ran) == GPTPS_OK); }
    c = total(s);
    CHECK(c.finished == 2 && c.pending == 0 && c.in_flight == 0 && c.wait_samples == 2);
    CHECK(health(s).resumes == 1 && health(s).handles_cap == 8);
    CHECK(gptps_stats_install_ex(NULL, NULL) == NULL);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    gptps_stats_close(s);
    CHECK(runs == 5);
}

/* 9b) a burst past the table on a real engine, before any read: the handles it could
 * not take are counted once, as untracked - not again as markers dropped - and every
 * gauge comes back to zero */
static void test_burst(void)
{
    gptps *e = NULL; gptps_config cfg; gptps_task_def d; gptps_stats_config sc;
    gptps_stats *s; gptps_handle h; size_t ran; int i;
    gptps_stats_counters c;
    gptps_stats_health_info hi;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.mode = GPTPS_RUN_MANUAL;
    cfg.limits.struct_size = sizeof cfg.limits; cfg.limits.max_concurrent_tasks = 1; cfg.limits.max_memory_bytes = 1 << 20;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK && e);
    if (!e) return;
    memset(&sc, 0, sizeof sc);
    sc.struct_size = sizeof sc; sc.handles = 16; sc.handles_max = 16;
    s = gptps_stats_install_ex(e, &sc);
    CHECK(s != NULL);
    if (!s) { gptps_shutdown(e); return; }
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "ok"; d.run = t_ok; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = 1;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    for (i = 0; i < 200; ++i) CHECK(gptps_submit(e, "ok", NULL, 0, &h) == GPTPS_OK);
    c = total(s);
    CHECK(c.queued == 200 && c.pending == 16);       /* the table holds 16: documented undercount */
    for (i = 0; i < 400; ++i) { ran = 0; if (gptps_step(e, &ran) != GPTPS_OK) break; }
    c = total(s);
    CHECK(c.finished == 200 && c.pending == 0 && c.in_flight == 0);
    CHECK(c.wait_samples == 16 && c.run_samples == 16);
    hi = health(s);
    CHECK(hi.handles_untracked == 184 && hi.orphans_dropped == 0 && hi.markers == 0 && hi.handles_live == 0);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    gptps_stats_close(s);
}

/* 11) late events */
static void test_late(void)
{
    gptps_stats *s = mk(0, 4, 4, 0);
    gptps_stats_health_info h;
    gptps_stats_counters c;
    int i;
    CHECK(s != NULL); if (!s) return;
    /* handle 1's FINISHED outran its QUEUED: a marker waits for it ... */
    emit(s, GPTPS_EV_STARTED, "t", 1, 1); emit(s, GPTPS_EV_FINISHED, "t", 1, 2);
    /* ... and 64 more markers push it out of the FIFO */
    for (i = 0; i < GPTPS_STATS_ORPHANS; ++i) {
        emit(s, GPTPS_EV_STARTED, "t", (gptps_handle)(100 + i), 3);
        emit(s, GPTPS_EV_FINISHED, "t", (gptps_handle)(100 + i), 4);
    }
    CHECK(health(s).orphans_dropped == 1);
    emit(s, GPTPS_EV_QUEUED, "t", 1, 0);              /* the late QUEUED still comes */
    c = total(s); h = health(s);
    CHECK(c.queued == 1 && c.pending == 0);          /* counted, and no gauge stuck */
    CHECK(h.handles_live == 0 && h.late_absorbed == 1);
    /* the same for a late RETRIED owed after a cancel (attempt 2 cancelled before its
     * RETRIED arrived), whose marker is dropped too */
    emit(s, GPTPS_EV_QUEUED, "t", 2, 0);
    emit_a(s, GPTPS_EV_STARTED, "t", 2, GPTPS_OK, 1, 1);
    emit_a(s, GPTPS_EV_FAILED, "t", 2, GPTPS_E_IO, 2, 1);
    emit_a(s, GPTPS_EV_FAILED, "t", 2, GPTPS_E_CANCELLED, 3, 2);
    CHECK(health(s).markers == GPTPS_STATS_ORPHANS);
    for (i = 0; i < GPTPS_STATS_ORPHANS; ++i) {
        emit(s, GPTPS_EV_STARTED, "t", (gptps_handle)(200 + i), 3);
        emit(s, GPTPS_EV_FINISHED, "t", (gptps_handle)(200 + i), 4);
    }
    emit_a(s, GPTPS_EV_RETRIED, "t", 2, GPTPS_OK, 5, 2);
    c = total(s); h = health(s);
    CHECK(c.pending == 0 && c.in_flight == 0 && h.handles_live == 0 && h.late_absorbed == 2);
    gptps_stats_close(s);

    /* the table refused a handle's QUEUED; it found room later, from STARTED on, and
     * its late events stay out of the table */
    s = mk(0, 2, 2, 0);
    CHECK(s != NULL); if (!s) return;
    for (i = 1; i <= 5; ++i) emit(s, GPTPS_EV_QUEUED, "q", (gptps_handle)i, 0);    /* 3..5 refused */
    for (i = 1; i <= 5; ++i) { emit(s, GPTPS_EV_STARTED, "q", (gptps_handle)i, 1); emit(s, GPTPS_EV_FINISHED, "q", (gptps_handle)i, 2); }
    h = health(s);
    CHECK(h.handles_untracked == 3 && h.markers == 0 && h.handles_live == 0);
    /* a handle inside the refused span whose QUEUED was merely late: its STARTED and
     * FINISHED are not tracked, and its QUEUED, when it comes, is absorbed */
    emit(s, GPTPS_EV_QUEUED, "q", 9, 0);              /* refused: the span is now 3..9 */
    emit(s, GPTPS_EV_QUEUED, "q", 10, 0); emit(s, GPTPS_EV_QUEUED, "q", 11, 0);
    emit(s, GPTPS_EV_STARTED, "q", 7, 1); emit(s, GPTPS_EV_FINISHED, "q", 7, 2);
    for (i = 9; i <= 11; ++i) { emit(s, GPTPS_EV_STARTED, "q", (gptps_handle)i, 1); emit(s, GPTPS_EV_FINISHED, "q", (gptps_handle)i, 2); }
    emit(s, GPTPS_EV_QUEUED, "q", 7, 3);
    c = total(s); h = health(s);
    CHECK(c.pending == 0 && c.in_flight == 0 && h.handles_live == 0 && h.markers == 0);
    CHECK(h.late_absorbed == 1);
    gptps_stats_close(s);

    /* reviving a marker needs the room a new entry would */
    s = mk(0, 2, 2, 0);
    CHECK(s != NULL); if (!s) return;
    emit(s, GPTPS_EV_STARTED, "svc", 100, 1); emit(s, GPTPS_EV_FINISHED, "svc", 100, 2);   /* a marker */
    emit(s, GPTPS_EV_QUEUED, "q", 1, 3); emit(s, GPTPS_EV_QUEUED, "q", 2, 3);             /* table full */
    emit(s, GPTPS_EV_STARTED, "svc", 100, 4);         /* the service's next run */
    h = health(s);
    CHECK(h.handles_untracked == 1 && h.handles_live <= h.handles_cap && h.handles_live == 2);
    emit(s, GPTPS_EV_FINISHED, "svc", 100, 5);
    c = total(s);
    CHECK(c.in_flight == 0 && c.pending == 2);
    gptps_stats_close(s);
}

/* 10) a storm (TSan): emitters on several threads, a reader that snapshots, grows and
 * switches the observer off and on. Each round reads the engine clock: when the switch
 * is off the observer takes no lock, and a loop with no HAL or pthread call is one the
 * simulation HAL cannot switch away from (docs/HAL.md). */
#if !defined(_WIN32)
static gptps_stats *storm_s;
static int storm_stop;
static int storm_warns;
static void quiet_sink(gptps_log_level lvl, const char *msg, void *ud)
{ (void)lvl; (void)msg; (void)ud; __atomic_add_fetch(&storm_warns, 1, __ATOMIC_SEQ_CST); }
static void *storm_emit(void *arg)
{
    gptps_handle base = (gptps_handle)(intptr_t)arg * 1000000u, n = 0;
    char nm[8];
    while (!__atomic_load_n(&storm_stop, __ATOMIC_ACQUIRE)) {
        uint64_t t = gptps_now_ms(NULL);
        gptps_handle h = base + ++n;
        snprintf(nm, sizeof nm, "w%u", (unsigned)(n % 12));
        emit(storm_s, GPTPS_EV_QUEUED, nm, h, t);
        emit(storm_s, GPTPS_EV_STARTED, nm, h, t);
        if (n % 5 == 0) emit_a(storm_s, GPTPS_EV_FAILED, nm, h, GPTPS_E_CANCELLED, t, 1);
        else emit(storm_s, GPTPS_EV_FINISHED, nm, h, t);
    }
    return NULL;
}
static void *storm_read(void *arg)
{
    gptps_stats_row rows[16];
    int i = 0;
    (void)arg;
    while (!__atomic_load_n(&storm_stop, __ATOMIC_ACQUIRE)) {
        gptps_stats_counters c; gptps_stats_health_info h; size_t need;
        memset(&c, 0, sizeof c); c.struct_size = sizeof c;
        memset(&h, 0, sizeof h); h.struct_size = sizeof h;
        (void)gptps_stats_snapshot(storm_s, &c, &h, rows, sizeof rows[0], 16, &need);
        if (++i % 7 == 0) (void)gptps_stats_reserve(storm_s, (size_t)(i % 200) + 4);
        if (i % 11 == 0) { (void)gptps_stats_set_enabled(storm_s, 0); (void)gptps_stats_set_enabled(storm_s, 1); }
    }
    return NULL;
}
static void test_storm(void)
{
    pthread_t th[5];
    int i, made = 0;
    uint64_t until;
    storm_s = mk(8, 4, 256, 0);
    CHECK(storm_s != NULL); if (!storm_s) return;
    gptps_set_log_sink(quiet_sink, NULL);             /* the switch WARNs; keep them off stderr */
    __atomic_store_n(&storm_stop, 0, __ATOMIC_RELEASE);
    for (i = 0; i < 4; ++i) if (pthread_create(&th[made], NULL, storm_emit, (void *)(intptr_t)(i + 1)) == 0) ++made;
    if (pthread_create(&th[made], NULL, storm_read, NULL) == 0) ++made;
    until = gptps_now_ms(NULL) + 500;
    while (gptps_now_ms(NULL) < until) { gptps_stats_health_info h; memset(&h, 0, sizeof h); h.struct_size = sizeof h; (void)gptps_stats_health(storm_s, &h); }
    __atomic_store_n(&storm_stop, 1, __ATOMIC_RELEASE);
    for (i = 0; i < made; ++i) pthread_join(th[i], NULL);
    log_end();
    CHECK(made == 5);
    {   gptps_stats_counters c = total(storm_s);
        gptps_stats_health_info h = health(storm_s);
        /* every handle ended: whatever was tracked came back to zero */
        CHECK(c.pending == 0 && c.in_flight == 0);
        CHECK(h.handles_live == 0 && h.markers <= GPTPS_STATS_ORPHANS); }
    gptps_stats_close(storm_s);
}
#endif

int main(void)
{
    log_end();                                        /* WARNs not under test stay off stderr */
    test_switch();
    test_task_index();
    test_orphans();
    test_bounded_table();
    test_struct_size();
    test_snapshot();
    test_oom();
    test_warn_rate();
    test_engine();
    test_burst();
    test_late();
#if !defined(_WIN32)
    test_storm();
#else
    printf("skip: the storm test (POSIX threads)\n");
#endif
    if (fails) { printf("%d stats unit check(s) FAILED\n", fails); return 1; }
    printf("all stats unit checks passed\n");
    return 0;
}
