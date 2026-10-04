/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * addon_waiter.c - an add-on whose setup waits for the host, for test_config_strict.
 *
 * Its setup registers an observer and a constraint, takes the scheduler seam if it is
 * free, and registers a task named waiter.started, so the host can see that the setup
 * has begun. Then it waits until the host has registered a task named host.done - or
 * one named host.fail, which makes the setup give up. Meanwhile the host can check
 * what happens while an add-on's setup runs on another thread: a task the host
 * registers takes its file values at once, a reload is refused, and a setup that fails
 * undoes what it registered and nothing the host did.
 *
 * So that the host can tell whether they are still there: the observer logs
 * "waiter's observer heard" for every event, and the constraint denies every item of
 * a task named probe.denied.
 */
#include "gptps.h"
#include <stddef.h>
#include <string.h>

static const gptps_api_routines *g_api;

static gptps_status job(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_OK; }
static void heard(const gptps_event *ev, void *ud)
{
    (void)ev; (void)ud;
    g_api->log(NULL, GPTPS_LOG_WARN, "waiter's observer heard an event");
}
static gptps_admit_decision deny_probe(const gptps_constraint_input *in, uint32_t *retry_after_ms, void *ud)
{
    (void)retry_after_ms; (void)ud;
    return strcmp(in->task_name, "probe.denied") == 0 ? GPTPS_DENY : GPTPS_ADMIT;
}
static int64_t score(const gptps_sched_input *in, void *ud) { (void)ud; return (int64_t)in->priority; }

static gptps_status setup(gptps *e, const gptps_api_routines *api, char **err)
{
    gptps_task_def d;
    gptps_status st;
    uint64_t t0;
    (void)err;
    g_api = api;
    if ((st = api->register_observer(e, heard, NULL)) != GPTPS_OK) return st;
    if ((st = api->register_constraint(e, deny_probe, NULL)) != GPTPS_OK) return st;
    if (api->struct_size > offsetof(gptps_api_routines, set_scheduler_ex) && api->set_scheduler_ex)
        (void)api->set_scheduler_ex(e, score, NULL, "waiter", 0);   /* GPTPS_E_BUSY if the host holds it */
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "waiter.started"; d.run = job; d.exec = GPTPS_EXEC_INPROC;
    if ((st = api->register_task(e, &d)) != GPTPS_OK) return st;
    t0 = api->now_ms(NULL);
    while (!api->task_exists(e, "host.done")) {
        if (api->task_exists(e, "host.fail")) return GPTPS_E_TASK;      /* the host's cue */
        if (api->now_ms(NULL) - t0 > 10000) return GPTPS_E_TIMEOUT;
    }
    return GPTPS_OK;
}

GPTPS_ADDON_INIT("waiter", GPTPS_SEAM_TASK, setup, 0)
