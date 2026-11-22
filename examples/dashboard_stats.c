/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * dashboard_stats.c - the dashboard SHOWING what a host's gptps_stats collects, through
 * gptps_tui_source (1.12). The host already runs gptps_stats (for its own export, say), so
 * the dashboard need not count the same events again: it is installed at tui.kpi = off -
 * one flag read an event, no lock - and a frame reads the totals, the gauges and every task
 * row from gptps_stats_snapshot, all under ONE stats lock.
 *
 * The adapter below (stats_source_*) is the whole of it, and it lives here, not in the
 * dashboard: gptps_tui does not link gptps_stats, so neither add-on depends on the other.
 * What it shows from gptps_stats:
 *   - the header: queued, started, finished, failed (cancelled), retried, dead, dropped,
 *     done/s and peak (from the source's in-flight), pending / in-flight - EXACT, or '>='
 *     while gptps_stats says its gauges undercount (untracked handles, or work admitted
 *     before a resume: gptps_stats_health);
 *   - TASKS: one row per task type, in first-seen order, then '(other)'; avg ms / max ms are
 *     gptps_stats's RUN time (STARTED -> end), not the dashboard's own queue-to-finish.
 * Ownership: the stats object stays the host's. The dashboard has no way to reset or close
 * it; close order is gptps_shutdown, gptps_tui_close, then gptps_stats_close.
 *
 * Headless (no TTY) it runs a MANUAL engine to completion, renders one frame, checks it
 * against gptps_stats and exits 0 - it is a ctest. On a terminal it also runs the live loop
 * ('q' quits).
 *
 *   cc dashboard_stats.c gptps.c addons/gptps_tui.c addons/gptps_stats.c -lpthread -ldl
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L   /* isatty, fileno */
#endif
#include "gptps.h"
#include "gptps_stats.h"
#include "gptps_tui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#  include <io.h>
#  define dash_isatty(f) _isatty(_fileno(f))
#else
#  include <unistd.h>
#  define dash_isatty(f) isatty(fileno(f))
#endif

/* ---- the adapter: gptps_stats -> gptps_tui_source ------------------------------------ */
typedef struct {
    gptps_stats     *stats;    /* the host's; never reset or closed here */
    gptps_stats_row *rows;     /* gptps_stats_snapshot's rows, grown to the frame's cap */
    size_t           cap;
} stats_source;

static gptps_status stats_source_read(void *ctx, gptps_tui_source_totals *totals,
                                      gptps_tui_source_row *rows, size_t row_size,
                                      size_t cap, size_t *nrows)
{
    stats_source *ss = (stats_source *)ctx;
    gptps_stats_counters c;
    gptps_stats_health_info h;
    gptps_tui_source_totals tt;
    size_t need = 0, i, n, tn;
    gptps_status st;
    if (cap > ss->cap) {                 /* on the frame's thread, never the event path */
        gptps_stats_row *nr = (gptps_stats_row *)realloc(ss->rows, cap * sizeof *nr);
        if (!nr) return GPTPS_E_NOMEM;   /* the dashboard counts it and shows its own counts */
        ss->rows = nr; ss->cap = cap;
    }
    memset(&c, 0, sizeof c); c.struct_size = sizeof c;
    memset(&h, 0, sizeof h); h.struct_size = sizeof h;
    st = gptps_stats_snapshot(ss->stats, &c, &h, cap ? ss->rows : NULL, sizeof *ss->rows, cap, &need);
    if (st != GPTPS_OK && st != GPTPS_E_FULL) return st;   /* E_FULL: only cap rows fit - "+N more" */

    memset(&tt, 0, sizeof tt);
    tt.queued = c.queued; tt.started = c.started; tt.finished = c.finished; tt.failed = c.failed;
    tt.retried = c.retried; tt.dead = c.dead_lettered; tt.dropped = c.dropped; tt.cancelled = c.cancelled;
    tt.pending = c.pending; tt.in_flight = c.in_flight;
    /* gptps_stats's gauges are exact for the handles it tracks: a lower bound when one was not
     * tracked, or while work admitted before a resume is not in them */
    tt.gauges = (h.handles_untracked || h.gauges_since_ms || !h.enabled) ? GPTPS_TUI_GAUGES_LOWER_BOUND
                                                                         : GPTPS_TUI_GAUGES_EXACT;
    tn = totals->struct_size < sizeof tt ? totals->struct_size : sizeof tt;   /* what the dashboard's has */
    if (tn > sizeof(size_t)) memcpy((char *)totals + sizeof(size_t), (char *)&tt + sizeof(size_t), tn - sizeof(size_t));

    n = need < cap ? need : cap;
    for (i = 0; i < n; ++i) {
        const gptps_stats_row *sr = &ss->rows[i];
        gptps_tui_source_row o;
        memset(&o, 0, sizeof o);
        o.struct_size = sizeof o;
        snprintf(o.name, sizeof o.name, "%s", sr->name);
        o.started = sr->c.started; o.finished = sr->c.finished; o.failed = sr->c.failed;
        o.dead = sr->c.dead_lettered; o.dropped = sr->c.dropped;
        if (sr->c.run_samples) {         /* run time: gptps_stats keeps wait and run apart */
            o.flags |= GPTPS_TUI_SRC_ROW_LATENCY;
            o.lat_n = sr->c.run_samples; o.lat_sum_ms = sr->c.run_ms_sum; o.lat_max_ms = sr->c.run_ms_max;
        }
        memcpy((char *)rows + i * row_size, &o, row_size < sizeof o ? row_size : sizeof o);
    }
    *nrows = need;
    return GPTPS_OK;
}

static void stats_source_free(stats_source *ss) { free(ss->rows); ss->rows = NULL; ss->cap = 0; }

/* ---- the demo ------------------------------------------------------------------------- */
static gptps_status resize(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_OK; }
static gptps_status upload(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_E_TASK; }

static int reg(gptps *e, const char *name, gptps_run_fn fn, uint32_t retries)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = fn; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = 1u << 16;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = retries;
    return gptps_register_task(e, &d) == GPTPS_OK;
}

