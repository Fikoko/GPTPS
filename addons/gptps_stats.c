/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_stats.c - counters, gauges and latency on the observer seam.
 *
 * Structure: one observer, one lock. Per event: lock, one hash lookup on the handle
 * (for the gauges and latencies), two counter bumps (engine row + task row), unlock.
 * The handle table holds one small entry per item that is queued or in flight and is
 * bounded by the engine's own queue, so it costs what the engine already costs.
 *
 * Accounting, per handle, driven by the core's event contract (a ONE-SHOT handle
 * reaches exactly one terminal event; FAILED is per attempt):
 *
 *   QUEUED         -> PENDING            pending++
 *   STARTED        -> RUNNING            pending--  in_flight++   wait sample
 *   FINISHED       -> terminal           in_flight--              run sample
 *   FAILED         -> LIMBO              in_flight-- (or pending-- if it never
 *                                        started: cancelled in queue)  run sample
 *                     ...unless status == CANCELLED, which is terminal
 *   RETRIED        -> PENDING            pending++   (re-stamps the queue time)
 *                     ...unless the attempt it announces already started or ended:
 *                     a LATE RETRIED moves nothing (see "Event ORDER" below)
 *   DEAD_LETTERED  -> terminal           pending-- if still PENDING (denied /
 *   DROPPED                              shutdown-terminated in queue), else none
 *
 * An event for a handle this observer never saw QUEUED (work submitted before install)
 * bumps the totals only: with no state to move, adjusting a gauge would be a guess.
 *
 * The two handle shapes outside the one-terminal rule are safe here, but they make the
 * TOTALS mean something different from the gauges, which is worth knowing before you
 * graph them. A GPTPS_TASK_SERVICE handle emits a terminal event per RUN, so `finished`
 * counts service runs rather than service instances, and one long-lived instance can
 * dominate a totals column while `in_flight` correctly shows one. A
 * GPTPS_ON_FAILURE_REQUEUE item emits a per-attempt FAILED each time round and no
 * terminal event until shutdown dead-letters it, so it sits in LIMBO between attempts
 * and its `failed` count grows without a matching terminal. Neither corrupts a gauge -
 * every transition above is still driven by the state the handle is actually in - but
 * `queued == terminal` is an identity for one-shot work only.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "gptps_stats.h"
#include "addon_compat.h"   /* portable mutex */

#include <stdlib.h>
#include <string.h>

enum { ST_NONE = 0, ST_PENDING, ST_RUNNING, ST_LIMBO, ST_DONE };

typedef struct {
    gptps_handle handle;     /* 0 = empty slot */
    uint32_t     task;       /* index into s->tasks */
    uint8_t      state;
    uint8_t      seen_queued;/* this observer saw the handle's QUEUED */
    uint8_t      have_queued_ms, have_started_ms;
    uint64_t     queued_ms;  /* last QUEUED / RETRIED */
    uint64_t     started_ms; /* last STARTED */
    /* Retry bookkeeping, so a RETRIED is judged by the attempt it announces rather
     * than by when it arrives (see "Event ORDER" above stats_observe). */
    uint32_t     att;        /* highest attempt started or ended in this cycle; an
                              * attempt-1 STARTED (a REQUEUE) resets it */
    uint32_t     ann;        /* highest attempt a RETRIED has announced, this cycle */
    uint32_t     owe_r;      /* attempt whose STARTED outran its RETRIED: wait owed */
    uint8_t      owe_q;      /* attempt 1's STARTED outran QUEUED: wait owed ... */
    uint64_t     q_started_ms; /* ... measured from here */
} stats_slot;

/* An event of attempt N > 1 has arrived and the RETRIED announcing attempt N has not:
 * it is still on its way, so the slot must outlive a terminal event to meet it. */
#define OWES_RETRIED(sl) ((sl)->att > 1 && (sl)->ann < (sl)->att)

typedef struct {
    char                *name;
    gptps_stats_counters c;
} stats_task;

struct gptps_stats {
    gptps                *e;
    apx_mutex             mu;
    gptps_stats_counters  total;
    stats_task           *tasks;
    size_t                ntasks, taskcap;
    stats_slot           *tab;   /* open addressing, power-of-two, keyed by handle */
    size_t                tabcap, tabn;
};

