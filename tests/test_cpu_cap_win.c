/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_cpu_cap_win.c - the CPU cap on Windows (ABI 2.6): a PROGRAM job's job object
 * holds it to cpu_max_millicpus, so the job is THROTTLED, not just labelled.
 *
 * The same job - prog_helper burning 300 ms of CPU - runs twice: uncapped, then capped
 * at 100 millicpus (a tenth of one CPU). Capped, those 300 ms of CPU take at least
 * three seconds of wall time; uncapped, a fraction of that. The test asks for less than
 * the ideal ten-to-one - Windows enforces the rate over its scheduling intervals, and a
 * CI runner is noisy - but for far more than noise could produce. It also checks that
 * the cap was reported as in force: no "not enforced" warning reached the log sink.
 *
 * Windows only: on Linux the cap is a cgroup's, which test_cgroup covers where a cpu
 * controller is delegated, and test_caps covers the warning where none is.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int g_warned;
static void sink(gptps_log_level lvl, const char *msg, void *ud)
{
    (void)ud;
    if (lvl == GPTPS_LOG_WARN && strstr(msg, "cpu_max_millicpus")) g_warned++;
    printf("log: %s\n", msg);
}

static uint64_t g_start, g_wall;
static gptps_status g_end = GPTPS_E_BUSY;
static void on_ev(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_STARTED) g_start = ev->ts_ms;
    if (ev->kind == GPTPS_EV_FINISHED || ev->kind == GPTPS_EV_FAILED) {
        g_wall = ev->ts_ms - g_start;
        g_end = ev->status;
    }
}

/* wall time of one run of `task` */
static uint64_t run(gptps *e, const char *task)
{
    size_t n;
    g_start = g_wall = 0; g_end = GPTPS_E_BUSY;
    CHECK(gptps_submit(e, task, NULL, 0, NULL) == GPTPS_OK);
    while (gptps_step(e, &n) == GPTPS_OK && n) { }
    CHECK(g_end == GPTPS_OK);
    return g_wall;
}

int main(void)
{
    static const char *const av[] = { HELPER_PATH, "spin", "300", NULL };
    gptps_config cfg; gptps *e = NULL; gptps_task_def d;
    uint64_t free_ms, capped_ms;

    gptps_set_log_sink(sink, NULL);
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;
    cfg.mode = GPTPS_RUN_MANUAL;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (!e) return 1;
    CHECK(gptps_set_event_cb(e, on_ev, NULL) == GPTPS_OK);

    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "free"; d.exec = GPTPS_EXEC_PROGRAM; d.argv = av;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.timeout_seconds = 120;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    d.name = "capped"; d.cpu_max_millicpus = 100;            /* a tenth of one CPU */
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);

    free_ms   = run(e, "free");
    capped_ms = run(e, "capped");
    printf("300 ms of CPU: uncapped %llu ms wall, capped at 100 millicpus %llu ms wall\n",
           (unsigned long long)free_ms, (unsigned long long)capped_ms);

    CHECK(g_warned == 0);                 /* the job object took the cap */
    CHECK(capped_ms >= 1500);             /* ideal 3000: held back, by far more than noise */
    CHECK(capped_ms >= 3 * free_ms);      /* and slower than the same job uncapped */

    CHECK(gptps_shutdown(e) == GPTPS_OK);
    gptps_set_log_sink(NULL, NULL);
    if (fails) { printf("%d Windows CPU-cap check(s) FAILED\n", fails); return 1; }
    printf("Windows CPU cap: all checks passed\n");
    return 0;
}
