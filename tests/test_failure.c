/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_failure.c - failure engine (T5): retries+backoff, on_failure
 * (dead_letter / drop / requeue), and the deadline watchdog (cooperative
 * timeout). Each scenario uses its own engine so global event counters are
 * isolated; shutdown() drains the full retry cycle before we assert.
 */
#include "gptps.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int c_queued, c_started, c_finished, c_failed, c_retried, c_dead, c_timeout;
static int g_calls, g_fail_n;

static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }

static void reset(void)
{
    c_queued = c_started = c_finished = c_failed = c_retried = c_dead = c_timeout = 0;
    g_calls = 0; g_fail_n = 0;
}

static void on_ev(const gptps_event *ev, void *ud)
{
    (void)ud;
    switch (ev->kind) {
        case GPTPS_EV_QUEUED:        inc(&c_queued);   break;
        case GPTPS_EV_STARTED:       inc(&c_started);  break;
        case GPTPS_EV_FINISHED:      inc(&c_finished); break;
        case GPTPS_EV_FAILED:        inc(&c_failed); if (ev->status == GPTPS_E_TIMEOUT) inc(&c_timeout); break;
        case GPTPS_EV_RETRIED:       inc(&c_retried);  break;
        case GPTPS_EV_DEAD_LETTERED: inc(&c_dead);     break;
        default: break;
    }
}

static gptps_status task_flaky(gptps_ctx *ctx, void *ud)
{
    int n = __atomic_fetch_add(&g_calls, 1, __ATOMIC_SEQ_CST);
    (void)ud; (void)ctx;
    return (n < __atomic_load_n(&g_fail_n, __ATOMIC_SEQ_CST)) ? GPTPS_E_TASK : GPTPS_OK;
}
static gptps_status task_fail(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_E_TASK; }
static gptps_status task_slow(gptps_ctx *ctx, void *ud)
{
    uint64_t start = gptps_now_ms(ctx);
    (void)ud;
    while (!gptps_is_cancelled(ctx)) {
        if (gptps_now_ms(ctx) - start > 5000) break; /* safety: never hang the test */
    }
    return GPTPS_OK; /* engine marks E_TIMEOUT because the flag was set */
}

static gptps *open_engine(void)
{
    gptps *e = NULL;
    if (gptps_open(NULL, &e) != GPTPS_OK) return NULL;
    gptps_set_event_cb(e, on_ev, NULL);
    return e;
}

static void def_init(gptps_task_def *d, const char *name, gptps_run_fn run,
                     uint32_t timeout_s, uint32_t retries, uint32_t backoff_s,
                     gptps_on_failure on_fail)
{
    memset(d, 0, sizeof *d);
    d->struct_size = sizeof *d; d->name = name; d->run = run; d->exec = GPTPS_EXEC_INPROC;
    d->default_cost.struct_size = sizeof d->default_cost; d->default_cost.mem_bytes = 1024;
    d->default_policy.struct_size = sizeof d->default_policy;
    d->default_policy.timeout_seconds = timeout_s;
    d->default_policy.max_retries = retries;
    d->default_policy.retry_backoff_seconds = backoff_s;
    d->default_policy.on_failure = on_fail;
}

int main(void)
{
    gptps *e;
    gptps_task_def d;
    gptps_handle h;

    /* 1) retry then succeed: fail twice, succeed on the 3rd attempt */
    reset(); g_fail_n = 2;
    e = open_engine(); CHECK(e != NULL);
    def_init(&d, "flaky", task_flaky, 0, 3, 0, GPTPS_ON_FAILURE_DEAD_LETTER);
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(gptps_submit(e, "flaky", NULL, 0, &h) == GPTPS_OK);
    gptps_shutdown(e);
    CHECK(get(&c_started)  == 3);
    CHECK(get(&c_failed)   == 2);
    CHECK(get(&c_retried)  == 2);
    CHECK(get(&c_finished) == 1);
    CHECK(get(&c_dead)     == 0);

    /* 2) dead-letter after retries exhausted (1 retry => 2 attempts) */
    reset();
    e = open_engine(); CHECK(e != NULL);
    def_init(&d, "deadf", task_fail, 0, 1, 0, GPTPS_ON_FAILURE_DEAD_LETTER);
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(gptps_submit(e, "deadf", NULL, 0, &h) == GPTPS_OK);
    gptps_shutdown(e);
    CHECK(get(&c_started)  == 2);
    CHECK(get(&c_retried)  == 1);
    CHECK(get(&c_dead)     == 1);
    CHECK(get(&c_finished) == 0);

    /* 3) drop after one attempt (no retries) */
    reset();
    e = open_engine(); CHECK(e != NULL);
    def_init(&d, "dropf", task_fail, 0, 0, 0, GPTPS_ON_FAILURE_DROP);
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(gptps_submit(e, "dropf", NULL, 0, &h) == GPTPS_OK);
    gptps_shutdown(e);
    CHECK(get(&c_started) == 1);
    CHECK(get(&c_retried) == 0);
    CHECK(get(&c_dead)    == 0);
    CHECK(get(&c_failed)  == 1);

    /* 4) deadline watchdog -> cooperative timeout (1s) */
    reset();
    e = open_engine(); CHECK(e != NULL);
    def_init(&d, "slow", task_slow, 1, 0, 0, GPTPS_ON_FAILURE_DROP);
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(gptps_submit(e, "slow", NULL, 0, &h) == GPTPS_OK);
    gptps_shutdown(e);
    CHECK(get(&c_timeout) >= 1);
    CHECK(get(&c_failed)  >= 1);
    CHECK(get(&c_finished) == 0);

    /* 5) emit-window stress: dead-letter completions (which open the dispatcher's
     *    lock-released emit window) interleaved with concurrent OK completions,
     *    repeated under shutdown. With the P1 lost-wakeup bug this hangs. */
    {
        int loop, k;
        for (loop = 0; loop < 30; ++loop) {
            reset(); g_fail_n = 0; /* flaky with 0 fails => always OK */
            e = open_engine(); CHECK(e != NULL);
            def_init(&d, "okk", task_flaky, 0, 0, 0, GPTPS_ON_FAILURE_DROP);
            CHECK(gptps_register_task(e, &d) == GPTPS_OK);
            def_init(&d, "dlf", task_fail, 0, 0, 0, GPTPS_ON_FAILURE_DEAD_LETTER);
            CHECK(gptps_register_task(e, &d) == GPTPS_OK);
            for (k = 0; k < 50; ++k)
                CHECK(gptps_submit(e, (k % 5 == 0) ? "dlf" : "okk", NULL, 0, &h) == GPTPS_OK);
            gptps_shutdown(e);
            CHECK(get(&c_finished) == 40);
            CHECK(get(&c_dead)     == 10);
            if (fails) break;
        }
    }

    if (fails) { printf("%d failure-engine check(s) FAILED\n", fails); return 1; }
    printf("all failure-engine checks passed\n");
    return 0;
}
