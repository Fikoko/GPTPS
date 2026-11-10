/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_deadline.c - the due time (ABI 2.6): GPTPS_SUBMIT_DEADLINE carried to the
 * scheduler and constraint seams as deadline_ms, and nothing more.
 *
 *  1) The scheduler and the constraint see enqueue_ms + deadline_in_ms, on the clock
 *     of gptps_now_ms; an item without one shows 0, and so does one whose flag is
 *     set by a caller whose struct ends before the field (a 2.5 caller).
 *  2) An "earliest deadline first" score over deadline_ms orders admission.
 *  3) The core acts on none of it: an item already past its due time still runs, and
 *     its deadline survives a retry.
 *  4) A huge deadline_in_ms saturates instead of wrapping to "due first".
 *  5) The constraint's now_ms is the engine clock at the decision.
 *
 * MANUAL mode with one slot, so queue state and admission order are exact.
 */
#include "gptps.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

#define MAXH 16
static uint64_t g_sched_dl[MAXH];     /* by handle: deadline_ms the scheduler saw */
static uint64_t g_sched_enq[MAXH];
static uint64_t g_att_dl[4];         /* by attempt: deadline_ms the scheduler saw */
static uint64_t g_cons_dl[MAXH];      /* by handle: deadline_ms the constraint saw */
static uint64_t g_cons_now[MAXH];
static int      g_order[MAXH], g_order_n;
static int      g_fail_first;         /* "flaky" fails its first attempt */
static int      g_flaky_runs;

static int64_t edf(const gptps_sched_input *in, void *ud)
{
    uint64_t d = 0;
    (void)ud;
    if (in->struct_size >= offsetof(gptps_sched_input, deadline_ms) + sizeof in->deadline_ms)
        d = in->deadline_ms;
    if (in->handle < MAXH) { g_sched_dl[in->handle] = d; g_sched_enq[in->handle] = in->enqueue_ms; }
    if (in->attempt < 4) g_att_dl[in->attempt] = d;
    /* no deadline: after every item that has one */
    return d ? INT64_MAX - (int64_t)(d > (uint64_t)INT64_MAX / 2 ? (uint64_t)INT64_MAX / 2 : d)
             : (int64_t)in->priority;
}

static gptps_admit_decision cons(const gptps_constraint_input *in, uint32_t *ra, void *ud)
{
    (void)ra; (void)ud;
    if (in->handle < MAXH &&
        in->struct_size >= offsetof(gptps_constraint_input, now_ms) + sizeof in->now_ms) {
        g_cons_dl[in->handle] = in->deadline_ms;
        g_cons_now[in->handle] = in->now_ms;
    }
    return GPTPS_ADMIT;
}

static void on_ev(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_STARTED && g_order_n < MAXH) g_order[g_order_n++] = (int)ev->handle;
}

static gptps_status ok_task(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_OK; }
static gptps_status flaky(gptps_ctx *ctx, void *ud)
{
    (void)ctx; (void)ud;
    ++g_flaky_runs;
    if (g_fail_first) { g_fail_first = 0; return GPTPS_E_TASK; }
    return GPTPS_OK;
}

