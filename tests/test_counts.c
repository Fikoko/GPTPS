/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_counts.c - the per-type queue counts (ABI 2.9), white-box, against the walk.
 *
 * gptps_task_get_info used to count a type's queued / running / dead items by walking
 * six queues under the engine lock; since 2.9 the queue primitives keep the counts as
 * items move (see "per-type counts" in src/engine.c). A move that forgets its count
 * would report work that is not there - or hide work that is - for the rest of the
 * engine's life, so this drives random runs of every operation that moves an item
 * and, after EVERY step, re-counts by the old walk (counts_verify, compiled in by
 * GPTPS_COUNTS_CHECK) and through the public getters:
 *
 *   submit (plain and with policies: retries, a backoff that parks the retry in
 *   `delayed`, dead letter / requeue / drop), cancel (queued, admitted, and from
 *   inside a task body while others run), fail and retry, dead letters evicted past
 *   a small cap, re-drive of a drain and plain discard, pause / resume, clone,
 *   unregister in all three modes and re-register under the same name, shutdown and
 *   a fresh engine; and, with every allocation failing across one unregister, a
 *   type parked on e->retired that still owns dead letters, then their drain.
 *
 * Each event the engine emits is checked too, from the observer - so the counts hold
 * between the halves of a transition, not only between API calls. Legs: MANUAL (1e5
 * operations), THREADED with workers racing the main thread (where the check runs
 * under the engine lock), a bounded engine, and, on POSIX outside the simulation
 * HAL, the OOP and PROGRAM executors. Built on tests/hal_sim.c the THREADED and
 * bounded legs run on the seeded schedule.
 *
 * It includes engine.c directly - counts_verify is static and test-only - and links
 * the rest of the core beside it, as tests/test_hidx.c does.
 *
 *   test_counts [seed]       (default: GPTPS_STRESS_SEED, else 1)
 */
#define GPTPS_COUNTS_CHECK 1
#include "../src/engine.c"
#include <stdio.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static uint64_t rng = 1;
static uint32_t rnd(uint32_t n)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return n ? (uint32_t)((rng >> 11) % n) : 0u;
}

static gptps *g_e;                 /* the engine the observer checks */
static int    g_manual;
static int    g_closing;           /* in gptps_shutdown: the public getters are not checked */
static unsigned long g_checks, g_ev_checks, g_bad;
static const char *g_leg = "";
static unsigned long g_op;
/* coverage: how many checks found each queue non-empty (intake .. dead letters) */
static unsigned long g_seen[6];

/* After every step: the walk agrees with the counts. Under e->m, so it holds in a
 * THREADED engine too. */
static void t_verify_locked(gptps *e, const char *where, int from_event)
{
    unsigned bad;
    gptps_mutex_lock(e->m);   /* the test's own tallies too: workers check from events */
    bad = counts_verify(e);
    g_seen[0] += e->intake.count != 0;        g_seen[1] += e->delayed.count != 0;
    g_seen[2] += e->ready.count != 0;         g_seen[3] += e->running_items.count != 0;
    g_seen[4] += e->done.count != 0;          g_seen[5] += e->dead_letter.count != 0;
    ++g_checks;
    if (from_event) ++g_ev_checks;
    if (bad) {
        if (g_bad++ < 10)
            printf("FAIL %s op %lu (%s): %u count(s) disagree with the walk\n", g_leg, g_op, where, bad);
        ++fails;
    }
    gptps_mutex_unlock(e->m);
}

/* The public view, single-threaded only (MANUAL): every row's counters summed are the
 * engine's totals, and each row is what the walk counts for that type. */
