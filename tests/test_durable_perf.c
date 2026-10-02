/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_durable_perf.c - draining a recovered backlog must stay LINEAR in its size.
 *
 * A complexity gate like tests/test_admission_perf.c: it asserts on the SHAPE of the
 * curve, never on a rate, so it means the same thing on a laptop and on a loaded
 * runner. The regression it exists for: the durable queue's observer found the
 * record behind each event by walking the whole record table, so draining n
 * recovered records cost O(n^2) - 0.54s for 40,000 where the engine alone took
 * 0.004s, and four times longer for each doubling. A crash that leaves a deep backlog
 * is exactly when recovery has to be fast.
 *
 * The journal is written here, in the add-on's format, rather than through
 * gptps_dq_submit, which fsyncs every record: a backlog deep enough to time would
 * take minutes to build. Opening and recovering one fsync nothing per record. MANUAL
 * mode makes the measurement thread-free; the clock runs from gptps_dq_recover to
 * the end of the drain, the two paths that touch the handle index.
 */
#include "gptps.h"
#include "gptps_hal.h"   /* the engine's own monotonic clock: portable, no feature macros */
#include "gptps_durable_queue.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

/* Linear is 2.0 and quadratic is 4.0; the bar sits between, as in admission_perf.
 * The ratio measured before the index was 3.5-3.9 at these sizes. */
#define MAX_RATIO 3.0
#define JOURNAL "dq_test_perf.journal"

static gptps_status noop(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v) { int i; for (i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
static void put64(unsigned char *p, uint64_t v) { int i; for (i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
static uint32_t fnv(const unsigned char *p, size_t n, uint32_t h) { while (n--) { h ^= *p++; h *= 16777619u; } return h; }

/* `n` pending records of task "n" with a one-byte payload: the file header, then for
 * each [DQR1][P][0][name_len][payload_len][seq][name][payload][fnv1a], little-endian. */
static int write_journal(int n)
{
    FILE *f = fopen(JOURNAL, "wb");
    unsigned char hdr[8], rec[20 + 1 + 1 + 4];
    int i;
    if (!f) return 0;
    put32(hdr, 0x47445131u); put32(hdr + 4, 1u);
    fwrite(hdr, 1, sizeof hdr, f);
    for (i = 0; i < n; ++i) {
        put32(rec, 0x44515231u); rec[4] = 'P'; rec[5] = 0;
        put16(rec + 6, 1); put32(rec + 8, 1); put64(rec + 12, (uint64_t)i + 1);
        rec[20] = 'n'; rec[21] = (unsigned char)i;
        put32(rec + 22, fnv(rec, 22, 2166136261u));
        if (fwrite(rec, 1, sizeof rec, f) != sizeof rec) { fclose(f); return 0; }
    }
    return fclose(f) == 0;
}

/* recover and drain `n` journaled records, `reps` times; the milliseconds that took */
static double drain(int n, int reps)
{
    double total = 0.0;
    int r;
    for (r = 0; r < reps && !fails; ++r) {
        gptps_config cfg;
        gptps *e = NULL;
        gptps_dq *dq;
        gptps_task_def d;
        uint64_t t0;
        size_t ran;
        if (!write_journal(n)) { ++fails; break; }
        memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg;
        cfg.limits.struct_size = sizeof cfg.limits;
        cfg.limits.max_concurrent_tasks = 1;
        cfg.mode = GPTPS_RUN_MANUAL;
        if (gptps_open_ex(&cfg, &e) != GPTPS_OK || !e) { ++fails; break; }
        memset(&d, 0, sizeof d); d.struct_size = sizeof d;
        d.name = "n"; d.run = noop; d.exec = GPTPS_EXEC_INPROC;
        d.default_cost.struct_size = sizeof d.default_cost;
        d.default_policy.struct_size = sizeof d.default_policy;
        if (gptps_register_task(e, &d) != GPTPS_OK) { ++fails; gptps_shutdown(e); break; }
        dq = gptps_dq_open(e, JOURNAL);
        if (!dq) { ++fails; gptps_shutdown(e); break; }

        t0 = gptps_hal_monotonic_ms();
        if (gptps_dq_recover(dq) != (size_t)n) ++fails;
        while (gptps_step(e, &ran) == GPTPS_OK && ran) { /* every completion is looked up */ }
        total += (double)(gptps_hal_monotonic_ms() - t0);

        if (gptps_dq_pending(dq) != 0) ++fails;
        gptps_shutdown(e);
        gptps_dq_close(dq);
    }
    remove(JOURNAL);
    return total;
}

int main(void)
{
    int n = 5000, reps = 1, i;
    double a, b, ratio;

    /* Grow n until the smaller run is long enough that clock noise cannot dominate the
     * ratio (100ms: six ticks of a ~16ms clock). Past 80000 records, repeat instead. */
    for (;;) {
        a = drain(n, reps);
        if (fails) { printf("test_durable_perf: FAILED (engine or journal error)\n"); return 1; }
        if (a >= 100.0) break;
        if (n < 80000) n *= 2; else if (reps < 32) reps *= 2; else break;
    }
    b = drain(2 * n, reps);
    /* Best of three per size, as in admission_perf: a stalled run on a shared machine
     * can push a linear ratio up, but a quadratic path is slow in EVERY run. */
    for (i = 0; i < 2 && !fails; ++i) {
        double a2 = drain(n, reps), b2 = fails ? 0.0 : drain(2 * n, reps);
        if (a2 < a) a = a2;
        if (!fails && b2 < b) b = b2;
    }
    if (fails) { printf("test_durable_perf: FAILED (engine or journal error)\n"); return 1; }

    ratio = (a > 0.0) ? b / a : 0.0;
    printf("drain %d x%d: %.0fms | drain %d x%d: %.0fms | ratio %.2f (linear ~2.0, quadratic ~4.0)\n",
           n, reps, a, 2 * n, reps, b, ratio);
    CHECK(ratio < MAX_RATIO);
    if (ratio >= MAX_RATIO)
        printf("       draining looks super-linear in backlog size - see \"handle index\" in addons/gptps_durable_queue.c\n");

    printf("test_durable_perf: %s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
