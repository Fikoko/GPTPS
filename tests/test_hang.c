/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_hang.c - teardown must always TERMINATE.
 *
 * Every check here is a way teardown used to go wrong: a re-entrant
 * gptps_shutdown that joins its own thread, a no-timeout external child that
 * outlives its pipe, a zero-backoff supervised service that re-admits itself as
 * fast as the dispatcher loops, an undrained dead-letter list that grows without
 * bound, a constraint that keeps deferring an item past the grace, queued work
 * still admitted past it, and a gptps_shutdown from a callback the engine made on
 * the host's own thread. All but the last share a property that makes them worth
 * one file: the failure mode is a HANG or unbounded growth, not a wrong answer -
 * so the CTest TIMEOUT on this test is itself part of the assertion. The last
 * freed the engine under its caller instead: case 10 checks the return codes, and
 * ASan the use-after-free.
 *
 * HELPER_PATH is the prog_helper binary (defined by CMake).
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }

/* All cross-thread state goes through the atomic helpers: these are written on a
 * worker / the dispatcher and read on the main thread. */
static gptps *g_engine;
static int    g_cb_shutdown_rc = 999;
static int    g_task_shutdown_rc = 999;
static int    g_service_runs;
static void set(int *p, int v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }

static void reg_inproc(gptps *e, const char *name, gptps_run_fn fn, uint64_t flags)
{
    gptps_task_def d; memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = fn; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.flags = flags;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

/* ---------------------------------------------------------------- 1. re-entry */

static gptps_status task_noop(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }

/* Calling shutdown from a task body would make this worker join itself. */
static gptps_status task_shuts_down(gptps_ctx *c, void *u)
{
    (void)c; (void)u;
    set(&g_task_shutdown_rc, (int)gptps_shutdown(g_engine));
    return GPTPS_OK;
}

/* Same hazard from an event callback (runs on a worker / the dispatcher). */
static void cb_shuts_down(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_FINISHED && get(&g_cb_shutdown_rc) == 999)
        set(&g_cb_shutdown_rc, (int)gptps_shutdown(g_engine));
}

static void test_reentrant_shutdown_is_refused(void)
{
    gptps *e = NULL;
    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    if (!e) return;
    g_engine = e;
    gptps_set_event_cb(e, cb_shuts_down, NULL);
    reg_inproc(e, "noop", task_noop, 0);
    reg_inproc(e, "suicide", task_shuts_down, 0);

    CHECK(gptps_submit(e, "noop", NULL, 0, NULL) == GPTPS_OK);
    CHECK(gptps_submit(e, "suicide", NULL, 0, NULL) == GPTPS_OK);

    /* If either re-entrant call had gone through, this would never return. */
    CHECK(gptps_shutdown(e) == GPTPS_OK);

    CHECK(get(&g_cb_shutdown_rc) == (int)GPTPS_E_BUSY);    /* refused, not deadlocked */
    CHECK(get(&g_task_shutdown_rc) == (int)GPTPS_E_BUSY);
    g_engine = NULL;
}

/* ------------------------------------------------------- 2. re-entrant step() */

static gptps_status task_steps(gptps_ctx *c, void *u)
{
    (void)c; (void)u;
    set(&g_task_shutdown_rc, (int)gptps_step(g_engine, NULL));
    return GPTPS_OK;
}

static void test_reentrant_step_is_refused(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    size_t ran = 0;

    memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.mode = GPTPS_RUN_MANUAL;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (!e) return;
    g_engine = e;
    set(&g_task_shutdown_rc, 999);
    reg_inproc(e, "recurse", task_steps, 0);
    CHECK(gptps_submit(e, "recurse", NULL, 0, NULL) == GPTPS_OK);
    CHECK(gptps_step(e, &ran) == GPTPS_OK);
    CHECK(ran == 1);
    CHECK(get(&g_task_shutdown_rc) == (int)GPTPS_E_BUSY);   /* the inner pump was refused */
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    g_engine = NULL;
}

/* -------------------------------------- 3. shutdown bounds a no-timeout child */

#if !defined(_WIN32)
static int g_started;

static void obs_started(const gptps_event *ev, void *ud)
{ (void)ud; if (ev->kind == GPTPS_EV_STARTED) inc(&g_started); }

/* A PROGRAM task with timeout_seconds == 0 (what a memset-zero task_def gives
 * you) whose child never exits. Teardown used to wait for it forever. */