static void verify_public(gptps *e)
{
    gptps_engine_totals t;
    uint64_t q = 0, run = 0, dead = 0;
    size_t i, n = gptps_task_count(e);
    memset(&t, 0, sizeof t); t.struct_size = sizeof t;
    CHECK(gptps_engine_counts(e, &t) == GPTPS_OK);
    for (i = 0; i < n; ++i) {
        gptps_task_info ti; gptps_reg *r;
        memset(&ti, 0, sizeof ti); ti.struct_size = sizeof ti;
        if (gptps_task_get_info(e, i, &ti) != GPTPS_OK) { ++fails; continue; }
        q += ti.queued; run += ti.running; dead += ti.dead;
        for (r = e->registry; r && strcmp(r->name, ti.name_copy) != 0; r = r->next) { }
        if (!r) { ++fails; continue; }
        if (ti.queued  != fifo_count_reg(&e->intake, r) + fifo_count_reg(&e->delayed, r) ||
            ti.running != fifo_count_reg(&e->ready, r) + fifo_count_reg(&e->running_items, r)
                          + fifo_count_reg(&e->done, r) ||
            ti.dead    != fifo_count_reg(&e->dead_letter, r)) {
            if (g_bad++ < 10) printf("FAIL %s op %lu: get_info row %s disagrees with the walk\n", g_leg, g_op, ti.name_copy);
            ++fails;
        }
    }
    if (q != t.queued || run != t.running || dead > t.dead ||
        t.dead != gptps_dead_letter_count(e) || t.delayed > t.queued) {
        if (g_bad++ < 10)
            printf("FAIL %s op %lu: totals q %llu/%llu run %llu/%llu dead %llu/%llu\n", g_leg, g_op,
                   (unsigned long long)q, (unsigned long long)t.queued,
                   (unsigned long long)run, (unsigned long long)t.running,
                   (unsigned long long)dead, (unsigned long long)t.dead);
        ++fails;
    }
}

static void on_event(const gptps_event *ev, void *ud)
{
    (void)ev; (void)ud;
    if (g_e) {
        t_verify_locked(g_e, "event", 1);
        /* MANUAL: the event is on this thread, inside gptps_step, between the halves of
         * a transition - where `done` holds what ran and is not yet accounted */
        if (g_manual && !g_closing) verify_public(g_e);
    }
}

/* ---- task bodies --------------------------------------------------------- */
/* payload[0]: 0 ok, 1 fail, 2 cancel a recent handle from inside the body, then ok,
 * 3 (THREADED) run a little while, polling for a cancel */
#define NH 256
static gptps_handle g_h[NH];
static unsigned     g_hn;
static void remember(gptps_handle h) { if (h) g_h[g_hn++ % NH] = h; }
static gptps_handle a_handle(void) { return g_hn ? g_h[rnd(g_hn < NH ? g_hn : NH)] : 0; }

/* The handle a body cancels. Atomic, as workers read it, and an int, as the MSVC shim's
 * atomics are 32-bit (tests/test_atomic_compat.h): a handle here is a small count. */
static int g_body_victim;

static gptps_status body(gptps_ctx *ctx, void *ud)
{
    const unsigned char *p; size_t len = 0;
    (void)ud;
    p = (const unsigned char *)gptps_payload(ctx, &len);
    if (!p || len == 0) return GPTPS_OK;
    switch (p[0]) {
    case 1: return GPTPS_E_TASK;
    case 2:
        {
            gptps_handle v = (gptps_handle)(unsigned)__atomic_load_n(&g_body_victim, __ATOMIC_SEQ_CST);
            if (v) (void)gptps_cancel(ctx->engine, v);
        }
        return GPTPS_OK;
    case 3: {
        uint64_t t0 = gptps_now_ms(ctx);
        while (!gptps_is_cancelled(ctx) && gptps_now_ms(ctx) - t0 < 3) { }
        return gptps_is_cancelled(ctx) ? GPTPS_E_CANCELLED : GPTPS_OK;
    }
    default: return GPTPS_OK;
    }
}

#define NTYPES 5
static const char *const TN[NTYPES] = { "a", "b", "c", "d", "e" };

