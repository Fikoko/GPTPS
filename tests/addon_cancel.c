/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * addon_cancel.c - a dlopen'd add-on whose task actually LOOPS.
 *
 * tests/addon_demo.c returns GPTPS_OK immediately, which is the one task shape
 * that never has to ask whether it should stop. That is why nobody noticed, until
 * ABI 2.1, that the host table had no is_cancelled: a binary plugin could not poll
 * for cancellation, so it could not honour a timeout, gptps_cancel,
 * GPTPS_REMOVE_CANCEL or limits.shutdown_grace_ms - it structurally could not meet
 * the liveness guarantees the core makes contractual.
 *
 * This add-on is the regression guard. Its task runs until it is told to stop, and
 * the ONLY way it can be told is api->is_cancelled - reached through the table,
 * never by linking a core symbol (it cannot: in the default static build the core
 * lives in the host executable behind the gptps_/gptps__ namespacing).
 *
 * If the table ever loses that routine, or a plugin built against an older core
 * calls past its floor, this add-on hangs and tests/test_addon.c fails on its CTest
 * timeout instead of silently passing.
 *
 * Registers two task types:
 *   "spinner"  - loops until cancelled, then returns GPTPS_E_CANCELLED
 *   "deadline" - reports whether the ctx accessors agree with each other, so the
 *                rest of tranche A is exercised rather than merely present
 */
#include "gptps.h"
#include <string.h>
#include <stddef.h>

static const gptps_api_routines *g_api;

/* Loop until the core says stop. A cooperative task must poll; this one has no
 * other way to end, which is exactly the property under test. */
static gptps_status spinner(gptps_ctx *ctx, void *ud)
{
    (void)ud;
    for (;;) {
        if (g_api->is_cancelled(ctx)) return GPTPS_E_CANCELLED;
    }
}

/* Deliver a one-byte verdict on the other tranche-A accessors:
 *   '1' = deadline_ms/now_ms are self-consistent (both reachable, and a deadline,
 *         when set, is in the future relative to now at entry)
 *   '0' = they are not
 * Returned as the task RESULT, so the host asserts on a value the plugin computed
 * on the far side of the ABI rather than on the plugin merely not crashing. */
static gptps_status deadline_probe(gptps_ctx *ctx, void *ud)
{
    uint64_t now, dl;
    char verdict;
    (void)ud;
    now = g_api->now_ms(ctx);
    dl  = g_api->deadline_ms(ctx);
    verdict = (now > 0 && (dl == 0 || dl >= now)) ? '1' : '0';
    return g_api->result_set(ctx, &verdict, 1);
}

static gptps_status reg(gptps *e, const char *name, gptps_run_fn fn, unsigned timeout_s)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.name = name;
    d.run = fn;
    d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_cost.mem_bytes = 512;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.timeout_seconds = timeout_s;
    return g_api->register_task(e, &d);
}

static gptps_status cancel_setup(gptps *e, const gptps_api_routines *api, char **err)
{
    gptps_status st;
    (void)err;
    g_api = api;

    /* Guard the 2.1 tranche exactly as an out-of-tree plugin must: check the table
     * is long enough BEFORE calling into it. Against a 2.0 core this add-on refuses
     * to load rather than registering a task it cannot ever stop - which is the
     * correct, and the safe, failure. */
    if (api->struct_size <= offsetof(gptps_api_routines, is_cancelled) || !api->is_cancelled)
        return GPTPS_E_ABI;

    st = reg(e, "spinner", spinner, 0);
    if (st != GPTPS_OK) return st;
    /* a 60s timeout it will never reach - present only so deadline_ms is non-zero */
    return reg(e, "deadline", deadline_probe, 60);
}

GPTPS_ADDON_INIT("cancel", GPTPS_SEAM_TASK, cancel_setup, 0)
