/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/* Retry notifications must reach observers before the next attempt starts.
 * Exercise a bounded slow callback and real stats, plus pending-buffer overflow.
 * The gate amplifies the scheduling window; it does not measure race frequency. */
#include "gptps_stats.h"
#include "gptps_hal.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

typedef struct {
    gptps_mutex *mu;
    gptps_cond *cv;
    int mode, queued, started2, finished2, retried, calls;
    int gate_timeouts, setup_timeouts;
} probe;

/* Caller holds mu. Condition waits release it; no engine lock is held here. */
static int wait_for(probe *p, int *flag, uint64_t timeout_ms)
{
    uint64_t end = gptps_hal_monotonic_ms() + timeout_ms;
    while (!*flag) {
        uint64_t now = gptps_hal_monotonic_ms();
        if (now >= end) return 1;
        gptps_cond_timedwait(p->cv, p->mu, end - now);
    }
    return 0;
}

static void primary(const gptps_event *ev, void *ud)
{
    probe *p = ud;
    if (ev->kind != GPTPS_EV_RETRIED || !p->mode) return;
    gptps_mutex_lock(p->mu);
    p->gate_timeouts += wait_for(p, p->mode == 1 ? &p->started2 : &p->finished2, 50);
    gptps_mutex_unlock(p->mu);
}

/* Registered BEFORE stats: engine inserts observers at the head, so the stats
 * observer consumes each event before this observer acknowledges it. */
static void acknowledge(const gptps_event *ev, void *ud)
{
    probe *p = ud;
    gptps_mutex_lock(p->mu);
    if (ev->kind == GPTPS_EV_QUEUED) p->queued = 1;
    if (ev->kind == GPTPS_EV_STARTED && ev->attempt == 2) p->started2 = 1;
    if (ev->kind == GPTPS_EV_FINISHED && ev->attempt == 2) p->finished2 = 1;
    if (ev->kind == GPTPS_EV_RETRIED) p->retried = 1;
    gptps_cond_broadcast(p->cv);
    gptps_mutex_unlock(p->mu);
}

static gptps_status task(gptps_ctx *ctx, void *ud)
{
    probe *p = ud;
    int n;
    (void)ctx;
    gptps_mutex_lock(p->mu);
    n = ++p->calls;
    /* Isolate the retry inversion from the independent late-QUEUED race. */
    if (n == 1) p->setup_timeouts += wait_for(p, &p->queued, 2000);
    if (n == 2 && p->mode == 1) p->setup_timeouts += wait_for(p, &p->retried, 2000);
    gptps_mutex_unlock(p->mu);
    return n == 1 ? GPTPS_E_TASK : GPTPS_OK;
}

static int same(const gptps_stats_counters *a, const gptps_stats_counters *b)
{
    return a->queued == b->queued && a->started == b->started &&
        a->failed == b->failed && a->finished == b->finished &&
        a->retried == b->retried && a->terminal == b->terminal &&
        a->pending == b->pending && a->in_flight == b->in_flight &&
        a->run_samples == b->run_samples;
}