static void test_shutdown_bounds_a_stuck_program(const char *mode)
{
    gptps *e = NULL;
    gptps_task_def d;
    const char *argv[3];
    uint64_t t0, elapsed;

    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    if (!e) return;
    /* Keep the bound short so the test is quick; the DEFAULT is 30s. */
    CHECK(gptps_settings_set(e, "limits.shutdown_grace_ms", "700") == GPTPS_OK);
    gptps_register_observer(e, obs_started, NULL);

    argv[0] = HELPER_PATH; argv[1] = mode; argv[2] = NULL;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "stuck"; d.exec = GPTPS_EXEC_PROGRAM; d.argv = argv;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;   /* timeout_seconds = 0 */
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);

    set(&g_started, 0);
    CHECK(gptps_submit(e, "stuck", NULL, 0, NULL) == GPTPS_OK);
    t0 = gptps_now_ms(NULL);
    while (get(&g_started) < 1 && gptps_now_ms(NULL) - t0 < 5000) { }
    CHECK(get(&g_started) == 1);

    t0 = gptps_now_ms(NULL);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    elapsed = gptps_now_ms(NULL) - t0;
    /* The assertion is that this returns AT ALL; the bound proves it was the
     * grace period that ended it and not luck. */
    CHECK(elapsed < 5000);
}
#endif /* !_WIN32 */

/* ----------------------------------------- 4. a zero-backoff service restarts,
 *                                              but does not spin a core          */

static gptps_status svc_exits_immediately(gptps_ctx *c, void *u)
{
    (void)c; (void)u;
    inc(&g_service_runs);
    return GPTPS_E_TASK;         /* fail at once => supervisor restarts us */
}

static void test_service_restart_is_rate_limited(void)
{
    gptps *e = NULL;
    uint64_t t0;
    int runs;

    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    if (!e) return;
    set(&g_service_runs, 0);
    /* retry_backoff_seconds stays 0 - the default a memset gives you. */
    reg_inproc(e, "svc", svc_exits_immediately, GPTPS_TASK_SERVICE);
    CHECK(gptps_submit(e, "svc", NULL, 0, NULL) == GPTPS_OK);

    t0 = gptps_now_ms(NULL);
    while (gptps_now_ms(NULL) - t0 < 600) { }
    runs = get(&g_service_runs);

    CHECK(gptps_shutdown(e) == GPTPS_OK);

    CHECK(runs >= 1);      /* it IS supervised: a failed service does restart */
    /* Floored at ~100ms per restart, so ~6 in 600ms. Without the floor this was
     * bounded only by how fast the dispatcher could loop (hundreds of thousands). */
    CHECK(runs < 100);
}

/* --------------------------------------------- 5. the dead-letter list is capped */

static gptps_status task_always_fails(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_E_TASK; }

