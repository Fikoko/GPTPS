/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_tui_order.c - the dashboard's handle table (KPI FULL) against EVERY order the
 * engine's threads can deliver one handle's events in: tests/test_stats_order.c's
 * scenarios, replayed against gptps_tui, with gptps_stats fed the same events beside it.
 *
 * gptps.h promises no order across threads, and tells an observer to key on the handle's
 * state, never on arrival order. The dashboard's exact pending / in-flight gauges and its
 * queue->finish latency (1.11) follow gptps_stats' rules, so the same matrix applies:
 * each scenario is one handle's events with the orderings that DO hold, and every linear
 * extension of that partial order is run. After every event the handle is in at most one
 * place (pending + in-flight <= 1, which also catches a gauge wrapping below zero) and the
 * dashboard's gauges equal gptps_stats'; at the end both gauges are zero, no slot or marker
 * is left behind, and a handle that FINISHED gave exactly one sample, of finish - queue.
 *
 * Then what the matrix cannot show: the orphan FIFO bounds the markers of terminal events
 * whose QUEUED never comes, and a full table is counted, never evicting.
 */
#include "../addons/gptps_tui.c"
#include "../addons/gptps_stats.c"
#include <stdio.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

typedef struct {
    const char      *lbl;
    gptps_event_kind kind;
    gptps_status     status;
    uint32_t         attempt;
    uint64_t         ts;          /* emit time on a plausible real timeline */
    uint32_t         after;       /* bitmask: events that must be delivered before this */
} ev_spec;

typedef struct {
    const char    *name;
    const ev_spec *ev;
    int            n;
} scenario;

static gptps     *eng;
static gptps_tui *tui;

/* A fresh handle table and row for the next order (the dashboard is reused: an install
 * per order would be thousands of engines). */
static void reset_tui(void)
{
    mu_lock(&tui->mu);
    memset(tui->tab, 0, (tui->tab_mask + 1) * sizeof *tui->tab);
    tui->tab_n = tui->tab_done = 0; tui->orph_n = tui->orph_head = 0;
    tui->g_pend = tui->g_run = 0; tui->trk_lb = 0;
    tui->q = tui->term = 0;
    if (tui->ntasks > 1) {
        tui_task *k = &tui->tasks[1];
        k->lat_n = k->lat_sum_ms = k->lat_max_ms = 0;
    }
    mu_unlock(&tui->mu);
}