static int run(int mode)
{
    probe p;
    gptps *e = NULL;
    gptps_stats *s;
    gptps_config cfg;
    gptps_task_def d;
    gptps_handle h;
    gptps_stats_counters total, row;
    int bad = 0;
    memset(&p, 0, sizeof p); memset(&cfg, 0, sizeof cfg); memset(&d, 0, sizeof d);
    p.mode = mode; p.mu = gptps_mutex_create(); p.cv = gptps_cond_create();
    if (!p.mu || !p.cv) return 1;
    cfg.struct_size = sizeof cfg;
    cfg.mode = mode ? GPTPS_RUN_THREADED : GPTPS_RUN_MANUAL;
    cfg.limits.struct_size = sizeof cfg.limits; cfg.limits.max_concurrent_tasks = 1;
    if (gptps_open_ex(&cfg, &e) != GPTPS_OK) return 1;
    if (gptps_set_event_cb(e, primary, &p) != GPTPS_OK ||
        gptps_register_observer(e, acknowledge, &p) != GPTPS_OK) return 1;
    s = gptps_stats_install(e);
    if (!s) return 1;
    d.struct_size = sizeof d; d.name = "stats_retry"; d.run = task;
    d.exec = GPTPS_EXEC_INPROC; d.user_data = &p;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = 1;
    if (gptps_register_task(e, &d) != GPTPS_OK ||
        gptps_submit(e, d.name, NULL, 0, &h) != GPTPS_OK) return 1;
    if (!mode) {
        int i;
        for (i = 0; i < 4; ++i) {
            size_t ran;
            if (gptps_step(e, &ran) != GPTPS_OK) return 1;
        }
    }
    if (gptps_shutdown(e) != GPTPS_OK) return 1;
    /* Stats intentionally outlives the engine per the public add-on contract. */
    if (gptps_stats_total(s, &total) != GPTPS_OK ||
        gptps_stats_task(s, d.name, &row) != GPTPS_OK) return 1;
    if (p.gate_timeouts != (mode ? 1 : 0) || p.setup_timeouts || p.calls != 2 || !same(&total, &row) ||
        total.queued != 1 || total.started != 2 || total.failed != 1 ||
        total.finished != 1 || total.retried != 1 || total.terminal != 1) bad = 1;
    if (total.pending || total.in_flight || total.run_samples != 2) bad = 1;
    printf("mode=%d calls=%d terminal=%llu pending=%llu in_flight=%llu run_samples=%llu gate_timeouts=%d setup_timeouts=%d result=%s\n",
        mode, p.calls, (unsigned long long)total.terminal,
        (unsigned long long)total.pending, (unsigned long long)total.in_flight,
        (unsigned long long)total.run_samples, p.gate_timeouts, p.setup_timeouts, bad ? "FAIL" : "PASS");
    gptps_stats_close(s); gptps_cond_destroy(p.cv); gptps_mutex_destroy(p.mu);
    return bad;
}

static gptps_status batch_task(gptps_ctx *ctx, void *ud)
{
    int *calls = ud;
    (void)ctx;
    return ++*calls <= 300 ? GPTPS_E_TASK : GPTPS_OK;
}

/* MANUAL wave: more completions than the engine's 256-event pending buffer.
 * All first attempts run before the next wave, so a shared call counter is safe. */
static int batch(void)
{
    gptps_config cfg;
    gptps_task_def d;
    gptps *e = NULL;
    gptps_stats *s;
    gptps_stats_counters c;
    int calls = 0, i, bad;
    memset(&cfg, 0, sizeof cfg); memset(&d, 0, sizeof d);
    cfg.struct_size = sizeof cfg; cfg.mode = GPTPS_RUN_MANUAL;
    cfg.limits.struct_size = sizeof cfg.limits; cfg.limits.max_concurrent_tasks = 300;
    cfg.limits.max_memory_bytes = 300;
    if (gptps_open_ex(&cfg, &e) != GPTPS_OK) return 1;
    s = gptps_stats_install(e); if (!s) return 1;
    d.struct_size = sizeof d; d.name = "batch"; d.exec = GPTPS_EXEC_INPROC;
    d.run = batch_task; d.user_data = &calls;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = 1;
    d.default_policy.struct_size = sizeof d.default_policy; d.default_policy.max_retries = 1;
    if (gptps_register_task(e, &d) != GPTPS_OK) return 1;
    for (i = 0; i < 300; ++i) {
        gptps_handle h;
        if (gptps_submit(e, d.name, NULL, 0, &h) != GPTPS_OK) return 1;
    }
    for (i = 0; i < 5; ++i) {
        size_t ran;
        if (gptps_step(e, &ran) != GPTPS_OK) return 1;
    }
    if (gptps_shutdown(e) != GPTPS_OK || gptps_stats_total(s, &c) != GPTPS_OK) return 1;
    bad = calls != 600 || c.queued != 300 || c.retried != 300 || c.failed != 300 ||
        c.finished != 300 || c.terminal != 300 || c.pending || c.in_flight || c.run_samples != 600;
    printf("batch-300: calls=%d retries=%llu terminal=%llu pending=%llu in_flight=%llu result=%s\n",
        calls, (unsigned long long)c.retried, (unsigned long long)c.terminal,
        (unsigned long long)c.pending, (unsigned long long)c.in_flight, bad ? "FAIL" : "PASS");
    gptps_stats_close(s);
    return bad;
}

