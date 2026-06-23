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
    void *p;
    (void)ctx; (void)ud;
    p = malloc(4ull * 1024 * 1024 * 1024); /* 4 GiB, well over the 512 MiB AS cap */
    if (!p) return GPTPS_E_TASK;
    memset(p, 1, 4ull * 1024 * 1024 * 1024);
    free(p);
    return GPTPS_OK;
}
#endif

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

    if (fails) { printf("%d oop check(s) FAILED\n", fails); return 1; }
    printf("all oop checks passed\n");
    return 0;
}
