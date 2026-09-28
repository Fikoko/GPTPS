/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_reconcile.c - every submitted ONE-SHOT handle reaches EXACTLY ONE terminal
 * event. The two opt-in shapes outside that rule - GPTPS_ON_FAILURE_REQUEUE, which
 * stays open while it requeues, and GPTPS_TASK_SERVICE, which emits one per run -
 * are documented in Readme.md and gptps.h. Both still have to CLOSE when the engine
 * does, and test_queued_service_reports_a_terminal_event and
 * test_requeue_is_closed_by_the_drain pin exactly that: whatever a policy does while
 * the engine runs, teardown owes every live handle a terminal event. The cases after
 * them pin the same for work parked BETWEEN attempts.
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
static int n_cancelled, n_timeout, n_retried, n_failed, n_selfcancel;

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
        case GPTPS_EV_RETRIED: inc(&n_retried); break;
        case GPTPS_EV_FINISHED:
        case GPTPS_EV_DROPPED:
        case GPTPS_EV_DEAD_LETTERED:
            inc(&n_terminal); break;
        case GPTPS_EV_FAILED:
            inc(&n_failed);
            if (ev->status == GPTPS_E_CANCELLED) { inc(&n_cancelled); inc(&n_terminal); }
            if (ev->flags & GPTPS_EV_FLAG_SELF_CANCELLED) inc(&n_selfcancel);
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
    __atomic_store_n(&n_retried, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&n_failed, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&n_selfcancel, 0, __ATOMIC_SEQ_CST);
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
    CHECK(get(&n_selfcancel) == 0);     /* stopped from outside, not by its body */
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

/* A REQUEUE item is re-admitted instead of ending, so while the engine runs it owes
 * no terminal event - that is the policy working, not a gap. Shutdown is different:
 * the drain refuses to re-admit it (an always-failing requeue would hang teardown
 * forever) and dead-letters it instead, and that disposition used to be SILENT.
 * Measured before the fix: 8 of 8 handles ended with zero terminal events after
 * gptps_shutdown had returned. It was the one shape that could reach teardown and
 * close without saying so - a gptps_await on such a handle never returned, and every
 * add-on that reconciles terminal events leaked a slot per item. */
static void test_requeue_is_closed_by_the_drain(void)
{
    gptps *e = open1();
    uint64_t t0;
    if (!e) return;
    reset();
    reg(e, "rq", task_fail, GPTPS_ON_FAILURE_REQUEUE);

    CHECK(gptps_submit(e, "rq", NULL, 0, NULL) == GPTPS_OK);
    /* wait for a second attempt: proof it is really looping, not merely failing once */
    t0 = gptps_now_ms(NULL);
    while (get(&n_started) < 2 && gptps_now_ms(NULL) - t0 < 3000) { }
    CHECK(get(&n_started) >= 2);
    CHECK(get(&n_terminal) == 0);       /* and owes nothing at all while it loops */

    CHECK(gptps_shutdown(e) == GPTPS_OK);

    CHECK(get(&n_terminal) == 1);       /* was 0: the drain dead-lettered it in silence */
    CHECK(get(&n_timeout) == 0);        /* a drain is not a deadline breach */
}

/* ------------------ work parked BETWEEN attempts, then removed or torn down */

/* An item waiting in backoff - a bounded retry, a REQUEUE, a service restart - still
 * carries `started` from the attempt before, and the one path that terminates such an
 * item without running it, drain_cancelled(), read `started` as "execute() already
 * reported this handle closed". So REMOVE_CANCEL, and teardown, freed it in silence:
 * its last event was a plain per-attempt FAILED (or, for a service, the FINISHED of
 * one run), and nothing ever closed it. The same test freed an item whose attempt had
 * merely FAILED and was awaiting its retry decision in `done`. A 60 s backoff keeps
 * every item parked for the whole case. */

