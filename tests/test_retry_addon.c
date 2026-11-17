/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_retry_addon.c - addons/gptps_retry: retry rules by exit code, on POSIX and
 * Windows, with a vendor's convention: 1 retry, 2 never, 3 retry much later.
 *
 *  1) A code with a NEVER rule runs once: its retry is refused, the item is
 *     dead-lettered with GPTPS_E_DENIED, and its retries are not spent.
 *  2) A code with a wait runs again only after the wait, every time, and as often
 *     as the policy allows - no more.
 *  3) A code with no rule retries as the policy says; so does a failure with no
 *     exit code (an in-process task).
 *  4) A task's own rule wins over an all-tasks rule.
 *  5) The counters, and argument checks.
 *  6) A program that never started (start.error, ABI 2.8) has a rule of its own.
 *
 * MANUAL mode: the test steps the engine itself, waiting out held retries.
 */
#include "gptps.h"
#include "gptps_retry.h"
#include <stdio.h>
#include <string.h>

#ifndef HELPER_PATH
#define HELPER_PATH "./prog_helper"
#endif

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

enum { T_BAD, T_BAD2, T_LOCK, T_LIC, T_INPROC, T_NOPROG, T_N };
static const char *const NAMES[T_N] = { "bad", "bad2", "lock", "lic", "inproc", "noprog" };
static int      g_runs[T_N], g_terminal;
static uint64_t g_start[T_N][4];
static gptps_status g_dl_status[T_N];
static int g_dl_start[T_N];       /* the dead letter carries start.error (ABI 2.8) */

static int idx(const char *name)
{
    int i;
    for (i = 0; i < T_N; ++i) if (name && strcmp(name, NAMES[i]) == 0) return i;
    return -1;
}

static void on_ev(const gptps_event *ev, void *ud)
{
    int i = idx(ev->task_name);
    (void)ud;
    if (i < 0) return;
    if (ev->kind == GPTPS_EV_STARTED) {
        if (g_runs[i] < 4) g_start[i][g_runs[i]] = ev->ts_ms;
        ++g_runs[i];
    }
    if (ev->kind == GPTPS_EV_DEAD_LETTERED || ev->kind == GPTPS_EV_FINISHED || ev->kind == GPTPS_EV_DROPPED)
        ++g_terminal;
}

static void on_dl(const gptps_dead_letter *dl, void *ud)
{
    int i = idx(dl->task_name);
    (void)ud;
    if (i >= 0) {
        size_t k;
        g_dl_status[i] = dl->status;
        if (dl->struct_size >= sizeof *dl)
            for (k = 0; k < dl->n_measures; ++k)
                if (strcmp(dl->measures[k].name, GPTPS_M_START_ERROR) == 0) g_dl_start[i] = 1;
    }
}

static gptps_status fail_body(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_E_TASK; }

static void reg(gptps *e, const char *name, const char *const *argv, uint32_t retries)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name;
    if (argv) { d.exec = GPTPS_EXEC_PROGRAM; d.argv = argv; } else d.run = fail_body;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = retries;      /* backoff 0: retry at once */
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

