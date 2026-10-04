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
 *      body that itself returns GPTPS_E_SHUTDOWN is still judged by its policy,
 *      and one that returns GPTPS_E_CANCELLED closes its record.
 *   E) recovery under backpressure: gptps_dq_recover can be called again,
 *      re-submitting what an intake of two refused.
 *   F) crash loops: a body that kills its own process is quarantined after three
 *      deaths, and the records that happened to run beside it are not; a clean
 *      shutdown, however often, counts as no death at all.
 *   G) a damaged journal: damage in the middle costs only the damaged bytes, is
 *      reported, and the original is preserved; a torn tail is dropped silently.
 *   H) journaling does not stall the engine, and every acknowledged submit survives
 *      a SIGKILL in the middle of concurrent submits and compactions.
 *   I) a record no writer makes - numbered past 2^63, or a 'P' at or below one
 *      before it - is damage, and the queue's numbers stop at 2^63-1 (GPTPS_E_FULL).
 *
 * Every phase fully shuts down (joining all engine threads) before a fork, so the
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
#  include <poll.h>
#  include <signal.h>
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
 * Run once more with flaky's attempt returning GPTPS_E_SHUTDOWN itself: the retry
 * is parked all the same, and the grace ends it with GPTPS_EV_FLAG_SHUTDOWN, so it
 * is still teardown's. */
static int          g_ok;               /* run 2: every body succeeds at once */
static gptps_status g_flaky_fail;       /* what flaky's run-1 attempt returns */
static int g_flaky_runs, g_spin_runs, g_retried, g_shutdown_ev, g_flagged;
static void shutdown_obs(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_RETRIED) inc(&g_retried);
    if ((ev->kind == GPTPS_EV_DEAD_LETTERED || ev->kind == GPTPS_EV_DROPPED) &&
        ev->status == GPTPS_E_SHUTDOWN) inc(&g_shutdown_ev);
    if (ev->flags & GPTPS_EV_FLAG_SHUTDOWN) inc(&g_flagged);   /* only on those kinds */
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
    __atomic_store_n(&g_flagged, 0, __ATOMIC_SEQ_CST);

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
    CHECK(get(&g_flagged) == 1);                        /* marked as teardown's */
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
 * out there is dead-lettered - teardown's doing, marked GPTPS_EV_FLAG_SHUTDOWN, so
 * the record stays pending. With the attempt's own status on that event it was
 * quarantined: work whose policy was to keep trying, filed as poison. Run again
 * with the body itself returning GPTPS_E_SHUTDOWN: its FAILED then looks exactly
 * like a dead_letter item's own verdict, and before the flag the queue quarantined
 * it; the flag says teardown ended it, so it stays pending too. */
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
    __atomic_store_n(&g_flagged, 0, __ATOMIC_SEQ_CST);

    e = open_engine(1); CHECK(e != NULL); if (!e) return;
    reg_requeue(e);
    CHECK(gptps_register_observer(e, shutdown_obs, NULL) == GPTPS_OK);
    dq = gptps_dq_open(e, JOURNAL_A); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }
    CHECK(gptps_dq_submit(dq, "flaky", "f", 1, &h) == GPTPS_OK);
    CHECK(wait_for(&g_flaky_runs, 2));                  /* really cycling */
    gptps_shutdown(e);
    CHECK(get(&g_shutdown_ev) == 1);                    /* DEAD_LETTERED, E_SHUTDOWN */
    CHECK(get(&g_flagged) == 1);                        /* ...and marked as teardown's */
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
    __atomic_store_n(&g_flagged, 0, __ATOMIC_SEQ_CST);

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
    CHECK(get(&g_flagged) == 0);                        /* but not as teardown's */
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

/* A body that returns GPTPS_E_CANCELLED itself ends its item (no retry, no dead
 * letter), and its FAILED is marked GPTPS_EV_FLAG_SELF_CANCELLED: the work's own
 * outcome, so the record closes. Without that, nothing ever closed it - it stayed
 * pending and re-ran on every restart. An outside cancel still leaves it pending
 * (test_cancel). */
static int g_selfc_runs;
static gptps_status task_self_cancels(gptps_ctx *ctx, void *ud)
{ (void)ctx; (void)ud; inc(&g_selfc_runs); return GPTPS_E_CANCELLED; }
static void test_body_cancels_itself(void)
{
    gptps *e; gptps_dq *dq; gptps_handle h; gptps_task_def d; int run;
    remove(JOURNAL_A);
    __atomic_store_n(&g_selfc_runs, 0, __ATOMIC_SEQ_CST);
    for (run = 0; run < 2; ++run) {
        e = open_engine(1); CHECK(e != NULL); if (!e) return;
        memset(&d, 0, sizeof d);
        d.struct_size = sizeof d; d.exec = GPTPS_EXEC_INPROC; d.name = "selfc"; d.run = task_self_cancels;
        d.default_cost.struct_size = sizeof d.default_cost;
        d.default_policy.struct_size = sizeof d.default_policy;
        d.default_policy.max_retries = 1;
        gptps_register_task(e, &d);
        dq = gptps_dq_open(e, JOURNAL_A); CHECK(dq != NULL);
        if (!dq) { gptps_shutdown(e); return; }
        if (run == 0) {
            CHECK(gptps_dq_submit(dq, "selfc", "x", 1, &h) == GPTPS_OK);
            { time_t t0 = time(NULL); while (gptps_dq_pending(dq) != 0 && time(NULL) - t0 < 10) nap(); }
            CHECK(gptps_dq_pending(dq) == 0);           /* closed by its own cancel */
            CHECK(gptps_dq_quarantined(dq) == 0);       /* a cancel is not poison */
        } else {
            CHECK(gptps_dq_pending(dq) == 0);
            CHECK(gptps_dq_recover(dq) == 0);           /* never re-run */
        }
        gptps_shutdown(e);
        gptps_dq_close(dq);
    }
    CHECK(get(&g_selfc_runs) == 1);
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

/* E) recovery under backpressure. With limits.max_intake_depth = 2, one
 * gptps_dq_recover re-submits what fits and leaves the rest pending - the engine
 * refuses them with GPTPS_E_FULL - and calling it again once work has drained offers
 * the rest. The header used to say "safe to call once after open", which read as if
 * the leftover record could only come back in another process. */
static gptps *open_manual_depth(uint32_t depth)
{
    gptps_config cfg; gptps *e = NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1; cfg.limits.max_memory_bytes = 64u << 20;
    cfg.limits.max_intake_depth = depth;
    cfg.mode = GPTPS_RUN_MANUAL;
    if (gptps_open_ex(&cfg, &e) != GPTPS_OK) return NULL;
    reg_work(e);
    return e;
}
static void test_recover_again(void)
{
    gptps *e; gptps_dq *dq; gptps_handle h; size_t ran = 0; unsigned char b;
    remove(JOURNAL_A);
    __atomic_store_n(&g_block, 0, __ATOMIC_SEQ_CST);
    reset_ran();

    e = open_manual_depth(0); CHECK(e != NULL); if (!e) return;   /* run 1: persist three */
    dq = gptps_dq_open(e, JOURNAL_A); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }
    for (b = 1; b <= 3; ++b) CHECK(gptps_dq_submit(dq, "work", &b, 1, &h) == GPTPS_OK);
    gptps_shutdown(e);                                  /* never stepped: all pending */
    CHECK(gptps_dq_pending(dq) == 3);
    gptps_dq_close(dq);

    e = open_manual_depth(2); CHECK(e != NULL); if (!e) return;   /* run 2: intake of 2 */
    dq = gptps_dq_open(e, JOURNAL_A); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }
    CHECK(gptps_dq_pending(dq) == 3);
    CHECK(gptps_dq_recover(dq) == 2);                   /* the third does not fit */
    CHECK(gptps_dq_recover(dq) == 0);                   /* still full: nothing new */
    while (gptps_step(e, &ran) == GPTPS_OK && ran) { }
    CHECK(gptps_dq_pending(dq) == 1);                   /* two done, one waiting */
    CHECK(gptps_dq_recover(dq) == 1);                   /* room again: the rest */
    while (gptps_step(e, &ran) == GPTPS_OK && ran) { }
    CHECK(gptps_dq_pending(dq) == 0);
    CHECK(gptps_dq_recover(dq) == 0);                   /* and nothing twice */
    CHECK(get(&g_ran[1]) == 1 && get(&g_ran[2]) == 1 && get(&g_ran[3]) == 1);
    gptps_shutdown(e);
    gptps_dq_close(dq);
    remove(JOURNAL_A);
}