static void reg_parked(gptps *e, const char *name, gptps_run_fn fn, gptps_on_failure onfail,
                       uint32_t retries, uint64_t flags)
{
    gptps_task_def d; memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = fn; d.exec = GPTPS_EXEC_INPROC;
    d.flags = flags;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.on_failure = onfail;
    d.default_policy.max_retries = retries;
    d.default_policy.retry_backoff_seconds = 60;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

/* Until `name`'s one item has run at least once and now waits in backoff with
 * nothing admitted - i.e. it is parked in `delayed`. `ended` counts an event the
 * engine emits only once the attempt is over (FAILED/FINISHED from execute(), or
 * the dispatcher's RETRIED), and it is read BEFORE the snapshot: a snapshot taken
 * first could show the item still waiting in intake, before its attempt started. */
static int wait_parked(gptps *e, const char *name, int *ended)
{
    uint64_t t0 = gptps_now_ms(NULL);
    for (;;) {
        size_t i, n = gptps_task_count(e);
        for (i = 0; i < n; ++i) {
            gptps_task_info ti; int over = get(ended) >= 1;
            memset(&ti, 0, sizeof ti); ti.struct_size = sizeof ti;
            if (over && gptps_task_get_info(e, i, &ti) == GPTPS_OK && strcmp(ti.name, name) == 0 &&
                ti.queued == 1 && ti.running == 0) return 1;
        }
        if (gptps_now_ms(NULL) - t0 > 3000) return 0;
    }
}

static gptps *open_manual(uint32_t slots)
{
    gptps *e = NULL;
    gptps_config cfg;
    memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg; cfg.mode = GPTPS_RUN_MANUAL;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = slots;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (e) gptps_register_observer(e, obs, NULL);
    return e;
}

static void test_cancel_reports_a_retry_in_backoff(void)
{
    gptps *e = open1();
    if (!e) return;
    reset();
    reg_parked(e, "flaky", task_fail, GPTPS_ON_FAILURE_DEAD_LETTER, 1, 0);
    CHECK(gptps_submit(e, "flaky", NULL, 0, NULL) == GPTPS_OK);
    CHECK(wait_parked(e, "flaky", &n_retried));   /* RETRIED is emitted after the park */
    CHECK(get(&n_retried) == 1 && get(&n_terminal) == 0);
    CHECK(gptps_unregister_task(e, "flaky", GPTPS_REMOVE_CANCEL) == GPTPS_OK);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    CHECK(get(&n_terminal) == 1);       /* was 0 */
    CHECK(get(&n_cancelled) == 1);
}

static void test_cancel_reports_a_requeue_in_backoff(void)
{
    gptps *e = open1();
    if (!e) return;
    reset();
    reg_parked(e, "rq", task_fail, GPTPS_ON_FAILURE_REQUEUE, 0, 0);
    CHECK(gptps_submit(e, "rq", NULL, 0, NULL) == GPTPS_OK);
    CHECK(wait_parked(e, "rq", &n_failed));
    CHECK(gptps_unregister_task(e, "rq", GPTPS_REMOVE_CANCEL) == GPTPS_OK);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    CHECK(get(&n_terminal) == 1);       /* was 0 */
    CHECK(get(&n_cancelled) == 1);
}

/* A clean exit closes one RUN of an always-up service, not the instance: gptps.h
 * promises the instance exactly one FAILED/E_CANCELLED when it is stopped. */
static void test_shutdown_stops_a_service_in_restart_backoff(void)
{
    gptps *e = open1();
    if (!e) return;
    reset();
    reg_parked(e, "svc", task_ok, GPTPS_ON_FAILURE_REQUEUE, 0, GPTPS_TASK_SERVICE);
    CHECK(gptps_submit(e, "svc", NULL, 0, NULL) == GPTPS_OK);
    CHECK(wait_parked(e, "svc", &n_terminal));   /* ran once, exited cleanly (FINISHED), waits to restart */
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    CHECK(get(&n_cancelled) == 1);      /* was 0: the instance was never closed */
    CHECK(get(&n_terminal) == 2);       /* the run's FINISHED + the stop */
}

static void test_manual_teardown_and_cancel_report_a_parked_retry(void)
{
    gptps *e;
    size_t ran = 0;
    /* a MANUAL host that stops stepping with a retry parked, then shuts down */
    e = open_manual(1);
    if (!e) return;
    reset();
    reg_parked(e, "flaky", task_fail, GPTPS_ON_FAILURE_DEAD_LETTER, 1, 0);
    CHECK(gptps_submit(e, "flaky", NULL, 0, NULL) == GPTPS_OK);
    CHECK(gptps_step(e, &ran) == GPTPS_OK && ran == 1 && get(&n_retried) == 1);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    CHECK(get(&n_terminal) == 1);       /* was 0 */
    /* ...and one that removes the type instead */
    e = open_manual(1);
    if (!e) return;
    reset();
    reg_parked(e, "flaky", task_fail, GPTPS_ON_FAILURE_DEAD_LETTER, 1, 0);
    CHECK(gptps_submit(e, "flaky", NULL, 0, NULL) == GPTPS_OK);
    CHECK(gptps_step(e, &ran) == GPTPS_OK && ran == 1 && get(&n_retried) == 1);
    CHECK(gptps_unregister_task(e, "flaky", GPTPS_REMOVE_CANCEL) == GPTPS_OK);
    CHECK(get(&n_terminal) == 1);       /* was 0 */
    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

/* MANUAL: an attempt that FAILED sits in `done` until the step's accounting pass
 * decides retry or dead-letter. Removing its type in between - here from the next
 * item's STARTED callback, inside the same step - must still close it. */
static gptps *manual_e;
static int st_remove_u = -1;
static void remove_u_on_t_start(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_STARTED && strcmp(ev->task_name, "t") == 0)
        st_remove_u = (int)gptps_unregister_task(manual_e, "u", GPTPS_REMOVE_CANCEL);
}
static void test_manual_cancel_reports_a_failed_attempt_awaiting_accounting(void)
{
    size_t ran = 0;
    manual_e = open_manual(2);
    if (!manual_e) return;
    reset();
    reg(manual_e, "u", task_fail, GPTPS_ON_FAILURE_DEAD_LETTER);
    reg(manual_e, "t", task_ok, GPTPS_ON_FAILURE_DEAD_LETTER);
    CHECK(gptps_set_event_cb(manual_e, remove_u_on_t_start, NULL) == GPTPS_OK);
    CHECK(gptps_submit(manual_e, "u", NULL, 0, NULL) == GPTPS_OK);   /* runs first, fails */
    CHECK(gptps_submit(manual_e, "t", NULL, 0, NULL) == GPTPS_OK);
    CHECK(gptps_step(manual_e, &ran) == GPTPS_OK && ran == 2);
    CHECK(st_remove_u == GPTPS_OK);
    CHECK(gptps_shutdown(manual_e) == GPTPS_OK);
    CHECK(get(&n_terminal) == 2);       /* t's FINISHED + u's cancel; was 1 */
    CHECK(get(&n_cancelled) == 1);
}

/* ...and the converse: an attempt that was cancelled while it ran has ALREADY been
 * closed by its FAILED/E_CANCELLED, so removing its type before the accounting pass
 * must not report it a second time. */
static gptps_status task_until_cancelled(gptps_ctx *c, void *u)
{ (void)u; return gptps_is_cancelled(c) ? GPTPS_E_CANCELLED : GPTPS_OK; }
static void cancel_c_then_remove_it(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind != GPTPS_EV_STARTED) return;
    if (strcmp(ev->task_name, "c") == 0) (void)gptps_cancel(manual_e, ev->handle);   /* running */
    if (strcmp(ev->task_name, "t") == 0)
        st_remove_u = (int)gptps_unregister_task(manual_e, "c", GPTPS_REMOVE_CANCEL);
}
static void test_manual_cancel_does_not_repeat_a_reported_cancel(void)
{
    size_t ran = 0;
    manual_e = open_manual(2);
    if (!manual_e) return;
    reset();
    st_remove_u = -1;
    reg(manual_e, "c", task_until_cancelled, GPTPS_ON_FAILURE_DEAD_LETTER);
    reg(manual_e, "t", task_ok, GPTPS_ON_FAILURE_DEAD_LETTER);
    CHECK(gptps_set_event_cb(manual_e, cancel_c_then_remove_it, NULL) == GPTPS_OK);
    CHECK(gptps_submit(manual_e, "c", NULL, 0, NULL) == GPTPS_OK);
    CHECK(gptps_submit(manual_e, "t", NULL, 0, NULL) == GPTPS_OK);
    CHECK(gptps_step(manual_e, &ran) == GPTPS_OK && ran == 2);
    CHECK(st_remove_u == GPTPS_OK);
    CHECK(gptps_shutdown(manual_e) == GPTPS_OK);
    CHECK(get(&n_cancelled) == 1);      /* c's own, once */
    CHECK(get(&n_terminal) == 2);       /* + t's FINISHED */
}