typedef struct {
    gptps *e;
    int calls, retries, terminals, cancelled, starts, order[8], overflow;
    int remove_on_retry, terminal_before_retry;
    gptps_status remove_status;
} manual_probe;

static gptps_status manual_task(gptps_ctx *ctx, void *ud)
{
    manual_probe *p = ud;
    (void)ctx;
    if (++p->calls != 1) return GPTPS_OK;
    /* Returning E_CANCELLED ends the item as a cancel does: its FAILED /
     * E_CANCELLED is terminal, so the engine must not retry it (mode 2). */
    return p->remove_on_retry == 2 ? GPTPS_E_CANCELLED : GPTPS_E_TASK;
}

static gptps_status low_task(gptps_ctx *ctx, void *ud)
{
    (void)ctx; (void)ud;
    return GPTPS_OK;
}

static void remove_retry(const gptps_event *ev, void *ud)
{
    manual_probe *p = ud;
    if (ev->kind == GPTPS_EV_RETRIED && p->remove_on_retry)
        p->remove_status = gptps_unregister_task(p->e, "hi", GPTPS_REMOVE_CANCEL);
}

/* Count events directly: stats is deliberately not involved in these cases.
 * A nested terminal event may reach this observer before the outer RETRIED. */
static void manual_observe(const gptps_event *ev, void *ud)
{
    manual_probe *p = ud;
    if (ev->kind == GPTPS_EV_STARTED) {
        if (p->starts < 8)
            p->order[p->starts++] = strcmp(ev->task_name, "hi") == 0 ? (int)ev->attempt : 0;
        else p->overflow = 1;
    }
    if (ev->kind == GPTPS_EV_RETRIED) {
        ++p->retries;
        p->terminal_before_retry = p->cancelled;
    }
    if (ev->kind == GPTPS_EV_FAILED && ev->status == GPTPS_E_CANCELLED) {
        ++p->cancelled; ++p->terminals;
    }
    if (ev->kind == GPTPS_EV_FINISHED || ev->kind == GPTPS_EV_DROPPED ||
        ev->kind == GPTPS_EV_DEAD_LETTERED) ++p->terminals;
}