/* Warnings the add-on sends to the core's log sink. It sends them from the call that
 * met the trouble, on the calling thread - here always the main one. */
static int  g_warns;
static char g_warn[1024];
static void warn_sink(gptps_log_level lvl, const char *msg, void *ud)
{
    (void)ud;
    if (lvl < GPTPS_LOG_WARN || !msg || !strstr(msg, "gptps_durable_queue")) return;
    ++g_warns;
    snprintf(g_warn, sizeof g_warn, "%s", msg);
}

/* F) A clean shutdown is not a crash. An attempt the grace cancels ENDS - its FAILED
 * is journaled - so nothing is counted against the record, however many times it
 * happens. Were those attempts mistaken for deaths, the third round would hand the
 * two records back one at a time, and a later one would quarantine them. */
static void test_shutdown_not_a_crash(void)
{
    gptps *e; gptps_dq *dq; gptps_handle h; int round;
    remove(JOURNAL_A);
    __atomic_store_n(&g_ok, 0, __ATOMIC_SEQ_CST);
    for (round = 0; round < 5; ++round) {
        __atomic_store_n(&g_spin_runs, 0, __ATOMIC_SEQ_CST);
        if (round == 4) __atomic_store_n(&g_ok, 1, __ATOMIC_SEQ_CST);  /* last: let them finish */
        e = open_engine(2); CHECK(e != NULL); if (!e) return;
        reg_abandon(e, GPTPS_ON_FAILURE_DEAD_LETTER);
        CHECK(gptps_settings_set(e, "limits.shutdown_grace_ms", "100") == GPTPS_OK);
        dq = gptps_dq_open(e, JOURNAL_A); CHECK(dq != NULL);
        if (!dq) { gptps_shutdown(e); return; }
        if (round == 0) {
            CHECK(gptps_dq_submit(dq, "spin", "a", 1, &h) == GPTPS_OK);
            CHECK(gptps_dq_submit(dq, "spin", "b", 1, &h) == GPTPS_OK);
        } else {
            CHECK(gptps_dq_quarantined(dq) == 0);
            CHECK(gptps_dq_recover(dq) == 2);           /* both at once, every time */
        }
        CHECK(wait_for(&g_spin_runs, 2));
        gptps_shutdown(e);                              /* the grace cancels both */
        CHECK(gptps_dq_pending(dq) == (round == 4 ? 0u : 2u));
        CHECK(gptps_dq_quarantined(dq) == 0);
        gptps_dq_close(dq);
    }
    remove(JOURNAL_A);
}

/* G) A damaged journal. Five pending records of task "work", payload bytes 0..4,
 * make a file of an 8-byte header and five 29-byte records. Replay used to stop at
 * the first record that did not verify, as if it were the torn tail a crash leaves,
 * and gptps_dq_open's compaction then rewrote the file without everything after it:
 * one flipped bit in record 2 left 1 pending record of 5, silently. */
#define JOURNAL_C "dq_test_c.journal"
#define REC 29

/* The journal format, for the tests that write one by hand: the file header, then
 * [DQR1][type][0][name_len][payload_len][seq][name][payload][fnv1a], little-endian. */
static uint32_t jfnv(const unsigned char *p, size_t n, uint32_t h) { while (n--) { h ^= *p++; h *= 16777619u; } return h; }
static void jrec(FILE *f, char type, uint64_t seq, const char *name, const void *payload, uint32_t plen)
{
    unsigned char hdr[20], crc[4];
    size_t nl = strlen(name);
    uint32_t h;
    int i;
    hdr[0] = 0x31; hdr[1] = 0x52; hdr[2] = 0x51; hdr[3] = 0x44;
    hdr[4] = (unsigned char)type; hdr[5] = 0;
    hdr[6] = (unsigned char)nl; hdr[7] = (unsigned char)(nl >> 8);
    for (i = 0; i < 4; ++i) hdr[8 + i]  = (unsigned char)(plen >> (8 * i));
    for (i = 0; i < 8; ++i) hdr[12 + i] = (unsigned char)(seq >> (8 * i));
    h = jfnv(hdr, sizeof hdr, 2166136261u);
    h = jfnv((const unsigned char *)name, nl, h);
    h = jfnv((const unsigned char *)payload, plen, h);
    for (i = 0; i < 4; ++i) crc[i] = (unsigned char)(h >> (8 * i));
    fwrite(hdr, 1, sizeof hdr, f);
    if (nl) fwrite(name, 1, nl, f);
    if (plen) fwrite(payload, 1, plen, f);
    fwrite(crc, 1, sizeof crc, f);
}
static FILE *jopen(const char *path)
{
    static const unsigned char H[8] = { 0x31, 0x51, 0x44, 0x47, 0x01, 0x00, 0x00, 0x00 };
    FILE *f = fopen(path, "wb");
    if (f) fwrite(H, 1, sizeof H, f);
    return f;
}

