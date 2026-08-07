/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * addon_ns.c - a well-behaved NAMESPACED add-on (ABI 2.1).
 *
 * Declares the namespace "nstest", registers everything under it, takes the
 * scheduler seam politely (flags == 0, failing on GPTPS_E_BUSY rather than
 * stealing), and offers a disable hook that gives it all back.
 *
 * This is the shape a third-party plug-in should copy: it cannot collide with
 * anyone else's task names or settings keys, it cannot silently displace another
 * add-on's scheduler, and an operator can turn it off without unloading it.
 */
#include "gptps.h"
#include <string.h>
#include <stddef.h>

static const gptps_api_routines *g_api;

static gptps_status work(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_OK; }

/* Deadline-ish ordering: newest first. Content does not matter here - what matters
 * is WHO holds the seam and that it can be handed back. */
static int64_t score(const gptps_sched_input *in, void *ud)
{ (void)ud; return (int64_t)in->priority; }

static gptps_status ns_setup(gptps *e, const gptps_api_routines *api, char **err)
{
    gptps_task_def d;
    gptps_status st;
    (void)err;
    g_api = api;

    if (api->struct_size <= offsetof(gptps_api_routines, set_scheduler_ex)) return GPTPS_E_ABI;

    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.name = "nstest.work";                 /* prefixed - required by our own ns */
    d.run = work;
    d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    st = api->register_task(e, &d);
    if (st != GPTPS_OK) return st;

    st = api->define_global(e, "nstest.level", GPTPS_SETTING_UINT, "3", 0, 0);
    if (st != GPTPS_OK) return st;

    /* A per-task setting leaf, prefixed. Materializes as tasks.<name>.nstest.units
     * and is read back by this add-on as "nstest.units". */
    st = api->define_task_setting(e, "nstest.units", GPTPS_SETTING_UINT, "1", 0, 0);
    if (st != GPTPS_OK) return st;

    st = api->define_resource(e, "nstest.slots", 8);
    if (st != GPTPS_OK) return st;

    /* Take the ordering seam POLITELY. flags == 0 means "only if free"; on
     * GPTPS_E_BUSY we fail setup so the loader unwinds us cleanly and the operator
     * sees a conflict, instead of our policy silently replacing someone else's. */
    return api->set_scheduler_ex(e, score, 0, "nstest", 0);
}

/* Stop participating. Nothing is unmapped - this hands back the seam and the task,
 * which is what "disable" means here. */
static gptps_status ns_disable(gptps *e)
{
    g_api->set_scheduler_ex(e, 0, 0, "nstest", 0);     /* release: allowed to the owner */
    g_api->unregister_task(e, "nstest.work", 0);
    return GPTPS_OK;
}

GPTPS_ADDON_INIT_NS("ns test", "nstest", GPTPS_SEAM_SCHEDULER, ns_setup, 0, ns_disable)