int main(void)
{
    static const char *const a_exit1[] = { HELPER_PATH, "exit", "1", NULL };
    static const char *const a_exit2[] = { HELPER_PATH, "exit", "2", NULL };
    static const char *const a_exit3[] = { HELPER_PATH, "exit", "3", NULL };
    gptps_config cfg;
    gptps *e = NULL;
    gptps_retry *r = NULL;
    uint64_t t0;
    int i;

    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;
    cfg.mode = GPTPS_RUN_MANUAL;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (!e) return 1;
    CHECK(gptps_set_event_cb(e, on_ev, NULL) == GPTPS_OK);

    reg(e, "bad", a_exit2, 3);
    reg(e, "bad2", a_exit2, 1);
    reg(e, "lock", a_exit1, 2);
    reg(e, "lic", a_exit3, 2);
    reg(e, "inproc", NULL, 1);
    {
        static const char *const a_none[] = { "gptps_retry_no_such_program_xyz", NULL };
        reg(e, "noprog", a_none, 3);
    }

    /* 5) argument checks */
    CHECK(gptps_retry_install(NULL, &r) == GPTPS_E_INVAL);
    CHECK(gptps_retry_install(e, NULL) == GPTPS_E_INVAL);
    CHECK(gptps_retry_on_exit(NULL, NULL, 2, GPTPS_RETRY_NEVER) == GPTPS_E_INVAL);

    CHECK(gptps_retry_install(e, &r) == GPTPS_OK && r != NULL);
    {
        char longname[200];
        memset(longname, 'x', sizeof longname - 1); longname[sizeof longname - 1] = 0;
        CHECK(gptps_retry_on_exit(r, longname, 2, 0) == GPTPS_E_INVAL);
    }
    CHECK(gptps_retry_on_exit(r, NULL, 2, GPTPS_RETRY_NEVER) == GPTPS_OK);   /* bad input: never */
    CHECK(gptps_retry_on_exit(r, "bad2", 2, 0) == GPTPS_OK);                 /* 4) except here */
    CHECK(gptps_retry_on_exit(r, "lic", 3, 50) == GPTPS_OK);
    CHECK(gptps_retry_on_exit(r, "lic", 3, 300) == GPTPS_OK);                /* replaces */
    CHECK(gptps_retry_on_start_error(r, NULL, GPTPS_RETRY_NEVER) == GPTPS_OK);
    CHECK(gptps_retry_on_start_error(NULL, NULL, 0) == GPTPS_E_INVAL);

    for (i = 0; i < T_N; ++i) CHECK(gptps_submit(e, NAMES[i], NULL, 0, NULL) == GPTPS_OK);
    t0 = gptps_now_ms(NULL);
    while (g_terminal < T_N && gptps_now_ms(NULL) - t0 < 20000) {
        size_t n;
        gptps_step(e, &n);           /* 0 while lic's retry is held: keep stepping */
    }
    CHECK(g_terminal == T_N);
    CHECK(gptps_dead_letter_drain(e, on_dl, NULL) == T_N);

    /* 1) never: one run, refused, not retried */
    CHECK(g_runs[T_BAD] == 1);
    CHECK(g_dl_status[T_BAD] == GPTPS_E_DENIED);
    /* 4) its own rule: retried as its policy says */
    CHECK(g_runs[T_BAD2] == 2 && g_dl_status[T_BAD2] == GPTPS_E_TASK);
    /* 3) no rule for 1, and no exit code at all */
    CHECK(g_runs[T_LOCK] == 3 && g_dl_status[T_LOCK] == GPTPS_E_TASK);
    CHECK(g_runs[T_INPROC] == 2 && g_dl_status[T_INPROC] == GPTPS_E_TASK);
    /* 2) a wait before each retry, and no extra runs */
    CHECK(g_runs[T_LIC] == 3 && g_dl_status[T_LIC] == GPTPS_E_TASK);
    CHECK(g_start[T_LIC][1] - g_start[T_LIC][0] >= 300);
    CHECK(g_start[T_LIC][2] - g_start[T_LIC][1] >= 300);
    /* 6) never started: refused at once, and the dead letter says why */
    CHECK(g_runs[T_NOPROG] == 1 && g_dl_status[T_NOPROG] == GPTPS_E_DENIED && g_dl_start[T_NOPROG]);
    /* 5) the counters */
    CHECK(gptps_retry_refused(r) == 2);
    CHECK(gptps_retry_forgotten(r) == 0);
    CHECK(gptps_retry_delayed(r) == 2);
    CHECK(gptps_retry_refused(NULL) == 0 && gptps_retry_delayed(NULL) == 0);

    CHECK(gptps_shutdown(e) == GPTPS_OK);
    gptps_retry_close(r);
    gptps_retry_close(NULL);
    if (fails) { printf("%d retry check(s) FAILED\n", fails); return 1; }
    printf("retry add-on: all checks passed\n");
    return 0;
}
