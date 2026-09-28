/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_durable.c - durable-queue add-on.
 *   A) happy path: dq_submit'd tasks run, and the journal marks them complete
 *      (pending -> 0) once they finish.
 *   B) crash recovery: a forked child dq_submits work and _exit()s WITHOUT a
 *      clean shutdown (a real crash); the parent reopens the journal and
 *      recovers every persisted-but-uncompleted task, which then runs.
 *   C) cancellation: gptps_cancel stops one execution and leaves the durable
 *      intent for the next run; gptps_dq_cancel retracts the intent itself.
 *   D) shutdown abandonment: work the engine gives up on at teardown stays
 *      pending - not quarantined, not lost - and the next run recovers it; a
 *      body that itself returns GPTPS_E_SHUTDOWN is still judged by its policy
 *      (under requeue, quarantined: the documented residue).
 *
 * Phase A fully shuts down (joining all engine threads) before the fork, so the
 * fork happens from a single-threaded process.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "gptps.h"
#include "gptps_durable_queue.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#if !defined(_WIN32)               /* the crash-recovery phase forks (POSIX only) */
#  include <unistd.h>
#  include <sys/wait.h>
#  include <sys/stat.h>
#  define TEST_DURABLE_FORK 1
static void nap(void) { struct timespec ts = { 0, 1000000L }; nanosleep(&ts, NULL); }
#else
#  include <windows.h>
static void nap(void) { Sleep(1); }
#endif

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static int g_work_runs;
static int g_ran[8];    /* g_ran[b]: runs of a task_work whose payload byte is b */
static int g_block;     /* child blocks tasks so none complete before the "crash" */
static int g_release;

/* Wait, bounded at ~10s of wall time, for a counter another thread bumps. Timed by
 * the clock rather than by counting naps: Sleep(1) lasts a whole scheduler tick. */
static int wait_for(int *p, int want)
{
    time_t t0 = time(NULL);
    while (get(p) < want && time(NULL) - t0 < 10) nap();
    return get(p) >= want;
}

static gptps_status task_work(gptps_ctx *ctx, void *ud)
{
    uint64_t start = gptps_now_ms(ctx);
    size_t n = 0;
    const unsigned char *b = (const unsigned char *)gptps_payload(ctx, &n);
    (void)ud;
    if (b && n == 1 && b[0] < 8) inc(&g_ran[b[0]]);
    inc(&g_work_runs);
    while (get(&g_block) && !get(&g_release) && !gptps_is_cancelled(ctx))
        if (gptps_now_ms(ctx) - start > 20000) break;  /* safety, inside ctest's 30s */
    return GPTPS_OK;
}

static void reg_work(gptps *e)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "work"; d.run = task_work; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = 1024;
    d.default_policy.struct_size = sizeof d.default_policy;
    gptps_register_task(e, &d);
}

static gptps *open_engine(unsigned conc)
{
    gptps_config cfg; gptps *e = NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = conc; cfg.limits.max_memory_bytes = 64u << 20;
    gptps_open_ex(&cfg, &e);
    return e;
}

#define JOURNAL_A "dq_test_a.journal"
#define JOURNAL_B "dq_test_b.journal"
#define NREC 5

static void test_happy(void)
{
    gptps *e;
    gptps_dq *dq;
    gptps_handle h;
    int i;

    remove(JOURNAL_A);
    __atomic_store_n(&g_work_runs, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_block, 0, __ATOMIC_SEQ_CST);

    e = open_engine(2); CHECK(e != NULL);
    if (!e) return;
    reg_work(e);
    dq = gptps_dq_open(e, JOURNAL_A); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }

    CHECK(gptps_dq_pending(dq) == 0);
    for (i = 0; i < NREC; ++i) {
        unsigned char b = (unsigned char)i;
        CHECK(gptps_dq_submit(dq, "work", &b, 1, &h) == GPTPS_OK);
    }

    gptps_shutdown(e);                       /* drains; observer marks each done */
    CHECK(get(&g_work_runs) == NREC);
    CHECK(gptps_dq_pending(dq) == 0);         /* all completions journaled */
    gptps_dq_close(dq);                       /* after shutdown */
    remove(JOURNAL_A);
}

