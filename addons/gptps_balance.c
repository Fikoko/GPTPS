/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_balance.c - late-binding router above gptps_pool.
 *
 * Structure: one lock, one priority heap (the router queue), one hash table from
 * balance handle to item, and per shard an observer plus a hash table from that
 * shard's engine handle to item. Dispatch runs under the lock and hands the head of
 * the heap to the least-loaded shard with room, repeating until no shard has room or
 * the heap is empty. It is called from submit (new work) and from the observers
 * (room freed by a terminal event).
 *
 * The one subtle rule: gptps_submit emits the engine's QUEUED event synchronously on
 * the CALLING thread, and dispatch calls gptps_submit while holding this module's
 * lock. So the observer must ignore QUEUED before it takes the lock - which is fine,
 * because this module emits its own QUEUED at balance_submit time, when the item was
 * actually queued (here). Every other engine event comes from a dispatcher thread and
 * simply waits for the lock. Host callbacks always run with the lock released.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "gptps_balance.h"
#include "addon_compat.h"   /* portable mutex */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum { IT_QUEUED = 1, IT_DISPATCHED = 2, IT_CANCELLED = 3 };

typedef struct bitem {
    gptps_balance_handle bh;
    uint32_t     state;
    size_t       shard;
    gptps_handle eh;
    int32_t      prio;
    uint64_t     seq;
    char        *task;
    void        *payload;
    size_t       len;
    struct bitem *next;       /* for the deferred-emit lists */
    gptps_status  fail;       /* status for a deferred terminal event */
} bitem;

/* ---- u64 -> item hash (open addressing, backward-shift delete) ---- */
typedef struct { uint64_t key; bitem *val; } hslot;
typedef struct { hslot *tab; size_t cap, n; } hmap;

static size_t hh(uint64_t x, size_t cap)
{ x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; return (size_t)x & (cap - 1); }
static bitem *hm_get(hmap *m, uint64_t k)
{
    size_t i;
    if (!m->cap) return NULL;
    for (i = hh(k, m->cap); ; i = (i + 1) & (m->cap - 1)) {
        if (m->tab[i].key == k) return m->tab[i].val;
        if (m->tab[i].key == 0) return NULL;
    }
}
static int hm_grow(hmap *m)
{
    size_t nc = m->cap ? m->cap * 2 : 64, i;
    hslot *nt = (hslot *)calloc(nc, sizeof *nt);
    if (!nt) return -1;
    for (i = 0; i < m->cap; ++i) if (m->tab[i].key) {
        size_t j; for (j = hh(m->tab[i].key, nc); nt[j].key; j = (j + 1) & (nc - 1)) { }
        nt[j] = m->tab[i];
    }
    free(m->tab); m->tab = nt; m->cap = nc; return 0;
}
static int hm_put(hmap *m, uint64_t k, bitem *v)
{
    size_t i;
    if ((m->n + 1) * 4 > m->cap * 3 && hm_grow(m) != 0) return -1;
    for (i = hh(k, m->cap); m->tab[i].key; i = (i + 1) & (m->cap - 1))
        if (m->tab[i].key == k) { m->tab[i].val = v; return 0; }
    m->tab[i].key = k; m->tab[i].val = v; m->n += 1; return 0;
}
static void hm_del(hmap *m, uint64_t k)
{
    size_t i, j, cap = m->cap;
    if (!cap) return;
    for (i = hh(k, cap); m->tab[i].key != k; i = (i + 1) & (cap - 1)) if (!m->tab[i].key) return;
    j = i;
    for (;;) {
        size_t h;
        j = (j + 1) & (cap - 1);
        if (!m->tab[j].key) break;
        h = hh(m->tab[j].key, cap);
        if ((i <= j) ? (h <= i || h > j) : (h <= i && h > j)) { m->tab[i] = m->tab[j]; i = j; }
    }
    m->tab[i].key = 0; m->tab[i].val = NULL; m->n -= 1;
}