static gptps_status t_reg_type(gptps *e, const char *name, gptps_exec_kind x, const char *const *argv)
{
    gptps_task_def d; memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = body; d.exec = x; d.argv = argv;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_cost.mem_bytes = 1024;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = rnd(3);
    d.default_policy.on_failure = GPTPS_ON_FAILURE_DEAD_LETTER;
    return gptps_register_task(e, &d);
}

static gptps *t_open_engine(gptps_run_mode mode, unsigned conc, uint64_t max_items)
{
    gptps_config cfg; gptps *e = NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = conc;
    cfg.limits.max_memory_bytes = 1u << 20;
    cfg.mode = mode;
    cfg.max_items = max_items;
    cfg.max_payload_bytes = max_items ? 8 : 0;
    cfg.max_dead_letters = 1 + rnd(12);             /* small: eviction is a path too */
    cfg.shutdown_grace_ms = 2000;
    if (gptps_open_ex(&cfg, &e) != GPTPS_OK || !e) { ++fails; return NULL; }
    CHECK(gptps_register_observer(e, on_event, NULL) == GPTPS_OK);
    return e;
}

static void redrive_cb(const gptps_dead_letter *dl, void *ud)
{
    gptps_handle h = 0;
    gptps *e = (gptps *)ud;
    if (rnd(3) == 0) return;   /* discarded */
    if (gptps_submit_ex(e, dl->task_name, dl->payload, dl->payload_len,
                        GPTPS_STRUCT_HAS(gptps_dead_letter, dl, options) ? &dl->options : NULL,
                        &h) == GPTPS_OK) remember(h);
}

static uint64_t live_items(gptps *e)
{
    gptps_engine_totals t; memset(&t, 0, sizeof t); t.struct_size = sizeof t;
    if (gptps_engine_counts(e, &t) != GPTPS_OK) return 0;
    return t.queued + t.running + t.dead;
}

/* One random operation. `setup_ok`: registry changes are allowed (not bounded). */
static void one_op(gptps *e, int setup_ok, int threaded)
{
    unsigned k = rnd(100);
    const char *tn = TN[rnd(NTYPES)];
    gptps_handle h = 0;
    if (k < 34) {                                   /* submit */
        unsigned char pl[2];
        gptps_submit_options o;
        if (live_items(e) > 96) { size_t ran; if (!threaded) gptps_step(e, &ran); return; }
        pl[0] = (unsigned char)rnd(threaded ? 4 : 3); pl[1] = 0;
        if (pl[0] == 2) __atomic_store_n(&g_body_victim, (int)(unsigned)a_handle(), __ATOMIC_SEQ_CST);
        memset(&o, 0, sizeof o); o.struct_size = sizeof o;
        if (rnd(2)) {
            o.flags = GPTPS_SUBMIT_POLICY | (rnd(2) ? GPTPS_SUBMIT_PRIORITY : 0u);
            o.priority = (int32_t)rnd(5) - 2;
            o.policy.struct_size = sizeof o.policy;
            o.policy.max_retries = rnd(3);
            /* 1 s: parked in `delayed`. Rarer THREADED, where an unregister that drains
             * and the shutdown wait the backoff out in real time. */
            o.policy.retry_backoff_seconds = rnd(threaded ? 40 : 3) == 0 ? 1u : 0u;
            k = rnd(20);
            o.policy.on_failure = k == 0 ? GPTPS_ON_FAILURE_REQUEUE
                                : k < 5  ? GPTPS_ON_FAILURE_DROP : GPTPS_ON_FAILURE_DEAD_LETTER;
            if (o.policy.on_failure == GPTPS_ON_FAILURE_REQUEUE) pl[0] = 0;   /* never forever */
            if (gptps_submit_ex(e, tn, pl, 1, &o, &h) == GPTPS_OK) remember(h);
        } else if (gptps_submit(e, tn, pl, 1, &h) == GPTPS_OK) remember(h);
    } else if (k < 48) {                            /* cancel: queued, admitted or done */
        (void)gptps_cancel(e, a_handle());
    } else if (k < 72) {                            /* step (MANUAL) */
        size_t ran;
        if (!threaded) { unsigned n = 1 + rnd(3); while (n--) (void)gptps_step(e, &ran); }
    } else if (k < 77) {                            /* drain the dead letters */
        if (rnd(2)) (void)gptps_dead_letter_drain(e, redrive_cb, e);
        else        (void)gptps_dead_letter_drain(e, NULL, NULL);
    } else if (k < 84) {                            /* pause / resume */
        (void)gptps_set_task_enabled(e, tn, (int)rnd(2));
    } else if (k < 87) {                            /* the dead-letter cap, live */
        char v[8];
        snprintf(v, sizeof v, "%u", 1 + rnd(12));
        (void)gptps_settings_set(e, "limits.max_dead_letters", v);
    } else if (!setup_ok) {
        (void)gptps_cancel(e, a_handle());
    } else if (k < 92) {                            /* unregister, one of three modes */
        static const unsigned modes[3] = { GPTPS_REMOVE_REJECT_IF_BUSY, GPTPS_REMOVE_DRAIN,
                                           GPTPS_REMOVE_CANCEL };
        unsigned m = modes[rnd(3)];
        if (threaded && m == GPTPS_REMOVE_DRAIN && rnd(4)) m = GPTPS_REMOVE_CANCEL;
        (void)gptps_unregister_task(e, tn, m);
    } else if (k < 97) {                            /* re-register what is gone */
        (void)t_reg_type(e, tn, GPTPS_EXEC_INPROC, NULL);
    } else {                                        /* clone, then submit to the clone */
        char nm[16];
        snprintf(nm, sizeof nm, "%s.c%u", tn, rnd(3));
        if (gptps_clone_task(e, tn, nm) == GPTPS_OK || gptps_task_exists(e, nm)) {
            unsigned char pl = (unsigned char)rnd(2);
            if (gptps_submit(e, nm, &pl, 1, &h) == GPTPS_OK) remember(h);
        }
        if (rnd(2)) (void)gptps_unregister_task(e, nm, GPTPS_REMOVE_CANCEL);
    }
}