/* ...and a one-shot whose attempt FINISHED is closed by that event: removing its type
 * before the accounting pass must not report it again. */
static void remove_a_on_t_start(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_STARTED && strcmp(ev->task_name, "t") == 0)
        st_remove_u = (int)gptps_unregister_task(manual_e, "a", GPTPS_REMOVE_CANCEL);
}
static void test_manual_cancel_does_not_repeat_a_finished_one_shot(void)
{
    size_t ran = 0;
    manual_e = open_manual(2);
    if (!manual_e) return;
    reset();
    st_remove_u = -1;
    reg(manual_e, "a", task_ok, GPTPS_ON_FAILURE_DEAD_LETTER);
    reg(manual_e, "t", task_ok, GPTPS_ON_FAILURE_DEAD_LETTER);
    CHECK(gptps_set_event_cb(manual_e, remove_a_on_t_start, NULL) == GPTPS_OK);
    CHECK(gptps_submit(manual_e, "a", NULL, 0, NULL) == GPTPS_OK);   /* runs first, finishes */
    CHECK(gptps_submit(manual_e, "t", NULL, 0, NULL) == GPTPS_OK);
    CHECK(gptps_step(manual_e, &ran) == GPTPS_OK && ran == 2);
    CHECK(st_remove_u == GPTPS_OK);
    CHECK(gptps_shutdown(manual_e) == GPTPS_OK);
    CHECK(get(&n_terminal) == 2);       /* a's FINISHED + t's, and nothing more */
    CHECK(get(&n_cancelled) == 0);
}

