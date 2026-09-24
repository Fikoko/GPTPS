/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_balance.c - the late-binding router above gptps_pool. Proves:
 *   - distribution: with one long item and many short ones, the short ones all land
 *     on the shard that is free, and the total time is the long item's, not a queue
 *     stuck behind it (round-robin would put half the short ones behind the long one)
 *   - ordering in the router: priority first, then FIFO
 *   - one terminal event per balance handle, including for cancelled-while-queued,
 *     cancelled-while-dispatched, unknown-at-dispatch and still-queued-at-close items
 *   - gauges: queued / shard_load rise and fall as work moves
 *   - depth: a shard is never handed more than shard_depth at once
 * Headless / portable.
 */
#include "gptps_balance.h"
#include "gptps_stats.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static void spin_ms(gptps_ctx *c, unsigned ms)
{ uint64_t s = gptps_now_ms(c); while (gptps_now_ms(c) - s < ms) { if (gptps_is_cancelled(c)) return; } }
static gptps_status t_long(gptps_ctx *c, void *u)  { (void)u; spin_ms(c, 200); return GPTPS_OK; }
static gptps_status t_short(gptps_ctx *c, void *u) { (void)u; spin_ms(c, 10);  return GPTPS_OK; }
static gptps_status t_block(gptps_ctx *c, void *u)
{ int *go = (int *)u; while (!get(go) && !gptps_is_cancelled(c)) { } return GPTPS_OK; }

/* event log from the balancer: terminal counts per handle, and the STARTED order */
#define MAXH 64
static int  terminals[MAXH];
static int  started_order[MAXH], nstarted;
static int  dead_shutdown, dropped_shutdown, cancelled;
static void on_ev(const gptps_event *ev, void *u)
{
    (void)u;
    if (ev->handle < MAXH) {
        if (ev->kind == GPTPS_EV_STARTED) started_order[__atomic_fetch_add(&nstarted, 1, __ATOMIC_SEQ_CST)] = (int)ev->handle;
        if (ev->kind == GPTPS_EV_FINISHED || ev->kind == GPTPS_EV_DROPPED || ev->kind == GPTPS_EV_DEAD_LETTERED ||
            (ev->kind == GPTPS_EV_FAILED && ev->status == GPTPS_E_CANCELLED))
            __atomic_add_fetch(&terminals[ev->handle], 1, __ATOMIC_SEQ_CST);
    }
    if (ev->kind == GPTPS_EV_DEAD_LETTERED && ev->status == GPTPS_E_SHUTDOWN) __atomic_add_fetch(&dead_shutdown, 1, __ATOMIC_SEQ_CST);
    if (ev->kind == GPTPS_EV_DROPPED && ev->status == GPTPS_E_SHUTDOWN) __atomic_add_fetch(&dropped_shutdown, 1, __ATOMIC_SEQ_CST);
    if (ev->kind == GPTPS_EV_FAILED && ev->status == GPTPS_E_CANCELLED) __atomic_add_fetch(&cancelled, 1, __ATOMIC_SEQ_CST);
}
static void reset_log(void) { memset(terminals, 0, sizeof terminals); nstarted = 0; dead_shutdown = dropped_shutdown = cancelled = 0; }
static int terminal_total(void) { int i, n = 0; for (i = 0; i < MAXH; ++i) n += get(&terminals[i]); return n; }
static void wait_terminals(int n)
{ uint64_t s = gptps_now_ms(NULL); while (terminal_total() < n && gptps_now_ms(NULL) - s < 5000) { } }

static void reg(gptps_pool *p, const char *n, gptps_run_fn f, void *ud)
{
    gptps_task_def d; memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = n; d.run = f; d.exec = GPTPS_EXEC_INPROC; d.user_data = ud;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = 1;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_pool_register_task(p, &d) == GPTPS_OK);
}
/* a service body: loops until stopped, like every supervised instance must */
static gptps_status t_svc(gptps_ctx *c, void *u)
{ (void)u; while (!gptps_is_cancelled(c)) { } return GPTPS_E_CANCELLED; }

static void reg_service(gptps_pool *p, const char *n, gptps_run_fn f)
{
    gptps_task_def d; memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = n; d.run = f; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = 1;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.flags = GPTPS_TASK_SERVICE;           /* legal on a pool; NOT through a balancer */
    CHECK(gptps_pool_register_task(p, &d) == GPTPS_OK);
}

static gptps_pool *open_pool(size_t nshards)
{
    gptps_config cfg; gptps_pool *p; size_t i;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1; cfg.limits.max_memory_bytes = 1024;
    p = gptps_pool_open(nshards, &cfg);
    CHECK(p != NULL);
    for (i = 0; i < nshards; ++i) gptps_settings_set(gptps_pool_shard(p, i), "limits.shutdown_grace_ms", "500");
    return p;
}

