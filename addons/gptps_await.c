/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_await.c - blocking wait on a handle (see gptps_await.h).
 *
 * Structure: one observer, one lock, one condvar broadcast on every terminal event.
 *
 * Two collections, and the split is the whole design:
 *   waiters - threads currently blocked in gptps_await_wait, keyed by handle. A
 *             terminal event for a waited-on handle is delivered STRAIGHT into the
 *             waiter's slot and never touches the ring.
 *   ring    - a bounded FIFO of completions nobody was waiting for yet. This is what
 *             closes the submit/finish race: in THREADED mode an item can finish
 *             before gptps_submit hands its handle back, so the completion has to
 *             land somewhere claimable. Oldest is evicted when full.
 *
 * Lock order: aw->mu is a LEAF. Nothing here calls back into the engine while
 * holding it - the observer fires with the engine lock released, and we never
 * submit, cancel or read settings from inside. So there is no order to invert.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "gptps_await.h"
#include "addon_compat.h"   /* portable mutex + condvar */

#include <stdlib.h>
#include <string.h>

#define AWAIT_DEFAULT_CAP 256u

typedef struct {
    gptps_handle h;
    gptps_status status;      /* the TASK's outcome */
    void        *result;      /* owned copy, or NULL */
    size_t       len;
} completion;

typedef struct waiter {
    gptps_handle   h;
    int            done;
    gptps_status   status;
    void          *result;
    size_t         len;
    struct waiter *next;
} waiter;

struct gptps_await {
    apx_mutex     mu;
    apx_cond      cv;
    waiter       *waiters;    /* intrusive list of blocked callers */
    completion   *ring;       /* bounded retention of unclaimed completions */
    size_t        cap, head, n;
    unsigned long seen;       /* total terminal events observed */
    unsigned      nblocked;   /* threads inside a cond wait (see await_obs) */
};

/* Is this event the LAST one this handle will ever produce?
 *
 * GPTPS_EV_FAILED is NOT terminal on its own: execute() emits it after every failed
 * ATTEMPT and the dispatcher only then decides retry / drop / dead-letter. Waking a
 * waiter on a plain FAILED would hand back a "result" for an item that is about to
 * run again. The one exception is a cancel, which is emitted exactly once - by
 * execute() if the item ran, or by the dispatcher if it never started.
 *
 * This is the same predicate tests/test_reconcile.c uses to assert that every
 * submitted handle reaches EXACTLY ONE terminal event; that guarantee is what makes
 * a single delivery per waiter correct. addons/gptps_orch.c uses it too. */
static int is_terminal(const gptps_event *ev)
{
    if (ev->kind == GPTPS_EV_FINISHED ||
        ev->kind == GPTPS_EV_DROPPED  ||
        ev->kind == GPTPS_EV_DEAD_LETTERED) return 1;
    return ev->kind == GPTPS_EV_FAILED && ev->status == GPTPS_E_CANCELLED;
}

static void *dup_mem(const void *s, size_t n)
{
    void *o;
    if (!s || !n) return NULL;
    o = malloc(n);
    if (o) memcpy(o, s, n);
    return o;
}

/* Push a completion nobody is waiting for, evicting the oldest if full. Caller
 * holds mu. A failed result copy is not fatal: the wait still completes and reports
 * the task's status, it just reports no bytes. Losing the payload beats blocking a
 * caller forever because the add-on was out of memory. */
static void ring_push(gptps_await *aw, gptps_handle h, gptps_status st,
                      const void *res, size_t len)
{
    completion *slot;
    if (aw->cap == 0) return;
    if (aw->n == aw->cap) {                       /* evict oldest */
        free(aw->ring[aw->head].result);
        aw->ring[aw->head].result = NULL;
        aw->head = (aw->head + 1) % aw->cap;
        aw->n -= 1;
    }
    slot = &aw->ring[(aw->head + aw->n) % aw->cap];
    slot->h = h; slot->status = st;
    slot->result = dup_mem(res, len);
    slot->len = slot->result ? len : 0;
    aw->n += 1;
}

