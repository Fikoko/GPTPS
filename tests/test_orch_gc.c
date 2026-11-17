/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_orch_gc.c - gptps_orch frees a gate once it is submitted.
 *
 * A released gate used to keep its task name, payload copy and dependency array until
 * gptps_orch_close, and every terminal event walked every gate ever created. A
 * long-running host that gated a few thousand jobs a night kept every payload it ever
 * gated. Here: a thousand gates with 4 KiB payloads are released, and the
 * orchestrator must hold none of them afterwards (nor a 1000-slot array); an abandoned
 * gate (a cost that can never fit) stays - stalled_at still reports it - until
 * retried or discarded.
 *
 * The add-on is compiled into this file so the test can read its gate table; it links
 * the core only. MANUAL mode: nothing runs unless this thread steps it. Portable.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "../addons/gptps_orch.c"
#include <stdio.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int g_runs;
static gptps_status task_count(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; ++g_runs; return GPTPS_OK; }

static void def(gptps *e, const char *name, uint64_t mem)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = task_count; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = mem;
    d.default_policy.struct_size = sizeof d.default_policy;
    gptps_register_task(e, &d);
}

static size_t gates_held(gptps_orch *o)
{ size_t n; apx_mutex_lock(&o->mu); n = o->ng; apx_mutex_unlock(&o->mu); return n; }

static void step_all(gptps *e)
{ size_t ran = 1; while (ran) { ran = 0; if (gptps_step(e, &ran) != GPTPS_OK) break; } }

int main(void)
{
    gptps_config cfg; gptps *e = NULL; gptps_orch *o;
    gptps_handle dep = 0, h = 0;
    static unsigned char payload[4096];
    char name[32]; gptps_status why = GPTPS_OK;
    int i;

    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1; cfg.limits.max_memory_bytes = 64u << 20;
    cfg.mode = GPTPS_RUN_MANUAL;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (!e) return 1;
    def(e, "work", 0);
    def(e, "huge", (uint64_t)1 << 30);   /* never fits the 64 MiB budget: refused for good */
    o = gptps_orch_install(e);
    CHECK(o != NULL);
    if (!o) { gptps_shutdown(e); return 1; }

    /* 1) a thousand gates on one dependency, all released at once */
    CHECK(gptps_submit(e, "work", NULL, 0, &dep) == GPTPS_OK);
    for (i = 0; i < 1000; ++i)
        CHECK(gptps_orch_after(o, "work", payload, sizeof payload, &dep, 1, &h) == GPTPS_OK);
    CHECK(gates_held(o) == 1000);
    CHECK(gptps_orch_pending(o) == 1000);
    step_all(e);
    CHECK(g_runs == 1001);
    CHECK(gptps_orch_pending(o) == 0);
    CHECK(gates_held(o) == 0);              /* was 1000, each with its 4 KiB payload */
    CHECK(o->gcap <= 16);                   /* and the array was given back */

    /* 2) an abandoned gate stays, reportable, until discarded */
    CHECK(gptps_submit(e, "work", NULL, 0, &dep) == GPTPS_OK);
    CHECK(gptps_orch_after(o, "huge", payload, 8, &dep, 1, &h) == GPTPS_OK);
    CHECK(gptps_orch_after(o, "work", payload, 8, &dep, 1, &h) == GPTPS_OK);
    step_all(e);
    CHECK(gptps_orch_pending(o) == 0);
    CHECK(gptps_orch_stalled(o) == 1);
    CHECK(gates_held(o) == 1);              /* the abandoned one only */
    CHECK(gptps_orch_stalled_at(o, 0, name, sizeof name, &why) == GPTPS_OK);
    CHECK(strcmp(name, "huge") == 0 && why == GPTPS_E_BUDGET);
    CHECK(gptps_orch_retry(o) == 0);        /* refused again: still abandoned, still held */
    CHECK(gates_held(o) == 1 && gptps_orch_stalled(o) == 1);
    CHECK(gptps_settings_set(e, "tasks.huge.mem_bytes", "1") == GPTPS_OK);
    CHECK(gptps_orch_retry(o) == 1);        /* submitted now - and freed */
    CHECK(gates_held(o) == 0 && gptps_orch_stalled(o) == 0);
    step_all(e);

    /* 3) discarding abandoned gates frees them unrun */
    CHECK(gptps_submit(e, "work", NULL, 0, &dep) == GPTPS_OK);
    CHECK(gptps_settings_set(e, "tasks.huge.mem_bytes", "1073741824") == GPTPS_OK);
    CHECK(gptps_orch_after(o, "huge", payload, 8, &dep, 1, &h) == GPTPS_OK);
    step_all(e);
    CHECK(gptps_orch_stalled(o) == 1);
    CHECK(gptps_orch_discard_stalled(o) == 1);
    CHECK(gptps_orch_stalled(o) == 0 && gates_held(o) == 0);
    CHECK(gptps_orch_discard_stalled(NULL) == 0);

    gptps_shutdown(e);
    gptps_orch_close(o);
    if (fails) { printf("%d orch gc check(s) FAILED\n", fails); return 1; }
    printf("all orch gc checks passed\n");
    return 0;
}
