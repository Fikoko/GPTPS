/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_stats.c - the stats add-on on the observer seam: totals, live gauges, per-task
 * rows, latency, reset, and merge across gptps_pool shards. One worker makes the
 * queueing deterministic. Headless / portable.
 */
#include "gptps_stats.h"
#include "gptps_pool.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static void spin_ms(gptps_ctx *c, unsigned ms)
{ uint64_t s = gptps_now_ms(c); while (gptps_now_ms(c) - s < ms) { } }

static gptps_status task_ok(gptps_ctx *c, void *u) { (void)u; spin_ms(c, 20); return GPTPS_OK; }
static int flaky_calls = 0;
static gptps_status task_flaky(gptps_ctx *c, void *u)
{ (void)c; (void)u; return __atomic_add_fetch(&flaky_calls, 1, __ATOMIC_SEQ_CST) == 1 ? GPTPS_E_TASK : GPTPS_OK; }
static gptps_status task_dead(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_E_TASK; }
static gptps_status task_block(gptps_ctx *c, void *u)
{ int *go = (int *)u; while (!get(go) && !gptps_is_cancelled(c)) { } return GPTPS_OK; }

static void reg(gptps *e, const char *n, gptps_run_fn f, uint32_t retries, void *ud)
{
    gptps_task_def d; memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = n; d.run = f; d.exec = GPTPS_EXEC_INPROC; d.user_data = ud;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = 1;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = retries; d.default_policy.retry_backoff_seconds = 0;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}
static gptps *open1(void)
{
    gptps *e = NULL;
    gptps_config cfg; memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1; cfg.limits.max_memory_bytes = 1024;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK && e);
    return e;
}
static void wait_terminal(gptps_stats *s, uint64_t n)
{
    gptps_stats_counters c; uint64_t t0 = gptps_now_ms(NULL);
    do { gptps_stats_total(s, &c); } while (c.terminal < n && gptps_now_ms(NULL) - t0 < 3000);
}

