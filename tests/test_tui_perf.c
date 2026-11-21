/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_tui_perf.c - the dashboard's event path has no O(window), O(types) or O(tail)
 * term (1.11). A complexity gate like tests/test_cancel_perf.c: it asserts on RATIOS
 * between two sizes of the same workload, never on nanoseconds, and confirms before it
 * fails. The add-on is compiled in, and its observer is called directly on one thread
 * (no engine dispatch, no contention: the per-event work alone).
 *
 *   1. FULL, latency window 1<<20 vs 1024, the same 512 handles in flight: <= 2x. The
 *      1.10 ring scanned its window for every FINISHED (89 ns at 1024, 4.5 us at 65536).
 *   2. NORMAL, 1000 task types vs 1, the same events: <= 1.5x. The 1.10 lookup compared
 *      the name with every row (33 -> 250 ns an event from 1 to 64 types).
 *   3. A FAILED with a 64 KiB stderr tail holding no newline vs one with no tail: <= 3x.
 *      The 1.10 "why" walked the whole tail looking for a line start (28 us measured).
 *
 * Caches are part of the honest cost: the big table is 64 MiB, and with each new handle
 * hashed to a line of its own it read 2.8-3x here from cache and TLB misses alone (the
 * probe count was flat) - which is why handles are hashed by blocks of 8 (trk_home), so
 * the ones submitted together share lines. The big table's pages are touched before
 * timing so a page fault is not counted as a probe. Measured on a 4-core Linux box:
 * 1.1x, 1.1x and 1.8-2.0x (2.6x under Wine); a linear name lookup reads about 25x, an
 * unbounded tail scan about 90x.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "../addons/gptps_tui.c"
#include <stdio.h>
#if !defined(_WIN32)
#  include <time.h>
#endif

static double now_ms(void)
{
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
#endif
}

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static gptps *eng[3];
static int neng;

static gptps_tui *make(int kpi, int window, int max_tasks)
{
    gptps_tui_config c;
    gptps_tui *t;
    memset(&c, 0, sizeof c);
    c.struct_size = sizeof c; c.kpi = kpi; c.latency_window = window; c.max_tasks = max_tasks;
    c.color = 0; c.interactive = 0;
    t = neng < 3 ? gptps_tui_install(eng[neng++], &c) : NULL;   /* one engine each: its own tui.* keys */
    if (t && t->tab) memset(t->tab, 0, (t->tab_mask + 1) * sizeof *t->tab);   /* fault the pages in */
    return t;
}

static gptps_event mk(int kind, const char *name, gptps_handle h)
{
    gptps_event e;
    memset(&e, 0, sizeof e);
    e.struct_size = sizeof e; e.kind = (gptps_event_kind)kind; e.task_name = name; e.handle = h;
    e.ts_ms = (uint64_t)h; e.attempt = kind == GPTPS_EV_QUEUED ? 0 : 1;
    return e;
}

/* 1: a sliding window of 512 handles in flight: QUEUED h, STARTED h-256, FINISHED h-512 */
static double run_window(gptps_tui *t, long n)
{
    gptps_event q = mk(GPTPS_EV_QUEUED, "w", 0), s = mk(GPTPS_EV_STARTED, "w", 0), f = mk(GPTPS_EV_FINISHED, "w", 0);
    static gptps_handle base = 1000;
    gptps_handle first = base, last = base + (gptps_handle)n - 1, h;
    long i;
    double t0 = now_ms(), took;
    for (i = 0; i < n; ++i) {
        h = first + (gptps_handle)i;
        q.handle = h; tui_on_event(&q, t);
        if (i >= 256) { s.handle = h - 256; tui_on_event(&s, t); }
        if (i >= 512) { f.handle = h - 512; tui_on_event(&f, t); }
    }
    took = now_ms() - t0;
    for (h = last - 511; h <= last; ++h) {         /* drain the window, untimed */
        if (h > last - 256) { s.handle = h; tui_on_event(&s, t); }
        f.handle = h; tui_on_event(&f, t);
    }
    base = last + 1000;                            /* fresh handles next time */
    return took;
}

/* 2: the same events over `types` names */
static char names[1000][16];
static double run_types(gptps_tui *t, long n, int types)
{
    gptps_event e = mk(GPTPS_EV_STARTED, names[0], 1);
    long i;
    double t0 = now_ms();
    for (i = 0; i < n; ++i) {
        e.task_name = names[types > 1 ? (int)(i % types) : 0];
        e.kind = (i & 1) ? GPTPS_EV_FINISHED : GPTPS_EV_STARTED;
        e.handle = (gptps_handle)(i + 1);
        tui_on_event(&e, t);
    }
    return now_ms() - t0;
}