static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n = -1;
    if (f) { if (fseek(f, 0, SEEK_END) == 0) n = ftell(f); fclose(f); }
    return n;
}
static int xor_byte(const char *path, long at, int mask)
{
    FILE *f = fopen(path, "r+b");
    int c = EOF;
    if (!f) return 0;
    if (fseek(f, at, SEEK_SET) == 0) c = fgetc(f);
    if (c != EOF && fseek(f, at, SEEK_SET) == 0) fputc(c ^ mask, f);
    fclose(f);
    return c != EOF;
}
static void make_five(void)
{
    gptps *e; gptps_dq *dq; gptps_handle h; unsigned char b;
    remove(JOURNAL_C);
    e = open_manual_depth(0); CHECK(e != NULL); if (!e) return;
    dq = gptps_dq_open(e, JOURNAL_C); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }
    for (b = 0; b < 5; ++b) CHECK(gptps_dq_submit(dq, "work", &b, 1, &h) == GPTPS_OK);
    gptps_shutdown(e);                                  /* never stepped: all five pending */
    gptps_dq_close(dq);
    CHECK(file_size(JOURNAL_C) == 8 + 5 * REC);         /* the layout the offsets assume */
}
static gptps *reopen_c(gptps_dq **out)
{
    gptps *e = open_manual_depth(0);
    *out = NULL;
    CHECK(e != NULL); if (!e) return NULL;
    *out = gptps_dq_open(e, JOURNAL_C); CHECK(*out != NULL);
    if (!*out) { gptps_shutdown(e); return NULL; }
    return e;
}
static void test_damaged_journal(void)
{
    gptps *e; gptps_dq *dq; size_t ran = 0; FILE *f;
    char copy[64];
    snprintf(copy, sizeof copy, "%s.corrupt", JOURNAL_C);
    remove(copy);
    __atomic_store_n(&g_block, 0, __ATOMIC_SEQ_CST);
    gptps_set_log_sink(warn_sink, NULL);

    /* 1. A flipped bit in record 2's payload costs record 2 and nothing else. */
    make_five();
    CHECK(xor_byte(JOURNAL_C, 8 + REC + 24, 0x01));
    g_warns = 0; reset_ran();
    e = reopen_c(&dq);
    if (e) {
        CHECK(gptps_dq_pending(dq) == 4);               /* was 1 */
        CHECK(g_warns == 1 && strstr(g_warn, "skipped") != NULL);
        CHECK(file_size(copy) == 8 + 5 * REC);          /* the damaged original, kept */
        CHECK(file_size(JOURNAL_C) == 8 + 4 * REC);     /* compacted to the four */
        CHECK(gptps_dq_recover(dq) == 4);
        while (gptps_step(e, &ran) == GPTPS_OK && ran) { }
        CHECK(get(&g_ran[0]) == 1 && get(&g_ran[1]) == 0 && get(&g_ran[2]) == 1 &&
              get(&g_ran[3]) == 1 && get(&g_ran[4]) == 1);
        CHECK(gptps_dq_pending(dq) == 0);
        gptps_shutdown(e); gptps_dq_close(dq);
    }
    e = reopen_c(&dq);                                  /* the compacted file is clean */
    if (e) { CHECK(g_warns == 1); gptps_shutdown(e); gptps_dq_close(dq); }
    remove(copy);

    /* 2. Record 2's length pushed past the end of the file. A torn write looks exactly
     * like that, and its payload could carry journal records of its own, so what
     * follows is reported and preserved but not loaded. */
    make_five();
    CHECK(xor_byte(JOURNAL_C, 8 + REC + 8 + 2, 0x10)); /* payload_len += 1 MiB */
    g_warns = 0;
    e = reopen_c(&dq);
    if (e) {
        CHECK(gptps_dq_pending(dq) == 1);
        CHECK(g_warns == 1 && strstr(g_warn, "were not read") != NULL);
        CHECK(file_size(copy) == 8 + 5 * REC);
        gptps_shutdown(e); gptps_dq_close(dq);
    }
    remove(copy);

    /* 3. A torn tail - a record's header and part of its body, as a crash mid-append
     * leaves it - is dropped silently, as it always was. */
    make_five();
    f = fopen(JOURNAL_C, "ab");
    if (f) {
        static const unsigned char torn[30] = {
            0x31, 0x52, 0x51, 0x44, 'P', 0, 4, 0, 100, 0, 0, 0,   /* "DQR1", P, name 4, payload 100 */
            6, 0, 0, 0, 0, 0, 0, 0, 'w', 'o', 'r', 'k',           /* seq 6, the name */
            1, 2, 3, 4, 5, 6 };                                   /* 6 of its 100 payload bytes */
        fwrite(torn, 1, sizeof torn, f);
        fclose(f);
    }
    g_warns = 0;
    e = reopen_c(&dq);
    if (e) {
        CHECK(gptps_dq_pending(dq) == 5);
        CHECK(g_warns == 0);
        CHECK(file_size(copy) < 0);                     /* nothing to preserve */
        gptps_shutdown(e); gptps_dq_close(dq);
    }

    /* 4. Damage, then a stale copy of an older record where record 3 was - the kind of
     * block a file system without data ordering can expose after a crash. Reading does
     * not resume at a 'P' whose seq is not above every one read before it, so record 1
     * does not come back twice; it resumes at record 4. */
    make_five();
    CHECK(xor_byte(JOURNAL_C, 8 + REC + 24, 0x01));
    f = fopen(JOURNAL_C, "r+b");
    if (f) {
        unsigned char b0 = 0;
        if (fseek(f, 8 + 2 * REC, SEEK_SET) == 0) jrec(f, 'P', 1, "work", &b0, 1);
        fclose(f);
    }
    CHECK(file_size(JOURNAL_C) == 8 + 5 * REC);
    g_warns = 0; reset_ran();
    e = reopen_c(&dq);
    if (e) {
        CHECK(gptps_dq_pending(dq) == 3);               /* records 1, 4 and 5 */
        CHECK(g_warns == 1);
        CHECK(gptps_dq_recover(dq) == 3);
        while (gptps_step(e, &ran) == GPTPS_OK && ran) { }
        CHECK(get(&g_ran[0]) == 1 && get(&g_ran[3]) == 1 && get(&g_ran[4]) == 1);
        CHECK(get(&g_ran[1]) + get(&g_ran[2]) == 0);
        gptps_shutdown(e); gptps_dq_close(dq);
    }
    gptps_set_log_sink(NULL, NULL);
    remove(JOURNAL_C); remove(copy);
}

/* H) Journaling must not stall the engine. A "pump" task submits durably back-to-back
 * - each gptps_dq_submit waiting on an fsync - while a thousand plain tasks have to
 * get through the same engine. The queue used to hold its lock across the fsync, and
 * its observer needs that lock for every task's events, so the workers queued behind
 * the pump: plain tasks went from ~420,000/s to 2-13/s, and this would take minutes.
 * Now the fsync holds no lock the engine needs. The pump runs on an engine worker, so
 * the test needs no threads of its own. */
static int       g_pump_submits, g_pump_stop, g_plain_done;
static gptps_dq *g_pdq;
static gptps_status task_pump(gptps_ctx *ctx, void *ud)
{
    uint64_t start = gptps_now_ms(ctx);
    (void)ud;
    while (!get(&g_pump_stop) && !gptps_is_cancelled(ctx) && gptps_now_ms(ctx) - start < 20000) {
        gptps_handle h; unsigned char b = 7;
        if (gptps_dq_submit(g_pdq, "jot", &b, 1, &h) == GPTPS_OK) inc(&g_pump_submits);
    }
    return GPTPS_OK;
}
static gptps_status task_jot(gptps_ctx *ctx, void *ud)   { (void)ctx; (void)ud; return GPTPS_OK; }
static gptps_status task_plain(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; inc(&g_plain_done); return GPTPS_OK; }
static void test_journaling_does_not_stall(void)
{
    gptps *e; gptps_dq *dq; gptps_task_def d; gptps_handle h; int i, before; time_t t0;
    remove(JOURNAL_A);
    __atomic_store_n(&g_pump_submits, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_pump_stop, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_plain_done, 0, __ATOMIC_SEQ_CST);
    e = open_engine(4); CHECK(e != NULL); if (!e) return;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.name = "pump";  d.run = task_pump;  gptps_register_task(e, &d);
    d.name = "jot";   d.run = task_jot;   gptps_register_task(e, &d);
    d.name = "plain"; d.run = task_plain; gptps_register_task(e, &d);
    dq = gptps_dq_open(e, JOURNAL_A); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }
    g_pdq = dq;
    CHECK(gptps_submit(e, "pump", NULL, 0, &h) == GPTPS_OK);
    CHECK(wait_for(&g_pump_submits, 5));                 /* journaling is under way */
    before = get(&g_pump_submits);
    for (i = 0; i < 1000; ++i) CHECK(gptps_submit(e, "plain", NULL, 0, &h) == GPTPS_OK);
    t0 = time(NULL);
    while (get(&g_plain_done) < 1000 && time(NULL) - t0 < 10) nap();
    CHECK(get(&g_plain_done) == 1000);                  /* was minutes */
    CHECK(get(&g_pump_submits) > before);               /* and the pump ran all along */
    __atomic_store_n(&g_pump_stop, 1, __ATOMIC_SEQ_CST);
    gptps_shutdown(e);
    gptps_dq_close(dq);
    g_pdq = NULL;
    remove(JOURNAL_A);
}

/* F) Suspects go back one at a time, each when the previous one's turn ends - however
 * it ends - and the crash count that made them suspects lives on disk as a 'K' marker.
 * The journals are written by hand, as two crashes would have left them. */