static void leg(const char *name, gptps_run_mode mode, unsigned conc, uint64_t max_items,
                unsigned long ops, unsigned long per_engine)
{
    gptps *e = NULL;
    unsigned long i, since = 0;
    unsigned k;
    int threaded = mode != GPTPS_RUN_MANUAL;
    g_leg = name;
    for (i = 0; i < ops && !fails; ++i) {
        g_op = i;
        if (!e) {
            unsigned t;
            e = t_open_engine(mode, conc, max_items);
            if (!e) return;
            g_manual = !threaded;
            for (t = 0; t < NTYPES; ++t) CHECK(t_reg_type(e, TN[t], GPTPS_EXEC_INPROC, NULL) == GPTPS_OK);
            if (threaded && !max_items) {
                /* a SERVICE: each instance runs ~3 ms and is restarted, through the
                 * retry path, until a cancel, an unregister or the shutdown stops it */
                gptps_task_def d; memset(&d, 0, sizeof d);
                d.struct_size = sizeof d; d.name = "svc"; d.run = body; d.exec = GPTPS_EXEC_INPROC;
                d.flags = GPTPS_TASK_SERVICE;
                d.default_cost.struct_size = sizeof d.default_cost;
                d.default_policy.struct_size = sizeof d.default_policy;
                CHECK(gptps_register_task(e, &d) == GPTPS_OK);
            }
            g_e = e;
            since = 0;
            t_verify_locked(e, "open", 0);
        }
        if (threaded && !max_items && rnd(200) == 0) {   /* start, or stop, a service instance */
            unsigned char pl = 3;
            gptps_handle h = 0;
            if (rnd(2) && gptps_submit(e, "svc", &pl, 1, &h) == GPTPS_OK) remember(h);
            else if (rnd(8) == 0) (void)gptps_unregister_task(e, "svc", GPTPS_REMOVE_CANCEL);
        } else
            one_op(e, max_items == 0, threaded);
        t_verify_locked(e, "op", 0);
        if (!threaded) verify_public(e);
        if (++since >= per_engine || rnd(4000) == 0) {
            /* shutdown: its DROPPED events are checked from the observer while the
             * queues empty; the engine is gone afterwards */
            g_closing = 1;
            gptps_shutdown(e);
            g_closing = 0;
            e = NULL; g_e = NULL;
        }
    }
    if (e) { g_closing = 1; gptps_shutdown(e); g_closing = 0; g_e = NULL; }
    printf("  leg %s: %lu operations; checks that saw intake %lu, delayed %lu, ready %lu, "
           "running %lu, done %lu, dead letters %lu\n", name, i,
           g_seen[0], g_seen[1], g_seen[2], g_seen[3], g_seen[4], g_seen[5]);
    for (k = 0; k < 6; ++k) { CHECK(g_seen[k] > 0); g_seen[k] = 0; }
    fflush(stdout);
}