static void test_totals_and_latency(void)
{
    gptps *e = open1(); gptps_handle h; gptps_stats_counters c, tc; int i;
    gptps_stats *s = gptps_stats_install(e);
    CHECK(s != NULL);
    reg(e, "ok",    task_ok,    0, NULL);
    reg(e, "flaky", task_flaky, 1, NULL);
    reg(e, "dead",  task_dead,  0, NULL);
    for (i = 0; i < 3; ++i) CHECK(gptps_submit(e, "ok", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_submit(e, "flaky", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_submit(e, "dead",  NULL, 0, &h) == GPTPS_OK);
    wait_terminal(s, 5);

    CHECK(gptps_stats_total(s, &c) == GPTPS_OK);
    CHECK(c.queued == 5);
    CHECK(c.started == 6);            /* flaky started twice */
    CHECK(c.finished == 4);
    CHECK(c.failed == 2);             /* flaky's first attempt + dead */
    CHECK(c.retried == 1);
    CHECK(c.dead_lettered == 1);
    CHECK(c.terminal == 5);
    CHECK(c.pending == 0 && c.in_flight == 0);
    CHECK(c.run_samples == 6);
    /* QUEUED is emitted on the submitting thread, so the FIRST submit's STARTED can
     * outrun it (queue time unknown -> no sample); the rest queue behind a 20ms task. */
    CHECK(c.wait_samples >= 5 && c.wait_samples <= 6);
    CHECK(c.run_ms_sum >= 3 * 20);    /* three 20ms tasks */
    CHECK(c.run_ms_max >= 20 && c.run_ms_max <= c.run_ms_sum);
    /* everything queued behind the first 20ms task waited for it */
    CHECK(c.wait_ms_max >= 20);

    CHECK(gptps_stats_task(s, "flaky", &tc) == GPTPS_OK);
    CHECK(tc.queued == 1 && tc.started == 2 && tc.failed == 1 && tc.retried == 1 && tc.finished == 1);
    CHECK(gptps_stats_task(s, "dead", &tc) == GPTPS_OK);
    CHECK(tc.dead_lettered == 1 && tc.finished == 0);
    CHECK(gptps_stats_task(s, "nope", &tc) == GPTPS_E_NOTFOUND);
    CHECK(gptps_stats_task_count(s) == 3);
    {   char nm[32]; CHECK(gptps_stats_task_at(s, 0, nm, sizeof nm, &tc) == GPTPS_OK);
        CHECK(strcmp(nm, "ok") == 0 && tc.finished == 3);
        CHECK(gptps_stats_task_at(s, 3, nm, sizeof nm, &tc) == GPTPS_E_NOTFOUND); }

    /* the sum of the task rows is the engine row */
    {   gptps_stats_counters sum; size_t k; memset(&sum, 0, sizeof sum);
        for (k = 0; k < gptps_stats_task_count(s); ++k) { gptps_stats_task_at(s, k, NULL, 0, &tc); gptps_stats_merge(&sum, &tc); }
        CHECK(sum.queued == c.queued && sum.started == c.started && sum.terminal == c.terminal);
        CHECK(sum.run_ms_sum == c.run_ms_sum && sum.run_ms_max == c.run_ms_max); }

    gptps_stats_reset(s);
    CHECK(gptps_stats_total(s, &c) == GPTPS_OK && c.queued == 0 && c.terminal == 0 && c.run_samples == 0);
    CHECK(gptps_stats_task(s, "ok", &tc) == GPTPS_OK && tc.finished == 0);

    CHECK(gptps_shutdown(e) == GPTPS_OK);
    gptps_stats_close(s);   /* AFTER shutdown, like every observer add-on */
}

static void test_gauges(void)
{
    gptps *e = open1(); gptps_handle h; gptps_stats_counters c; int go = 0, i;
    gptps_stats *s = gptps_stats_install(e);
    reg(e, "block", task_block, 0, &go);
    reg(e, "ok",    task_ok,    0, NULL);
    CHECK(gptps_submit(e, "block", NULL, 0, &h) == GPTPS_OK);
    for (i = 0; i < 4; ++i) CHECK(gptps_submit(e, "ok", NULL, 0, &h) == GPTPS_OK);
    {   uint64_t t0 = gptps_now_ms(NULL);
        do { gptps_stats_total(s, &c); } while (c.started < 1 && gptps_now_ms(NULL) - t0 < 2000); }
    CHECK(c.in_flight == 1);          /* block is running */
    CHECK(c.pending == 4);            /* the four ok's wait behind it on one worker */
    CHECK(gptps_cancel(e, h) == GPTPS_OK);   /* cancel one queued ok: FAILED/CANCELLED before STARTED */
    __atomic_store_n(&go, 1, __ATOMIC_SEQ_CST);
    wait_terminal(s, 5);
    CHECK(gptps_stats_total(s, &c) == GPTPS_OK);
    CHECK(c.cancelled == 1 && c.terminal == 5);
    CHECK(c.pending == 0 && c.in_flight == 0);
    CHECK(c.started == 4);            /* block + three ok's; the cancelled one never ran */
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    gptps_stats_close(s);
}

static void test_pool_merge(void)
{
    gptps_pool *p; gptps_stats *s[3]; gptps_stats_counters sum, c; size_t i; int k;
    gptps_pool_handle ph;
    gptps_config cfg; memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1; cfg.limits.max_memory_bytes = 1024;
    p = gptps_pool_open(3, &cfg);
    CHECK(p != NULL);
    for (i = 0; i < 3; ++i) { s[i] = gptps_stats_install(gptps_pool_shard(p, i)); CHECK(s[i] != NULL); }
    {   gptps_task_def d; memset(&d, 0, sizeof d);
        d.struct_size = sizeof d; d.name = "ok"; d.run = task_ok; d.exec = GPTPS_EXEC_INPROC;
        d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = 1;
        d.default_policy.struct_size = sizeof d.default_policy;
        CHECK(gptps_pool_register_task(p, &d) == GPTPS_OK); }
    for (k = 0; k < 9; ++k) CHECK(gptps_pool_submit(p, "ok", NULL, 0, &ph) == GPTPS_OK);
    {   uint64_t t0 = gptps_now_ms(NULL);
        do { memset(&sum, 0, sizeof sum);
             for (i = 0; i < 3; ++i) { gptps_stats_total(s[i], &c); gptps_stats_merge(&sum, &c); }
        } while (sum.terminal < 9 && gptps_now_ms(NULL) - t0 < 3000); }
    CHECK(sum.queued == 9 && sum.finished == 9 && sum.terminal == 9);
    CHECK(sum.pending == 0 && sum.in_flight == 0);
    for (i = 0; i < 3; ++i) { gptps_stats_total(s[i], &c); CHECK(c.queued == 3); }   /* round-robin */
    gptps_pool_close(p);
    for (i = 0; i < 3; ++i) gptps_stats_close(s[i]);
}

int main(void)
{
    test_totals_and_latency();
    test_gauges();
    test_pool_merge();
    if (fails) { printf("%d stats check(s) FAILED\n", fails); return 1; }
    printf("all stats checks passed\n");
    return 0;
}