/* ---- the router queue: binary heap by (prio desc, seq asc) ---- */
typedef struct { bitem **a; size_t n, cap; } heap;
static int before(const bitem *x, const bitem *y)
{ return x->prio != y->prio ? x->prio > y->prio : x->seq < y->seq; }
static int heap_push(heap *h, bitem *it)
{
    size_t i;
    if (h->n == h->cap) {
        size_t nc = h->cap ? h->cap * 2 : 64;
        bitem **na = (bitem **)realloc(h->a, nc * sizeof *na);
        if (!na) return -1;
        h->a = na; h->cap = nc;
    }
    i = h->n++;
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (!before(it, h->a[p])) break;
        h->a[i] = h->a[p]; i = p;
    }
    h->a[i] = it;
    return 0;
}
static bitem *heap_pop(heap *h)
{
    bitem *top, *last; size_t i = 0;
    if (!h->n) return NULL;
    top = h->a[0]; last = h->a[--h->n];
    while (h->n) {
        size_t l = 2 * i + 1, r = l + 1, c;
        if (l >= h->n) break;
        c = (r < h->n && before(h->a[r], h->a[l])) ? r : l;
        if (!before(h->a[c], last)) break;
        h->a[i] = h->a[c]; i = c;
    }
    if (h->n) h->a[i] = last;
    return top;
}

/* ---- shards ---- */
typedef struct {
    struct gptps_balance *b;
    gptps    *e;
    uint32_t  cap;        /* shard_depth: running + waiting in the shard */
    uint32_t  load;       /* handed over, not yet terminal */
    hmap      by_eh;
} bshard;

struct gptps_balance {
    apx_mutex      mu;
    gptps_pool    *pool;
    bshard        *sh;
    size_t         nsh;
    heap           q;
    size_t         queued;      /* live items in q (cancelled ones excluded) */
    hmap           by_bh;
    uint64_t       next_bh, next_seq;
    gptps_event_cb cb; void *cb_ud;
};

static void item_free(bitem *it)
{ if (it) { free(it->task); free(it->payload); free(it); } }

static void emit_own(gptps_balance *b, const bitem *it, gptps_event_kind kind, gptps_status st)
{
    gptps_event ev;
    if (!b->cb) return;
    memset(&ev, 0, sizeof ev);
    ev.struct_size = sizeof ev; ev.kind = kind; ev.handle = it->bh;
    ev.task_name = it->task; ev.ts_ms = gptps_now_ms(NULL); ev.status = st;
    b->cb(&ev, b->cb_ud);
}

/* Hand queued work to shards with room. b->mu HELD. Items that could not be
 * dispatched (the shard refused the submit) are returned on a list for the caller to
 * emit + free with the lock released. */
static bitem *dispatch_locked(gptps_balance *b)
{
    bitem *fails = NULL;
    for (;;) {
        size_t i, best = (size_t)-1; uint32_t bestload = 0;
        bitem *it;
        gptps_submit_options o;
        gptps_status st;

        if (!b->queued) break;
        for (i = 0; i < b->nsh; ++i)
            if (b->sh[i].load < b->sh[i].cap && (best == (size_t)-1 || b->sh[i].load < bestload))
            { best = i; bestload = b->sh[i].load; }
        if (best == (size_t)-1) break;                  /* every shard is full to its depth */

        it = heap_pop(&b->q);
        if (!it) break;
        if (it->state == IT_CANCELLED) { item_free(it); continue; }   /* lazily deleted */
        b->queued -= 1;

        it->state = IT_DISPATCHED; it->shard = best;
        b->sh[best].load += 1;
        memset(&o, 0, sizeof o); o.struct_size = sizeof o;
        o.flags = GPTPS_SUBMIT_PRIORITY; o.priority = it->prio;
        /* Emits the engine's QUEUED on THIS thread; the observer ignores QUEUED. */
        st = gptps_submit_ex(b->sh[best].e, it->task, it->payload, it->len, &o, &it->eh);
        if (st == GPTPS_OK && hm_put(&b->sh[best].by_eh, it->eh, it) == 0) {
            free(it->payload); it->payload = NULL; it->len = 0;   /* the engine copied it */
            continue;
        }
        /* Refused (E_SHUTDOWN, E_BUDGET, E_FULL...) or no memory to track it: this
         * item is terminal here. (If the engine did accept it and we failed to track
         * it, cancel it so its own terminal event cannot leak past us.) */
        if (st == GPTPS_OK) gptps_cancel(b->sh[best].e, it->eh);
        b->sh[best].load -= 1;
        hm_del(&b->by_bh, it->bh);
        it->fail = (st == GPTPS_OK) ? GPTPS_E_NOMEM : st;
        it->next = fails; fails = it;
    }
    return fails;
}
static void emit_fails(gptps_balance *b, bitem *fails)
{
    while (fails) {
        bitem *n = fails->next;
        emit_own(b, fails, GPTPS_EV_DEAD_LETTERED, fails->fail);
        item_free(fails);
        fails = n;
    }
}

