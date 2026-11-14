/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_sched.c - ready-made admission disciplines (see gptps_sched.h).
 *
 * The scheduler hook runs on the dispatcher with the engine lock held, once per
 * pending item per pass, so everything here is O(1) per call and allocation is
 * confined to the fair-share item table's occasional growth. The fair-share observer
 * runs on whichever thread emits, with the engine lock released.
 *
 * Lock order: s->mu is a LEAF. The hook is entered with the engine's lock held and
 * takes s->mu; the observer takes s->mu with no engine lock; neither calls the engine
 * while holding it. So there is no order to invert.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "gptps_sched.h"
#include "addon_compat.h"   /* portable mutex */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum { K_EDF = 1, K_AGING, K_FAIR, K_SHED };

/* The fair-share virtual clock moves by FAIR_UNIT / weight per admitted item. 2^20 per
 * item at weight 1 leaves 2^43 admissions before a 64-bit tag could wrap. */
#define FAIR_UNIT        (1u << 20)
#define FAIR_MAX_WEIGHT  (1u << 20)
#define FAIR_DEF_TENANTS 64u

typedef struct {
    uint64_t id;
    uint64_t finish;      /* virtual finish of the tenant's last tagged item */
    uint32_t weight;
    int      used;
} tenant;

typedef struct {
    gptps_handle h;       /* 0 = empty: the core never hands out handle 0 */
    uint64_t     start;   /* this item's virtual start tag */
} tagged;

struct gptps_sched {
    int        kind;
    apx_mutex  mu;
    /* aging */
    uint32_t   ms_per_level;
    /* shed */
    uint32_t   grace_ms;
    uint64_t   shed;
    /* fair */
    gptps_sched_tenant_fn tenant_fn;
    void      *tenant_ud;
    tenant    *tenants;   /* open addressing on id; slot 0 doubles as the overflow */
    size_t     ntenants;
    tagged    *items;     /* open addressing on handle, linear probing, no tombstones */
    size_t     icap, ilen;
    uint64_t   vtime;     /* virtual time: the start tag of the latest item to start */
};

static gptps_sched *new_sched(int kind)
{
    gptps_sched *s = (gptps_sched *)calloc(1, sizeof *s);
    if (s) { s->kind = kind; apx_mutex_init(&s->mu); }
    return s;
}

/* ---- EDF -------------------------------------------------------------------
 * Two bands in one int64. Due items: INT64_MAX - due, so earlier is higher, and every
 * one of them is above 2^62. Items without a due time: their priority, an int32, far
 * below. A due time is clamped to 2^62 (it is monotonic milliseconds: 146 million
 * years), which keeps the bands apart. */
static int64_t edf_score(const gptps_sched_input *in, void *ud)
{
    uint64_t d = 0;
    (void)ud;
    if (in->struct_size >= offsetof(gptps_sched_input, deadline_ms) + sizeof in->deadline_ms)
        d = in->deadline_ms;
    if (d) {
        if (d > ((uint64_t)1 << 62)) d = (uint64_t)1 << 62;
        return INT64_MAX - (int64_t)d;
    }
    return (int64_t)in->priority;
}

/* ---- aging -----------------------------------------------------------------
 * priority * ms_per_level - enqueue_ms. The product is clamped to +-2^61 so that the
 * subtraction of a monotonic time (well under 2^61) cannot overflow. */
static int64_t aging_score(const gptps_sched_input *in, void *ud)
{
    const gptps_sched *s = (const gptps_sched *)ud;
    const int64_t lim = (int64_t)1 << 61;
    int64_t p = (int64_t)in->priority, m = (int64_t)s->ms_per_level, w;
    if (p != 0 && m != 0 && (p > lim / m || p < -(lim / m))) w = p > 0 ? lim : -lim;
    else                                                    w = p * m;
    return w - (int64_t)(in->enqueue_ms & (((uint64_t)1 << 61) - 1));
}

/* ---- fair share ------------------------------------------------------------ */

static uint64_t name_tenant(const gptps_sched_input *in, void *ud)
{
    const unsigned char *p = (const unsigned char *)(in->task_name ? in->task_name : "");
    uint64_t h = 1469598103934665603ull;      /* FNV-1a */
    (void)ud;
    while (*p) { h ^= *p++; h *= 1099511628211ull; }
    return h;
}

static size_t mix(uint64_t x, size_t cap)
{
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33;
    return (size_t)(x & (uint64_t)(cap - 1));     /* cap is a power of two */
}

/* The tenant's slot. A full table sends a new tenant to slot 0, which it then
 * shares - documented as what max_tenants means. Caller holds mu. */
