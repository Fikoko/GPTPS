/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_hang.c - teardown must always TERMINATE.
 *
 * Every check here is a way the engine used to stop returning: a re-entrant
 * gptps_shutdown that joins its own thread, a no-timeout external child that
 * outlives its pipe, a zero-backoff supervised service that re-admits itself as
 * fast as the dispatcher loops, and an undrained dead-letter list that grows
 * without bound. They share a property that makes them worth one file: the
 * failure mode is a HANG or unbounded growth, not a wrong answer - so the CTest
 * TIMEOUT on this test is itself part of the assertion.
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
static int    g_started;
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

    {   /* wait for the queue to drain into the dead-letter list */
        uint64_t t0 = gptps_now_ms(NULL);
        while (gptps_now_ms(NULL) - t0 < 3000 && gptps_dead_letter_count(e) < 8) { }
    }
    n = gptps_dead_letter_count(e);
    CHECK(n <= 8);            /* bounded: it used to grow to 60, and forever in prod */
    CHECK(n >= 1);

    /* the truncation is reported, never silent */
    CHECK(gptps_settings_get(e, "stats.dead_letters_evicted", buf, sizeof buf) == GPTPS_OK);
    CHECK(strcmp(buf, "0") != 0);

    CHECK(gptps_shutdown(e) == GPTPS_OK);
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

    if (fails) { printf("%d teardown check(s) FAILED\n", fails); return 1; }
    printf("all teardown checks passed\n");
    return 0;
}