/* ------------- a stop that lands after the attempt read its cancel flag */

/* execute() reads the cancel flag once, when the body returns. A gptps_cancel that
 * lands after that read - from the attempt's own FAILED or FINISHED callback,
 * deterministically, or from another thread in that window - or a REMOVE_CANCEL
 * from another thread in that window (from the callback itself it is refused with
 * GPTPS_E_BUSY) finds the item not yet accounted for and stops it, but the attempt
 * has already reported its own outcome. For a failing one-shot that was a
 * per-attempt FAILED, and for an always-up service the FINISHED of one run: neither
 * is terminal. The done-drain then freed the item with no terminal event: it read
 * `started` alone as "closed" for the failed attempt, and did not check the
 * service's OK run at all. Measured before: cancelled 0 in every case below, and
 * terminal 0 in the one-shot ones. The attempt's own events are sent in full first,
 * so the terminal FAILED / GPTPS_E_CANCELLED follows them. */
static gptps_status own_stop_st;
static int          own_stop_calls;
static void cancel_self_on_failed(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_FAILED && ev->status == GPTPS_E_TASK) {
        own_stop_st = gptps_cancel(manual_e, ev->handle);
        inc(&own_stop_calls);
    }
}
/* Holds the worker in the attempt's FAILED callback until the main thread's
 * REMOVE_CANCEL has tombstoned the type (the callback itself may not unregister:
 * that would wait on its own attempt, so it is refused with GPTPS_E_BUSY). */