/* dead-letter quarantine: a task that exhausts retries is RETAINED (its poison
 * payload survives), not silently dropped; drain returns it. */
static char          g_drained_name[64];
static unsigned char g_drained_payload;
static int           g_drained_n;
static void quarantine_cb(const char *name, const void *payload, size_t len, void *ud)
{
    (void)ud;
    snprintf(g_drained_name, sizeof g_drained_name, "%s", name ? name : "");
    if (payload && len == 1) g_drained_payload = *(const unsigned char *)payload;
    inc(&g_drained_n);
}
static gptps_status task_poison(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_E_TASK; }

static void test_quarantine(void)
{
    gptps *e; gptps_dq *dq; gptps_handle h; gptps_task_def d;
    remove(JOURNAL_A);
    g_drained_n = 0; g_drained_payload = 0; g_drained_name[0] = 0;

    e = open_engine(2); CHECK(e != NULL); if (!e) return;
    memset(&d, 0, sizeof d); d.struct_size = sizeof d; d.name = "poison";
    d.run = task_poison; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;  /* dead_letter, 0 retries */
    gptps_register_task(e, &d);

    dq = gptps_dq_open(e, JOURNAL_A); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }
    { unsigned char b = 0xAB; CHECK(gptps_dq_submit(dq, "poison", &b, 1, &h) == GPTPS_OK); }

    gptps_shutdown(e);   /* fails -> dead-lettered -> quarantined by the observer */
    CHECK(gptps_dq_pending(dq) == 0);       /* not pending */
    CHECK(gptps_dq_quarantined(dq) == 1);   /* retained, not dropped */

    CHECK(gptps_dq_drain_quarantine(dq, quarantine_cb, NULL) == 1);
    CHECK(get(&g_drained_n) == 1);
    CHECK(strcmp(g_drained_name, "poison") == 0);
    CHECK(g_drained_payload == 0xAB);       /* the poison payload survived */
    CHECK(gptps_dq_quarantined(dq) == 0);   /* cleared after drain */

    gptps_dq_close(dq);
    remove(JOURNAL_A);
}

static void reset_ran(void) { int i; for (i = 0; i < 8; ++i) __atomic_store_n(&g_ran[i], 0, __ATOMIC_SEQ_CST); }

/* C) cancellation. A holds the only worker, so B, C and D stay queued and each
 * cancel below reaches a queued item, whose FAILED / GPTPS_E_CANCELLED the engine
 * delivers on the cancelling thread before gptps_cancel returns. So g_ch and
 * g_pending_seen are touched only on the main thread, and need no atomics - which
 * matters: the MSVC shim (tests/test_atomic_compat.h) is 32-bit only.
 *
 * cancel_obs also calls back into the queue, as the header allows for any event but
 * QUEUED. That holds gptps_dq_cancel to its order: retract, release the lock, THEN
 * tell the engine. Told first, under the lock, this call would deadlock on POSIX,
 * and on Windows (a recursive lock) it would see the record still pending. */
static gptps_handle g_ch[4];
static int          g_cancel_ev[4];
static gptps_dq    *g_cdq;              /* non-NULL while cancel_obs may re-enter */
static size_t       g_pending_seen[4];
static void cancel_obs(const gptps_event *ev, void *ud)
{
    int i;
    (void)ud;
    if (ev->kind != GPTPS_EV_FAILED || ev->status != GPTPS_E_CANCELLED) return;
    for (i = 0; i < 4; ++i) {
        if (ev->handle != g_ch[i]) continue;
        inc(&g_cancel_ev[i]);
        if (g_cdq) g_pending_seen[i] = gptps_dq_pending(g_cdq);
    }
}

