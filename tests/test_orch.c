/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_orch.c - orchestration add-on (run-after / fan-in). Proves task
 * dependencies are buildable purely on the public seams: a gated task does not
 * run until its dependency handles all reach a terminal state. Headless / portable.
 */
#include "gptps_orch.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int started, c_mark, g_release, fin_mark;
static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }

static void on_ev(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_STARTED  && strcmp(ev->task_name, "block") == 0) inc(&started);
}
/* Counts "mark" completions. Registered BEFORE the orchestrator so it fires
 * AFTER the orch's observer (observers fire newest-first) - so once fin_mark
 * advances, the orchestrator has already recorded that handle as terminal. */
static void obs_fin(const gptps_event *ev, void *ud)
{ (void)ud; if (ev->kind == GPTPS_EV_FINISHED && strcmp(ev->task_name, "mark") == 0) inc(&fin_mark); }
static gptps_status task_block(gptps_ctx *c, void *u) { (void)u; while (!get(&g_release) && !gptps_is_cancelled(c)) { } return GPTPS_OK; }
static gptps_status task_mark(gptps_ctx *c, void *u) { (void)c; (void)u; inc(&c_mark); return GPTPS_OK; }

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
    cfg.limits.max_concurrent_tasks = conc;
    gptps_open_ex(&cfg, &e);
    return e;
}
static void wait_for(int *p, int target)
{ uint64_t s = gptps_now_ms(NULL); while (get(p) < target && gptps_now_ms(NULL) - s < 2000) { } }

int main(void)
{
    gptps *e;
    gptps_orch *o;
    gptps_handle hA = 0, hB = 0, hX = 0;
    gptps_handle deps[2];

    /* ===== 1) fan-in: "C after A and B" - C is held until both finish ===== */
    __atomic_store_n(&started, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&c_mark, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_release, 0, __ATOMIC_SEQ_CST);

    e = open_engine(2); CHECK(e != NULL); if (!e) return 1;
    gptps_set_event_cb(e, on_ev, NULL);
    o = gptps_orch_install(e); CHECK(o != NULL); if (!o) { gptps_shutdown(e); return 1; }
    reg(e, "block", task_block);
    reg(e, "mark",  task_mark);

    CHECK(gptps_submit(e, "block", NULL, 0, &hA) == GPTPS_OK);   /* A: runs, blocks */
    CHECK(gptps_submit(e, "block", NULL, 0, &hB) == GPTPS_OK);   /* B: runs, blocks */
    wait_for(&started, 2);
    CHECK(get(&started) == 2);

    deps[0] = hA; deps[1] = hB;
    CHECK(gptps_orch_after(o, "mark", NULL, 0, deps, 2, NULL) == GPTPS_OK);
    CHECK(gptps_orch_pending(o) == 1);   /* held: deps not yet terminal */
    CHECK(get(&c_mark) == 0);            /* C has not run */

    __atomic_store_n(&g_release, 1, __ATOMIC_SEQ_CST);  /* let A and B finish */
    wait_for(&c_mark, 1);
    CHECK(get(&c_mark) == 1);            /* C ran after BOTH deps finished */
    CHECK(gptps_orch_pending(o) == 0);   /* gate released */

    gptps_shutdown(e);
    gptps_orch_close(o);

    /* ===== 2) deps already terminal => submit immediately, handle returned ===== */
    __atomic_store_n(&c_mark, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&fin_mark, 0, __ATOMIC_SEQ_CST);
    e = open_engine(2); CHECK(e != NULL); if (!e) return 1;
    CHECK(gptps_register_observer(e, obs_fin, NULL) == GPTPS_OK);  /* registered first => fires after orch's */
    o = gptps_orch_install(e); CHECK(o != NULL); if (!o) { gptps_shutdown(e); return 1; }
    reg(e, "mark", task_mark);

    CHECK(gptps_submit(e, "mark", NULL, 0, &hA) == GPTPS_OK);    /* A: finishes fast */
    wait_for(&fin_mark, 1);              /* A finished AND the orchestrator recorded it terminal */
    CHECK(get(&c_mark) == 1);

    deps[0] = hA;
    CHECK(gptps_orch_after(o, "mark", NULL, 0, deps, 1, &hX) == GPTPS_OK);
    CHECK(hX != 0);                      /* submitted immediately (dep already terminal) */
    CHECK(gptps_orch_pending(o) == 0);
    wait_for(&c_mark, 2);
    CHECK(get(&c_mark) == 2);

    gptps_shutdown(e);
    gptps_orch_close(o);

    if (fails) { printf("%d orch check(s) FAILED\n", fails); return 1; }
    printf("all orch checks passed\n");
    return 0;
}
