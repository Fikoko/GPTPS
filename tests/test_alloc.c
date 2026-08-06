/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_alloc.c - the process-wide allocator hook (gptps_set_allocator). Installs
 * a counting allocator, drives a MANUAL engine end-to-end (single-threaded, so the
 * counters need no atomics), and asserts the core routed every allocation through
 * it and balanced back to zero at shutdown. Then resets to libc and confirms the
 * hook goes quiet. Also covers the validation guards. Headless / portable.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

/* counting allocator (manual engine => single-threaded => plain counters are safe).
 * gptps_free never passes NULL (the wrapper guards it), so every cnt_free is a live
 * block. realloc keeps the block count steady (one in, one out) unless it was a
 * fresh allocation (ptr == NULL). */
static long live, allocs;
static void *cnt_malloc(size_t n, void *ud)           { void *p; (void)ud; p = malloc(n); if (p) { ++live; ++allocs; } return p; }
static void *cnt_realloc(void *p, size_t n, void *ud) { (void)ud; if (!p) { void *q = malloc(n); if (q) { ++live; ++allocs; } return q; } return realloc(p, n); }
static void  cnt_free(void *p, void *ud)              { (void)ud; free(p); --live; }

static gptps_status echo_task(gptps_ctx *ctx, void *ud)
{
    size_t n; const void *p; (void)ud;
    p = gptps_payload(ctx, &n);
    gptps_result_set(ctx, p ? p : "x", p ? n : 1);   /* forces a result copy via the hook */
    return GPTPS_OK;
}

static gptps *open_manual(void)
{
    gptps_config cfg; gptps *e = NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 2;
    cfg.limits.max_memory_bytes = 1u << 20;
    cfg.mode = GPTPS_RUN_MANUAL;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    return e;
}

static void reg_echo(gptps *e)
{
    gptps_task_def d; memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "echo"; d.run = echo_task; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

int main(void)
{
    gptps_allocator a;

    /* validation guards */
    memset(&a, 0, sizeof a); a.struct_size = sizeof a;
    CHECK(gptps_set_allocator(&a) == GPTPS_E_INVAL);          /* missing fn pointers */
    a.malloc_fn = cnt_malloc; a.realloc_fn = cnt_realloc; a.free_fn = cnt_free;
    a.struct_size = 1;
    CHECK(gptps_set_allocator(&a) == GPTPS_E_INVAL);          /* undersized struct */
    a.struct_size = sizeof a;
    CHECK(gptps_set_allocator(&a) == GPTPS_OK);               /* installed */

    /* drive an engine entirely through the custom allocator */
    {
        gptps *e; size_t n; gptps_handle h; int i;
        live = allocs = 0;
        e = open_manual();
        reg_echo(e);
        for (i = 0; i < 4; ++i) CHECK(gptps_submit(e, "echo", "payload", 7, &h) == GPTPS_OK);
        do { CHECK(gptps_step(e, &n) == GPTPS_OK); } while (n);
        CHECK(allocs > 0);             /* the core actually used our allocator */
        CHECK(live > 0);               /* engine still holds its own structures */
        gptps_shutdown(e);
        CHECK(live == 0);              /* every core allocation was freed (balanced) */
    }

    /* reset to libc: the custom hook must go quiet */
    {
        gptps *e; size_t n; gptps_handle h; long snapshot;
        CHECK(gptps_set_allocator(NULL) == GPTPS_OK);
        snapshot = allocs;
        e = open_manual();
        reg_echo(e);
        CHECK(gptps_submit(e, "echo", "p", 1, &h) == GPTPS_OK);
        do { CHECK(gptps_step(e, &n) == GPTPS_OK); } while (n);
        gptps_shutdown(e);
        CHECK(allocs == snapshot);     /* libc path used; custom allocator untouched */
    }

    if (fails) { printf("%d alloc check(s) FAILED\n", fails); return 1; }
    printf("all alloc checks passed\n");
    return 0;
}