static tenant *tenant_of(gptps_sched *s, uint64_t id)
{
    size_t i = mix(id, s->ntenants), n;
    for (n = 0; n < s->ntenants; ++n, i = (i + 1) & (s->ntenants - 1)) {
        tenant *t = &s->tenants[i];
        if (!t->used) { t->used = 1; t->id = id; t->weight = 1; t->finish = 0; return t; }
        if (t->id == id) return t;
    }
    return &s->tenants[0];
}

static tagged *item_find(gptps_sched *s, gptps_handle h)
{
    size_t i;
    if (!s->icap) return NULL;
    for (i = mix(h, s->icap); s->items[i].h; i = (i + 1) & (s->icap - 1))
        if (s->items[i].h == h) return &s->items[i];
    return NULL;
}

/* Linear-probing delete with backward shift: no tombstones, so a long run of
 * insert/delete cannot fill the table with dead slots. */
static void item_del(gptps_sched *s, tagged *slot)
{
    size_t i = (size_t)(slot - s->items), j = i;
    for (;;) {
        size_t k;
        j = (j + 1) & (s->icap - 1);
        if (!s->items[j].h) break;
        k = mix(s->items[j].h, s->icap);
        /* move j back to i unless its home k lies cyclically in (i, j] */
        if ((j > i && (k <= i || k > j)) || (j < i && (k <= i && k > j))) {
            s->items[i] = s->items[j];
            i = j;
        }
    }
    s->items[i].h = 0;
    s->ilen -= 1;
}

static int item_grow(gptps_sched *s)
{
    size_t ncap = s->icap ? s->icap * 2 : 64, i;
    tagged *old = s->items, *nw = (tagged *)calloc(ncap, sizeof *nw);
    size_t ocap = s->icap;
    if (!nw) return -1;
    s->items = nw; s->icap = ncap;
    for (i = 0; i < ocap; ++i) if (old[i].h) {
        size_t j = mix(old[i].h, ncap);
        while (nw[j].h) j = (j + 1) & (ncap - 1);
        nw[j] = old[i];
    }
    free(old);
    return 0;
}

static int64_t fair_score(const gptps_sched_input *in, void *ud)
{
    gptps_sched *s = (gptps_sched *)ud;
    uint64_t start;
    tagged *it;
    apx_mutex_lock(&s->mu);
    it = item_find(s, in->handle);
    if (it) {
        start = it->start;                         /* tagged once: the key does not move */
    } else {
        tenant *t = tenant_of(s, s->tenant_fn(in, s->tenant_ud));
        start = t->finish > s->vtime ? t->finish : s->vtime;
        /* Keep the table at most half full, so probes stay short. If it cannot grow
         * the item is scored without being remembered - and without charging its
         * tenant, so a later pass tags it properly: degraded order for a moment, never
         * a wrong share. */
        if ((s->ilen + 1) * 2 <= s->icap || item_grow(s) == 0) {
            size_t j = mix(in->handle, s->icap);
            while (s->items[j].h) j = (j + 1) & (s->icap - 1);
            s->items[j].h = in->handle; s->items[j].start = start;
            s->ilen += 1;
            t->finish = start + FAIR_UNIT / t->weight;
        }
    }
    apx_mutex_unlock(&s->mu);
    /* earliest start first; tags stay far below 2^63 (see FAIR_UNIT) */
    return -(int64_t)(start & (((uint64_t)1 << 62) - 1));
}

/* Virtual time follows what starts; anything that ends the item's wait forgets it. A
 * retry is a new wait, tagged and charged afresh when it is next scored. */
static void fair_obs(const gptps_event *ev, void *ud)
{
    gptps_sched *s = (gptps_sched *)ud;
    tagged *it;
    switch (ev->kind) {
    case GPTPS_EV_STARTED: case GPTPS_EV_FINISHED: case GPTPS_EV_FAILED:
    case GPTPS_EV_DROPPED: case GPTPS_EV_DEAD_LETTERED:
        break;
    default:
        return;                                    /* QUEUED, RETRIED, SAMPLE, and kinds to come */
    }
    apx_mutex_lock(&s->mu);
    it = item_find(s, ev->handle);
    if (it) {
        if (ev->kind == GPTPS_EV_STARTED && it->start > s->vtime) s->vtime = it->start;
        item_del(s, it);
    }
    apx_mutex_unlock(&s->mu);
}

/* ---- shed late ------------------------------------------------------------- */
static gptps_admit_decision shed_fn(const gptps_constraint_input *in, uint32_t *retry_after, void *ud)
{
    gptps_sched *s = (gptps_sched *)ud;
    (void)retry_after;
    if (in->struct_size < offsetof(gptps_constraint_input, now_ms) + sizeof in->now_ms) return GPTPS_ADMIT;
    if (in->deadline_ms && in->now_ms > in->deadline_ms &&
        in->now_ms - in->deadline_ms > (uint64_t)s->grace_ms) {
        apx_mutex_lock(&s->mu);
        s->shed += 1;
        apx_mutex_unlock(&s->mu);
        return GPTPS_DENY;
    }
    return GPTPS_ADMIT;
}

