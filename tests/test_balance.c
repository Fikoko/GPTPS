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
static int  dead_shutdown, dropped_shutdown, cancelled, shutdown_flagged;
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
    if (ev->flags & GPTPS_EV_FLAG_SHUTDOWN) __atomic_add_fetch(&shutdown_flagged, 1, __ATOMIC_SEQ_CST);
    if (ev->kind == GPTPS_EV_FAILED && ev->status == GPTPS_E_CANCELLED) __atomic_add_fetch(&cancelled, 1, __ATOMIC_SEQ_CST);
}
static void reset_log(void) { memset(terminals, 0, sizeof terminals); nstarted = 0; dead_shutdown = dropped_shutdown = cancelled = shutdown_flagged = 0; }
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
    CHECK(get(&shutdown_flagged) == 4);   /* this module's own: each refused dispatch */
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

/* 5) the service check reads nothing another thread can free. It walked
 *    gptps_task_get_info and strcmp'd the returned `name`, which is borrowed only
 *    until the registry next changes: an unregister on another thread (reg_destroy
 *    runs after the engine lock is dropped) freed it mid-compare. Types registered
 *    later are walked FIRST, so churning 16 of them on shard 0 while submitting a type
 *    registered before them puts every compare before the match on a name that may
 *    be going away. The churn registers at runtime, which the header calls setup-time
 *    work; the engine's locks support it, and nothing else keeps the frees coming.
 *    ASan/TSan only - a plain build reads the freed bytes without noticing. Before the
 *    fix: heap-use-after-free in task_is_service in every run. The churn runs on a
 *    worker of a separate engine, so the test needs no raw threads. */
static int churn_stop;
static gptps_status t_nop(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }
static gptps_status t_churn(gptps_ctx *c, void *u)
{
    gptps *e0 = (gptps *)u; char names[16][40]; int i;
    for (i = 0; i < 16; ++i) snprintf(names[i], sizeof names[i], "churn%02d_abcdefghijklmnop", i);
    while (!get(&churn_stop) && !gptps_is_cancelled(c)) {
        gptps_task_def d;
        for (i = 0; i < 16; ++i) {
            memset(&d, 0, sizeof d);
            d.struct_size = sizeof d; d.name = names[i]; d.run = t_nop; d.exec = GPTPS_EXEC_INPROC;
            d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = 1;
            d.default_policy.struct_size = sizeof d.default_policy;
            gptps_register_task(e0, &d);
        }
        for (i = 0; i < 16; ++i) gptps_unregister_task(e0, names[i], GPTPS_REMOVE_CANCEL);
    }
    return GPTPS_OK;
}
static void test_service_check_vs_unregister(void)
{
    gptps_pool *p = open_pool(1); gptps_balance *b; gptps *ec = NULL; gptps_task_def d;
    gptps_balance_handle h; uint64_t t0; unsigned long n = 0;
    reset_log();
    reg(p, "plain", t_nop, NULL);                   /* registered first: walked last */
    b = gptps_balance_open(p, NULL);
    CHECK(b != NULL);
    CHECK(gptps_open(NULL, &ec) == GPTPS_OK);
    if (!b || !ec) { if (ec) gptps_shutdown(ec); gptps_pool_close(p); gptps_balance_close(b); return; }
    gptps_balance_set_event_cb(b, on_ev, NULL);
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "churn"; d.run = t_churn; d.user_data = gptps_pool_shard(p, 0);
    d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = 1;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(ec, &d) == GPTPS_OK);
    __atomic_store_n(&churn_stop, 0, __ATOMIC_SEQ_CST);
    CHECK(gptps_submit(ec, "churn", NULL, 0, NULL) == GPTPS_OK);

    t0 = gptps_now_ms(NULL);
    while (gptps_now_ms(NULL) - t0 < 1000) {
        if (gptps_balance_queued(b) > 1000) continue;   /* keep the router queue bounded */
        CHECK(gptps_balance_submit(b, "plain", NULL, 0, &h) == GPTPS_OK);
        ++n;
    }
    __atomic_store_n(&churn_stop, 1, __ATOMIC_SEQ_CST);
    gptps_shutdown(ec);
    CHECK(n > 0);
    gptps_pool_close(p); gptps_balance_close(b);    /* the documented order */
}

/* 6) submit and cancel read nothing after the lock that another thread may free.
 *    Once b->mu drops, a queued item is reachable from the heap: a shard worker
 *    finishing earlier work runs dispatch_locked, which can pop it and - once it ends,
 *    or at once if it was cancelled - free it. submit_ex then read the item for the
 *    caller's handle and the QUEUED event, and cancel read it for the FAILED /
 *    CANCELLED one. Here the main thread submits and cancels top-priority items
 *    while the shard's own worker keeps dispatching filler work. ASan/TSan only.
 *    Before the fix: heap-use-after-free in gptps_balance_cancel or submit_ex. */
static void test_submit_and_cancel_read_nothing_freed(void)
{
    gptps_pool *p = open_pool(1); gptps_balance *b; gptps_balance_handle h;
    uint64_t t0; unsigned long cancelled_q = 0;
    reset_log();
    reg(p, "t", t_nop, NULL);
    b = gptps_balance_open(p, NULL);
    CHECK(b != NULL);
    if (!b) { gptps_pool_close(p); return; }
    gptps_balance_set_event_cb(b, on_ev, NULL);
    t0 = gptps_now_ms(NULL);
    while (gptps_now_ms(NULL) - t0 < 1000) {
        if (gptps_balance_queued(b) < 64)
            CHECK(gptps_balance_submit(b, "t", NULL, 0, &h) == GPTPS_OK);   /* filler */
        h = 0;
        if (gptps_balance_submit_ex(b, "t", NULL, 0, 100, &h) == GPTPS_OK && h &&
            gptps_balance_cancel(b, h) == GPTPS_OK) ++cancelled_q;
    }
    CHECK(cancelled_q > 0);
    gptps_pool_close(p); gptps_balance_close(b);    /* the documented order */
}

int main(void)
{
    test_distribution();
    test_order_and_cancel();
    test_dispatched_cancel_and_close();
    test_service_is_refused();
    test_service_check_vs_unregister();
    test_submit_and_cancel_read_nothing_freed();
    if (fails) { printf("%d balance check(s) FAILED\n", fails); return 1; }
    printf("all balance checks passed\n");
    return 0;
}