static void reg(gptps *e, const char *name, gptps_run_fn fn, uint32_t retries)
{
    gptps_task_def d; memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = fn; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = retries;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

static gptps *open_manual(void)
{
    gptps_config cfg; gptps *e = NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;
    cfg.limits.max_memory_bytes = 1u << 20;
    cfg.mode = GPTPS_RUN_MANUAL;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    return e;
}

static gptps_handle sub_dl(gptps *e, const char *task, uint64_t in_ms, size_t size)
{
    gptps_submit_options o; gptps_handle h = 0;
    memset(&o, 0, sizeof o);
    o.struct_size = size; o.flags = GPTPS_SUBMIT_DEADLINE; o.deadline_in_ms = in_ms;
    CHECK(gptps_submit_ex(e, task, NULL, 0, &o, &h) == GPTPS_OK);
    return h;
}

static void drain(gptps *e) { size_t n; while (gptps_step(e, &n) == GPTPS_OK && n) { } }

int main(void)
{
    /* 1 + 2: what the seams see, and EDF ordering */
    {
        gptps *e = open_manual();
        gptps_handle hl, hs, hn, hm, hold;
        uint64_t before = gptps_now_ms(NULL);
        reg(e, "t", ok_task, 0);
        CHECK(gptps_set_event_cb(e, on_ev, NULL) == GPTPS_OK);
        CHECK(gptps_set_scheduler(e, edf, NULL) == GPTPS_OK);
        CHECK(gptps_register_constraint(e, cons, NULL) == GPTPS_OK);

        hl = sub_dl(e, "t", 50000, sizeof(gptps_submit_options));   /* late */
        hs = sub_dl(e, "t", 1000, sizeof(gptps_submit_options));    /* soon */
        CHECK(gptps_submit(e, "t", NULL, 0, &hn) == GPTPS_OK);      /* none */
        hm = sub_dl(e, "t", 20000, sizeof(gptps_submit_options));   /* middle */
        /* a 2.5 caller: its struct ends at timeout_ms, so the flag has nothing to read */
        hold = sub_dl(e, "t", 10, offsetof(gptps_submit_options, timeout_ms) + sizeof(uint32_t));
        drain(e);

        CHECK(g_order_n == 5);
        CHECK(g_order[0] == (int)hs && g_order[1] == (int)hm && g_order[2] == (int)hl);
        /* the two without a deadline come last, FIFO between them */
        CHECK(g_order[3] == (int)hn && g_order[4] == (int)hold);

        CHECK(g_sched_dl[hs] == g_sched_enq[hs] + 1000);
        CHECK(g_sched_dl[hm] == g_sched_enq[hm] + 20000);
        CHECK(g_sched_dl[hl] == g_sched_enq[hl] + 50000);
        CHECK(g_sched_enq[hs] >= before && g_sched_enq[hs] <= gptps_now_ms(NULL));
        CHECK(g_sched_dl[hn] == 0);
        CHECK(g_sched_dl[hold] == 0);
        CHECK(g_cons_dl[hs] == g_sched_dl[hs]);
        CHECK(g_cons_dl[hn] == 0);
        /* 5: now_ms is the engine clock at the decision */
        CHECK(g_cons_now[hs] >= g_sched_enq[hs] && g_cons_now[hs] <= gptps_now_ms(NULL));
        CHECK(gptps_shutdown(e) == GPTPS_OK);
    }

    /* 3: a due time is an input, not a policy - a late item still runs, and its
     * deadline is the item's, so the retry carries the same one */
    {
        gptps *e = open_manual();
        gptps_handle h;
        memset(g_sched_dl, 0, sizeof g_sched_dl);
        g_order_n = 0; g_fail_first = 1; g_flaky_runs = 0;
        reg(e, "flaky", flaky, 1);
        CHECK(gptps_set_event_cb(e, on_ev, NULL) == GPTPS_OK);
        CHECK(gptps_set_scheduler(e, edf, NULL) == GPTPS_OK);
        h = sub_dl(e, "flaky", 0, sizeof(gptps_submit_options));     /* due at once: late by the time it runs */
        drain(e);
        CHECK(g_flaky_runs == 2);                                    /* late, and run anyway - twice */
        CHECK(g_att_dl[1] != 0);
        CHECK(g_att_dl[2] == g_att_dl[1]);                           /* the item's, not the attempt's */
        CHECK(g_sched_dl[h] == g_att_dl[1]);
        CHECK(gptps_shutdown(e) == GPTPS_OK);
    }

    /* 4: saturate, never wrap */
    {
        gptps *e = open_manual();
        gptps_handle h;
        memset(g_sched_dl, 0, sizeof g_sched_dl);
        g_order_n = 0;
        reg(e, "t", ok_task, 0);
        CHECK(gptps_set_scheduler(e, edf, NULL) == GPTPS_OK);
        h = sub_dl(e, "t", UINT64_MAX, sizeof(gptps_submit_options));
        drain(e);
        CHECK(g_sched_dl[h] == UINT64_MAX);
        CHECK(gptps_shutdown(e) == GPTPS_OK);
    }

    if (fails) { printf("%d deadline check(s) FAILED\n", fails); return 1; }
    printf("deadline: all checks passed\n");
    return 0;
}