static int is_terminal(const gptps_event *ev)
{
    if (ev->kind == GPTPS_EV_FINISHED || ev->kind == GPTPS_EV_DROPPED ||
        ev->kind == GPTPS_EV_DEAD_LETTERED) return 1;
    return ev->kind == GPTPS_EV_FAILED && ev->status == GPTPS_E_CANCELLED;
}

/* Observer on one shard (its dispatcher thread; QUEUED also on submitters). */
static void observe(const gptps_event *ev, void *ud)
{
    bshard *s = (bshard *)ud;
    gptps_balance *b = s->b;
    bitem *it, *fails = NULL;
    gptps_event fwd;
    int term;

    if (ev->kind == GPTPS_EV_QUEUED) return;         /* see the header comment */

    apx_mutex_lock(&b->mu);
    it = hm_get(&s->by_eh, ev->handle);
    if (!it) { apx_mutex_unlock(&b->mu); return; }   /* not balanced work */
    fwd = *ev; fwd.handle = it->bh;
    term = is_terminal(ev);
    if (term) {
        hm_del(&s->by_eh, ev->handle);
        hm_del(&b->by_bh, it->bh);
        s->load -= 1;
        fails = dispatch_locked(b);                   /* room freed: hand over the next */
    }
    apx_mutex_unlock(&b->mu);

    if (b->cb) b->cb(&fwd, b->cb_ud);
    if (term) item_free(it);
    emit_fails(b, fails);
}

/* ---- public ---- */
gptps_balance *gptps_balance_open(gptps_pool *p, const gptps_balance_config *cfg)
{
    gptps_balance *b;
    size_t i, n;
    uint32_t depth = 0;

    if (!p) return NULL;
    if (cfg) {
        if (cfg->struct_size < sizeof(gptps_balance_config)) return NULL;
        depth = cfg->shard_depth;
    }
    n = gptps_pool_count(p);
    if (!n) return NULL;
    b = (gptps_balance *)calloc(1, sizeof *b);
    if (!b) return NULL;
    b->sh = (bshard *)calloc(n, sizeof *b->sh);
    if (!b->sh) { free(b); return NULL; }
    b->pool = p; b->nsh = n;
    apx_mutex_init(&b->mu);

    for (i = 0; i < n; ++i) {
        char v[64]; unsigned long conc = 1;
        b->sh[i].b = b;
        b->sh[i].e = gptps_pool_shard(p, i);
        if (gptps_settings_get(b->sh[i].e, "limits.max_concurrent_tasks", v, sizeof v) == GPTPS_OK)
            conc = strtoul(v, NULL, 10);
        if (conc < 1) conc = 1;
        b->sh[i].cap = depth ? depth : 2u * (uint32_t)conc;
        if (gptps_register_observer(b->sh[i].e, observe, &b->sh[i]) != GPTPS_OK) {
            size_t j;
            for (j = 0; j < i; ++j) gptps_unregister_observer(b->sh[j].e, observe, &b->sh[j]);
            apx_mutex_destroy(&b->mu); free(b->sh); free(b);
            return NULL;
        }
    }
    return b;
}

gptps_status gptps_balance_set_event_cb(gptps_balance *b, gptps_event_cb cb, void *ud)
{
    if (!b) return GPTPS_E_INVAL;
    apx_mutex_lock(&b->mu);
    b->cb = cb; b->cb_ud = ud;
    apx_mutex_unlock(&b->mu);
    return GPTPS_OK;
}