static void test_cancel(void)
{
    gptps *e; gptps_dq *dq; gptps_handle h[4] = { 0, 0, 0, 0 }; int i;
    remove(JOURNAL_A);
    __atomic_store_n(&g_work_runs, 0, __ATOMIC_SEQ_CST);
    reset_ran();
    __atomic_store_n(&g_block, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_release, 0, __ATOMIC_SEQ_CST);
    for (i = 0; i < 4; ++i) { g_ch[i] = 0; g_cancel_ev[i] = 0; g_pending_seen[i] = 0; }
    g_cdq = NULL;

    /* ---- run 1 ---- */
    e = open_engine(1); CHECK(e != NULL); if (!e) return;
    reg_work(e);
    CHECK(gptps_register_observer(e, cancel_obs, NULL) == GPTPS_OK);
    dq = gptps_dq_open(e, JOURNAL_A); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }
    g_cdq = dq;
    for (i = 0; i < 4; ++i) {
        unsigned char b = (unsigned char)i;
        CHECK(gptps_dq_submit(dq, "work", &b, 1, &h[i]) == GPTPS_OK);
        g_ch[i] = h[i];
    }
    CHECK(wait_for(&g_work_runs, 1));                   /* A holds the worker */

    /* B: the engine cancel stops the execution; the durable intent stays */
    CHECK(gptps_cancel(e, h[1]) == GPTPS_OK);
    CHECK(get(&g_cancel_ev[1]) == 1);
    CHECK(gptps_dq_pending(dq) == 4);

    /* C: gptps_dq_cancel retracts the intent AND cancels the execution */
    CHECK(gptps_dq_cancel(dq, h[2]) == GPTPS_OK);
    CHECK(get(&g_cancel_ev[2]) == 1);
    CHECK(g_pending_seen[2] == 3);              /* the callback saw it retracted */
    CHECK(gptps_dq_pending(dq) == 3);
    CHECK(gptps_dq_cancel(dq, h[2]) == GPTPS_E_NOTFOUND);   /* retracted already */

    /* D: stopped by the engine first, retracted after - OK although the engine
     * has nothing left to cancel, and still only the one terminal event */
    CHECK(gptps_cancel(e, h[3]) == GPTPS_OK);
    CHECK(gptps_dq_cancel(dq, h[3]) == GPTPS_OK);
    CHECK(get(&g_cancel_ev[3]) == 1);
    CHECK(gptps_dq_pending(dq) == 2);                   /* A running, B's intent */

    CHECK(gptps_dq_cancel(dq, 0) == GPTPS_E_INVAL);
    CHECK(gptps_dq_cancel(NULL, h[0]) == GPTPS_E_INVAL);
    CHECK(gptps_dq_cancel(dq, h[3] + 1000) == GPTPS_E_NOTFOUND);

    __atomic_store_n(&g_release, 1, __ATOMIC_SEQ_CST);
    { time_t t0 = time(NULL); while (gptps_dq_pending(dq) != 1 && time(NULL) - t0 < 10) nap(); }
    CHECK(gptps_dq_pending(dq) == 1);                   /* A finished: B is left */
    CHECK(gptps_dq_cancel(dq, h[0]) == GPTPS_E_NOTFOUND);   /* finished: nothing to retract */
    g_cdq = NULL;
    gptps_shutdown(e);
    CHECK(get(&g_ran[0]) == 1);                         /* only A ran */
    CHECK(get(&g_ran[1]) + get(&g_ran[2]) + get(&g_ran[3]) == 0);
    CHECK(get(&g_cancel_ev[0]) == 0);
    CHECK(gptps_dq_pending(dq) == 1);
    gptps_dq_close(dq);

    /* ---- run 2: B's intent comes back; C and D stay retracted ---- */
    __atomic_store_n(&g_block, 0, __ATOMIC_SEQ_CST);
    reset_ran();
    e = open_engine(2); CHECK(e != NULL); if (!e) return;
    reg_work(e);
    dq = gptps_dq_open(e, JOURNAL_A); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }
    CHECK(gptps_dq_pending(dq) == 1);
    CHECK(gptps_dq_recover(dq) == 1);
    gptps_shutdown(e);
    CHECK(get(&g_ran[1]) == 1);                         /* B, and only B */
    CHECK(get(&g_ran[0]) + get(&g_ran[2]) + get(&g_ran[3]) == 0);
    CHECK(gptps_dq_pending(dq) == 0);
    gptps_dq_close(dq);
    remove(JOURNAL_A);
}

