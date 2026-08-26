/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * addon_unwind.c - an add-on that registers MANY task types and then fails.
 *
 * gptps_load_addon must leave no trace of a setup() that gave up: the host is told
 * the load failed, so a task type still sitting in the registry - with run/cost
 * pointers into a module the host believes did not load - is a live footgun.
 *
 * The unwind used to buffer at most GPTPS_ADDON_UNWIND_MAX (16) task NAMES into a
 * fixed array and unregister those, so an add-on that registered more than 16 before
 * failing kept the surplus. This one registers 20 for exactly that reason: 16 is not
 * a number a fixture should be shy of. It is also why the count matters more than
 * the failure - any single-task failing add-on would have passed the old code.
 *
 * Registering succeeds all the way through and THEN returns an error, which is the
 * realistic shape: a setup that validates its configuration last, or that fails
 * allocating its own state after wiring up its task types.
 */
#include "gptps.h"
#include <string.h>
#include <stdio.h>

/* More than the old 16-entry unwind buffer, and enough that an off-by-one in a
 * future cap is visible rather than marginal. */
#define UNWIND_TASKS 20

static const gptps_api_routines *g_api;

static gptps_status noop(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_OK; }

static gptps_status unwind_setup(gptps *e, const gptps_api_routines *api, char **err)
{
    int i;
    (void)err;
    g_api = api;

    for (i = 0; i < UNWIND_TASKS; ++i) {
        gptps_task_def d;
        char name[16];
        snprintf(name, sizeof name, "uw%d", i);
        memset(&d, 0, sizeof d);
        d.struct_size = sizeof d;
        d.name = name;                 /* register_task deep-copies the name */
        d.run = noop;
        d.exec = GPTPS_EXEC_INPROC;
        d.default_cost.struct_size = sizeof d.default_cost;
        d.default_policy.struct_size = sizeof d.default_policy;
        if (api->register_task(e, &d) != GPTPS_OK) return GPTPS_E_TASK;
    }
    /* Everything registered; NOW give up. The core must undo all of it. */
    return GPTPS_E_TASK;
}

GPTPS_ADDON_INIT("unwind", GPTPS_SEAM_TASK, unwind_setup, 0)