static gptps_stats *fresh_stats(void)
{
    gptps_stats *s = (gptps_stats *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->total.struct_size = sizeof s->total;
    apx_mutex_init(&s->mu);
    return s;
}
static void discard_stats(gptps_stats *s)
{
    size_t i;
    for (i = 0; i < s->ntasks; ++i) free(s->tasks[i].name);
    free(s->tasks); free(s->tab);
    apx_mutex_destroy(&s->mu);
    free(s);
}

static void feed(gptps_stats *s, const ev_spec *e)
{
    gptps_event ev;
    memset(&ev, 0, sizeof ev);
    ev.struct_size = sizeof ev; ev.kind = e->kind; ev.handle = 7; ev.task_name = "t";
    ev.ts_ms = e->ts; ev.status = e->status; ev.attempt = e->attempt;
    tui_on_event(&ev, tui);
    stats_observe(&ev, s);
}

static void report(const scenario *sc, const int *order, int upto, const char *why)
{
    int i;
    printf("FAIL %s: %s after", sc->name, why);
    for (i = 0; i < upto; ++i) printf(" %s", sc->ev[order[i]].lbl);
    printf(" (pending=%llu in_flight=%llu slots=%lu markers=%lu samples=%llu)\n",
           (unsigned long long)tui->g_pend, (unsigned long long)tui->g_run, (unsigned long)tui->tab_n,
           (unsigned long)tui->tab_done, (unsigned long long)(tui->ntasks > 1 ? tui->tasks[1].lat_n : 0));
}

/* The sample a FINISHED handle gives: finish - queue, 0 when the stamps invert. */
static int expect(const scenario *sc, uint64_t *lat)
{
    int i, fin = -1, q = -1;
    for (i = 0; i < sc->n; ++i) {
        if (sc->ev[i].kind == GPTPS_EV_FINISHED) fin = i;
        if (sc->ev[i].kind == GPTPS_EV_QUEUED) q = i;
    }
    if (fin < 0) { *lat = 0; return 0; }
    *lat = sc->ev[fin].ts >= sc->ev[q].ts ? sc->ev[fin].ts - sc->ev[q].ts : 0;
    return 1;
}

/* Run one complete order; 1 if it broke an invariant (described if `say`). */
static int run_order(const scenario *sc, const int *order, int say)
{
    gptps_stats *s = fresh_stats();
    int i, bad = 0, samples;
    uint64_t lat;
    if (!s) { printf("FAIL out of memory\n"); return 1; }
    reset_tui();
    for (i = 0; i < sc->n && !bad; ++i) {
        feed(s, &sc->ev[order[i]]);
        if (tui->g_pend > 1 || tui->g_run > 1 || tui->g_pend + tui->g_run > 1) {
            if (say) report(sc, order, i + 1, "handle counted twice (or a gauge wrapped)");
            bad = 1;
        } else if (tui->g_pend != s->total.pending || tui->g_run != s->total.in_flight) {
            if (say) report(sc, order, i + 1, "gauges differ from gptps_stats'");
            bad = 1;
        }
    }
    if (!bad) {
        const char *why = NULL;
        tui_task *k = tui->ntasks > 1 ? &tui->tasks[1] : NULL;
        samples = expect(sc, &lat);
        if (tui->g_pend || tui->g_run)                             why = "gauges left nonzero";
        else if (tui->tab_n)                                       why = "slot or marker left behind";
        else if (!k || k->lat_n != (uint64_t)samples)              why = "latency samples";
        else if (samples && k->lat_sum_ms != lat)                  why = "latency value";
        if (why) { if (say) report(sc, order, sc->n, why); bad = 1; }
    }
    discard_stats(s);
    return bad;
}

/* Depth-first over every linear extension of the scenario's partial order. */
static void extend(const scenario *sc, int *order, int depth, uint32_t used, long *count, int *bad)
{
    int i;
    if (depth == sc->n) { ++*count; if (run_order(sc, order, *bad < 3)) ++*bad; return; }
    for (i = 0; i < sc->n; ++i) {
        uint32_t bit = 1u << i;
        if ((used & bit) || (sc->ev[i].after & ~used)) continue;
        order[depth] = i;
        extend(sc, order, depth + 1, used | bit, count, bad);
    }
}

#define B(i) (1u << (i))
#define Q_   { "Q",    GPTPS_EV_QUEUED,        GPTPS_OK,          0,  0, 0 }

static void matrix(void)
{
    /* tests/test_stats_order.c's scenarios, as they are there. Indices matter: `after`
     * masks name events by position. Q is never ordered against anything - the
     * submitting thread can be preempted before its emit. */
    static const ev_spec retry_finish[] = {
        Q_,
        { "S1",   GPTPS_EV_STARTED,  GPTPS_OK,    1, 10, 0 },
        { "F1",   GPTPS_EV_FAILED,   GPTPS_E_TASK, 1, 20, B(1) },
        { "R2",   GPTPS_EV_RETRIED,  GPTPS_E_TASK, 2, 21, B(2) },
        { "S2",   GPTPS_EV_STARTED,  GPTPS_OK,    2, 22, B(2) },
        { "FIN2", GPTPS_EV_FINISHED, GPTPS_OK,    2, 30, B(4) },
    };
    static const ev_spec retry_dead[] = {
        Q_,
        { "S1",  GPTPS_EV_STARTED,       GPTPS_OK,    1, 10, 0 },
        { "F1",  GPTPS_EV_FAILED,        GPTPS_E_TASK, 1, 20, B(1) },
        { "R2",  GPTPS_EV_RETRIED,       GPTPS_E_TASK, 2, 21, B(2) },
        { "S2",  GPTPS_EV_STARTED,       GPTPS_OK,    2, 22, B(2) },
        { "F2",  GPTPS_EV_FAILED,        GPTPS_E_TASK, 2, 30, B(4) },
        { "DL2", GPTPS_EV_DEAD_LETTERED, GPTPS_E_TASK, 2, 31, B(3) | B(5) },
    };
    static const ev_spec two_retries[] = {
        Q_,
        { "S1",   GPTPS_EV_STARTED,  GPTPS_OK,    1, 10, 0 },
        { "F1",   GPTPS_EV_FAILED,   GPTPS_E_TASK, 1, 20, B(1) },
        { "R2",   GPTPS_EV_RETRIED,  GPTPS_E_TASK, 2, 21, B(2) },
        { "S2",   GPTPS_EV_STARTED,  GPTPS_OK,    2, 22, B(2) },
        { "F2",   GPTPS_EV_FAILED,   GPTPS_E_TASK, 2, 30, B(4) },
        { "R3",   GPTPS_EV_RETRIED,  GPTPS_E_TASK, 3, 31, B(3) | B(5) },
        { "S3",   GPTPS_EV_STARTED,  GPTPS_OK,    3, 32, B(3) | B(5) },
        { "FIN3", GPTPS_EV_FINISHED, GPTPS_OK,    3, 40, B(7) },
    };
    static const ev_spec cancel_parked[] = {
        Q_,
        { "S1", GPTPS_EV_STARTED, GPTPS_OK,          1, 10, 0 },
        { "F1", GPTPS_EV_FAILED,  GPTPS_E_TASK,      1, 20, B(1) },
        { "R2", GPTPS_EV_RETRIED, GPTPS_E_TASK,      2, 21, B(2) },
        { "C2", GPTPS_EV_FAILED,  GPTPS_E_CANCELLED, 2, 21, B(2) },
    };
    static const ev_spec cancel_running[] = {
        Q_,
        { "S1", GPTPS_EV_STARTED, GPTPS_OK,          1, 10, 0 },
        { "F1", GPTPS_EV_FAILED,  GPTPS_E_TASK,      1, 20, B(1) },
        { "R2", GPTPS_EV_RETRIED, GPTPS_E_TASK,      2, 21, B(2) },
        { "S2", GPTPS_EV_STARTED, GPTPS_OK,          2, 22, B(2) },
        { "C2", GPTPS_EV_FAILED,  GPTPS_E_CANCELLED, 2, 30, B(4) },
    };
    static const ev_spec retry_denied[] = {
        Q_,
        { "S1",  GPTPS_EV_STARTED,       GPTPS_OK,     1, 10, 0 },
        { "F1",  GPTPS_EV_FAILED,        GPTPS_E_TASK,  1, 20, B(1) },
        { "R2",  GPTPS_EV_RETRIED,       GPTPS_E_TASK,  2, 21, B(2) },
        { "DL2", GPTPS_EV_DEAD_LETTERED, GPTPS_E_DENIED, 2, 21, B(3) },
    };
    static const ev_spec cancel_queued[] = {
        Q_,
        { "C1", GPTPS_EV_FAILED, GPTPS_E_CANCELLED, 1, 5, 0 },
    };
    static const ev_spec plain[] = {
        Q_,
        { "S1",   GPTPS_EV_STARTED,  GPTPS_OK, 1, 10, 0 },
        { "FIN1", GPTPS_EV_FINISHED, GPTPS_OK, 1, 20, B(1) },
    };
    static const ev_spec dead_once[] = {
        Q_,
        { "S1",  GPTPS_EV_STARTED,       GPTPS_OK,    1, 10, 0 },
        { "F1",  GPTPS_EV_FAILED,        GPTPS_E_TASK, 1, 20, B(1) },
        { "DL1", GPTPS_EV_DEAD_LETTERED, GPTPS_E_TASK, 1, 21, B(2) },
    };
    static const ev_spec dropped_once[] = {            /* on_failure = drop (not in stats' set) */
        Q_,
        { "S1",  GPTPS_EV_STARTED, GPTPS_OK,    1, 10, 0 },
        { "F1",  GPTPS_EV_FAILED,  GPTPS_E_TASK, 1, 20, B(1) },
        { "DR1", GPTPS_EV_DROPPED, GPTPS_E_TASK, 1, 21, B(2) },
    };
    static const ev_spec requeue[] = {
        Q_,
        { "S1",  GPTPS_EV_STARTED,       GPTPS_OK,       1, 10, 0 },
        { "F1",  GPTPS_EV_FAILED,        GPTPS_E_TASK,    1, 20, B(1) },
        { "S1b", GPTPS_EV_STARTED,       GPTPS_OK,       1, 30, B(2) },
        { "F1b", GPTPS_EV_FAILED,        GPTPS_E_TASK,    1, 40, B(3) },
        { "DL",  GPTPS_EV_DEAD_LETTERED, GPTPS_E_SHUTDOWN, 1, 41, B(4) },
    };
    static const ev_spec requeue_retry[] = {
        Q_,
        { "S1",    GPTPS_EV_STARTED,  GPTPS_OK,    1, 10, 0 },
        { "F1",    GPTPS_EV_FAILED,   GPTPS_E_TASK, 1, 20, B(1) },
        { "R2",    GPTPS_EV_RETRIED,  GPTPS_E_TASK, 2, 21, B(2) },
        { "S2",    GPTPS_EV_STARTED,  GPTPS_OK,    2, 22, B(2) },
        { "F2",    GPTPS_EV_FAILED,   GPTPS_E_TASK, 2, 30, B(4) },
        { "S1b",   GPTPS_EV_STARTED,  GPTPS_OK,    1, 40, B(3) | B(5) },
        { "F1b",   GPTPS_EV_FAILED,   GPTPS_E_TASK, 1, 50, B(6) },
        { "R2b",   GPTPS_EV_RETRIED,  GPTPS_E_TASK, 2, 51, B(7) },
        { "S2b",   GPTPS_EV_STARTED,  GPTPS_OK,    2, 52, B(7) },
        { "FIN2b", GPTPS_EV_FINISHED, GPTPS_OK,    2, 60, B(9) },
    };
    static const ev_spec requeue_ok[] = {
        Q_,
        { "S1",    GPTPS_EV_STARTED,  GPTPS_OK,    1, 10, 0 },
        { "F1",    GPTPS_EV_FAILED,   GPTPS_E_TASK, 1, 20, B(1) },
        { "R2",    GPTPS_EV_RETRIED,  GPTPS_E_TASK, 2, 21, B(2) },
        { "S2",    GPTPS_EV_STARTED,  GPTPS_OK,    2, 22, B(2) },
        { "F2",    GPTPS_EV_FAILED,   GPTPS_E_TASK, 2, 30, B(4) },
        { "S1b",   GPTPS_EV_STARTED,  GPTPS_OK,    1, 40, B(3) | B(5) },
        { "FIN1b", GPTPS_EV_FINISHED, GPTPS_OK,    1, 50, B(6) },
    };
    static const ev_spec requeue_cancel[] = {
        Q_,
        { "S1",  GPTPS_EV_STARTED, GPTPS_OK,          1, 10, 0 },
        { "F1",  GPTPS_EV_FAILED,  GPTPS_E_TASK,      1, 20, B(1) },
        { "R2",  GPTPS_EV_RETRIED, GPTPS_E_TASK,      2, 21, B(2) },
        { "S2",  GPTPS_EV_STARTED, GPTPS_OK,          2, 22, B(2) },
        { "F2",  GPTPS_EV_FAILED,  GPTPS_E_TASK,      2, 30, B(4) },
        { "S1b", GPTPS_EV_STARTED, GPTPS_OK,          1, 40, B(3) | B(5) },
        { "F1b", GPTPS_EV_FAILED,  GPTPS_E_TASK,      1, 50, B(6) },
        { "R2b", GPTPS_EV_RETRIED, GPTPS_E_TASK,      2, 51, B(7) },
        { "C2b", GPTPS_EV_FAILED,  GPTPS_E_CANCELLED, 2, 51, B(7) },
    };
    static const ev_spec stamps_q[] = {
        { "Q",    GPTPS_EV_QUEUED,   GPTPS_OK, 0, 15, 0 },
        { "S1",   GPTPS_EV_STARTED,  GPTPS_OK, 1, 10, 0 },
        { "FIN1", GPTPS_EV_FINISHED, GPTPS_OK, 1, 20, B(1) },
    };
    static const ev_spec stamps_r[] = {
        Q_,
        { "S1",   GPTPS_EV_STARTED,  GPTPS_OK,    1, 10, 0 },
        { "F1",   GPTPS_EV_FAILED,   GPTPS_E_TASK, 1, 20, B(1) },
        { "R2",   GPTPS_EV_RETRIED,  GPTPS_E_TASK, 2, 23, B(2) },
        { "S2",   GPTPS_EV_STARTED,  GPTPS_OK,    2, 22, B(2) },
        { "FIN2", GPTPS_EV_FINISHED, GPTPS_OK,    2, 30, B(4) },
    };
    static const scenario all[] = {
        { "retry, then finish",        retry_finish,   6 },
        { "retry, then dead-letter",   retry_dead,     7 },
        { "two retries",               two_retries,    9 },
        { "cancel the parked retry",   cancel_parked,  5 },
        { "cancel the running retry",  cancel_running, 6 },
        { "retry denied",              retry_denied,   5 },
        { "cancel while queued",       cancel_queued,  2 },
        { "plain success",             plain,          3 },
        { "fail, dead-letter",         dead_once,      4 },
        { "fail, dropped",             dropped_once,   4 },
        { "REQUEUE, then shutdown",    requeue,        6 },
        { "REQUEUE with retries",      requeue_retry, 11 },
        { "REQUEUE, then success",     requeue_ok,     8 },
        { "REQUEUE, cancel parked",    requeue_cancel, 10 },
        { "QUEUED stamped late",       stamps_q,       3 },
        { "RETRIED stamped late",      stamps_r,       6 },
    };
    size_t k;
    long total = 0;
    for (k = 0; k < sizeof all / sizeof all[0]; ++k) {
        int order[16], bad = 0;
        long count = 0;
        extend(&all[k], order, 0, 0, &count, &bad);
        total += count;
        if (bad) { printf("%s: %d of %ld orders broke the dashboard\n", all[k].name, bad, count); fails += bad; }
    }
    printf("handle-table orders: %ld across %lu scenarios\n", total, (unsigned long)(sizeof all / sizeof all[0]));
}

static void ev(int kind, gptps_handle h, gptps_status st, uint32_t att)
{
    gptps_event e;
    memset(&e, 0, sizeof e);
    e.struct_size = sizeof e; e.kind = (gptps_event_kind)kind; e.handle = h; e.task_name = "t";
    e.ts_ms = 1; e.status = st; e.attempt = att;
    tui_on_event(&e, tui);
}

/* Terminal events whose QUEUED never comes (work submitted before the table existed,
 * an always-up service's runs after the first) leave markers; the newest 64 are kept,
 * the rest dropped and counted - 10,000 of them cannot grow the table. */
static void orphans_bounded(void)
{
    gptps_handle h;
    uint64_t exp0;
    size_t most = 0;
    reset_tui();
    exp0 = tui->h_orphans;
    for (h = 1; h <= 10000; ++h) {
        ev(GPTPS_EV_STARTED, h, GPTPS_OK, 1);                 /* a run with no QUEUED */
        ev(GPTPS_EV_FINISHED, h, GPTPS_OK, 1);
        if (tui->tab_n > most) most = tui->tab_n;
    }
    CHECK(most <= TUI_ORPHANS + 1 && tui->tab_done == TUI_ORPHANS && tui->tab_n == TUI_ORPHANS);
    CHECK(tui->h_orphans - exp0 == 10000 - TUI_ORPHANS);
    CHECK(tui->g_run == 0 && tui->g_pend == 0);
    ev(GPTPS_EV_QUEUED, 10000, GPTPS_OK, 0);                  /* a late QUEUED still finds its marker */
    CHECK(tui->tab_n == TUI_ORPHANS - 1 && tui->g_pend == 0);
}

/* A full table: QUEUEDs past the window are not tracked - counted - and the live ones
 * are never evicted; every terminal kind frees what it held. */
static void full_table(void)
{
    gptps_handle h;
    int win = tui->lat_cap;
    uint64_t u0;
    reset_tui();
    u0 = tui->h_untracked;
    for (h = 1; h <= (gptps_handle)win + 5; ++h) ev(GPTPS_EV_QUEUED, h, GPTPS_OK, 0);
    CHECK(tui->g_pend == (uint64_t)win && tui->h_untracked - u0 == 5 && tui->trk_lb == 1);
    for (h = 1; h <= (gptps_handle)win; ++h) {                /* every one still there */
        tui_slot *s = trk_find(tui, h);
        CHECK(s && s->state == TS_PENDING);
    }
    for (h = 1; h <= (gptps_handle)win; ++h) {                /* each kind of end, round robin */
        switch (h % 4) {
        case 0: ev(GPTPS_EV_STARTED, h, GPTPS_OK, 1); ev(GPTPS_EV_FINISHED, h, GPTPS_OK, 1); break;
        case 1: ev(GPTPS_EV_FAILED, h, GPTPS_E_CANCELLED, 1); break;
        case 2: ev(GPTPS_EV_STARTED, h, GPTPS_OK, 1); ev(GPTPS_EV_FAILED, h, GPTPS_E_TASK, 1);
                ev(GPTPS_EV_DEAD_LETTERED, h, GPTPS_E_TASK, 1); break;
        default: ev(GPTPS_EV_STARTED, h, GPTPS_OK, 1); ev(GPTPS_EV_FAILED, h, GPTPS_E_TASK, 1);
                ev(GPTPS_EV_DROPPED, h, GPTPS_E_TASK, 1); break;
        }
    }
    CHECK(tui->g_pend == 0 && tui->g_run == 0);
    CHECK(tui->tab_n - tui->tab_done == 0);                   /* no live entry left */
}

int main(void)
{
    gptps_config cfg;
    gptps_tui_config tc;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1; cfg.mode = GPTPS_RUN_MANUAL;
    if (gptps_open_ex(&cfg, &eng) != GPTPS_OK || !eng) { printf("FAIL open\n"); return 1; }
    memset(&tc, 0, sizeof tc);
    tc.struct_size = sizeof tc; tc.kpi = GPTPS_TUI_KPI_FULL; tc.latency_window = 16;
    tui = gptps_tui_install(eng, &tc);
    if (!tui || !tui->tab) { printf("FAIL install\n"); return 1; }
    matrix();
    orphans_bounded();
    full_table();
    gptps_shutdown(eng);
    gptps_tui_close(tui);
    if (fails) { printf("%d tui order check(s) FAILED\n", fails); return 1; }
    printf("all tui order checks passed\n");
    return 0;
}
