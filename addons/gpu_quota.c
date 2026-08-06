/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gpu_quota.c - GPU-unit admission quota (see gpu_quota.h).
 *
 * Every operation forwards to the core's named-resource budgets. There is no
 * counter, no mutex, and no name->units table here any more: the engine reserves
 * on admit and releases at a terminal state, under the same lock that makes the
 * admission decision, so the reserve/release pair cannot drift.
 *
 * The previous implementation was a constraint + observer pair with its own
 * counter, which had to infer releases from the event stream by task name. That
 * made it structurally vulnerable to any terminal path the observer did not see -
 * and there was one, so a cancelled admitted item leaked its units permanently
 * until the gate deferred everything forever. Reserving through the core removes
 * the failure mode rather than patching it.
 */
#include "gpu_quota.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct gptps_gpu_quota { gptps *e; };

/* settings registry bindings (target = the quota) */
static size_t gq_rd_total(void *p, char *b, size_t c)
{
    gptps_gpu_quota *q = (gptps_gpu_quota *)p;
    uint64_t budget = 0;
    gptps_resource_usage(q->e, GPTPS_GPU_RESOURCE, NULL, &budget);
    return (size_t)snprintf(b, c, "%llu", (unsigned long long)budget);
}
static gptps_status gq_wr_total(void *p, const char *v)
{
    return gptps_gpu_quota_set_total((gptps_gpu_quota *)p, (uint64_t)strtoull(v, NULL, 10));
}

gptps_gpu_quota *gptps_gpu_quota_install(gptps *e, uint64_t total_units, uint32_t defer_ms)
{
    gptps_gpu_quota *q;
    (void)defer_ms;   /* see gpu_quota.h: accepted for source compatibility, unused */
    if (!e) return NULL;
    q = (gptps_gpu_quota *)calloc(1, sizeof *q);
    if (!q) return NULL;
    q->e = e;

    if (gptps_define_resource(e, GPTPS_GPU_RESOURCE, total_units) != GPTPS_OK) {
        free(q);      /* nothing registered yet: safe to free */
        return NULL;
    }
    {   /* expose the budget as a runtime setting */
        gptps_setting_def d;
        memset(&d, 0, sizeof d); d.struct_size = sizeof d;
        d.key = "gpu_quota.total_units"; d.type = GPTPS_SETTING_UINT; d.hot = 1;
        d.desc = "total GPU units budget"; d.target = q; d.read = gq_rd_total; d.write = gq_wr_total;
        gptps_register_setting(e, &d);
    }
    return q;
}

gptps_status gptps_gpu_quota_set_task_units(gptps_gpu_quota *q, const char *task_name,
                                            uint64_t units)
{
    if (!q) return GPTPS_E_INVAL;
    return gptps_set_task_resource_cost(q->e, task_name, GPTPS_GPU_RESOURCE, units);
}

gptps_status gptps_gpu_quota_set_total(gptps_gpu_quota *q, uint64_t total_units)
{
    if (!q) return GPTPS_E_INVAL;
    return gptps_define_resource(q->e, GPTPS_GPU_RESOURCE, total_units); /* re-budgets */
}

uint64_t gptps_gpu_quota_in_use(gptps_gpu_quota *q)
{
    uint64_t reserved = 0;
    if (!q) return 0;
    gptps_resource_usage(q->e, GPTPS_GPU_RESOURCE, &reserved, NULL);
    return reserved;
}

uint64_t gptps_gpu_quota_total(gptps_gpu_quota *q)
{
    uint64_t budget = 0;
    if (!q) return 0;
    gptps_resource_usage(q->e, GPTPS_GPU_RESOURCE, NULL, &budget);
    return budget;
}

void gptps_gpu_quota_close(gptps_gpu_quota *q)
{
    /* The engine owns the budget and the settings entry; this is just the handle.
     * The settings entry's target dangles after this, so close it only once the
     * engine is done with it - same lifetime rule as before. */
    free(q);
}