/* ---- handle table (handles are never 0: the core starts at 1) ---- */
static size_t slot_hash(gptps_handle h, size_t cap)
{
    uint64_t x = (uint64_t)h;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;   /* murmur3 finalizer */
    return (size_t)x & (cap - 1);
}
static stats_slot *tab_find(gptps_stats *s, gptps_handle h)
{
    size_t i, cap = s->tabcap;
    if (!cap) return NULL;
    for (i = slot_hash(h, cap); ; i = (i + 1) & (cap - 1)) {
        if (s->tab[i].handle == h) return &s->tab[i];
        if (s->tab[i].handle == 0) return NULL;
    }
}
/* Tombstone-free deletion (backward shift) so lookups stay bounded. */
static void tab_del(gptps_stats *s, stats_slot *slot)
{
    size_t cap = s->tabcap, i = (size_t)(slot - s->tab), j = i;
    for (;;) {
        size_t k;
        j = (j + 1) & (cap - 1);
        if (s->tab[j].handle == 0) break;
        k = slot_hash(s->tab[j].handle, cap);
        /* can the entry at j move to i? yes iff its home k is not in (i, j] cyclically */
        if ((i <= j) ? (k <= i || k > j) : (k <= i && k > j)) { s->tab[i] = s->tab[j]; i = j; }
    }
    memset(&s->tab[i], 0, sizeof s->tab[i]);
    s->tabn -= 1;
}
static int tab_grow(gptps_stats *s)
{
    size_t ncap = s->tabcap ? s->tabcap * 2 : 64, i;
    stats_slot *nt = (stats_slot *)calloc(ncap, sizeof *nt);
    if (!nt) return -1;
    for (i = 0; i < s->tabcap; ++i) {
        stats_slot *o = &s->tab[i];
        if (o->handle) {
            size_t j;
            for (j = slot_hash(o->handle, ncap); nt[j].handle; j = (j + 1) & (ncap - 1)) { }
            nt[j] = *o;
        }
    }
    free(s->tab); s->tab = nt; s->tabcap = ncap;
    return 0;
}
static stats_slot *tab_insert(gptps_stats *s, gptps_handle h)
{
    size_t i;
    if ((s->tabn + 1) * 4 > s->tabcap * 3 && tab_grow(s) != 0) return NULL;   /* load <= 3/4 */
    for (i = slot_hash(h, s->tabcap); s->tab[i].handle; i = (i + 1) & (s->tabcap - 1))
        if (s->tab[i].handle == h) return &s->tab[i];
    memset(&s->tab[i], 0, sizeof s->tab[i]);
    s->tab[i].handle = h;
    s->tabn += 1;
    return &s->tab[i];
}

/* ---- task rows ---- */
static uint32_t task_index(gptps_stats *s, const char *name)
{
    size_t i;
    if (!name) name = "";
    for (i = 0; i < s->ntasks; ++i)
        if (strcmp(s->tasks[i].name, name) == 0) return (uint32_t)i;
    if (s->ntasks == s->taskcap) {
        size_t nc = s->taskcap ? s->taskcap * 2 : 8;
        stats_task *nt = (stats_task *)realloc(s->tasks, nc * sizeof *nt);
        if (!nt) return UINT32_MAX;
        s->tasks = nt; s->taskcap = nc;
    }
    s->tasks[s->ntasks].name = (char *)malloc(strlen(name) + 1);
    if (!s->tasks[s->ntasks].name) return UINT32_MAX;
    strcpy(s->tasks[s->ntasks].name, name);
    memset(&s->tasks[s->ntasks].c, 0, sizeof s->tasks[s->ntasks].c);
    s->tasks[s->ntasks].c.struct_size = sizeof(gptps_stats_counters);
    return (uint32_t)s->ntasks++;
}

/* ---- the observer ---- */
#define BUMP(field) do { s->total.field += 1; if (t) t->field += 1; } while (0)
#define GAUGE(field, d) do { s->total.field += (uint64_t)(d); if (t) t->field += (uint64_t)(d); } while (0)