static gptps_handle g_qh[8];
static int          g_nq;
static void queued_obs(const gptps_event *ev, void *ud)
{
    (void)ud;   /* QUEUED: records the handle, and must not call into the queue */
    if (ev->kind == GPTPS_EV_QUEUED && g_nq < 8) g_qh[g_nq++] = ev->handle;
}
static void test_suspects_one_at_a_time(void)
{
    gptps *e; gptps_dq *dq; gptps_task_def d; size_t ran = 0; FILE *f;
    unsigned char two[4] = { 2, 0, 0, 0 }, b;
    __atomic_store_n(&g_block, 0, __ATOMIC_SEQ_CST);

    /* 1. A suspect that fails ends its turn: it is dead-lettered and quarantined as
     * any failure is, and the next one goes. */
    remove(JOURNAL_C);
    f = jopen(JOURNAL_C); CHECK(f != NULL); if (!f) return;
    b = 0xAB; jrec(f, 'P', 1, "poison", &b, 1); jrec(f, 'K', 1, "", two, 4);
    b = 1;    jrec(f, 'P', 2, "work", &b, 1);   jrec(f, 'K', 2, "", two, 4);
    b = 2;    jrec(f, 'P', 3, "work", &b, 1);   jrec(f, 'K', 3, "", two, 4);
    fclose(f);
    reset_ran();
    e = open_manual_depth(0); CHECK(e != NULL); if (!e) return;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "poison"; d.run = task_poison; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;       /* dead_letter, 0 retries */
    gptps_register_task(e, &d);
    dq = gptps_dq_open(e, JOURNAL_C); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }
    CHECK(gptps_dq_pending(dq) == 3);
    CHECK(gptps_dq_recover(dq) == 1);                   /* the oldest suspect, alone */
    CHECK(gptps_dq_recover(dq) == 0);                   /* nothing more while its turn lasts */
    CHECK(gptps_step(e, &ran) == GPTPS_OK && ran == 1); /* poison fails; the next is handed over */
    CHECK(get(&g_ran[1]) == 0);
    CHECK(gptps_step(e, &ran) == GPTPS_OK && ran == 1);
    CHECK(get(&g_ran[1]) == 1 && get(&g_ran[2]) == 0);
    CHECK(gptps_step(e, &ran) == GPTPS_OK && ran == 1);
    CHECK(get(&g_ran[2]) == 1);
    CHECK(gptps_dq_pending(dq) == 0);
    CHECK(gptps_dq_quarantined(dq) == 1);
    gptps_shutdown(e); gptps_dq_close(dq);

    /* 2. A suspect retracted with gptps_dq_cancel ends its turn, and so does one the
     * engine cancels while it waits - which keeps its record, as a cancel always does. */
    remove(JOURNAL_C);
    f = jopen(JOURNAL_C); CHECK(f != NULL); if (!f) return;
    b = 3; jrec(f, 'P', 1, "work", &b, 1); jrec(f, 'K', 1, "", two, 4);
    b = 4; jrec(f, 'P', 2, "work", &b, 1); jrec(f, 'K', 2, "", two, 4);
    b = 5; jrec(f, 'P', 3, "work", &b, 1); jrec(f, 'K', 3, "", two, 4);
    fclose(f);
    reset_ran(); g_nq = 0;
    e = open_manual_depth(0); CHECK(e != NULL); if (!e) return;
    CHECK(gptps_register_observer(e, queued_obs, NULL) == GPTPS_OK);
    dq = gptps_dq_open(e, JOURNAL_C); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }
    CHECK(gptps_dq_recover(dq) == 1 && g_nq == 1);
    CHECK(gptps_dq_cancel(dq, g_qh[0]) == GPTPS_OK);
    CHECK(g_nq == 2);                                   /* the retraction handed over the next */
    CHECK(gptps_cancel(e, g_qh[1]) == GPTPS_OK);
    CHECK(g_nq == 3);                                   /* and so did that one's FAILED */
    while (gptps_step(e, &ran) == GPTPS_OK && ran) { }
    CHECK(get(&g_ran[3]) == 0 && get(&g_ran[4]) == 0 && get(&g_ran[5]) == 1);
    CHECK(gptps_dq_pending(dq) == 1);                   /* the engine-cancelled one, kept */
    gptps_shutdown(e); gptps_dq_close(dq);
    remove(JOURNAL_C);
}

/* gptps_dq_submit_batch: every item journaled with one fsync, then each enqueued.
 * An item the engine refuses is closed, as a single submit's is; an item past the
 * limits refuses the whole batch before anything is written; the next run recovers
 * exactly the items that were taken. */
static void test_batch(void)
{
    gptps *e; gptps_dq *dq; gptps_dq_item it[5], bad[2];
    unsigned char b[5] = { 0, 1, 2, 3, 4 };
    size_t ran = 0; int i;
    remove(JOURNAL_A);
    __atomic_store_n(&g_block, 0, __ATOMIC_SEQ_CST);
    reset_ran();
    e = open_manual_depth(0); CHECK(e != NULL); if (!e) return;
    dq = gptps_dq_open(e, JOURNAL_A); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }
    for (i = 0; i < 5; ++i) { it[i].task_name = "work"; it[i].payload = &b[i]; it[i].len = 1; }
    it[3].task_name = "nosuch";                         /* the engine refuses this one */
    CHECK(gptps_dq_submit_batch(dq, it, 5) == GPTPS_OK);
    for (i = 0; i < 5; ++i)
        CHECK(i == 3 ? (it[i].status == GPTPS_E_NOTFOUND && it[i].handle == 0)
                     : (it[i].status == GPTPS_OK && it[i].handle != 0));
    CHECK(gptps_dq_pending(dq) == 4);
    bad[0] = it[0];
    bad[1].task_name = "work"; bad[1].payload = NULL; bad[1].len = 7;   /* no bytes for its length */
    CHECK(gptps_dq_submit_batch(dq, bad, 2) == GPTPS_E_INVAL);
    CHECK(bad[0].status == GPTPS_E_INVAL && bad[0].handle == 0);
    CHECK(gptps_dq_pending(dq) == 4);                   /* nothing of it was written */
    CHECK(gptps_dq_submit_batch(dq, NULL, 0) == GPTPS_OK);
    gptps_shutdown(e);                                  /* never stepped: four pending */
    gptps_dq_close(dq);

    e = open_manual_depth(0); CHECK(e != NULL); if (!e) return;
    dq = gptps_dq_open(e, JOURNAL_A); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }
    CHECK(gptps_dq_pending(dq) == 4);
    CHECK(gptps_dq_recover(dq) == 4);
    while (gptps_step(e, &ran) == GPTPS_OK && ran) { }
    CHECK(get(&g_ran[0]) == 1 && get(&g_ran[1]) == 1 && get(&g_ran[2]) == 1 &&
          get(&g_ran[3]) == 0 && get(&g_ran[4]) == 1);
    CHECK(gptps_dq_pending(dq) == 0);
    gptps_shutdown(e);
    gptps_dq_close(dq);
    remove(JOURNAL_A);
}

/* gptps_dq_set_resubmit_cb: every record the queue hands back to the engine is
 * reported with its new handle - three recovered at once, a suspect on its turn, and
 * the next suspect when that turn ends, from inside the step that ended it. Each handle
 * reported is the one the engine's QUEUED announced, in the same order. */
