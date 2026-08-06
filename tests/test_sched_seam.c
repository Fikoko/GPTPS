/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_sched_seam.c - the pluggable scheduler seam (gptps_set_scheduler). Proves a
 * custom ordering hook replaces the built-in priority/FIFO admission order WITHOUT
 * any core change: with one worker, three items queue behind a blocker, then are
 * admitted in the order the installed scheduler dictates - here ascending by a
 * payload key - which differs from the default FIFO-by-submit order.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }

static int  g_release, g_block_running, g_n;
static char g_order[8];

/* occupies the single worker until released, so the jobs pile up in intake */
static gptps_status task_block(gptps_ctx *c, void *u)
{ (void)u; __atomic_store_n(&g_block_running, 1, __ATOMIC_SEQ_CST);
  while (!get(&g_release) && !gptps_is_cancelled(c)) { } return GPTPS_OK; }

/* records its payload key in admission order (one worker => sequential) */
static gptps_status task_job(gptps_ctx *c, void *u)
{
    size_t len; const unsigned char *p = (const unsigned char *)gptps_payload(c, &len);
    int i = __atomic_fetch_add(&g_n, 1, __ATOMIC_SEQ_CST);
    (void)u;
    if (i < (int)sizeof g_order) g_order[i] = (char)((p && len) ? p[0] : 0);
    return GPTPS_OK;
}

/* order ASCENDING by the payload's first byte (ignores priority): lowest key first */
static int64_t sched_by_payload_asc(const gptps_sched_input *in, void *ud)
{ (void)ud; return (in->payload && in->payload_len) ? -(int64_t)((const unsigned char *)in->payload)[0] : 0; }

static void reg(gptps *e, const char *n, gptps_run_fn f)
{
    gptps_task_def d; memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = n; d.run = f; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

/* submit b3,b1,b2 behind a blocker; return the admission order into out[3] */
static void run_case(int use_sched, char out[3])
{
    gptps_config cfg; gptps *e = NULL; gptps_handle h = 0;
    static const unsigned char b3 = 3, b1 = 1, b2 = 2;
    uint64_t s;

    __atomic_store_n(&g_release, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_block_running, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_n, 0, __ATOMIC_SEQ_CST);
    memset(g_order, 0, sizeof g_order);

    memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;              /* strictly sequential => deterministic order */
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK); if (!e) return;
    if (use_sched) CHECK(gptps_set_scheduler(e, sched_by_payload_asc, NULL) == GPTPS_OK);
    reg(e, "block", task_block);
    reg(e, "job",   task_job);

    /* occupy the worker, then queue three jobs behind it */
    CHECK(gptps_submit(e, "block", NULL, 0, &h) == GPTPS_OK);
    s = gptps_now_ms(NULL);
    while (!get(&g_block_running) && gptps_now_ms(NULL) - s < 2000) { }
    CHECK(get(&g_block_running) == 1);

    CHECK(gptps_submit(e, "job", &b3, 1, NULL) == GPTPS_OK);   /* submit order: 3, 1, 2 */
    CHECK(gptps_submit(e, "job", &b1, 1, NULL) == GPTPS_OK);
    CHECK(gptps_submit(e, "job", &b2, 1, NULL) == GPTPS_OK);

    __atomic_store_n(&g_release, 1, __ATOMIC_SEQ_CST);         /* free the worker */
    s = gptps_now_ms(NULL);
    while (get(&g_n) < 3 && gptps_now_ms(NULL) - s < 3000) { }
    CHECK(get(&g_n) == 3);
    gptps_shutdown(e);                                         /* join => g_order settled */

    out[0] = g_order[0]; out[1] = g_order[1]; out[2] = g_order[2];
}

int main(void)
{
    char def[3], sch[3];

    /* default: equal scores => FIFO by submit order 3,1,2 */
    run_case(0, def);
    CHECK(def[0] == 3 && def[1] == 1 && def[2] == 2);

    /* custom scheduler: ascending by payload key => 1,2,3 (order changed, no core edit) */
    run_case(1, sch);
    CHECK(sch[0] == 1 && sch[1] == 2 && sch[2] == 3);

    if (fails) { printf("%d scheduler-seam check(s) FAILED\n", fails); return 1; }
    printf("all scheduler-seam checks passed\n");
    return 0;
}
