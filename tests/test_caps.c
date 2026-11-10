/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_caps.c - the process executors' CPU and I/O caps where they CANNOT be
 * enforced (ABI 2.6). test_cgroup covers where they can.
 *
 * With no GPTPS_CGROUP_PARENT there is nothing to hold a CPU or I/O cap on POSIX, and
 * the contract is: the job runs anyway, and the host is told - one GPTPS_LOG_WARN per
 * cap per task type, naming it - never silence and never a failure. Also:
 *  - an in-process task with a cap is not warned about: the cap is documented as
 *    ignored there, not as unenforced;
 *  - the caps are live settings (tasks.<t>.cpu_max_millicpus, ...) read at submit, so a
 *    cap set only through a setting is warned about too, and one set back to 0 is not;
 *  - a def from before 2.6 (struct_size ending at `flags`) registers with no caps.
 */
#define _POSIX_C_SOURCE 200809L
#include "gptps.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int g_warn_cpu, g_warn_io, g_warn_other;
static void sink(gptps_log_level lvl, const char *msg, void *ud)
{
    (void)ud;
    if (lvl != GPTPS_LOG_WARN) return;
    if      (strstr(msg, "cpu_max_millicpus")) __atomic_add_fetch(&g_warn_cpu, 1, __ATOMIC_SEQ_CST);
    else if (strstr(msg, "io_max_read_bps"))   __atomic_add_fetch(&g_warn_io, 1, __ATOMIC_SEQ_CST);
    else                                       __atomic_add_fetch(&g_warn_other, 1, __ATOMIC_SEQ_CST);
}
static int wc(void) { return __atomic_load_n(&g_warn_cpu, __ATOMIC_SEQ_CST); }
static int wi(void) { return __atomic_load_n(&g_warn_io, __ATOMIC_SEQ_CST); }

static int g_ok, g_throttled_seen;
static void on_ev(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_FINISHED) g_ok++;
    if ((ev->kind == GPTPS_EV_FINISHED || ev->kind == GPTPS_EV_FAILED) &&
        gptps_event_measure(ev, GPTPS_M_CPU_THROTTLED_MS)) g_throttled_seen++;
}

static gptps_status work(gptps_ctx *ctx, void *ud) { (void)ud; return gptps_result_set(ctx, "x", 1); }

static gptps *open_manual(void)
{
    gptps_config cfg; gptps *e = NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;
    cfg.limits.max_memory_bytes = 256u << 20;
    cfg.mode = GPTPS_RUN_MANUAL;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    CHECK(gptps_set_event_cb(e, on_ev, NULL) == GPTPS_OK);
    return e;
}

static void reg(gptps *e, const char *name, gptps_exec_kind k, uint64_t cpu, uint64_t rd, uint64_t wr, size_t size)
{
    gptps_task_def d; memset(&d, 0, sizeof d);
    d.struct_size = size; d.name = name; d.run = work; d.exec = k;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.timeout_seconds = 10;
    d.cpu_max_millicpus = cpu; d.io_max_read_bps = rd; d.io_max_write_bps = wr;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

static void run(gptps *e, const char *task, int n)
{
    size_t ran; int i;
    for (i = 0; i < n; ++i) CHECK(gptps_submit(e, task, NULL, 0, NULL) == GPTPS_OK);
    while (gptps_step(e, &ran) == GPTPS_OK && ran) { }
}

int main(void)
{
    char buf[64];
    gptps *e;
    unsetenv("GPTPS_CGROUP_PARENT");        /* CI may run the suite inside a delegated cgroup */
    gptps_set_log_sink(sink, NULL);

    e = open_manual();
    /* an OOP job with both caps: runs, and each cap is warned about exactly once */
    reg(e, "capped", GPTPS_EXEC_OOP, 500, 1u << 20, 0, sizeof(gptps_task_def));
    run(e, "capped", 3);
    CHECK(g_ok == 3);
    CHECK(wc() == 1);
    CHECK(wi() == 1);
    CHECK(g_throttled_seen == 0);           /* not enforced, so not measured either */

    /* the same caps on an in-process task: ignored there by definition, so no warning */
    reg(e, "inproc", GPTPS_EXEC_INPROC, 500, 1u << 20, 1u << 20, sizeof(gptps_task_def));
    run(e, "inproc", 2);
    CHECK(g_ok == 5);
    CHECK(wc() == 1 && wi() == 1);

    /* a pre-2.6 def: its struct ends at `flags`, so it has no caps and gets no warning,
     * whatever happens to lie past its end */
    reg(e, "old", GPTPS_EXEC_OOP, 999, 999, 999, offsetof(gptps_task_def, flags) + sizeof(uint64_t));
    CHECK(gptps_settings_get(e, "tasks.old.cpu_max_millicpus", buf, sizeof buf) == GPTPS_OK);
    CHECK(strcmp(buf, "0") == 0);
    run(e, "old", 1);
    CHECK(g_ok == 6);
    CHECK(wc() == 1 && wi() == 1);

    /* a cap given only as a live setting is read at submit and warned about, once */
    CHECK(gptps_settings_set(e, "tasks.old.cpu_max_millicpus", "250") == GPTPS_OK);
    CHECK(gptps_settings_get(e, "tasks.old.cpu_max_millicpus", buf, sizeof buf) == GPTPS_OK);
    CHECK(strcmp(buf, "250") == 0);
    run(e, "old", 2);
    CHECK(g_ok == 8);
    CHECK(wc() == 2 && wi() == 1);

    /* back to 0: no cap, nothing to say */
    CHECK(gptps_settings_set(e, "tasks.old.cpu_max_millicpus", "0") == GPTPS_OK);
    CHECK(gptps_settings_set(e, "tasks.old.io_max_write_bps", "0") == GPTPS_OK);
    run(e, "old", 1);
    CHECK(wc() == 2 && wi() == 1);
    CHECK(__atomic_load_n(&g_warn_other, __ATOMIC_SEQ_CST) == 0);

    CHECK(gptps_shutdown(e) == GPTPS_OK);
    gptps_set_log_sink(NULL, NULL);

    if (fails) { printf("%d caps check(s) FAILED\n", fails); return 1; }
    printf("caps: all checks passed\n");
    return 0;
}
