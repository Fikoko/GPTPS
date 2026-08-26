/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_admission_perf.c - admission must stay LINEAR in queue depth.
 *
 * This is a complexity gate, not a benchmark: it asserts on the SHAPE of the curve,
 * never on an absolute rate, so it means the same thing on a fast laptop and a
 * loaded CI runner.
 *
 * The regression it exists to catch: admission used to scan the whole intake queue
 * twice for every item it admitted. limits.max_intake_depth defaults to 0
 * (unbounded, deliberately - see docs/SECURITY.md), so a producer that outran the
 * dispatcher grew the queue to O(n) and draining n items cost O(n^2). It was
 * invisible to every other test because it only appears once the queue is deep -
 * throughput was fine at 20k queued items and had fallen ~25x by 160k.
 *
 * MANUAL mode makes the measurement deterministic and thread-free: nothing is
 * admitted until gptps_step(), so every item is queued before the first admission
 * decision and the queue drains from full depth. Doubling n must roughly double the
 * time (ratio ~2); a quadratic admission path shows up as ~4.
 */
#include "gptps.h"
#include "gptps_hal.h"   /* the engine's own monotonic clock: portable, no feature macros */
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

/* Linear is 2.0 and quadratic is 4.0, so the bar sits between them - far enough
 * above 2.0 to absorb a noisy shared runner, far enough below 4.0 to fail loudly if
 * the scan ever comes back. The observed pre-fix ratio was 4.6-7.0. */
#define MAX_RATIO 3.0

static gptps_status noop(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }

/* queue `n` items, then drain them, and return the milliseconds that took */
static double drain(int n)
{
    gptps_config cfg;
    gptps *e = NULL;
    gptps_task_def d;
    uint64_t t0;
    size_t ran;
    int i;

    memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;
    cfg.limits.max_intake_depth = 0;          /* the default: unbounded intake */
    cfg.mode = GPTPS_RUN_MANUAL;
    if (gptps_open_ex(&cfg, &e) != GPTPS_OK || !e) { ++fails; return 0.0; }

    memset(&d, 0, sizeof d); d.struct_size = sizeof d;
    d.name = "n"; d.run = noop; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    if (gptps_register_task(e, &d) != GPTPS_OK) { ++fails; gptps_shutdown(e); return 0.0; }

    t0 = gptps_hal_monotonic_ms();
    for (i = 0; i < n; ++i)
        if (gptps_submit(e, "n", NULL, 0, NULL) != GPTPS_OK) { ++fails; break; }
    while (gptps_step(e, &ran) == GPTPS_OK && ran) { /* drain from full depth */ }
    { double dt = (double)(gptps_hal_monotonic_ms() - t0); gptps_shutdown(e); return dt; }
}

int main(void)
{
    int n = 20000;
    double a, b, ratio;

    /* Grow n until the smaller run is long enough that clock noise cannot dominate
     * the ratio. Bounded so a very slow machine still terminates. */
    for (;;) {
        a = drain(n);
        if (fails) { printf("test_admission_perf: FAILED (engine error)\n"); return 1; }
        if (a >= 50.0 || n >= 160000) break;   /* 50ms: enough for a ms clock to be exact */
        n *= 2;
    }
    b = drain(2 * n);
    if (fails) { printf("test_admission_perf: FAILED (engine error)\n"); return 1; }

    ratio = (a > 0.0) ? b / a : 0.0;
    printf("drain %d: %.0fms | drain %d: %.0fms | ratio %.2f (linear ~2.0, quadratic ~4.0)\n",
           n, a, 2 * n, b, ratio);
    CHECK(ratio < MAX_RATIO);
    if (ratio >= MAX_RATIO)
        printf("       admission looks super-linear in queue depth - see \"intake ordering\" in src/engine.c\n");

    printf("test_admission_perf: %s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
