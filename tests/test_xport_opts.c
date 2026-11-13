/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_xport_opts.c - gptps_xport_submit_opts / _async_opts: submit options cross the
 * link and are applied by the WORKER's engine (ENGINE MODE).
 *
 * The worker's child_init installs a constraint that refuses an item already due
 * (deadline_ms != 0 and now >= it): such an item is dead-lettered with GPTPS_E_DENIED,
 * which is the reply's task status. So the reply proves, from outside the worker
 * process, what due time the worker's engine saw:
 *  1) "due now" (deadline_in_ms 0) is refused - the due time arrived;
 *  2) due in a minute runs, and so does an item sent with no options at all;
 *  3) a failure policy arrives too: a body that fails runs 1 + max_retries times, and
 *     the reply's status is the last attempt's under on_failure = drop;
 *  4) the async form carries them the same way;
 *  5) an undersized options struct is refused here, before anything is sent.
 * POSIX only, like gptps_xport.
 */
#define _POSIX_C_SOURCE 200809L
#include "gptps.h"
#include "gptps_xport.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static gptps_admit_decision refuse_due(const gptps_constraint_input *in, uint32_t *ra, void *ud)
{
    (void)ra; (void)ud;
    if (in->struct_size >= offsetof(gptps_constraint_input, now_ms) + sizeof in->now_ms &&
        in->deadline_ms && in->now_ms >= in->deadline_ms) return GPTPS_DENY;
    return GPTPS_ADMIT;
}
static void child_init(gptps *e, void *ud) { (void)ud; gptps_register_constraint(e, refuse_due, NULL); }

static int g_runs;   /* per worker process: counted there, reported in the result */
static gptps_status ok_body(gptps_ctx *ctx, void *ud) { (void)ud; return gptps_result_set(ctx, "ok", 2); }
static gptps_status bad_body(gptps_ctx *ctx, void *ud)
{
    unsigned char n;
    (void)ud;
    n = (unsigned char)++g_runs;
    gptps_result_set(ctx, &n, 1);            /* a failure's result is not delivered; the count is in g_runs */
    return GPTPS_E_TASK;
}
static gptps_status count_body(gptps_ctx *ctx, void *ud)
{
    unsigned char n = (unsigned char)g_runs;
    (void)ud;
    return gptps_result_set(ctx, &n, 1);
}

static volatile int g_async_done;
static gptps_status g_async_status = GPTPS_E_IO;
static void on_reply(const gptps_xport_reply *r, void *ud)
{
    (void)ud;
    if (r->io == GPTPS_OK) g_async_status = r->task_status;
    __atomic_store_n(&g_async_done, 1, __ATOMIC_SEQ_CST);
}

static gptps_submit_options due_in(uint64_t ms)
{
    gptps_submit_options o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o; o.flags = GPTPS_SUBMIT_DEADLINE; o.deadline_in_ms = ms;
    return o;
}

static gptps_status send(gptps_xport *xp, const char *task, const gptps_submit_options *o, unsigned char *res0)
{
    gptps_xport_reply r;
    gptps_status st;
    memset(&r, 0, sizeof r); r.struct_size = sizeof r;
    st = gptps_xport_submit_opts(xp, task, "x", 1, o, &r);
    if (st != GPTPS_OK) return st;
    if (res0) *res0 = (r.result && r.result_len) ? ((const unsigned char *)r.result)[0] : 0;
    st = r.task_status;
    gptps_xport_result_free(&r);
    return st;
}

int main(void)
{
    gptps_task_def tasks[3];
    gptps_config ecfg;
    gptps_xport_config cfg;
    gptps_xport *xp;
    gptps_submit_options o;
    unsigned char n = 0;
    int i;

    memset(tasks, 0, sizeof tasks);
    for (i = 0; i < 3; ++i) {
        tasks[i].struct_size = sizeof tasks[i];
        tasks[i].default_cost.struct_size = sizeof tasks[i].default_cost;
        tasks[i].default_policy.struct_size = sizeof tasks[i].default_policy;
    }
    tasks[0].name = "ok";    tasks[0].run = ok_body;
    tasks[1].name = "bad";   tasks[1].run = bad_body;    /* no retries by default */
    tasks[2].name = "count"; tasks[2].run = count_body;
    memset(&ecfg, 0, sizeof ecfg);
    ecfg.struct_size = sizeof ecfg; ecfg.limits.struct_size = sizeof ecfg.limits;
    ecfg.limits.max_concurrent_tasks = 1; ecfg.limits.max_memory_bytes = 1024;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.nworkers = 1;    /* one worker: g_runs is one process's */
    cfg.engine_cfg = &ecfg; cfg.tasks = tasks; cfg.ntasks = 3; cfg.child_init = child_init;
    xp = gptps_xport_open_ex(&cfg);
    CHECK(xp != NULL);
    if (!xp) return 1;

    /* 1) and 2) */
    o = due_in(0);
    CHECK(send(xp, "ok", &o, NULL) == GPTPS_E_DENIED);
    o = due_in(60000);
    CHECK(send(xp, "ok", &o, NULL) == GPTPS_OK);
    CHECK(send(xp, "ok", NULL, NULL) == GPTPS_OK);

    /* 3) a policy: two retries, then dropped */
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o; o.flags = GPTPS_SUBMIT_POLICY;
    o.policy.struct_size = sizeof o.policy; o.policy.max_retries = 2;
    o.policy.on_failure = GPTPS_ON_FAILURE_DROP;
    CHECK(send(xp, "bad", &o, NULL) == GPTPS_E_TASK);
    CHECK(send(xp, "count", NULL, &n) == GPTPS_OK);
    CHECK(n == 3);                                   /* the first attempt and both retries */

    /* 4) async */
    o = due_in(0);
    CHECK(gptps_xport_submit_async_opts(xp, "ok", "x", 1, &o, on_reply, NULL, NULL) == GPTPS_OK);
    {
        struct timespec ts = { 0, 1000000L };
        int k;
        for (k = 0; k < 10000 && !__atomic_load_n(&g_async_done, __ATOMIC_SEQ_CST); ++k) nanosleep(&ts, NULL);
    }
    CHECK(g_async_done && g_async_status == GPTPS_E_DENIED);

    /* 5) undersized: refused locally */
    o = due_in(1000);
    o.struct_size = offsetof(gptps_submit_options, timeout_ms);
    {
        gptps_xport_reply r; memset(&r, 0, sizeof r); r.struct_size = sizeof r;
        CHECK(gptps_xport_submit_opts(xp, "ok", "x", 1, &o, &r) == GPTPS_E_INVAL);
    }

    gptps_xport_close(xp);
    if (fails) { printf("%d xport-options check(s) FAILED\n", fails); return 1; }
    printf("xport options: all checks passed\n");
    return 0;
}