/* Claim a retained completion by handle, transferring ownership of its result to
 * the caller. Returns 1 if found. Caller holds mu. */
static int ring_take(gptps_await *aw, gptps_handle h, gptps_status *st,
                     void **res, size_t *len)
{
    size_t i, idx;
    for (i = 0; i < aw->n; ++i) {
        idx = (aw->head + i) % aw->cap;
        if (aw->ring[idx].h != h) continue;
        *st = aw->ring[idx].status;
        *res = aw->ring[idx].result;
        *len = aw->ring[idx].len;
        /* compact: shift the tail down one so the FIFO order of the rest holds */
        for (; i + 1 < aw->n; ++i) {
            size_t a = (aw->head + i) % aw->cap, b = (aw->head + i + 1) % aw->cap;
            aw->ring[a] = aw->ring[b];
        }
        aw->n -= 1;
        return 1;
    }
    return 0;
}

static void await_obs(const gptps_event *ev, void *ud)
{
    gptps_await *aw = (gptps_await *)ud;
    waiter *w;
    int delivered = 0;

    if (!is_terminal(ev)) return;

    apx_mutex_lock(&aw->mu);
    aw->seen += 1;

    for (w = aw->waiters; w; w = w->next) {
        if (w->h != ev->handle || w->done) continue;
        /* Copy the result HERE. gptps_event.result is valid only for the duration
         * of this callback - handing the pointer to a waiter that wakes later would
         * be a use-after-free. */
        w->result = dup_mem(ev->result, ev->result_len);
        w->len    = w->result ? ev->result_len : 0;
        w->status = ev->status;
        w->done   = 1;
        delivered = 1;
        break;                       /* exactly one terminal event per handle */
    }
    if (!delivered)
        ring_push(aw, ev->handle, ev->status, ev->result, ev->result_len);

    /* Broadcast, not signal: waiters are keyed by handle, so the thread this event
     * concerns is not necessarily the one a signal would wake - and
     * gptps_await_quiesce waits on the same condvar for a different predicate again.
     *
     * Only when somebody is actually blocked, though. This observer is on the hot
     * path - it runs once per terminal event, i.e. once per completed task - and a
     * broadcast to an empty condvar is pure overhead paid by every item in the run.
     * nblocked is maintained under the same lock, so the test cannot race: a thread
     * that increments it does so before releasing the lock to wait. */
    if (aw->nblocked) apx_cond_broadcast(&aw->cv);
    apx_mutex_unlock(&aw->mu);
}

gptps_await *gptps_await_install_ex(gptps *e, size_t cap)
{
    gptps_await *aw;
    if (!e) return NULL;
    if (cap == 0) cap = AWAIT_DEFAULT_CAP;

    aw = (gptps_await *)calloc(1, sizeof *aw);
    if (!aw) return NULL;
    aw->ring = (completion *)calloc(cap, sizeof *aw->ring);
    if (!aw->ring) { free(aw); return NULL; }
    aw->cap = cap;
    apx_mutex_init(&aw->mu);
    apx_cond_init(&aw->cv);

    if (gptps_register_observer(e, await_obs, aw) != GPTPS_OK) {
        apx_cond_destroy(&aw->cv);
        apx_mutex_destroy(&aw->mu);
        free(aw->ring); free(aw);
        return NULL;
    }
    return aw;
}

gptps_await *gptps_await_install(gptps *e) { return gptps_await_install_ex(e, 0); }