static void sample_wait(gptps_stats *s, gptps_stats_counters *t, uint64_t ms)
{
    s->total.wait_samples += 1; s->total.wait_ms_sum += ms;
    if (ms > s->total.wait_ms_max) s->total.wait_ms_max = ms;
    if (t) { t->wait_samples += 1; t->wait_ms_sum += ms; if (ms > t->wait_ms_max) t->wait_ms_max = ms; }
}
static void sample_run(gptps_stats *s, gptps_stats_counters *t, uint64_t ms)
{
    s->total.run_samples += 1; s->total.run_ms_sum += ms;
    if (ms > s->total.run_ms_max) s->total.run_ms_max = ms;
    if (t) { t->run_samples += 1; t->run_ms_sum += ms; if (ms > t->run_ms_max) t->run_ms_max = ms; }
}

/* Event ORDER is not guaranteed across threads (see EVENT ORDER in gptps.h). Two
 * inversions reach this observer, and every transition below is keyed on the slot's
 * state and on the attempt an event names - never on the order events arrive in:
 *   - QUEUED comes from the submitting thread, so a fast task can report STARTED -
 *     or even FINISHED - before its own QUEUED. A terminal event that outruns QUEUED
 *     leaves a ST_DONE tombstone that the late QUEUED then clears.
 *   - RETRIED comes from the dispatcher, and attempt N can be cancelled - its
 *     terminal FAILED/E_CANCELLED emitted at once, on the cancelling thread - while
 *     the RETRIED announcing it is still being delivered. (An older engine could
 *     also re-admit a zero-backoff retry in the same pass, so attempt N could even
 *     start, finish or fail first; the engine no longer does, and nothing below
 *     depends on which.) Taken on arrival order, that late RETRIED reopened a handle
 *     already running or already over as PENDING, and the gauges never came back:
 *     pending and in_flight stuck above zero, a run sample lost, and after a
 *     terminal event a fresh slot leaked as well. So a RETRIED for an attempt this
 *     handle has already started or ended moves nothing, and a terminal event that
 *     outruns the RETRIED it is owed leaves a tombstone for that RETRIED to clear.
 * An attempt that starts before the event that queued it carries off its wait
 * sample, and that sample is taken when the late event arrives. (Work submitted
 * before attach never sends its QUEUED, so it leaves one small tombstone each - the
 * price of installing late.) */
static void finish_slot(gptps_stats *s, stats_slot *slot)
{
    if (slot->seen_queued && !OWES_RETRIED(slot)) tab_del(s, slot);
    else slot->state = ST_DONE;                /* wait for the late QUEUED / RETRIED */
}
static void note_attempt(stats_slot *slot, uint32_t attempt)
{
    if (attempt > slot->att) slot->att = attempt;
}

