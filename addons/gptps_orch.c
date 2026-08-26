/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_orch.c - run-after / fan-in orchestration (see gptps_orch.h).
 *
 * One observer watches terminal events and remembers completed handles. A gate
 * holds a pending submission plus a count of unsatisfied dependencies; when that
 * count reaches zero the gate is released (gptps_submit). Both the public
 * gptps_orch_after path and the observer take the same lock, in the same order
 * (orch lock -> engine lock, via gptps_submit), so there is no lock inversion;
 * the observer runs with the engine lock released, which the API guarantees.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "gptps_orch.h"
#include "addon_compat.h"   /* portable mutex */

#include <stdlib.h>
#include <string.h>

typedef struct {
    char         *task;
    void         *payload;
    size_t        len;
    gptps_handle *deps;
    size_t        ndeps;
    size_t        remaining;   /* deps not yet terminal */
    int           released;
    unsigned      attempts;    /* rejected release_gate submits so far (see the cap) */
} gate;

/* How many times a gate whose dependencies are all satisfied may be re-submitted
 * before the orchestrator gives up on it. A retry is not free: each one runs
 * gptps_submit's pre-lock work - a malloc + memcpy of the whole payload - inside an
 * observer callback on the engine's emit path, so an uncapped retry turns one stuck
 * gate into a per-terminal-event tax on the entire engine. The cap is what makes
 * gptps_orch_pending() converge to 0 and the cost bounded.
 *
 * 16 is chosen to ride out a BRIEF pause (the TUI's `a` key, a tasks.<t>.enabled
 * flip) while capping the worst case at 16 payload copies. A type that stays paused
 * longer than sixteen terminal events is a host-level condition the orchestrator
 * cannot paper over, and retrying into it indefinitely only makes it everyone's
 * problem. Note E_NOTFOUND cannot distinguish "paused" from "never registered", so a
 * typo'd task name also costs this many attempts before the gate is abandoned. */
#define GPTPS_ORCH_RELEASE_ATTEMPTS 16

struct gptps_orch {
    gptps        *e;
    apx_mutex     mu;
    gate         *gates;
    size_t        ng, gcap;
    /* Terminal handles seen so far, as an OPEN-ADDRESSED SET: dcap is a power of two
     * (or 0), an empty slot holds 0, and handle 0 is never stored because the engine
     * numbers items from 1. It was a plain array scanned linearly, which made every
     * terminal event cost O(handles completed so far) - quadratic over a run, paid on
     * the engine's emit path, and paid even by a host that never gates anything. */
    gptps_handle *done;
    size_t        ndone, dcap;
};

static char *dup_str(const char *s) { size_t n = strlen(s) + 1; char *o = (char *)malloc(n); if (o) memcpy(o, s, n); return o; }
static void *dup_mem(const void *s, size_t n) { void *o; if (!n) return NULL; o = malloc(n); if (o) memcpy(o, s, n); return o; }

/* Is this event the LAST one this handle will ever produce?
 *
 * This is the whole correctness of the add-on, and the obvious answer is wrong.
 * GPTPS_EV_FAILED is NOT terminal: execute() emits it after EVERY failed attempt,
 * and only afterwards does the dispatcher decide retry / drop / dead-letter. A gate
 * that counted FAILED released as soon as a dependency FAILED TWICE - so "run C
 * after A and B" would run C while B was still going, merely because A retried.
 * With a single dep it is just as wrong: A fails once, C runs, A then retries and
 * succeeds - C ran before its dependency finished.
 *
 * Derived from the dispatcher's own branches in src/engine.c:
 *   retry              -> EV_RETRIED, then the item runs again
 *   ON_FAILURE_DROP    -> EV_DROPPED        after the attempt's FAILED
 *   ON_FAILURE_DEAD_.. -> EV_DEAD_LETTERED  after the attempt's FAILED
 *   ON_FAILURE_REQUEUE -> re-admitted; NEVER terminal (see the header)
 *   gptps_cancel       -> FAILED/E_CANCELLED, exactly once: emitted by execute() if
 *                         the item ran, or by the dispatcher if it never started
 * A FAILED carrying E_TIMEOUT is therefore not terminal either - a timed-out item
 * still goes through retry and the on_failure policy like any other failure.
 *
 * This is the same predicate tests/test_reconcile.c uses to assert that every
 * submitted handle reaches EXACTLY ONE terminal event, and the same one
 * durable_queue.c already applies. */
static int is_terminal(const gptps_event *ev)
{
    if (ev->kind == GPTPS_EV_FINISHED ||
        ev->kind == GPTPS_EV_DROPPED  ||
        ev->kind == GPTPS_EV_DEAD_LETTERED) return 1;
    return ev->kind == GPTPS_EV_FAILED && ev->status == GPTPS_E_CANCELLED;
}

