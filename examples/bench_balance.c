/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * bench_balance.c - round-robin (gptps_pool) vs late binding (gptps_balance) on a
 * HEAVY-TAILED workload: most items are short, a few are long, sizes are random
 * (fixed seed). Round-robin assigns each item to a shard as it arrives, so the long
 * ones fall where they fall and the makespan is the unluckiest shard's. The balancer
 * holds the queue and hands each shard work as it frees up, so a shard that drew a
 * long item simply receives fewer others. Both runs use the same shards, the same
 * items, the same order; the report is the wall time to drain everything, and the
 * ideal (total work / shards) for reference.
 *
 * Pass an item count as argv[1] for a longer run (default is CI-quick). Task bodies
 * SPIN for their duration (calibrated at startup), so the result only means
 * something with >= SHARDS free cores; on fewer, both runs just time-slice.
 */
#include "gptps_balance.h"
#include "gptps_await.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHARDS 4

/* gptps_now_ms is ms-granular; sub-ms work burns a loop calibrated at startup. */
static unsigned long iters_per_us = 100ul;
static void calibrate(void)
{
    volatile unsigned long x = 0; unsigned long n = 0; uint64_t t0 = gptps_now_ms(NULL);
    while (gptps_now_ms(NULL) - t0 < 100) { unsigned long i; for (i = 0; i < 10000ul; ++i) x += i; n += 10000ul; }
    iters_per_us = n / (100ul * 1000ul);
    if (iters_per_us < 1) iters_per_us = 1;
}
static void spin_us(gptps_ctx *c, unsigned us)
{
    volatile unsigned long x = 0; unsigned long i, n = (unsigned long)us * iters_per_us;
    (void)c;
    for (i = 0; i < n; ++i) x += i;
}
static gptps_status t_work(gptps_ctx *c, void *u)
{
    const unsigned *dur = (const unsigned *)gptps_payload(c, NULL); (void)u;
    spin_us(c, *dur);
    return GPTPS_OK;
}

/* heavy tail: 1 in 32 items is 100x a short one */
static unsigned dur_us(unsigned *seed)
{
    *seed = *seed * 1103515245u + 12345u;
    return ((*seed >> 16) % 32u == 0u) ? 20000u : 200u;
}

static double run(int balanced, unsigned long items, double *ideal_ms)
{
    gptps_config cfg; gptps_pool *p; gptps_balance *b = NULL; gptps_await *aw[SHARDS];
    unsigned long i, done; unsigned seed = 42u; double total_us = 0; uint64_t t0, t1;
    gptps_task_def d; size_t s;

    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;            /* one worker per shard: routing is everything */
    cfg.limits.max_memory_bytes = 1u << 20;
    p = gptps_pool_open(SHARDS, &cfg);
    if (!p) { fprintf(stderr, "pool open failed\n"); exit(1); }
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "work"; d.run = t_work; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = 1;
    d.default_policy.struct_size = sizeof d.default_policy;
    gptps_pool_register_task(p, &d);
    for (s = 0; s < SHARDS; ++s) aw[s] = gptps_await_install(gptps_pool_shard(p, s));
    if (balanced) {
        gptps_balance_config bc; memset(&bc, 0, sizeof bc); bc.struct_size = sizeof bc; bc.shard_depth = 2;
        b = gptps_balance_open(p, &bc);
        if (!b) { fprintf(stderr, "balance open failed\n"); exit(1); }
    }

    t0 = gptps_now_ms(NULL);
    for (i = 0; i < items; ++i) {
        unsigned us = dur_us(&seed);
        total_us += us;
        if (balanced) { gptps_balance_handle h; gptps_balance_submit(b, "work", &us, sizeof us, &h); }
        else          { gptps_pool_handle h;    gptps_pool_submit(p, "work", &us, sizeof us, &h); }
    }
    /* drain: every shard's terminal count, summed, reaches `items` */
    do {
        done = 0;
        for (s = 0; s < SHARDS; ++s) done += gptps_await_count(aw[s]);
        if (done < items) gptps_await_quiesce(aw[0], gptps_await_count(aw[0]) + 1, 5);
    } while (done < items);
    t1 = gptps_now_ms(NULL);

    gptps_pool_close(p);
    if (b) gptps_balance_close(b);
    for (s = 0; s < SHARDS; ++s) gptps_await_close(aw[s]);
    *ideal_ms = total_us / 1000.0 / SHARDS;
    return (double)(t1 - t0);
}

int main(int argc, char **argv)
{
    unsigned long items = (argc > 1) ? strtoul(argv[1], NULL, 10) : 4000ul;
    double ideal, rr, bal;
    calibrate();
    printf("bench_balance: %lu items, %d shards x 1 worker, heavy tail (1/32 items 100x longer)\n", items, SHARDS);
    printf("  (task bodies spin: this needs >= %d free cores to mean anything)\n", SHARDS);
    rr  = run(0, items, &ideal);
    bal = run(1, items, &ideal);
    printf("  ideal (total work / shards): %8.0f ms\n", ideal);
    printf("  round-robin  (gptps_pool):   %8.0f ms  (%.2fx ideal)\n", rr,  rr  / ideal);
    printf("  late binding (gptps_balance):%8.0f ms  (%.2fx ideal)\n", bal, bal / ideal);
    printf("  balance / round-robin:       %8.2f\n", bal / rr);
    return 0;
}
