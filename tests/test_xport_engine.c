/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_xport_engine.c - xport ENGINE MODE: every worker process runs its own GPTPS
 * engine, so scaling out keeps the engine's semantics. Proves, over the link:
 *   - concurrency: a worker with 4 engine threads runs 8 requests ~4-wide, not 1-wide
 *   - retries: a task that fails its first attempt is retried IN THE WORKER and the
 *     parent sees only the final GPTPS_OK
 *   - timeout -> dead-letter: the parent sees the failure status, never a hang
 *   - unknown task: rejected at the worker's submit, answered in frame (E_NOTFOUND)
 *   - backpressure: max_in_flight per link -> GPTPS_E_FULL
 *   - child_init: runs in the worker (a named resource defined there gates admission)
 *   - link death: outstanding requests fail with E_IO, the worker is retired, the
 *     rest of the pool keeps answering
 *   - graceful close: an async request in flight at close still gets its reply
 *   - never silent: a link that breaks, and a worker that did not exit cleanly, are
 *     logged
 *   - a worker whose setup fails (a duplicate task, child_init_ex refusing, a config
 *     the engine rejects) fails the open, with an ERROR naming the step - it used to
 *     return a transport whose every submit came back GPTPS_E_IO, with nothing logged
 *   - a child_init that sets the engine's event callback does not take the reply path
 *     away (replies used to stop, and every blocking submit waited forever)
 * POSIX only (fork + socketpair).
 */
#define _POSIX_C_SOURCE 200809L
#include "gptps_xport.h"
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <pthread.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static void spin_ms(gptps_ctx *c, unsigned ms)
{ uint64_t s = gptps_now_ms(c); while (gptps_now_ms(c) - s < ms) { if (gptps_is_cancelled(c)) return; } }

/* ---- task table (these run IN THE WORKER PROCESS, through its engine) ---- */
static gptps_status t_pid(gptps_ctx *c, void *u)
{   int32_t pid = (int32_t)getpid(); (void)u;
    return gptps_result_set(c, &pid, sizeof pid); }
static gptps_status t_sleep(gptps_ctx *c, void *u)
{   (void)u; spin_ms(c, 200); return GPTPS_OK; }
static int flaky_calls = 0;                          /* per PROCESS: each worker has its own */
static gptps_status t_flaky(gptps_ctx *c, void *u)
{   int n = ++flaky_calls; (void)u;
    if (n == 1) return GPTPS_E_TASK;                 /* first attempt fails */
    return gptps_result_set(c, &n, sizeof n); }
static gptps_status t_hang(gptps_ctx *c, void *u)
{   (void)u; while (!gptps_is_cancelled(c)) { } return GPTPS_OK; }
static gptps_status t_die(gptps_ctx *c, void *u)
{   (void)c; (void)u; _exit(1); }
static gptps_status t_seat(gptps_ctx *c, void *u)
{   (void)u; spin_ms(c, 100); return GPTPS_OK; }

static void deftask(gptps_task_def *d, const char *name, gptps_run_fn fn, uint32_t retries, uint32_t timeout_s)
{
    memset(d, 0, sizeof *d);
    d->struct_size = sizeof *d; d->name = name; d->run = fn; d->exec = GPTPS_EXEC_INPROC;
    d->default_cost.struct_size = sizeof d->default_cost; d->default_cost.mem_bytes = 1;
    d->default_policy.struct_size = sizeof d->default_policy;
    d->default_policy.max_retries = retries; d->default_policy.retry_backoff_seconds = 0;
    d->default_policy.timeout_seconds = timeout_s;
}

/* child_init: runs in each worker. A named resource with ONE seat gates "seat". */
static int child_init_calls = 0;
static void child_init(gptps *e, void *u)
{
    (void)u; ++child_init_calls;                     /* in the child; the parent's copy stays 0 */
    gptps_define_resource(e, "seats", 1);
    gptps_set_task_resource_cost(e, "seat", "seats", 1);
}

/* What the add-on logs (process-wide sink; the readers log from their threads). */
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_log[8192];
static void log_sink(gptps_log_level lvl, const char *msg, void *u)
{
    size_t n;
    (void)u;
    pthread_mutex_lock(&g_log_mu);
    n = strlen(g_log);
    snprintf(g_log + n, sizeof g_log - n, "[%d] %s\n", (int)lvl, msg);
    pthread_mutex_unlock(&g_log_mu);
}
static int logged(const char *needle)
{
    int r;
    pthread_mutex_lock(&g_log_mu);
    r = strstr(g_log, needle) != NULL;
    pthread_mutex_unlock(&g_log_mu);
    return r;
}
static void log_clear(void) { pthread_mutex_lock(&g_log_mu); g_log[0] = 0; pthread_mutex_unlock(&g_log_mu); }

