/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_counts_perf.c - gptps_task_get_info and gptps_engine_counts stay O(1) in
 * queue depth (ABI 2.9).
 *
 * Before 2.9 each gptps_task_get_info call counted its type's items by walking six
 * queues under the engine lock: 116 ms a call at 64 types and 100k queued, stalling
 * the dispatcher and every submitter while a dashboard refreshed. The counts are now
 * kept as items move (see "per-type counts" in src/engine.c).
 *
 * A complexity gate like tests/test_cancel_perf.c: it asserts on a RATIO, never on a
 * rate. The large case is 64 types with 100k items queued (MANUAL, never stepped, so
 * all of it stays in intake), read for the LAST type's row - the registry is a list,
 * so the row lookup is O(types) and the last row pays all of it. The small case is
 * one type with 64 items queued. Kept counts give a ratio of a few (the 64-entry
 * list); the 1.11 walk gave ~7000 (1 ms a call). The bar sits at 25. The time per
 * call is printed too: ~0.1 us here, against the < 50 us the change was asked for.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "gptps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#  include <windows.h>
#else
#  include <time.h>
#endif

static double now_us(void)
{
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / (double)f.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
#endif
}

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

#define MAX_RATIO 25.0
#define BIG_TYPES 64
#define BIG_ITEMS 100000

static gptps_status noop(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }

static gptps *build(int types, int items)
{
    gptps_config cfg;
    gptps *e = NULL;
    int t, i;
    memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;
    cfg.mode = GPTPS_RUN_MANUAL;
    if (gptps_open_ex(&cfg, &e) != GPTPS_OK || !e) return NULL;
    for (t = 0; t < types; ++t) {
        gptps_task_def d;
        char nm[16];
        snprintf(nm, sizeof nm, "t%d", t);
        memset(&d, 0, sizeof d); d.struct_size = sizeof d;
        d.name = nm; d.run = noop; d.exec = GPTPS_EXEC_INPROC;
        d.default_cost.struct_size = sizeof d.default_cost;
        d.default_policy.struct_size = sizeof d.default_policy;
        if (gptps_register_task(e, &d) != GPTPS_OK) { gptps_shutdown(e); return NULL; }
    }
    for (i = 0; i < items; ++i) {
        char nm[16];
        snprintf(nm, sizeof nm, "t%d", i % types);
        if (gptps_submit(e, nm, NULL, 0, NULL) != GPTPS_OK) { gptps_shutdown(e); return NULL; }
    }
    return e;
}

/* us per call: `calls` reads of the last row, then as many engine_counts */
static double per_call(gptps *e, int types, int calls, double *totals_us)
{
    gptps_task_info ti;
    gptps_engine_totals t;
    double t0, t1, t2;
    int i;
    memset(&ti, 0, sizeof ti); ti.struct_size = sizeof ti;
    memset(&t, 0, sizeof t); t.struct_size = sizeof t;
    t0 = now_us();
    for (i = 0; i < calls; ++i)
        if (gptps_task_get_info(e, (size_t)(types - 1), &ti) != GPTPS_OK) { ++fails; break; }
    t1 = now_us();
    for (i = 0; i < calls; ++i)
        if (gptps_engine_counts(e, &t) != GPTPS_OK) { ++fails; break; }
    t2 = now_us();
    *totals_us = (t2 - t1) / calls;
    return (t1 - t0) / calls;
}

int main(void)
{
    gptps *big = build(BIG_TYPES, BIG_ITEMS), *small = build(1, 64);
    double a, b, ta, tb, ratio, tratio;
    int calls = 2000, i;
    if (!big || !small) { printf("test_counts_perf: FAILED (engine error)\n"); return 1; }

    {   /* the values themselves, once: the last type has its share of the queue */
        gptps_task_info ti; gptps_engine_totals t;
        memset(&ti, 0, sizeof ti); ti.struct_size = sizeof ti;
        memset(&t, 0, sizeof t); t.struct_size = sizeof t;
        CHECK(gptps_task_get_info(big, BIG_TYPES - 1, &ti) == GPTPS_OK);
        {   /* t<k> got every item i with i % 64 == k (the row order is the registry's) */
            int k = atoi(ti.name_copy + 1);
            uint32_t want = (uint32_t)(BIG_ITEMS / BIG_TYPES + (k < BIG_ITEMS % BIG_TYPES ? 1 : 0));
            CHECK(ti.name_copy[0] == 't' && ti.queued == want && ti.running == 0 && ti.dead == 0);
        }
        CHECK(gptps_engine_counts(big, &t) == GPTPS_OK);
        CHECK(t.queued == BIG_ITEMS && t.delayed == 0 && t.running == 0 && t.dead == 0);
    }

    /* Best of three per size: a stalled run on a shared machine can push one pass up,
     * but a walk is slow in EVERY run. */
    b = per_call(big, BIG_TYPES, calls, &tb);
    a = per_call(small, 1, calls, &ta);
    for (i = 0; i < 2 && !fails; ++i) {
        double ta2, tb2;
        double b2 = per_call(big, BIG_TYPES, calls, &tb2), a2 = per_call(small, 1, calls, &ta2);
        if (b2 < b) b = b2;
        if (a2 < a) a = a2;
        if (tb2 < tb) tb = tb2;
        if (ta2 < ta) ta = ta2;
    }
    /* the clock's resolution floor: a call well under a microsecond reads as noise */
    if (a < 0.01) a = 0.01;
    if (ta < 0.01) ta = 0.01;
    ratio = b / a; tratio = tb / ta;
    /* Confirm before failing: over the bar, measure again, up to six more pairs,
     * keeping the best of each; a stall does not repeat, a walk does. */
    for (i = 0; i < 6 && (ratio >= MAX_RATIO || tratio >= MAX_RATIO) && !fails; ++i) {
        double ta2, tb2;
        double b2 = per_call(big, BIG_TYPES, calls, &tb2), a2 = per_call(small, 1, calls, &ta2);
        printf("  over the bar (%.1f / %.1f): measuring again - %.3fus / %.3fus\n", ratio, tratio, b2, a2);
        if (b2 < b) b = b2;
        if (a2 < a) a = a2;
        if (tb2 < tb) tb = tb2;
        if (ta2 < ta) ta = ta2;
        if (a < 0.01) a = 0.01;
        if (ta < 0.01) ta = 0.01;
        ratio = b / a; tratio = tb / ta;
    }
    printf("get_info: %d types x %d queued %.3fus/call | 1 type x 64 %.3fus/call | ratio %.1f (bar %.0f; the 1.11 walk ~7000)\n",
           BIG_TYPES, BIG_ITEMS, b, a, ratio, MAX_RATIO);
    printf("engine_counts: %.3fus/call | %.3fus/call | ratio %.1f\n", tb, ta, tratio);
    CHECK(ratio < MAX_RATIO);
    CHECK(tratio < MAX_RATIO);
    if (ratio >= MAX_RATIO)
        printf("       gptps_task_get_info looks O(queue depth) - see \"per-type counts\" in src/engine.c\n");

    gptps_shutdown(big);
    gptps_shutdown(small);
    printf("test_counts_perf: %s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
