/*
 * gpu_quota.h - GPU-unit admission quota for GPTPS (optional add-on).
 *
 * Caps the total in-flight GPU usage by the `gpu_units` each task type declares
 * in its cost. Built only on the public seams: a CONSTRAINT gates admission
 * (reserve units, or DEFER when the budget is full) and an OBSERVER releases
 * units when a task run ends. This is admission-level *oversubscription
 * prevention* - it stops the engine from starting more GPU work than your budget
 * allows. It is NOT driver-level VRAM enforcement (that needs vendor APIs / real
 * hardware); pair it with an OOP/cgroup memory cap for hard limits.
 *
 * Model: gpu_units is treated as a fixed property of a task TYPE (the natural
 * case - "this model needs 2 units"). Install it as the engine's GPU constraint.
 *
 * Lifecycle (like the durable queue): close AFTER gptps_shutdown - the engine has
 * no unregister call, so the quota must outlive event delivery.
 *
 *     q = gptps_gpu_quota_install(e, 8, 25);   // 8 units total, 25ms defer
 *     ... submit GPU tasks (def.default_cost.gpu_units = N) ...
 *     gptps_shutdown(e);
 *     gptps_gpu_quota_close(q);
 *
 * Portable (Linux/macOS/Windows) via the addon_compat mutex shim.
 */
#ifndef GPTPS_GPU_QUOTA_H
#define GPTPS_GPU_QUOTA_H

#include "gptps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gptps_gpu_quota gptps_gpu_quota;

/* Install a GPU-unit quota of `total_units` on engine `e`. A task whose
 * gpu_units would exceed the live budget is deferred and re-checked after
 * `defer_ms` (0 => a small default). Tasks with gpu_units == 0 are never gated.
 * Returns NULL on allocation/registration failure. */
gptps_gpu_quota *gptps_gpu_quota_install(gptps *e, uint64_t total_units, uint32_t defer_ms);

/* GPU units currently reserved by in-flight tasks. */
uint64_t gptps_gpu_quota_in_use(gptps_gpu_quota *q);

/* Total budget the quota was installed with. */
uint64_t gptps_gpu_quota_total(gptps_gpu_quota *q);

/* Retune the total budget at runtime (also exposed as the "gpu_quota.total_units"
 * setting in the engine's settings registry). */
gptps_status gptps_gpu_quota_set_total(gptps_gpu_quota *q, uint64_t total_units);

/* Free the quota. Call AFTER gptps_shutdown(e). */
void gptps_gpu_quota_close(gptps_gpu_quota *q);

#ifdef __cplusplus
}
#endif
#endif /* GPTPS_GPU_QUOTA_H */
