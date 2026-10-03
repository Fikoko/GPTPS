/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * addon_waiter.c - an add-on whose setup waits for the host, for test_config_strict.
 *
 * Its setup registers a task named waiter.started, so the host can see that the
 * setup has begun, then waits until the host has registered a task named host.done.
 * Meanwhile the host can check what happens while an add-on's setup runs on another
 * thread: a task the host registers takes its file values at once, and a reload is
 * refused.
 */
#include "gptps.h"
#include <string.h>

static gptps_status job(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_OK; }

static gptps_status setup(gptps *e, const gptps_api_routines *api, char **err)
{
    gptps_task_def d;
    gptps_status st;
    uint64_t t0;
    (void)err;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "waiter.started"; d.run = job; d.exec = GPTPS_EXEC_INPROC;
    if ((st = api->register_task(e, &d)) != GPTPS_OK) return st;
    t0 = api->now_ms(NULL);
    while (!api->task_exists(e, "host.done"))
        if (api->now_ms(NULL) - t0 > 10000) return GPTPS_E_TIMEOUT;
    return GPTPS_OK;
}

GPTPS_ADDON_INIT("waiter", GPTPS_SEAM_TASK, setup, 0)