static void test_dead_letter_is_capped(void)
{
    gptps *e = NULL;
    char buf[64];
    int i;
    size_t n;

    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    if (!e) return;
    CHECK(gptps_settings_set(e, "limits.max_dead_letters", "8") == GPTPS_OK);
    reg_inproc(e, "bad", task_always_fails, 0);   /* default on_failure = DEAD_LETTER */

    for (i = 0; i < 60; ++i) CHECK(gptps_submit(e, "bad", NULL, 0, NULL) == GPTPS_OK);

    /* Wait for EVERY submitted item to reach a terminal state - queued and running
     * both drained - not merely for the cap to be reached. Waiting on
     * "dead_letter_count >= 8" samples the eviction counter at the exact moment the
     * 8th item filled the list, when nothing has been evicted yet: a race the fast
     * Linux runners happened to win and the Windows runner did not. Draining fully
     * also makes the numbers below EXACT rather than approximate. */
    {
        uint64_t t0 = gptps_now_ms(NULL);
        for (;;) {
            size_t k, ntypes = gptps_task_count(e);
            unsigned busy = 0;
            for (k = 0; k < ntypes; ++k) {
                gptps_task_info ti;
                memset(&ti, 0, sizeof ti); ti.struct_size = sizeof ti;
                if (gptps_task_get_info(e, k, &ti) == GPTPS_OK) busy += ti.queued + ti.running;
            }
            if (busy == 0) break;
            if (gptps_now_ms(NULL) - t0 > 10000) break;   /* safety net; the checks below decide */
        }
    }

    n = gptps_dead_letter_count(e);
    CHECK(n == 8);            /* exactly the cap - it used to grow to 60, and forever in prod */

    /* 60 submitted, 8 retained => 52 evicted, and the truncation is never silent */
    CHECK(gptps_settings_get(e, "stats.dead_letters_evicted", buf, sizeof buf) == GPTPS_OK);
    CHECK(strcmp(buf, "52") == 0);

    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

/* ------------------------------------- 6. the grace-cancel must be TERMINAL,
 *                                          not merely another failed attempt   */

static int g_grace_started;
static int g_body_runs;

static void obs_grace(const gptps_event *ev, void *ud)
{ (void)ud; if (ev->kind == GPTPS_EV_STARTED) inc(&g_grace_started); }

/* A body that never polls gptps_is_cancelled. Nothing can preempt an in-process
 * function, so the only thing a grace period can bound here is how many MORE
 * times the engine runs it - and the answer has to be zero. */
static gptps_status task_noncooperative(gptps_ctx *c, void *u)
{
    uint64_t t0 = gptps_now_ms(NULL);
    (void)c; (void)u;
    inc(&g_body_runs);
    while (gptps_now_ms(NULL) - t0 < 800) { }
    return GPTPS_OK;
}

/* reg_inproc above leaves the memset-zero policy, i.e. max_retries == 0. That is
 * precisely why the two bugs below shipped: with no retries configured neither the
 * retry branch nor the backoff queue is ever reached, so the whole suite exercised
 * the grace only on the one path where it happened to work. */
static void reg_retry(gptps *e, const char *name, gptps_run_fn fn,
                      uint32_t retries, uint32_t backoff_s)
{
    gptps_task_def d; memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = fn; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = retries;
    d.default_policy.retry_backoff_seconds = backoff_s;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

/* Open a threaded engine with a single worker and a SHORT drain bound.
 * limits.shutdown_grace_ms is a live SETTING, not a gptps_limits field - the
 * default is 30s, which no test can afford to wait out. */
static gptps *open_graced(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;   /* deterministic: one runs, the rest queue */
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (e) CHECK(gptps_settings_set(e, "limits.shutdown_grace_ms", "200") == GPTPS_OK);
    return e;
}

/* The grace-cancel used to raise the item's cancel FLAG without also setting
 * it->cancelled - the pairing every other cancel site does. execute() reports a
 * raised flag as GPTPS_E_CANCELLED, so the done-drain saw an ordinary failed
 * attempt, took the RETRY branch, and re-admitted the item with a freshly cleared
 * flag. With max_retries = 3 a 200ms grace made shutdown take 3.90s and run the
 * body FOUR times - four times LONGER than having no grace at all, and it threw
 * away a result the body had already produced. A grace is a bound only if it is
 * terminal. */
static void test_grace_cancel_is_terminal(void)
{
    gptps *e = open_graced();
    uint64_t t0, elapsed;

    if (!e) return;
    gptps_register_observer(e, obs_grace, NULL);
    set(&g_grace_started, 0);
    set(&g_body_runs, 0);
    reg_retry(e, "noncoop", task_noncooperative, 3, 0);
    CHECK(gptps_submit(e, "noncoop", NULL, 0, NULL) == GPTPS_OK);

    t0 = gptps_now_ms(NULL);
    while (get(&g_grace_started) < 1 && gptps_now_ms(NULL) - t0 < 5000) { }
    CHECK(get(&g_grace_started) == 1);

    t0 = gptps_now_ms(NULL);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    elapsed = gptps_now_ms(NULL) - t0;

    /* Ceiling: the 200ms grace plus the one 800ms body already in flight, with
     * slack for a loaded runner. Retrying even once would blow straight past it. */
    CHECK(elapsed < 1800);
    CHECK(get(&g_body_runs) == 1);      /* was 4: the forced stop was retried */
}

/* --------------------------------- 7. the grace must bound the BACKOFF queue */

static int g_retried;
static int g_backoff_terminal;

static void obs_backoff(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_RETRIED) inc(&g_retried);
    if (ev->kind == GPTPS_EV_DEAD_LETTERED || ev->kind == GPTPS_EV_DROPPED) inc(&g_backoff_terminal);
}

/* The dispatcher refuses to exit while `delayed` is non-empty, and only promotes
 * an item once its backoff has elapsed - so a retry parked in the backoff queue
 * held teardown for the whole backoff no matter what the grace said. Measured:
 * 23.70s with a 0.2s grace and retry_backoff_seconds = 8. Past the deadline the
 * backoff is moot: the queue is terminated by policy and every item still gets
 * the terminal event it owes (so far it has only seen EV_RETRIED). */
static void test_grace_bounds_the_backoff_queue(void)
{
    gptps *e = open_graced();
    uint64_t t0, elapsed;

    if (!e) return;
    gptps_register_observer(e, obs_backoff, NULL);
    set(&g_retried, 0);
    set(&g_backoff_terminal, 0);
    reg_retry(e, "boom", task_always_fails, 3, 8);   /* 3 x 8s of backoff to sit through */
    CHECK(gptps_submit(e, "boom", NULL, 0, NULL) == GPTPS_OK);

    /* Wait for attempt 1 to fail and PARK in `delayed` - that is the state under
     * test; shutting down before it would prove nothing. */
    t0 = gptps_now_ms(NULL);
    while (get(&g_retried) < 1 && gptps_now_ms(NULL) - t0 < 5000) { }
    CHECK(get(&g_retried) == 1);

    t0 = gptps_now_ms(NULL);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    elapsed = gptps_now_ms(NULL) - t0;

    CHECK(elapsed < 2000);              /* was 23700: one whole backoff chain */
    CHECK(get(&g_backoff_terminal) == 1);   /* and it is reported, not just freed */
}

/* ------------------------ 8. the grace ends an item a constraint keeps deferring */

static int g_defer_calls, g_defer_ran, g_defer_dead, g_defer_status, g_defer_flagged;

/* Re-check after 1ms: the item is due again at every wake, which is the state the
 * hang needed - once the deadline passes its own re-check is the only wake left. */
static gptps_admit_decision always_defer(const gptps_constraint_input *in,
                                         uint32_t *retry_after_ms, void *ud)
{
    (void)in; (void)ud;
    inc(&g_defer_calls);
    *retry_after_ms = 1;
    return GPTPS_DEFER;
}

static gptps_status task_counts(gptps_ctx *c, void *u) { (void)c; (void)u; inc(&g_defer_ran); return GPTPS_OK; }

static void obs_defer(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind != GPTPS_EV_DEAD_LETTERED) return;
    set(&g_defer_status, (int)ev->status);
    if (ev->flags & GPTPS_EV_FLAG_SHUTDOWN) inc(&g_defer_flagged);
    inc(&g_defer_dead);
}

