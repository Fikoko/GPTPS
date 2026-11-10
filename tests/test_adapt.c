/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_adapt.c - addons/gptps_adapt: declarations from measured peaks.
 *
 *  1) No suggestion before min_samples measured attempts (apply says GPTPS_E_BUSY);
 *     then largest peak + headroom, rounded up to a MiB, and apply writes it to
 *     tasks.<t>.mem_bytes.
 *  2) A job that hits its cap is grown from the declaration it hit, at once.
 *  3) auto_apply writes the suggestion without being asked.
 *  4) An in-process task is never adapted: it has no measurements.
 *
 * The jobs declare 8 MiB: below the executor's 16 MiB cap floor, so no RLIMIT_AS
 * meets a sanitizer runtime's address-space reservations, as in test_measure. POSIX
 * only (GPTPS_EXEC_OOP).
 */
#define _POSIX_C_SOURCE 200809L
#include "gptps.h"
#include "gptps_adapt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

#define MiB (1024ull * 1024ull)

static gptps_status take40(gptps_ctx *ctx, void *ud)
{
    size_t n = (size_t)(40 * MiB);
    volatile char *p = (volatile char *)malloc(n);
    size_t i;
    (void)ctx; (void)ud;
    if (!p) return GPTPS_E_NOMEM;
    for (i = 0; i < n; i += 4096) p[i] = 1;     /* resident, not just reserved */
    free((void *)p);
    return GPTPS_OK;
}
static gptps_status oom(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_E_NOMEM; }
static gptps_status quick(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_OK; }

static gptps *open_manual(void)
{
    gptps_config cfg; gptps *e = NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;
    cfg.limits.max_memory_bytes = 64ull * 1024 * MiB;  /* room for whatever is suggested */
    cfg.mode = GPTPS_RUN_MANUAL;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    return e;
}

static void reg(gptps *e, const char *name, gptps_run_fn fn, gptps_exec_kind k)
{
    gptps_task_def d; memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = fn; d.exec = k;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = 8 * MiB;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.timeout_seconds = 30;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

static void run(gptps *e, const char *task, int n)
{
    size_t ran; int i;
    for (i = 0; i < n; ++i) CHECK(gptps_submit(e, task, NULL, 0, NULL) == GPTPS_OK);
    while (gptps_step(e, &ran) == GPTPS_OK && ran) { }
}

static uint64_t setting(gptps *e, const char *key)
{
    char buf[64];
    if (gptps_settings_get(e, key, buf, sizeof buf) != GPTPS_OK) return 0;
    return strtoull(buf, NULL, 10);
}

int main(void)
{
    gptps *e;
    gptps_adapt *a;
    gptps_adapt_config c;
    gptps_adapt_report r;

    /* 1) on request, after enough evidence */
    e = open_manual();
    memset(&c, 0, sizeof c);
    c.struct_size = sizeof c; c.min_samples = 3;            /* headroom 25%, by default */
    a = gptps_adapt_open(e, &c);
    CHECK(a != NULL);
    reg(e, "work", take40, GPTPS_EXEC_OOP);
    reg(e, "oom", oom, GPTPS_EXEC_OOP);
    reg(e, "inproc", quick, GPTPS_EXEC_INPROC);

    memset(&r, 0, sizeof r); r.struct_size = sizeof r;
    CHECK(gptps_adapt_get(a, "work", &r) == GPTPS_E_NOTFOUND);
    run(e, "work", 2);
    CHECK(gptps_adapt_get(a, "work", &r) == GPTPS_OK);
    CHECK(r.samples == 2);
    CHECK(r.peak >= 40 * MiB);
    CHECK(r.method[0] != 0);
    CHECK(r.suggested == 0);
    CHECK(gptps_adapt_apply(a, "work") == GPTPS_E_BUSY);
    CHECK(setting(e, "tasks.work.mem_bytes") == 8 * MiB);

    run(e, "work", 1);
    CHECK(gptps_adapt_get(a, "work", &r) == GPTPS_OK);
    CHECK(r.samples == 3 && r.cap_hits == 0 && r.declared == 8 * MiB);
    CHECK(r.suggested >= r.peak + r.peak / 4);              /* the headroom */
    CHECK(r.suggested < r.peak + r.peak / 4 + MiB);         /* ...rounded up to a MiB, no more */
    CHECK(r.suggested % MiB == 0);
    CHECK(gptps_adapt_apply(a, "work") == GPTPS_OK);
    CHECK(setting(e, "tasks.work.mem_bytes") == r.suggested);
    CHECK(gptps_adapt_get(a, "work", &r) == GPTPS_OK && r.applied == 1);
    run(e, "work", 1);                                      /* the next run declares it */
    CHECK(gptps_adapt_get(a, "work", &r) == GPTPS_OK && r.declared == setting(e, "tasks.work.mem_bytes"));

    /* 2) a cap hit grows from what was hit, without waiting for min_samples */
    run(e, "oom", 1);
    CHECK(gptps_adapt_get(a, "oom", &r) == GPTPS_OK);
    CHECK(r.cap_hits == 1 && r.hit_declared == 8 * MiB);
    CHECK(r.suggested >= 12 * MiB);                         /* 8 MiB + 50% */
    CHECK(r.suggested % MiB == 0);

    /* 4) never an in-process task */
    run(e, "inproc", 3);
    CHECK(gptps_adapt_get(a, "inproc", &r) == GPTPS_E_NOTFOUND);

    /* bad arguments */
    r.struct_size = sizeof r - 1;
    CHECK(gptps_adapt_get(a, "work", &r) == GPTPS_E_INVAL);
    CHECK(gptps_adapt_apply(a, "nope") == GPTPS_E_NOTFOUND);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    gptps_adapt_close(a);

    /* 3) auto_apply */
    e = open_manual();
    memset(&c, 0, sizeof c);
    c.struct_size = sizeof c; c.min_samples = 1; c.auto_apply = 1; c.floor_bytes = 64 * MiB;
    a = gptps_adapt_open(e, &c);
    CHECK(a != NULL);
    reg(e, "work", take40, GPTPS_EXEC_OOP);
    run(e, "work", 1);
    r.struct_size = sizeof r;
    CHECK(gptps_adapt_get(a, "work", &r) == GPTPS_OK);
    CHECK(r.applied == 1);
    CHECK(r.suggested >= 64 * MiB);                         /* the floor */
    CHECK(setting(e, "tasks.work.mem_bytes") == r.suggested);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    gptps_adapt_close(a);
    gptps_adapt_close(NULL);

    if (fails) { printf("%d adapt check(s) FAILED\n", fails); return 1; }
    printf("adapt: all checks passed\n");
    return 0;
}
