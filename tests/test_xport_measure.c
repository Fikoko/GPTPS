/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_xport_measure.c - measurements across worker processes (docs/MEASUREMENTS.md).
 *
 * In engine mode a gptps_xport worker runs its own engine, so a process job's
 * measurements are taken there; the reply carries those of the item's last attempt
 * back to the parent. Checks, over the link:
 *   - gptps_xport_submit_ex: a 32 MiB program's reply carries mem.peak >= 32 MiB and
 *     its CPU, and gptps_xport_result_free releases them;
 *   - gptps_xport_submit_async_ex: the callback sees them too;
 *   - a program that fails twice and is dead-lettered: the reply carries the LAST
 *     failed attempt's measurements, though the dead letter itself has none;
 *   - an in-process task, and handler mode: no measurements, and nothing breaks;
 *   - the plain gptps_xport_submit still answers on the same links;
 *   - a parent with no engine folds the replies into gptps_stats_open().
 * POSIX only (fork + socketpair), like gptps_xport.
 */
#define _POSIX_C_SOURCE 200809L
#include "gptps_xport.h"
#include "gptps_stats.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef HELPER_PATH
#  error "HELPER_PATH must point at prog_helper"
#endif

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
#define MiB (1024ull * 1024ull)

static const char *const av_take[] = { HELPER_PATH, "mem", "32", "50", NULL };
static const char *const av_fail[] = { HELPER_PATH, "mem", "8", "0", "3", NULL };

static gptps_status t_inproc(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }

static const gptps_measure *find(const gptps_measure *m, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; ++i) if (strcmp(m[i].name, name) == 0) return &m[i];
    return NULL;
}

/* ---- async: one reply, waited for by hand ---- */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;
static int             g_done;
static uint64_t        g_peak;
static int             g_has_cpu, g_io;