static gptps_handle  g_rh[8];
static unsigned char g_rp[8];
static int           g_nr;
static void resub_cb(const char *name, const void *payload, size_t len, gptps_handle h, void *ud)
{
    (void)ud;
    if (g_nr < 8 && name && strcmp(name, "work") == 0) {
        g_rh[g_nr] = h;
        g_rp[g_nr] = (payload && len == 1) ? *(const unsigned char *)payload : 0xFF;
        ++g_nr;
    }
}
static void test_resubmit_cb(void)
{
    gptps *e; gptps_dq *dq; FILE *f; size_t ran = 0; int i;
    unsigned char two[4] = { 2, 0, 0, 0 }, b;
    remove(JOURNAL_C);
    __atomic_store_n(&g_block, 0, __ATOMIC_SEQ_CST);
    f = jopen(JOURNAL_C); CHECK(f != NULL); if (!f) return;
    for (b = 1; b <= 3; ++b) jrec(f, 'P', b, "work", &b, 1);
    b = 4; jrec(f, 'P', 4, "work", &b, 1); jrec(f, 'K', 4, "", two, 4);   /* two suspects */
    b = 5; jrec(f, 'P', 5, "work", &b, 1); jrec(f, 'K', 5, "", two, 4);
    fclose(f);
    g_nr = 0; g_nq = 0;
    e = open_manual_depth(0); CHECK(e != NULL); if (!e) return;
    CHECK(gptps_register_observer(e, queued_obs, NULL) == GPTPS_OK);
    dq = gptps_dq_open(e, JOURNAL_C); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }
    CHECK(gptps_dq_set_resubmit_cb(NULL, resub_cb, NULL) == GPTPS_E_INVAL);
    CHECK(gptps_dq_set_resubmit_cb(dq, resub_cb, NULL) == GPTPS_OK);
    CHECK(gptps_dq_recover(dq) == 4);                   /* three, and the first suspect */
    CHECK(g_nr == 4);
    for (i = 0; i < g_nr && i < 4; ++i) CHECK(g_rp[i] == i + 1);
    while (gptps_step(e, &ran) == GPTPS_OK && ran) { }   /* suspect 4's turn ends: 5 goes */
    CHECK(g_nr == 5 && g_rp[4] == 5);
    CHECK(g_nq == 5);
    for (i = 0; i < 5 && i < g_nq; ++i) CHECK(g_rh[i] == g_qh[i]);
    CHECK(gptps_dq_pending(dq) == 0);
    gptps_shutdown(e);
    gptps_dq_close(dq);
    remove(JOURNAL_C);
}

/* I) Records no writer makes. A writer numbers each record one above the last, from
 * 1, so no record it writes is numbered near 2^63, and no 'P' at or below one before
 * it. Replay applied such records anyway, and find_by_seq's binary search, which needs
 * the table in rising order, then lost records:
 *   - a record at 2^64-1 set the next number to 2^64-1 + 1, which wrapped to 0. With
 *     two or more records in the table, the next submit could not find the record it
 *     had just added at 0 and wrote through NULL; with one, the record at 2^64-1 was
 *     the one the search lost, and its verdict closed nothing;
 *   - in P1 P7 P2, the search lost record 2 - and once a submit added 8, record 7.
 * Each such record is now damage: skipped, preserved, reported. The journals are
 * written by hand. Every record is task "work" with a one-byte payload, its tag. */
#define JOURNAL_D "dq_test_d.journal"
#define SEQ_TOP   0xFFFFFFFFFFFFFFFFull     /* 2^64-1 */
#define SEQ_MAX   0x7FFFFFFFFFFFFFFFull     /* 2^63-1: the highest a record may carry */
#define REC_D     (8 + 29)                  /* the file header, and one such record */

typedef struct { char type; uint64_t seq; unsigned char tag; } jspec;

static void write_d(const jspec *r, int n)
{
    FILE *f;
    int i;
    remove(JOURNAL_D);
    f = jopen(JOURNAL_D); CHECK(f != NULL); if (!f) return;
    for (i = 0; i < n; ++i) {
        if (r[i].type == 'P') jrec(f, 'P', r[i].seq, "work", &r[i].tag, 1);
        else                  jrec(f, r[i].type, r[i].seq, "", NULL, 0);   /* a marker */
    }
    fclose(f);
}
static gptps *open_d(gptps_dq **out)
{
    gptps *e = open_manual_depth(0);
    *out = NULL;
    CHECK(e != NULL); if (!e) return NULL;
    *out = gptps_dq_open(e, JOURNAL_D); CHECK(*out != NULL);
    if (!*out) { gptps_shutdown(e); return NULL; }
    return e;
}
static void step_all(gptps *e) { size_t ran = 0; while (gptps_step(e, &ran) == GPTPS_OK && ran) { } }
/* Every copy a damaged JOURNAL_D can leave. One an earlier run left would be taken for
 * this run's: a copy identical to the journal is not made again. */
static void remove_copies_d(void)
{
    char p[64];
    int k;
    snprintf(p, sizeof p, "%s.corrupt", JOURNAL_D); remove(p);
    for (k = 1; k <= 9; ++k) { snprintf(p, sizeof p, "%s.corrupt.%d", JOURNAL_D, k); remove(p); }
}

/* The next start: nothing pending, nothing damaged. A record whose verdict closed
 * nothing would come back here, and the tag it carries is named. */
static void next_start_clean(int warns_before)
{
    gptps *e; gptps_dq *dq;
    g_nr = 0;
    e = open_d(&dq);
    if (!e) return;
    CHECK(g_warns == warns_before);
    CHECK(gptps_dq_set_resubmit_cb(dq, resub_cb, NULL) == GPTPS_OK);
    CHECK(gptps_dq_recover(dq) == 0);
    if (g_nr) printf("  the record tagged %d stayed pending\n", g_rp[0]);
    CHECK(g_nr == 0);
    gptps_shutdown(e); gptps_dq_close(dq);
}

