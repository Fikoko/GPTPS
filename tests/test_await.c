/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_await.c - blocking wait on a handle, built on the observer seam.
 *
 * Proves the four things the add-on claims, each of which is a way it could be
 * silently wrong:
 *   1) a wait returns the task's RESULT and the task's STATUS, distinctly from
 *      whether the wait itself succeeded;
 *   2) the submit/finish race is closed - a task that completes BEFORE gptps_submit
 *      returns its handle is still waitable (this is the one that motivates
 *      installing the observer up front, and the reason retention exists at all);
 *   3) a RETRYING task does not wake a waiter early - the terminal predicate again;
 *   4) a wait on something that never terminates times out rather than hanging, and
 *      a cancel IS terminal so it wakes the waiter.
 * Headless / portable.
 */
#include "gptps_await.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int g_release, g_attempts;
static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }

static gptps_status task_echo(gptps_ctx *c, void *u)
{
    size_t n; const void *p = gptps_payload(c, &n);
    (void)u;
    return gptps_result_set(c, p, n);
}
static gptps_status task_fail(gptps_ctx *c, void *u)
{ (void)c; (void)u; inc(&g_attempts); return GPTPS_E_IO; }
static gptps_status task_block(gptps_ctx *c, void *u)
{ (void)u; while (!get(&g_release) && !gptps_is_cancelled(c)) { } return GPTPS_OK; }

static void reg(gptps *e, const char *n, gptps_run_fn f, int retries)
{
    gptps_task_def d; memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = n; d.run = f; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = retries;
    d.default_policy.retry_backoff_seconds = 0;
    d.default_policy.on_failure = GPTPS_ON_FAILURE_DEAD_LETTER;
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

int main(void)
{
    gptps *e;
    gptps_await *aw;
    gptps_handle h = 0;
    void *res = NULL;
    size_t len = 0;
    gptps_status task_st = GPTPS_OK;
    int i;

    /* ===== 1) result + status round-trip ===== */
    e = open_engine(4); CHECK(e != NULL); if (!e) return 1;
    aw = gptps_await_install(e); CHECK(aw != NULL); if (!aw) { gptps_shutdown(e); return 1; }
    reg(e, "echo", task_echo, 0);

    CHECK(gptps_submit(e, "echo", "hello", 5, &h) == GPTPS_OK);
    CHECK(gptps_await_wait(aw, h, 5000, &res, &len, &task_st) == GPTPS_OK);
    CHECK(task_st == GPTPS_OK);
    CHECK(len == 5);
    CHECK(res && memcmp(res, "hello", 5) == 0);
    free(res); res = NULL;

    /* A wait on a handle that never existed must time out, not hang or succeed. */
    CHECK(gptps_await_wait(aw, 999999u, 50, NULL, NULL, NULL) == GPTPS_E_TIMEOUT);
    /* timeout_ms == 0 polls */
    CHECK(gptps_await_wait(aw, 999999u, 0, NULL, NULL, NULL) == GPTPS_E_TIMEOUT);
    CHECK(gptps_await_wait(aw, 0, 100, NULL, NULL, NULL) == GPTPS_E_INVAL);

    /* ===== 2) the submit/finish race =====
     * Submit many fast tasks and only THEN wait on each handle. Every one of them
     * has certainly completed before its wait is called - which is exactly the case
     * a naive implementation (register interest when wait() is called) loses. The
     * ring is sized to hold them all. */
    {
        gptps_handle hs[64];
        for (i = 0; i < 64; ++i)
            CHECK(gptps_submit(e, "echo", &i, sizeof i, &hs[i]) == GPTPS_OK);
        CHECK(gptps_await_quiesce(aw, 1 + 64, 5000) == GPTPS_OK);   /* 1 from case 1 */
        for (i = 0; i < 64; ++i) {
            int v = -1;
            CHECK(gptps_await_wait(aw, hs[i], 5000, &res, &len, &task_st) == GPTPS_OK);
            CHECK(task_st == GPTPS_OK && len == sizeof v);
            if (res && len == sizeof v) memcpy(&v, res, sizeof v);
            CHECK(v == i);                       /* the RIGHT result, not just any */
            free(res); res = NULL;
        }
    }
    gptps_shutdown(e);
    gptps_await_close(aw);

    /* ===== 3) a retrying task must not wake the waiter early =====
     * "flaky" fails twice (attempt + one retry) then dead-letters. A waiter woken by
     * the first GPTPS_EV_FAILED would return while the item was still going to run
     * again - so assert the task actually attempted twice before we were woken. */
    __atomic_store_n(&g_attempts, 0, __ATOMIC_SEQ_CST);
    e = open_engine(4); CHECK(e != NULL); if (!e) return 1;
    aw = gptps_await_install(e); CHECK(aw != NULL); if (!aw) { gptps_shutdown(e); return 1; }
    reg(e, "flaky", task_fail, 1);

    CHECK(gptps_submit(e, "flaky", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_await_wait(aw, h, 5000, &res, &len, &task_st) == GPTPS_OK);
    CHECK(get(&g_attempts) == 2);        /* woken only after BOTH attempts */
    CHECK(task_st == GPTPS_E_IO);        /* the task's own failure, reported */
    CHECK(res == NULL && len == 0);      /* a dead-lettered item has no result */
    free(res); res = NULL;

    gptps_shutdown(e);
    gptps_await_close(aw);

    /* ===== 4) never-terminating work times out; a cancel is terminal ===== */
    __atomic_store_n(&g_release, 0, __ATOMIC_SEQ_CST);
    e = open_engine(4); CHECK(e != NULL); if (!e) return 1;
    aw = gptps_await_install(e); CHECK(aw != NULL); if (!aw) { gptps_shutdown(e); return 1; }
    reg(e, "block", task_block, 0);

    CHECK(gptps_submit(e, "block", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_await_wait(aw, h, 100, NULL, NULL, NULL) == GPTPS_E_TIMEOUT);

    CHECK(gptps_cancel(e, h) == GPTPS_OK);
    CHECK(gptps_await_wait(aw, h, 5000, &res, &len, &task_st) == GPTPS_OK);
    CHECK(task_st == GPTPS_E_CANCELLED); /* FAILED/E_CANCELLED IS terminal */
    free(res); res = NULL;

    __atomic_store_n(&g_release, 1, __ATOMIC_SEQ_CST);
    gptps_shutdown(e);
    gptps_await_close(aw);

    if (fails) { printf("%d await check(s) FAILED\n", fails); return 1; }
    printf("all await checks passed\n");
    return 0;
}
