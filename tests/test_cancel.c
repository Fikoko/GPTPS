/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_cancel.c - gptps_cancel(handle): cancel a queued item before it runs,
 * cancel an in-flight item (cooperative), and no-op on unknown / already-terminal
 * handles. Single worker makes the queueing deterministic. Headless / portable.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int started, fin_noop, fin_block, cancelled;
static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }

static void obs(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_STARTED) inc(&started);
    if (ev->kind == GPTPS_EV_FINISHED) {
        if (strcmp(ev->task_name, "noop")  == 0) inc(&fin_noop);
        if (strcmp(ev->task_name, "block") == 0) inc(&fin_block);
    }
    if (ev->kind == GPTPS_EV_FAILED && ev->status == GPTPS_E_CANCELLED) inc(&cancelled);
}

static gptps_status task_noop(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }
/* spins until cancelled; no timeout, so only gptps_cancel can stop it */
static gptps_status task_block(gptps_ctx *c, void *u) { (void)u; while (!gptps_is_cancelled(c)) { } return GPTPS_E_CANCELLED; }

static void reg(gptps *e, const char *name, gptps_run_fn fn)
{
    gptps_task_def d; memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = fn; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

/* After every way an item leaves the engine's queues, its handle is gone for
 * gptps_cancel: GPTPS_E_NOTFOUND, never a stale pointer. gptps_cancel finds items
 * through a handle index (see "finding an item" in src/engine.c), and an exit that
 * forgot to drop its entry would leave freed memory behind - which ASan turns into a
 * failure here. Cancelling from `ready` and from `delayed` is covered too, and enough
 * churn to make the index grow and reuse its deleted slots. MANUAL mode: exact. */
static gptps_status task_fail(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_E_TASK; }
static int g_deny, g_ran_noop;
static gptps_status task_count(gptps_ctx *c, void *u) { (void)c; (void)u; ++g_ran_noop; return GPTPS_OK; }
static gptps_admit_decision deny_when_asked(const gptps_constraint_input *in, uint32_t *ra, void *ud)
{ (void)in; (void)ra; (void)ud; return g_deny ? GPTPS_DENY : GPTPS_ADMIT; }

static void reg_policy(gptps *e, const char *name, gptps_run_fn fn, uint32_t retries,
                       uint32_t backoff, gptps_on_failure of)
{
    gptps_task_def d; memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = fn; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = retries; d.default_policy.retry_backoff_seconds = backoff;
    d.default_policy.on_failure = of;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

static void drain(gptps *e) { size_t ran; while (gptps_step(e, &ran) == GPTPS_OK && ran) { } }

static void test_every_exit(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    gptps_handle h, h2, hs[200];
    size_t ran;
    int i;

    memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;
    cfg.mode = GPTPS_RUN_MANUAL;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (!e) return;
    CHECK(gptps_register_constraint(e, deny_when_asked, NULL) == GPTPS_OK);
    reg(e, "ok", task_count);
    reg_policy(e, "dl",    task_fail, 0, 0,    GPTPS_ON_FAILURE_DEAD_LETTER);
    reg_policy(e, "drop",  task_fail, 0, 0,    GPTPS_ON_FAILURE_DROP);
    reg_policy(e, "retry", task_fail, 1, 3600, GPTPS_ON_FAILURE_DEAD_LETTER);
    reg(e, "big", task_count);
    reg(e, "gone", task_count);
    CHECK(gptps_define_resource(e, "seats", 2) == GPTPS_OK);
    CHECK(gptps_set_task_resource_cost(e, "big", "seats", 2) == GPTPS_OK);

    /* finished */
    CHECK(gptps_submit(e, "ok", NULL, 0, &h) == GPTPS_OK); drain(e);
    CHECK(gptps_cancel(e, h) == GPTPS_E_NOTFOUND);
    /* dead-lettered, then drained */
    CHECK(gptps_submit(e, "dl", NULL, 0, &h) == GPTPS_OK); drain(e);
    CHECK(gptps_cancel(e, h) == GPTPS_E_NOTFOUND);
    CHECK(gptps_dead_letter_drain(e, NULL, NULL) == 1);
    CHECK(gptps_cancel(e, h) == GPTPS_E_NOTFOUND);
    /* dropped */
    CHECK(gptps_submit(e, "drop", NULL, 0, &h) == GPTPS_OK); drain(e);
    CHECK(gptps_cancel(e, h) == GPTPS_E_NOTFOUND);
    /* denied by a constraint */
    g_deny = 1;
    CHECK(gptps_submit(e, "ok", NULL, 0, &h) == GPTPS_OK); drain(e);
    g_deny = 0;
    CHECK(gptps_cancel(e, h) == GPTPS_E_NOTFOUND);
    /* stranded by a budget cut while queued */
    CHECK(gptps_submit(e, "big", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_define_resource(e, "seats", 1) == GPTPS_OK);
    drain(e);
    CHECK(gptps_cancel(e, h) == GPTPS_E_NOTFOUND);
    CHECK(gptps_dead_letter_drain(e, NULL, NULL) == 2);   /* the denied one and this */
    /* removed by unregister while queued */
    CHECK(gptps_submit(e, "gone", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_submit(e, "gone", NULL, 0, &h2) == GPTPS_OK);
    CHECK(gptps_unregister_task(e, "gone", GPTPS_REMOVE_CANCEL) == GPTPS_OK);
    CHECK(gptps_cancel(e, h) == GPTPS_E_NOTFOUND);
    CHECK(gptps_cancel(e, h2) == GPTPS_E_NOTFOUND);
    /* waiting out a retry backoff in `delayed`: cancellable, then gone */
    CHECK(gptps_submit(e, "retry", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_step(e, &ran) == GPTPS_OK && ran == 1);
    CHECK(gptps_cancel(e, h) == GPTPS_OK);
    CHECK(gptps_cancel(e, h) == GPTPS_E_NOTFOUND);
    /* admitted but not started: a step's last pass leaves the second item in `ready` */
    g_ran_noop = 0;
    CHECK(gptps_submit(e, "ok", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_submit(e, "ok", NULL, 0, &h2) == GPTPS_OK);
    CHECK(gptps_step(e, &ran) == GPTPS_OK && ran == 1);
    CHECK(gptps_cancel(e, h2) == GPTPS_OK);
    drain(e);
    CHECK(g_ran_noop == 1);                               /* the cancelled one never ran */
    CHECK(gptps_cancel(e, h2) == GPTPS_E_NOTFOUND);
    /* churn: the index grows past its first size and reuses deleted slots */
    g_ran_noop = 0;
    for (i = 0; i < 200; ++i) CHECK(gptps_submit(e, "ok", NULL, 0, &hs[i]) == GPTPS_OK);
    for (i = 199; i >= 0; i -= 2) CHECK(gptps_cancel(e, hs[i]) == GPTPS_OK);
    for (i = 0; i < 200; i += 2) { CHECK(gptps_cancel(e, hs[i]) == GPTPS_OK);
                                   CHECK(gptps_submit(e, "ok", NULL, 0, &hs[i]) == GPTPS_OK); }
    drain(e);
    CHECK(g_ran_noop == 100);
    for (i = 0; i < 200; ++i) CHECK(gptps_cancel(e, hs[i]) == GPTPS_E_NOTFOUND);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

int main(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    gptps_handle hb = 0, hn = 0;
    uint64_t s;

    memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;          /* strictly sequential => deterministic queueing */
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (!e) return 1;
    gptps_register_observer(e, obs, NULL);
    reg(e, "block", task_block);
    reg(e, "noop",  task_noop);

    /* the single worker is occupied by "block" */
    CHECK(gptps_submit(e, "block", NULL, 0, &hb) == GPTPS_OK);
    s = gptps_now_ms(NULL);
    while (get(&started) < 1 && gptps_now_ms(NULL) - s < 2000) { }
    CHECK(get(&started) == 1);

    /* "noop" queues behind the busy worker; cancel it before it can run */
    CHECK(gptps_submit(e, "noop", NULL, 0, &hn) == GPTPS_OK);
    CHECK(gptps_cancel(e, hn) == GPTPS_OK);            /* queued -> removed + terminal event */
    CHECK(gptps_cancel(e, hn) == GPTPS_E_NOTFOUND);    /* already gone */
    CHECK(gptps_cancel(e, 999999u) == GPTPS_E_NOTFOUND); /* unknown handle */

    /* cancel the in-flight "block": without this, shutdown would hang on the
     * spinning task, so a clean shutdown is the proof that in-flight cancel works */
    CHECK(gptps_cancel(e, hb) == GPTPS_OK);

    gptps_shutdown(e);

    CHECK(get(&fin_noop)  == 0);   /* the cancelled queued item never ran */
    CHECK(get(&fin_block) == 0);   /* block was cancelled, not finished-OK */
    CHECK(get(&cancelled) >= 1);   /* the queued noop emitted FAILED/E_CANCELLED */

    test_every_exit();

    if (fails) { printf("%d cancel check(s) FAILED\n", fails); return 1; }
    printf("all cancel checks passed\n");
    return 0;
}