/* Scatter the handle before probing. Handles are allocated sequentially, but only a
 * SUBSET of them is ever recorded here (one task type of several, say), and a subset
 * with a stride that shares factors with the table size would pile every entry into
 * the same few slots under an identity hash. */
static uint64_t done_hash(gptps_handle h)
{
    uint64_t x = (uint64_t)h;
    x ^= x >> 33;
    x *= (uint64_t)0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return x;
}

/* Slot for h: either the one holding it, or the first empty one after it. cap must be
 * a non-zero power of two, and the table must never be full, both of which
 * done_grow() guarantees (it keeps the load factor at or below 1/2). */
static size_t done_slot(const gptps_handle *tab, size_t cap, gptps_handle h)
{
    size_t mask = cap - 1;
    size_t i = (size_t)(done_hash(h) & (uint64_t)mask);
    while (tab[i] && tab[i] != h) i = (i + 1) & mask;
    return i;
}

static int is_done(gptps_orch *o, gptps_handle h)
{
    if (!o->dcap || !h) return 0;
    return o->done[done_slot(o->done, o->dcap, h)] == h;
}

/* Double the table and rehash. Returns 0 on OOM, leaving the old table intact. */
static int done_grow(gptps_orch *o)
{
    size_t nc = o->dcap ? o->dcap * 2 : 32;
    size_t i;
    gptps_handle *nt = (gptps_handle *)calloc(nc, sizeof *nt);
    if (!nt) return 0;
    for (i = 0; i < o->dcap; ++i)
        if (o->done[i]) nt[done_slot(nt, nc, o->done[i])] = o->done[i];
    free(o->done);
    o->done = nt; o->dcap = nc;
    return 1;
}

/* Record a terminal handle. Returns 1 if this handle is NEWLY terminal, 0 if it was
 * already known. Callers advance gates only on a 1, which makes the decrement
 * idempotent: even if a future engine ever emitted two terminal events for one
 * handle, a gate could not be double-decremented into an early release. On an
 * allocation failure we cannot remember the handle, so we return 1 and let the gate
 * advance - best effort, matching the pre-existing choice to risk a lost hint rather
 * than stall a gate forever. */
static int mark_done(gptps_orch *o, gptps_handle h)
{
    if (!h) return 0;                /* 0 is the table's empty marker; the engine never
                                      * issues it, so refuse to advance a gate on one */
    if (is_done(o, h)) return 0;
    /* keep the load factor at 1/2 so probe chains stay short */
    if ((o->ndone + 1) * 2 > o->dcap) {
        if (!done_grow(o)) return 1; /* drop the hint; advance anyway (see above) */
    }
    o->done[done_slot(o->done, o->dcap, h)] = h;
    o->ndone += 1;
    return 1;
}

/* release a gate: submit its task (caller holds o->mu; gptps_submit takes the
 * engine lock, preserving the orch->engine lock order).
 *
 * A rejected submit used to mark the gate released anyway, dropping the submission
 * on the floor: the task never ran, no event was emitted, and gptps_orch_pending()
 * fell to 0, so the host could not even observe the loss. Some rejections really are
 * transient - the type is PAUSED (E_NOTFOUND, what the TUI's `a` key and
 * tasks.<t>.enabled do) or the intake is at limits.max_intake_depth (E_FULL) - and
 * those deserve a retry on the next terminal event.
 *
 * But most are NOT transient, and retrying them forever is worse than dropping them.
 * gptps_submit rejects permanently for a cost that can never fit the budget
 * (E_BUDGET, "never-fits: reject at submit"), for a status a task type's cost hook
 * returns, and for a task name that was simply never registered - which is reachable
 * here because gptps_orch_after does not validate the name on the held-gate path, so
 * a typo surfaces only at release time. Holding those forever pins
 * gptps_orch_pending() above 0 (a documented liveness predicate that hosts and
 * tests/test_orch.c drain on) and re-submits the gate on EVERY subsequent terminal
 * event, with no cap - a full payload malloc+memcpy per event, engine-wide.
 *
 * So: retry only the statuses that can plausibly clear, and only a bounded number of
 * times. Anything else ends the gate immediately. Either way the gate always reaches
 * `released`, so pending() always converges. */
