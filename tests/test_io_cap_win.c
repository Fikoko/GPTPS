/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_io_cap_win.c - the I/O caps on Windows (ABI 2.8): a PROGRAM job's job object
 * holds its disk bandwidth through I/O rate control, so the job is THROTTLED.
 *
 * prog_helper writes 16 MiB past the cache, twice: uncapped, then capped at 4 MiB/s.
 * Capped, that takes at least about four seconds; the test asks for 2.5, well clear of
 * noise. Where the system cannot hold the cap (before Windows 10 1607 / Server 2016, or
 * Wine) the job runs uncapped and the log says so once - which is then what is checked.
 * A read cap that differs from the write cap holds as one combined figure, the smaller,
 * and the log says that once too.
 *
 * Windows only: Linux's cap is io.max on a cgroup, which test_cgroup covers.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int g_not_enforced, g_combined;
static void sink(gptps_log_level lvl, const char *msg, void *ud)
{
    (void)ud;
    if (lvl == GPTPS_LOG_WARN && strstr(msg, "io_max_read_bps") && strstr(msg, "not enforced")) g_not_enforced++;
    if (lvl == GPTPS_LOG_INFO && strstr(msg, "hold as one limit")) g_combined++;
    printf("log: %s\n", msg);
}

static uint64_t g_start, g_wall;
static gptps_status g_end = GPTPS_E_BUSY;
static void on_ev(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_STARTED) g_start = ev->ts_ms;
    if (ev->kind == GPTPS_EV_FINISHED || ev->kind == GPTPS_EV_FAILED) { g_wall = ev->ts_ms - g_start; g_end = ev->status; }
}

static uint64_t run(gptps *e, const char *name, uint64_t rbps, uint64_t wbps)
{
    static const char *const argv[] = { HELPER_PATH, "diskwrite", "gptps_io_cap_win.bin", "16", NULL };
    gptps_task_def d;
    size_t n;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.exec = GPTPS_EXEC_PROGRAM; d.argv = argv;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.io_max_read_bps = rbps; d.io_max_write_bps = wbps;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    g_end = GPTPS_E_BUSY;
    CHECK(gptps_submit(e, name, NULL, 0, NULL) == GPTPS_OK);
    while (gptps_step(e, &n) == GPTPS_OK && n) { }
    CHECK(g_end == GPTPS_OK);
    return g_wall;
}

int main(void)
{
    gptps_config cfg;
    gptps *e = NULL;
    uint64_t free_ms, capped_ms;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;
    cfg.mode = GPTPS_RUN_MANUAL;
    gptps_set_log_sink(sink, NULL);
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (!e) return 1;
    CHECK(gptps_set_event_cb(e, on_ev, NULL) == GPTPS_OK);

    free_ms = run(e, "free", 0, 0);
    capped_ms = run(e, "capped", 0, 4u << 20);
    printf("16 MiB: %llu ms uncapped, %llu ms capped at 4 MiB/s\n",
           (unsigned long long)free_ms, (unsigned long long)capped_ms);
    if (g_not_enforced) {
        printf("(the I/O cap is not enforced on this system: checked that it said so, once)\n");
        CHECK(g_not_enforced == 1);
    } else {
        CHECK(capped_ms >= 2500);                    /* throttled, not just labelled */
        run(e, "split", 8u << 20, 4u << 20);         /* read 8, write 4: one figure, 4 */
        CHECK(g_combined == 1);
    }
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    gptps_set_log_sink(NULL, NULL);
    if (fails) { printf("%d io-cap check(s) FAILED\n", fails); return 1; }
    printf("io cap (Windows): all checks passed\n");
    return 0;
}
