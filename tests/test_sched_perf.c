/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_sched_perf.c - admission stays LINEAR in queue depth with a scheduler
 * installed: addons/gptps_sched's EDF, fair share and aging.
 *
 * A complexity gate like test_admission_perf: it asserts on the SHAPE of the curve,
 * never on a rate. The regression it exists to catch: with any scheduler hook
 * installed, the engine rescored and re-sorted every pending item on every admission
 * pass, under its lock - so draining n items cost O(n^2 log n). Measured before the
 * fix, 8 slots: 5 000 items 154 ms, 10 000 673 ms, 20 000 6 031 ms (a ratio of ~9 per
 * doubling), against 2 / 3 / 5 ms with no hook. The fix is GPTPS_SCHED_STABLE: a hook
 * whose keys never move while an item waits is asked once per item, as it enters the
 * queue - which all three of these orderings are.
 *
 * MANUAL mode: every item is queued before the first admission decision, so each
 * drain starts from full depth.
 *
 * THE BAR. The queue is a balanced search tree, so a drain is O(n log n), not O(n):
 * with deadlines arriving in random order each insert lands somewhere new in a tree
 * that outgrows the CPU cache, and EDF's doubling ratio measured 2.05-2.73 on Linux
 * and 3.03 on a macOS runner - over the 3.0 that test_admission_perf uses for a
 * strictly linear structure, which this gate first copied. So it compares n with 4n
 * instead of 2n, which widens the gap between the two cases it must tell apart:
 * linear ~4, O(n log n) with cache effects ~5-6, quadratic ~16 (and the bug this
 * gate exists for measured 9-15 on a mere doubling). The bar sits at 8.
 */
#include "gptps.h"
#include "gptps_hal.h"
#include "gptps_sched.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

#define SCALE     4     /* compare n with SCALE * n */
#define MAX_RATIO 8.0   /* linear ~4, n log n ~5-6, quadratic ~16: see THE BAR above */

/* No drain here takes a second when admission is linear. One that runs past this is
 * itself the verdict - the quadratic path took minutes for a single drain - so it is
 * stopped and failed at once rather than left to reach ctest's timeout. */
#define BUDGET_MS 30000u
static int g_overtime;

enum { K_EDF, K_FAIR, K_AGING, K_N };
static const char *const NAMES[K_N] = { "EDF", "fair share", "aging" };

static gptps_status noop(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }
static uint64_t by_byte(const gptps_sched_input *in, void *u)
{ (void)u; return in->payload_len ? ((const unsigned char *)in->payload)[0] : 0u; }

/* queue `n` items - mixed due times, eight tenants - then drain; milliseconds */
static double drain(int kind, int n, int reps)
{
    gptps_config cfg;
    gptps *e = NULL;
    gptps_task_def d;
    gptps_sched *s = NULL;
    uint64_t t0;
    size_t ran;
    int i, r;

    memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 8;
    cfg.mode = GPTPS_RUN_MANUAL;
    if (gptps_open_ex(&cfg, &e) != GPTPS_OK || !e) { ++fails; return 0.0; }
    memset(&d, 0, sizeof d); d.struct_size = sizeof d;
    d.name = "n"; d.run = noop;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    if (gptps_register_task(e, &d) != GPTPS_OK) { ++fails; gptps_shutdown(e); return 0.0; }
    if ((kind == K_EDF   && gptps_sched_edf(e, &s) != GPTPS_OK) ||
        (kind == K_FAIR  && gptps_sched_fair(e, by_byte, NULL, 16, &s) != GPTPS_OK) ||
        (kind == K_AGING && gptps_sched_aging(e, 10, &s) != GPTPS_OK)) { ++fails; gptps_shutdown(e); return 0.0; }

    t0 = gptps_hal_monotonic_ms();
    for (r = 0; r < reps && !fails; ++r) {
        for (i = 0; i < n; ++i) {
            unsigned char b = (unsigned char)(i % 8);
            gptps_submit_options o;
            memset(&o, 0, sizeof o);
            o.struct_size = sizeof o;
            o.flags = GPTPS_SUBMIT_DEADLINE | GPTPS_SUBMIT_PRIORITY;
            o.deadline_in_ms = (uint64_t)((i * 7919) % 50000);
            o.priority = (int32_t)(i % 5);
            if (gptps_submit_ex(e, "n", &b, 1, &o, NULL) != GPTPS_OK) { ++fails; break; }
        }
        {
            unsigned steps = 0;
            while (gptps_step(e, &ran) == GPTPS_OK && ran)
                if ((++steps & 255u) == 0 && gptps_hal_monotonic_ms() - t0 > BUDGET_MS) { g_overtime = 1; break; }
        }
        if (g_overtime) break;
    }
    { double dt = (double)(gptps_hal_monotonic_ms() - t0); gptps_shutdown(e); gptps_sched_close(s); return dt; }
}

int main(void)
{
    int k;
    for (k = 0; k < K_N; ++k) {
        int n = 20000, reps = 1, i;
        double a, b, ratio;
        for (;;) {                                 /* grow until clock noise cannot dominate */
            a = drain(k, n, reps);
            if (fails) break;
            if (a >= 100.0) break;
            if (n < 160000) n *= 2; else if (reps < 32) reps *= 2; else break;
        }
        if (fails) break;
        b = drain(k, SCALE * n, reps);
        if (g_overtime) {
            printf("%-10s a drain of %d items ran past %u ms: admission is super-linear\n",
                   NAMES[k], SCALE * n, BUDGET_MS);
            ++fails;
            break;
        }
        /* Best of three - unless the first is already far past the bar: the repeats
         * only filter out a noisy HIGH reading, and a quadratic path would spend
         * minutes on them before failing (it reached ctest's timeout instead). */
        for (i = 0; i < 2 && !fails && a > 0.0 && b / a < 2.0 * MAX_RATIO; ++i) {
            double a2 = drain(k, n, reps), b2 = drain(k, SCALE * n, reps);
            if (a2 < a) a = a2;
            if (b2 < b) b = b2;
        }
        if (fails) break;
        ratio = (a > 0.0) ? b / a : 0.0;
        printf("%-10s drain %d x%d: %.0fms | drain %d x%d: %.0fms | ratio %.2f (linear ~4, quadratic ~16)\n",
               NAMES[k], n, reps, a, SCALE * n, reps, b, ratio);
        CHECK(ratio < MAX_RATIO);
    }
    printf("test_sched_perf: %s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
