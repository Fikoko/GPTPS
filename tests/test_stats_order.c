/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_stats_order.c - gptps_stats against EVERY order the engine's threads can
 * deliver one handle's events in.
 *
 * gptps.h promises no order across threads, and tells an observer to key on the
 * handle's state, never on arrival order. That is a claim about all interleavings, so
 * test all of them. A real engine produces most of these orders only under load, and
 * once RETRIED is published before its retry is re-admitted it stops producing some
 * altogether - which is exactly when a stats bug that depends on them would go
 * unnoticed. So this drives the observer directly: the add-on is compiled in (first,
 * since it sets its own feature macros) and fed synthetic events.
 *
 * Each scenario is one handle's events with the orderings that DO hold (one thread's
 * events stay in order, and an event never precedes the one it is derived from); every
 * linear extension of that partial order is run. After every event the handle must be
 * in at most one place (pending + in_flight <= 1, which also catches a gauge wrapping
 * below zero); at the end both gauges are zero, no slot is left behind, and every
 * attempt that ran has its run sample and its wait sample.
 */
#include "../addons/gptps_stats.c"
#include <stdio.h>

static int fails = 0;

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
    uint64_t       runs, waits;   /* expected run / wait samples; waits UINT64_MAX = skip */
} scenario;

static gptps_stats *fresh(void)
{
    gptps_stats *s = (gptps_stats *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->total.struct_size = sizeof s->total;
    apx_mutex_init(&s->mu);
    return s;
}

static void discard(gptps_stats *s)
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
    stats_observe(&ev, s);
}

static void report(const scenario *sc, const int *order, const char *why, const gptps_stats *s)
{
    int i;
    printf("FAIL %s: %s after", sc->name, why);
    for (i = 0; i < sc->n; ++i) printf(" %s", sc->ev[order[i]].lbl);
    printf(" (pending=%llu in_flight=%llu slots=%lu runs=%llu waits=%llu)\n",
           (unsigned long long)s->total.pending, (unsigned long long)s->total.in_flight,
           (unsigned long)s->tabn, (unsigned long long)s->total.run_samples,
           (unsigned long long)s->total.wait_samples);
}