/* D) shutdown abandonment. "flaky" fails once and parks a retry behind a 300s
 * backoff; "spin" runs until cancelled. A 100ms shutdown grace then ends both:
 * the parked retry by its policy with status GPTPS_E_SHUTDOWN, the spinning body
 * with FAILED / GPTPS_E_CANCELLED. Neither is a verdict on the work, so both stay
 * pending and run 2 recovers them. The dead-lettered retry used to be quarantined
 * as poison, and the dropped one marked done - lost with a retry still owed.
 * Run once more with flaky's attempt returning GPTPS_E_SHUTDOWN itself: that
 * attempt's verdict is followed by a RETRIED, so the parked retry is still
 * teardown's to end. */
static int          g_ok;               /* run 2: every body succeeds at once */
static gptps_status g_flaky_fail;       /* what flaky's run-1 attempt returns */
static int g_flaky_runs, g_spin_runs, g_retried, g_shutdown_ev;
static void shutdown_obs(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_RETRIED) inc(&g_retried);
    if ((ev->kind == GPTPS_EV_DEAD_LETTERED || ev->kind == GPTPS_EV_DROPPED) &&
        ev->status == GPTPS_E_SHUTDOWN) inc(&g_shutdown_ev);
}
static gptps_status task_flaky(gptps_ctx *ctx, void *ud)
{
    (void)ctx; (void)ud;
    inc(&g_flaky_runs);
    return get(&g_ok) ? GPTPS_OK : g_flaky_fail;
}
static gptps_status task_spin(gptps_ctx *ctx, void *ud)
{
    uint64_t start = gptps_now_ms(ctx);
    (void)ud;
    inc(&g_spin_runs);
    if (get(&g_ok)) return GPTPS_OK;
    while (!gptps_is_cancelled(ctx) && gptps_now_ms(ctx) - start < 10000) nap();
    return GPTPS_E_CANCELLED;
}
static void reg_abandon(gptps *e, gptps_on_failure on_failure)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.name = "flaky"; d.run = task_flaky;
    d.default_policy.max_retries = 1; d.default_policy.retry_backoff_seconds = 300;
    d.default_policy.on_failure = on_failure;
    gptps_register_task(e, &d);
    d.name = "spin"; d.run = task_spin;
    d.default_policy.max_retries = 0; d.default_policy.retry_backoff_seconds = 0;
    gptps_register_task(e, &d);
}

static void test_shutdown_abandon(gptps_on_failure on_failure, gptps_status fail)
{
    gptps *e; gptps_dq *dq; gptps_handle h;
    remove(JOURNAL_A);
    g_flaky_fail = fail;                /* before any engine thread exists */
    __atomic_store_n(&g_ok, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_flaky_runs, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_spin_runs, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_retried, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_shutdown_ev, 0, __ATOMIC_SEQ_CST);

    /* ---- run 1 ---- */
    e = open_engine(2); CHECK(e != NULL); if (!e) return;
    reg_abandon(e, on_failure);
    CHECK(gptps_register_observer(e, shutdown_obs, NULL) == GPTPS_OK);
    CHECK(gptps_settings_set(e, "limits.shutdown_grace_ms", "100") == GPTPS_OK);
    dq = gptps_dq_open(e, JOURNAL_A); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }
    CHECK(gptps_dq_submit(dq, "flaky", "f", 1, &h) == GPTPS_OK);
    CHECK(gptps_dq_submit(dq, "spin", "s", 1, &h) == GPTPS_OK);
    CHECK(wait_for(&g_retried, 1));                     /* flaky's retry is parked */
    CHECK(wait_for(&g_spin_runs, 1));                   /* spin is running */
    gptps_shutdown(e);
    CHECK(get(&g_shutdown_ev) == 1);                    /* the path under test ran */
    CHECK(get(&g_flaky_runs) == 1);
    CHECK(gptps_dq_quarantined(dq) == 0);               /* not filed as poison */
    CHECK(gptps_dq_pending(dq) == 2);                   /* not lost */
    gptps_dq_close(dq);

    /* ---- run 2 ---- */
    __atomic_store_n(&g_ok, 1, __ATOMIC_SEQ_CST);
    e = open_engine(2); CHECK(e != NULL); if (!e) return;
    reg_abandon(e, on_failure);
    dq = gptps_dq_open(e, JOURNAL_A); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }
    CHECK(gptps_dq_pending(dq) == 2);
    CHECK(gptps_dq_quarantined(dq) == 0);
    CHECK(gptps_dq_recover(dq) == 2);
    gptps_shutdown(e);
    CHECK(get(&g_flaky_runs) == 2);
    CHECK(get(&g_spin_runs) == 2);
    CHECK(gptps_dq_pending(dq) == 0);
    gptps_dq_close(dq);
    remove(JOURNAL_A);
}