/* 1) distribution + time: 2 shards x 1 worker, depth 1 (no prefetch). One long
 *    (200ms) then 6 short (10ms). Balanced: every short goes to the free shard;
 *    total ~200ms. Round-robin would queue 3 shorts behind the long one. */
static void test_distribution(void)
{
    gptps_pool *p = open_pool(2); gptps_balance *b; gptps_balance_handle h;
    gptps_stats *st0, *st1; gptps_stats_counters c0, c1; int i; uint64_t t0;
    gptps_balance_config bc; memset(&bc, 0, sizeof bc); bc.struct_size = sizeof bc; bc.shard_depth = 1;
    reset_log();
    reg(p, "long", t_long, NULL); reg(p, "short", t_short, NULL);
    st0 = gptps_stats_install(gptps_pool_shard(p, 0)); st1 = gptps_stats_install(gptps_pool_shard(p, 1));
    b = gptps_balance_open(p, &bc);
    CHECK(b != NULL);
    gptps_balance_set_event_cb(b, on_ev, NULL);

    t0 = gptps_now_ms(NULL);
    CHECK(gptps_balance_submit(b, "long", NULL, 0, &h) == GPTPS_OK);
    for (i = 0; i < 6; ++i) CHECK(gptps_balance_submit(b, "short", NULL, 0, &h) == GPTPS_OK);
    wait_terminals(7);
    CHECK(gptps_now_ms(NULL) - t0 < 500);            /* the long item's time (200ms), not a queue behind it */
    CHECK(terminal_total() == 7);
    gptps_stats_total(st0, &c0); gptps_stats_total(st1, &c1);
    /* whichever shard took the long item started exactly 1; the other took all 6.
     * (`started`, not `finished`: the balancer's terminal event may be observed a
     * moment before the stats observer sees the same FINISHED - two observers on one
     * event - but every EARLIER event on an engine is fully delivered first.) */
    CHECK((c0.started == 1 && c1.started == 6) || (c0.started == 6 && c1.started == 1));
    CHECK(gptps_balance_queued(b) == 0);
    CHECK(gptps_balance_shard_load(b, 0) == 0 && gptps_balance_shard_load(b, 1) == 0);

    gptps_pool_close(p);
    gptps_balance_close(b);
    gptps_stats_close(st0); gptps_stats_close(st1);
}

/* 2) router ordering (priority, then FIFO), gauges, depth, cancel-while-queued */
static void test_order_and_cancel(void)
{
    gptps_pool *p = open_pool(1); gptps_balance *b; gptps_balance_handle hb, ha, hp, hc, hz; int go = 0;
    gptps_balance_config bc; memset(&bc, 0, sizeof bc); bc.struct_size = sizeof bc; bc.shard_depth = 1;
    reset_log();
    reg(p, "block", t_block, &go); reg(p, "short", t_short, NULL);
    b = gptps_balance_open(p, &bc);
    gptps_balance_set_event_cb(b, on_ev, NULL);

    CHECK(gptps_balance_submit(b, "nope", NULL, 0, &hz) == GPTPS_E_NOTFOUND);
    CHECK(gptps_balance_submit(b, "block", NULL, 0, &hb) == GPTPS_OK);          /* dispatched: occupies the shard */
    CHECK(gptps_balance_submit_ex(b, "short", NULL, 0, 0, &ha) == GPTPS_OK);    /* queued here, prio 0 */
    CHECK(gptps_balance_submit_ex(b, "short", NULL, 0, 5, &hp) == GPTPS_OK);    /* queued here, prio 5 */
    CHECK(gptps_balance_submit_ex(b, "short", NULL, 0, 0, &hc) == GPTPS_OK);    /* queued here, prio 0, to cancel */
    CHECK(gptps_balance_queued(b) == 3);
    CHECK(gptps_balance_shard_load(b, 0) == 1);                                  /* depth 1: only block handed over */

    CHECK(gptps_balance_cancel(b, hc) == GPTPS_OK);
    CHECK(get(&cancelled) == 1 && get(&terminals[hc]) == 1);                    /* one terminal, right away */
    CHECK(gptps_balance_cancel(b, hc) == GPTPS_E_NOTFOUND);
    CHECK(gptps_balance_queued(b) == 2);

    __atomic_store_n(&go, 1, __ATOMIC_SEQ_CST);
    wait_terminals(4);
    CHECK(terminal_total() == 4);
    CHECK(get(&nstarted) == 3);
    CHECK(started_order[0] == (int)hb && started_order[1] == (int)hp && started_order[2] == (int)ha);   /* block, then prio 5, then prio 0 */
    CHECK(gptps_balance_cancel(b, ha) == GPTPS_E_NOTFOUND);                     /* finished: gone */

    gptps_pool_close(p);
    gptps_balance_close(b);
}

/* 3) cancel-while-dispatched forwards to the shard; close with work still queued
 *    gives every remaining handle exactly one terminal event */
