/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * addon_ns_bad.c - a namespaced add-on that violates its OWN namespace.
 *
 * Declares "nsbad" and then tries to register a task called "unprefixed". The core
 * must refuse that registration, so setup() fails, so the load fails - and, because
 * the add-on had already registered one legitimate task by then, the unwind must
 * leave the engine with neither.
 *
 * That last part is the point. A namespace that only rejected the bad name while
 * leaving the good one behind would give a half-installed add-on, which is worse
 * than either outcome.
 */
#include "gptps.h"
#include <string.h>
#include <stddef.h>

static gptps_status work(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_OK; }
static int64_t      score(const gptps_sched_input *in, void *ud) { (void)ud; return (int64_t)in->priority; }

static gptps_status bad_setup(gptps *e, const gptps_api_routines *api, char **err)
{
    gptps_task_def d;
    gptps_status st;
    (void)err;

    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.run = work;
    d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;

    d.name = "nsbad.fine";                    /* correctly prefixed: accepted */
    st = api->register_task(e, &d);
    if (st != GPTPS_OK) return st;

    /* Take the scheduler seam BEFORE failing. Badly behaved on purpose - an add-on
     * should pass flags == 0 and accept E_BUSY - but that is exactly the point: the
     * unwind has to put the seam AND its owner label back however it was taken. A
     * fixture that only registers tasks never exercises that path at all. */
    if (api->struct_size > offsetof(gptps_api_routines, set_scheduler_ex) && api->set_scheduler_ex)
        api->set_scheduler_ex(e, score, 0, "nsbad", GPTPS_SCHED_REPLACE);

    d.name = "unprefixed";                    /* violates our own namespace */
    st = api->register_task(e, &d);
    if (st != GPTPS_OK) return st;            /* expected: GPTPS_E_INVAL -> load fails */

    return GPTPS_OK;                          /* unreachable if enforcement works */
}

GPTPS_ADDON_INIT_NS("ns bad", "nsbad", GPTPS_SEAM_TASK, bad_setup, 0, 0)
