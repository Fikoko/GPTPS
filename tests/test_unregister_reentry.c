/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_unregister_reentry.c - gptps_unregister_task called from the engine's own
 * threads (THREADED mode) refuses with GPTPS_E_BUSY instead of waiting for a drain
 * that needs the very thread it is waiting on, and leaves the type untouched.
 *
 * On the dispatcher (a RETRIED callback), with the type's work in each place a drain
 * would wait on:
 *   - running on a worker ("blk")            DRAIN, CANCEL -> E_BUSY
 *   - finished but not yet accounted ("d")   DRAIN, CANCEL -> E_BUSY
 *   - only queued ("q")                      DRAIN -> E_BUSY; CANCEL -> OK, the type is
 *                                            gone and its handle gets one terminal event
 *   - the retried type itself ("flaky")      DRAIN, REJECT_IF_BUSY -> E_BUSY
 *   - an idle type ("idle")                  DRAIN -> OK
 * On a worker:
 *   - a task body removing its own type ("self")          CANCEL, DRAIN -> E_BUSY
 *   - a FINISHED callback removing its own type ("fin")   CANCEL, DRAIN -> E_BUSY
 * Then, from a thread of our own, every removal goes through.
 *
 * Before the fix this test does not fail, it hangs; the CTest TIMEOUT is its verdict.
 * Headless / portable.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static void put(int *p, int v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static void bump(int *p) { __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }

static gptps *eng;
static int go, blk_running, d_running, d_release, d_returned, flaky_calls;
static int cb_done, self_done, fin_done, q_terminal;
static int st_blk_drain, st_blk_cancel, st_d_drain, st_d_cancel, st_q_submit, st_q_drain,
           st_q_cancel, st_q_exists, st_fl_drain, st_fl_reject, st_idle, st_self_cancel,
           st_self_drain, st_fin_cancel, st_fin_drain;

/* Spin (portable: no sleep API needed) until *flag is set or 5 s pass. */
static int wait_flag(int *flag)
{
    uint64_t t0 = gptps_now_ms(NULL);
    while (!get(flag)) if (gptps_now_ms(NULL) - t0 > 5000) return 0;
    return 1;
}
static void spin_ms(unsigned ms) { uint64_t t0 = gptps_now_ms(NULL); while (gptps_now_ms(NULL) - t0 < ms) { } }

static gptps_status task_blk(gptps_ctx *c, void *u)
{
    (void)u;
    put(&blk_running, 1);
    while (!get(&go) && !gptps_is_cancelled(c)) { }
    return GPTPS_OK;
}
static gptps_status task_d(gptps_ctx *c, void *u)
{
    (void)u;
    put(&d_running, 1);
    while (!get(&d_release) && !gptps_is_cancelled(c)) { }
    put(&d_returned, 1);
    return GPTPS_OK;
}
static gptps_status task_flaky(gptps_ctx *c, void *u)
{
    (void)c; (void)u;
    return __atomic_add_fetch(&flaky_calls, 1, __ATOMIC_SEQ_CST) == 1 ? GPTPS_E_TASK : GPTPS_OK;
}
static gptps_status task_self(gptps_ctx *c, void *u)
{
    (void)c; (void)u;
    put(&st_self_cancel, (int)gptps_unregister_task(eng, "self", GPTPS_REMOVE_CANCEL));
    put(&st_self_drain,  (int)gptps_unregister_task(eng, "self", GPTPS_REMOVE_DRAIN));
    put(&self_done, 1);
    return GPTPS_OK;
}
static gptps_status task_nop(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }

static void on_retried(void)          /* on the dispatcher, which admits nothing meanwhile */
{
    gptps_handle h;
    /* running */
    put(&st_blk_drain,  (int)gptps_unregister_task(eng, "blk", GPTPS_REMOVE_DRAIN));
    put(&st_blk_cancel, (int)gptps_unregister_task(eng, "blk", GPTPS_REMOVE_CANCEL));
    /* queued only: submitted now, it stays in intake until this callback returns */
    put(&st_q_submit, (int)gptps_submit(eng, "q", NULL, 0, &h));
    put(&st_q_drain,  (int)gptps_unregister_task(eng, "q", GPTPS_REMOVE_DRAIN));
    put(&st_q_cancel, (int)gptps_unregister_task(eng, "q", GPTPS_REMOVE_CANCEL));
    put(&st_q_exists, gptps_task_exists(eng, "q"));
    /* the retried type itself: its own retry is still live */
    put(&st_fl_drain,  (int)gptps_unregister_task(eng, "flaky", GPTPS_REMOVE_DRAIN));
    put(&st_fl_reject, (int)gptps_unregister_task(eng, "flaky", GPTPS_REMOVE_REJECT_IF_BUSY));
    /* nothing to wait for */
    put(&st_idle, (int)gptps_unregister_task(eng, "idle", GPTPS_REMOVE_DRAIN));
    /* finished but not accounted: "d" returns on its worker and lands in `done`, which
     * only this thread drains. The margin lets the worker get there; were it slower,
     * "d" would still be running, and the answer is the same. */
    put(&d_release, 1);
    wait_flag(&d_returned);
    spin_ms(100);
    put(&st_d_drain,  (int)gptps_unregister_task(eng, "d", GPTPS_REMOVE_DRAIN));
    put(&st_d_cancel, (int)gptps_unregister_task(eng, "d", GPTPS_REMOVE_CANCEL));
    put(&cb_done, 1);
}

static void on_event(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_FAILED && ev->status == GPTPS_E_CANCELLED &&
        strcmp(ev->task_name, "q") == 0) bump(&q_terminal);
    if (ev->kind == GPTPS_EV_FINISHED && strcmp(ev->task_name, "fin") == 0) {   /* a worker */
        put(&st_fin_cancel, (int)gptps_unregister_task(eng, "fin", GPTPS_REMOVE_CANCEL));
        put(&st_fin_drain,  (int)gptps_unregister_task(eng, "fin", GPTPS_REMOVE_DRAIN));
        put(&fin_done, 1);
    }
    if (ev->kind == GPTPS_EV_RETRIED && strcmp(ev->task_name, "flaky") == 0) on_retried();
}

