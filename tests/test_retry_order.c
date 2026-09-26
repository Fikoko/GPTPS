/* SPDX-License-Identifier: MIT */
/* Retry notifications must reach observers before the next attempt starts.
 * Exercise a bounded slow callback and real stats, plus pending-buffer overflow.
 * The gate amplifies the scheduling window; it does not measure race frequency. */
#include "gptps_stats.h"
#include "gptps_hal.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    gptps_mutex *mu;
    gptps_cond *cv;
    int mode, queued, started2, finished2, retried, calls, timeouts;
} probe;

/* Caller holds mu. Condition waits release it; no engine lock is held here. */
static void wait_for(probe *p, int *flag, uint64_t timeout_ms)
{
    uint64_t end = gptps_hal_monotonic_ms() + timeout_ms;
    while (!*flag) {
        uint64_t now = gptps_hal_monotonic_ms();
        if (now >= end) { ++p->timeouts; break; }
        gptps_cond_timedwait(p->cv, p->mu, end - now);
    }
}

static void primary(const gptps_event *ev, void *ud)
{
    probe *p = ud;
    if (ev->kind != GPTPS_EV_RETRIED || !p->mode) return;
    gptps_mutex_lock(p->mu);
    wait_for(p, p->mode == 1 ? &p->started2 : &p->finished2, 50);
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
    if (n == 1) wait_for(p, &p->queued, 2000);
    if (n == 2 && p->mode == 1) wait_for(p, &p->retried, 2000);
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
    if (p.timeouts != (mode ? 1 : 0) || p.calls != 2 || !same(&total, &row) ||
        total.queued != 1 || total.started != 2 || total.failed != 1 ||
        total.finished != 1 || total.retried != 1 || total.terminal != 1) bad = 1;
    if (total.pending || total.in_flight || total.run_samples != 2) bad = 1;
    printf("mode=%d calls=%d terminal=%llu pending=%llu in_flight=%llu run_samples=%llu timeouts=%d result=%s\n",
        mode, p.calls, (unsigned long long)total.terminal,
        (unsigned long long)total.pending, (unsigned long long)total.in_flight,
        (unsigned long long)total.run_samples, p.timeouts, bad ? "FAIL" : "PASS");
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

int main(void)
{
    int errors = run(0);
    errors += run(1); errors += run(2);
    errors += batch();
    return errors ? 1 : 0;
}