static void manual_case(int remove_on_retry, unsigned backoff, unsigned slots)
{
    manual_probe p;
    gptps_config cfg;
    gptps_task_def d;
    gptps_handle h;
    int i;
    memset(&p, 0, sizeof p); memset(&cfg, 0, sizeof cfg); memset(&d, 0, sizeof d);
    p.remove_on_retry = remove_on_retry; p.remove_status = GPTPS_E_TASK;
    cfg.struct_size = sizeof cfg; cfg.mode = GPTPS_RUN_MANUAL;
    cfg.limits.struct_size = sizeof cfg.limits; cfg.limits.max_concurrent_tasks = slots;
    CHECK(gptps_open_ex(&cfg, &p.e) == GPTPS_OK);
    if (!p.e) return;
    CHECK(gptps_set_event_cb(p.e, remove_retry, &p) == GPTPS_OK);
    CHECK(gptps_register_observer(p.e, manual_observe, &p) == GPTPS_OK);
    d.struct_size = sizeof d; d.name = "hi"; d.run = manual_task;
    d.exec = GPTPS_EXEC_INPROC; d.user_data = &p;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = 1; d.default_policy.retry_backoff_seconds = backoff;
    CHECK(gptps_register_task(p.e, &d) == GPTPS_OK);
    CHECK(gptps_set_task_priority(p.e, "hi", 10) == GPTPS_OK);
    CHECK(gptps_submit(p.e, "hi", NULL, 0, &h) == GPTPS_OK);
    if (!remove_on_retry) {
        d.name = "lo"; d.run = low_task; d.default_policy.max_retries = 0;
        CHECK(gptps_register_task(p.e, &d) == GPTPS_OK);
        for (i = 0; i < 3; ++i) CHECK(gptps_submit(p.e, "lo", NULL, 0, &h) == GPTPS_OK);
    }
    for (i = 0; i < 8; ++i) {
        size_t ran;
        CHECK(gptps_step(p.e, &ran) == GPTPS_OK);
        if (i == 0 && remove_on_retry == 1) {
            CHECK(p.remove_status == GPTPS_OK);
            CHECK(p.terminals == 1);
            CHECK(!gptps_task_exists(p.e, "hi"));
        }
    }
    CHECK(gptps_shutdown(p.e) == GPTPS_OK);
    CHECK(!p.overflow);
    if (remove_on_retry == 2) {
        /* No RETRIED, so the removal never runs: the body's own FAILED /
         * E_CANCELLED closed the handle, once, and shutdown adds nothing. Before,
         * the engine retried it, reopening a handle observers had closed. */
        CHECK(p.retries == 0);
        CHECK(p.calls == 1); CHECK(p.starts == 1);
        CHECK(p.terminals == 1); CHECK(p.cancelled == 1);
        CHECK(p.remove_status == GPTPS_E_TASK);   /* untouched */
        return;
    }
    CHECK(p.retries == 1);
    if (remove_on_retry) {
        CHECK(p.calls == 1); CHECK(p.starts == 1);
        CHECK(p.terminals == 1); CHECK(p.cancelled == 1);
        CHECK(p.terminal_before_retry == 1);
    } else {
        /* One slot: the retry reclaims it ahead of every lo. Two: hi#1 and a lo
         * share the first step, then hi#2 still runs ahead of the next lo - which
         * only MANUAL mode's full stop in the parking pass guarantees. */
        const int one_slot[] = { 1, 2, 0, 0, 0 }, two_slots[] = { 1, 0, 2, 0, 0 };
        const int *expected = slots == 1 ? one_slot : two_slots;
        CHECK(p.calls == 2); CHECK(p.starts == 5);
        CHECK(p.terminals == 4); CHECK(p.cancelled == 0);
        CHECK(memcmp(p.order, expected, sizeof one_slot) == 0);
    }
    printf("manual remove=%d backoff=%u slots=%u starts=%d terminals=%d\n",
           remove_on_retry, backoff, slots, p.starts, p.terminals);
}

/* THREADED: the same removal from a RETRIED callback, on the dispatcher. The retry
 * is parked, not admitted, while its RETRIED is delivered, so there is nothing to
 * wait for and the removal completes - the old engine had already admitted a
 * zero-backoff retry by then, and the removal was refused with GPTPS_E_BUSY. */
static int threaded_removed;
static void remove_retry_threaded(const gptps_event *ev, void *ud)
{
    remove_retry(ev, ud);
    if (ev->kind == GPTPS_EV_RETRIED) __atomic_store_n(&threaded_removed, 1, __ATOMIC_SEQ_CST);
}

