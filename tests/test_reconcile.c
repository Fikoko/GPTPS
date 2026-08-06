/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_reconcile.c - every submitted handle reaches EXACTLY ONE terminal event.
 *
 * The core deliberately never aggregates: observers are the only completion
 * channel, so an item that vanishes without a terminal event makes every add-on
 * built on that seam quietly wrong (gpu_quota, for one, releases its reservation
 * only when it sees a terminal event - a silently-freed item leaked its budget
 * permanently). This file pins the contract for the paths that used to break it:
 * unregister-with-CANCEL, the DROP failure policy, and cancel of an item that had
 * already started.
 *
 * It also pins the DISTINCTION between the two ways an item can be stopped: an
 * operator's gptps_cancel must not be reported as a deadline breach.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }

static int n_queued, n_terminal, n_started;
static int n_cancelled, n_timeout;

/* Which events CLOSE a handle:
 *   FINISHED                     - success, always terminal
 *   DROPPED / DEAD_LETTERED      - the disposition after retries are exhausted
 *   FAILED with GPTPS_E_CANCELLED - cancellation is terminal in its own right
 * A plain FAILED is per-ATTEMPT, not terminal: a RETRIED, DROPPED or
 * DEAD_LETTERED always follows it. Counting it as terminal would double-count
 * every ordinary failure, which is why it is excluded here. */
static void obs(const gptps_event *ev, void *ud)
{
    (void)ud;
    switch (ev->kind) {
        case GPTPS_EV_QUEUED:  inc(&n_queued); break;
        case GPTPS_EV_STARTED: inc(&n_started); break;
        case GPTPS_EV_FINISHED:
        case GPTPS_EV_DROPPED:
        case GPTPS_EV_DEAD_LETTERED:
            inc(&n_terminal); break;
        case GPTPS_EV_FAILED:
            if (ev->status == GPTPS_E_CANCELLED) { inc(&n_cancelled); inc(&n_terminal); }
            if (ev->status == GPTPS_E_TIMEOUT)   inc(&n_timeout);
            break;
        default: break;
    }
}

static void reset(void)
{
    __atomic_store_n(&n_queued, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&n_terminal, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&n_started, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&n_cancelled, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&n_timeout, 0, __ATOMIC_SEQ_CST);
}

static gptps_status task_block(gptps_ctx *c, void *u)
{ (void)u; while (!gptps_is_cancelled(c)) { } return GPTPS_E_CANCELLED; }
static gptps_status task_fail(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_E_TASK; }

static void reg(gptps *e, const char *name, gptps_run_fn fn, gptps_on_failure onfail)
{
    gptps_task_def d; memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = fn; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.on_failure = onfail;      /* max_retries stays 0 */
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

static gptps *open1(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;    /* deterministic: one runs, the rest queue */
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (e) gptps_register_observer(e, obs, NULL);
    return e;
}

/* REMOVE_CANCEL used to free the queued backlog with no event at all. */
static void test_unregister_cancel_reports_every_item(void)
{
    gptps *e = open1();
    uint64_t t0;
    int i;
    if (!e) return;
    reset();
    reg(e, "block", task_block, GPTPS_ON_FAILURE_DEAD_LETTER);

    for (i = 0; i < 5; ++i) CHECK(gptps_submit(e, "block", NULL, 0, NULL) == GPTPS_OK);
    t0 = gptps_now_ms(NULL);
    while (get(&n_started) < 1 && gptps_now_ms(NULL) - t0 < 2000) { }
    CHECK(get(&n_started) == 1);        /* one running, four queued behind it */

    CHECK(gptps_unregister_task(e, "block", GPTPS_REMOVE_CANCEL) == GPTPS_OK);
    CHECK(gptps_shutdown(e) == GPTPS_OK);

    CHECK(get(&n_queued) == 5);
    CHECK(get(&n_terminal) == 5);       /* was 1: the four queued items vanished */
}

/* The DROP policy already emitted EV_DROPPED from the normal path; check the
 * removal path agrees rather than free-ing silently. */
static void test_drop_policy_reports_every_item(void)
{
    gptps *e = open1();
    int i;
    if (!e) return;
    reset();
    reg(e, "bad", task_fail, GPTPS_ON_FAILURE_DROP);

    for (i = 0; i < 6; ++i) CHECK(gptps_submit(e, "bad", NULL, 0, NULL) == GPTPS_OK);
    CHECK(gptps_shutdown(e) == GPTPS_OK);

    CHECK(get(&n_queued) == 6);
    CHECK(get(&n_terminal) == 6);
}

/* An item cancelled while RUNNING already got its terminal event from the
 * executor; the done-drain must not emit a second one. */
static void test_running_cancel_is_not_double_counted(void)
{
    gptps *e = open1();
    gptps_handle h = 0;
    uint64_t t0;
    if (!e) return;
    reset();
    reg(e, "block", task_block, GPTPS_ON_FAILURE_DEAD_LETTER);

    CHECK(gptps_submit(e, "block", NULL, 0, &h) == GPTPS_OK);
    t0 = gptps_now_ms(NULL);
    while (get(&n_started) < 1 && gptps_now_ms(NULL) - t0 < 2000) { }
    CHECK(get(&n_started) == 1);

    CHECK(gptps_cancel(e, h) == GPTPS_OK);
    CHECK(gptps_shutdown(e) == GPTPS_OK);

    CHECK(get(&n_queued) == 1);
    CHECK(get(&n_terminal) == 1);       /* exactly one, not zero and not two */
    CHECK(get(&n_cancelled) == 1);      /* and it says CANCELLED, not TIMEOUT */
    CHECK(get(&n_timeout) == 0);
}

/* The other half of the distinction: a real deadline still reports E_TIMEOUT. */
static void test_timeout_still_reports_timeout(void)
{
    gptps *e = open1();
    gptps_submit_options o;
    if (!e) return;
    reset();
    reg(e, "block", task_block, GPTPS_ON_FAILURE_DROP);

    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.flags = GPTPS_SUBMIT_TIMEOUT_MS;
    o.timeout_ms = 150;
    CHECK(gptps_submit_ex(e, "block", NULL, 0, &o, NULL) == GPTPS_OK);

    CHECK(gptps_shutdown(e) == GPTPS_OK);
    CHECK(get(&n_timeout) == 1);
    CHECK(get(&n_cancelled) == 0);
}

int main(void)
{
    test_unregister_cancel_reports_every_item();
    test_drop_policy_reports_every_item();
    test_running_cancel_is_not_double_counted();
    test_timeout_still_reports_timeout();

    if (fails) { printf("%d reconcile check(s) FAILED\n", fails); return 1; }
    printf("all reconcile checks passed\n");
    return 0;
}
