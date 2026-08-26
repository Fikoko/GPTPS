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

/* --------------------- a deny-all constraint over more items than the engine's
 *                        per-pass event buffer holds                           */

static gptps_status task_ok(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }

static gptps_admit_decision deny_all(const gptps_constraint_input *in,
                                     uint32_t *retry_after_ms, void *ud)
{ (void)in; (void)retry_after_ms; (void)ud; return GPTPS_DENY; }

/* Bigger than GPTPS_PENDING_CAP (256), the buffer engine_pass fills for the
 * caller to emit with the lock released. */
#define DENY_N 600

/* GPTPS_DENY does not raise e->running, so nothing throttles the admission loop:
 * it could deny an ENTIRE intake queue in one pass - and intake is unbounded by
 * default. Past 256 denials the pass simply stopped recording events while it
 * kept dead-lettering items. Measured: 600 submitted, 600 QUEUED, 256 terminal -
 * 344 handles silently lost, which is exactly the reconciliation hole this file
 * exists to prevent. A full buffer must DEFER the rest of the work to the next
 * pass, never drop the event.
 *
 * MANUAL mode is what makes this exact rather than statistical: gptps_step runs
 * no threads, so the whole 600-item queue is present for the pass, and the step
 * is required to keep pumping while events are still owed. */
static void test_deny_over_event_buffer_reports_every_item(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    size_t ran = 1;
    int i;

    memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 2;
    cfg.mode = GPTPS_RUN_MANUAL;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (!e) return;
    gptps_register_observer(e, obs, NULL);
    reset();
    reg(e, "denied", task_ok, GPTPS_ON_FAILURE_DEAD_LETTER);
    CHECK(gptps_register_constraint(e, deny_all, NULL) == GPTPS_OK);

    for (i = 0; i < DENY_N; ++i) CHECK(gptps_submit(e, "denied", NULL, 0, NULL) == GPTPS_OK);
    CHECK(gptps_step(e, &ran) == GPTPS_OK);
    CHECK(ran == 0);                        /* denied at admission: nothing ever ran */

    CHECK(get(&n_queued) == DENY_N);
    CHECK(get(&n_terminal) == DENY_N);      /* was 256 */

    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

/* ------------------------- a SERVICE instance still queued at gptps_shutdown */

static void reg_service(gptps *e, const char *name, gptps_run_fn fn)
{
    gptps_task_def d; memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = fn; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.flags = GPTPS_TASK_SERVICE;      /* services are THREADED + INPROC only */
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

/* Teardown stops services first so the dispatcher can reach its drain condition,
 * and the instances that had reserved no admission budget - queued in intake, or
 * sitting in restart backoff - used to be FREED OUTRIGHT there, with no terminal
 * event at all. Measured: queued=1 terminal=0. That is the steady state for a
 * service given the restart floor, so it was not an edge case.
 *
 * The single worker is occupied by an ordinary task first, so the service is
 * guaranteed to still be in intake when shutdown runs. The short grace bounds how
 * long teardown waits on that (non-service) task; the default is 30s. */
static void test_queued_service_reports_a_terminal_event(void)
{
    gptps *e = open1();
    uint64_t t0;
    if (!e) return;
    reset();
    CHECK(gptps_settings_set(e, "limits.shutdown_grace_ms", "200") == GPTPS_OK);
    reg(e, "hog", task_block, GPTPS_ON_FAILURE_DEAD_LETTER);
    reg_service(e, "svc", task_block);

    CHECK(gptps_submit(e, "hog", NULL, 0, NULL) == GPTPS_OK);
    t0 = gptps_now_ms(NULL);
    while (get(&n_started) < 1 && gptps_now_ms(NULL) - t0 < 2000) { }
    CHECK(get(&n_started) == 1);        /* the only slot is taken */

    CHECK(gptps_submit(e, "svc", NULL, 0, NULL) == GPTPS_OK);   /* queues behind it */
    CHECK(get(&n_queued) == 2);
    CHECK(get(&n_started) == 1);        /* the service never got to start */

    CHECK(gptps_shutdown(e) == GPTPS_OK);

    CHECK(get(&n_terminal) == 2);       /* was 1: the queued service vanished */
    CHECK(get(&n_cancelled) == 2);      /* both stopped, neither timed out */
    CHECK(get(&n_timeout) == 0);
}

int main(void)
{
    test_unregister_cancel_reports_every_item();
    test_drop_policy_reports_every_item();
    test_running_cancel_is_not_double_counted();
    test_timeout_still_reports_timeout();
    test_deny_over_event_buffer_reports_every_item();
    test_queued_service_reports_a_terminal_event();

    if (fails) { printf("%d reconcile check(s) FAILED\n", fails); return 1; }
    printf("all reconcile checks passed\n");
    return 0;
}
