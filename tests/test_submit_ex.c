/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_submit_ex.c - per-submit overrides (gptps_submit_ex): scheduling priority,
 * failure policy, and a sub-second deadline, all without cloning the task type.
 * Headless / portable.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }

static gptps_status task_ok(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }
static gptps_status task_fail(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_E_TASK; }
static gptps_status task_spin(gptps_ctx *c, void *u) { (void)u; while (!gptps_is_cancelled(c)) { } return GPTPS_OK; }

static void reg(gptps *e, const char *n, gptps_run_fn f)
{
    gptps_task_def d; memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = n; d.run = f; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;  /* default on_failure = dead_letter, 0 retries */
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

/* --- section 1: priority ordering --- */
static gptps_handle g_order[8]; static int g_order_n; static int g_started;
static void obs_order(const gptps_event *ev, void *u)
{
    (void)u;
    if (ev->kind == GPTPS_EV_STARTED && strcmp(ev->task_name, "block") == 0) inc(&g_started);
    if (ev->kind == GPTPS_EV_FINISHED && strcmp(ev->task_name, "work") == 0) {
        /* single worker (max_concurrent_tasks=1) serializes finishes, so write the
         * slot first, THEN publish via the SEQ_CST counter the reader waits on -
         * establishes happens-before so main never reads an unwritten slot. */
        int i = get(&g_order_n);
        if (i >= 0 && i < 8) { g_order[i] = ev->handle; inc(&g_order_n); }
    }
}

/* --- section 3: timeout --- */
static int g_timed_out;
static void obs_timeout(const gptps_event *ev, void *u)
{
    (void)u;
    if (ev->kind == GPTPS_EV_FAILED && ev->status == GPTPS_E_TIMEOUT) inc(&g_timed_out);
}

int main(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    gptps_submit_options o;
    gptps_handle hb = 0, hlo = 0, hhi = 0, h = 0;
    uint64_t s;

    /* ===== 1) priority override: higher-priority submit runs first ===== */
    memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits; cfg.limits.max_concurrent_tasks = 1;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK); if (!e) return 1;
    gptps_set_event_cb(e, obs_order, NULL);
    reg(e, "block", task_spin);
    reg(e, "work",  task_ok);

    CHECK(gptps_submit(e, "block", NULL, 0, &hb) == GPTPS_OK);   /* occupy the worker */
    s = gptps_now_ms(NULL); while (get(&g_started) < 1 && gptps_now_ms(NULL) - s < 2000) { }

    memset(&o, 0, sizeof o); o.struct_size = sizeof o; o.flags = GPTPS_SUBMIT_PRIORITY; o.priority = -5;
    CHECK(gptps_submit_ex(e, "work", NULL, 0, &o, &hlo) == GPTPS_OK);   /* low priority */
    o.priority = 5;
    CHECK(gptps_submit_ex(e, "work", NULL, 0, &o, &hhi) == GPTPS_OK);   /* high priority */

    CHECK(gptps_cancel(e, hb) == GPTPS_OK);    /* free the worker; queued work admits by priority */
    s = gptps_now_ms(NULL); while (get(&g_order_n) < 2 && gptps_now_ms(NULL) - s < 2000) { }
    CHECK(get(&g_order_n) == 2);
    CHECK(g_order[0] == hhi);   /* high priority finished first */
    CHECK(g_order[1] == hlo);
    gptps_shutdown(e); e = NULL;

    /* ===== 2) policy override: per-submit DROP instead of the default dead_letter ===== */
    CHECK(gptps_open(NULL, &e) == GPTPS_OK); if (!e) return 1;
    reg(e, "fail", task_fail);

    CHECK(gptps_submit(e, "fail", NULL, 0, &h) == GPTPS_OK);   /* default policy => dead-lettered */
    s = gptps_now_ms(NULL); while (gptps_dead_letter_count(e) < 1 && gptps_now_ms(NULL) - s < 2000) { }
    CHECK(gptps_dead_letter_count(e) == 1);

    memset(&o, 0, sizeof o); o.struct_size = sizeof o; o.flags = GPTPS_SUBMIT_POLICY;
    o.policy.struct_size = sizeof o.policy; o.policy.on_failure = GPTPS_ON_FAILURE_DROP;
    CHECK(gptps_submit_ex(e, "fail", NULL, 0, &o, &h) == GPTPS_OK);  /* this one drops on failure */
    /* give it time to run + fail; the dead-letter count must NOT grow */
    s = gptps_now_ms(NULL); while (gptps_now_ms(NULL) - s < 200) { }
    CHECK(gptps_dead_letter_count(e) == 1);   /* still 1: the override dropped instead of retaining */
    gptps_shutdown(e); e = NULL;

    /* ===== 3) sub-second deadline: timeout_ms cancels a spinner ===== */
    CHECK(gptps_open(NULL, &e) == GPTPS_OK); if (!e) return 1;
    gptps_set_event_cb(e, obs_timeout, NULL);
    reg(e, "spin", task_spin);
    memset(&o, 0, sizeof o); o.struct_size = sizeof o; o.flags = GPTPS_SUBMIT_TIMEOUT_MS; o.timeout_ms = 10;
    CHECK(gptps_submit_ex(e, "spin", NULL, 0, &o, &h) == GPTPS_OK);
    s = gptps_now_ms(NULL); while (get(&g_timed_out) < 1 && gptps_now_ms(NULL) - s < 2000) { }
    CHECK(get(&g_timed_out) == 1);   /* the 10ms deadline fired and stopped the spinner */
    gptps_shutdown(e);

    /* undersized options struct is rejected */
    CHECK(gptps_open(NULL, &e) == GPTPS_OK); if (e) {
        gptps_submit_options bad; memset(&bad, 0, sizeof bad); bad.struct_size = 1;
        reg(e, "work", task_ok);
        CHECK(gptps_submit_ex(e, "work", NULL, 0, &bad, &h) == GPTPS_E_INVAL);
        gptps_shutdown(e);
    }

    if (fails) { printf("%d submit_ex check(s) FAILED\n", fails); return 1; }
    printf("all submit_ex checks passed\n");
    return 0;
}
