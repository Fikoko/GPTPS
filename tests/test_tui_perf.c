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
 *   3. A FAILED with a 64 KiB stderr tail holding no newline vs one with a 512-byte tail
 *      (TUI_WHY_SCAN, the window the "why" looks at): <= 2x. The 1.10 "why" walked the
 *      whole tail looking for a line start (28 us measured). Both sides scan the same
 *      bounded window, so the ratio says O(tail) or not whatever the build; until 1.13.1
 *      the cheap side had no tail at all, which also weighed the fixed scan against the
 *      bare event - near the 3x bar in MSVC's unoptimised Debug build (3.4x there).
 *   4. The same two gates at the tiers that add those layers, LATENCY (6: the handle
 *      table) and TASKS (4: the name index), without the layers above them.
 *   5. The tasks pane on a busy engine (64 types, 20000 queued, pane_refresh_ms 100): the
 *      share of the time its passes spend in gptps_task_get_info - which holds the
 *      engine's lock (and walked every queue before ABI 2.9; O(1) since) - is <= 1.5%
 *      (the rule is 1%: a pass waits 100x what the last took). 1.10 read every row on
 *      every frame: 116 ms a frame at 64 types and 100k queued, 46% of the time at 250 ms.
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

#define NENG 5
static gptps *eng[NENG];
static int neng;

