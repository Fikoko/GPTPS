/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * bench_tui_stripes.c - would striping the dashboard's lock pay? (1.12; the answer, and
 * the numbers, are in docs/MEASUREMENTS.md, "Dashboard lock stripes"). POSIX, gcc/clang.
 * Not built by default: cmake --build . --target gptps_bench_tui_stripes
 *
 * The documented stripe design (stripe = mix(handle) & (S-1); counters, rows and table
 * slices per stripe; journals and the name index behind their own locks) is modelled by
 * its UPPER BOUND: S complete, independent dashboards, each with its own lock, rows,
 * handle table and journals, and a router observer that sends each event to dashboard
 * mix(handle) & (S-1). Nothing is shared between stripes, so the real design - which
 * shares the journals and the name index - can only be slower. stripes=1 goes through
 * the same router, so the router's call is in every stripe column; stripes=0 installs one
 * dashboard directly on the measured engine (the shipped configuration).
 *
 *   e2e:    a THREADED engine with N workers; every item, when it runs, submits its
 *           successor (so all three events come from the N worker threads) and the
 *           engine stays at N*8 queued. Items finished per second.
 *   direct: N threads call the router with QUEUED/STARTED/FINISHED for their own
 *           handles (512 in flight each), no engine. Events per second: the ceiling.
 *
 * usage: bench_tui_stripes e2e|direct <kpi:minimal|normal|full> <threads 1..64> <stripes 1|2|4|8> <ms>
 *        bench_tui_stripes e2e none <threads> 0 <ms>      (no dashboard at all)
 *        bench_tui_stripes e2e <kpi> <threads> 0 <ms>     (one dashboard installed directly)
 */
#define _POSIX_C_SOURCE 200809L
#include "../addons/gptps_tui.c"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>

static double now_ms(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }

#define SMAX 8
#define TMAX 64
static gptps_tui *st[SMAX];
static gptps *dummy[SMAX];
static unsigned smask;

static uint64_t mix64(uint64_t x)
{ x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33; return x; }

static void router(const gptps_event *ev, void *ud)
{
    (void)ud;
    tui_on_event(ev, st[(unsigned)mix64(ev->handle) & smask]);
}

static int kpi_of(const char *s)
{
    if (!strcmp(s, "minimal")) return GPTPS_TUI_KPI_MINIMAL;
    if (!strcmp(s, "normal")) return GPTPS_TUI_KPI_NORMAL;
    if (!strcmp(s, "full")) return GPTPS_TUI_KPI_FULL;
    return -1;
}

static gptps_tui *mk_tui(gptps *e, int kpi)
{
    gptps_tui_config c;
    memset(&c, 0, sizeof c);
    c.struct_size = sizeof c; c.kpi = kpi; c.color = 0; c.interactive = 0;
    c.latency_window = 4096;
    return gptps_tui_install(e, &c);
}

static gptps *manual_engine(void)
{
    gptps_config cfg; gptps *e = NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1; cfg.mode = GPTPS_RUN_MANUAL;
    if (gptps_open_ex(&cfg, &e) != GPTPS_OK) return NULL;
    return e;
}

static int mk_stripes(int stripes, int kpi)
{
    int i;
    for (i = 0; i < stripes; ++i) {
        if (!(dummy[i] = manual_engine())) { printf("open failed\n"); return -1; }
        if (!(st[i] = mk_tui(dummy[i], kpi))) { printf("tui install failed\n"); return -1; }
    }
    return 0;
}

/* ---- e2e ---- */
static int stop_flag;   /* __atomic_* only: written by main, read by the workers */
static gptps *E;
static long done_total;
static const char *TN[4] = { "w0", "w1", "w2", "w3" };

static gptps_status w_run(gptps_ctx *c, void *u)
{
    gptps_handle h;
    size_t i = (size_t)u;
    (void)c;
    if (!__atomic_load_n(&stop_flag, __ATOMIC_RELAXED)) gptps_submit(E, TN[(i + 1) & 3], NULL, 0, &h);
    __atomic_add_fetch(&done_total, 1, __ATOMIC_RELAXED);
    return GPTPS_OK;
}

static double e2e(const char *kpis, int threads, int stripes, int ms)
{
    gptps_config cfg;
    gptps_tui *direct = NULL;
    int i, kpi = kpi_of(kpis);
    long d0, d1;
    double t0, t1;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = (uint32_t)threads;
    if (gptps_open_ex(&cfg, &E) != GPTPS_OK) { printf("open failed\n"); return -1; }
    for (i = 0; i < 4; ++i) {
        gptps_task_def d;
        memset(&d, 0, sizeof d);
        d.struct_size = sizeof d; d.name = TN[i]; d.exec = GPTPS_EXEC_INPROC; d.run = w_run;
        d.user_data = (void *)(size_t)i;
        d.default_cost.struct_size = sizeof d.default_cost; d.default_policy.struct_size = sizeof d.default_policy;
        if (gptps_register_task(E, &d) != GPTPS_OK) { printf("register failed\n"); return -1; }
    }
    if (kpi >= 0 && stripes == 0) {
        if (!(direct = mk_tui(E, kpi))) { printf("tui install failed\n"); return -1; }
    } else if (kpi >= 0) {
        smask = (unsigned)stripes - 1;
        if (mk_stripes(stripes, kpi) != 0) return -1;
        if (gptps_register_observer(E, router, NULL) != GPTPS_OK) { printf("observer failed\n"); return -1; }
    }
    __atomic_store_n(&stop_flag, 0, __ATOMIC_RELAXED); done_total = 0;
    for (i = 0; i < threads * 8; ++i) { gptps_handle h; gptps_submit(E, TN[i & 3], NULL, 0, &h); }
    t0 = now_ms();
    while (now_ms() - t0 < 200) { struct timespec s = { 0, 10000000 }; nanosleep(&s, NULL); }   /* warm up */
    d0 = __atomic_load_n(&done_total, __ATOMIC_RELAXED); t0 = now_ms();
    while (now_ms() - t0 < ms) { struct timespec s = { 0, 10000000 }; nanosleep(&s, NULL); }
    d1 = __atomic_load_n(&done_total, __ATOMIC_RELAXED); t1 = now_ms();
    __atomic_store_n(&stop_flag, 1, __ATOMIC_RELAXED);
    gptps_shutdown(E);
    if (direct) gptps_tui_close(direct);
    if (kpi >= 0 && stripes > 0) for (i = 0; i < stripes; ++i) { gptps_tui_close(st[i]); gptps_shutdown(dummy[i]); }
    return (double)(d1 - d0) / ((t1 - t0) / 1000.0);
}