static void test_records_no_writer_makes(void)
{
    gptps *e; gptps_dq *dq; gptps_handle h; gptps_dq_item it[2];
    unsigned char t3 = 3, t4 = 4;
    char copy[64];
    snprintf(copy, sizeof copy, "%s.corrupt", JOURNAL_D);
    remove_copies_d();
    __atomic_store_n(&g_block, 0, __ATOMIC_SEQ_CST);
    gptps_set_log_sink(warn_sink, NULL);

    /* 1. Two records, the second at 2^64-1. The submit wrote through NULL. */
    {
        static const jspec J[] = { { 'P', 1, 1 }, { 'P', SEQ_TOP, 2 } };
        write_d(J, 2);
        g_warns = 0; reset_ran();
        e = open_d(&dq);
        if (e) {
            CHECK(gptps_dq_pending(dq) == 1);
            CHECK(g_warns == 1 && strstr(g_warn, "skipped") != NULL);
            CHECK(file_size(copy) == REC_D + 29);                /* the original, kept */
            CHECK(gptps_dq_submit(dq, "work", &t3, 1, &h) == GPTPS_OK);
            CHECK(gptps_dq_recover(dq) == 1);
            step_all(e);
            CHECK(get(&g_ran[1]) == 1 && get(&g_ran[2]) == 0 && get(&g_ran[3]) == 1);
            CHECK(gptps_dq_pending(dq) == 0);
            gptps_shutdown(e); gptps_dq_close(dq);
        }
        next_start_clean(1);
        remove_copies_d();
    }

    /* 2. One record, at 2^64-1: no crash, but its verdict closed nothing, so it ran
     * again at every start. */
    {
        static const jspec J[] = { { 'P', SEQ_TOP, 2 } };
        write_d(J, 1);
        g_warns = 0; reset_ran();
        e = open_d(&dq);
        if (e) {
            CHECK(gptps_dq_pending(dq) == 0);
            CHECK(g_warns == 1);
            CHECK(gptps_dq_submit(dq, "work", &t3, 1, &h) == GPTPS_OK);
            (void)gptps_dq_recover(dq);
            step_all(e);
            CHECK(get(&g_ran[2]) == 0 && get(&g_ran[3]) == 1);
            CHECK(gptps_dq_pending(dq) == 0);
            gptps_shutdown(e); gptps_dq_close(dq);
        }
        next_start_clean(1);
        remove_copies_d();
    }

    /* 3. A stray marker at 2^64-1, after two records. The open's compaction drops the
     * marker, so only a submit in the same run as that open met it - which fuzzing a
     * journal, run after it was compacted, does not reach. */
    {
        static const jspec J[] = { { 'P', 1, 1 }, { 'P', 2, 2 }, { 'D', SEQ_TOP, 0 } };
        write_d(J, 3);
        g_warns = 0; reset_ran();
        e = open_d(&dq);
        if (e) {
            CHECK(gptps_dq_pending(dq) == 2);
            CHECK(g_warns == 1);
            CHECK(gptps_dq_submit(dq, "work", &t3, 1, &h) == GPTPS_OK);
            CHECK(gptps_dq_recover(dq) == 2);
            step_all(e);
            CHECK(get(&g_ran[1]) == 1 && get(&g_ran[2]) == 1 && get(&g_ran[3]) == 1);
            CHECK(gptps_dq_pending(dq) == 0);
            gptps_shutdown(e); gptps_dq_close(dq);
        }
        next_start_clean(1);
        remove_copies_d();
    }

    /* 4. A record at 2^63-1, the highest number allowed, is a record: it runs and
     * closes. Its number is the last there is, so a submit or a batch now returns
     * GPTPS_E_FULL, writing nothing - until an open reads a journal without it. The
     * open says so. */
    {
        static const jspec J[] = { { 'P', 1, 1 }, { 'P', SEQ_MAX, 2 } };
        write_d(J, 2);
        g_warns = 0; reset_ran();
        e = open_d(&dq);
        if (e) {
            long size = file_size(JOURNAL_D);
            CHECK(gptps_dq_pending(dq) == 2);
            CHECK(g_warns == 1 && strstr(g_warn, "GPTPS_E_FULL") != NULL);
            CHECK(file_size(copy) < 0);                         /* not damage */
            CHECK(gptps_dq_submit(dq, "work", &t3, 1, &h) == GPTPS_E_FULL);
            memset(it, 0, sizeof it);
            it[0].task_name = "work"; it[0].payload = &t3; it[0].len = 1;
            CHECK(gptps_dq_submit_batch(dq, it, 1) == GPTPS_E_FULL && it[0].status == GPTPS_E_FULL);
            CHECK(file_size(JOURNAL_D) == size);              /* nothing written */
            CHECK(gptps_dq_recover(dq) == 2);
            step_all(e);
            CHECK(get(&g_ran[1]) == 1 && get(&g_ran[2]) == 1 && get(&g_ran[3]) == 0);
            CHECK(gptps_dq_pending(dq) == 0);
            gptps_shutdown(e); gptps_dq_close(dq);
        }
        e = open_d(&dq);            /* its markers are still in the journal this open reads */
        if (e) {
            CHECK(g_warns == 2);
            CHECK(gptps_dq_submit(dq, "work", &t3, 1, &h) == GPTPS_E_FULL);
            gptps_shutdown(e); gptps_dq_close(dq);
        }
        e = open_d(&dq);            /* ...and gone from the one it compacted */
        if (e) {
            CHECK(g_warns == 2);
            CHECK(gptps_dq_submit(dq, "work", &t3, 1, &h) == GPTPS_OK);
            gptps_shutdown(e); gptps_dq_close(dq);
        }
        remove_copies_d();
    }

    /* 5. One number left: a batch of two does not fit, a batch of one does, and then
     * nothing more. */
    {
        static const jspec J[] = { { 'P', SEQ_MAX - 1, 1 } };
        write_d(J, 1);
        g_warns = 0; reset_ran();
        e = open_d(&dq);
        if (e) {
            memset(it, 0, sizeof it);
            it[0].task_name = "work"; it[0].payload = &t3; it[0].len = 1;
            it[1].task_name = "work"; it[1].payload = &t4; it[1].len = 1;
            CHECK(gptps_dq_submit_batch(dq, it, 2) == GPTPS_E_FULL);
            CHECK(it[0].status == GPTPS_E_FULL && it[1].status == GPTPS_E_FULL);
            CHECK(gptps_dq_submit_batch(dq, it, 1) == GPTPS_OK && it[0].status == GPTPS_OK);
            CHECK(gptps_dq_submit(dq, "work", &t4, 1, &h) == GPTPS_E_FULL);
            CHECK(g_warns == 0);                                /* reached here, not read */
            CHECK(gptps_dq_pending(dq) == 2);
            CHECK(gptps_dq_recover(dq) == 1);
            step_all(e);
            CHECK(get(&g_ran[1]) == 1 && get(&g_ran[3]) == 1 && get(&g_ran[4]) == 0);
            CHECK(gptps_dq_pending(dq) == 0);
            gptps_shutdown(e); gptps_dq_close(dq);
        }
        remove_copies_d();
    }

    /* 6. P1 P7 P2, as a stale block could leave them. Record 2 is damage, and every
     * verdict on the others closes them. Bare, the search lost record 2. */
    {
        static const jspec J[] = { { 'P', 1, 1 }, { 'P', 7, 7 }, { 'P', 2, 2 } };
        int round;
        for (round = 0; round < 2; ++round) {
            write_d(J, 3);
            g_warns = 0; reset_ran();
            e = open_d(&dq);
            if (e) {
                CHECK(gptps_dq_pending(dq) == 2);
                CHECK(g_warns == 1 && strstr(g_warn, "skipped") != NULL);
                CHECK(file_size(copy) == 8 + 3 * 29);
                /* With a submit first, the table was 1 7 2 8, and the search lost
                 * record 7 instead. */
                if (round == 1) CHECK(gptps_dq_submit(dq, "work", &t3, 1, &h) == GPTPS_OK);
                CHECK(gptps_dq_recover(dq) == 2);
                step_all(e);
                CHECK(get(&g_ran[1]) == 1 && get(&g_ran[7]) == 1 && get(&g_ran[2]) == 0);
                CHECK(get(&g_ran[3]) == round);
                CHECK(gptps_dq_pending(dq) == 0);
                gptps_shutdown(e); gptps_dq_close(dq);
            }
            next_start_clean(1);
            remove_copies_d();
        }
    }
    gptps_set_log_sink(NULL, NULL);
    remove(JOURNAL_D); remove_copies_d();
}

#if defined(TEST_DURABLE_FORK)
/* gptps_dq_cancel's failure path: "if it cannot be made durable the call returns
 * GPTPS_E_IO, leaving the record open and the execution alone". The journal's
 * descriptor is swapped for a pipe,
 * on which fwrite and fflush succeed but fsync fails, so the marker cannot be made
 * durable: the record must stay open and the execution must be left alone. The
 * same window holds gptps_dq_submit to its own promise not to enqueue. A pipe cannot
 * be truncated either, so the queue cannot take the marker back out, and breaks: it
 * warns once, writes nothing more, and fails every durable call - with the journal's
 * descriptor back, too - until a compaction rewrites the journal. POSIX only (dup2),
 * and skipped where fsync on a pipe does not fail. */
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
            gptps_set_log_sink(warn_sink, NULL);
            g_warns = 0;
            CHECK(gptps_dq_cancel(dq, hb) == GPTPS_E_IO);
            CHECK(g_warns == 1 && strstr(g_warn, "fails until gptps_dq_compact") != NULL);
            CHECK(gptps_dq_pending(dq) == 2);           /* nothing retracted... */
            CHECK(get(&g_io_ev) == 0);                  /* ...and nothing cancelled */
            {
                gptps_handle hx = 0; unsigned char x = 5;
                gptps_dq_item bi[2];
                CHECK(gptps_dq_submit(dq, "work", &x, 1, &hx) == GPTPS_E_IO);
                CHECK(hx == 0 && gptps_dq_pending(dq) == 2);
                bi[0].task_name = bi[1].task_name = "work";
                bi[0].payload = bi[1].payload = &x; bi[0].len = bi[1].len = 1;
                CHECK(gptps_dq_submit_batch(dq, bi, 2) == GPTPS_E_IO);   /* one fsync, failed: none */
                CHECK(bi[0].status == GPTPS_E_IO && bi[1].status == GPTPS_E_IO && bi[0].handle == 0);
                CHECK(gptps_dq_pending(dq) == 2);
            }
            CHECK(dup2(saved, jfd) == jfd);
            close(saved);
            CHECK(gptps_dq_cancel(dq, hb) == GPTPS_E_IO); /* broken: writes nothing */
            CHECK(get(&g_io_ev) == 0 && gptps_dq_pending(dq) == 2);
            CHECK(g_warns == 1);                        /* one warning per break */
            CHECK(gptps_dq_compact(dq) == GPTPS_OK);    /* repaired */
            gptps_set_log_sink(NULL, NULL);
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