/* Run one complete order; 1 if it broke an invariant (described if `say`). */
static int run_order(const scenario *sc, const int *order, int say)
{
    gptps_stats *s = fresh();
    int i, bad = 0;
    if (!s) { printf("FAIL out of memory\n"); return 1; }
    for (i = 0; i < sc->n && !bad; ++i) {
        feed(s, &sc->ev[order[i]]);
        if (s->total.pending > 1 || s->total.in_flight > 1 ||
            s->total.pending + s->total.in_flight > 1) {
            if (say) report(sc, order, "handle counted twice (or a gauge wrapped)", s);
            bad = 1;
        }
    }
    if (!bad) {
        const char *why = NULL;
        if (s->total.pending || s->total.in_flight)               why = "gauges left nonzero";
        else if (s->tabn)                                          why = "slot left behind";
        else if (s->total.run_samples != sc->runs)                 why = "run samples";
        else if (sc->waits != UINT64_MAX && s->total.wait_samples != sc->waits) why = "wait samples";
        if (why) { if (say) report(sc, order, why, s); bad = 1; }
    }
    discard(s);
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

int main(void)
{
    /* Indices matter: `after` masks name events by position. Q is never ordered
     * against anything - the submitting thread can be preempted before its emit. */
    static const ev_spec retry_finish[] = {           /* fail once, then succeed */
        Q_,
        { "S1",   GPTPS_EV_STARTED,  GPTPS_OK,    1, 10, 0 },
        { "F1",   GPTPS_EV_FAILED,   GPTPS_E_TASK, 1, 20, B(1) },
        { "R2",   GPTPS_EV_RETRIED,  GPTPS_E_TASK, 2, 21, B(2) },
        { "S2",   GPTPS_EV_STARTED,  GPTPS_OK,    2, 22, B(2) },
        { "FIN2", GPTPS_EV_FINISHED, GPTPS_OK,    2, 30, B(4) },
    };
    static const ev_spec retry_dead[] = {             /* fail twice, dead-letter */
        Q_,
        { "S1",  GPTPS_EV_STARTED,       GPTPS_OK,    1, 10, 0 },
        { "F1",  GPTPS_EV_FAILED,        GPTPS_E_TASK, 1, 20, B(1) },
        { "R2",  GPTPS_EV_RETRIED,       GPTPS_E_TASK, 2, 21, B(2) },
        { "S2",  GPTPS_EV_STARTED,       GPTPS_OK,    2, 22, B(2) },
        { "F2",  GPTPS_EV_FAILED,        GPTPS_E_TASK, 2, 30, B(4) },
        { "DL2", GPTPS_EV_DEAD_LETTERED, GPTPS_E_TASK, 2, 31, B(3) | B(5) },
    };
    static const ev_spec two_retries[] = {            /* fail twice, then succeed */
        Q_,
        { "S1",   GPTPS_EV_STARTED,  GPTPS_OK,    1, 10, 0 },
        { "F1",   GPTPS_EV_FAILED,   GPTPS_E_TASK, 1, 20, B(1) },
        { "R2",   GPTPS_EV_RETRIED,  GPTPS_E_TASK, 2, 21, B(2) },
        { "S2",   GPTPS_EV_STARTED,  GPTPS_OK,    2, 22, B(2) },
        { "F2",   GPTPS_EV_FAILED,   GPTPS_E_TASK, 2, 30, B(4) },
        /* the dispatcher publishes pass N's events before it runs pass N+1 */
        { "R3",   GPTPS_EV_RETRIED,  GPTPS_E_TASK, 3, 31, B(3) | B(5) },
        { "S3",   GPTPS_EV_STARTED,  GPTPS_OK,    3, 32, B(3) | B(5) },
        { "FIN3", GPTPS_EV_FINISHED, GPTPS_OK,    3, 40, B(7) },
    };
    static const ev_spec cancel_parked[] = {          /* cancel while the retry waits */
        Q_,
        { "S1", GPTPS_EV_STARTED, GPTPS_OK,          1, 10, 0 },
        { "F1", GPTPS_EV_FAILED,  GPTPS_E_TASK,      1, 20, B(1) },
        { "R2", GPTPS_EV_RETRIED, GPTPS_E_TASK,      2, 21, B(2) },
        { "C2", GPTPS_EV_FAILED,  GPTPS_E_CANCELLED, 2, 21, B(2) },
    };
    static const ev_spec cancel_running[] = {         /* cancel the retried attempt mid-run */
        Q_,
        { "S1", GPTPS_EV_STARTED, GPTPS_OK,          1, 10, 0 },
        { "F1", GPTPS_EV_FAILED,  GPTPS_E_TASK,      1, 20, B(1) },
        { "R2", GPTPS_EV_RETRIED, GPTPS_E_TASK,      2, 21, B(2) },
        { "S2", GPTPS_EV_STARTED, GPTPS_OK,          2, 22, B(2) },
        { "C2", GPTPS_EV_FAILED,  GPTPS_E_CANCELLED, 2, 30, B(4) },
    };
    static const ev_spec retry_denied[] = {           /* the retry is denied at re-admission */
        Q_,
        { "S1",  GPTPS_EV_STARTED,       GPTPS_OK,     1, 10, 0 },
        { "F1",  GPTPS_EV_FAILED,        GPTPS_E_TASK,  1, 20, B(1) },
        { "R2",  GPTPS_EV_RETRIED,       GPTPS_E_TASK,  2, 21, B(2) },
        { "DL2", GPTPS_EV_DEAD_LETTERED, GPTPS_E_DENIED, 2, 21, B(3) },
    };
    static const ev_spec cancel_queued[] = {          /* cancelled before it ever ran */
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
    /* REQUEUE: exhausted retries re-admit silently at attempt 1, and the terminal event
     * comes only when shutdown dead-letters it. Its wait count is not checked: a QUEUED
     * that outlives a whole attempt AND the requeue delay leaves two waits owed to one
     * event, and only the later is kept. The gauges must still come back to zero. */
    static const ev_spec requeue[] = {
        Q_,
        { "S1",  GPTPS_EV_STARTED,       GPTPS_OK,       1, 10, 0 },
        { "F1",  GPTPS_EV_FAILED,        GPTPS_E_TASK,    1, 20, B(1) },
        { "S1b", GPTPS_EV_STARTED,       GPTPS_OK,       1, 30, B(2) },
        { "F1b", GPTPS_EV_FAILED,        GPTPS_E_TASK,    1, 40, B(3) },
        { "DL",  GPTPS_EV_DEAD_LETTERED, GPTPS_E_SHUTDOWN, 1, 41, B(4) },
    };
    /* REQUEUE with retries: the cycle restarts at attempt 1 with no event, so the
     * attempts a slot has seen - and the RETRIEDs it is owed - must restart with it.
     * The re-admission follows the drain of F2, after R2 was published. */
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
    static const ev_spec requeue_ok[] = {             /* ...and the next cycle succeeds */
        Q_,
        { "S1",    GPTPS_EV_STARTED,  GPTPS_OK,    1, 10, 0 },
        { "F1",    GPTPS_EV_FAILED,   GPTPS_E_TASK, 1, 20, B(1) },
        { "R2",    GPTPS_EV_RETRIED,  GPTPS_E_TASK, 2, 21, B(2) },
        { "S2",    GPTPS_EV_STARTED,  GPTPS_OK,    2, 22, B(2) },
        { "F2",    GPTPS_EV_FAILED,   GPTPS_E_TASK, 2, 30, B(4) },
        { "S1b",   GPTPS_EV_STARTED,  GPTPS_OK,    1, 40, B(3) | B(5) },
        { "FIN1b", GPTPS_EV_FINISHED, GPTPS_OK,    1, 50, B(6) },
    };
    static const ev_spec requeue_cancel[] = {         /* ...and cancelled while parked */
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
    /* Stamps come from different threads, so an event can arrive in order yet carry
     * an earlier time than the one before it. The wait is then 0, not missing. */
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
        { "retry, then finish",        retry_finish,   6, 2, 2 },
        { "retry, then dead-letter",   retry_dead,     7, 2, 2 },
        { "two retries",               two_retries,    9, 3, 3 },
        { "cancel the parked retry",   cancel_parked,  5, 1, 1 },
        { "cancel the running retry",  cancel_running, 6, 2, 2 },
        { "retry denied",              retry_denied,   5, 1, 1 },
        { "cancel while queued",       cancel_queued,  2, 0, 0 },
        { "plain success",             plain,          3, 1, 1 },
        { "fail, dead-letter",         dead_once,      4, 1, 1 },
        { "REQUEUE, then shutdown",    requeue,        6, 2, UINT64_MAX },
        { "REQUEUE with retries",      requeue_retry, 11, 4, UINT64_MAX },
        { "REQUEUE, then success",     requeue_ok,     8, 3, UINT64_MAX },
        { "REQUEUE, cancel parked",    requeue_cancel, 10, 3, UINT64_MAX },
        { "QUEUED stamped late",       stamps_q,       3, 1, 1 },
        { "RETRIED stamped late",      stamps_r,       6, 2, 2 },
    };
    size_t k;
    long total = 0;
    for (k = 0; k < sizeof all / sizeof all[0]; ++k) {
        int order[16], bad = 0;
        long count = 0;
        extend(&all[k], order, 0, 0, &count, &bad);
        total += count;
        if (bad) { printf("%s: %d of %ld orders broke stats\n", all[k].name, bad, count); fails += bad; }
    }
    if (fails) { printf("%d stats order(s) FAILED\n", fails); return 1; }
    printf("all stats orders passed (%ld orders across %lu scenarios)\n",
           total, (unsigned long)(sizeof all / sizeof all[0]));
    return 0;
}