static void threaded_remove_case(void)
{
    manual_probe p;
    gptps_config cfg;
    gptps_task_def d;
    gptps_handle h;
    uint64_t t0;
    memset(&p, 0, sizeof p); memset(&cfg, 0, sizeof cfg); memset(&d, 0, sizeof d);
    p.remove_on_retry = 1; p.remove_status = GPTPS_E_TASK;
    cfg.struct_size = sizeof cfg; cfg.mode = GPTPS_RUN_THREADED;
    cfg.limits.struct_size = sizeof cfg.limits; cfg.limits.max_concurrent_tasks = 1;
    CHECK(gptps_open_ex(&cfg, &p.e) == GPTPS_OK);
    if (!p.e) return;
    CHECK(gptps_set_event_cb(p.e, remove_retry_threaded, &p) == GPTPS_OK);
    CHECK(gptps_register_observer(p.e, manual_observe, &p) == GPTPS_OK);
    d.struct_size = sizeof d; d.name = "hi"; d.run = manual_task;
    d.exec = GPTPS_EXEC_INPROC; d.user_data = &p;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = 1;              /* zero backoff */
    CHECK(gptps_register_task(p.e, &d) == GPTPS_OK);
    CHECK(gptps_submit(p.e, "hi", NULL, 0, &h) == GPTPS_OK);
    /* shutdown would refuse the removal (E_SHUTDOWN): let the callback run first */
    t0 = gptps_hal_monotonic_ms();
    while (!__atomic_load_n(&threaded_removed, __ATOMIC_SEQ_CST) &&
           gptps_hal_monotonic_ms() - t0 < 5000) { }
    CHECK(__atomic_load_n(&threaded_removed, __ATOMIC_SEQ_CST) == 1);
    CHECK(gptps_shutdown(p.e) == GPTPS_OK);   /* joins every thread: p is quiescent */
    CHECK(p.remove_status == GPTPS_OK);
    CHECK(p.calls == 1); CHECK(p.starts == 1); CHECK(p.retries == 1);
    CHECK(p.terminals == 1); CHECK(p.cancelled == 1);
    CHECK(p.terminal_before_retry == 1);
    printf("threaded remove: status=%d terminals=%d\n", (int)p.remove_status, p.terminals);
}

typedef struct {
    probe sync;
    gptps *e;
    int low_done, low_during_retry, retries;
    gptps_status submitted;
} admission_probe;

static gptps_status admission_low(gptps_ctx *ctx, void *ud)
{
    admission_probe *p = ud;
    (void)ctx;
    gptps_mutex_lock(p->sync.mu);
    p->low_done = 1;
    gptps_cond_broadcast(p->sync.cv);
    gptps_mutex_unlock(p->sync.mu);
    return GPTPS_OK;
}

static gptps_status admission_retry(gptps_ctx *ctx, void *ud)
{
    admission_probe *p = ud;
    int first;
    (void)ctx;
    gptps_mutex_lock(p->sync.mu);
    first = ++p->sync.calls == 1;
    gptps_mutex_unlock(p->sync.mu);
    if (first) {
        gptps_handle h;
        /* All slots (or the memory budget) are still occupied by this attempt.
         * The unrelated task cannot start before this attempt is accounted. */
        p->submitted = gptps_submit(p->e, "other", NULL, 0, &h);
        return GPTPS_E_TASK;
    }
    return GPTPS_OK;
}

static void admission_event(const gptps_event *ev, void *ud)
{
    admission_probe *p = ud;
    if (ev->kind != GPTPS_EV_RETRIED) return;
    gptps_mutex_lock(p->sync.mu);
    ++p->retries;
    /* Give independent work time to use a freed worker. This is a liveness
     * check, not a performance threshold; timeout also bounds broken engines. */
    p->sync.gate_timeouts += wait_for(&p->sync, &p->low_done, 500);
    p->low_during_retry = p->low_done;
    gptps_mutex_unlock(p->sync.mu);
}