/* Step 2 of a dispatcher pass moves parked items that fell due back to intake,
 * and only THEN did the grace end what was left in `delayed` - so an item that was
 * due was never there, and step 4 deferred it again. After the deadline nothing
 * else woke the dispatcher, so every pass found it due: gptps_shutdown spun on it
 * forever. The CTest TIMEOUT is the assertion that it returns. */
static void test_grace_ends_a_deferred_item(void)
{
    gptps *e = open_graced();
    uint64_t t0, elapsed;

    if (!e) return;
    set(&g_defer_calls, 0); set(&g_defer_ran, 0); set(&g_defer_dead, 0);
    set(&g_defer_status, 0); set(&g_defer_flagged, 0);
    gptps_register_observer(e, obs_defer, NULL);
    CHECK(gptps_register_constraint(e, always_defer, NULL) == GPTPS_OK);
    reg_inproc(e, "gated", task_counts, 0);
    CHECK(gptps_submit(e, "gated", NULL, 0, NULL) == GPTPS_OK);

    t0 = gptps_now_ms(NULL);
    while (get(&g_defer_calls) < 3 && gptps_now_ms(NULL) - t0 < 5000) { }
    CHECK(get(&g_defer_calls) >= 3);            /* parked, and cycling */

    t0 = gptps_now_ms(NULL);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    elapsed = gptps_now_ms(NULL) - t0;

    CHECK(elapsed < 2000);                      /* the 200ms grace, not forever */
    CHECK(get(&g_defer_ran) == 0);              /* never admitted */
    CHECK(get(&g_defer_dead) == 1);             /* ended by its policy... */
    CHECK(get(&g_defer_status) == (int)GPTPS_E_SHUTDOWN);
    CHECK(get(&g_defer_flagged) == 1);          /* ...as teardown's doing */
}

/* ------------------------------ 9. past the grace, queued work is ended, not run */

static int g_q_started, g_q_dead, g_q_flagged;