/* The drain never schedules a requeue item another cycle; one whose retries run
 * out there is dead-lettered - teardown's doing, so the engine reports
 * GPTPS_E_SHUTDOWN and the record stays pending. With the attempt's own status on
 * that event it was quarantined: work whose policy was to keep trying, filed as
 * poison. Run again with the body itself returning GPTPS_E_SHUTDOWN: its FAILED
 * then looks exactly like a dead_letter item's own verdict, and the observer
 * cannot see the policy, so the record is quarantined - the documented residue,
 * pinned here so the header cannot drift from it. Retained, not lost. */
static void reg_requeue(gptps *e)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.exec = GPTPS_EXEC_INPROC; d.name = "flaky"; d.run = task_flaky;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.on_failure = GPTPS_ON_FAILURE_REQUEUE;  /* no retries, no backoff */
    gptps_register_task(e, &d);
}
static void test_requeue_drain(gptps_status fail)
{
    gptps *e; gptps_dq *dq; gptps_handle h;
    remove(JOURNAL_A);
    g_flaky_fail = fail;                /* before any engine thread exists */
    __atomic_store_n(&g_ok, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_flaky_runs, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_shutdown_ev, 0, __ATOMIC_SEQ_CST);

    e = open_engine(1); CHECK(e != NULL); if (!e) return;
    reg_requeue(e);
    CHECK(gptps_register_observer(e, shutdown_obs, NULL) == GPTPS_OK);
    dq = gptps_dq_open(e, JOURNAL_A); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }
    CHECK(gptps_dq_submit(dq, "flaky", "f", 1, &h) == GPTPS_OK);
    CHECK(wait_for(&g_flaky_runs, 2));                  /* really cycling */
    gptps_shutdown(e);
    CHECK(get(&g_shutdown_ev) == 1);                    /* DEAD_LETTERED, E_SHUTDOWN */
    if (fail == GPTPS_E_SHUTDOWN) {
        CHECK(gptps_dq_quarantined(dq) == 1);           /* the residue: retained */
        CHECK(gptps_dq_pending(dq) == 0);
        gptps_dq_close(dq);
        remove(JOURNAL_A);
        return;
    }
    CHECK(gptps_dq_quarantined(dq) == 0);
    CHECK(gptps_dq_pending(dq) == 1);
    gptps_dq_close(dq);

    __atomic_store_n(&g_ok, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_flaky_runs, 0, __ATOMIC_SEQ_CST);
    e = open_engine(1); CHECK(e != NULL); if (!e) return;
    reg_requeue(e);
    dq = gptps_dq_open(e, JOURNAL_A); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }
    CHECK(gptps_dq_recover(dq) == 1);
    gptps_shutdown(e);
    CHECK(get(&g_flaky_runs) == 1);
    CHECK(gptps_dq_pending(dq) == 0);
    gptps_dq_close(dq);
    remove(JOURNAL_A);
}

/* The engine does not reserve GPTPS_E_SHUTDOWN: a body that returns it on a live
 * engine is dead-lettered or dropped with it. That is the task's own verdict, and
 * the queue judges it by the policy - quarantined, or closed - and does not run it
 * again on the next open. A filter on the status alone would keep it pending and
 * re-run it on every restart. */
