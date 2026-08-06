/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_pool.h - horizontal scale-up by COMPOSITION (optional add-on).
 *
 * The GPTPS core is a single-writer engine: one lock, one dispatcher. That is
 * simple and correct, but it is also the single-node throughput ceiling. Rather
 * than complicate the core, this add-on scales past that ceiling the modular way -
 * it runs N INDEPENDENT engine shards (each its own lock + dispatcher + worker
 * pool) and routes each submit to one of them. Nothing here touches the core; it
 * is built entirely on the public API, so the proof that "scale by composition"
 * works is that this file needs no engine changes at all.
 *
 *   pool = gptps_pool_open(4, &cfg);          // 4 independent shards
 *   gptps_pool_register_task(pool, &def);     // same task type on every shard
 *   gptps_pool_submit(pool, "resize", buf, len, &h);       // round-robin
 *   gptps_pool_submit_keyed(pool, tenant_id, "resize", ..., &h); // affinity
 *   gptps_pool_cancel(pool, h);
 *   gptps_pool_close(pool);                    // shuts down + frees every shard
 *
 * SIZING: with N shards you get N independent locks/dispatchers, so aggregate
 * budget and concurrency are N x the per-shard cfg. Set cfg->limits so the shards
 * SUM to what the machine can bear (e.g. max_concurrent_tasks ~= cores / N); a
 * NULL cfg auto-tunes EACH shard to the whole machine, which oversubscribes for
 * N > 1. Register the SAME task types on every shard (a submit may land on any).
 *
 * ROUTING: gptps_pool_submit spreads load round-robin. gptps_pool_submit_keyed
 * sends a given key to a fixed shard (key % N) - use it for per-key affinity
 * (per-tenant locality, or preserving order among items that share a key, since
 * a single shard admits in its own order). Keyed routing is fully lock-free.
 *
 * Portable (Linux/macOS/Windows) via addon_compat.h.
 */
#ifndef GPTPS_POOL_H
#define GPTPS_POOL_H

#include "gptps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gptps_pool gptps_pool;

/* An opaque pool handle: encodes (shard index, that shard's engine handle) so a
 * single value routes back to the right shard for gptps_pool_cancel. 0 = none. */
typedef uint64_t gptps_pool_handle;

/* Open a pool of `nshards` (>=1, <=65535) independent engine shards, each opened
 * from `cfg` (copied; may be NULL for per-shard auto-tune - see SIZING above).
 * Returns NULL on a bad argument, OOM, or if any shard fails to open. */
gptps_pool *gptps_pool_open(size_t nshards, const gptps_config *cfg);

/* Number of shards, and direct access to shard `i` (0-based) for per-shard setup:
 * registering observers/settings, introspection, or MANUAL-mode stepping. NULL if
 * `i` is out of range. Do NOT gptps_shutdown a shard yourself - gptps_pool_close does. */
size_t gptps_pool_count(gptps_pool *p);
gptps *gptps_pool_shard(gptps_pool *p, size_t i);

/* Register a task type on EVERY shard (fans `def` out). All-or-nothing: on a
 * failure it rolls the type back off the shards already done and returns the error.
 * A setup-time call, like gptps_register_task. */
gptps_status gptps_pool_register_task(gptps_pool *p, const gptps_task_def *def);

/* Submit work. gptps_pool_submit picks the next shard round-robin; _keyed picks
 * shard (key % nshards) for affinity. `out` (may be NULL) receives the pool handle.
 * Returns the chosen shard's gptps_submit status. */
gptps_status gptps_pool_submit(gptps_pool *p, const char *task,
                               const void *payload, size_t len, gptps_pool_handle *out);
gptps_status gptps_pool_submit_keyed(gptps_pool *p, uint64_t key, const char *task,
                                     const void *payload, size_t len, gptps_pool_handle *out);

/* Cancel a work item by its pool handle (routes to the owning shard). GPTPS_E_INVAL
 * on a malformed handle; otherwise the shard's gptps_cancel status. */
gptps_status gptps_pool_cancel(gptps_pool *p, gptps_pool_handle h);

/* Aggregate: total retained dead-lettered items across all shards. */
size_t gptps_pool_dead_letter_count(gptps_pool *p);

/* Shut down (drain + join) and free every shard, then the pool. */
void gptps_pool_close(gptps_pool *p);

#ifdef __cplusplus
}
#endif
#endif /* GPTPS_POOL_H */