gptps_status gptps_await_wait(gptps_await *aw, gptps_handle h, unsigned timeout_ms,
                              void **out_result, size_t *out_len,
                              gptps_status *out_status)
{
    waiter w, **pp;
    gptps_status task_st = GPTPS_OK;
    void *res = NULL;
    size_t len = 0;
    uint64_t deadline;
    int got = 0;

    if (out_result) *out_result = NULL;
    if (out_len)    *out_len    = 0;
    if (!aw || h == 0) return GPTPS_E_INVAL;

    apx_mutex_lock(&aw->mu);

    /* Already finished? Then the completion is sitting in the ring - this is the
     * submit/finish race resolving in the caller's favour. */
    if (ring_take(aw, h, &task_st, &res, &len)) {
        apx_mutex_unlock(&aw->mu);
        if (out_status) *out_status = task_st;
        if (out_result) *out_result = res; else free(res);
        if (out_len)    *out_len    = len;
        return GPTPS_OK;
    }
    if (timeout_ms == 0) { apx_mutex_unlock(&aw->mu); return GPTPS_E_TIMEOUT; }

    /* Register interest, then block. The waiter lives on THIS thread's stack: it is
     * linked while the lock is held and unlinked before we return, so the observer
     * can only ever see it inside that window. No allocation, so no failure path. */
    memset(&w, 0, sizeof w);
    w.h = h;
    w.next = aw->waiters;
    aw->waiters = &w;

    deadline = gptps_now_ms(NULL) + (uint64_t)timeout_ms;
    aw->nblocked += 1;
    while (!w.done) {
        uint64_t now = gptps_now_ms(NULL);
        if (now >= deadline) break;
        /* Re-checked in a loop because apx_cond_wait_ms may wake spuriously, and
         * because a broadcast wakes every waiter regardless of handle. */
        apx_cond_wait_ms(&aw->cv, &aw->mu, (unsigned)(deadline - now));
    }
    aw->nblocked -= 1;

    for (pp = &aw->waiters; *pp; pp = &(*pp)->next)
        if (*pp == &w) { *pp = w.next; break; }

    got = w.done;
    task_st = w.status; res = w.result; len = w.len;
    apx_mutex_unlock(&aw->mu);

    if (!got) return GPTPS_E_TIMEOUT;
    if (out_status) *out_status = task_st;
    if (out_result) *out_result = res; else free(res);
    if (out_len)    *out_len    = len;
    return GPTPS_OK;
}

unsigned long gptps_await_count(gptps_await *aw)
{
    unsigned long n;
    if (!aw) return 0;
    apx_mutex_lock(&aw->mu);
    n = aw->seen;
    apx_mutex_unlock(&aw->mu);
    return n;
}

gptps_status gptps_await_quiesce(gptps_await *aw, unsigned long n, unsigned timeout_ms)
{
    uint64_t deadline;
    int ok;
    if (!aw) return GPTPS_E_INVAL;

    apx_mutex_lock(&aw->mu);
    deadline = gptps_now_ms(NULL) + (uint64_t)timeout_ms;
    aw->nblocked += 1;
    while (aw->seen < n) {
        uint64_t now = gptps_now_ms(NULL);
        if (now >= deadline) break;
        apx_cond_wait_ms(&aw->cv, &aw->mu, (unsigned)(deadline - now));
    }
    aw->nblocked -= 1;
    ok = (aw->seen >= n);
    apx_mutex_unlock(&aw->mu);
    return ok ? GPTPS_OK : GPTPS_E_TIMEOUT;
}

void gptps_await_close(gptps_await *aw)
{
    size_t i;
    if (!aw) return;
    /* Caller contract: gptps_shutdown has ALREADY run, so no observer is firing and
     * no thread is still inside gptps_await_wait. Same contract as gptps_orch_close
     * and the other observer add-ons.
     *
     * Deliberately NO gptps_unregister_observer here. gptps_shutdown frees the engine
     * itself, so by the time close() runs there is nothing left to unregister from -
     * calling it reads a freed engine, which is a use-after-free ThreadSanitizer
     * catches immediately. The registration cannot outlive the engine that holds it,
     * so there is nothing to clean up. */
    for (i = 0; i < aw->n; ++i) free(aw->ring[(aw->head + i) % aw->cap].result);
    free(aw->ring);
    apx_cond_destroy(&aw->cv);
    apx_mutex_destroy(&aw->mu);
    free(aw);
}