static int g_says_runs;
static gptps_status task_says_shutdown(gptps_ctx *ctx, void *ud)
{
    (void)ctx; (void)ud;
    inc(&g_says_runs);
    return GPTPS_E_SHUTDOWN;
}
static void reg_says(gptps *e, gptps_on_failure on_failure)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.exec = GPTPS_EXEC_INPROC; d.name = "says"; d.run = task_says_shutdown;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.on_failure = on_failure;         /* max_retries = 0 */
    gptps_register_task(e, &d);
}
static void test_body_shutdown(gptps_on_failure on_failure)
{
    gptps *e; gptps_dq *dq; gptps_handle h;
    size_t want_q = (on_failure == GPTPS_ON_FAILURE_DEAD_LETTER) ? 1 : 0;
    remove(JOURNAL_A);
    __atomic_store_n(&g_says_runs, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_shutdown_ev, 0, __ATOMIC_SEQ_CST);

    e = open_engine(1); CHECK(e != NULL); if (!e) return;
    reg_says(e, on_failure);
    CHECK(gptps_register_observer(e, shutdown_obs, NULL) == GPTPS_OK);
    dq = gptps_dq_open(e, JOURNAL_A); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }
    CHECK(gptps_dq_submit(dq, "says", "x", 1, &h) == GPTPS_OK);
    { time_t t0 = time(NULL); while (gptps_dq_pending(dq) != 0 && time(NULL) - t0 < 10) nap(); }
    CHECK(gptps_dq_pending(dq) == 0);                   /* judged while the engine runs */
    CHECK(gptps_dq_quarantined(dq) == want_q);
    gptps_shutdown(e);
    CHECK(get(&g_shutdown_ev) == 1);                    /* its terminal event carried it */
    gptps_dq_close(dq);

    e = open_engine(1); CHECK(e != NULL); if (!e) return;
    reg_says(e, on_failure);
    dq = gptps_dq_open(e, JOURNAL_A); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }
    CHECK(gptps_dq_pending(dq) == 0);
    CHECK(gptps_dq_quarantined(dq) == want_q);
    CHECK(gptps_dq_recover(dq) == 0);
    gptps_shutdown(e);
    CHECK(get(&g_says_runs) == 1);                      /* never again */
    gptps_dq_close(dq);
    remove(JOURNAL_A);
}

/* ...and in MANUAL mode, work still queued when the host shuts down without
 * pumping gets FAILED / GPTPS_E_CANCELLED from gptps_shutdown, and stays pending.
 * A gptps_dq_cancel made from that event's callback - the engine is tearing down -
 * still retracts its record, but returns GPTPS_E_SHUTDOWN: the engine stopped
 * nothing for it. Everything here runs on the one thread, so it is exact. */
static gptps_dq    *g_mdq;
static gptps_handle g_mh[2];
static gptps_status g_mcancel;
static int          g_mcalls;
static void manual_obs(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind != GPTPS_EV_FAILED || ev->status != GPTPS_E_CANCELLED) return;
    if (ev->handle == g_mh[0] && g_mdq) { g_mcancel = gptps_dq_cancel(g_mdq, g_mh[1]); ++g_mcalls; }
}
static void test_shutdown_manual(void)
{
    gptps_config cfg; gptps *e = NULL; gptps_dq *dq;
    unsigned char b0 = 5, b1 = 6;
    remove(JOURNAL_A);
    __atomic_store_n(&g_block, 0, __ATOMIC_SEQ_CST);
    reset_ran();
    g_mdq = NULL; g_mcancel = GPTPS_OK; g_mcalls = 0;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1; cfg.limits.max_memory_bytes = 64u << 20;
    cfg.mode = GPTPS_RUN_MANUAL;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK); if (!e) return;
    reg_work(e);
    CHECK(gptps_register_observer(e, manual_obs, NULL) == GPTPS_OK);
    dq = gptps_dq_open(e, JOURNAL_A); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }
    CHECK(gptps_dq_submit(dq, "work", &b0, 1, &g_mh[0]) == GPTPS_OK);
    CHECK(gptps_dq_submit(dq, "work", &b1, 1, &g_mh[1]) == GPTPS_OK);
    g_mdq = dq;
    gptps_shutdown(e);                                  /* never stepped */
    g_mdq = NULL;
    CHECK(g_mcalls == 1);
    CHECK(g_mcancel == GPTPS_E_SHUTDOWN);               /* retracted, not stopped */
    CHECK(get(&g_ran[5]) + get(&g_ran[6]) == 0);
    CHECK(gptps_dq_pending(dq) == 1);                   /* the first; the second is gone */
    gptps_dq_close(dq);

    e = open_engine(1); CHECK(e != NULL); if (!e) return;
    reg_work(e);
    dq = gptps_dq_open(e, JOURNAL_A); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }
    CHECK(gptps_dq_recover(dq) == 1);
    gptps_shutdown(e);
    CHECK(get(&g_ran[5]) == 1);
    CHECK(get(&g_ran[6]) == 0);
    CHECK(gptps_dq_pending(dq) == 0);
    gptps_dq_close(dq);
    remove(JOURNAL_A);
}