static void stats_observe(const gptps_event *ev, void *ud)
{
    gptps_stats *s = (gptps_stats *)ud;
    stats_slot *slot;
    gptps_stats_counters *t = NULL;
    uint32_t ti;

    apx_mutex_lock(&s->mu);
    slot = tab_find(s, ev->handle);
    /* The task row: from the slot if we have one (the name pointer is only valid for
     * this call, so the row index is what we keep), else by name. */
    ti = slot ? slot->task : task_index(s, ev->task_name);
    if (ti != UINT32_MAX) t = &s->tasks[ti].c;
    if (!slot) {
        slot = tab_insert(s, ev->handle);
        if (slot) { slot->task = ti; slot->state = ST_NONE; }
    }

    switch (ev->kind) {
    case GPTPS_EV_QUEUED:
        BUMP(queued);
        if (slot) {
            slot->seen_queued = 1;
            /* A STARTED that outran this QUEUED left its wait sample owed here: the
             * queue time was not known yet, so the STARTED arm below kept when it
             * started instead (owe_q). Take the sample now rather than drop it. This
             * event's ts was stamped when the submitting thread finally reached the
             * emit, at or after the real enqueue, so started - this is a LOWER bound on
             * the true wait; when even that inverts, the item started before its own
             * notification left the submitter and 0 is the only honest answer. Dropping
             * the sample instead biases the reported mean UP, because the items that
             * lose this race are exactly the ones that waited least. Placement is
             * load-bearing: tab_del() backward-shifts and invalidates `slot`, so the
             * sample must be taken first. */
            if (slot->owe_q) {
                sample_wait(s, t, (slot->q_started_ms > ev->ts_ms) ? slot->q_started_ms - ev->ts_ms : 0);
                slot->owe_q = 0;
            }
            if (slot->state == ST_DONE) {                               /* already over */
                if (!OWES_RETRIED(slot)) tab_del(s, slot);
                break;
            }
            if (slot->state == ST_NONE) { slot->state = ST_PENDING; GAUGE(pending, 1); }
            if (!slot->have_queued_ms) { slot->queued_ms = ev->ts_ms; slot->have_queued_ms = 1; }
        }
        break;
    case GPTPS_EV_STARTED:
        BUMP(started);
        if (slot) {
            if (slot->state == ST_PENDING) GAUGE(pending, -1);
            if (ev->attempt <= 1) { slot->ann = 0; slot->owe_r = 0; }  /* a new cycle: REQUEUE */
            slot->att = ev->attempt;
            slot->state = ST_RUNNING; slot->started_ms = ev->ts_ms; slot->have_started_ms = 1;
            GAUGE(in_flight, 1);
            /* The wait is measured from the event that queued THIS attempt. If that
             * event has not arrived yet, the sample is owed to it instead. */
            if (ev->attempt > 1 && slot->ann < ev->attempt) {
                slot->owe_r = ev->attempt;                 /* outran its RETRIED */
            } else if (!slot->have_queued_ms) {
                slot->owe_q = 1; slot->q_started_ms = ev->ts_ms;   /* outran its QUEUED */
            } else {    /* in order, but two threads' stamps can still invert: 0 */
                sample_wait(s, t, (ev->ts_ms > slot->queued_ms) ? ev->ts_ms - slot->queued_ms : 0);
            }
        }
        break;
    case GPTPS_EV_FINISHED:
        BUMP(finished); BUMP(terminal);
        if (slot) {
            note_attempt(slot, ev->attempt);
            if (slot->state == ST_RUNNING) {
                GAUGE(in_flight, -1);
                if (slot->have_started_ms && ev->ts_ms >= slot->started_ms)
                    sample_run(s, t, ev->ts_ms - slot->started_ms);
            }
            finish_slot(s, slot);
        }
        break;
    case GPTPS_EV_FAILED:
        BUMP(failed);
        if (ev->status == GPTPS_E_CANCELLED) { BUMP(cancelled); BUMP(terminal); }
        if (slot) {
            note_attempt(slot, ev->attempt);
            if (slot->state == ST_RUNNING) {
                GAUGE(in_flight, -1);
                if (slot->have_started_ms && ev->ts_ms >= slot->started_ms)
                    sample_run(s, t, ev->ts_ms - slot->started_ms);
            } else if (slot->state == ST_PENDING) {
                GAUGE(pending, -1);           /* cancelled before it ever started */
            }
            slot->state = ST_LIMBO;
            if (ev->status == GPTPS_E_CANCELLED) finish_slot(s, slot);
        }
        break;
    case GPTPS_EV_RETRIED:
        BUMP(retried);
        if (slot) {
            if (ev->attempt > slot->ann) slot->ann = ev->attempt;
            if (slot->owe_r && slot->owe_r == ev->attempt) {   /* its STARTED outran it */
                sample_wait(s, t, (slot->started_ms > ev->ts_ms) ? slot->started_ms - ev->ts_ms : 0);
                slot->owe_r = 0;
            }
            /* LATE: the attempt it announces has already started or ended, or the
             * handle is over. It moves no state - reopening the slot as PENDING is
             * what used to leave the gauges stuck - and it clears the tombstone a
             * terminal event left waiting for it (tab_del invalidates `slot`). */
            if (slot->att >= ev->attempt || slot->state == ST_DONE) {
                if (slot->state == ST_DONE && slot->seen_queued) tab_del(s, slot);
                break;
            }
            if (slot->state != ST_PENDING) GAUGE(pending, 1);
            slot->state = ST_PENDING; slot->queued_ms = ev->ts_ms; slot->have_queued_ms = 1;
        }
        break;
    case GPTPS_EV_DEAD_LETTERED:
    case GPTPS_EV_DROPPED:
        if (ev->kind == GPTPS_EV_DEAD_LETTERED) BUMP(dead_lettered); else BUMP(dropped);
        BUMP(terminal);
        if (slot) {
            note_attempt(slot, ev->attempt);
            if (slot->state == ST_PENDING) GAUGE(pending, -1);
            else if (slot->state == ST_RUNNING) GAUGE(in_flight, -1);   /* defensive */
            finish_slot(s, slot);
        }
        break;
    default:
        break;
    }
    apx_mutex_unlock(&s->mu);
}

