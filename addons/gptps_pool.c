/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_pool.c - N-shard router built purely on the public GPTPS API.
 *
 * See gptps_pool.h. The pool owns N engines and a round-robin cursor; a submit is
 * routed to one engine and the returned handle is tagged with the shard index so a
 * later cancel finds its way back. The only pool-level shared state is the cursor,
 * guarded by a mutex held for a single increment - negligible next to the per-engine
 * dispatch lock it relieves (and keyed routing avoids even that).
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L   /* expose pthread + fileno for addon_compat.h */
#endif
#include "gptps_pool.h"
#include "addon_compat.h"

#include <stdlib.h>
#include <string.h>

/* pool handle layout: [ 16-bit shard | 48-bit engine handle ]. Engine handles start
 * at 1 and increment, so 48 bits (2.8e14) is far more than any real run submits. */
#define GPTPS_POOL_SHARD_BITS  16
#define GPTPS_POOL_HANDLE_BITS  48
#define GPTPS_POOL_HANDLE_MASK  ((gptps_pool_handle)0xFFFFFFFFFFFFull)
#define GPTPS_POOL_MAX_SHARDS   ((size_t)0xFFFFu)

struct gptps_pool {
    gptps    **shards;
    size_t     n;
    apx_mutex  cursor_lock;   /* guards `cursor` (round-robin only) */
    uint64_t   cursor;
};

static gptps_pool_handle pool_pack(size_t shard, gptps_handle h)
{
    if (h == 0) return 0;     /* engine reported no handle */
    return ((gptps_pool_handle)shard << GPTPS_POOL_HANDLE_BITS) | ((gptps_pool_handle)h & GPTPS_POOL_HANDLE_MASK);
}

gptps_pool *gptps_pool_open(size_t nshards, const gptps_config *cfg)
{
    gptps_pool *p;
    size_t i;

    if (nshards < 1 || nshards > GPTPS_POOL_MAX_SHARDS) return NULL;

    p = (gptps_pool *)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->shards = (gptps **)calloc(nshards, sizeof *p->shards);
    if (!p->shards) { free(p); return NULL; }
    apx_mutex_init(&p->cursor_lock);
    p->cursor = 0;

    for (i = 0; i < nshards; ++i) {
        if (gptps_open_ex(cfg, &p->shards[i]) != GPTPS_OK || !p->shards[i]) {
            /* roll back the shards already opened */
            size_t j;
            for (j = 0; j < i; ++j) gptps_shutdown(p->shards[j]);
            apx_mutex_destroy(&p->cursor_lock);
            free(p->shards); free(p);
            return NULL;
        }
        p->n = i + 1;   /* keep n consistent as we go (for a clean partial teardown) */
    }
    return p;
}

size_t gptps_pool_count(gptps_pool *p) { return p ? p->n : 0; }

gptps *gptps_pool_shard(gptps_pool *p, size_t i)
{ return (p && i < p->n) ? p->shards[i] : NULL; }

gptps_status gptps_pool_register_task(gptps_pool *p, const gptps_task_def *def)
{
    size_t i;
    gptps_status st;
    if (!p || !def) return GPTPS_E_INVAL;
    for (i = 0; i < p->n; ++i) {
        st = gptps_register_task(p->shards[i], def);
        if (st != GPTPS_OK) {
            /* all-or-nothing: undo the shards already registered (no work yet, so
             * REJECT_IF_BUSY removes immediately) */
            size_t j;
            for (j = 0; j < i; ++j)
                gptps_unregister_task(p->shards[j], def->name, GPTPS_REMOVE_REJECT_IF_BUSY);
            return st;
        }
    }
    return GPTPS_OK;
}

/* pick the next shard round-robin (tiny critical section) */
static size_t next_rr(gptps_pool *p)
{
    size_t idx;
    apx_mutex_lock(&p->cursor_lock);
    idx = (size_t)(p->cursor++ % p->n);
    apx_mutex_unlock(&p->cursor_lock);
    return idx;
}

gptps_status gptps_pool_submit(gptps_pool *p, const char *task,
                               const void *payload, size_t len, gptps_pool_handle *out)
{
    size_t idx;
    gptps_handle h = 0;
    gptps_status st;
    if (out) *out = 0;
    if (!p || !task) return GPTPS_E_INVAL;
    idx = next_rr(p);
    st = gptps_submit(p->shards[idx], task, payload, len, &h);
    if (st == GPTPS_OK && out) *out = pool_pack(idx, h);
    return st;
}

gptps_status gptps_pool_submit_keyed(gptps_pool *p, uint64_t key, const char *task,
                                     const void *payload, size_t len, gptps_pool_handle *out)
{
    size_t idx;
    gptps_handle h = 0;
    gptps_status st;
    if (out) *out = 0;
    if (!p || !task) return GPTPS_E_INVAL;
    idx = (size_t)(key % (uint64_t)p->n);   /* lock-free: same key -> same shard */
    st = gptps_submit(p->shards[idx], task, payload, len, &h);
    if (st == GPTPS_OK && out) *out = pool_pack(idx, h);
    return st;
}

gptps_status gptps_pool_cancel(gptps_pool *p, gptps_pool_handle h)
{
    size_t shard;
    gptps_handle eh;
    if (!p || h == 0) return GPTPS_E_INVAL;
    shard = (size_t)(h >> GPTPS_POOL_HANDLE_BITS);
    eh    = (gptps_handle)(h & GPTPS_POOL_HANDLE_MASK);
    if (shard >= p->n) return GPTPS_E_INVAL;
    return gptps_cancel(p->shards[shard], eh);
}

size_t gptps_pool_dead_letter_count(gptps_pool *p)
{
    size_t i, total = 0;
    if (!p) return 0;
    for (i = 0; i < p->n; ++i) total += gptps_dead_letter_count(p->shards[i]);
    return total;
}

void gptps_pool_close(gptps_pool *p)
{
    size_t i;
    if (!p) return;
    for (i = 0; i < p->n; ++i) gptps_shutdown(p->shards[i]);
    apx_mutex_destroy(&p->cursor_lock);
    free(p->shards);
    free(p);
}
