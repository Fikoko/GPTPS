/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_bounded.c - a bounded engine allocates nothing once work starts.
 *
 * docs/BOUNDED.md promises that after the first submit to an engine opened with
 * gptps_config.max_items, the work path neither allocates nor frees. This holds it
 * to that with a counting allocator (gptps_set_allocator): set up, submit once (the
 * seal), then drive the work path - payloads, results, retries, backoff, a deadline,
 * dead letters and their drain, eviction, named resources, a constraint that defers,
 * a scheduler hook, cancels from intake and from backoff, settings writes, a full
 * pool and its reuse - and fail on any allocator call after the seal. MANUAL first,
 * then THREADED with producers on three host threads. Also the mode's edges:
 * GPTPS_E_FULL with every item in use, GPTPS_E_INVAL past a payload or result limit,
 * GPTPS_E_BUSY for setup after the seal, in-process tasks only.
 */
#include "gptps.h"
#include "gptps_hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

/* --- the counting allocator -------------------------------------------------
 * Its mutex comes from the HAL before the hook is installed, so the HAL's own
 * allocation is not counted (the HAL allocates only in its create calls). */
static gptps_mutex *a_m;
static long a_live, a_sealed, a_alloc_after, a_free_after;

static void *c_malloc(size_t n, void *ud)
{
    void *p = malloc(n);
    (void)ud;
    gptps_mutex_lock(a_m);
    if (p) ++a_live;
    if (a_sealed) ++a_alloc_after;
    gptps_mutex_unlock(a_m);
    return p;
}
static void *c_realloc(void *p, size_t n, void *ud)
{
    void *q = realloc(p, n);
    (void)ud;
    gptps_mutex_lock(a_m);
    if (a_sealed) ++a_alloc_after;
    gptps_mutex_unlock(a_m);
    return q;
}
static void c_free(void *p, void *ud)
{
    (void)ud;
    free(p);
    gptps_mutex_lock(a_m);
    --a_live;
    if (a_sealed) ++a_free_after;
    gptps_mutex_unlock(a_m);
}
static void seal_watch(long on)
{
    gptps_mutex_lock(a_m);
    a_sealed = on;
    if (on) { a_alloc_after = 0; a_free_after = 0; }
    gptps_mutex_unlock(a_m);
}

/* A pause through the HAL's own timed wait, on a pair made up front. */
static gptps_mutex *nap_m;
static gptps_cond  *nap_c;
static void nap(uint64_t ms)
{
    gptps_mutex_lock(nap_m);
    gptps_cond_timedwait(nap_c, nap_m, ms);
    gptps_mutex_unlock(nap_m);
}

/* --- tasks ------------------------------------------------------------------- */

enum { PAYLOAD_MAX = 16, RESULT_MAX = 8 };

static gptps_status echo(gptps_ctx *ctx, void *ud)        /* the payload back, up to RESULT_MAX */
{
    size_t n;
    const void *p = gptps_payload(ctx, &n);
    (void)ud;
    if (!p) return gptps_result_set(ctx, "e", 1);
    return gptps_result_set(ctx, p, n < RESULT_MAX ? n : RESULT_MAX);
}

static gptps_status fail_always(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_E_TASK; }

static int g_flaky_left;                                  /* MANUAL: one thread */
static gptps_status flaky(gptps_ctx *ctx, void *ud)       /* fails until its count runs out */
{
    (void)ctx; (void)ud;
    if (g_flaky_left > 0) { --g_flaky_left; return GPTPS_E_TASK; }
    return GPTPS_OK;
}

static gptps_status spin_past_deadline(gptps_ctx *ctx, void *ud)
{
    (void)ud;
    while (gptps_deadline_ms(ctx) && gptps_now_ms(ctx) < gptps_deadline_ms(ctx)) { }
    return GPTPS_OK;
}

static gptps_status g_big_set, g_big_nocopy;
static char g_static_result[] = "nocopy!";
static gptps_status big(gptps_ctx *ctx, void *ud)         /* one byte past the limit, then nocopy */
{
    char buf[RESULT_MAX + 1];
    (void)ud;
    memset(buf, 'x', sizeof buf);
    g_big_set = gptps_result_set(ctx, buf, sizeof buf);
    g_big_nocopy = gptps_result_set_nocopy(ctx, g_static_result, sizeof g_static_result, NULL);
    return GPTPS_OK;
}

