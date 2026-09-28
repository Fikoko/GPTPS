/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_durable_oom.c - gptps_dq_submit's GPTPS_E_NOMEM means "not submitted".
 *
 * The in-memory record table used to grow AFTER the 'P' record was fsync'd. When
 * that realloc failed, the call returned GPTPS_E_NOMEM - the caller was told the
 * work was not submitted - while the journal already held it durably, so unless a
 * compaction ran first, the next gptps_dq_recover ran it anyway. Measured before
 * the fix: pending 1 on reopen, and the body ran once in the next run.
 *
 * The add-on is compiled into this file with realloc redirected, so one growth of
 * the table can be made to fail; it links the core only, not gptps_durable_queue.
 * <stdlib.h> comes first, so its own declaration of realloc keeps the real name.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int g_fail_realloc;      /* set: the next realloc in the add-on fails */
static void *dq_test_realloc(void *p, size_t n);
#define realloc dq_test_realloc
#include "../addons/gptps_durable_queue.c"
#undef realloc
static void *dq_test_realloc(void *p, size_t n)
{
    if (g_fail_realloc) { g_fail_realloc = 0; return NULL; }
    return realloc(p, n);
}

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

#define JOURNAL "dq_test_oom.journal"

static int g_runs;
static gptps_status task_count(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; ++g_runs; return GPTPS_OK; }

/* MANUAL, so nothing runs unless this thread steps it */
static gptps *open_manual(void)
{
    gptps_config cfg; gptps *e = NULL; gptps_task_def d;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1; cfg.limits.max_memory_bytes = 64u << 20;
    cfg.mode = GPTPS_RUN_MANUAL;
    if (gptps_open_ex(&cfg, &e) != GPTPS_OK) return NULL;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "work"; d.run = task_count; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    gptps_register_task(e, &d);
    return e;
}

int main(void)
{
    gptps *e; gptps_dq *dq; gptps_handle h = 0; size_t ran = 0;
    unsigned char b = 1;
    remove(JOURNAL);

    e = open_manual(); CHECK(e != NULL); if (!e) return 1;
    dq = gptps_dq_open(e, JOURNAL); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return 1; }
    g_fail_realloc = 1;                     /* the empty table's first growth fails */
    CHECK(gptps_dq_submit(dq, "work", &b, 1, &h) == GPTPS_E_NOMEM);
    CHECK(g_fail_realloc == 0);             /* the failure was really injected */
    CHECK(h == 0);
    CHECK(gptps_dq_pending(dq) == 0);
    CHECK(gptps_step(e, &ran) == GPTPS_OK && ran == 0);    /* nothing was enqueued */
    CHECK(gptps_dq_submit(dq, "work", &b, 1, &h) == GPTPS_OK);  /* and it recovers */
    CHECK(gptps_step(e, &ran) == GPTPS_OK && ran == 1);
    CHECK(gptps_dq_pending(dq) == 0);
    gptps_shutdown(e);
    gptps_dq_close(dq);

    /* the next run: nothing left to recover - the refused submit never reached disk */
    g_runs = 0;
    e = open_manual(); CHECK(e != NULL); if (!e) return 1;
    dq = gptps_dq_open(e, JOURNAL); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return 1; }
    CHECK(gptps_dq_pending(dq) == 0);       /* was 1 */
    CHECK(gptps_dq_recover(dq) == 0);
    CHECK(gptps_step(e, &ran) == GPTPS_OK && ran == 0);
    CHECK(g_runs == 0);
    gptps_shutdown(e);
    gptps_dq_close(dq);
    remove(JOURNAL);

    if (fails) { printf("%d durable-queue OOM check(s) FAILED\n", fails); return 1; }
    printf("all durable-queue OOM checks passed\n");
    return 0;
}
