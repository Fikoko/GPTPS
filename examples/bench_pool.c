/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * bench_pool.c - empirical proof of scale-UP by composition.
 *
 * A single engine is a single-writer design: one lock, one dispatcher. That is the
 * per-node throughput ceiling. The gptps_pool add-on runs N independent engines and
 * routes across them, so N shards are N independent lock+dispatcher pairs. This
 * benchmark hammers tiny (dispatch/lock-bound) tasks from several producer threads
 * and reports aggregate throughput at 1, 2, 4, 8 shards - so you can watch the single
 * shard hit the ceiling and composition break past it. Nothing here touches the core.
 *
 * Pass an item count as argv[1] for a longer run (default is a quick, CI-friendly one).
 * Numbers are machine- and load-dependent; the SHAPE (throughput rising with shards)
 * is the point, not the absolute figures. POSIX (spawns producer pthreads).
 */
/* feature-test macros before any header (match hal_posix.c): expose clock_gettime,
 * _SC_NPROCESSORS_ONLN, and pthreads under -std=c99 on each platform. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#  define _DARWIN_C_SOURCE
#endif
#include "gptps_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define PRODUCERS 8

static int g_done;
static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }

static gptps_status tiny(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }
static void obs(const gptps_event *ev, void *ud) { (void)ud; if (ev->kind == GPTPS_EV_FINISHED) inc(&g_done); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec / 1e9; }

static gptps_pool *g_pool;
static int g_per_producer;

static void *producer(void *a)
{
    int i; (void)a;
    for (i = 0; i < g_per_producer; ++i)
        while (gptps_pool_submit(g_pool, "t", NULL, 0, NULL) == GPTPS_E_FULL) { /* backpressure */ }
    return NULL;
}

static double run_shards(size_t nshards, int total)
{
    gptps_config cfg;
    pthread_t th[PRODUCERS];
    size_t i;
    int p;
    double t0;

    memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 2;
    cfg.limits.max_intake_depth = 8192;
    g_pool = gptps_pool_open(nshards, &cfg);
    if (!g_pool) return 0.0;
    {
        gptps_task_def d; memset(&d, 0, sizeof d);
        d.struct_size = sizeof d; d.name = "t"; d.run = tiny; d.exec = GPTPS_EXEC_INPROC;
        d.default_cost.struct_size = sizeof d.default_cost;
        d.default_policy.struct_size = sizeof d.default_policy;
        gptps_pool_register_task(g_pool, &d);
    }
    for (i = 0; i < nshards; ++i) gptps_register_observer(gptps_pool_shard(g_pool, i), obs, NULL);

    __atomic_store_n(&g_done, 0, __ATOMIC_SEQ_CST);
    g_per_producer = total / PRODUCERS;
    t0 = now();
    for (p = 0; p < PRODUCERS; ++p) pthread_create(&th[p], NULL, producer, NULL);
    for (p = 0; p < PRODUCERS; ++p) pthread_join(th[p], NULL);
    while (get(&g_done) < g_per_producer * PRODUCERS) { }
    {
        double dt = now() - t0;
        gptps_pool_close(g_pool);
        return dt > 0 ? (double)(g_per_producer * PRODUCERS) / dt : 0.0;
    }
}

int main(int argc, char **argv)
{
    size_t shards[] = { 1, 2, 4, 8 };
    int total = (argc > 1) ? atoi(argv[1]) : 40000;   /* CI-quick default */
    int i;
    double base = 0.0;

    total = (total / PRODUCERS) * PRODUCERS;           /* even split */
    printf("pool scale-up: %ld cores, %d tiny items x %d producers\n",
           sysconf(_SC_NPROCESSORS_ONLN), total, PRODUCERS);
    for (i = 0; i < 4; ++i) {
        double tp = run_shards(shards[i], total);
        if (i == 0) base = tp;
        printf("  %zu shard(s): %9.0f items/sec   %5.2fx\n",
               shards[i], tp, base > 0 ? tp / base : 0.0);
    }
    return 0;
}