static void test_dispatched_cancel_and_close(void)
{
    gptps_pool *p = open_pool(1); gptps_balance *b; gptps_balance_handle hb, h, hs[4]; int go = 0, i;
    gptps_balance_config bc; memset(&bc, 0, sizeof bc); bc.struct_size = sizeof bc; bc.shard_depth = 1;
    reset_log();
    reg(p, "block", t_block, &go); reg(p, "short", t_short, NULL);
    b = gptps_balance_open(p, &bc);
    gptps_balance_set_event_cb(b, on_ev, NULL);

    CHECK(gptps_balance_submit(b, "block", NULL, 0, &hb) == GPTPS_OK);
    for (i = 0; i < 5; ++i) CHECK(gptps_balance_submit(b, "short", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_balance_queued(b) == 5);

    CHECK(gptps_balance_cancel(b, hb) == GPTPS_OK);   /* dispatched: gptps_cancel on the shard */
    wait_terminals(1);
    CHECK(get(&terminals[hb]) == 1);
    /* the shard is now free, so the router drains the 5 shorts normally */
    wait_terminals(6);
    CHECK(terminal_total() == 6);

    /* now: block the shard again and leave work queued in the router across close */
    __atomic_store_n(&go, 0, __ATOMIC_SEQ_CST);
    reset_log();
    CHECK(gptps_balance_submit(b, "block", NULL, 0, &hb) == GPTPS_OK);
    for (i = 0; i < 4; ++i) CHECK(gptps_balance_submit(b, "short", NULL, 0, &hs[i]) == GPTPS_OK);
    CHECK(gptps_balance_queued(b) == 4);
    gptps_pool_close(p);       /* grace 500ms cancels block -> its terminal frees room -> the
                                * router tries to hand over -> the shard refuses (shutting down)
                                * -> DEAD_LETTERED/E_SHUTDOWN here; anything else still queued
                                * is DROPPED/E_SHUTDOWN at balance_close */
    gptps_balance_close(b);
    CHECK(terminal_total() == 5);
    CHECK(get(&dead_shutdown) + get(&dropped_shutdown) == 4);
    CHECK(get(&terminals[hb]) == 1);                             /* exactly one each */
    for (i = 0; i < 4; ++i) CHECK(get(&terminals[hs[i]]) == 1);
}

/* 4) a service is refused at the boundary. Routed through here it would corrupt the
 *    router's own load accounting, and nothing in the event stream would show it: a
 *    service's FIRST run ends with its own terminal event, which this module takes
 *    for the item finishing - it drops the shard mapping, decrements that shard's
 *    load and frees the item, while the instance is still on the shard and about to
 *    restart. The router then believes in a free slot that does not exist, for good.
 *    Measured before the check: shard loads [0,0] with the instance still running.
 *
 *    Worse, losing the item loses the only handle close() had on it. Without the
 *    refusal this very test does not merely fail, it CRASHES: gptps_balance_close
 *    frees the balancer while the instance is still running with this module's
 *    observer registered, and ASan reports a heap-use-after-free in observe() on a
 *    worker thread. Forwarding was never the problem (a later terminal event for the
 *    same shard handle finds no mapping, so a balance handle still reaches exactly
 *    one) - which is why this had to be refused at the boundary, not detected. */
static void test_service_is_refused(void)
{
    gptps_pool *p = open_pool(2); gptps_balance *b; gptps_balance_handle h = 12345;
    gptps_balance_config bc; memset(&bc, 0, sizeof bc);
    bc.struct_size = sizeof bc; bc.shard_depth = 2;
    reset_log();
    reg(p, "plain", t_short, NULL);
    reg_service(p, "svc", t_svc);
    b = gptps_balance_open(p, &bc);
    CHECK(b != NULL);
    if (!b) { gptps_pool_close(p); return; }
    gptps_balance_set_event_cb(b, on_ev, NULL);

    CHECK(gptps_balance_submit(b, "svc", NULL, 0, &h) == GPTPS_E_INVAL);
    CHECK(h == 0);                                   /* refused: no handle issued */
    CHECK(gptps_balance_queued(b) == 0);
    CHECK(gptps_balance_shard_load(b, 0) == 0 && gptps_balance_shard_load(b, 1) == 0);
    CHECK(terminal_total() == 0);                    /* and no phantom terminal event */

    /* an ordinary type on the same pool is untouched by the check */
    CHECK(gptps_balance_submit(b, "plain", NULL, 0, &h) == GPTPS_OK);
    wait_terminals(1);
    CHECK(terminal_total() == 1);

    gptps_balance_close(b); gptps_pool_close(p);
}

int main(void)
{
    test_distribution();
    test_order_and_cancel();
    test_dispatched_cancel_and_close();
    test_service_is_refused();
    if (fails) { printf("%d balance check(s) FAILED\n", fails); return 1; }
    printf("all balance checks passed\n");
    return 0;
}