static int u_removed(void)
{
    size_t i, n = gptps_task_count(manual_e);
    for (i = 0; i < n; ++i) {
        gptps_task_info ti;
        memset(&ti, 0, sizeof ti); ti.struct_size = sizeof ti;
        if (gptps_task_get_info(manual_e, i, &ti) == GPTPS_OK && strcmp(ti.name, "u") == 0)
            return ti.removed;
    }
    return 1;                           /* already gone */
}
static void hold_failed_until_removed(const gptps_event *ev, void *ud)
{
    uint64_t t0;
    (void)ud;
    if (ev->kind != GPTPS_EV_FAILED || ev->status != GPTPS_E_TASK) return;
    inc(&own_stop_calls);
    t0 = gptps_now_ms(NULL);
    while (!u_removed() && gptps_now_ms(NULL) - t0 < 3000) { }
}
static void cancel_self_on_finished(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_FINISHED && get(&own_stop_calls) == 0) {
        own_stop_st = gptps_cancel(manual_e, ev->handle);
        inc(&own_stop_calls);
    }
}
static void reset_own_stop(void)
{
    own_stop_st = GPTPS_E_INVAL;
    __atomic_store_n(&own_stop_calls, 0, __ATOMIC_SEQ_CST);
}

/* MANUAL, cancel: with and without a retry left - the cancel must stop the retry too */
static void test_manual_cancel_after_a_failed_attempt_reported(void)
{
    uint32_t retries;
    for (retries = 0; retries <= 1; ++retries) {
        size_t ran = 0;
        manual_e = open_manual(1);
        if (!manual_e) return;
        reset(); reset_own_stop();
        reg_parked(manual_e, "u", task_fail, GPTPS_ON_FAILURE_DEAD_LETTER, retries, 0);
        CHECK(gptps_set_event_cb(manual_e, cancel_self_on_failed, NULL) == GPTPS_OK);
        CHECK(gptps_submit(manual_e, "u", NULL, 0, NULL) == GPTPS_OK);
        CHECK(gptps_step(manual_e, &ran) == GPTPS_OK && ran == 1);
        CHECK(get(&own_stop_calls) == 1 && own_stop_st == GPTPS_OK);
        CHECK(get(&n_terminal) == 1);   /* was 0 */
        CHECK(get(&n_cancelled) == 1);
        CHECK(get(&n_retried) == 0);    /* cancelled, so never retried */
        CHECK(gptps_shutdown(manual_e) == GPTPS_OK);
        CHECK(get(&n_terminal) == 1);   /* and teardown adds nothing */
    }
}

/* THREADED, REMOVE_CANCEL of the type while its attempt's FAILED is being delivered */
static void test_remove_after_a_failed_attempt_reported(void)
{
    uint64_t t0;
    manual_e = open1();
    if (!manual_e) return;
    reset(); reset_own_stop();
    reg(manual_e, "u", task_fail, GPTPS_ON_FAILURE_DEAD_LETTER);
    CHECK(gptps_set_event_cb(manual_e, hold_failed_until_removed, NULL) == GPTPS_OK);
    CHECK(gptps_submit(manual_e, "u", NULL, 0, NULL) == GPTPS_OK);
    t0 = gptps_now_ms(NULL);
    while (get(&own_stop_calls) < 1 && gptps_now_ms(NULL) - t0 < 3000) { }
    CHECK(get(&own_stop_calls) == 1);   /* the attempt has reported its own FAILED */
    CHECK(gptps_unregister_task(manual_e, "u", GPTPS_REMOVE_CANCEL) == GPTPS_OK);
    CHECK(gptps_shutdown(manual_e) == GPTPS_OK);
    CHECK(get(&n_terminal) == 1);       /* was 0 */
    CHECK(get(&n_cancelled) == 1);
}

/* THREADED: the same cancel, from the callback on the worker that ran the attempt */
static void test_threaded_cancel_after_a_failed_attempt_reported(void)
{
    uint64_t t0;
    manual_e = open1();                 /* named for MANUAL; the callbacks just need `e` */
    if (!manual_e) return;
    reset(); reset_own_stop();
    reg(manual_e, "u", task_fail, GPTPS_ON_FAILURE_DEAD_LETTER);
    CHECK(gptps_set_event_cb(manual_e, cancel_self_on_failed, NULL) == GPTPS_OK);
    CHECK(gptps_submit(manual_e, "u", NULL, 0, NULL) == GPTPS_OK);
    t0 = gptps_now_ms(NULL);
    while (get(&n_terminal) < 1 && gptps_now_ms(NULL) - t0 < 3000) { }
    CHECK(gptps_shutdown(manual_e) == GPTPS_OK);
    CHECK(get(&own_stop_calls) == 1 && own_stop_st == GPTPS_OK);
    CHECK(get(&n_terminal) == 1);       /* was 0: not dead-lettered, not closed at all */
    CHECK(get(&n_cancelled) == 1);
}