#if defined(TEST_DURABLE_FORK)
/* gptps_dq_cancel's failure path: "if it cannot be made durable the call returns
 * GPTPS_E_IO, leaving the record open and the execution alone". The journal's
 * descriptor is swapped for a pipe,
 * on which fwrite and fflush succeed but fsync fails, so the marker cannot be made
 * durable: the record must stay open and the execution must be left alone. The
 * same window holds gptps_dq_submit to its own promise not to enqueue. POSIX only
 * (dup2), and skipped where fsync on a pipe does not fail. */
static int          g_io_ev;
static gptps_handle g_io_h;
static void io_obs(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_FAILED && ev->status == GPTPS_E_CANCELLED && ev->handle == g_io_h)
        inc(&g_io_ev);
}
static int find_journal_fd(const char *path)
{
    struct stat js, fs; int fd, found = -1, n = 0;
    if (stat(path, &js) != 0) return -1;
    for (fd = 3; fd < 1024; ++fd)
        if (fstat(fd, &fs) == 0 && fs.st_dev == js.st_dev && fs.st_ino == js.st_ino) { found = fd; ++n; }
    return n == 1 ? found : -1;
}
static void test_cancel_io(void)
{
    gptps *e; gptps_dq *dq; gptps_handle ha, hb; int jfd, saved, pfd[2];
    unsigned char a = 6, b = 7;
    remove(JOURNAL_A);
    reset_ran();
    __atomic_store_n(&g_work_runs, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_block, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_release, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_io_ev, 0, __ATOMIC_SEQ_CST);
    g_io_h = 0;

    e = open_engine(1); CHECK(e != NULL); if (!e) return;
    reg_work(e);
    CHECK(gptps_register_observer(e, io_obs, NULL) == GPTPS_OK);
    dq = gptps_dq_open(e, JOURNAL_A); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }
    CHECK(gptps_dq_submit(dq, "work", &a, 1, &ha) == GPTPS_OK);
    CHECK(gptps_dq_submit(dq, "work", &b, 1, &hb) == GPTPS_OK);
    g_io_h = hb;
    CHECK(wait_for(&g_work_runs, 1));                   /* A holds the worker; B is queued */
    jfd = find_journal_fd(JOURNAL_A);
    CHECK(jfd >= 0);
    if (jfd >= 0 && pipe(pfd) == 0) {
        if (fsync(pfd[1]) != 0) {
            saved = dup(jfd);
            CHECK(saved >= 0 && dup2(pfd[1], jfd) == jfd);
            CHECK(gptps_dq_cancel(dq, hb) == GPTPS_E_IO);
            CHECK(gptps_dq_pending(dq) == 2);           /* nothing retracted... */
            CHECK(get(&g_io_ev) == 0);                  /* ...and nothing cancelled */
            {
                gptps_handle hx = 0; unsigned char x = 5;
                CHECK(gptps_dq_submit(dq, "work", &x, 1, &hx) == GPTPS_E_IO);
                CHECK(hx == 0 && gptps_dq_pending(dq) == 2);
            }
            CHECK(dup2(saved, jfd) == jfd);
            close(saved);
            CHECK(gptps_dq_cancel(dq, hb) == GPTPS_OK); /* durable again: it retracts */
            CHECK(get(&g_io_ev) == 1);
            CHECK(gptps_dq_pending(dq) == 1);
        } else {
            printf("SKIP test_cancel_io: fsync on a pipe succeeds here\n");
        }
        close(pfd[0]); close(pfd[1]);
    }
    __atomic_store_n(&g_release, 1, __ATOMIC_SEQ_CST);
    gptps_shutdown(e);
    CHECK(get(&g_ran[6]) == 1);
    CHECK(get(&g_ran[5]) + get(&g_ran[7]) == 0);
    gptps_dq_close(dq);

    e = open_engine(1); CHECK(e != NULL); if (!e) return;
    reg_work(e);
    dq = gptps_dq_open(e, JOURNAL_A); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }
    CHECK(gptps_dq_pending(dq) == 0);                   /* the retraction reached the file */
    gptps_shutdown(e);
    gptps_dq_close(dq);
    remove(JOURNAL_A);
}