static void on_reply(const gptps_xport_reply *r, void *ud)
{
    const gptps_measure *pk = find(r->measures, r->n_measures, GPTPS_M_MEM_PEAK);
    (void)ud;
    pthread_mutex_lock(&g_mu);
    g_io = r->io;
    g_peak = pk ? pk->value : 0;
    g_has_cpu = find(r->measures, r->n_measures, GPTPS_M_CPU_USER_MS) != NULL;
    CHECK(r->struct_size == sizeof *r && r->task_status == GPTPS_OK && !r->measures_cut);
    g_done = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

static gptps_status h_echo(const char *task, const void *p, size_t n, void **out, size_t *on, void *ud)
{
    (void)task; (void)ud;
    *out = NULL; *on = 0;
    if (n) { *out = malloc(n); if (!*out) return GPTPS_E_NOMEM; memcpy(*out, p, n); *on = n; }
    return GPTPS_OK;
}

int main(void)
{
    gptps_task_def tasks[3];
    gptps_xport_config cfg;
    gptps_xport *xp;
    gptps_xport_reply r;
    gptps_stats *st;
    gptps_stats_measure sm;
    const gptps_measure *pk;
    void *res = NULL; size_t rl = 0; gptps_status ts = GPTPS_E_IO;

    memset(tasks, 0, sizeof tasks);
    tasks[0].struct_size = sizeof tasks[0]; tasks[0].name = "take";
    tasks[0].exec = GPTPS_EXEC_PROGRAM; tasks[0].argv = av_take;
    tasks[1].struct_size = sizeof tasks[1]; tasks[1].name = "fail";
    tasks[1].exec = GPTPS_EXEC_PROGRAM; tasks[1].argv = av_fail;
    tasks[1].default_policy.struct_size = sizeof tasks[1].default_policy;
    tasks[1].default_policy.max_retries = 1;
    tasks[1].default_policy.on_failure = GPTPS_ON_FAILURE_DEAD_LETTER;
    tasks[2].struct_size = sizeof tasks[2]; tasks[2].name = "inproc"; tasks[2].run = t_inproc;
    { size_t i; for (i = 0; i < 3; ++i) {
        tasks[i].default_cost.struct_size = sizeof tasks[i].default_cost;
        if (!tasks[i].default_policy.struct_size) tasks[i].default_policy.struct_size = sizeof tasks[i].default_policy;
        tasks[i].default_policy.timeout_seconds = 30; } }

    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.nworkers = 2;
    cfg.tasks = tasks; cfg.ntasks = 3;
    xp = gptps_xport_open_ex(&cfg);
    CHECK(xp != NULL);
    if (!xp) return 1;
    st = gptps_stats_open();
    CHECK(st != NULL);

    /* 1) blocking, with the measurements */
    memset(&r, 0, sizeof r); r.struct_size = sizeof r;
    CHECK(gptps_xport_submit_ex(xp, "take", NULL, 0, &r) == GPTPS_OK);
    CHECK(r.io == GPTPS_OK && r.task_status == GPTPS_OK && r.request_id != 0 && !r.measures_cut);
    pk = find(r.measures, r.n_measures, GPTPS_M_MEM_PEAK);
    CHECK(pk != NULL && pk->value >= 32 * MiB && pk->unit == GPTPS_UNIT_BYTES && pk->kind == GPTPS_MEASURE_PEAK);
    CHECK(pk != NULL && pk->method != NULL && strlen(pk->method) > 0);
    CHECK(find(r.measures, r.n_measures, GPTPS_M_CPU_USER_MS) != NULL);
    if (st) CHECK(gptps_stats_measure_fold(st, "take", r.measures, r.n_measures, 1) == GPTPS_OK);
    if (st && pk) {
        sm.struct_size = sizeof sm;
        CHECK(gptps_stats_measure_get(st, "take", GPTPS_M_MEM_PEAK, pk->method, &sm) == GPTPS_OK);
        CHECK(sm.count == 1 && sm.max == pk->value);
    }
    gptps_xport_result_free(&r);
    CHECK(r.measures == NULL && r.n_measures == 0 && r.result == NULL && r.struct_size == sizeof r);
    gptps_xport_result_free(&r);                     /* twice is fine */

    /* 2) async, with the measurements */
    g_done = 0;
    CHECK(gptps_xport_submit_async_ex(xp, "take", NULL, 0, on_reply, NULL, NULL) == GPTPS_OK);
    pthread_mutex_lock(&g_mu);
    while (!g_done) pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
    CHECK(g_io == GPTPS_OK && g_peak >= 32 * MiB && g_has_cpu);

    /* 3) dead-lettered after two failed attempts: the last attempt's measurements */
    memset(&r, 0, sizeof r); r.struct_size = sizeof r;
    CHECK(gptps_xport_submit_ex(xp, "fail", NULL, 0, &r) == GPTPS_OK);
    CHECK(r.io == GPTPS_OK && r.task_status == GPTPS_E_TASK);
    pk = find(r.measures, r.n_measures, GPTPS_M_MEM_PEAK);
    CHECK(pk != NULL && pk->value >= 8 * MiB);
    gptps_xport_result_free(&r);

    /* 4) an in-process task: answered, nothing measured */
    memset(&r, 0, sizeof r); r.struct_size = sizeof r;
    CHECK(gptps_xport_submit_ex(xp, "inproc", NULL, 0, &r) == GPTPS_OK);
    CHECK(r.io == GPTPS_OK && r.task_status == GPTPS_OK && r.measures == NULL && r.n_measures == 0);
    gptps_xport_result_free(&r);

    /* 5) the plain submit, on the same links */
    CHECK(gptps_xport_submit(xp, "take", NULL, 0, &res, &rl, &ts) == GPTPS_OK && ts == GPTPS_OK);
    free(res);

    /* 6) a bad argument: refused, and the reply left empty */
    memset(&r, 0, sizeof r);
    CHECK(gptps_xport_submit_ex(xp, "take", NULL, 0, &r) == GPTPS_E_INVAL);   /* struct_size 0 */
    CHECK(gptps_xport_submit_ex(xp, "take", NULL, 0, NULL) == GPTPS_E_INVAL);
    gptps_xport_close(xp);

    /* 7) handler mode carries none, and replies still parse */
    xp = gptps_xport_open(1, h_echo, NULL);
    CHECK(xp != NULL);
    if (xp) {
        memset(&r, 0, sizeof r); r.struct_size = sizeof r;
        CHECK(gptps_xport_submit_ex(xp, "x", "hi", 2, &r) == GPTPS_OK);
        CHECK(r.task_status == GPTPS_OK && r.result_len == 2 && memcmp(r.result, "hi", 2) == 0);
        CHECK(r.measures == NULL && r.n_measures == 0);
        gptps_xport_result_free(&r);
        gptps_xport_close(xp);
    }
    gptps_stats_close(st);

    if (fails) { printf("%d xport-measure check(s) FAILED\n", fails); return 1; }
    printf("all xport-measure checks passed\n");
    return 0;
}
