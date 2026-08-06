/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_engine.c - end-to-end engine: open -> register -> submit -> run ->
 * events -> shutdown, plus submit validation (unknown task, never-fit).
 * Counters are atomic because tasks/events run on the worker pool.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int g_run = 0, g_queued = 0, g_started = 0, g_finished = 0, g_failed = 0;
static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }

static gptps_status task_ok(gptps_ctx *ctx, void *ud)
{
    size_t n; const void *p = gptps_payload(ctx, &n);
    (void)ud; (void)p;
    if (gptps_is_cancelled(ctx)) return GPTPS_E_CANCELLED; /* should be false in normal run */
    inc(&g_run);
    return gptps_result_set(ctx, "done", 4);
}

static void on_ev(const gptps_event *ev, void *ud)
{
    (void)ud;
    switch (ev->kind) {
        case GPTPS_EV_QUEUED:   inc(&g_queued);   break;
        case GPTPS_EV_STARTED:  inc(&g_started);  break;
        case GPTPS_EV_FINISHED: inc(&g_finished); break;
        case GPTPS_EV_FAILED:   inc(&g_failed);   break;
        default: break;
    }
}

int main(void)
{
    gptps *e = NULL;
    gptps_handle h;
    gptps_task_def d, big;
    int i, N = 50;

    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    CHECK(e != NULL);
    if (!e) { printf("engine open failed\n"); return 1; }
    gptps_set_event_cb(e, on_ev, NULL);

    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "ok"; d.run = task_ok; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = 1024;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(gptps_register_task(e, &d) == GPTPS_E_DUP);              /* dup name */

    CHECK(gptps_submit(e, "nope", "x", 1, &h) == GPTPS_E_NOTFOUND); /* unknown task */

    memset(&big, 0, sizeof big);
    big.struct_size = sizeof big; big.name = "big"; big.run = task_ok; big.exec = GPTPS_EXEC_INPROC;
    big.default_cost.struct_size = sizeof big.default_cost;
    big.default_cost.mem_bytes = UINT64_MAX;                       /* can never fit */
    CHECK(gptps_register_task(e, &big) == GPTPS_OK);
    CHECK(gptps_submit(e, "big", "x", 1, &h) == GPTPS_E_BUDGET);   /* never-fit reject */

    for (i = 0; i < N; ++i)
        CHECK(gptps_submit(e, "ok", &i, sizeof i, &h) == GPTPS_OK);

    gptps_shutdown(e);  /* drains all in-flight + queued, then joins */

    CHECK(get(&g_run)      == N);
    CHECK(get(&g_started)  == N);
    CHECK(get(&g_finished) == N);
    CHECK(get(&g_queued)   == N);   /* rejected submits never reach QUEUED */
    CHECK(get(&g_failed)   == 0);

    if (fails) { printf("%d check(s) FAILED\n", fails); return 1; }
    printf("all engine checks passed (ran %d tasks)\n", get(&g_run));
    return 0;
}