static gptps_tui *make(int kpi, int window, int max_tasks)
{
    gptps_tui_config c;
    gptps_tui *t;
    memset(&c, 0, sizeof c);
    c.struct_size = sizeof c; c.kpi = kpi; c.latency_window = window; c.max_tasks = max_tasks;
    c.color = 0; c.interactive = 0;
    t = neng < NENG ? gptps_tui_install(eng[neng++], &c) : NULL;   /* one engine each: its own tui.* keys */
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
static double run_failed(gptps_tui *t, long n, int with_tail)   /* 1: 64 KiB tail; 2: TUI_WHY_SCAN */
{
    gptps_event e = mk(GPTPS_EV_FAILED, "f", 1);
    long i;
    double t0 = now_ms();
    e.status = GPTPS_E_TASK;
    if (with_tail) { e.stderr_tail = bigtail; e.stderr_tail_len = with_tail == 2 ? (size_t)TUI_WHY_SCAN : sizeof bigtail; }
    for (i = 0; i < n; ++i) { e.handle = (gptps_handle)(i + 1); tui_on_event(&e, t); }
    return now_ms() - t0;
}

/* 5: the tasks pane on an engine with `types` types and `queued` items waiting */
static gptps_status perf_ok(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }
static gptps *busy_engine(int types, int queued)
{
    gptps_config cfg;
    gptps *e = NULL;
    int i;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1; cfg.mode = GPTPS_RUN_MANUAL;
    if (gptps_open_ex(&cfg, &e) != GPTPS_OK || !e) return NULL;
    for (i = 0; i < types; ++i) {
        gptps_task_def d;
        char nm[32];
        snprintf(nm, sizeof nm, "busy_%02d", i);
        memset(&d, 0, sizeof d);
        d.struct_size = sizeof d; d.name = nm; d.exec = GPTPS_EXEC_INPROC; d.run = perf_ok;
        d.default_cost.struct_size = sizeof d.default_cost; d.default_policy.struct_size = sizeof d.default_policy;
        if (gptps_register_task(e, &d) != GPTPS_OK) { gptps_shutdown(e); return NULL; }
    }
    for (i = 0; i < queued; ++i) {
        char nm[32];
        gptps_handle h;
        snprintf(nm, sizeof nm, "busy_%02d", i % types);
        if (gptps_submit(e, nm, NULL, 0, &h) != GPTPS_OK) { printf("note: submit %d refused\n", i); break; }
    }
    return e;
}

/* The share of `ms` of continuous rendering of the tasks pane that its passes took. */
static double pane_duty(gptps_tui *t, double ms, uint64_t *passes, long *frames)
{
    static char frame[65536];
    uint64_t us0 = t->h_pane_us, n0 = t->h_pane_reads;
    double t0 = now_ms(), took;
    long f = 0;
    gptps_tui_render(t, frame, sizeof frame);   /* lands right after a pass: the window starts there */
    us0 = t->h_pane_us; n0 = t->h_pane_reads; t0 = now_ms();
    do { gptps_tui_render(t, frame, sizeof frame); ++f; } while ((took = now_ms() - t0) < ms);
    *passes = t->h_pane_reads - n0; *frames = f;
    return (double)(t->h_pane_us - us0) / 1000.0 / took;
}

typedef double (*runner)(gptps_tui *, long, int);
static gptps_tui *A, *Bt, *C, *La, *Lb, *Tt;
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
    for (i = 0; i < NENG; ++i) if (gptps_open_ex(&cfg, &eng[i]) != GPTPS_OK || !eng[i]) { printf("FAIL open\n"); return 1; }
    for (i = 0; i < 1000; ++i) snprintf(names[i], sizeof names[i], "type_%04d", i);
    memset(bigtail, 'x', sizeof bigtail);

    A = make(GPTPS_TUI_KPI_FULL, 1024, 0); Bt = make(GPTPS_TUI_KPI_FULL, TUI_LAT_CAP, 0);
    C = make(GPTPS_TUI_KPI_NORMAL, 0, 1000);
    La = make(GPTPS_TUI_KPI_LATENCY, 1024, 0); Lb = make(GPTPS_TUI_KPI_LATENCY, TUI_LAT_CAP, 0);
    Tt = NULL;
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
        r = gate("FAILED 64 KiB newline-free tail vs 512 B", r_failed, C, 2, C, 1, 2.0);
        CHECK(r <= 2.0);
    }
    /* 4: the layers at their own tiers - LATENCY has the handle table and no histogram,
     * TASKS the name index and nothing above it (no ACTIVITY journal) */
    CHECK(La && Lb && La->tab && Lb->tab && !La->hist && La->tier == TUI_T_LATENCY);
    if (La && Lb && La->tab && Lb->tab) {
        r = gate("LATENCY window 1<<20 vs 1024", r_window, La, 0, Lb, 0, 2.0);
        CHECK(r <= 2.0);
        CHECK(La->tab_n - La->tab_done == 0 && Lb->tab_n - Lb->tab_done == 0);
    }
    if (C) {                                       /* C's engine is reused at TASKS, a new table */
        CHECK(gptps_tui_set_kpi(C, GPTPS_TUI_KPI_MINIMAL) == GPTPS_OK && !C->tasks);
        CHECK(gptps_tui_set_kpi(C, GPTPS_TUI_KPI_TASKS) == GPTPS_OK);
        Tt = C;
        CHECK(Tt->tier == TUI_T_TASKS && Tt->tasks && !Tt->act && !Tt->tab);
        if (Tt->tasks) {
            r = gate("TASKS 1000 types vs 1", r_types, Tt, 1, Tt, 1000, 1.5);
            CHECK(r <= 1.5);
            CHECK(Tt->ntasks == 1001 && Tt->nfold == 0);
        }
    }
    {   /* 5: the tasks pane's share of the engine's lock */
        gptps *be = busy_engine(64, 20000);
        gptps_tui *pt = NULL;
        CHECK(be != NULL);
        if (be) {
            gptps_tui_config c;
            memset(&c, 0, sizeof c);
            c.struct_size = sizeof c; c.kpi = GPTPS_TUI_KPI_OFF; c.color = 0; c.interactive = 0;
            c.pane_refresh_ms = 100;               /* the stretch, not this, must hold the duty */
            pt = gptps_tui_install(be, &c);
            CHECK(pt != NULL);
        }
        if (pt) {
            static char frame[65536];
            uint64_t passes = 0;
            long frames = 0;
            double duty, w, best = 1.0;
            int k;
            pt->pane = TUI_PANE_TASKS;
            gptps_tui_render(pt, frame, sizeof frame);    /* the pane opens: one pass */
            printf("tasks pane, 64 types / 20000 queued: a pass takes %.1f ms; the next waits %.1f s\n",
                   (double)pt->pane_cost_us / 1000.0, (double)pt->pane_every_ms / 1000.0);
            CHECK(pt->pane_every_ms * 1000u >= pt->pane_cost_us * 100u);
            for (k = 0; k < 3; ++k) {              /* confirm before failing: a stall does not repeat */
                w = 3.0 * (double)pt->pane_every_ms;
                if (w < 2000.0) w = 2000.0;
                if (w > 20000.0) w = 20000.0;
                duty = pane_duty(pt, w, &passes, &frames);
                printf("  %.1f s of frames: %ld frames, %llu passes, %.2f%% of the time in passes (bar 1.5%%)\n",
                       w / 1000.0, frames, (unsigned long long)passes, duty * 100.0);
                if (duty < best) best = duty;
                if (best <= 0.015) break;
            }
            CHECK(best <= 0.015);
            CHECK((uint64_t)frames > passes * 10u);    /* frames between passes read nothing */
        }
        if (be) gptps_shutdown(be);
        gptps_tui_close(pt);
    }
    for (i = 0; i < NENG; ++i) gptps_shutdown(eng[i]);   /* close AFTER shutdown */
    gptps_tui_close(A); gptps_tui_close(Bt); gptps_tui_close(C); gptps_tui_close(La); gptps_tui_close(Lb);
    printf("test_tui_perf: %s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