static void admission_case_mem(int priority, unsigned slots, uint64_t mem, int expect_progress)
{
    admission_probe p;
    gptps_config cfg;
    gptps_task_def d;
    gptps_handle h;
    memset(&p, 0, sizeof p); memset(&cfg, 0, sizeof cfg); memset(&d, 0, sizeof d);
    p.submitted = GPTPS_E_TASK;
    p.sync.mu = gptps_mutex_create(); p.sync.cv = gptps_cond_create();
    CHECK(p.sync.mu && p.sync.cv);
    if (!p.sync.mu || !p.sync.cv) return;
    cfg.struct_size = sizeof cfg; cfg.mode = GPTPS_RUN_THREADED;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = slots; cfg.limits.max_memory_bytes = mem;
    CHECK(gptps_open_ex(&cfg, &p.e) == GPTPS_OK);
    if (!p.e) return;
    CHECK(gptps_set_event_cb(p.e, admission_event, &p) == GPTPS_OK);
    d.struct_size = sizeof d; d.exec = GPTPS_EXEC_INPROC; d.user_data = &p;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = 1;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.name = "other"; d.run = admission_low;
    CHECK(gptps_register_task(p.e, &d) == GPTPS_OK);
    d.name = "retry"; d.run = admission_retry; d.default_cost.mem_bytes = 2;
    d.default_policy.max_retries = 1;
    CHECK(gptps_register_task(p.e, &d) == GPTPS_OK);
    CHECK(gptps_set_task_priority(p.e, "retry", priority) == GPTPS_OK);
    CHECK(gptps_submit(p.e, "retry", NULL, 0, &h) == GPTPS_OK);
    /* Wait until the task has submitted its companion before shutting down;
     * shutdown would otherwise legitimately reject that internal submission. */
    gptps_mutex_lock(p.sync.mu);
    p.sync.setup_timeouts += wait_for(&p.sync, &p.low_done, 4000);
    gptps_mutex_unlock(p.sync.mu);
    CHECK(gptps_shutdown(p.e) == GPTPS_OK);
    CHECK(p.submitted == GPTPS_OK); CHECK(p.sync.setup_timeouts == 0);
    CHECK(p.sync.calls == 2); CHECK(p.retries == 1); CHECK(p.low_done == 1);
    CHECK(p.low_during_retry == expect_progress);
    CHECK(p.sync.gate_timeouts == !expect_progress);
    printf("admission priority=%d slots=%u mem=%llu independent_progress=%d expected=%d\n",
           priority, slots, (unsigned long long)mem, p.low_during_retry, expect_progress);
    gptps_cond_destroy(p.sync.cv); gptps_mutex_destroy(p.sync.mu);
}

/* The retry costs 2 bytes: a 2-byte budget leaves nothing beside it. */
static void admission_case(int priority, unsigned slots, int expect_progress)
{
    admission_case_mem(priority, slots, 2, expect_progress);
}

int main(void)
{
    int errors = run(0);
    errors += run(1); errors += run(2);
    errors += batch();
    CHECK(errors == 0);
    manual_case(1, 0, 1);
    manual_case(1, 1, 1);
    manual_case(2, 0, 1); /* self-returned E_CANCELLED ends the item: no retry */
    manual_case(2, 1, 1);
    manual_case(0, 0, 1);
    threaded_remove_case();
    manual_case(0, 0, 2);     /* spare slot: only MANUAL's full stop keeps hi#2 first */
    admission_case(0, 1, 1);  /* equal score, already queued: do not stall it */
    admission_case(-10, 1, 1); /* higher-score unrelated work goes first */
    admission_case(10, 1, 0); /* the only slot and the whole budget are the retry's */
    admission_case(10, 2, 0); /* spare slot, but retry needs the memory budget */
    admission_case_mem(10, 1, 1u << 20, 0); /* memory to spare: only the slot is held */
    /* Room for both: lower work fits BESIDE the held retry and must start. A correct
     * engine always passes this; a hold that stops all lower work usually fails it,
     * since the retry's own slot is freed in the same pass that parks it. */
    admission_case_mem(10, 2, 1u << 20, 1);
    return fails ? 1 : 0;
}