static void obs_queued(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_STARTED) inc(&g_q_started);
    if (ev->kind == GPTPS_EV_DEAD_LETTERED && ev->status == GPTPS_E_SHUTDOWN) {
        if (ev->flags & GPTPS_EV_FLAG_SHUTDOWN) inc(&g_q_flagged);
        inc(&g_q_dead);
    }
}

/* The grace cancelled what was RUNNING, and admission carried on regardless: work
 * still queued was started after the deadline, only to be cancelled on the next
 * pass - or, an in-process body that never polls, run to completion, one queued
 * item after another. Four 800ms bodies behind one worker took ~3.2s to shut down
 * with a 200ms grace. Past the deadline nothing is admitted, and the queue is
 * ended by policy like the backoff queue in case 7. */
static void test_grace_ends_queued_work(void)
{
    gptps *e = open_graced();                   /* one worker: the other three queue */
    uint64_t t0, elapsed;
    int i;

    if (!e) return;
    set(&g_q_started, 0); set(&g_q_dead, 0); set(&g_q_flagged, 0);
    set(&g_body_runs, 0);
    gptps_register_observer(e, obs_queued, NULL);
    reg_inproc(e, "slow", task_noncooperative, 0);
    for (i = 0; i < 4; ++i) CHECK(gptps_submit(e, "slow", NULL, 0, NULL) == GPTPS_OK);

    t0 = gptps_now_ms(NULL);
    while (get(&g_q_started) < 1 && gptps_now_ms(NULL) - t0 < 5000) { }
    CHECK(get(&g_q_started) == 1);

    t0 = gptps_now_ms(NULL);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    elapsed = gptps_now_ms(NULL) - t0;

    CHECK(elapsed < 1800);                      /* the grace + the one body in flight */
    CHECK(get(&g_body_runs) == 1);              /* was 4 */
    CHECK(get(&g_q_started) == 1);
    CHECK(get(&g_q_dead) == 3);
    CHECK(get(&g_q_flagged) == 3);
}

/* ------------------------------ 9b. past the grace, nothing more is admitted */

static int g_blk_started, g_rest_started, g_rest_dead, g_rest_scored;

/* A scheduler hook that only counts: it must not be called past the grace. */
static int64_t sched_count(const gptps_sched_input *in, void *ud)
{ (void)in; (void)ud; inc(&g_rest_scored); return 0; }

static gptps_status task_until_cancelled(gptps_ctx *c, void *u)
{
    (void)u;
    set(&g_blk_started, 1);
    while (!gptps_is_cancelled(c)) { }
    return GPTPS_OK;
}

static void obs_rest(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (strcmp(ev->task_name, "rest") != 0) return;
    if (ev->kind == GPTPS_EV_STARTED) inc(&g_rest_started);
    if (ev->kind == GPTPS_EV_DEAD_LETTERED && ev->status == GPTPS_E_SHUTDOWN &&
        (ev->flags & GPTPS_EV_FLAG_SHUTDOWN)) inc(&g_rest_dead);
}

/* Case 9's three queued items are all ended in the pass the grace expires in. A
 * pass ends at most GPTPS_PENDING_CAP (256) of them, though, so a long queue is
 * still there on the next pass - with a slot free once the running item has been
 * cancelled - and nothing may be admitted from it. Nor may a scheduler hook be
 * run over it: rescoring the whole remainder on every such pass made a shutdown
 * with a long queue quadratic, under the engine lock. */
static void test_grace_admits_nothing(void)
{
    gptps *e = open_graced();                   /* one worker, 200ms grace */
    uint64_t t0;
    int i, scored;

    if (!e) return;
    set(&g_blk_started, 0); set(&g_rest_started, 0); set(&g_rest_dead, 0);
    set(&g_rest_scored, 0);
    gptps_register_observer(e, obs_rest, NULL);
    CHECK(gptps_set_scheduler(e, sched_count, NULL) == GPTPS_OK);
    reg_inproc(e, "blocker", task_until_cancelled, 0);
    reg_inproc(e, "rest", task_noop, 0);
    CHECK(gptps_submit(e, "blocker", NULL, 0, NULL) == GPTPS_OK);
    t0 = gptps_now_ms(NULL);
    while (!get(&g_blk_started) && gptps_now_ms(NULL) - t0 < 5000) { }
    CHECK(get(&g_blk_started) == 1);
    for (i = 0; i < 600; ++i)
        if (gptps_submit(e, "rest", NULL, 0, NULL) != GPTPS_OK) { CHECK(!"submit rest"); break; }

    /* The blocker holds the only slot until the grace, so nothing rescores before
     * it either: any hook call from here on is one past the grace. */
    scored = get(&g_rest_scored);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    CHECK(get(&g_rest_started) == 0);           /* none admitted after the grace */
    CHECK(get(&g_rest_dead) == 600);            /* every one ended, flagged, instead */
    CHECK(get(&g_rest_scored) == scored);       /* and none rescored */
}