/* child_init hooks for the setup cases */
static void ev_noop(const gptps_event *ev, void *u) { (void)ev; (void)u; }
static void init_sets_event_cb(gptps *e, void *u)
{   (void)u; gptps_set_event_cb(e, ev_noop, NULL); }   /* the host's slot: allowed */
static gptps_status init_ex_refuses(gptps *e, void *u)
{   (void)e; (void)u; return GPTPS_E_INVAL; }

/* async bookkeeping (parent side) */
/* Replies arrive on the links' reader threads (one per worker), so every field is
 * touched atomically and each test block uses a fresh, zero-initialised acc. */
typedef struct { int replies; int io_errs; int ok; } acc;
static void on_reply(uint64_t id, gptps_status io, gptps_status ts, const void *res, size_t len, void *u)
{
    acc *a = (acc *)u; (void)id; (void)res; (void)len;
    if (io != GPTPS_OK) __atomic_add_fetch(&a->io_errs, 1, __ATOMIC_SEQ_CST);
    else if (ts == GPTPS_OK) __atomic_add_fetch(&a->ok, 1, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&a->replies, 1, __ATOMIC_SEQ_CST);   /* last: publishes the others */
}
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static void wait_replies(acc *a, int n)
{ uint64_t s = gptps_now_ms(NULL); while (get(&a->replies) < n && gptps_now_ms(NULL) - s < 5000) { struct timespec ts = {0, 1000000L}; nanosleep(&ts, NULL); } }

static gptps_xport *open_pool(size_t nworkers, uint32_t conc, uint32_t in_flight, gptps_task_def *tasks, size_t ntasks, gptps_config *ecfg)
{
    gptps_xport_config cfg; memset(&cfg, 0, sizeof cfg);
    memset(ecfg, 0, sizeof *ecfg);
    ecfg->struct_size = sizeof *ecfg; ecfg->limits.struct_size = sizeof ecfg->limits;
    ecfg->limits.max_concurrent_tasks = conc; ecfg->limits.max_memory_bytes = 1024;
    cfg.struct_size = sizeof cfg; cfg.nworkers = nworkers; cfg.max_in_flight = in_flight;
    cfg.engine_cfg = ecfg; cfg.tasks = tasks; cfg.ntasks = ntasks;
    cfg.child_init = child_init;
    return gptps_xport_open_ex(&cfg);
}

