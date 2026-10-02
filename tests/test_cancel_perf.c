/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_cancel_perf.c - cancelling must stay LINEAR in queue depth.
 *
 * A complexity gate like tests/test_admission_perf.c: it asserts on the SHAPE of the
 * curve, never on a rate. The regression it exists for: gptps_cancel found its item by
 * walking the queues, and a cancel from intake wiped the cache that keeps an ordered
 * insert O(1), so the next submit walked the whole queue for its place. Cancelling
 * 40,000 queued items newest-first took 4.5s, and cancelling and resubmitting at that
 * depth 5.5s - each cancel holding the engine lock for its walk.
 *
 * Three shapes, each a full pass over a queue of n: newest-first from intake; cancel
 * and resubmit at depth; and newest-first from `delayed`, where a constraint that
 * DEFERs everything parks the whole queue in one pass. MANUAL mode keeps it thread-free
 * and the queue as deep as it was built: nothing runs unless stepped.
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

/* A fine clock of its own, not the engine's millisecond one: on Windows that ticks every
 * ~15.6ms, and at a hundred milliseconds a tick either way moves the ratio by a third. */
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

/* Linear is 2.0 and quadratic is 4.0; the bar sits between, as in admission_perf.
 * The ratio before the fix was 5-6 for newest-first. */
#define MAX_RATIO 3.0

static gptps_status noop(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }

static gptps_admit_decision defer_all(const gptps_constraint_input *in, uint32_t *retry_after, void *ud)
{
    (void)in; (void)ud;
    *retry_after = 3600000u;          /* an hour: parked for the rest of the test */
    return GPTPS_DEFER;
}

static gptps *open_manual(void)
{
    gptps_config cfg;
    gptps *e = NULL;
    gptps_task_def d;
    memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;
    cfg.mode = GPTPS_RUN_MANUAL;
    if (gptps_open_ex(&cfg, &e) != GPTPS_OK || !e) return NULL;
    memset(&d, 0, sizeof d); d.struct_size = sizeof d;
    d.name = "n"; d.run = noop; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    if (gptps_register_task(e, &d) != GPTPS_OK) { gptps_shutdown(e); return NULL; }
    return e;
}

static int fill(gptps *e, gptps_handle *h, int n)
{
    int i;
    for (i = 0; i < n; ++i) if (gptps_submit(e, "n", NULL, 0, &h[i]) != GPTPS_OK) return 0;
    return 1;
}

/* the milliseconds the three shapes take on a queue of n, `reps` times over */
static double cancels(int n, int reps)
{
    gptps_handle *h = (gptps_handle *)malloc((size_t)n * sizeof *h);
    double total = 0.0;
    int r, i;
    if (!h) { ++fails; return 0.0; }
    for (r = 0; r < reps && !fails; ++r) {
        gptps *e = open_manual();
        double t0;
        size_t ran;
        if (!e) { ++fails; break; }

        if (!fill(e, h, n)) { ++fails; gptps_shutdown(e); break; }
        t0 = now_ms();
        for (i = n - 1; i >= 0; --i) if (gptps_cancel(e, h[i]) != GPTPS_OK) { ++fails; break; }
        total += now_ms() - t0;

        if (!fill(e, h, n)) { ++fails; gptps_shutdown(e); break; }
        t0 = now_ms();
        for (i = 0; i < n; ++i)
            if (gptps_cancel(e, h[i]) != GPTPS_OK || gptps_submit(e, "n", NULL, 0, &h[i]) != GPTPS_OK) { ++fails; break; }
        total += now_ms() - t0;
        for (i = 0; i < n; ++i) gptps_cancel(e, h[i]);   /* empty it again, untimed */

        if (gptps_register_constraint(e, defer_all, NULL) != GPTPS_OK || !fill(e, h, n)) {
            ++fails; gptps_shutdown(e); break;
        }
        if (gptps_step(e, &ran) != GPTPS_OK || ran != 0) ++fails;   /* all of it now in `delayed` */
        t0 = now_ms();
        for (i = n - 1; i >= 0; --i) if (gptps_cancel(e, h[i]) != GPTPS_OK) { ++fails; break; }
        total += now_ms() - t0;

        gptps_shutdown(e);
    }
    free(h);
    return total;
}

int main(void)
{
    int n = 1250, reps = 1, i;
    double a, b, ratio;

    /* Grow n until the smaller run takes 100ms - but past 5000 items, repeat instead.
     * Every cancel is a lookup by handle, scattered across memory, and once the queue
     * outgrows the caches a miss per item reads as a super-linear curve on an O(1) path:
     * 3.2 at 20000/40000 on Windows CI, with the coarse clock's noise on top. A walk
     * shows long before the cap - the old one measured 4.3 at 5000/10000 - so the cap
     * costs the gate nothing. */
    for (;;) {
        a = cancels(n, reps);
        if (fails) { printf("test_cancel_perf: FAILED (engine error)\n"); return 1; }
        if (a >= 100.0) break;
        if (n < 5000) n *= 2; else if (reps < 4096) reps *= 2; else break;
    }
    b = cancels(2 * n, reps);
    /* Best of three per size, as in admission_perf: a stalled run on a shared machine
     * can push a linear ratio up, but a quadratic path is slow in EVERY run. */
    for (i = 0; i < 2 && !fails; ++i) {
        double a2 = cancels(n, reps), b2 = fails ? 0.0 : cancels(2 * n, reps);
        if (a2 < a) a = a2;
        if (!fails && b2 < b) b = b2;
    }
    if (fails) { printf("test_cancel_perf: FAILED (engine error)\n"); return 1; }

    ratio = (a > 0.0) ? b / a : 0.0;
    printf("cancel %d x%d: %.0fms | cancel %d x%d: %.0fms | ratio %.2f (linear ~2.0, quadratic ~4.0)\n",
           n, reps, a, 2 * n, reps, b, ratio);
    CHECK(ratio < MAX_RATIO);
    if (ratio >= MAX_RATIO)
        printf("       cancelling looks super-linear in queue depth - see \"finding an item\" in src/engine.c\n");

    printf("test_cancel_perf: %s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
