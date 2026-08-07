/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_plugin_tier.c - the two-tier model, exercised rather than asserted.
 *
 * addons/gptps_gpu_quota.c and addons/gptps_gpu_quota_plugin.c carry the SAME policy in the two
 * tiers. This drives the Tier-B one the way its tier is meant to be driven: the host
 * calls nothing on it, loads it as a binary, and configures it purely through
 * settings - then checks the policy actually took effect.
 *
 * That last part is what makes this a test rather than a demo. A plug-in that loads
 * and registers knobs nobody reads would pass a weaker check; here the quota has to
 * genuinely throttle admission.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

#ifndef ADDON_GPUQ_PATH
#define ADDON_GPUQ_PATH "./gpu_quota_plugin.so"
#endif

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int g_release, g_ran;
static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }

/* Occupies its quota until released, so "is admission actually throttled?" is
 * observable rather than inferred from timing. */
static gptps_status hog(gptps_ctx *c, void *u)
{
    (void)u;
    inc(&g_ran);
    while (!get(&g_release) && !gptps_is_cancelled(c)) { }
    return GPTPS_OK;
}

int main(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    gptps_task_def d;
    gptps_handle h;
    int i;

    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 8;   /* concurrency is NOT the limiter here */
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (!e) { printf("open failed\n"); return 1; }
    CHECK(gptps_settings_set(e, "limits.shutdown_grace_ms", "500") == GPTPS_OK);

    /* Loaded as a BINARY. The host makes no call into it, now or ever. */
    CHECK(gptps_load_addon(e, ADDON_GPUQ_PATH) == GPTPS_OK);

    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "render"; d.run = hog; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);

    /* ===== configure it the way an operator would: settings only ===== */
    CHECK(gptps_settings_set(e, "gpuq.total_units", "4") == GPTPS_OK);
    /* The per-task knob the plug-in defined for EVERY task type, including one
     * registered after it loaded. Writing it makes the plug-in apply the cost. */
    CHECK(gptps_settings_set(e, "tasks.render.gpuq.units", "2") == GPTPS_OK);

    {   /* the budget really is what we set */
        uint64_t reserved = 0, budget = 0;
        CHECK(gptps_resource_usage(e, "gpuq.units", &reserved, &budget) == GPTPS_OK);
        CHECK(budget == 4);
    }

    /* ===== the policy BITES: 4 units / 2 per task => at most 2 concurrent ===== */
    __atomic_store_n(&g_release, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_ran, 0, __ATOMIC_SEQ_CST);
    for (i = 0; i < 6; ++i)
        CHECK(gptps_submit(e, "render", NULL, 0, &h) == GPTPS_OK);

    {   /* wait for the quota to fill, then confirm it does not exceed */
        uint64_t t0 = gptps_now_ms(NULL), reserved = 0, budget = 0;
        while (get(&g_ran) < 2 && gptps_now_ms(NULL) - t0 < 3000) { }
        CHECK(get(&g_ran) == 2);          /* two admitted... */
        gptps_resource_usage(e, "gpuq.units", &reserved, &budget);
        CHECK(reserved == 4);             /* ...consuming the whole budget */

        /* Hold long enough that a broken quota would let a third in. Concurrency is
         * 8, so nothing but the quota is stopping them. */
        t0 = gptps_now_ms(NULL);
        while (gptps_now_ms(NULL) - t0 < 200) { }
        CHECK(get(&g_ran) == 2);          /* still exactly two */
    }

    __atomic_store_n(&g_release, 1, __ATOMIC_SEQ_CST);

    /* ===== introspection sees it, and disable turns the policy off ===== */
    {
        gptps_addon_info info;
        memset(&info, 0, sizeof info);
        info.struct_size = sizeof info;
        CHECK(gptps_addon_get_info(e, 0, &info) == GPTPS_OK);
        CHECK(info.ns && strcmp(info.ns, "gpuq") == 0);
        CHECK(info.enabled == 1);
    }
    CHECK(gptps_addon_disable(e, "gpuq") == GPTPS_OK);
    {
        /* Widened until it constrains nothing. NOT zero: the engine admits when
         * reserved+cost <= budget, so a zero budget with a non-zero cost admits
         * NOTHING - it is the most restrictive setting, not the least. */
        uint64_t budget = 0;
        CHECK(gptps_resource_usage(e, "gpuq.units", NULL, &budget) == GPTPS_OK);
        CHECK(budget == (uint64_t)~(uint64_t)0);
    }

    gptps_shutdown(e);

    if (fails) { printf("%d plugin-tier check(s) FAILED\n", fails); return 1; }
    printf("all plugin tier checks passed\n");
    return 0;
}