/* --- observer, constraint, scheduler ---------------------------------------- */

static gptps_mutex *ev_m;
static gptps_cond  *ev_c;
static long ev_finished, ev_failed, ev_dead, ev_queued, ev_bad_result, ev_nocopy;

static void observe(const gptps_event *ev, void *ud)
{
    (void)ud;
    gptps_mutex_lock(ev_m);
    switch (ev->kind) {
    case GPTPS_EV_QUEUED:        ++ev_queued; break;
    case GPTPS_EV_FINISHED:
        ++ev_finished;
        if (ev->result_len > RESULT_MAX + 0u && !(ev->result_len == sizeof g_static_result)) ++ev_bad_result;
        if (ev->result_len == sizeof g_static_result && memcmp(ev->result, g_static_result, sizeof g_static_result) == 0) ++ev_nocopy;
        break;
    case GPTPS_EV_FAILED:        ++ev_failed; break;
    case GPTPS_EV_DEAD_LETTERED: ++ev_dead; break;
    default: break;
    }
    gptps_cond_broadcast(ev_c);
    gptps_mutex_unlock(ev_m);
}

/* Defer, once, every item whose payload starts with 'D': it goes to the backoff
 * queue for a millisecond. The handles deferred are kept in a fixed table. */
static gptps_handle g_deferred[64];
static int g_ndeferred;
static gptps_admit_decision defer_d(const gptps_constraint_input *in, uint32_t *retry_after_ms, void *ud)
{
    int i;
    (void)ud;
    if (!in->payload || in->payload_len == 0 || ((const char *)in->payload)[0] != 'D') return GPTPS_ADMIT;
    for (i = 0; i < g_ndeferred; ++i) if (g_deferred[i] == in->handle) return GPTPS_ADMIT;
    if (g_ndeferred < 64) g_deferred[g_ndeferred++] = in->handle;
    *retry_after_ms = 1;
    return GPTPS_DEFER;
}

static int64_t by_priority(const gptps_sched_input *in, void *ud) { (void)ud; return (int64_t)in->priority; }

typedef gptps_status (*task_fn)(gptps_ctx *, void *);
static void reg(gptps *e, const char *name, task_fn fn, uint32_t retries, gptps_on_failure onf)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = fn; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = retries;
    d.default_policy.on_failure = onf;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

static gptps *open_bounded(gptps_run_mode mode, uint64_t max_items, uint32_t workers)
{
    gptps_config cfg;
    gptps *e = NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = workers;
    cfg.limits.max_memory_bytes = 1u << 20;
    cfg.mode = mode;
    cfg.max_items = max_items;
    cfg.max_payload_bytes = PAYLOAD_MAX;
    cfg.max_result_bytes = RESULT_MAX;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    return e;
}

static void step_all(gptps *e)
{
    size_t ran;
    while (gptps_step(e, &ran) == GPTPS_OK && ran) { }
}

static long finished_now(void) { long v; gptps_mutex_lock(ev_m); v = ev_finished; gptps_mutex_unlock(ev_m); return v; }
static long dead_now(void)     { long v; gptps_mutex_lock(ev_m); v = ev_dead;     gptps_mutex_unlock(ev_m); return v; }

/* Re-drive each dead letter as an echo of the same payload. */
static long g_redriven;
static void redrive(const gptps_dead_letter *dl, void *ud)
{
    gptps_handle h;
    if (gptps_submit((gptps *)ud, "echo", dl->payload, dl->payload_len, &h) == GPTPS_OK) ++g_redriven;
}

/* ===== MANUAL ================================================================ */

