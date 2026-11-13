/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_durable_opts.c - addons/gptps_durable_queue: submit options survive a restart.
 *
 * A restart without a fork: work a MANUAL host never steps stays pending at shutdown,
 * so a fresh engine and queue on the same journal is the next run. Portable for that
 * reason - Windows runs it too.
 *
 *  1) gptps_dq_submit_ex journals priority, policy and due time; after a restart a
 *     recovered record comes back with them - the due time as what was left of it on
 *     the wall clock, the policy by how often a failing body runs.
 *  2) A due time that passed while the host was down comes back as "due now".
 *  3) The options outlive compaction (every gptps_dq_open compacts), restart after
 *     restart.
 *  4) A record journaled without options - as gptps_dq_submit and every older build
 *     write it - recovers with none.
 *  5) gptps_dq_submit_batch_ex journals the batch's options for every item.
 *  6) An undersized options struct is refused before anything is written.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "gptps.h"
#include "gptps_durable_queue.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

#define JOURNAL "test_durable_opts.journal"
#define MAXH 64

/* what the scheduler saw, by the payload's first byte (the record's tag) */
static uint64_t g_dl[256], g_enq[256];
static int32_t  g_prio[256];
static int      g_seen[256], g_runs[256];

static int64_t spy(const gptps_sched_input *in, void *ud)
{
    unsigned char t;
    (void)ud;
    if (!in->payload_len) return 0;
    t = ((const unsigned char *)in->payload)[0];
    g_seen[t] = 1;
    g_prio[t] = in->priority;
    g_enq[t]  = in->enqueue_ms;
    g_dl[t]   = in->struct_size >= offsetof(gptps_sched_input, deadline_ms) + sizeof in->deadline_ms
              ? in->deadline_ms : 0;
    return in->priority;
}

static gptps_status ok_body(gptps_ctx *ctx, void *ud)
{
    size_t n; const unsigned char *p = (const unsigned char *)gptps_payload(ctx, &n);
    (void)ud;
    if (n) g_runs[p[0]]++;
    return GPTPS_OK;
}
static gptps_status bad_body(gptps_ctx *ctx, void *ud)
{
    size_t n; const unsigned char *p = (const unsigned char *)gptps_payload(ctx, &n);
    (void)ud;
    if (n) g_runs[p[0]]++;
    return GPTPS_E_TASK;
}

static gptps *open_engine(void)
{
    gptps_config cfg; gptps *e = NULL; gptps_task_def d;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;
    cfg.limits.max_memory_bytes = 1u << 20;
    cfg.mode = GPTPS_RUN_MANUAL;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "ok"; d.run = ok_body;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;   /* no retries by default */
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    d.name = "bad"; d.run = bad_body;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(gptps_set_scheduler(e, spy, NULL) == GPTPS_OK);
    return e;
}

static void reset_seen(void)
{
    memset(g_dl, 0, sizeof g_dl); memset(g_enq, 0, sizeof g_enq);
    memset(g_prio, 0, sizeof g_prio); memset(g_seen, 0, sizeof g_seen); memset(g_runs, 0, sizeof g_runs);
}

static void drain(gptps *e) { size_t n; while (gptps_step(e, &n) == GPTPS_OK && n) { } }

static gptps_submit_options due_in(uint64_t ms, int32_t prio)
{
    gptps_submit_options o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.flags = GPTPS_SUBMIT_DEADLINE | GPTPS_SUBMIT_PRIORITY;
    o.deadline_in_ms = ms; o.priority = prio;
    return o;
}

/* "due in about `ms`" after a restart: what was left, give or take the test's own run */
static int left_about(unsigned char t, uint64_t ms)
{
    uint64_t left;
    if (!g_seen[t] || !g_dl[t] || g_dl[t] < g_enq[t]) return 0;
    left = g_dl[t] - g_enq[t];
    return left <= ms && left + 5000u >= ms;
}

