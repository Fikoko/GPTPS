/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * addon_greedy_run.c - a plug-in whose setup() is impeccable and whose TASK BODY
 * reads past the table.
 *
 * This is the likelier real-world mistake, and the one the conformance harness used
 * to miss entirely: it only ever called setup(), so a plug-in that guarded every
 * setup-time call and then used api->is_cancelled unguarded in run() was certified
 * CONFORMANT. The harness's OWN reference template has is_cancelled in run(), so the
 * tool could not check the shape it tells authors to write.
 *
 * is_cancelled is an ABI 2.1 routine, so against any older host's table it lies past
 * the end - and a looping task is exactly where you need it.
 */
#include "gptps.h"
#include <string.h>
#include <stddef.h>

static const gptps_api_routines *g_api;

static gptps_status greedy_run(gptps_ctx *ctx, void *ud)
{
    (void)ud;
    /* THE BUG, on purpose: no api->struct_size guard before the call. Against a
     * current host this is fine, which is what makes it easy to ship. */
    if (g_api->is_cancelled(ctx)) return GPTPS_E_CANCELLED;
    return GPTPS_OK;
}

static gptps_status greedy_run_setup(gptps *e, const gptps_api_routines *api, char **err)
{
    gptps_task_def d;
    (void)err;
    g_api = api;
    /* setup() itself is scrupulous - it touches only v1.0 routines. */
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "greedyrun"; d.run = greedy_run;
    d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    return api->register_task(e, &d);
}

GPTPS_ADDON_INIT("greedy run", GPTPS_SEAM_TASK, greedy_run_setup, 0)
