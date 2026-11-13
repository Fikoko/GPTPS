/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_route_opts.c - submit options through the add-ons that submit for you:
 * gptps_pool, gptps_orch and gptps_balance (gptps_xport: test_xport_opts).
 *
 *  1) pool: _submit_ex and _submit_keyed_ex hand the shard the options unchanged.
 *  2) orch: a held gate keeps its options, and its due time counts from the
 *     gptps_orch_after_ex call, not from the release - one that waited past it is
 *     released due now. The fast path (deps already terminal) applies them at once.
 *  3) balance: the shard gets the options, with the due time as what is left of it
 *     after waiting in the router; GPTPS_BALANCE_BY_DEADLINE orders the router's own
 *     queue earliest-due first, where the default orders by priority; and a config
 *     struct from before `flags` existed still opens.
 *
 * A scheduler on every engine records, by the payload's tag byte, the deadline_ms,
 * enqueue_ms and priority it was shown, and the order it first saw each tag. MANUAL
 * mode throughout, so everything happens on this thread, in a known order.
 */
#include "gptps.h"
#include "gptps_pool.h"
#include "gptps_orch.h"
#include "gptps_balance.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static uint64_t g_dl[256], g_enq[256];
static int32_t  g_prio[256];
static int      g_seen[256];
static char     g_order[64];
static int      g_n;

static void reset(void)
{
    memset(g_dl, 0, sizeof g_dl); memset(g_enq, 0, sizeof g_enq);
    memset(g_prio, 0, sizeof g_prio); memset(g_seen, 0, sizeof g_seen);
    memset(g_order, 0, sizeof g_order); g_n = 0;
}

static int64_t spy(const gptps_sched_input *in, void *ud)
{
    unsigned char t;
    (void)ud;
    if (!in->payload_len) return in->priority;
    t = ((const unsigned char *)in->payload)[0];
    if (!g_seen[t] && g_n < (int)sizeof g_order - 1) g_order[g_n++] = (char)t;
    g_seen[t] = 1;
    g_dl[t] = in->deadline_ms; g_enq[t] = in->enqueue_ms; g_prio[t] = in->priority;
    return in->priority;
}

static gptps_status ok_body(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_OK; }

static gptps_config manual_cfg(void)
{
    gptps_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;
    cfg.limits.max_memory_bytes = 1u << 20;
    cfg.mode = GPTPS_RUN_MANUAL;
    return cfg;
}

static void def_ok(gptps_task_def *d)
{
    memset(d, 0, sizeof *d);
    d->struct_size = sizeof *d; d->name = "t"; d->run = ok_body;
    d->default_cost.struct_size = sizeof d->default_cost;
    d->default_policy.struct_size = sizeof d->default_policy;
}

static gptps_submit_options due(uint64_t in_ms, int32_t prio)
{
    gptps_submit_options o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.flags = GPTPS_SUBMIT_DEADLINE | GPTPS_SUBMIT_PRIORITY;
    o.deadline_in_ms = in_ms; o.priority = prio;
    return o;
}

static void spin_ms(uint64_t ms) { uint64_t t = gptps_now_ms(NULL); while (gptps_now_ms(NULL) - t < ms) { } }
static void drain(gptps *e) { size_t n; while (gptps_step(e, &n) == GPTPS_OK && n) { } }

/* the due time a tag was shown, relative to `t0`: deadline_ms - t0 */
static int64_t due_from(unsigned char t, uint64_t t0) { return (int64_t)(g_dl[t] - t0); }