/* F) crash loops. A body that kills its own process is never dead-lettered, so
 * quarantine never saw it: every run recovered it and died with it. Here "killer"
 * waits until three "slow" records are running beside it and then SIGKILLs its
 * process, as the OOM killer would. Each run is a child process:
 *   run 1 - all four submitted; the journal is compacted while all four run, so the
 *           evidence has to survive a rewrite; then the first death.
 *   run 2 - one death changes nothing: all four go back at once; the second death.
 *   run 3 - all four are suspects and go back one at a time, oldest first: each slow
 *           one runs alone and finishes, then killer runs and dies a third time.
 *   then  - gptps_dq_open quarantines killer, and only killer, with a warning. The
 *           slow records were running at two of its deaths and are not convicted.
 * A child that sees anything else _exit()s with a code instead of being killed. */
static int g_run;                       /* which run a child is: set before the fork */
static int g_slow_started, g_slow_active, g_slow_overlap, g_killer_started, g_compacted;

static gptps_status task_slow(gptps_ctx *ctx, void *ud)
{
    uint64_t start = gptps_now_ms(ctx);
    (void)ud;
    if (inc(&g_slow_active) > 1) inc(&g_slow_overlap);
    inc(&g_slow_started);
    if (g_run >= 3) while (gptps_now_ms(ctx) - start < 150) nap();   /* on trial: then pass */
    else while (!gptps_is_cancelled(ctx) && gptps_now_ms(ctx) - start < 20000) nap();
    __atomic_sub_fetch(&g_slow_active, 1, __ATOMIC_SEQ_CST);
    return GPTPS_OK;
}
static gptps_status task_killer(gptps_ctx *ctx, void *ud)
{
    uint64_t start = gptps_now_ms(ctx);
    (void)ud;
    inc(&g_killer_started);
    if (g_run < 3)
        while ((get(&g_slow_started) < 3 || (g_run == 1 && !get(&g_compacted))) &&
               gptps_now_ms(ctx) - start < 20000) nap();
    else if (get(&g_slow_started) != 3 || get(&g_slow_overlap) != 0)
        _exit(20);                      /* the suspects did not go one at a time */
    kill(getpid(), SIGKILL);
    /* Never return. On macOS kill() returns before the process is gone, and a body
     * that returned would end its attempt - an 'F' - clearing the very count under
     * test. A real crash (a segfault, an abort, the OOM killer) does not return. */
    for (;;) nap();
}
static void crash_child(void)
{
    gptps *e; gptps_dq *dq; gptps_handle h; gptps_task_def d; unsigned char b; time_t t0;
    e = open_engine(4);
    if (!e) _exit(2);
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.name = "slow"; d.run = task_slow; gptps_register_task(e, &d);
    d.name = "killer"; d.run = task_killer; gptps_register_task(e, &d);
    dq = gptps_dq_open(e, JOURNAL_B);
    if (!dq) _exit(3);
    if (g_run == 1) {
        for (b = 1; b <= 3; ++b) if (gptps_dq_submit(dq, "slow", &b, 1, &h) != GPTPS_OK) _exit(4);
        b = 9;
        if (gptps_dq_submit(dq, "killer", &b, 1, &h) != GPTPS_OK) _exit(4);
        t0 = time(NULL);
        while ((get(&g_slow_started) < 3 || !get(&g_killer_started)) && time(NULL) - t0 < 10) nap();
        if (gptps_dq_compact(dq) != GPTPS_OK) _exit(5);
        inc(&g_compacted);
    } else {
        size_t got;
        if (gptps_dq_pending(dq) != 4) _exit(6);
        got = gptps_dq_recover(dq);
        if (got != (g_run == 2 ? 4u : 1u)) _exit(70 + (int)(got < 9 ? got : 9));   /* run 3: one suspect */
    }
    t0 = time(NULL);
    while (time(NULL) - t0 < 15) nap();
    _exit(8);                           /* killer never took the process down */
}
/* H) Every acknowledged submit survives a SIGKILL that lands in the middle of the
 * group commit. In a child, three pumps submit durably at once - so they share
 * fsyncs, and one of them in batches of eight - while a fourth task compacts the
 * journal over and over, and each pump writes the id of every submit that returned
 * GPTPS_OK to a pipe. The work itself never finishes, so every record stays pending.
 * The parent kills the child mid-storm, then recovers the journal: every id the child
 * acknowledged must run, exactly once.
 * This is the protocol's whole promise - a compaction that dropped a record whose 'P'
 * had become durable, or that wrote one whose submit then failed, would show here. */
