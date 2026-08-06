/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_backpressure.c - bounded intake (max_intake_depth): gptps_submit returns
 * GPTPS_E_FULL once that many un-admitted items are queued, recovers as the queue
 * drains, and is tunable live via the "limits.max_intake_depth" setting.
 * One worker makes the queueing deterministic. Headless / portable.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int started, finished;
static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static void obs(const gptps_event *ev, void *u)
{
    (void)u;
    if (ev->kind == GPTPS_EV_STARTED  && strcmp(ev->task_name, "block") == 0) inc(&started);
    if (ev->kind == GPTPS_EV_FINISHED && strcmp(ev->task_name, "noop")  == 0) inc(&finished);
}
static gptps_status task_noop(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }
static gptps_status task_block(gptps_ctx *c, void *u) { (void)u; while (!gptps_is_cancelled(c)) { } return GPTPS_OK; }
static void reg(gptps *e, const char *n, gptps_run_fn f)
{
    gptps_task_def d; memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = n; d.run = f; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}
static void wait_for(int *p, int target)
{ uint64_t s = gptps_now_ms(NULL); while (get(p) < target && gptps_now_ms(NULL) - s < 2000) { } }

int main(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    gptps_handle h = 0, hb = 0;
    char buf[64];
    int i;

    memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;     /* single worker => deterministic queueing */
    cfg.limits.max_intake_depth = 2;         /* bound the intake queue */
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (!e) return 1;
    gptps_register_observer(e, obs, NULL);
    reg(e, "block", task_block);
    reg(e, "noop",  task_noop);

    /* the configured bound is visible as a setting */
    CHECK(gptps_settings_get(e, "limits.max_intake_depth", buf, sizeof buf) == GPTPS_OK);
    CHECK(strcmp(buf, "2") == 0);

    /* occupy the single worker so submitted noops stay in intake */
    CHECK(gptps_submit(e, "block", NULL, 0, &hb) == GPTPS_OK);
    wait_for(&started, 1);
    CHECK(get(&started) == 1);

    /* intake bounded at 2: two accepted, the third refused */
    CHECK(gptps_submit(e, "noop", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_submit(e, "noop", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_submit(e, "noop", NULL, 0, &h) == GPTPS_E_FULL);

    /* free the worker; the two queued items drain and run */
    CHECK(gptps_cancel(e, hb) == GPTPS_OK);
    wait_for(&finished, 2);
    CHECK(get(&finished) == 2);

    /* queue has room again now that it drained */
    CHECK(gptps_submit(e, "noop", NULL, 0, &h) == GPTPS_OK);
    wait_for(&finished, 3);
    CHECK(get(&finished) == 3);

    /* live tuning: 0 => unbounded; many submits while busy, none refused */
    CHECK(gptps_settings_set(e, "limits.max_intake_depth", "0") == GPTPS_OK);
    __atomic_store_n(&started, 0, __ATOMIC_SEQ_CST);
    CHECK(gptps_submit(e, "block", NULL, 0, &hb) == GPTPS_OK);
    wait_for(&started, 1);
    for (i = 0; i < 10; ++i) CHECK(gptps_submit(e, "noop", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_cancel(e, hb) == GPTPS_OK);

    gptps_shutdown(e);
    if (fails) { printf("%d backpressure check(s) FAILED\n", fails); return 1; }
    printf("all backpressure checks passed\n");
    return 0;
}
