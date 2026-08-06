/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gpu_quota.h - GPU-unit admission quota for GPTPS (optional add-on).
 *
 * Caps total in-flight GPU usage so the engine never STARTS more GPU work than
 * your budget allows. This is admission-level oversubscription prevention, not
 * driver-level VRAM enforcement (that needs vendor APIs and real hardware); pair
 * it with an OOP/cgroup memory cap when you need a hard limit.
 *
 * ---------------------------------------------------------------------------
 * This add-on is now a THIN WRAPPER over the core's named-resource budgets
 * (gptps_define_resource / gptps_set_task_resource_cost / gptps_resource_usage,
 * ABI 1.10). It carries no counter, no lock, and no bookkeeping of its own.
 *
 * That is deliberate, and it is the point: "GPU units" is not a mechanism, it is
 * a NAME for one. The core already reserves an arbitrary named budget on admit
 * and releases it at a terminal state, so an add-on re-implementing that with a
 * constraint + observer pair was duplicating the engine - and, because it tracked
 * releases by task name through the event stream, it silently leaked its budget
 * whenever an item reached a terminal state the observer did not see. Deleting
 * that machinery deletes the bug class with it.
 *
 * Read this file as the worked example of the named-resource API. "Licence
 * seats", "IO bandwidth", or "tenant quota" is the same code with a different
 * string - which is why the core has no idea what a GPU is.
 * ---------------------------------------------------------------------------
 *
 * Model: units are a fixed property of a task TYPE (the natural case - "this
 * model needs 2 units"), declared per type after registration.
 *
 *     q = gptps_gpu_quota_install(e, 8, 0);           // 8 units total
 *     gptps_register_task(e, &def);                   // ... your GPU task
 *     gptps_gpu_quota_set_task_units(q, "infer", 2);  // each item costs 2
 *     ... submit work ...
 *     gptps_shutdown(e);
 *     gptps_gpu_quota_close(q);
 *
 * An item that does not fit the live budget is simply not admitted this pass and
 * is reconsidered on the next one - the engine's own skip-to-fit backfill and
 * starvation guard apply, so a large GPU task cannot be starved by small ones.
 * Portable: no platform code at all.
 */
#ifndef GPTPS_GPU_QUOTA_H
#define GPTPS_GPU_QUOTA_H

#include "gptps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gptps_gpu_quota gptps_gpu_quota;

/* The named resource this add-on manages. Exposed so a host can read it directly
 * with gptps_resource_usage() or gate on it from its own constraint. */
#define GPTPS_GPU_RESOURCE "gpu"

/* Install a GPU-unit quota of `total_units` on engine `e`. Setup-time: call
 * before submitting work. `defer_ms` is ACCEPTED AND IGNORED - it existed to
 * pace this add-on's own DEFER retry loop, and the dispatcher now reconsiders a
 * non-fitting item every pass with no polling interval to tune. It is kept so
 * existing call sites still compile. Returns NULL on failure. */
gptps_gpu_quota *gptps_gpu_quota_install(gptps *e, uint64_t total_units, uint32_t defer_ms);

/* Declare how many GPU units ONE item of `task_name` costs (0 = not gated). The
 * task must already be registered. This replaces the removed
 * gptps_cost.gpu_units field: cost against a named resource is now declared
 * per task type through the core, not carried in every task's cost struct. */
gptps_status gptps_gpu_quota_set_task_units(gptps_gpu_quota *q, const char *task_name,
                                            uint64_t units);

/* ---------------------------------------------------------------------------
 * LIFETIME - changed in ABI 2.0, read this if you are porting.
 *
 * The quota no longer owns a counter; it is a VIEW onto the engine's live
 * resource ledger. So every call below except _close() reads engine state and is
 * valid ONLY while the engine is open. Calling them after gptps_shutdown() is a
 * use-after-free, not a stale read.
 *
 * The old add-on kept its own counter, so `_in_use()` happened to still answer
 * after shutdown. It no longer does - query before you shut down:
 *
 *     ... submit work, let it drain ...
 *     printf("%llu still reserved\n", gptps_gpu_quota_in_use(q));   // engine ALIVE
 *     gptps_shutdown(e);
 *     gptps_gpu_quota_close(q);                                     // only legal call now
 * ------------------------------------------------------------------------- */

/* GPU units currently reserved by in-flight tasks. Engine must be open. */
uint64_t gptps_gpu_quota_in_use(gptps_gpu_quota *q);

/* Total budget currently in effect. Engine must be open. */
uint64_t gptps_gpu_quota_total(gptps_gpu_quota *q);

/* Retune the total budget at runtime (also exposed as the "gpu_quota.total_units"
 * setting in the engine's settings registry). Engine must be open. */
gptps_status gptps_gpu_quota_set_total(gptps_gpu_quota *q, uint64_t total_units);

/* Release the handle. The engine owns the budget and the settings entry, so call
 * this AFTER gptps_shutdown() - the settings entry points at this handle and the
 * engine may read it until it is torn down. This is the only call that is legal
 * once the engine is gone. */
void gptps_gpu_quota_close(gptps_gpu_quota *q);

#ifdef __cplusplus
}
#endif
#endif /* GPTPS_GPU_QUOTA_H */