static void test_manual(void)
{
    enum { N = 24 };
    gptps *e;
    gptps_handle h, hs[N + 1];
    gptps_submit_options o;
    gptps_task_def d;
    char pay[PAYLOAD_MAX + 1], buf[32];
    int i, taken;
    long fin0;

    e = open_bounded(GPTPS_RUN_MANUAL, N, 2);
    if (!e) return;
    /* setup: allocates, as it always has */
    reg(e, "echo", echo, 0, GPTPS_ON_FAILURE_DEAD_LETTER);
    reg(e, "fail", fail_always, 1, GPTPS_ON_FAILURE_DEAD_LETTER);
    reg(e, "flaky", flaky, 0, GPTPS_ON_FAILURE_REQUEUE);
    reg(e, "spin", spin_past_deadline, 0, GPTPS_ON_FAILURE_DEAD_LETTER);
    reg(e, "big", big, 0, GPTPS_ON_FAILURE_DEAD_LETTER);
    reg(e, "seat", echo, 0, GPTPS_ON_FAILURE_DEAD_LETTER);
    CHECK(gptps_define_resource(e, "seats", 2) == GPTPS_OK);
    CHECK(gptps_set_task_resource_cost(e, "seat", "seats", 1) == GPTPS_OK);
    CHECK(gptps_register_observer(e, observe, NULL) == GPTPS_OK);
    CHECK(gptps_register_constraint(e, defer_d, NULL) == GPTPS_OK);
    CHECK(gptps_set_scheduler(e, by_priority, NULL) == GPTPS_OK);

    /* the seal: the first submit allocates the working set */
    CHECK(gptps_submit(e, "echo", "a", 1, &h) == GPTPS_OK);
    seal_watch(1);

    /* payload limits */
    memset(pay, 'p', sizeof pay);
    CHECK(gptps_submit(e, "echo", pay, PAYLOAD_MAX + 1, &h) == GPTPS_E_INVAL);
    CHECK(gptps_submit(e, "echo", pay, PAYLOAD_MAX, &h) == GPTPS_OK);
    CHECK(gptps_submit(e, "echo", NULL, 0, &h) == GPTPS_OK);
    step_all(e);

    /* a full pool, then every item back in it */
    for (taken = 0; taken < N + 1; ++taken)
        if (gptps_submit(e, "echo", "f", 1, &hs[taken]) != GPTPS_OK) break;
    CHECK(taken == N);
    CHECK(gptps_submit(e, "echo", "f", 1, &h) == GPTPS_E_FULL);
    fin0 = finished_now();
    step_all(e);
    CHECK(finished_now() - fin0 == N);
    for (i = 0; i < N; ++i) CHECK(gptps_submit(e, "echo", "r", 1, &hs[i]) == GPTPS_OK);
    step_all(e);

    /* cancels: from intake, an unknown handle, and from backoff */
    for (i = 0; i < 4; ++i) CHECK(gptps_submit(e, "echo", "c", 1, &hs[i]) == GPTPS_OK);
    CHECK(gptps_cancel(e, hs[1]) == GPTPS_OK);
    CHECK(gptps_cancel(e, hs[1]) == GPTPS_E_NOTFOUND);
    CHECK(gptps_cancel(e, (gptps_handle)999999) == GPTPS_E_NOTFOUND);
    CHECK(gptps_submit(e, "echo", "D1", 2, &hs[5]) == GPTPS_OK);
    CHECK(gptps_submit(e, "echo", "D2", 2, &hs[6]) == GPTPS_OK);
    { size_t ran; CHECK(gptps_step(e, &ran) == GPTPS_OK); }   /* both deferred into backoff */
    CHECK(gptps_cancel(e, hs[6]) == GPTPS_OK);
    for (i = 0; i < 200 && finished_now() < fin0 + 2 * N + 4; ++i) { nap(2); step_all(e); }

    /* retries, dead letters, a drain that re-drives them */
    g_redriven = 0;
    for (i = 0; i < 3; ++i) CHECK(gptps_submit(e, "fail", "x", 1, &h) == GPTPS_OK);
    step_all(e);
    CHECK(gptps_dead_letter_count(e) == 3);
    CHECK(gptps_dead_letter_drain(e, redrive, e) == 3);
    CHECK(g_redriven == 3);
    step_all(e);

    /* eviction past limits.max_dead_letters, through a live settings write */
    CHECK(gptps_settings_set(e, "limits.max_dead_letters", "2") == GPTPS_OK);
    CHECK(gptps_settings_get(e, "limits.max_dead_letters", buf, sizeof buf) == GPTPS_OK);
    for (i = 0; i < 4; ++i) CHECK(gptps_submit(e, "fail", "y", 1, &h) == GPTPS_OK);
    step_all(e);
    CHECK(gptps_dead_letter_count(e) == 2);
    CHECK(gptps_dead_letter_drain(e, NULL, NULL) == 2);

    /* requeue until it succeeds; named resources; a deadline; a too-big result */
    g_flaky_left = 2;
    CHECK(gptps_submit(e, "flaky", NULL, 0, &h) == GPTPS_OK);
    for (i = 0; i < 6; ++i) CHECK(gptps_submit(e, "seat", "s", 1, &h) == GPTPS_OK);
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o; o.flags = GPTPS_SUBMIT_TIMEOUT_MS; o.timeout_ms = 3;
    CHECK(gptps_submit_ex(e, "spin", NULL, 0, &o, &h) == GPTPS_OK);
    CHECK(gptps_submit(e, "big", NULL, 0, &h) == GPTPS_OK);
    /* a requeue waits at least the engine's floor between cycles: give it up to 10 s */
    for (i = 0; i < 2000; ++i) { step_all(e); if (g_flaky_left == 0) break; nap(5); }
    step_all(e);
    CHECK(g_flaky_left == 0);
    CHECK(g_big_set == GPTPS_E_INVAL && g_big_nocopy == GPTPS_OK && ev_nocopy == 1);

    /* setup after the seal is refused; what allocates nothing still works */
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "late"; d.run = echo; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_E_BUSY);
    CHECK(gptps_unregister_task(e, "echo", 0) == GPTPS_E_BUSY);
    CHECK(gptps_define_resource(e, "gpu", 1) == GPTPS_E_BUSY);
    CHECK(gptps_define_resource(e, "seats", 3) == GPTPS_OK);              /* re-budget */
    CHECK(gptps_set_task_resource_cost(e, "seat", "seats", 2) == GPTPS_OK);
    CHECK(gptps_register_observer(e, observe, NULL) == GPTPS_E_BUSY);
    CHECK(gptps_register_constraint(e, defer_d, NULL) == GPTPS_E_BUSY);
    CHECK(gptps_define_global(e, "late.knob", GPTPS_SETTING_UINT, "1", NULL, 0) == GPTPS_E_BUSY);
    CHECK(gptps_set_scheduler(e, by_priority, NULL) == GPTPS_OK);
    CHECK(ev_bad_result == 0);

    gptps_mutex_lock(a_m);
    printf("MANUAL:   after the seal: %ld allocations, %ld frees\n", a_alloc_after, a_free_after);
    CHECK(a_alloc_after == 0);
    CHECK(a_free_after == 0);
    gptps_mutex_unlock(a_m);
    seal_watch(0);
    gptps_shutdown(e);
}