/* ---- direct ---- */
typedef struct { int id; long n; } targ;
static int go;   /* __atomic_* only: 0 wait, 1 run, 2 stop */
static void *dthread(void *p)
{
    targ *a = (targ *)p;
    gptps_event q, s, f;
    gptps_handle base = (gptps_handle)(a->id + 1) << 40, h;
    long i = 0;
    memset(&q, 0, sizeof q); q.struct_size = sizeof q; q.task_name = TN[a->id & 3];
    s = q; f = q;
    q.kind = GPTPS_EV_QUEUED; s.kind = GPTPS_EV_STARTED; f.kind = GPTPS_EV_FINISHED; s.attempt = f.attempt = 1;
    while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE)) ;
    while (__atomic_load_n(&go, __ATOMIC_RELAXED) == 1) {
        h = base + (gptps_handle)i;
        q.handle = h; q.ts_ms = (uint64_t)i; router(&q, NULL);
        if (i >= 256) { s.handle = h - 256; router(&s, NULL); }
        if (i >= 512) { f.handle = h - 512; f.ts_ms = (uint64_t)i; router(&f, NULL); }
        ++i;
    }
    a->n = i * 3;
    return NULL;
}

static double direct_run(const char *kpis, int threads, int stripes, int ms)
{
    pthread_t th[TMAX]; targ a[TMAX];
    int i, kpi = kpi_of(kpis);
    long n = 0;
    double t0, t1;
    smask = (unsigned)stripes - 1;
    if (mk_stripes(stripes, kpi) != 0) return -1;
    __atomic_store_n(&go, 0, __ATOMIC_RELAXED);
    for (i = 0; i < threads; ++i) {
        a[i].id = i; a[i].n = 0;
        if (pthread_create(&th[i], NULL, dthread, &a[i]) != 0) {
            printf("pthread_create failed\n");
            __atomic_store_n(&go, 2, __ATOMIC_RELEASE);
            while (i-- > 0) pthread_join(th[i], NULL);
            return -1;
        }
    }
    t0 = now_ms(); __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
    while (now_ms() - t0 < ms) { struct timespec s = { 0, 10000000 }; nanosleep(&s, NULL); }
    __atomic_store_n(&go, 2, __ATOMIC_RELAXED); t1 = now_ms();
    for (i = 0; i < threads; ++i) { pthread_join(th[i], NULL); n += a[i].n; }
    for (i = 0; i < stripes; ++i) { gptps_tui_close(st[i]); gptps_shutdown(dummy[i]); }
    return (double)n / ((t1 - t0) / 1000.0);
}

static int usage(const char *why)
{
    fprintf(stderr, "bench_tui_stripes: %s\n"
            "usage: bench_tui_stripes e2e|direct minimal|normal|full <threads 1..%d> <stripes 1|2|4|8> <ms>\n"
            "       bench_tui_stripes e2e none|minimal|normal|full <threads> 0 <ms>\n", why, TMAX);
    return 2;
}

/* strict decimal in [lo, hi], or -1 */
static int arg_int(const char *s, int lo, int hi)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end || v < lo || v > hi) return -1;
    return (int)v;
}

int main(int argc, char **argv)
{
    double r;
    int e2e_mode, none, threads, stripes, ms;
    if (argc != 6) return usage("expected 5 arguments");
    if (!strcmp(argv[1], "e2e")) e2e_mode = 1;
    else if (!strcmp(argv[1], "direct")) e2e_mode = 0;
    else return usage("mode must be e2e or direct");
    none = !strcmp(argv[2], "none");
    if (!none && kpi_of(argv[2]) < 0) return usage("kpi must be none, minimal, normal or full");
    if (none && !e2e_mode) return usage("kpi none is e2e only");
    if ((threads = arg_int(argv[3], 1, TMAX)) < 0) return usage("threads out of range");
    if ((stripes = arg_int(argv[4], 0, SMAX)) < 0 || (stripes & (stripes - 1)))
        return usage("stripes must be 0, 1, 2, 4 or 8");
    if (stripes == 0 && !e2e_mode) return usage("stripes 0 (installed directly) is e2e only");
    if (none && stripes != 0) return usage("kpi none takes stripes 0");
    if ((ms = arg_int(argv[5], 1, 3600000)) < 0) return usage("ms out of range");
    r = e2e_mode ? e2e(argv[2], threads, stripes, ms) : direct_run(argv[2], threads, stripes, ms);
    if (r < 0) return 1;
    printf("%s %s threads=%d stripes=%d rate=%.0f\n", argv[1], argv[2], threads, stripes, r);
    return 0;
}
