/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gpu_quota_plugin.c - the SAME policy as addons/gpu_quota.c, as a BINARY PLUG-IN.
 *
 * These two files ship side by side deliberately, and the diff between them is the
 * documentation of the two-tier model - more convincing than any prose about it:
 *
 *   addons/gpu_quota.c        TIER A, compiled-in module.
 *     The host calls gptps_gpu_quota_install() and holds a gptps_gpu_quota*. It
 *     needs a header, therefore it needs your build. It links core symbols directly.
 *
 *   addons/gpu_quota_plugin.c TIER B, binary plug-in (this file).
 *     The host calls NOTHING. It is named in a TOML `addons = [...]` line, and an
 *     operator configures it entirely through settings. It links NO core symbols -
 *     everything goes through the passed gptps_api_routines table, which is what
 *     lets one .so work against a static, shared or amalgamated host.
 *
 * The tier rule that decides which one anything should be:
 *     Does the host have to call INTO the module?
 *       yes -> compiled-in module      no -> binary plug-in
 * That is why gptps_pool and gptps_xport can never be plug-ins (they own engines
 * and processes, so the host must drive them), and why a QUOTA can: a quota is a
 * policy, and policy is exactly what an operator wants to add without recompiling.
 *
 * Namespaced "gpuq", so it cannot collide with another plug-in - including with the
 * Tier-A gpu_quota, whose resource is called "gpu". The two can coexist in one
 * process precisely because this one claims a namespace.
 *
 * Configuration, all of it, with no host involvement:
 *     gpuq.total_units            the budget           (global setting)
 *     tasks.<task>.gpuq.units     per-task cost        (per-task setting)
 * Writing either at runtime takes effect immediately, which is the whole point of
 * the tier: policy that moves without a rebuild.
 */
#include "gptps.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>

#define GPUQ_NS       "gpuq"
#define GPUQ_RESOURCE "gpuq.units"          /* namespaced, per our own rule */
#define GPUQ_TOTAL    "gpuq.total_units"
#define GPUQ_LEAF     "gpuq.units"          /* -> tasks.<task>.gpuq.units */

/* "No limit", in the mechanism's own terms.
 *
 * The engine admits when `reserved + cost <= budget`, so a budget of ZERO with any
 * non-zero cost admits NOTHING - it is maximally restrictive, not unlimited. An
 * operator typing 0 into a quota means the opposite, so this plug-in translates:
 * 0 in the setting means unlimited, and unlimited is expressed to the engine as the
 * largest budget nothing can exceed. Getting this backwards silently wedges every
 * task of the type, which is exactly the sort of thing a policy plug-in must not do. */
#define GPUQ_UNLIMITED ((uint64_t)~(uint64_t)0)

static const gptps_api_routines *g_api;
static gptps *g_engine;

static uint64_t gpuq_budget_of(const char *value)
{
    uint64_t v = (uint64_t)strtoull(value, NULL, 10);
    return v ? v : GPUQ_UNLIMITED;
}

/* An operator wrote a setting. If it was a per-task quota, apply it as this task's
 * cost against our resource.
 *
 * Keys arrive as "tasks.<task>.gpuq.units". The task name is everything between the
 * first and the last "." of that shape, so it is extracted rather than assumed - a
 * task name may itself contain dots. */
static void on_setting(const char *key, const char *value, void *ud)
{
    const char *rest, *tail;
    char task[128];
    size_t n;
    (void)ud;

    if (!key || !value) return;

    /* the budget itself */
    if (strcmp(key, GPUQ_TOTAL) == 0) {
        g_api->define_resource(g_engine, GPUQ_RESOURCE, gpuq_budget_of(value));
        return;
    }

    if (strncmp(key, "tasks.", 6) != 0) return;
    rest = key + 6;

    /* must end with "." GPUQ_LEAF */
    n = strlen(rest);
    if (n <= sizeof(GPUQ_LEAF)) return;                       /* need "<task>." + leaf */
    tail = rest + n - (sizeof(GPUQ_LEAF) - 1);
    if (strcmp(tail, GPUQ_LEAF) != 0) return;
    if (tail == rest || tail[-1] != '.') return;

    n = (size_t)(tail - 1 - rest);                            /* length of <task> */
    if (n == 0 || n >= sizeof task) return;
    memcpy(task, rest, n);
    task[n] = '\0';

    g_api->set_task_resource_cost(g_engine, task, GPUQ_RESOURCE,
                                  (uint64_t)strtoull(value, NULL, 10));
}

static gptps_status gpuq_setup(gptps *e, const gptps_api_routines *api, char **err)
{
    gptps_status st;
    (void)err;
    g_api = api;
    g_engine = e;

    /* Guard the whole 2.1 tranche before using any of it. Against an older core we
     * refuse to load rather than half-installing: settings_watch is what makes this
     * plug-in configurable at all, so without it there would be nothing to load. */
    if (api->struct_size <= offsetof(gptps_api_routines, settings_watch) ||
        !api->settings_watch || !api->define_global || !api->define_task_setting)
        return GPTPS_E_ABI;

    /* The budget. Reserved and released by the ENGINE under the same lock that makes
     * the admission decision, so the reserve/release pair cannot drift - the failure
     * mode the pre-ABI-2.0 constraint+observer implementation had.
     *
     * Starts UNLIMITED, deliberately. Loading a policy plug-in must not change
     * behaviour until it is configured: a zero budget here would stall every task
     * that later gets a non-zero cost, so merely having the plug-in present would
     * wedge the engine. Opt-in, not opt-out. */
    st = api->define_resource(e, GPUQ_RESOURCE, GPUQ_UNLIMITED);
    if (st != GPTPS_OK) return st;

    st = api->define_global(e, GPUQ_TOTAL, GPTPS_SETTING_UINT, "0", 0, 0);
    if (st != GPTPS_OK) return st;

    /* Applies to every task type registered now OR LATER - so a task added after
     * this plug-in loaded still gets its quota knob. */
    st = api->define_task_setting(e, GPUQ_LEAF, GPTPS_SETTING_UINT, "0", 0, 0);
    if (st != GPTPS_OK) return st;

    return api->settings_watch(e, on_setting, 0);
}

/* Stop participating: widen the budget until it constrains nothing.
 *
 * Note what disable does NOT do - it cannot unregister the settings it defined,
 * because there is no gptps_unregister_setting. That is the same absence that makes
 * gptps_unload_addon impossible (a settings entry would be left pointing into an
 * unmapped library), so this is the honest meaning of "disabled" here: the knobs
 * remain present and readable, they simply stop constraining admission. */
static gptps_status gpuq_disable(gptps *e)
{
    return g_api->define_resource(e, GPUQ_RESOURCE, GPUQ_UNLIMITED);
}

GPTPS_ADDON_INIT_NS("gpu quota (plugin)", GPUQ_NS, GPTPS_SEAM_CONSTRAINT,
                    gpuq_setup, 0, gpuq_disable)