/* 3: FAILED, with or without a 64 KiB tail that has no newline */
static char bigtail[65536];
static double run_failed(gptps_tui *t, long n, int with_tail)
{
    gptps_event e = mk(GPTPS_EV_FAILED, "f", 1);
    long i;
    double t0 = now_ms();
    e.status = GPTPS_E_TASK;
    if (with_tail) { e.stderr_tail = bigtail; e.stderr_tail_len = sizeof bigtail; }
    for (i = 0; i < n; ++i) { e.handle = (gptps_handle)(i + 1); tui_on_event(&e, t); }
    return now_ms() - t0;
}

typedef double (*runner)(gptps_tui *, long, int);
static gptps_tui *A, *Bt, *C;
static int which;
static double r_window(gptps_tui *t, long n, int unused) { (void)unused; return run_window(t, n); }
static double r_types(gptps_tui *t, long n, int types) { return run_types(t, n, types); }
static double r_failed(gptps_tui *t, long n, int tail) { return run_failed(t, n, tail); }

/* Best of three, then up to six more pairs while over the bar (a stall does not repeat;
 * a scan is slow in every run). Returns the ratio b / a. */
static double gate(const char *what, runner f, gptps_tui *ta, int xa, gptps_tui *tb, int xb, double bar)
{
    long n = 20000;
    double a, b, ratio;
    int i;
    for (;;) {                                     /* grow n until the cheap side takes 100 ms */
        a = f(ta, n, xa);
        if (a >= 100.0 || n >= (1L << 24)) break;
        n *= 2;
    }
    b = f(tb, n, xb);
    for (i = 0; i < 2; ++i) {
        double a2 = f(ta, n, xa), b2 = f(tb, n, xb);
        if (a2 < a) a = a2;
        if (b2 < b) b = b2;
    }
    ratio = a > 0.0 ? b / a : 0.0;
    for (i = 0; i < 6 && ratio > bar; ++i) {
        double a2 = f(ta, n, xa), b2 = f(tb, n, xb);
        printf("  %s over the bar (%.2f): measuring again - %.1fms / %.1fms\n", what, ratio, a2, b2);
        if (a2 < a) a = a2;
        if (b2 < b) b = b2;
        ratio = a > 0.0 ? b / a : 0.0;
    }
    printf("%s: %ld x: %.1fms vs %.1fms, ratio %.2f (bar %.1f)\n", what, n, a, b, ratio, bar);
    (void)which;
    return ratio;
}

int main(void)
{
    gptps_config cfg;
    int i;
    double r;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1; cfg.mode = GPTPS_RUN_MANUAL;
    for (i = 0; i < 3; ++i) if (gptps_open_ex(&cfg, &eng[i]) != GPTPS_OK || !eng[i]) { printf("FAIL open\n"); return 1; }
    for (i = 0; i < 1000; ++i) snprintf(names[i], sizeof names[i], "type_%04d", i);
    memset(bigtail, 'x', sizeof bigtail);

    A = make(GPTPS_TUI_KPI_FULL, 1024, 0); Bt = make(GPTPS_TUI_KPI_FULL, TUI_LAT_CAP, 0);
    C = make(GPTPS_TUI_KPI_NORMAL, 0, 1000);
    CHECK(A && Bt && A->tab && Bt->tab && C && C->tasks);
    if (A && Bt && A->tab && Bt->tab) {
        r = gate("FULL window 1<<20 vs 1024", r_window, A, 0, Bt, 0, 2.0);
        CHECK(r <= 2.0);
        CHECK(A->h_untracked == 0 && Bt->h_untracked == 0);
        CHECK(A->tab_n - A->tab_done == 0 && Bt->tab_n - Bt->tab_done == 0 && A->g_run == 0 && A->g_pend == 0);
    }
    if (C && C->tasks) {
        r = gate("NORMAL 1000 types vs 1", r_types, C, 1, C, 1000, 1.5);
        CHECK(r <= 1.5);
        CHECK(C->ntasks == 1001 && C->nfold == 0);
        r = gate("FAILED 64 KiB newline-free tail vs none", r_failed, C, 0, C, 1, 3.0);
        CHECK(r <= 3.0);
    }
    for (i = 0; i < 3; ++i) gptps_shutdown(eng[i]);   /* close AFTER shutdown */
    gptps_tui_close(A); gptps_tui_close(Bt); gptps_tui_close(C);
    printf("test_tui_perf: %s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