/* A dead letter whose name copy cannot be made when its type is unregistered keeps
 * naming the type through its reg, which is parked on e->retired and still holds that
 * letter's count (item_own_name returns before qcount_out). Fail every allocation
 * across one unregister to get there, then drain: the counts on the retired reg must
 * match the walk at each step, and the drain must take them back to 0. */
static int g_oom;
static void *oom_malloc(size_t n, void *u)           { (void)u; return g_oom ? NULL : malloc(n ? n : 1); }
static void *oom_realloc(void *p, size_t n, void *u) { (void)u; return g_oom ? NULL : realloc(p, n ? n : 1); }
static void  oom_free(void *p, void *u)              { (void)u; free(p); }
static void retired_leg(void)
{
    gptps_allocator a;
    gptps_engine_totals t;
    gptps *e;
    unsigned i, nd;
    size_t ran;
    int before = fails;
    memset(&a, 0, sizeof a);
    a.struct_size = sizeof a; a.malloc_fn = oom_malloc; a.realloc_fn = oom_realloc; a.free_fn = oom_free;
    CHECK(gptps_set_allocator(&a) == GPTPS_OK);
    g_leg = "retired"; g_op = 0;
    e = t_open_engine(GPTPS_RUN_MANUAL, 2, 0);
    if (!e) { (void)gptps_set_allocator(NULL); return; }
    CHECK(gptps_settings_set(e, "limits.max_dead_letters", "16") == GPTPS_OK);
    CHECK(t_reg_type(e, "oomr", GPTPS_EXEC_INPROC, NULL) == GPTPS_OK);
    CHECK(t_reg_type(e, "keep", GPTPS_EXEC_INPROC, NULL) == GPTPS_OK);
    g_e = e; g_manual = 1;
    for (i = 0; i < 4; ++i) {
        gptps_submit_options o; unsigned char pl = 1; gptps_handle h = 0;
        memset(&o, 0, sizeof o); o.struct_size = sizeof o; o.flags = GPTPS_SUBMIT_POLICY;
        o.policy.struct_size = sizeof o.policy; o.policy.on_failure = GPTPS_ON_FAILURE_DEAD_LETTER;
        CHECK(gptps_submit_ex(e, i & 1 ? "keep" : "oomr", &pl, 1, &o, &h) == GPTPS_OK);
    }
    for (i = 0; i < 20; ++i) (void)gptps_step(e, &ran);
    nd = (unsigned)gptps_dead_letter_count(e);
    CHECK(nd == 4);
    g_oom = 1;
    (void)gptps_unregister_task(e, "oomr", GPTPS_REMOVE_CANCEL);
    g_oom = 0;
    gptps_mutex_lock(e->m);
    CHECK(e->retired != NULL);
    if (e->retired) CHECK(e->retired->nq[GPTPS_Q_DEAD] == 2);
    gptps_mutex_unlock(e->m);
    t_verify_locked(e, "retired: after unregister", 0);
    verify_public(e);
    memset(&t, 0, sizeof t); t.struct_size = sizeof t;
    CHECK(gptps_engine_counts(e, &t) == GPTPS_OK && t.dead == 4);
    CHECK(gptps_dead_letter_drain(e, NULL, NULL) == 4);
    t_verify_locked(e, "retired: after drain", 0);
    gptps_mutex_lock(e->m);
    if (e->retired) CHECK(e->retired->nq[GPTPS_Q_DEAD] == 0);
    gptps_mutex_unlock(e->m);
    g_closing = 1; gptps_shutdown(e); g_closing = 0; g_e = NULL; g_manual = 0;
    CHECK(gptps_set_allocator(NULL) == GPTPS_OK);
    printf("  leg retired: %s\n", fails == before ? "a retired type's dead letters counted on it" : "FAILED");
}