/* ===== THREADED ============================================================== */

enum { PRODUCERS = 3, PER_PRODUCER = 400 };
static gptps *g_te;
static long g_cancelled_ok;

static void *producer(void *arg)
{
    int i, id = (int)(size_t)arg;
    for (i = 0; i < PER_PRODUCER; ++i) {
        gptps_handle h;
        unsigned char pay[4];
        gptps_status st;
        pay[0] = (unsigned char)id; pay[1] = (unsigned char)i; pay[2] = (unsigned char)(i >> 8); pay[3] = 0xAB;
        while ((st = gptps_submit(g_te, (i % 50 == 7) ? "fail" : "echo", pay, sizeof pay, &h)) == GPTPS_E_FULL)
            nap(1);
        CHECK(st == GPTPS_OK);
        if (i % 37 == 0) {
            st = gptps_cancel(g_te, h);
            CHECK(st == GPTPS_OK || st == GPTPS_E_NOTFOUND);
            if (st == GPTPS_OK) { gptps_mutex_lock(ev_m); ++g_cancelled_ok; gptps_mutex_unlock(ev_m); }
        }
    }
    return NULL;
}

static void test_threaded(void)
{
    gptps *e;
    gptps_handle h;
    gptps_thread *th[PRODUCERS];
    int i;
    long fin0, target, waited;

    e = open_bounded(GPTPS_RUN_THREADED, 100, 4);   /* past the index's 64-slot minimum, so a trim would apply */
    if (!e) return;
    g_te = e;
    reg(e, "echo", echo, 0, GPTPS_ON_FAILURE_DEAD_LETTER);
    reg(e, "fail", fail_always, 0, GPTPS_ON_FAILURE_DEAD_LETTER);
    CHECK(gptps_register_observer(e, observe, NULL) == GPTPS_OK);

    CHECK(gptps_submit(e, "echo", "z", 1, &h) == GPTPS_OK);   /* the seal */
    seal_watch(1);
    fin0 = finished_now() - 1;                                 /* the seal's own item may finish late */

    for (i = 0; i < PRODUCERS; ++i) th[i] = gptps_thread_start(producer, (void *)(size_t)i);
    for (i = 0; i < PRODUCERS; ++i) if (th[i]) gptps_thread_join(th[i]);
    /* every echo that was not cancelled finishes; every fail dead-letters */
    target = PRODUCERS * (PER_PRODUCER - (PER_PRODUCER / 50 + 1)) - g_cancelled_ok;
    for (waited = 0; waited < 20000 && finished_now() - fin0 < target - PRODUCERS; waited += 5) nap(5);
    CHECK(finished_now() - fin0 >= target - PRODUCERS);
    for (waited = 0; waited < 20000 && gptps_dead_letter_count(e) < (size_t)(dead_now() ? 1 : 0); waited += 5) nap(5);
    g_redriven = 0;
    gptps_dead_letter_drain(e, redrive, e);
    for (waited = 0; waited < 5000 && gptps_dead_letter_count(e) != 0; waited += 5) nap(5);

    gptps_mutex_lock(a_m);
    printf("THREADED: after the seal: %ld allocations, %ld frees (%ld finished, %ld re-driven)\n",
           a_alloc_after, a_free_after, finished_now() - fin0, g_redriven);
    CHECK(a_alloc_after == 0);
    CHECK(a_free_after == 0);
    gptps_mutex_unlock(a_m);
    seal_watch(0);
    gptps_shutdown(e);
}

