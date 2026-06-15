/*
 * test_oop.c - out-of-process executor (T13). Proves the enforced path:
 *  (B) a task that ignores everything (infinite loop) is HARD-KILLED on timeout
 *      -> E_TIMEOUT. The in-process executor fundamentally cannot do this.
 *  (C) a task exceeding its OS memory cap fails (alloc denied / OOM-kill).
 */
#include "gptps.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int c_finished, c_failed, c_timeout;
static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static void reset(void) { c_finished = c_failed = c_timeout = 0; }

static void on_ev(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_FINISHED) inc(&c_finished);
    else if (ev->kind == GPTPS_EV_FAILED) { inc(&c_failed); if (ev->status == GPTPS_E_TIMEOUT) inc(&c_timeout); }
}

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

    if (fails) { printf("%d oop check(s) FAILED\n", fails); return 1; }
    printf("all oop checks passed\n");
    return 0;
}
