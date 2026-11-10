/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_adapt.c - memory declarations from evidence (see gptps_adapt.h).
 *
 * One observer, one lock, one record per task type. The observer folds each measured
 * attempt into its task's record under a->mu; applying a suggestion writes a setting,
 * which takes the engine's locks, so it is done with a->mu RELEASED. a->mu is a leaf:
 * nothing calls the engine while holding it.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "gptps_adapt.h"
#include "addon_compat.h"   /* portable mutex */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ADAPT_MIB (1024ull * 1024ull)

typedef struct {
    char               name[GPTPS_TASK_NAME_MAX + 1];
    gptps_adapt_report r;
    uint64_t           last_written;   /* the value auto_apply last wrote; 0 = none */
} rec;

struct gptps_adapt {
    gptps             *e;
    gptps_adapt_config cfg;
    apx_mutex          mu;
    rec               *recs;
    size_t             n, cap;
};

static rec *find(gptps_adapt *a, const char *task)
{
    size_t i;
    for (i = 0; i < a->n; ++i) if (strcmp(a->recs[i].name, task) == 0) return &a->recs[i];
    return NULL;
}

static rec *find_or_add(gptps_adapt *a, const char *task)
{
    rec *r = find(a, task);
    if (r) return r;
    if (strlen(task) > GPTPS_TASK_NAME_MAX) return NULL;
    if (a->n == a->cap) {
        size_t nc = a->cap ? a->cap * 2 : 8;
        rec *nr = (rec *)realloc(a->recs, nc * sizeof *nr);
        if (!nr) return NULL;                 /* this attempt goes uncounted: no worse */
        a->recs = nr; a->cap = nc;
    }
    r = &a->recs[a->n++];
    memset(r, 0, sizeof *r);
    strcpy(r->name, task);
    r->r.struct_size = sizeof r->r;
    return r;
}

/* x * (100 + pct) / 100, saturating */
static uint64_t plus_pct(uint64_t x, uint32_t pct)
{
    uint64_t q = x / 100u, rem = x % 100u, f = 100u + pct;
    if (q > UINT64_MAX / f) return UINT64_MAX;
    return q * f + rem * f / 100u;
}

static uint64_t round_mib(uint64_t x)
{
    if (x == 0) return 0;
    if (x > UINT64_MAX - (ADAPT_MIB - 1)) return UINT64_MAX;
    return (x + ADAPT_MIB - 1) / ADAPT_MIB * ADAPT_MIB;
}

/* The suggestion from the evidence (see the header for the rule). */
static void suggest(const gptps_adapt *a, gptps_adapt_report *r)
{
    uint64_t s = 0;
    if (r->cap_hits == 0 && r->samples < a->cfg.min_samples) { r->suggested = 0; return; }
    if (r->peak) s = plus_pct(r->peak, a->cfg.headroom_pct);
    if (r->hit_declared) {
        uint64_t g = plus_pct(r->hit_declared, a->cfg.grow_pct);
        if (g > s) s = g;
    }
    if (s && s < a->cfg.floor_bytes) s = a->cfg.floor_bytes;
    r->suggested = round_mib(s);
}

static gptps_status write_setting(gptps *e, const char *task, uint64_t v)
{
    char key[GPTPS_TASK_NAME_MAX + 32], val[32];
    snprintf(key, sizeof key, "tasks.%s.mem_bytes", task);
    snprintf(val, sizeof val, "%llu", (unsigned long long)v);
    return gptps_settings_set(e, key, val);
}

