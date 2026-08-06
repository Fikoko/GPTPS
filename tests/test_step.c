/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_step.c - MANUAL (single-threaded) mode: the engine runs with NO worker or
 * dispatcher threads; the caller drives it via gptps_step(). Verifies the full
 * lifecycle, batch admission (max_concurrent_tasks bounds one wave), retry +
 * backoff, dead-letter, result delivery, and that THREADED engines reject
 * gptps_step. Single-threaded, so plain int counters are fine. Headless/portable.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

/* manual mode => the event callback runs on THIS thread; no atomics needed */
static int ev_queued, ev_started, ev_finished, ev_failed, ev_retried, ev_deadletter;
static char last_result[64]; static size_t last_result_len;

static void on_event(const gptps_event *ev, void *ud)
{
    (void)ud;
    switch (ev->kind) {
        case GPTPS_EV_QUEUED:        ev_queued++;     break;
        case GPTPS_EV_STARTED:       ev_started++;    break;
        case GPTPS_EV_FINISHED:      ev_finished++;
            last_result_len = ev->result_len;
            if (ev->result && ev->result_len < sizeof last_result) {
                memcpy(last_result, ev->result, ev->result_len); last_result[ev->result_len] = 0;
            }
            break;
        case GPTPS_EV_FAILED:        ev_failed++;     break;
        case GPTPS_EV_RETRIED:       ev_retried++;    break;
        case GPTPS_EV_DEAD_LETTERED: ev_deadletter++; break;
        case GPTPS_EV_DROPPED:       break;   /* not exercised here; named so -Wswitch
                                               * keeps flagging the NEXT event kind added */
    }
}

static int g_runs;
static gptps_status ok_task(gptps_ctx *ctx, void *ud)   { (void)ud; g_runs++; gptps_result_set(ctx, "done", 4); return GPTPS_OK; }
static gptps_status fail_task(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_E_TASK; }

static void reg(gptps *e, const char *name, gptps_status (*fn)(gptps_ctx *, void *))
{
    gptps_task_def d; memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = fn; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

static gptps *open_manual(uint32_t conc)
{
    gptps_config cfg; gptps *e = NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = conc;
    cfg.limits.max_memory_bytes = 1u << 20;   /* 1 MiB: plenty for these tasks */
    cfg.mode = GPTPS_RUN_MANUAL;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    return e;
}

int main(void)
{
    /* 1) a THREADED engine rejects gptps_step; NULL is rejected too */
    {
        gptps *t = NULL; size_t n = 123;
        CHECK(gptps_open(NULL, &t) == GPTPS_OK);
        CHECK(gptps_step(t, &n) == GPTPS_E_INVAL);
        CHECK(n == 0);                       /* out_ran cleared even on error */
        gptps_shutdown(t);
    }
    CHECK(gptps_step(NULL, NULL) == GPTPS_E_INVAL);

    /* 2) lifecycle: submit several; nothing runs until we step; then all run */
    {
        gptps *e = open_manual(2);
        size_t n, total = 0; int i; gptps_handle h;
        reg(e, "ok", ok_task);
        gptps_set_event_cb(e, on_event, NULL);
        g_runs = ev_queued = ev_started = ev_finished = 0;

        for (i = 0; i < 5; ++i) CHECK(gptps_submit(e, "ok", NULL, 0, &h) == GPTPS_OK);
        CHECK(ev_queued == 5);               /* QUEUED fires at submit time... */
        CHECK(g_runs == 0);                  /* ...but nothing has RUN (no threads) */

        CHECK(gptps_step(e, &n) == GPTPS_OK);
        CHECK(n == 2);                       /* batch admission: conc=2 => wave of 2 */
        do { CHECK(gptps_step(e, &n) == GPTPS_OK); total += n; } while (n);
        CHECK(2 + total == 5);
        CHECK(g_runs == 5);
        CHECK(ev_started == 5 && ev_finished == 5 && ev_failed == 0);
        CHECK(last_result_len == 4 && strcmp(last_result, "done") == 0);

        CHECK(gptps_step(e, &n) == GPTPS_OK && n == 0); /* idle step is a clean no-op */
        gptps_shutdown(e);
    }

    /* 3) retry + dead-letter across steps (backoff 0 => one attempt per step) */
    {
        gptps *e = open_manual(1);
        size_t n; gptps_handle h; int guard = 0;
        gptps_task_def d; memset(&d, 0, sizeof d);
        d.struct_size = sizeof d; d.name = "boom"; d.run = fail_task; d.exec = GPTPS_EXEC_INPROC;
        d.default_cost.struct_size = sizeof d.default_cost;
        d.default_policy.struct_size = sizeof d.default_policy;
        d.default_policy.max_retries = 2;
        d.default_policy.retry_backoff_seconds = 0;
        d.default_policy.on_failure = GPTPS_ON_FAILURE_DEAD_LETTER;
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        gptps_set_event_cb(e, on_event, NULL);
        ev_failed = ev_retried = ev_deadletter = 0;

        CHECK(gptps_submit(e, "boom", NULL, 0, &h) == GPTPS_OK);
        do { CHECK(gptps_step(e, &n) == GPTPS_OK); } while (n && ++guard < 100);
        CHECK(ev_failed == 3);               /* attempts 1,2,3 each emit FAILED */
        CHECK(ev_retried == 2);              /* two retries before exhaustion */
        CHECK(ev_deadletter == 1);
        CHECK(gptps_dead_letter_count(e) == 1);
        gptps_shutdown(e);
    }

    if (fails) { printf("%d step check(s) FAILED\n", fails); return 1; }
    printf("all step checks passed\n");
    return 0;
}
