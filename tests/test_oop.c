/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_oop.c - out-of-process executor (T13). Proves the enforced path:
 *  (B) a task that ignores everything (infinite loop) is HARD-KILLED on timeout
 *      -> E_TIMEOUT. The in-process executor fundamentally cannot do this.
 *  (C) a task exceeding its OS memory cap fails (alloc denied / OOM-kill).
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L   /* setenv / unsetenv */
#endif
#include "gptps.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>              /* nanosleep: bounded naps for case E's ordering */

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int c_finished, c_failed, c_timeout;
static char g_result[32]; static size_t g_result_len;
static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static void reset(void) { c_finished = c_failed = c_timeout = 0; g_result[0] = 0; g_result_len = 0; }

static void on_ev(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_FINISHED) {
        inc(&c_finished);
        if (ev->result && ev->result_len <= sizeof g_result) { memcpy(g_result, ev->result, ev->result_len); g_result_len = ev->result_len; }
    }
    else if (ev->kind == GPTPS_EV_FAILED) { inc(&c_failed); if (ev->status == GPTPS_E_TIMEOUT) inc(&c_timeout); }
}

/* child_setup runs in the forked OOP child before the task fn: set an env var
 * the task will read back, proving the hook fired (and in the child, not the parent). */
static void cs_setenv(void *ud) { (void)ud; setenv("GPTPS_CHILD_SETUP", "yes", 1); }
static gptps_status t_readenv(gptps_ctx *ctx, void *ud)
{ const char *v = getenv("GPTPS_CHILD_SETUP"); (void)ud; return gptps_result_set(ctx, v ? v : "", v ? strlen(v) : 0); }

static gptps_status t_ok(gptps_ctx *ctx, void *ud)   { (void)ud; return gptps_result_set(ctx, "ok", 2); }
static gptps_status t_hang(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; for (;;) { } return GPTPS_OK; /*unreachable*/ }
#if defined(__linux__) /* RLIMIT_AS-enforced memory cap is Linux-only for now */
static gptps_status t_bigmem(gptps_ctx *ctx, void *ud)
{
    /* 1 GiB: over the 512 MiB cap, and fits a 32-bit size_t (4 GiB would truncate
     * to 0 there). Triggers an RLIMIT_AS denial or a cgroup OOM-kill on memset. */
    size_t big = (size_t)1024 * 1024 * 1024;
    void *p;
    (void)ctx; (void)ud;
    p = malloc(big);
    if (!p) return GPTPS_E_TASK;
    memset(p, 1, big);
    free(p);
    return GPTPS_OK;
}
#endif

/* ---- case E: the fd-registry regression --------------------------------- *
 * The OOP child never exec()s, so FD_CLOEXEC never fires for it and it used to
 * inherit AND PIN every other executor's pipe ends for the whole OOP task. A
 * concurrent PROGRAM task's stdin write end stayed open in it, `cat` never saw
 * EOF, and that task pumped to its own deadline and reported GPTPS_E_TIMEOUT with
 * an empty result instead of the payload. Track the PROGRAM task's outcome
 * separately (by task name) - its result is far too big for g_result. */
#define RACE_LEN (400u * 1024u)          /* multi-hundred KiB: > any pipe buffer */
static unsigned char g_race[RACE_LEN];
static int           p_started, p_finished, p_failed, p_timeout;
static size_t        p_len;
static unsigned long p_sum;
static unsigned long sum_bytes(const unsigned char *b, size_t n)
{ unsigned long s = 1469598103u; size_t i; for (i = 0; i < n; ++i) s = (s ^ b[i]) * 16777619u; return s; }
static void reset_race(void)
{
    size_t i;
    p_started = p_finished = p_failed = p_timeout = 0; p_len = 0; p_sum = 0;
    for (i = 0; i < RACE_LEN; ++i) g_race[i] = (unsigned char)((i * 1103515245u + 12345u) >> 16);
}
static void on_ev_race(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (!ev->task_name || strcmp(ev->task_name, "pcat") != 0) return;   /* ignore the OOP task */
    if (ev->kind == GPTPS_EV_STARTED) inc(&p_started);
    else if (ev->kind == GPTPS_EV_FINISHED) {
        __atomic_store_n(&p_len, ev->result_len, __ATOMIC_SEQ_CST);
        __atomic_store_n(&p_sum, sum_bytes((const unsigned char *)ev->result, ev->result_len), __ATOMIC_SEQ_CST);
        inc(&p_finished);
    } else if (ev->kind == GPTPS_EV_FAILED) {
        inc(&p_failed);
        if (ev->status == GPTPS_E_TIMEOUT) inc(&p_timeout);
    }
}
static void nap_ms(unsigned ms)
{ struct timespec t; t.tv_sec = (time_t)(ms / 1000u); t.tv_nsec = (long)(ms % 1000u) * 1000000L; nanosleep(&t, NULL); }
/* bounded wait - never an unbounded spin, so a regression fails the test instead
 * of wedging ctest until its 30s timeout. */
static int wait_pstarted(unsigned ms)
{ uint64_t s = gptps_now_ms(NULL); while (get(&p_started) < 1 && gptps_now_ms(NULL) - s < ms) nap_ms(5); return get(&p_started) >= 1; }
/* Outlives the PROGRAM task's deadline: with the bug the PROGRAM task can only
 * end at that deadline, so the OOP child must still be alive when it passes. */
static gptps_status t_sleep4(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; nap_ms(4000); return GPTPS_OK; }