/* ===== edges ================================================================== */

static void test_edges(void)
{
    gptps *e;
    gptps_task_def d;
    gptps_handle h;

    /* in-process tasks only */
    e = open_bounded(GPTPS_RUN_MANUAL, 4, 1);
    if (!e) return;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "prog"; d.exec = GPTPS_EXEC_PROGRAM;
    {
        static const char *const argv[] = { "true", NULL };
        d.argv = argv;
    }
    d.default_cost.struct_size = sizeof d.default_cost; d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_E_INVAL);
    gptps_shutdown(e);

    /* one item: the second submit finds the pool empty, until the first is done. And a
     * first submit that names no task does not seal it: setup goes on after it. */
    e = open_bounded(GPTPS_RUN_MANUAL, 1, 1);
    if (!e) return;
    CHECK(gptps_submit(e, "echo", "0", 1, &h) == GPTPS_E_NOTFOUND);
    reg(e, "echo", echo, 0, GPTPS_ON_FAILURE_DEAD_LETTER);
    CHECK(gptps_submit(e, "echo", "1", 1, &h) == GPTPS_OK);
    CHECK(gptps_submit(e, "echo", "2", 1, &h) == GPTPS_E_FULL);
    step_all(e);
    CHECK(gptps_submit(e, "echo", "3", 1, &h) == GPTPS_OK);
    step_all(e);
    gptps_shutdown(e);
}

int main(void)
{
    gptps_allocator a;
    a_m = gptps_mutex_create(); ev_m = gptps_mutex_create(); ev_c = gptps_cond_create();
    nap_m = gptps_mutex_create(); nap_c = gptps_cond_create();
    if (!a_m || !ev_m || !ev_c || !nap_m || !nap_c) { printf("test_bounded: no HAL mutex\n"); return 1; }
    memset(&a, 0, sizeof a);
    a.struct_size = sizeof a; a.malloc_fn = c_malloc; a.realloc_fn = c_realloc; a.free_fn = c_free;
    CHECK(gptps_set_allocator(&a) == GPTPS_OK);

    test_manual();
    test_threaded();
    test_edges();
    CHECK(a_live == 0);                       /* everything came back at shutdown */

    CHECK(gptps_set_allocator(NULL) == GPTPS_OK);
    printf("test_bounded: %s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