static void test_recovery(void)
{
    pid_t pid;
    int wst = 0;

    remove(JOURNAL_B);

    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        /* ---- CHILD: persist work, then "crash" (no shutdown) ---- */
        gptps *e; gptps_dq *dq; gptps_handle h; int i;
        __atomic_store_n(&g_block, 1, __ATOMIC_SEQ_CST);   /* block so nothing completes */
        __atomic_store_n(&g_release, 0, __ATOMIC_SEQ_CST);
        e = open_engine(1);                                /* one worker: rest queue */
        if (!e) _exit(2);
        reg_work(e);
        dq = gptps_dq_open(e, JOURNAL_B);
        if (!dq) _exit(3);
        for (i = 0; i < NREC; ++i) { unsigned char b = (unsigned char)i; gptps_dq_submit(dq, "work", &b, 1, &h); }
        _exit(0);   /* abrupt: PENDING records are fsync'd, no DONE written */
    }

    /* ---- PARENT: reopen the journal and recover ---- */
    waitpid(pid, &wst, 0);
    CHECK(WIFEXITED(wst) && WEXITSTATUS(wst) == 0);

    /* Simulate a write torn by the crash: a record magic followed by a truncated
     * header. Replay must ignore this tail and keep the NREC valid records. */
    {
        FILE *j = fopen(JOURNAL_B, "ab");
        if (j) {
            unsigned char torn[8] = { 0x31, 0x52, 0x51, 0x44, 0xAA, 0xBB, 0xCC, 0xDD };
            fwrite(torn, 1, sizeof torn, j);
            fclose(j);
        }
    }
    {
        gptps *e; gptps_dq *dq; size_t recovered;
        __atomic_store_n(&g_block, 0, __ATOMIC_SEQ_CST);   /* parent: run to completion */
        __atomic_store_n(&g_work_runs, 0, __ATOMIC_SEQ_CST);
        e = open_engine(2); CHECK(e != NULL);
        if (!e) return;
        reg_work(e);
        dq = gptps_dq_open(e, JOURNAL_B); CHECK(dq != NULL);
        if (!dq) { gptps_shutdown(e); return; }

        CHECK(gptps_dq_pending(dq) == NREC);   /* the child's survivors */
        recovered = gptps_dq_recover(dq);
        CHECK(recovered == NREC);

        gptps_shutdown(e);
        CHECK(get(&g_work_runs) == NREC);      /* every survivor ran */
        CHECK(gptps_dq_pending(dq) == 0);
        gptps_dq_close(dq);
    }
    remove(JOURNAL_B);
}
#endif /* TEST_DURABLE_FORK */

int main(void)
{
    test_happy();      /* portable: dq_open/submit/observer/pending/fsync/close */
    test_quarantine(); /* portable: dead-letter quarantine + drain */
    test_cancel();     /* portable: engine cancel keeps the intent, dq cancel retracts it */
    test_shutdown_abandon(GPTPS_ON_FAILURE_DEAD_LETTER, GPTPS_E_TASK);
    test_shutdown_abandon(GPTPS_ON_FAILURE_DROP, GPTPS_E_TASK);
    test_shutdown_abandon(GPTPS_ON_FAILURE_DEAD_LETTER, GPTPS_E_SHUTDOWN);
    test_requeue_drain(GPTPS_E_TASK);
    test_requeue_drain(GPTPS_E_SHUTDOWN);
    test_body_shutdown(GPTPS_ON_FAILURE_DEAD_LETTER);
    test_body_shutdown(GPTPS_ON_FAILURE_DROP);
    test_shutdown_manual();
#if defined(TEST_DURABLE_FORK)
    test_cancel_io();  /* POSIX: the E_IO path, by swapping the journal's fd */
    test_recovery();   /* crash-recovery via fork (POSIX) */
#endif
    if (fails) { printf("%d durable-queue check(s) FAILED\n", fails); return 1; }
    printf("all durable-queue checks passed\n");
    return 0;
}