#define STORM_PUMPS 3
static int g_storm_fd = -1, g_storm_next, g_storm_ran[1 << 16];
static gptps_dq *g_sdq;
static gptps_status task_storm_pump(gptps_ctx *ctx, void *ud)
{
    (void)ud;
    while (!gptps_is_cancelled(ctx)) {
        uint32_t id = (uint32_t)inc(&g_storm_next);
        gptps_handle h;
        if (id >= (1u << 16)) break;
        if (gptps_dq_submit(g_sdq, "item", &id, sizeof id, &h) == GPTPS_OK &&
            write(g_storm_fd, &id, sizeof id) != (ssize_t)sizeof id) _exit(9);
    }
    return GPTPS_OK;
}
static gptps_status task_storm_bpump(gptps_ctx *ctx, void *ud)   /* batches of 8 */
{
    (void)ud;
    while (!gptps_is_cancelled(ctx)) {
        gptps_dq_item bi[8];
        uint32_t id[8];
        int k;
        for (k = 0; k < 8; ++k) {
            id[k] = (uint32_t)inc(&g_storm_next);
            if (id[k] >= (1u << 16)) return GPTPS_OK;
            bi[k].task_name = "item"; bi[k].payload = &id[k]; bi[k].len = sizeof id[k];
        }
        if (gptps_dq_submit_batch(g_sdq, bi, 8) != GPTPS_OK) continue;
        for (k = 0; k < 8; ++k)
            if (bi[k].status == GPTPS_OK && write(g_storm_fd, &id[k], sizeof id[k]) != (ssize_t)sizeof id[k])
                _exit(9);
    }
    return GPTPS_OK;
}
static gptps_status task_storm_compact(gptps_ctx *ctx, void *ud)
{
    (void)ud;
    while (!gptps_is_cancelled(ctx)) { gptps_dq_compact(g_sdq); nap(); }
    return GPTPS_OK;
}
static gptps_status task_storm_item(gptps_ctx *ctx, void *ud)
{
    size_t n = 0;
    const uint32_t *id = (const uint32_t *)gptps_payload(ctx, &n);
    (void)ud;
    if (g_sdq) { while (!gptps_is_cancelled(ctx)) nap(); return GPTPS_OK; }   /* child: never done */
    if (id && n == sizeof *id && *id < (1u << 16)) ++g_storm_ran[*id];       /* parent: count it */
    return GPTPS_OK;
}
static void reg_storm(gptps *e)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.name = "pump";    d.run = task_storm_pump;    gptps_register_task(e, &d);
    d.name = "bpump";   d.run = task_storm_bpump;   gptps_register_task(e, &d);
    d.name = "compact"; d.run = task_storm_compact; gptps_register_task(e, &d);
    d.name = "item";    d.run = task_storm_item;    gptps_register_task(e, &d);
}
static void test_storm_crash(void)
{
    pid_t pid;
    int pfd[2], wst = 0, i, acked = 0, missing = 0, twice = 0;
    static int acked_id[1 << 16];
    uint32_t id;
    remove(JOURNAL_B);
    memset(acked_id, 0, sizeof acked_id);
    memset(g_storm_ran, 0, sizeof g_storm_ran);
    CHECK(pipe(pfd) == 0);
    pid = fork();
    CHECK(pid >= 0);
    if (pid < 0) return;
    if (pid == 0) {
        gptps *e; gptps_handle h;
        close(pfd[0]);
        g_storm_fd = pfd[1];
        e = open_engine(STORM_PUMPS + 3);   /* the pumps, the compactor, two stuck items */
        if (!e) _exit(2);
        reg_storm(e);
        g_sdq = gptps_dq_open(e, JOURNAL_B);
        if (!g_sdq) _exit(3);
        for (i = 0; i < STORM_PUMPS; ++i) gptps_submit(e, i ? "pump" : "bpump", NULL, 0, &h);
        gptps_submit(e, "compact", NULL, 0, &h);
        for (;;) nap();                     /* until the parent kills it */
    }
    close(pfd[1]);
    {   /* let the storm run until 300 submits are acknowledged, or 10s */
        time_t t0 = time(NULL);
        struct pollfd pf;
        pf.fd = pfd[0]; pf.events = POLLIN; pf.revents = 0;
        while (acked < 300 && time(NULL) - t0 < 10) {
            if (poll(&pf, 1, 100) <= 0) continue;
            if (read(pfd[0], &id, sizeof id) != (ssize_t)sizeof id) break;
            if (id < (1u << 16)) { ++acked_id[id]; ++acked; }
        }
    }
    kill(pid, SIGKILL);
    waitpid(pid, &wst, 0);
    CHECK(WIFSIGNALED(wst) && WTERMSIG(wst) == SIGKILL);
    while (read(pfd[0], &id, sizeof id) == (ssize_t)sizeof id)   /* acknowledged before it died */
        if (id < (1u << 16)) { ++acked_id[id]; ++acked; }
    close(pfd[0]);
    CHECK(acked >= 300);
    {
        gptps *e; gptps_dq *dq; size_t ran = 0;
        g_sdq = NULL;
        e = open_manual_depth(0); CHECK(e != NULL); if (!e) return;
        reg_storm(e);
        dq = gptps_dq_open(e, JOURNAL_B); CHECK(dq != NULL);
        if (!dq) { gptps_shutdown(e); return; }
        gptps_dq_recover(dq);
        while (gptps_step(e, &ran) == GPTPS_OK && ran) { }
        for (i = 0; i < (1 << 16); ++i) {
            if (acked_id[i] && g_storm_ran[i] == 0) ++missing;
            if (g_storm_ran[i] > 1) ++twice;
        }
        CHECK(missing == 0);                /* acknowledged, then lost */
        CHECK(twice == 0);
        if (missing) printf("  storm: %d of %d acknowledged submits were lost\n", missing, acked);
        gptps_shutdown(e);
        gptps_dq_close(dq);
    }
    remove(JOURNAL_B);
}

/* Print a journal's records - type, seq, and a 'K' marker's count - so that a crash-loop
 * run that went wrong says what the journal held. Diagnostics only. */
static void dump_journal(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char h[20], body[4];
    printf("  journal %s:", path);
    if (!f || fseek(f, 8, SEEK_SET) != 0) { printf(" (unreadable)\n"); if (f) fclose(f); return; }
    while (fread(h, 1, sizeof h, f) == sizeof h) {
        unsigned nlen = (unsigned)(h[6] | (h[7] << 8));
        unsigned long plen = (unsigned long)h[8] | ((unsigned long)h[9] << 8) |
                             ((unsigned long)h[10] << 16) | ((unsigned long)h[11] << 24);
        printf(" %c%u", h[4] >= 32 && h[4] < 127 ? h[4] : '?', (unsigned)h[12]);
        if (h[4] == 'K' && plen == 4 && nlen == 0 && fread(body, 1, 4, f) == 4) {
            printf("(%u)", (unsigned)body[0]);
            if (fseek(f, 4, SEEK_CUR) != 0) break;
            continue;
        }
        if (fseek(f, (long)(nlen + plen + 4), SEEK_CUR) != 0) break;
    }
    printf("\n");
    fclose(f);
}

static void test_crash_loop(void)
{
    pid_t pid;
    int wst = 0;
    remove(JOURNAL_B);
    for (g_run = 1; g_run <= 3; ++g_run) {
        __atomic_store_n(&g_slow_started, 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&g_slow_active, 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&g_slow_overlap, 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&g_killer_started, 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&g_compacted, 0, __ATOMIC_SEQ_CST);
        pid = fork();
        CHECK(pid >= 0);
        if (pid < 0) return;
        if (pid == 0) crash_child();
        waitpid(pid, &wst, 0);
        CHECK(WIFSIGNALED(wst) && WTERMSIG(wst) == SIGKILL);
        if (WIFEXITED(wst)) {
            printf("  crash-loop run %d exited with %d\n", g_run, WEXITSTATUS(wst));
            dump_journal(JOURNAL_B);
        }
    }
    {
        gptps *e; gptps_dq *dq;
        g_warns = 0;
        gptps_set_log_sink(warn_sink, NULL);
        e = open_manual_depth(0); CHECK(e != NULL);
        dq = e ? gptps_dq_open(e, JOURNAL_B) : NULL; CHECK(dq != NULL);
        if (dq) {
            CHECK(gptps_dq_quarantined(dq) == 1);       /* killer, and only killer */
            CHECK(gptps_dq_pending(dq) == 0);           /* the slow ones finished in run 3 */
            CHECK(g_warns == 1 && strstr(g_warn, "'killer'") != NULL);
            g_drained_n = 0; g_drained_name[0] = 0; g_drained_payload = 0;
            CHECK(gptps_dq_drain_quarantine(dq, quarantine_cb, NULL) == 1);
            CHECK(strcmp(g_drained_name, "killer") == 0 && g_drained_payload == 9);
        }
        if (e) gptps_shutdown(e);
        if (dq) gptps_dq_close(dq);
        gptps_set_log_sink(NULL, NULL);
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
    test_body_cancels_itself();
    test_shutdown_manual();
    test_recover_again();
    test_shutdown_not_a_crash();
    test_suspects_one_at_a_time();
    test_batch();
    test_resubmit_cb();
    test_damaged_journal();
    test_records_no_writer_makes();
    test_journaling_does_not_stall();
#if defined(TEST_DURABLE_FORK)
    test_cancel_io();  /* POSIX: the E_IO path, by swapping the journal's fd */
    test_recovery();   /* crash-recovery via fork (POSIX) */
    test_crash_loop(); /* POSIX: a body that kills its process, three times */
    test_storm_crash();/* POSIX: SIGKILL in the middle of the group commit */
#endif
    if (fails) { printf("%d durable-queue check(s) FAILED\n", fails); return 1; }
    printf("all durable-queue checks passed\n");
    return 0;
}