int main(void)
{
    gptps_config cfg = manual_cfg();
    gptps_task_def d;
    gptps_submit_options o;
    unsigned char tag;
    size_t i;

    /* 1) pool */
    {
        gptps_pool *p = gptps_pool_open(2, &cfg);
        gptps_pool_handle ph = 0;
        CHECK(p != NULL);
        if (p) {
            reset();
            def_ok(&d);
            CHECK(gptps_pool_register_task(p, &d) == GPTPS_OK);
            for (i = 0; i < gptps_pool_count(p); ++i) CHECK(gptps_set_scheduler(gptps_pool_shard(p, i), spy, NULL) == GPTPS_OK);
            tag = 'p'; o = due(5000, 3);
            CHECK(gptps_pool_submit_ex(p, "t", &tag, 1, &o, &ph) == GPTPS_OK && ph != 0);
            tag = 'k'; o = due(7000, -2);
            CHECK(gptps_pool_submit_keyed_ex(p, 42, "t", &tag, 1, &o, &ph) == GPTPS_OK);
            tag = 'n';
            CHECK(gptps_pool_submit(p, "t", &tag, 1, NULL) == GPTPS_OK);
            o.struct_size = offsetof(gptps_submit_options, timeout_ms);     /* undersized */
            CHECK(gptps_pool_submit_ex(p, "t", &tag, 1, &o, NULL) == GPTPS_E_INVAL);
            for (i = 0; i < gptps_pool_count(p); ++i) drain(gptps_pool_shard(p, i));
            CHECK(g_seen['p'] && g_dl['p'] - g_enq['p'] == 5000 && g_prio['p'] == 3);
            CHECK(g_seen['k'] && g_dl['k'] - g_enq['k'] == 7000 && g_prio['k'] == -2);
            CHECK(g_seen['n'] && g_dl['n'] == 0);
            gptps_pool_close(p);
        }
    }

    /* 2) orch */
    {
        gptps *e = NULL;
        gptps_orch *orch;
        gptps_handle dep = 0, h = 1;
        uint64_t t0, t1;
        reset();
        CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
        def_ok(&d);
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        CHECK(gptps_set_scheduler(e, spy, NULL) == GPTPS_OK);
        orch = gptps_orch_install(e);
        CHECK(orch != NULL);
        tag = 'D';
        CHECK(gptps_submit(e, "t", &tag, 1, &dep) == GPTPS_OK);
        t0 = gptps_now_ms(NULL);
        tag = 'g'; o = due(10000, 4);                       /* held: due 10 s after THIS call */
        CHECK(gptps_orch_after_ex(orch, "t", &tag, 1, &dep, 1, &o, &h) == GPTPS_OK && h == 0);
        tag = 'l'; o = due(10, 0);                          /* held past its due time */
        CHECK(gptps_orch_after_ex(orch, "t", &tag, 1, &dep, 1, &o, &h) == GPTPS_OK);
        o.struct_size = offsetof(gptps_submit_options, timeout_ms);
        CHECK(gptps_orch_after_ex(orch, "t", &tag, 1, &dep, 1, &o, &h) == GPTPS_E_INVAL);
        spin_ms(60);
        drain(e);                                           /* D runs; both gates release */
        t1 = gptps_now_ms(NULL);
        CHECK(g_seen['g'] && g_prio['g'] == 4);
        /* counted from the call: within the few ms between t0 and the call itself */
        CHECK(due_from('g', t0) >= 10000 && due_from('g', t0) <= 10000 + 20);
        CHECK(g_enq['g'] >= t0 + 60);                       /* ...though released 60 ms later */
        CHECK(g_seen['l'] && g_dl['l'] - g_enq['l'] == 0);  /* late by the release: due now */
        /* fast path: the dependency is terminal now, so the options apply at once */
        tag = 'f'; o = due(3000, 1);
        CHECK(gptps_orch_after_ex(orch, "t", &tag, 1, &dep, 1, &o, &h) == GPTPS_OK && h != 0);
        drain(e);
        CHECK(g_seen['f'] && g_dl['f'] - g_enq['f'] == 3000 && g_prio['f'] == 1);
        CHECK(t1 >= t0);
        CHECK(gptps_shutdown(e) == GPTPS_OK);
        gptps_orch_close(orch);
    }

    /* 3) balance: one shard of depth 1, so all but one item wait in the router */
    {
        int pass;
        for (pass = 0; pass < 2; ++pass) {
            gptps_pool *p = gptps_pool_open(1, &cfg);
            gptps_balance_config bc;
            gptps_balance *b;
            gptps *sh;
            uint64_t t0;
            CHECK(p != NULL);
            if (!p) break;
            reset();
            def_ok(&d);
            CHECK(gptps_pool_register_task(p, &d) == GPTPS_OK);
            sh = gptps_pool_shard(p, 0);
            CHECK(gptps_set_scheduler(sh, spy, NULL) == GPTPS_OK);
            memset(&bc, 0, sizeof bc);
            bc.shard_depth = 1;
            if (pass == 0) {
                bc.struct_size = sizeof bc; bc.flags = GPTPS_BALANCE_BY_DEADLINE;
            } else {                                          /* a 1.7 caller: no flags field */
                bc.struct_size = offsetof(gptps_balance_config, shard_depth) + sizeof bc.shard_depth;
            }
            b = gptps_balance_open(p, &bc);
            CHECK(b != NULL);
            if (!b) { gptps_pool_close(p); break; }
            t0 = gptps_now_ms(NULL);
            tag = 'X'; CHECK(gptps_balance_submit(b, "t", &tag, 1, NULL) == GPTPS_OK);   /* takes the shard */
            tag = 'A'; o = due(30000, 9); CHECK(gptps_balance_submit_opts(b, "t", &tag, 1, &o, NULL) == GPTPS_OK);
            tag = 'B'; o = due(2000, 0);  CHECK(gptps_balance_submit_opts(b, "t", &tag, 1, &o, NULL) == GPTPS_OK);
            tag = 'C'; o = due(20000, 5); CHECK(gptps_balance_submit_opts(b, "t", &tag, 1, &o, NULL) == GPTPS_OK);
            o.struct_size = offsetof(gptps_submit_options, timeout_ms);
            CHECK(gptps_balance_submit_opts(b, "t", &tag, 1, &o, NULL) == GPTPS_E_INVAL);
            spin_ms(40);                                      /* time spent waiting in the router */
            drain(sh);
            if (pass == 0) CHECK(strcmp(g_order, "XBCA") == 0);   /* earliest due first */
            else           CHECK(strcmp(g_order, "XACB") == 0);   /* by priority: 9, 5, 0 */
            /* the shard was told what was left of each due time, counted from submit */
            CHECK(due_from('B', t0) >= 2000 && due_from('B', t0) <= 2000 + 20);
            CHECK(g_enq['B'] >= t0 + 40);
            CHECK(g_prio['A'] == 9 && g_prio['C'] == 5);
            gptps_pool_close(p);
            gptps_balance_close(b);
        }
    }

    if (fails) { printf("%d routed-options check(s) FAILED\n", fails); return 1; }
    printf("routed options: all checks passed\n");
    return 0;
}
