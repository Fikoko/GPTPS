/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_resource.c - generic named-resource budgets. A resource (e.g. "gpu") with
 * a total budget gates admission independently of worker concurrency and the
 * memory budget: the dispatcher admits an item only while every resource it costs
 * still fits, reserving on admit and releasing when it ends. Headless / portable.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int started, finished, g_release;
static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static void obs(const gptps_event *ev, void *u)
{
    (void)u;
    if (strcmp(ev->task_name, "render") != 0) return;
    if (ev->kind == GPTPS_EV_STARTED)  inc(&started);
    if (ev->kind == GPTPS_EV_FINISHED) inc(&finished);
}
static gptps_status task_block(gptps_ctx *c, void *u) { (void)u; while (!get(&g_release) && !gptps_is_cancelled(c)) { } return GPTPS_OK; }
static gptps_status task_noop(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }

static void reg(gptps *e, const char *n, gptps_run_fn f)
{
    gptps_task_def d; memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = n; d.run = f; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}
static gptps *open_engine(unsigned conc)
{
    gptps_config cfg; gptps *e = NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = conc; cfg.limits.max_memory_bytes = 1ull << 30;
    gptps_open_ex(&cfg, &e);
    return e;
}
/* poll a resource's reserved amount until it reaches `want` (or a 2s timeout) */
static uint64_t wait_reserved(gptps *e, const char *name, uint64_t want)
{
    uint64_t res = ~0ull, s = gptps_now_ms(NULL);
    while (gptps_now_ms(NULL) - s < 2000) {
        if (gptps_resource_usage(e, name, &res, NULL) != GPTPS_OK) break;
        if (res == want) break;
    }
    return res;
}
static void wait_for(int *p, int target)
{ uint64_t s = gptps_now_ms(NULL); while (get(p) < target && gptps_now_ms(NULL) - s < 2000) { } }

int main(void)
{
    gptps *e;
    gptps_handle h;
    uint64_t res, bud;

    /* ===== 1) gpu budget gates admission (concurrency + memory are free) ===== */
    __atomic_store_n(&started, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&finished, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_release, 0, __ATOMIC_SEQ_CST);

    e = open_engine(4); CHECK(e != NULL); if (!e) return 1;   /* 4 workers => not concurrency-bound */
    gptps_set_event_cb(e, obs, NULL);
    CHECK(gptps_define_resource(e, "gpu", 4) == GPTPS_OK);
    reg(e, "render", task_block);
    CHECK(gptps_set_task_resource_cost(e, "render", "gpu", 3) == GPTPS_OK);  /* 2 => 6 > 4 budget */

    CHECK(gptps_submit(e, "render", NULL, 0, &h) == GPTPS_OK);   /* A: admits, reserves 3 */
    CHECK(gptps_submit(e, "render", NULL, 0, &h) == GPTPS_OK);   /* B: 3+3 > 4 => gated */
    CHECK(wait_reserved(e, "gpu", 3) == 3);
    wait_for(&started, 1);
    CHECK(get(&started) == 1);     /* only A ran; B is held by the gpu budget, not workers/mem */
    CHECK(gptps_resource_usage(e, "gpu", &res, &bud) == GPTPS_OK);
    CHECK(res == 3 && bud == 4);

    __atomic_store_n(&g_release, 1, __ATOMIC_SEQ_CST);          /* A finishes => frees 3 => B admits */
    wait_for(&finished, 2);
    CHECK(get(&finished) == 2);
    CHECK(get(&started) == 2);
    CHECK(wait_reserved(e, "gpu", 0) == 0);                     /* all reservations released */
    gptps_shutdown(e);

    /* ===== 2) never-fits + unknown task/resource ===== */
    e = open_engine(2); CHECK(e != NULL); if (!e) return 1;
    CHECK(gptps_define_resource(e, "gpu", 4) == GPTPS_OK);
    reg(e, "big", task_noop);
    CHECK(gptps_set_task_resource_cost(e, "big", "gpu", 5) == GPTPS_OK);   /* 5 > whole budget */
    CHECK(gptps_submit(e, "big", NULL, 0, &h) == GPTPS_E_BUDGET);          /* rejected at submit */
    CHECK(gptps_set_task_resource_cost(e, "big", "nope", 1) == GPTPS_E_NOTFOUND);
    CHECK(gptps_set_task_resource_cost(e, "ghost", "gpu", 1) == GPTPS_E_NOTFOUND);
    CHECK(gptps_resource_usage(e, "nope", &res, &bud) == GPTPS_E_NOTFOUND);
    gptps_shutdown(e);

    /* ===== 3) define a resource AFTER tasks exist (grows their cost vectors);
     *          a bigger budget lets both items run at once ===== */
    __atomic_store_n(&started, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&finished, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_release, 0, __ATOMIC_SEQ_CST);

    e = open_engine(4); CHECK(e != NULL); if (!e) return 1;
    gptps_set_event_cb(e, obs, NULL);
    reg(e, "render", task_block);                              /* registered BEFORE the resource exists */
    reg(e, "free", task_noop);                                 /* a task with no resource cost */
    CHECK(gptps_define_resource(e, "gpu", 8) == GPTPS_OK);     /* grows existing tasks' cost vectors */
    CHECK(gptps_set_task_resource_cost(e, "render", "gpu", 3) == GPTPS_OK);  /* 3+3 = 6 <= 8 */

    CHECK(gptps_submit(e, "render", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_submit(e, "render", NULL, 0, &h) == GPTPS_OK);
    wait_for(&started, 2);
    CHECK(get(&started) == 2);     /* both fit in the 8-unit budget */
    CHECK(wait_reserved(e, "gpu", 6) == 6);
    /* a no-cost task is never gated by the resource */
    CHECK(gptps_submit(e, "free", NULL, 0, &h) == GPTPS_OK);
    __atomic_store_n(&g_release, 1, __ATOMIC_SEQ_CST);
    wait_for(&finished, 2);
    CHECK(get(&finished) == 2);
    CHECK(wait_reserved(e, "gpu", 0) == 0);
    gptps_shutdown(e);

    if (fails) { printf("%d resource check(s) FAILED\n", fails); return 1; }
    printf("all resource checks passed\n");
    return 0;
}
