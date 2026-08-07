/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * dashboard.c - a live terminal dashboard over a running GPTPS engine, using the
 * tui add-on. On a real terminal it redraws continuously and takes hotkeys:
 *   [w] submit 'work'  [f] submit 'flaky'  |  [k]/[j] scroll  [m] KPI level
 *   [p] pause  [s] settings editor  [?] help  [q] quit
 * `m` cycles how much the dashboard computes (minimal/normal/full) at runtime, so
 * its own CPU/RAM cost is tunable live; `s` opens a live editor over the unified
 * settings registry (navigate with j/k, Enter to edit, w to save). Run it headless
 * (no TTY) and gptps_tui_run() simply returns - the engine still drains the work.
 *
 *   cc dashboard.c gptps.c addons/tui.c -lpthread -ldl   (amalgamation; Win: drop -ldl)
 */
#include "gptps.h"
#include "gptps_tui.h"
#include <stdio.h>
#include <string.h>

/* a task that does ~120 ms of (cancellable) work, so activity is visible */
static gptps_status work(gptps_ctx *ctx, void *ud)
{
    uint64_t start = gptps_now_ms(ctx);
    (void)ud;
    while (!gptps_is_cancelled(ctx))
        if (gptps_now_ms(ctx) - start > 120) break;
    return GPTPS_OK;
}
static gptps_status flaky(gptps_ctx *ctx, void *ud)
{
    (void)ud; return (gptps_now_ms(ctx) & 1) ? GPTPS_OK : GPTPS_E_TASK;
}

static void reg(gptps *e, const char *name, gptps_run_fn fn, uint32_t retries)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = fn; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = 1u << 20;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = retries;
    gptps_register_task(e, &d);
}

int main(void)
{
    gptps *engine;
    gptps_tui *tui;
    gptps_tui_config cfg;
    gptps_handle h;
    int i;

    if (gptps_open(NULL, &engine) != GPTPS_OK) { fprintf(stderr, "open failed\n"); return 1; }

    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.title = "GPTPS live demo";
    cfg.refresh_ms = 120;
    cfg.settings_path = "gptps_settings.toml";   /* where the [s] pane's save writes */
    cfg.color = -1; cfg.interactive = -1;        /* auto: on when attached to a terminal */
    cfg.unicode = -1;                            /* auto: box rules + block gauge with color */
    tui = gptps_tui_install(engine, &cfg);
    if (!tui) { gptps_shutdown(engine); return 1; }

    reg(engine, "work",  work,  0);
    reg(engine, "flaky", flaky, 2);
    gptps_tui_add_task(tui, "work",  "Work",  'w', NULL, 0);   /* [w] submits work  */
    gptps_tui_add_task(tui, "flaky", "Flaky", 'f', NULL, 0);   /* [f] submits flaky */

    for (i = 0; i < 12; ++i) gptps_submit(engine, "work",  NULL, 0, &h);
    for (i = 0; i < 6;  ++i) gptps_submit(engine, "flaky", NULL, 0, &h);

    gptps_tui_run(tui);     /* live on a TTY; returns at once headless */

    gptps_shutdown(engine); /* drains remaining work */
    gptps_tui_close(tui);   /* after shutdown */
    printf("dashboard demo done.\n");
    return 0;
}
