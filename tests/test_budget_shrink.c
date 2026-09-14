/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_budget_shrink.c - a budget lowered at runtime must not strand queued work.
 *
 * submit() rejects an item whose declared cost can never fit (GPTPS_E_BUDGET), but
 * an item that WAS admissible when queued, and then had the budget shrunk under it,
 * was previously never re-checked: it stayed in intake forever, the reserve-for-`top`
 * starvation guard then froze everything queued behind it, and gptps_shutdown never
 * returned (the dispatcher exits only on an empty intake). Both the memory budget
 * ("limits.max_memory_bytes") and a named resource (gptps_define_resource re-budget)
 * are covered. One worker makes the queueing deterministic. Headless / portable.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int started_big, dead_big, finished_small, finished_wait;
static gptps_status dead_big_status;
static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static void obs(const gptps_event *ev, void *u)
{
    (void)u;
    if (strcmp(ev->task_name, "big") == 0) {
        if (ev->kind == GPTPS_EV_STARTED) inc(&started_big);
        if (ev->kind == GPTPS_EV_DEAD_LETTERED) { dead_big_status = ev->status; inc(&dead_big); }
    }
    if (ev->kind == GPTPS_EV_FINISHED && strcmp(ev->task_name, "small") == 0) inc(&finished_small);
    if (ev->kind == GPTPS_EV_FINISHED && strcmp(ev->task_name, "wait")  == 0) inc(&finished_wait);
}
static gptps_status task_noop(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }
static gptps_status task_wait(gptps_ctx *c, void *u)
{   /* holds the single worker until the test lets go */
    int *go = (int *)u;
    while (!get(go) && !gptps_is_cancelled(c)) { }
    return GPTPS_OK;
}
static void reg(gptps *e, const char *n, gptps_run_fn f, uint64_t mem, void *ud)
{
    gptps_task_def d; memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = n; d.run = f; d.exec = GPTPS_EXEC_INPROC; d.user_data = ud;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = mem;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}
static void wait_for(int *p, int target)
{ uint64_t s = gptps_now_ms(NULL); while (get(p) < target && gptps_now_ms(NULL) - s < 3000) { } }
static gptps *open1(uint64_t mem)
{
    gptps *e = NULL;
    gptps_config cfg; memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1; cfg.limits.max_memory_bytes = mem;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK && e);
    CHECK(gptps_settings_set(e, "limits.shutdown_grace_ms", "2000") == GPTPS_OK);
    gptps_set_event_cb(e, obs, NULL);
    return e;
}
static void reset(void) { started_big = dead_big = finished_small = finished_wait = 0; dead_big_status = GPTPS_OK; }

/* 1) memory budget: 100 -> 50 while the worker is held; a queued cost-60 item and
 *    20 cost-10 items behind it (more than reserve_after_skips). */
static void test_memory_shrink(void)
{
    gptps *e = open1(100); gptps_handle h; int go = 0, i;
    reset();
    reg(e, "wait",  task_wait, 1,  &go);
    reg(e, "big",   task_noop, 60, NULL);
    reg(e, "small", task_noop, 10, NULL);
    CHECK(gptps_submit(e, "wait", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_submit(e, "big",  NULL, 0, &h) == GPTPS_OK);      /* fits budget 100 */
    for (i = 0; i < 20; ++i) CHECK(gptps_submit(e, "small", NULL, 0, &h) == GPTPS_OK);

    CHECK(gptps_settings_set(e, "limits.max_memory_bytes", "50") == GPTPS_OK);
    CHECK(gptps_submit(e, "big", NULL, 0, &h) == GPTPS_E_BUDGET); /* submit-time check still holds */
    __atomic_store_n(&go, 1, __ATOMIC_SEQ_CST);

    wait_for(&dead_big, 1);
    CHECK(get(&dead_big) == 1);                                   /* stranded item gets a terminal event */
    CHECK(dead_big_status == GPTPS_E_BUDGET);
    CHECK(get(&started_big) == 0);                                /* and never ran */
    wait_for(&finished_small, 20);
    CHECK(get(&finished_small) == 20);                            /* nothing behind it is frozen */
    CHECK(gptps_dead_letter_count(e) == 1);
    CHECK(gptps_shutdown(e) == GPTPS_OK);                        /* returns (the old hang) */
}

/* 2) named resource: same shape via gptps_define_resource re-budget. */
static void test_resource_shrink(void)
{
    gptps *e = open1(1000); gptps_handle h; int go = 0, i;
    reset();
    CHECK(gptps_define_resource(e, "seats", 4) == GPTPS_OK);
    reg(e, "wait",  task_wait, 1, &go);
    reg(e, "big",   task_noop, 1, NULL);
    reg(e, "small", task_noop, 1, NULL);
    CHECK(gptps_set_task_resource_cost(e, "big", "seats", 3) == GPTPS_OK);
    CHECK(gptps_submit(e, "wait", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_submit(e, "big",  NULL, 0, &h) == GPTPS_OK);
    for (i = 0; i < 20; ++i) CHECK(gptps_submit(e, "small", NULL, 0, &h) == GPTPS_OK);

    CHECK(gptps_define_resource(e, "seats", 2) == GPTPS_OK);     /* re-budget below big's cost */
    __atomic_store_n(&go, 1, __ATOMIC_SEQ_CST);

    wait_for(&dead_big, 1);
    CHECK(get(&dead_big) == 1 && dead_big_status == GPTPS_E_BUDGET);
    CHECK(get(&started_big) == 0);
    wait_for(&finished_small, 20);
    CHECK(get(&finished_small) == 20);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

/* 3) a shrink that still fits must change nothing: budget 100 -> 70 with a queued
 *    cost-60 item just waits for the running one and then runs. */
static void test_shrink_still_fits(void)
{
    gptps *e = open1(100); gptps_handle h; int go = 0;
    reset();
    reg(e, "wait", task_wait, 1,  &go);
    reg(e, "big",  task_noop, 60, NULL);
    CHECK(gptps_submit(e, "wait", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_submit(e, "big",  NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_settings_set(e, "limits.max_memory_bytes", "70") == GPTPS_OK);
    __atomic_store_n(&go, 1, __ATOMIC_SEQ_CST);
    wait_for(&started_big, 1);
    CHECK(get(&started_big) == 1);
    CHECK(get(&dead_big) == 0);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    CHECK(get(&finished_wait) == 1);
}

int main(void)
{
    test_memory_shrink();
    test_resource_shrink();
    test_shrink_still_fits();
    if (fails) { printf("%d budget-shrink check(s) FAILED\n", fails); return 1; }
    printf("all budget-shrink checks passed\n");
    return 0;
}