/* THREADED service: cancelled from the FINISHED of its first run. That FINISHED ends
 * the run; the stop must still close the handle, exactly once. */
static void test_service_cancel_after_a_run_finished(void)
{
    uint64_t t0;
    manual_e = open1();
    if (!manual_e) return;
    reset(); reset_own_stop();
    reg_service(manual_e, "svc", task_ok);          /* every run returns OK at once */
    CHECK(gptps_set_event_cb(manual_e, cancel_self_on_finished, NULL) == GPTPS_OK);
    CHECK(gptps_submit(manual_e, "svc", NULL, 0, NULL) == GPTPS_OK);
    t0 = gptps_now_ms(NULL);
    while (get(&n_cancelled) < 1 && gptps_now_ms(NULL) - t0 < 3000) { }
    CHECK(gptps_shutdown(manual_e) == GPTPS_OK);
    CHECK(get(&own_stop_calls) == 1 && own_stop_st == GPTPS_OK);
    CHECK(get(&n_cancelled) == 1);      /* was 0: the instance never closed */
    CHECK(get(&n_terminal) == 2);       /* the run's FINISHED + the stop, once */
    CHECK(get(&n_started) == 1);        /* and it did not restart after the stop */
}

/* A body that returns GPTPS_E_CANCELLED itself - not cancelled by anyone - ends its
 * item as a cancel would. Its FAILED / GPTPS_E_CANCELLED is what observers
 * reconciling handles count as terminal, and the engine used to retry it anyway:
 * with two retries and no backoff, three attempts, two RETRIEDs and a dead letter,
 * all after observers had closed the handle. Its FAILED says the cancel was the
 * body's own. */
static gptps_status task_says_cancelled(gptps_ctx *c, void *u)
{ (void)c; (void)u; return GPTPS_E_CANCELLED; }
static void test_body_returning_cancelled_is_final(void)
{
    gptps *e = open1();
    gptps_task_def d;
    uint64_t t0;
    if (!e) return;
    reset();
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "sc"; d.run = task_says_cancelled; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = 2;                   /* no backoff: a retry would show */
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(gptps_submit(e, "sc", NULL, 0, NULL) == GPTPS_OK);
    t0 = gptps_now_ms(NULL);
    while (get(&n_terminal) < 1 && gptps_now_ms(NULL) - t0 < 3000) { }
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    CHECK(get(&n_started) == 1);        /* was 3: retried twice */
    CHECK(get(&n_retried) == 0);        /* was 2 */
    CHECK(get(&n_terminal) == 1);       /* its own FAILED / E_CANCELLED, once; was 4 */
    CHECK(get(&n_cancelled) == 1);
    CHECK(get(&n_selfcancel) == 1);     /* and marked as the body's own */
}

int main(void)
{
    test_unregister_cancel_reports_every_item();
    test_drop_policy_reports_every_item();
    test_running_cancel_is_not_double_counted();
    test_timeout_still_reports_timeout();
    test_deny_over_event_buffer_reports_every_item();
    test_queued_service_reports_a_terminal_event();
    test_requeue_is_closed_by_the_drain();
    test_cancel_reports_a_retry_in_backoff();
    test_cancel_reports_a_requeue_in_backoff();
    test_shutdown_stops_a_service_in_restart_backoff();
    test_manual_teardown_and_cancel_report_a_parked_retry();
    test_manual_cancel_reports_a_failed_attempt_awaiting_accounting();
    test_manual_cancel_does_not_repeat_a_reported_cancel();
    test_manual_cancel_does_not_repeat_a_finished_one_shot();
    test_manual_cancel_after_a_failed_attempt_reported();
    test_remove_after_a_failed_attempt_reported();
    test_threaded_cancel_after_a_failed_attempt_reported();
    test_service_cancel_after_a_run_finished();
    test_body_returning_cancelled_is_final();

    if (fails) { printf("%d reconcile check(s) FAILED\n", fails); return 1; }
    printf("all reconcile checks passed\n");
    return 0;
}