#if !defined(_WIN32) && !defined(GPTPS_TEST_HAL_SIM)
/* The process executors move items through the same queues; a short run of each. */
static void proc_leg(void)
{
    static const char *const ok_argv[]   = { "/bin/sh", "-c", "exit 0", NULL };
    static const char *const fail_argv[] = { "/bin/sh", "-c", "exit 3", NULL };
    gptps *e;
    unsigned i;
    g_leg = "proc";
    e = t_open_engine(GPTPS_RUN_THREADED, 3, 0);
    if (!e) return;
    CHECK(t_reg_type(e, "oop", GPTPS_EXEC_OOP, NULL) == GPTPS_OK);
    CHECK(t_reg_type(e, "pok", GPTPS_EXEC_PROGRAM, ok_argv) == GPTPS_OK);
    CHECK(t_reg_type(e, "pfail", GPTPS_EXEC_PROGRAM, fail_argv) == GPTPS_OK);
    g_e = e;
    for (i = 0; i < 200 && !fails; ++i) {
        static const char *const names[3] = { "oop", "pok", "pfail" };
        unsigned char pl = (unsigned char)rnd(2);
        gptps_handle h = 0;
        g_op = i;
        switch (rnd(4)) {
        case 0: case 1:
            if (gptps_submit(e, names[rnd(3)], &pl, 1, &h) == GPTPS_OK) remember(h);
            break;
        case 2: (void)gptps_cancel(e, a_handle()); break;
        default: (void)gptps_dead_letter_drain(e, redrive_cb, e); break;
        }
        t_verify_locked(e, "proc op", 0);
    }
    gptps_shutdown(e);
    g_e = NULL;
    printf("  leg proc: %u operations; checks that saw intake %lu, running %lu, dead letters %lu\n",
           i, g_seen[0], g_seen[3], g_seen[5]);
    CHECK(g_seen[3] > 0 && g_seen[5] > 0);
}
#endif

int main(int argc, char **argv)
{
    unsigned long seed = 1;
    const char *s = getenv("GPTPS_STRESS_SEED");
    if (argc > 1) seed = strtoul(argv[1], NULL, 10);
    else if (s && *s) seed = strtoul(s, NULL, 10);
    rng = 0x9E3779B97F4A7C15ull ^ ((uint64_t)seed * 0x100000001B3ull);
    if (!rng) rng = 1;
    printf("test_counts: seed %lu\n", seed);
    fflush(stdout);

    leg("manual",   GPTPS_RUN_MANUAL,   2, 0, 100000ul, 20000ul);
    leg("threaded", GPTPS_RUN_THREADED, 3, 0,  20000ul,  5000ul);
    leg("bounded",  GPTPS_RUN_MANUAL,   2, 64, 20000ul,  5000ul);
    leg("bounded-threaded", GPTPS_RUN_THREADED, 2, 64, 10000ul, 5000ul);
    retired_leg();
#if !defined(_WIN32) && !defined(GPTPS_TEST_HAL_SIM)
    proc_leg();
#endif
    printf("test_counts: %lu checks after operations, %lu from events\n",
           g_checks - g_ev_checks, g_ev_checks);
    printf("test_counts: %s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