static void release_gate(gptps_orch *o, gate *g)
{
    gptps_handle h = 0;
    gptps_status st = gptps_submit(o->e, g->task, g->payload, g->len, &h);

    if (st == GPTPS_OK) { g->released = 1; return; }

    /* Transient: a paused type can be resumed, a full intake can drain, and memory
     * can come back. Everything else - E_BUDGET, E_SHUTDOWN, E_INVAL, a cost hook's
     * own status - will say the same thing next time, so stop now. */
    if (st != GPTPS_E_NOTFOUND && st != GPTPS_E_FULL && st != GPTPS_E_NOMEM) {
        g->released = 1;
        return;
    }
    if (++g->attempts >= GPTPS_ORCH_RELEASE_ATTEMPTS) g->released = 1;  /* gave up */
    /* the released task is now a normal engine item; we don't track its handle
     * further (chaining onto a deferred gate is out of scope - see the header). */
}

static void orch_obs(const gptps_event *ev, void *ud)
{
    gptps_orch *o = (gptps_orch *)ud;
    size_t i, j;
    /* only terminal events advance dependencies; ignore QUEUED/STARTED/RETRIED and
     * every non-final FAILED (see is_terminal). Returning before taking the lock also
     * means a re-entrant QUEUED - emitted by the gptps_submit in release_gate, while
     * we still hold o->mu - can't deadlock. */
    if (!is_terminal(ev)) return;

    apx_mutex_lock(&o->mu);
    if (!mark_done(o, ev->handle)) { apx_mutex_unlock(&o->mu); return; }  /* already counted */
    for (i = 0; i < o->ng; ++i) {
        gate *g = &o->gates[i];
        if (g->released) continue;
        for (j = 0; j < g->ndeps; ++j) {
            if (g->deps[j] == ev->handle && g->remaining > 0) { g->remaining -= 1; break; }
        }
        if (g->remaining == 0) release_gate(o, g);
    }
    apx_mutex_unlock(&o->mu);
}

gptps_orch *gptps_orch_install(gptps *e)
{
    gptps_orch *o;
    if (!e) return NULL;
    o = (gptps_orch *)calloc(1, sizeof *o);
    if (!o) return NULL;
    apx_mutex_init(&o->mu);
    o->e = e;
    if (gptps_register_observer(e, orch_obs, o) != GPTPS_OK) {
        apx_mutex_destroy(&o->mu); free(o); return NULL;
    }
    return o;
}

gptps_status gptps_orch_after(gptps_orch *o, const char *task,
                              const void *payload, size_t len,
                              const gptps_handle *deps, size_t ndeps,
                              gptps_handle *out)
{
    gate *g;
    size_t i, remaining;
    if (out) *out = 0;
    if (!o || !task || (ndeps && !deps)) return GPTPS_E_INVAL;

    apx_mutex_lock(&o->mu);

    /* fast path: no deps, or all deps already terminal => submit immediately */
    remaining = 0;
    for (i = 0; i < ndeps; ++i) if (!is_done(o, deps[i])) ++remaining;
    if (remaining == 0) {
        gptps_handle h = 0;
        gptps_status st = gptps_submit(o->e, task, payload, len, &h);
        apx_mutex_unlock(&o->mu);
        if (st == GPTPS_OK && out) *out = h;
        return st;
    }

    /* hold a gate until the remaining deps finish */
    if (o->ng == o->gcap) {
        size_t nc = o->gcap ? o->gcap * 2 : 8;
        gate *ng = (gate *)realloc(o->gates, nc * sizeof *ng);
        if (!ng) { apx_mutex_unlock(&o->mu); return GPTPS_E_NOMEM; }
        o->gates = ng; o->gcap = nc;
    }
    g = &o->gates[o->ng];
    memset(g, 0, sizeof *g);
    g->task = dup_str(task);
    g->payload = dup_mem(payload, len);
    g->len = len;
    g->ndeps = ndeps;
    g->remaining = remaining;
    g->deps = (gptps_handle *)malloc(ndeps * sizeof *g->deps);
    if (!g->task || (len && !g->payload) || !g->deps) {
        free(g->task); free(g->payload); free(g->deps);
        apx_mutex_unlock(&o->mu);
        return GPTPS_E_NOMEM;
    }
    memcpy(g->deps, deps, ndeps * sizeof *g->deps);
    o->ng += 1;
    apx_mutex_unlock(&o->mu);
    return GPTPS_OK;
}

size_t gptps_orch_pending(gptps_orch *o)
{
    size_t i, n = 0;
    if (!o) return 0;
    apx_mutex_lock(&o->mu);
    for (i = 0; i < o->ng; ++i) if (!o->gates[i].released) ++n;
    apx_mutex_unlock(&o->mu);
    return n;
}

void gptps_orch_close(gptps_orch *o)
{
    size_t i;
    if (!o) return;
    /* Caller contract: engine already shut down, so no observer fires here. */
    for (i = 0; i < o->ng; ++i) { free(o->gates[i].task); free(o->gates[i].payload); free(o->gates[i].deps); }
    free(o->gates); free(o->done);
    apx_mutex_destroy(&o->mu);
    free(o);
}