/* ---- public ---- */
gptps_stats *gptps_stats_install(gptps *e)
{
    gptps_stats *s;
    if (!e) return NULL;
    s = (gptps_stats *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->e = e;
    s->total.struct_size = sizeof s->total;
    apx_mutex_init(&s->mu);
    if (gptps_register_observer(e, stats_observe, s) != GPTPS_OK) {
        apx_mutex_destroy(&s->mu); free(s); return NULL;
    }
    return s;
}

void gptps_stats_close(gptps_stats *s)
{
    size_t i;
    if (!s) return;
    /* No unregister: this runs AFTER gptps_shutdown, which freed the engine and its
     * observer list. See the contract in the header. */
    apx_mutex_destroy(&s->mu);
    for (i = 0; i < s->ntasks; ++i) free(s->tasks[i].name);
    free(s->tasks);
    free(s->tab);
    free(s);
}

gptps_status gptps_stats_total(gptps_stats *s, gptps_stats_counters *out)
{
    if (!s || !out) return GPTPS_E_INVAL;
    apx_mutex_lock(&s->mu);
    *out = s->total;
    apx_mutex_unlock(&s->mu);
    return GPTPS_OK;
}

gptps_status gptps_stats_task(gptps_stats *s, const char *task, gptps_stats_counters *out)
{
    size_t i;
    gptps_status st = GPTPS_E_NOTFOUND;
    if (!s || !task || !out) return GPTPS_E_INVAL;
    apx_mutex_lock(&s->mu);
    for (i = 0; i < s->ntasks; ++i)
        if (strcmp(s->tasks[i].name, task) == 0) { *out = s->tasks[i].c; st = GPTPS_OK; break; }
    apx_mutex_unlock(&s->mu);
    return st;
}

size_t gptps_stats_task_count(gptps_stats *s)
{
    size_t n;
    if (!s) return 0;
    apx_mutex_lock(&s->mu);
    n = s->ntasks;
    apx_mutex_unlock(&s->mu);
    return n;
}

gptps_status gptps_stats_task_at(gptps_stats *s, size_t index,
                                 char *name_buf, size_t name_cap, gptps_stats_counters *out)
{
    gptps_status st = GPTPS_E_NOTFOUND;
    if (!s) return GPTPS_E_INVAL;
    apx_mutex_lock(&s->mu);
    if (index < s->ntasks) {
        if (name_buf && name_cap) {
            size_t n = strlen(s->tasks[index].name);
            if (n >= name_cap) n = name_cap - 1;
            memcpy(name_buf, s->tasks[index].name, n); name_buf[n] = 0;
        }
        if (out) *out = s->tasks[index].c;
        st = GPTPS_OK;
    }
    apx_mutex_unlock(&s->mu);
    return st;
}

static void reset_row(gptps_stats_counters *c)
{
    uint64_t pending = c->pending, in_flight = c->in_flight;
    memset(c, 0, sizeof *c);
    c->struct_size = sizeof *c;
    c->pending = pending; c->in_flight = in_flight;
}

void gptps_stats_reset(gptps_stats *s)
{
    size_t i;
    if (!s) return;
    apx_mutex_lock(&s->mu);
    reset_row(&s->total);
    for (i = 0; i < s->ntasks; ++i) reset_row(&s->tasks[i].c);
    apx_mutex_unlock(&s->mu);
}

void gptps_stats_merge(gptps_stats_counters *d, const gptps_stats_counters *a)
{
    if (!d || !a) return;
    d->struct_size = sizeof *d;
    d->queued += a->queued; d->started += a->started; d->finished += a->finished;
    d->failed += a->failed; d->retried += a->retried; d->dead_lettered += a->dead_lettered;
    d->dropped += a->dropped; d->cancelled += a->cancelled; d->terminal += a->terminal;
    d->pending += a->pending; d->in_flight += a->in_flight;
    d->wait_samples += a->wait_samples; d->wait_ms_sum += a->wait_ms_sum;
    if (a->wait_ms_max > d->wait_ms_max) d->wait_ms_max = a->wait_ms_max;
    d->run_samples += a->run_samples; d->run_ms_sum += a->run_ms_sum;
    if (a->run_ms_max > d->run_ms_max) d->run_ms_max = a->run_ms_max;
}
