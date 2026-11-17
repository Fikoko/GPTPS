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
#include <stdio.h>    /* snprintf: the grace setting, and what close reports */

/* pool handle layout: [ 16-bit shard | 48-bit engine handle ]. Engine handles start
 * at 1 and increment, so 48 bits (2.8e14) is far more than any real run submits. */
#define GPTPS_POOL_SHARD_BITS  16
#define GPTPS_POOL_HANDLE_BITS  48
#define GPTPS_POOL_HANDLE_MASK  ((gptps_pool_handle)0xFFFFFFFFFFFFull)
#define GPTPS_POOL_MAX_SHARDS   ((size_t)0xFFFFu)

struct gptps_pool {
    gptps    **shards;
    size_t     n;
    apx_mutex  cursor_lock;   /* guards `cursor` (round-robin only), and `closing` */
    uint64_t   cursor;
    int        closing;       /* gptps_pool_close has begun (gptps_pool_closing) */
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

/* One submit to shard `idx`. The options go through gptps_submit_ex unchanged: a
 * shard is an engine of this process, on the same clock, so even a due time needs no
 * translation. gptps_submit_ex with NULL options is gptps_submit. */
static gptps_status shard_submit(gptps_pool *p, size_t idx, const char *task, const void *payload,
                                 size_t len, const gptps_submit_options *opts, gptps_pool_handle *out)
{
    gptps_handle h = 0;
    gptps_status st = gptps_submit_ex(p->shards[idx], task, payload, len, opts, &h);
    if (st == GPTPS_OK && out) *out = pool_pack(idx, h);
    return st;
}

gptps_status gptps_pool_submit(gptps_pool *p, const char *task,
                               const void *payload, size_t len, gptps_pool_handle *out)
{
    return gptps_pool_submit_ex(p, task, payload, len, NULL, out);
}

gptps_status gptps_pool_submit_keyed(gptps_pool *p, uint64_t key, const char *task,
                                     const void *payload, size_t len, gptps_pool_handle *out)
{
    return gptps_pool_submit_keyed_ex(p, key, task, payload, len, NULL, out);
}

gptps_status gptps_pool_submit_ex(gptps_pool *p, const char *task,
                                  const void *payload, size_t len,
                                  const gptps_submit_options *opts, gptps_pool_handle *out)
{
    if (out) *out = 0;
    if (!p || !task) return GPTPS_E_INVAL;
    return shard_submit(p, next_rr(p), task, payload, len, opts, out);
}

gptps_status gptps_pool_submit_keyed_ex(gptps_pool *p, uint64_t key, const char *task,
                                        const void *payload, size_t len,
                                        const gptps_submit_options *opts, gptps_pool_handle *out)
{
    if (out) *out = 0;
    if (!p || !task) return GPTPS_E_INVAL;
    /* lock-free: same key -> same shard */
    return shard_submit(p, (size_t)(key % (uint64_t)p->n), task, payload, len, opts, out);
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

int gptps_pool_closing(gptps_pool *p)
{
    int c;
    if (!p) return 0;
    apx_mutex_lock(&p->cursor_lock);
    c = p->closing;
    apx_mutex_unlock(&p->cursor_lock);
    return c;
}

/* A shard's limits.shutdown_grace_ms; 0 means "wait forever", as the engine reads it. */
static uint64_t shard_grace(gptps *e)
{
    char buf[32];
    if (gptps_settings_get(e, "limits.shutdown_grace_ms", buf, sizeof buf) != GPTPS_OK) return 30000u;
    return strtoull(buf, NULL, 10);
}

/* ONE grace for the whole pool. gptps_shutdown is bounded per engine, and shards are
 * shut down one after another on the calling thread (it is the thread gptps_shutdown
 * checks for a callback - a helper thread would not be refused, and would join the very
 * thread waiting for it). So each shard's grace is counted from the moment close
 * began, not from its own turn: before shard k is shut down, what is left of its
 * grace is written to its live limits.shutdown_grace_ms (at least 1 ms - 0 would mean
 * forever). A pool's close took up to N x shutdown_grace_ms; now it takes at most the
 * largest grace plus each shard's ~200 ms hard-kill slice, and a shard left waiting
 * for its turn ran its own work in the meantime, as it would during its own drain.
 *
 * A shard that refuses (GPTPS_E_BUSY: close was called from one of its callbacks) is
 * left running, and so is the pool: it is reported, not freed, and a later call from
 * the host's own thread finishes it - freeing the shards under a running engine was a
 * use-after-free. */
void gptps_pool_close(gptps_pool *p)
{
    size_t i, busy = 0;
    uint64_t start;
    if (!p) return;
    apx_mutex_lock(&p->cursor_lock);
    p->closing = 1;              /* gptps_balance stops handing out its backlog */
    apx_mutex_unlock(&p->cursor_lock);
    start = gptps_now_ms(NULL);
    for (i = 0; i < p->n; ++i) {
        uint64_t grace, used;
        gptps_status st;
        if (!p->shards[i]) continue;                 /* shut down by an earlier call */
        grace = shard_grace(p->shards[i]);
        used = gptps_now_ms(NULL) - start;
        if (grace && used > 0) {                     /* 0: this shard waits forever */
            char v[32];
            snprintf(v, sizeof v, "%llu", (unsigned long long)(used < grace ? grace - used : 1u));
            (void)gptps_settings_set(p->shards[i], "limits.shutdown_grace_ms", v);
        }
        st = gptps_shutdown(p->shards[i]);
        if (st == GPTPS_E_BUSY) { ++busy; continue; }
        p->shards[i] = NULL;
    }
    if (busy) {
        char msg[256];
        snprintf(msg, sizeof msg, "gptps_pool_close: called from a callback of %lu of its shards, "
                 "which cannot shut down from there (GPTPS_E_BUSY); they and the pool are left "
                 "as they are - call gptps_pool_close again from the host's own thread",
                 (unsigned long)busy);
        gptps_log(NULL, GPTPS_LOG_ERROR, msg);
        return;
    }
    apx_mutex_destroy(&p->cursor_lock);
    free(p->shards);
    free(p);
}