/* ---- install ---------------------------------------------------------------- */

static gptps_status install_order(gptps *e, int kind, gptps_sched_fn fn, const char *owner, gptps_sched **out)
{
    gptps_sched *s;
    gptps_status st;
    if (!e || !out) return GPTPS_E_INVAL;
    *out = NULL;
    s = new_sched(kind);
    if (!s) return GPTPS_E_NOMEM;
    st = gptps_set_scheduler_ex(e, fn, s, owner, GPTPS_SCHED_STABLE);   /* never take a seam someone holds; scores never move */
    if (st != GPTPS_OK) { gptps_sched_close(s); return st; }
    *out = s;
    return GPTPS_OK;
}

gptps_status gptps_sched_edf(gptps *e, gptps_sched **out)
{
    return install_order(e, K_EDF, edf_score, "sched.edf", out);
}

gptps_status gptps_sched_aging(gptps *e, uint32_t ms_per_level, gptps_sched **out)
{
    gptps_status st;
    gptps_sched *s;
    if (!e || !out) return GPTPS_E_INVAL;
    *out = NULL;
    s = new_sched(K_AGING);
    if (!s) return GPTPS_E_NOMEM;
    s->ms_per_level = ms_per_level;                     /* set before the engine can call */
    st = gptps_set_scheduler_ex(e, aging_score, s, "sched.aging", GPTPS_SCHED_STABLE);
    if (st != GPTPS_OK) { gptps_sched_close(s); return st; }
    *out = s;
    return GPTPS_OK;
}

gptps_status gptps_sched_fair(gptps *e, gptps_sched_tenant_fn tenant_fn, void *user_data,
                              size_t max_tenants, gptps_sched **out)
{
    gptps_sched *s;
    gptps_status st;
    size_t n = 1;
    if (!e || !out) return GPTPS_E_INVAL;
    *out = NULL;
    if (max_tenants == 0) max_tenants = FAIR_DEF_TENANTS;
    if (max_tenants > ((size_t)1 << 20)) return GPTPS_E_INVAL;
    while (n < max_tenants) n <<= 1;                    /* a power of two, for the probe mask */
    s = new_sched(K_FAIR);
    if (!s) return GPTPS_E_NOMEM;
    s->tenant_fn = tenant_fn ? tenant_fn : name_tenant;
    s->tenant_ud = tenant_fn ? user_data : NULL;
    s->ntenants = n;
    s->tenants = (tenant *)calloc(n, sizeof *s->tenants);
    if (!s->tenants || item_grow(s) != 0) { gptps_sched_close(s); return GPTPS_E_NOMEM; }
    /* The observer first: once the hook is in, items get tagged, and only the observer
     * ever releases a tag. Installed the other way round, a failure between the two
     * would leave an ordering in place that could not advance. */
    st = gptps_register_observer(e, fair_obs, s);
    if (st != GPTPS_OK) { gptps_sched_close(s); return st; }
    st = gptps_set_scheduler_ex(e, fair_score, s, "sched.fair", GPTPS_SCHED_STABLE);
    if (st != GPTPS_OK) {
        gptps_unregister_observer(e, fair_obs, s);
        gptps_sched_close(s);
        return st;
    }
    *out = s;
    return GPTPS_OK;
}

gptps_status gptps_sched_fair_weight(gptps_sched *s, uint64_t id, uint32_t weight)
{
    if (!s || s->kind != K_FAIR || weight < 1 || weight > FAIR_MAX_WEIGHT) return GPTPS_E_INVAL;
    apx_mutex_lock(&s->mu);
    tenant_of(s, id)->weight = weight;
    apx_mutex_unlock(&s->mu);
    return GPTPS_OK;
}

gptps_status gptps_sched_shed_late(gptps *e, uint32_t grace_ms, gptps_sched **out)
{
    gptps_sched *s;
    gptps_status st;
    if (!e || !out) return GPTPS_E_INVAL;
    *out = NULL;
    s = new_sched(K_SHED);
    if (!s) return GPTPS_E_NOMEM;
    s->grace_ms = grace_ms;
    st = gptps_register_constraint(e, shed_fn, s);
    if (st != GPTPS_OK) { gptps_sched_close(s); return st; }
    *out = s;
    return GPTPS_OK;
}

uint64_t gptps_sched_shed_count(const gptps_sched *s)
{
    uint64_t n;
    if (!s || s->kind != K_SHED) return 0;
    apx_mutex_lock((apx_mutex *)&s->mu);
    n = s->shed;
    apx_mutex_unlock((apx_mutex *)&s->mu);
    return n;
}

void gptps_sched_close(gptps_sched *s)
{
    if (!s) return;
    free(s->tenants);
    free(s->items);
    apx_mutex_destroy(&s->mu);
    free(s);
}