gptps_status gptps_balance_submit_ex(gptps_balance *b, const char *task,
                                     const void *payload, size_t len, int32_t priority,
                                     gptps_balance_handle *out)
{
    bitem *it, *fails;
    if (out) *out = 0;
    if (!b || !task || !*task) return GPTPS_E_INVAL;
    if (len && !payload) return GPTPS_E_INVAL;
    if (!gptps_task_exists(b->sh[0].e, task)) return GPTPS_E_NOTFOUND;

    it = (bitem *)calloc(1, sizeof *it);
    if (!it) return GPTPS_E_NOMEM;
    it->task = (char *)malloc(strlen(task) + 1);
    if (!it->task) { free(it); return GPTPS_E_NOMEM; }
    strcpy(it->task, task);
    if (len) {
        it->payload = malloc(len);
        if (!it->payload) { item_free(it); return GPTPS_E_NOMEM; }
        memcpy(it->payload, payload, len);
    }
    it->len = len; it->prio = priority; it->state = IT_QUEUED;

    apx_mutex_lock(&b->mu);
    it->bh = ++b->next_bh; it->seq = ++b->next_seq;
    if (hm_put(&b->by_bh, it->bh, it) != 0 || heap_push(&b->q, it) != 0) {
        hm_del(&b->by_bh, it->bh);
        apx_mutex_unlock(&b->mu); item_free(it); return GPTPS_E_NOMEM;
    }
    b->queued += 1;
    apx_mutex_unlock(&b->mu);

    if (out) *out = it->bh;
    emit_own(b, it, GPTPS_EV_QUEUED, GPTPS_OK);      /* queued HERE; lock released */

    apx_mutex_lock(&b->mu);
    fails = dispatch_locked(b);
    apx_mutex_unlock(&b->mu);
    emit_fails(b, fails);
    return GPTPS_OK;
}

gptps_status gptps_balance_submit(gptps_balance *b, const char *task,
                                  const void *payload, size_t len, gptps_balance_handle *out)
{ return gptps_balance_submit_ex(b, task, payload, len, 0, out); }

gptps_status gptps_balance_cancel(gptps_balance *b, gptps_balance_handle h)
{
    bitem *it; gptps *e; gptps_handle eh;
    if (!b) return GPTPS_E_INVAL;
    apx_mutex_lock(&b->mu);
    it = hm_get(&b->by_bh, h);
    if (!it) { apx_mutex_unlock(&b->mu); return GPTPS_E_NOTFOUND; }
    if (it->state == IT_QUEUED) {
        /* Stays in the heap (lazily skipped and freed on pop); leaves the handle
         * table now so a second cancel is NOTFOUND, and gets its one terminal event. */
        it->state = IT_CANCELLED;
        b->queued -= 1;
        hm_del(&b->by_bh, it->bh);
        apx_mutex_unlock(&b->mu);
        emit_own(b, it, GPTPS_EV_FAILED, GPTPS_E_CANCELLED);
        return GPTPS_OK;
    }
    e = b->sh[it->shard].e; eh = it->eh;
    apx_mutex_unlock(&b->mu);
    return gptps_cancel(e, eh);                       /* its terminal event flows back through observe() */
}

size_t gptps_balance_queued(gptps_balance *b)
{
    size_t n;
    if (!b) return 0;
    apx_mutex_lock(&b->mu); n = b->queued; apx_mutex_unlock(&b->mu);
    return n;
}

size_t gptps_balance_shard_load(gptps_balance *b, size_t i)
{
    size_t n;
    if (!b || i >= b->nsh) return 0;
    apx_mutex_lock(&b->mu); n = b->sh[i].load; apx_mutex_unlock(&b->mu);
    return n;
}

void gptps_balance_close(gptps_balance *b)
{
    bitem *it; size_t i;
    if (!b) return;
    /* No unregister: this runs AFTER gptps_pool_close freed the shards. Whatever was
     * still queued here never reached a shard and owes its terminal event. */
    apx_mutex_lock(&b->mu);
    while ((it = heap_pop(&b->q)) != NULL) {
        if (it->state == IT_CANCELLED) { item_free(it); continue; }
        it->next = NULL; it->fail = GPTPS_E_SHUTDOWN;
        apx_mutex_unlock(&b->mu);
        emit_own(b, it, GPTPS_EV_DROPPED, GPTPS_E_SHUTDOWN);
        item_free(it);
        apx_mutex_lock(&b->mu);
    }
    b->queued = 0;
    /* Dispatched items all reached a terminal event during the pool's shutdown and
     * were freed by observe(); anything left is defensive cleanup. */
    for (i = 0; i < b->nsh; ++i) {
        size_t k;
        for (k = 0; k < b->sh[i].by_eh.cap; ++k)
            if (b->sh[i].by_eh.tab[k].key) item_free(b->sh[i].by_eh.tab[k].val);
        free(b->sh[i].by_eh.tab);
    }
    apx_mutex_unlock(&b->mu);
    apx_mutex_destroy(&b->mu);
    free(b->by_bh.tab);
    free(b->q.a);
    free(b->sh);
    free(b);
}