static void def_init(gptps_task_def *d, const char *name, gptps_run_fn run,
                     uint32_t timeout_s, uint64_t mem_bytes)
{
    memset(d, 0, sizeof *d);
    d->struct_size = sizeof *d; d->name = name; d->run = run; d->exec = GPTPS_EXEC_OOP;
    d->default_cost.struct_size = sizeof d->default_cost; d->default_cost.mem_bytes = mem_bytes;
    d->default_policy.struct_size = sizeof d->default_policy;
    d->default_policy.timeout_seconds = timeout_s;
    d->default_policy.on_failure = GPTPS_ON_FAILURE_DROP;
}

int main(void)
{
    gptps *e;
    gptps_task_def d;
    gptps_handle h;

    /* A) OOP task runs normally end-to-end */
    reset();
    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    gptps_set_event_cb(e, on_ev, NULL);
    def_init(&d, "ook", t_ok, 0, 1024 * 1024);
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(gptps_submit(e, "ook", NULL, 0, &h) == GPTPS_OK);
    gptps_shutdown(e);
    CHECK(get(&c_finished) == 1);
    CHECK(get(&c_failed) == 0);

    /* B) uncooperative task hard-killed on timeout -> E_TIMEOUT (the demo) */
    reset();
    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    gptps_set_event_cb(e, on_ev, NULL);
    def_init(&d, "hang", t_hang, 1, 1024 * 1024);
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(gptps_submit(e, "hang", NULL, 0, &h) == GPTPS_OK);
    gptps_shutdown(e);
    CHECK(get(&c_timeout) >= 1);
    CHECK(get(&c_failed) >= 1);
    CHECK(get(&c_finished) == 0);

    /* C) task exceeding its OS memory cap fails - only where the cap is
     *    enforced (RLIMIT_AS, Linux). macOS/cgroups enforcement is deferred. */
#if defined(__linux__)
    reset();
    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    gptps_set_event_cb(e, on_ev, NULL);
    def_init(&d, "big", t_bigmem, 10, 512ull * 1024 * 1024);
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(gptps_submit(e, "big", NULL, 0, &h) == GPTPS_OK);
    gptps_shutdown(e);
    CHECK(get(&c_failed) >= 1);
    CHECK(get(&c_finished) == 0);
#endif

    /* D) child_setup hook fires in the OOP child before the task fn */
    unsetenv("GPTPS_CHILD_SETUP");           /* ensure the parent env doesn't pre-seed it */
    reset();
    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    gptps_set_event_cb(e, on_ev, NULL);
    def_init(&d, "renv", t_readenv, 0, 1024 * 1024);
    d.child_setup = cs_setenv;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(gptps_submit(e, "renv", NULL, 0, &h) == GPTPS_OK);
    gptps_shutdown(e);
    CHECK(get(&c_finished) == 1);
    CHECK(g_result_len == 3 && memcmp(g_result, "yes", 3) == 0);  /* hook ran in the child before the read */

    /* E) an OOP child must not pin a concurrent PROGRAM task's pipes. Order is
     *    what makes this deterministic: submit the PROGRAM task, wait for its
     *    STARTED event (emitted immediately before the executor creates its pipes
     *    and forks), nap while the program is still inside its `sleep 0.3` - so
     *    the parent is mid-pump with the stdin write end open - and only THEN
     *    submit the OOP task, whose fork is the one that used to capture it.
     *    Needs >= 2 concurrent slots, or the two never overlap at all. */
    {
        gptps_config cfg;
        static const char *slowcat[] = { "/bin/sh", "-c", "sleep 0.3; exec cat", (const char *)0 };
        gptps_handle ph;
        reset_race();
        memset(&cfg, 0, sizeof cfg);
        cfg.struct_size = sizeof cfg;
        cfg.limits.struct_size = sizeof cfg.limits;
        cfg.limits.max_concurrent_tasks = 4;
        CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
        gptps_set_event_cb(e, on_ev_race, NULL);

        memset(&d, 0, sizeof d);
        d.struct_size = sizeof d; d.name = "pcat"; d.exec = GPTPS_EXEC_PROGRAM; d.argv = slowcat;
        d.default_cost.struct_size = sizeof d.default_cost;   /* mem 0 => no AS cap on the program */
        d.default_policy.struct_size = sizeof d.default_policy;
        d.default_policy.timeout_seconds = 3;                 /* the deadline the bug ran into */
        d.default_policy.on_failure = GPTPS_ON_FAILURE_DROP;
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        def_init(&d, "osleep", t_sleep4, 20, 1024 * 1024);
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);

        CHECK(gptps_submit(e, "pcat", g_race, RACE_LEN, &ph) == GPTPS_OK);
        CHECK(wait_pstarted(5000));
        nap_ms(150);
        CHECK(gptps_submit(e, "osleep", NULL, 0, &h) == GPTPS_OK);
        gptps_shutdown(e);
        CHECK(get(&p_finished) == 1);          /* was 0: E_TIMEOUT, empty result */
        CHECK(get(&p_timeout) == 0);
        CHECK(get(&p_failed) == 0);
        CHECK(p_len == RACE_LEN);              /* every byte round-tripped through cat */
        CHECK(p_sum == sum_bytes(g_race, RACE_LEN));
    }

    if (fails) { printf("%d oop check(s) FAILED\n", fails); return 1; }
    printf("all oop checks passed\n");
    return 0;
}