static void obs(const gptps_event *ev, void *ud)
{
    gptps_adapt *a = (gptps_adapt *)ud;
    const gptps_measure *peak, *hit;
    rec *r;
    int was_hit;
    uint64_t to_write = 0;
    char name[GPTPS_TASK_NAME_MAX + 1];

    if (ev->kind != GPTPS_EV_FINISHED && ev->kind != GPTPS_EV_FAILED) return;
    /* only measured attempts: an in-process one has none, and a cancel or a timeout says
     * nothing about what the job needed */
    if (!gptps_event_measure(ev, GPTPS_M_CPU_USER_MS) && !gptps_event_measure(ev, GPTPS_M_MEM_PEAK)) return;
    if (ev->kind == GPTPS_EV_FAILED &&
        (ev->status == GPTPS_E_CANCELLED || ev->status == GPTPS_E_TIMEOUT || ev->status == GPTPS_E_SHUTDOWN)) return;
    peak = gptps_event_measure(ev, GPTPS_M_MEM_PEAK);
    hit  = gptps_event_measure(ev, GPTPS_M_MEM_CAP_HIT);
    was_hit = (hit && hit->value) || (ev->kind == GPTPS_EV_FAILED && ev->status == GPTPS_E_NOMEM);
    if (!ev->task_name) return;

    apx_mutex_lock(&a->mu);
    r = find_or_add(a, ev->task_name);
    if (r) {
        if (peak) {
            if (!r->r.method[0]) {
                strncpy(r->r.method, peak->method ? peak->method : "", sizeof r->r.method - 1);
                r->r.method[sizeof r->r.method - 1] = 0;
            }
            if (peak->method && strncmp(r->r.method, peak->method, sizeof r->r.method - 1) == 0) {
                if (peak->value > r->r.peak) r->r.peak = peak->value;
            } else {
                r->r.other_method += 1;
                peak = NULL;                        /* not this task's evidence */
            }
        }
        if (peak || was_hit) {
            r->r.samples += 1;
            r->r.declared = ev->mem_bytes;
            if (was_hit) {
                r->r.cap_hits += 1;
                if (ev->mem_bytes > r->r.hit_declared) r->r.hit_declared = ev->mem_bytes;
            }
            suggest(a, &r->r);
            if (a->cfg.auto_apply && r->r.suggested &&
                r->r.suggested != ev->mem_bytes && r->r.suggested != r->last_written) {
                to_write = r->last_written = r->r.suggested;
                strcpy(name, r->name);
            }
        }
    }
    apx_mutex_unlock(&a->mu);

    if (to_write && write_setting(a->e, name, to_write) == GPTPS_OK) {
        apx_mutex_lock(&a->mu);
        if ((r = find(a, name)) != NULL) r->r.applied += 1;
        apx_mutex_unlock(&a->mu);
    }
}

gptps_adapt *gptps_adapt_open(gptps *e, const gptps_adapt_config *cfg)
{
    gptps_adapt *a;
    if (!e) return NULL;
    a = (gptps_adapt *)calloc(1, sizeof *a);
    if (!a) return NULL;
    a->e = e;
    a->cfg.struct_size = sizeof a->cfg;
    /* copy only what the caller's struct has: an older one is shorter */
    if (cfg && cfg->struct_size) {
        size_t n = cfg->struct_size < sizeof a->cfg ? cfg->struct_size : sizeof a->cfg;
        memcpy(&a->cfg, cfg, n);
        a->cfg.struct_size = sizeof a->cfg;
    }
    if (!a->cfg.headroom_pct) a->cfg.headroom_pct = 25;
    if (!a->cfg.min_samples)  a->cfg.min_samples = 5;
    if (!a->cfg.grow_pct)     a->cfg.grow_pct = 50;
    apx_mutex_init(&a->mu);
    if (gptps_register_observer(e, obs, a) != GPTPS_OK) {
        apx_mutex_destroy(&a->mu);
        free(a);
        return NULL;
    }
    return a;
}

gptps_status gptps_adapt_get(gptps_adapt *a, const char *task, gptps_adapt_report *out)
{
    rec *r;
    if (!a || !task || !out || out->struct_size < sizeof *out) return GPTPS_E_INVAL;
    apx_mutex_lock(&a->mu);
    r = find(a, task);
    if (r) *out = r->r;
    apx_mutex_unlock(&a->mu);
    return r ? GPTPS_OK : GPTPS_E_NOTFOUND;
}

gptps_status gptps_adapt_apply(gptps_adapt *a, const char *task)
{
    rec *r;
    uint64_t v = 0;
    gptps_status st;
    int found;
    if (!a || !task) return GPTPS_E_INVAL;
    apx_mutex_lock(&a->mu);
    r = find(a, task);
    found = r != NULL;
    if (r) v = r->r.suggested;
    apx_mutex_unlock(&a->mu);
    if (!found) return GPTPS_E_NOTFOUND;
    if (!v) return GPTPS_E_BUSY;
    st = write_setting(a->e, task, v);
    if (st == GPTPS_OK) {
        apx_mutex_lock(&a->mu);
        if ((r = find(a, task)) != NULL) { r->r.applied += 1; r->last_written = v; }
        apx_mutex_unlock(&a->mu);
    }
    return st;
}

void gptps_adapt_close(gptps_adapt *a)
{
    if (!a) return;
    free(a->recs);
    apx_mutex_destroy(&a->mu);
    free(a);
}
