/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_shutdown_dl.c - ABI 2.11: gptps_shutdown_ex hands the host every dead letter
 * the engine still retains, the ones its own grace made included, before the engine
 * is freed; gptps_dead_letter.attempts_run counts the attempts that ran.
 *
 *  1) attempts vs attempts_run through an ordinary drain: a type retried once and
 *     failing twice reports 2 / 2; a retry a constraint refuses before it runs (the
 *     shape of gptps_retry's NEVER rule) reports 2 / 1.
 *  2) gptps_shutdown_ex with a callback, on a MANUAL engine that stopped stepping:
 *     a dead letter never drained (E_DENIED) comes first, then the ones shutdown made
 *     (GPTPS_E_SHUTDOWN): two queued items (attempts 1, attempts_run 0) and one waiting
 *     an hour to retry (2 / 1) - each with its payload and submit options, each once.
 *  3) inside that callback: gptps_submit is GPTPS_E_SHUTDOWN, gptps_shutdown and
 *     gptps_shutdown_ex are GPTPS_E_BUSY.
 *  4) a struct_size below the 2.11 struct is GPTPS_E_INVAL and frees nothing (the
 *     engine still answers); opt NULL and a NULL callback are plain gptps_shutdown.
 *  5) a THREADED engine: attempts_run counts the worker's runs (2 of 2), and work
 *     its grace ends queued behind a busy slot reaches the callback, GPTPS_E_SHUTDOWN,
 *     payload intact.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "gptps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#  include <windows.h>
#endif

static void sleep_ms(unsigned ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u); ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) != 0) { }
#endif
}

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static gptps_status run_fail(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_E_TASK; }
static gptps_status run_ok(gptps_ctx *c, void *u)   { (void)c; (void)u; return GPTPS_OK; }

/* a long body for 5): polls its cancel flag. slow_started crosses threads: atomics,
 * as tests/test_hang.c does */
static int slow_started;
static gptps_status run_slow(gptps_ctx *c, void *u)
{
    (void)u;
    __atomic_store_n(&slow_started, 1, __ATOMIC_SEQ_CST);
    while (!gptps_is_cancelled(c)) sleep_ms(5);
    return GPTPS_E_CANCELLED;
}

/* the constraint: refuse an item of type "never" once it has failed (as gptps_retry's
 * NEVER verdict does, at the retry's admission) */
static gptps_handle failed_h[16];
static int nfailed;
static void observe(const gptps_event *ev, void *u)
{
    (void)u;
    if (ev->kind == GPTPS_EV_FAILED && nfailed < 16) failed_h[nfailed++] = ev->handle;
}
static gptps_admit_decision deny_after_fail(const gptps_constraint_input *in, uint32_t *ra, void *u)
{
    int i;
    (void)ra; (void)u;
    if (strcmp(in->task_name, "never") != 0) return GPTPS_ADMIT;
    for (i = 0; i < nfailed; ++i) if (failed_h[i] == in->handle) return GPTPS_DENY;
    return GPTPS_ADMIT;
}

static gptps *open_engine(gptps_run_mode mode, unsigned grace_ms)
{
    gptps_config cfg;
    gptps *e = NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1; cfg.limits.max_memory_bytes = 1u << 20;
    cfg.shutdown_grace_ms = grace_ms;
    cfg.mode = mode;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    return e;
}
static void reg(gptps *e, const char *name, gptps_run_fn fn, uint32_t retries, uint32_t backoff_s)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = fn; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = retries;
    d.default_policy.retry_backoff_seconds = backoff_s;
    d.default_policy.on_failure = GPTPS_ON_FAILURE_DEAD_LETTER;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}
static void pump(gptps *e)
{
    size_t ran = 1;
    int k;
    for (k = 0; k < 100 && ran; ++k) CHECK(gptps_step(e, &ran) == GPTPS_OK);
}