static int fails;
#define EXPECT(c) do { if (!(c)) { fprintf(stderr, "dashboard_stats: FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

int main(void)
{
    gptps_config ecfg;
    gptps *e = NULL;
    gptps_stats *stats;
    gptps_tui *tui;
    gptps_tui_config cfg;
    gptps_tui_source src;
    gptps_tui_health_info th;
    gptps_stats_counters c;
    stats_source ss;
    static char frame[32768];
    gptps_handle hd;
    size_t ran;
    int i, live;

    memset(&ecfg, 0, sizeof ecfg);
    ecfg.struct_size = sizeof ecfg; ecfg.limits.struct_size = sizeof ecfg.limits;
    ecfg.limits.max_concurrent_tasks = 4; ecfg.limits.max_memory_bytes = 1u << 24;
    ecfg.mode = GPTPS_RUN_MANUAL;        /* deterministic: the host steps it */
    if (gptps_open_ex(&ecfg, &e) != GPTPS_OK) { fprintf(stderr, "open failed\n"); return 1; }

    stats = gptps_stats_install(e);      /* the host's own aggregation */
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.title = "GPTPS + gptps_stats";
    cfg.kpi = GPTPS_TUI_KPI_OFF;         /* the dashboard collects nothing of its own */
    cfg.clock = GPTPS_TUI_CLOCK_OFF;
    live = dash_isatty(stdout) && dash_isatty(stdin);
    cfg.color = live ? -1 : 0; cfg.interactive = live ? -1 : 0; cfg.unicode = live ? -1 : 0;
    tui = stats ? gptps_tui_install(e, &cfg) : NULL;
    if (!tui) { gptps_shutdown(e); gptps_stats_close(stats); fprintf(stderr, "install failed\n"); return 1; }

    EXPECT(reg(e, "resize", resize, 0));
    EXPECT(reg(e, "upload", upload, 1)); /* fails twice: one retry, then a dead letter */
    gptps_tui_add_task(tui, "resize", "Resize images", 'r', NULL, 0);

    memset(&ss, 0, sizeof ss);
    ss.stats = stats;
    memset(&src, 0, sizeof src);
    src.struct_size = sizeof src; src.ctx = &ss; src.read = stats_source_read; src.name = "gptps_stats";
    EXPECT(gptps_tui_set_source(tui, &src) == GPTPS_OK);

    for (i = 0; i < 20; ++i) EXPECT(gptps_submit(e, "resize", NULL, 0, &hd) == GPTPS_OK);
    for (i = 0; i < 3; ++i)  EXPECT(gptps_submit(e, "upload", NULL, 0, &hd) == GPTPS_OK);
    for (i = 0; i < 1000; ++i) { ran = 0; if (gptps_step(e, &ran) != GPTPS_OK || ran == 0) break; }

    /* one frame: every number is gptps_stats's, the label is the dashboard's */
    gptps_tui_render(tui, frame, sizeof frame);
    if (!live) fputs(frame, stdout);
    memset(&c, 0, sizeof c); c.struct_size = sizeof c;
    EXPECT(gptps_stats_total(stats, &c) == GPTPS_OK);
    EXPECT(c.finished == 20 && c.dead_lettered == 3 && c.retried == 3 && c.failed == 6);
    EXPECT(strstr(frame, "source:gptps_stats") != NULL);
    EXPECT(strstr(frame, "finished 20") != NULL && strstr(frame, "failed 6") != NULL &&
           strstr(frame, "dead 3") != NULL && strstr(frame, "retried 3") != NULL);
    EXPECT(strstr(frame, "queued 23  started 26  pending 0  in-flight 0") != NULL);   /* EXACT: no mark; a retry is no QUEUED */
    EXPECT(strstr(frame, "Resize images") != NULL && strstr(frame, "upload") != NULL);
    memset(&th, 0, sizeof th); th.struct_size = sizeof th;
    EXPECT(gptps_tui_health(tui, &th) == GPTPS_OK);
    EXPECT(th.source == 1 && th.source_reads == 1 && th.source_errors == 0 && th.source_rows == 2);

    if (live) gptps_tui_run(tui);        /* on a terminal: the live loop, polling the source */

    /* back to its own collection (at OFF: nothing counted) - the stats are untouched */
    EXPECT(gptps_tui_set_source(tui, NULL) == GPTPS_OK);
    gptps_tui_render(tui, frame, sizeof frame);
    EXPECT(strstr(frame, "source:") == NULL && strstr(frame, "finished 0") != NULL);

    gptps_shutdown(e);
    gptps_tui_close(tui);                /* after shutdown; it never resets or closes the stats */
    memset(&c, 0, sizeof c); c.struct_size = sizeof c;
    EXPECT(gptps_stats_total(stats, &c) == GPTPS_OK && c.finished == 20 && c.dead_lettered == 3);
    gptps_stats_close(stats);            /* the host's, last */
    stats_source_free(&ss);
    if (!fails) printf("dashboard_stats: the dashboard showed gptps_stats through gptps_tui_source.\n");
    return fails ? 1 : 0;
}