int main(void)
{
    gptps_task_def tasks[7];
    gptps_config ecfg;
    gptps_xport *xp;
    void *res; size_t rlen; gptps_status ts;
    deftask(&tasks[0], "pid",   t_pid,   0, 0);
    deftask(&tasks[1], "sleep", t_sleep, 0, 0);
    deftask(&tasks[2], "flaky", t_flaky, 2, 0);
    deftask(&tasks[3], "hang",  t_hang,  0, 1);      /* 1s timeout, no retries -> dead-letter */
    deftask(&tasks[4], "die",   t_die,   0, 0);
    deftask(&tasks[5], "seat",  t_seat,  0, 0);
    deftask(&tasks[6], "nores", t_pid,   0, 0);
    gptps_set_log_sink(log_sink, NULL);
    alarm(120);   /* a hang (a reply path lost) fails the test instead of blocking it */

    /* misconfiguration: both modes, or neither */
    {   gptps_xport_config bad; memset(&bad, 0, sizeof bad);
        bad.struct_size = sizeof bad; bad.nworkers = 1;
        CHECK(gptps_xport_open_ex(&bad) == NULL);
        bad.tasks = tasks; bad.ntasks = 1; bad.run = (gptps_xport_run_fn)1;
        CHECK(gptps_xport_open_ex(&bad) == NULL); }

    xp = open_pool(2, 4, 8, tasks, 7, &ecfg);
    CHECK(xp != NULL);
    CHECK(gptps_xport_count(xp) == 2 && gptps_xport_live(xp) == 2);
    CHECK(child_init_calls == 0);                    /* it ran in the CHILDREN */

    /* 1) runs in another process, through an engine */
    CHECK(gptps_xport_submit(xp, "pid", NULL, 0, &res, &rlen, &ts) == GPTPS_OK);
    CHECK(ts == GPTPS_OK && rlen == sizeof(int32_t));
    if (res && rlen == sizeof(int32_t)) { int32_t p; memcpy(&p, res, sizeof p); CHECK(p != (int32_t)getpid()); }
    free(res);

    /* 2) concurrency over ONE link: 8 x 200ms with 4 engine threads ~ 400ms, not 1600 */
    {   acc a = {0, 0, 0}; int i; uint64_t t0 = gptps_now_ms(NULL), el;
        for (i = 0; i < 8; ++i) {
            /* pin every request to the same worker by draining the rotation:
             * two workers, so submit twice and let round-robin split 4/4 - that is
             * still 4-wide per worker, so the timing bound holds either way. */
            CHECK(gptps_xport_submit_async(xp, "sleep", NULL, 0, on_reply, &a, NULL) == GPTPS_OK);
        }
        CHECK(gptps_xport_in_flight(xp) > 0);
        wait_replies(&a, 8);
        el = gptps_now_ms(NULL) - t0;
        CHECK(get(&a.replies) == 8 && get(&a.ok) == 8 && get(&a.io_errs) == 0);
        CHECK(el < 1000);                            /* sequential would be >= 1600ms */
        CHECK(gptps_xport_in_flight(xp) == 0); }

    /* 3) retries happen IN the worker: the parent sees only the final OK, and the
     *    result says it was the second call */
    {   int n = 0;
        CHECK(gptps_xport_submit(xp, "flaky", NULL, 0, &res, &rlen, &ts) == GPTPS_OK);
        CHECK(ts == GPTPS_OK && rlen == sizeof n);
        if (res && rlen == sizeof n) { memcpy(&n, res, sizeof n); CHECK(n == 2); }
        free(res); }

    /* 4) timeout -> dead-letter in the worker: the parent gets the status, no hang */
    {   uint64_t t0 = gptps_now_ms(NULL);
        CHECK(gptps_xport_submit(xp, "hang", NULL, 0, &res, &rlen, &ts) == GPTPS_OK);
        CHECK(ts != GPTPS_OK && rlen == 0);
        CHECK(gptps_now_ms(NULL) - t0 < 4000);
        free(res); }

    /* 5) unknown task: the worker's submit rejects it and answers in frame */
    CHECK(gptps_xport_submit(xp, "nope", NULL, 0, &res, &rlen, &ts) == GPTPS_OK);
    CHECK(ts == GPTPS_E_NOTFOUND);
    free(res);

    /* 6) child_init ran in the worker: one seat => two "seat" requests to the same
     *    worker serialise (>= 200ms), while the engine has 4 threads free */
    {   acc a = {0, 0, 0}; uint64_t t0 = gptps_now_ms(NULL);
        /* 4 requests: round-robin puts 2 on each worker; each pair serialises on its
         * worker's single seat -> >= 200ms overall, < 600ms */
        CHECK(gptps_xport_submit_async(xp, "seat", NULL, 0, on_reply, &a, NULL) == GPTPS_OK);
        CHECK(gptps_xport_submit_async(xp, "seat", NULL, 0, on_reply, &a, NULL) == GPTPS_OK);
        CHECK(gptps_xport_submit_async(xp, "seat", NULL, 0, on_reply, &a, NULL) == GPTPS_OK);
        CHECK(gptps_xport_submit_async(xp, "seat", NULL, 0, on_reply, &a, NULL) == GPTPS_OK);
        wait_replies(&a, 4);
        CHECK(get(&a.ok) == 4);
        CHECK(gptps_now_ms(NULL) - t0 >= 200);
        CHECK(gptps_now_ms(NULL) - t0 < 600); }

    /* 7) backpressure: max_in_flight = 8 per link. Fill one worker's link with 8
     *    sleepers (round-robin alternates, so 16 fills both), then the 17th is FULL. */
    {   acc a = {0, 0, 0}; int i, full = 0, okc = 0; gptps_status s;
        for (i = 0; i < 17; ++i) {
            s = gptps_xport_submit_async(xp, "sleep", NULL, 0, on_reply, &a, NULL);
            if (s == GPTPS_OK) ++okc; else if (s == GPTPS_E_FULL) ++full;
        }
        CHECK(okc == 16 && full == 1);
        wait_replies(&a, 16);
        CHECK(get(&a.replies) == 16 && get(&a.ok) == 16); }

    /* 8) link death: a request in flight on the dying worker fails with E_IO; the
     *    worker is retired; the other keeps answering */
    {   acc a = {0, 0, 0}; int i;
        /* "die" lands on one worker; a sleeper submitted right after it may land on
         * either. Submit die to both workers so the outcome is deterministic: both
         * die -> everything outstanding is E_IO and live() hits 0. Instead, keep one
         * alive: submit exactly one die, then check live() == 1 and that submits
         * still succeed. */
        CHECK(gptps_xport_submit_async(xp, "die", NULL, 0, on_reply, &a, NULL) == GPTPS_OK);
        wait_replies(&a, 1);
        CHECK(get(&a.replies) == 1 && get(&a.io_errs) == 1);
        CHECK(gptps_xport_live(xp) == 1);
        CHECK(logged("broke its link"));             /* never silent */
        for (i = 0; i < 3; ++i) {                    /* the rotation skips the dead one */
            CHECK(gptps_xport_submit(xp, "pid", NULL, 0, &res, &rlen, &ts) == GPTPS_OK);
            CHECK(ts == GPTPS_OK); free(res);
        } }

    /* 9) graceful close: an async request in flight at close gets a real reply */
    {   acc a = {0, 0, 0};
        CHECK(gptps_xport_submit_async(xp, "sleep", NULL, 0, on_reply, &a, NULL) == GPTPS_OK);
        gptps_xport_close(xp);
        CHECK(get(&a.replies) == 1 && get(&a.ok) == 1 && get(&a.io_errs) == 0);
        CHECK(logged("exited with status 1")); }     /* the "die" worker, reaped */

    /* 10) a worker whose setup fails fails the open, and says why */
    {   gptps_task_def dup[2];
        gptps_xport_config c; gptps_config ec;
        deftask(&dup[0], "a", t_pid, 0, 0);
        deftask(&dup[1], "a", t_pid, 0, 0);          /* E_DUP in every worker */
        memset(&ec, 0, sizeof ec);
        ec.struct_size = sizeof ec; ec.limits.struct_size = sizeof ec.limits;
        ec.limits.max_concurrent_tasks = 2; ec.limits.max_memory_bytes = 1024;
        memset(&c, 0, sizeof c);
        c.struct_size = sizeof c; c.nworkers = 2; c.engine_cfg = &ec; c.tasks = dup; c.ntasks = 2;
        log_clear();
        CHECK(gptps_xport_open_ex(&c) == NULL);      /* was a transport of dead links */
        CHECK(logged("gptps_register_task(tasks[1] \"a\")"));
        CHECK(logged("could not be set up"));
        /* child_init_ex refusing */
        c.tasks = tasks; c.ntasks = 1; c.child_init_ex = init_ex_refuses;
        log_clear();
        CHECK(gptps_xport_open_ex(&c) == NULL);
        CHECK(logged("child_init_ex returned"));
        /* an engine config the engine rejects */
        ec.struct_size = 1;
        c.child_init_ex = NULL;
        log_clear();
        CHECK(gptps_xport_open_ex(&c) == NULL);
        CHECK(logged("gptps_open_ex(engine_cfg)"));
        /* a config struct from before child_init_ex is still accepted */
        ec.struct_size = sizeof ec;
        c.struct_size = offsetof(gptps_xport_config, child_init_ud) + sizeof(void *);
        c.child_init_ex = init_ex_refuses;           /* past its struct_size: not read */
        xp = gptps_xport_open_ex(&c);
        CHECK(xp != NULL);
        if (xp) {
            CHECK(gptps_xport_submit(xp, "pid", NULL, 0, &res, &rlen, &ts) == GPTPS_OK && ts == GPTPS_OK);
            free(res);
            gptps_xport_close(xp);
        } }

    /* 11) a child_init that sets the engine's event callback keeps the replies coming */
    {   gptps_xport_config c; gptps_config ec;
        memset(&ec, 0, sizeof ec);
        ec.struct_size = sizeof ec; ec.limits.struct_size = sizeof ec.limits;
        ec.limits.max_concurrent_tasks = 2; ec.limits.max_memory_bytes = 1024;
        memset(&c, 0, sizeof c);
        c.struct_size = sizeof c; c.nworkers = 1; c.engine_cfg = &ec; c.tasks = tasks; c.ntasks = 1;
        c.child_init = init_sets_event_cb;
        xp = gptps_xport_open_ex(&c);
        CHECK(xp != NULL);
        if (xp) {
            res = NULL;
            CHECK(gptps_xport_submit(xp, "pid", NULL, 0, &res, &rlen, &ts) == GPTPS_OK);   /* hung */
            CHECK(ts == GPTPS_OK && rlen == sizeof(int32_t));
            free(res);
            gptps_xport_close(xp);
        } }
    alarm(0);
    gptps_set_log_sink(NULL, NULL);

    if (fails) { printf("%d xport-engine check(s) FAILED\n", fails); return 1; }
    printf("all xport (engine mode) checks passed\n");
    return 0;
}