/* what a callback saw */
typedef struct {
    char        name[32];
    char        payload[32];
    gptps_status status;
    uint32_t    attempts, runs, flags;
    int32_t     priority;
} seen_dl;
static seen_dl seen[16];
static int nseen;
static gptps *cb_engine;            /* for 3) */
static gptps_status in_cb_submit = GPTPS_OK, in_cb_shut = GPTPS_OK, in_cb_shut_ex = GPTPS_OK;
static void collect(const gptps_dead_letter *dl, void *u)
{
    seen_dl *s;
    (void)u;
    if (nseen >= 16) return;
    s = &seen[nseen++];
    snprintf(s->name, sizeof s->name, "%s", dl->task_name ? dl->task_name : "?");
    snprintf(s->payload, sizeof s->payload, "%.*s", (int)(dl->payload_len < 31 ? dl->payload_len : 31),
             dl->payload ? (const char *)dl->payload : "");
    s->status = dl->status; s->attempts = dl->attempts; s->runs = dl->attempts_run;
    s->flags = dl->options.flags; s->priority = dl->options.priority;
    if (cb_engine && nseen == 1) {   /* 3): once, from inside the callback */
        gptps_shutdown_options so;
        gptps_handle h;
        in_cb_submit = gptps_submit(cb_engine, "ok", "x", 1, &h);
        in_cb_shut = gptps_shutdown(cb_engine);
        memset(&so, 0, sizeof so); so.struct_size = sizeof so;
        in_cb_shut_ex = gptps_shutdown_ex(cb_engine, &so);
    }
}
static const seen_dl *find(const char *payload)
{
    int i;
    for (i = 0; i < nseen; ++i) if (strcmp(seen[i].payload, payload) == 0) return &seen[i];
    return NULL;
}
static int count_of(const char *payload)
{
    int i, n = 0;
    for (i = 0; i < nseen; ++i) n += strcmp(seen[i].payload, payload) == 0;
    return n;
}