/* ---------------- 10. no shutdown or step from a callback on the host's thread */

static int g_rc_queued, g_rc_cancel, g_rc_cancel_step, g_rc_drain, g_rc_watch, g_rc_teardown;
static int g_rc_unreg;

static void obs_host(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_QUEUED && strcmp(ev->task_name, "q") == 0) {
        /* A guarded call of its own first: the thread is still inside the QUEUED
         * callback when that call returns. */
        CHECK(gptps_settings_set(g_engine, "limits.max_dead_letters", "512") == GPTPS_OK);
        set(&g_rc_queued, (int)gptps_shutdown(g_engine));
    }
    if (ev->kind == GPTPS_EV_FAILED && ev->status == GPTPS_E_CANCELLED) {
        if (strcmp(ev->task_name, "c") == 0) {
            set(&g_rc_cancel, (int)gptps_shutdown(g_engine));
            set(&g_rc_cancel_step, (int)gptps_step(g_engine, NULL));
        }
        if (strcmp(ev->task_name, "u") == 0 && get(&g_rc_unreg) == 999)
            set(&g_rc_unreg, (int)gptps_shutdown(g_engine));
        if (strcmp(ev->task_name, "t") == 0)
            set(&g_rc_teardown, (int)gptps_shutdown(g_engine));
    }
}

/* The event callback's half of submit's guard (the case above uses an observer). */
static int g_rc_evcb;
static void evcb_shuts_down(const gptps_event *ev, void *ud)
{ (void)ud; if (ev->kind == GPTPS_EV_QUEUED) set(&g_rc_evcb, (int)gptps_shutdown(g_engine)); }

static void drain_shuts_down(const gptps_dead_letter *dl, void *ud)
{ (void)dl; (void)ud; set(&g_rc_drain, (int)gptps_shutdown(g_engine)); }

/* A host setting whose write accessor shuts down: gptps_settings_reload runs it. */
static int g_rc_reload;
static size_t knob_rd(void *t, char *b, size_t c) { (void)t; return (size_t)snprintf(b, c, "%s", "x"); }
static gptps_status knob_wr(void *t, const char *v)
{ (void)t; (void)v; set(&g_rc_reload, (int)gptps_shutdown(g_engine)); return GPTPS_OK; }

static void watch_shuts_down(const char *key, const char *value, void *ud)
{
    (void)value; (void)ud;
    if (strcmp(key, "limits.max_dead_letters") == 0) set(&g_rc_watch, (int)gptps_shutdown(g_engine));
}

/* Case 1 covers callbacks on the engine's own threads. These run on the HOST's:
 * the QUEUED gptps_submit emits (to an observer, with a guarded gptps_settings_set
 * nested in it, and to a bare event callback), the FAILED of gptps_unregister_task
 * and of gptps_cancel (which also tries gptps_step), a dead-letter drain callback,
 * a settings watcher, a write accessor gptps_settings_reload runs, and the terminal
 * events gptps_shutdown itself emits for what it cancels. No check knew about
 * them, so a gptps_shutdown from one went through and freed the engine under the
 * call still walking it (and from shutdown's own events, ran teardown inside
 * teardown). MANUAL mode, so every one of them is on this thread. */