static void reg(const char *name, gptps_run_fn fn, uint32_t retries)
{
    gptps_task_def d; memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = fn; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = 1;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = retries;
    CHECK(gptps_register_task(eng, &d) == GPTPS_OK);
}

int main(void)
{
    gptps_config cfg; gptps_handle h;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.mode = GPTPS_RUN_THREADED;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 3; cfg.limits.max_memory_bytes = 1024;
    CHECK(gptps_open_ex(&cfg, &eng) == GPTPS_OK && eng);
    if (!eng) return 1;
    CHECK(gptps_set_event_cb(eng, on_event, NULL) == GPTPS_OK);
    reg("blk", task_blk, 0);
    reg("d", task_d, 0);
    reg("flaky", task_flaky, 1);
    reg("q", task_nop, 0);
    reg("idle", task_nop, 0);
    reg("self", task_self, 0);
    reg("fin", task_nop, 0);

    /* the dispatcher: "blk" and "d" hold two slots, "flaky" fails in the third */
    CHECK(gptps_submit(eng, "blk", NULL, 0, &h) == GPTPS_OK);
    CHECK(wait_flag(&blk_running));
    CHECK(gptps_submit(eng, "d", NULL, 0, &h) == GPTPS_OK);
    CHECK(wait_flag(&d_running));
    CHECK(gptps_submit(eng, "flaky", NULL, 0, &h) == GPTPS_OK);
    CHECK(wait_flag(&cb_done));
    CHECK(get(&st_blk_drain) == GPTPS_E_BUSY && get(&st_blk_cancel) == GPTPS_E_BUSY);
    CHECK(get(&st_d_drain)   == GPTPS_E_BUSY && get(&st_d_cancel)   == GPTPS_E_BUSY);
    CHECK(get(&st_q_submit)  == GPTPS_OK);
    CHECK(get(&st_q_drain)   == GPTPS_E_BUSY);
    CHECK(get(&st_q_cancel)  == GPTPS_OK);        /* queued-only work is detached, not waited on */
    CHECK(get(&st_q_exists)  == 0);
    CHECK(get(&q_terminal)   == 1);               /* ...and its handle still ends, once */
    CHECK(get(&st_fl_drain)  == GPTPS_E_BUSY && get(&st_fl_reject) == GPTPS_E_BUSY);
    CHECK(get(&st_idle)      == GPTPS_OK);
    CHECK(gptps_task_exists(eng, "blk") == 1 && gptps_task_exists(eng, "d") == 1);
    CHECK(gptps_task_exists(eng, "flaky") == 1 && gptps_task_exists(eng, "idle") == 0);

    /* a worker: a task body, then a FINISHED callback, each removing its own type */
    CHECK(gptps_submit(eng, "self", NULL, 0, &h) == GPTPS_OK);
    CHECK(wait_flag(&self_done));
    CHECK(get(&st_self_cancel) == GPTPS_E_BUSY && get(&st_self_drain) == GPTPS_E_BUSY);
    CHECK(gptps_submit(eng, "fin", NULL, 0, &h) == GPTPS_OK);
    CHECK(wait_flag(&fin_done));
    CHECK(get(&st_fin_cancel) == GPTPS_E_BUSY && get(&st_fin_drain) == GPTPS_E_BUSY);
    CHECK(gptps_task_exists(eng, "self") == 1 && gptps_task_exists(eng, "fin") == 1);

    /* From a thread of our own, every removal goes through. */
    put(&go, 1);
    CHECK(gptps_unregister_task(eng, "blk",   GPTPS_REMOVE_DRAIN) == GPTPS_OK);
    CHECK(gptps_unregister_task(eng, "d",     GPTPS_REMOVE_DRAIN) == GPTPS_OK);
    CHECK(gptps_unregister_task(eng, "flaky", GPTPS_REMOVE_DRAIN) == GPTPS_OK);
    CHECK(gptps_unregister_task(eng, "self",  GPTPS_REMOVE_DRAIN) == GPTPS_OK);
    CHECK(gptps_unregister_task(eng, "fin",   GPTPS_REMOVE_DRAIN) == GPTPS_OK);
    CHECK(gptps_shutdown(eng) == GPTPS_OK);

    if (fails) { printf("%d unregister-reentry check(s) FAILED\n", fails); return 1; }
    printf("all unregister-reentry checks passed\n");
    return 0;
}