/* 1) */
static void test_attempts_run(void)
{
    gptps *e = open_engine(GPTPS_RUN_MANUAL, 0);
    gptps_handle h;
    const seen_dl *s;
    if (!e) return;
    nfailed = 0; nseen = 0;
    CHECK(gptps_register_observer(e, observe, NULL) == GPTPS_OK);
    CHECK(gptps_register_constraint(e, deny_after_fail, NULL) == GPTPS_OK);
    reg(e, "twice", run_fail, 1, 0);
    reg(e, "never", run_fail, 2, 0);
    CHECK(gptps_submit(e, "twice", "T", 1, &h) == GPTPS_OK);
    CHECK(gptps_submit(e, "never", "N", 1, &h) == GPTPS_OK);
    pump(e);
    CHECK(gptps_dead_letter_drain(e, collect, NULL) == 2);
    CHECK((s = find("T")) != NULL && s->attempts == 2 && s->runs == 2 && s->status == GPTPS_E_TASK);
    CHECK((s = find("N")) != NULL && s->attempts == 2 && s->runs == 1 && s->status == GPTPS_E_DENIED);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

/* 2), 3), 4) */
static void test_shutdown_ex(void)
{
    gptps *e = open_engine(GPTPS_RUN_MANUAL, 0);
    gptps_handle h;
    gptps_submit_options o;
    gptps_shutdown_options so;
    const seen_dl *s;
    if (!e) return;
    nfailed = 0; nseen = 0;
    CHECK(gptps_register_observer(e, observe, NULL) == GPTPS_OK);
    CHECK(gptps_register_constraint(e, deny_after_fail, NULL) == GPTPS_OK);
    reg(e, "never", run_fail, 2, 0);
    reg(e, "later", run_fail, 1, 3600);          /* its retry waits an hour */
    reg(e, "ok", run_ok, 0, 0);
    CHECK(gptps_submit(e, "never", "N", 1, &h) == GPTPS_OK);
    CHECK(gptps_submit(e, "later", "W", 1, &h) == GPTPS_OK);
    pump(e);                                      /* N dead-lettered; W failed once, waits */
    CHECK(gptps_dead_letter_count(e) == 1);
    memset(&o, 0, sizeof o); o.struct_size = sizeof o;
    o.flags = GPTPS_SUBMIT_PRIORITY; o.priority = 7;
    CHECK(gptps_submit_ex(e, "ok", "Q1", 2, &o, &h) == GPTPS_OK);   /* queued, never stepped */
    CHECK(gptps_submit(e, "ok", "Q2", 2, &h) == GPTPS_OK);

    /* 4) too small: refused, nothing freed - the engine still answers */
    memset(&so, 0, sizeof so);
    so.struct_size = sizeof(size_t); so.dead_letter = collect;
    CHECK(gptps_shutdown_ex(e, &so) == GPTPS_E_INVAL);
    CHECK(gptps_dead_letter_count(e) == 1 && nseen == 0);

    /* 2) + 3) */
    so.struct_size = sizeof so;
    cb_engine = e;
    CHECK(gptps_shutdown_ex(e, &so) == GPTPS_OK);
    cb_engine = NULL;
    CHECK(nseen == 4);
    CHECK(strcmp(seen[0].payload, "N") == 0);     /* the one retained before: oldest first */
    CHECK((s = find("N")) != NULL && s->status == GPTPS_E_DENIED && s->attempts == 2 && s->runs == 1 &&
          strcmp(s->name, "never") == 0);
    CHECK((s = find("W")) != NULL && s->status == GPTPS_E_SHUTDOWN && s->attempts == 2 && s->runs == 1 &&
          strcmp(s->name, "later") == 0);
    CHECK((s = find("Q1")) != NULL && s->status == GPTPS_E_SHUTDOWN && s->attempts == 1 && s->runs == 0 &&
          (s->flags & GPTPS_SUBMIT_PRIORITY) && s->priority == 7 && strcmp(s->name, "ok") == 0);
    CHECK((s = find("Q2")) != NULL && s->status == GPTPS_E_SHUTDOWN && s->attempts == 1 && s->runs == 0);
    CHECK(count_of("N") == 1 && count_of("W") == 1 && count_of("Q1") == 1 && count_of("Q2") == 1);
    CHECK(in_cb_submit == GPTPS_E_SHUTDOWN && in_cb_shut == GPTPS_E_BUSY && in_cb_shut_ex == GPTPS_E_BUSY);

    /* 4) NULL options, and options without a callback: plain shutdown */
    e = open_engine(GPTPS_RUN_MANUAL, 0);
    if (!e) return;
    reg(e, "ok", run_ok, 0, 0);
    CHECK(gptps_submit(e, "ok", "Z", 1, &h) == GPTPS_OK);
    CHECK(gptps_shutdown_ex(e, NULL) == GPTPS_OK);
    e = open_engine(GPTPS_RUN_MANUAL, 0);
    if (!e) return;
    reg(e, "ok", run_ok, 0, 0);
    CHECK(gptps_submit(e, "ok", "Z", 1, &h) == GPTPS_OK);
    memset(&so, 0, sizeof so); so.struct_size = sizeof so;
    nseen = 0;
    CHECK(gptps_shutdown_ex(e, &so) == GPTPS_OK && nseen == 0);
}

/* 5) */
static void test_threaded(void)
{
    gptps *e = open_engine(GPTPS_RUN_THREADED, 200);   /* 200 ms of grace */
    gptps_handle h;
    gptps_shutdown_options so;
    const seen_dl *s;
    int k;
    if (!e) return;
    nseen = 0; __atomic_store_n(&slow_started, 0, __ATOMIC_SEQ_CST);
    reg(e, "slow", run_slow, 0, 0);
    reg(e, "ok", run_ok, 0, 0);
    reg(e, "twice", run_fail, 1, 0);
    /* attempts_run on the worker path: run, fail, retried, run, fail */
    CHECK(gptps_submit(e, "twice", "T2", 2, &h) == GPTPS_OK);
    for (k = 0; k < 5000 && gptps_dead_letter_count(e) == 0; ++k) sleep_ms(1);
    CHECK(gptps_dead_letter_drain(e, collect, NULL) == 1);
    CHECK((s = find("T2")) != NULL && s->attempts == 2 && s->runs == 2 && s->status == GPTPS_E_TASK);
    nseen = 0;
    CHECK(gptps_submit(e, "slow", "S", 1, &h) == GPTPS_OK);
    for (k = 0; k < 2000 && !__atomic_load_n(&slow_started, __ATOMIC_SEQ_CST); ++k) sleep_ms(1);
    CHECK(__atomic_load_n(&slow_started, __ATOMIC_SEQ_CST));
    CHECK(gptps_submit(e, "ok", "B1", 2, &h) == GPTPS_OK);   /* behind the one slot */
    CHECK(gptps_submit(e, "ok", "B2", 2, &h) == GPTPS_OK);
    memset(&so, 0, sizeof so); so.struct_size = sizeof so; so.dead_letter = collect;
    CHECK(gptps_shutdown_ex(e, &so) == GPTPS_OK);
    CHECK((s = find("B1")) != NULL && s->status == GPTPS_E_SHUTDOWN && s->runs == 0);
    CHECK((s = find("B2")) != NULL && s->status == GPTPS_E_SHUTDOWN && s->runs == 0);
    CHECK(find("S") == NULL);                    /* cancelled while running: not a dead letter */
}

int main(void)
{
    test_attempts_run();
    test_shutdown_ex();
    test_threaded();
    if (fails) { printf("test_shutdown_dl: %d FAILED\n", fails); return 1; }
    printf("test_shutdown_dl: OK\n");
    return 0;
}