static void test_host_thread_callbacks_refuse_shutdown(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    gptps_handle h = 0;
    size_t ran = 0;

    memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.mode = GPTPS_RUN_MANUAL;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (!e) return;
    g_engine = e;
    set(&g_rc_queued, 999); set(&g_rc_cancel, 999); set(&g_rc_cancel_step, 999);
    set(&g_rc_drain, 999); set(&g_rc_watch, 999); set(&g_rc_teardown, 999);
    set(&g_rc_reload, 999); set(&g_rc_unreg, 999);
    gptps_register_observer(e, obs_host, NULL);
    reg_inproc(e, "q", task_noop, 0);
    reg_inproc(e, "c", task_noop, 0);
    reg_inproc(e, "t", task_noop, 0);
    reg_inproc(e, "u", task_noop, 0);
    reg_inproc(e, "bad", task_always_fails, 0);

    CHECK(gptps_submit(e, "q", NULL, 0, NULL) == GPTPS_OK);
    CHECK(get(&g_rc_queued) == (int)GPTPS_E_BUSY);   /* refused after its nested call too */

    CHECK(gptps_submit(e, "u", NULL, 0, NULL) == GPTPS_OK);
    CHECK(gptps_submit(e, "u", NULL, 0, NULL) == GPTPS_OK);
    CHECK(gptps_unregister_task(e, "u", GPTPS_REMOVE_CANCEL) == GPTPS_OK);
    CHECK(get(&g_rc_unreg) == (int)GPTPS_E_BUSY);    /* from the FAILED the removal emits */

    CHECK(gptps_submit(e, "c", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_cancel(e, h) == GPTPS_OK);      /* still queued: FAILED here, on this thread */
    CHECK(get(&g_rc_cancel) == (int)GPTPS_E_BUSY);
    CHECK(get(&g_rc_cancel_step) == (int)GPTPS_E_BUSY);

    CHECK(gptps_submit(e, "bad", NULL, 0, NULL) == GPTPS_OK);
    while (gptps_step(e, &ran) == GPTPS_OK && ran) { }
    CHECK(gptps_dead_letter_count(e) == 1);
    CHECK(gptps_dead_letter_drain(e, drain_shuts_down, NULL) == 1);
    CHECK(get(&g_rc_drain) == (int)GPTPS_E_BUSY);

    CHECK(gptps_settings_watch(e, watch_shuts_down, NULL) == GPTPS_OK);
    CHECK(gptps_settings_set(e, "limits.max_dead_letters", "64") == GPTPS_OK);
    CHECK(get(&g_rc_watch) == (int)GPTPS_E_BUSY);

    {
        gptps_setting_def sd;
        FILE *f = fopen("hang_reload.toml", "wb");
        CHECK(f != NULL);
        if (f) { fputs("[host]\nknob = \"y\"\n", f); fclose(f); }
        memset(&sd, 0, sizeof sd);
        sd.struct_size = sizeof sd; sd.key = "host.knob"; sd.type = GPTPS_SETTING_STRING;
        sd.desc = "writes shut down"; sd.hot = 1; sd.read = knob_rd; sd.write = knob_wr;
        CHECK(gptps_register_setting(e, &sd) == GPTPS_OK);
        CHECK(gptps_settings_reload(e, "hang_reload.toml") == GPTPS_OK);
        CHECK(get(&g_rc_reload) == (int)GPTPS_E_BUSY);   /* the accessor ran, and was refused */
        remove("hang_reload.toml");
    }

    CHECK(gptps_submit(e, "t", NULL, 0, NULL) == GPTPS_OK);   /* left queued for teardown */
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    CHECK(get(&g_rc_teardown) == (int)GPTPS_E_BUSY);

    /* The same QUEUED, reaching an event callback and no observer. */
    e = NULL;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (e) {
        g_engine = e;
        set(&g_rc_evcb, 999);
        reg_inproc(e, "q", task_noop, 0);
        gptps_set_event_cb(e, evcb_shuts_down, NULL);
        CHECK(gptps_submit(e, "q", NULL, 0, NULL) == GPTPS_OK);
        CHECK(get(&g_rc_evcb) == (int)GPTPS_E_BUSY);
        CHECK(gptps_shutdown(e) == GPTPS_OK);
    }
    g_engine = NULL;
}

int main(void)
{
    test_reentrant_shutdown_is_refused();
    test_reentrant_step_is_refused();
#if !defined(_WIN32)
    test_shutdown_bounds_a_stuck_program("hang");      /* never writes, never exits */
    test_shutdown_bounds_a_stuck_program("eofhang");   /* closes stdout, then lives on */
#endif
    test_service_restart_is_rate_limited();
    test_dead_letter_is_capped();
    test_grace_cancel_is_terminal();
    test_grace_bounds_the_backoff_queue();
    test_grace_ends_a_deferred_item();
    test_grace_ends_queued_work();
    test_grace_admits_nothing();
    test_host_thread_callbacks_refuse_shutdown();

    if (fails) { printf("%d teardown check(s) FAILED\n", fails); return 1; }
    printf("all teardown checks passed\n");
    return 0;
}