int main(void)
{
    gptps *e;
    gptps_dq *dq;
    gptps_submit_options o;
    unsigned char t;
    remove(JOURNAL);

    /* run 1: journal, never step - all of it is still pending at shutdown */
    e = open_engine();
    dq = gptps_dq_open(e, JOURNAL); CHECK(dq != NULL);
    t = 'a'; o = due_in(600000, 7);                     /* due in ten minutes, priority 7 */
    CHECK(gptps_dq_submit_ex(dq, "ok", &t, 1, &o, NULL) == GPTPS_OK);
    t = 'l'; o = due_in(0, 0);                          /* due now: late by the next run */
    CHECK(gptps_dq_submit_ex(dq, "ok", &t, 1, &o, NULL) == GPTPS_OK);
    t = 'n';                                            /* no options at all */
    CHECK(gptps_dq_submit(dq, "ok", &t, 1, NULL) == GPTPS_OK);
    t = 'r';                                            /* a policy: two retries, so three runs */
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o; o.flags = GPTPS_SUBMIT_POLICY;
    o.policy.struct_size = sizeof o.policy; o.policy.max_retries = 2;
    o.policy.on_failure = GPTPS_ON_FAILURE_DROP;
    CHECK(gptps_dq_submit_ex(dq, "bad", &t, 1, &o, NULL) == GPTPS_OK);
    {   /* a batch: both items take the batch's options */
        gptps_dq_item it[2];
        unsigned char b1 = 'x', b2 = 'y';
        memset(it, 0, sizeof it);
        it[0].task_name = "ok"; it[0].payload = &b1; it[0].len = 1;
        it[1].task_name = "ok"; it[1].payload = &b2; it[1].len = 1;
        o = due_in(300000, -3);
        CHECK(gptps_dq_submit_batch_ex(dq, it, 2, &o) == GPTPS_OK);
        CHECK(it[0].status == GPTPS_OK && it[1].status == GPTPS_OK);
    }
    /* 6) refused before anything is written */
    {
        gptps_submit_options small = due_in(1000, 1);
        size_t before = gptps_dq_pending(dq);
        small.struct_size = offsetof(gptps_submit_options, timeout_ms);   /* ends before timeout_ms */
        t = 'z';
        CHECK(gptps_dq_submit_ex(dq, "ok", &t, 1, &small, NULL) == GPTPS_E_INVAL);
        CHECK(gptps_dq_pending(dq) == before);
    }
    CHECK(gptps_dq_pending(dq) == 6);
    CHECK(gptps_shutdown(e) == GPTPS_OK);               /* unstepped: stays pending */
    gptps_dq_close(dq);

    /* run 2 opens (and compacts), and does not step either: the options must outlive
     * the compaction for run 3 to see them */
    e = open_engine();
    dq = gptps_dq_open(e, JOURNAL); CHECK(dq != NULL);
    CHECK(gptps_dq_pending(dq) == 6);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    gptps_dq_close(dq);

    /* run 3: recover, and see what came back */
    reset_seen();
    e = open_engine();
    dq = gptps_dq_open(e, JOURNAL); CHECK(dq != NULL);
    CHECK(gptps_dq_recover(dq) == 6);
    drain(e);

    /* 1) and 3): priority and what is left of the due time */
    CHECK(g_seen['a'] && g_prio['a'] == 7);
    CHECK(left_about('a', 600000));
    /* 2) due while the host was down: due now */
    CHECK(g_seen['l'] && g_dl['l'] != 0 && g_dl['l'] - g_enq['l'] == 0);
    /* 4) journaled without options: none come back */
    CHECK(g_seen['n'] && g_dl['n'] == 0 && g_prio['n'] == 0);
    /* 1) the policy: a failing body ran its first attempt and both retries */
    CHECK(g_runs['r'] == 3);
    /* 5) the batch */
    CHECK(g_prio['x'] == -3 && g_prio['y'] == -3);
    CHECK(left_about('x', 300000) && left_about('y', 300000));
    CHECK(g_runs['a'] == 1 && g_runs['l'] == 1 && g_runs['n'] == 1 && g_runs['x'] == 1 && g_runs['y'] == 1);
    CHECK(gptps_dq_pending(dq) == 0);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    gptps_dq_close(dq);
    remove(JOURNAL);

    if (fails) { printf("%d durable-options check(s) FAILED\n", fails); return 1; }
    printf("durable options: all checks passed\n");
    return 0;
}
