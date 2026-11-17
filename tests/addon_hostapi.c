/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * addon_hostapi.c - a binary plug-in that uses the two host-table routines ABI 2.8
 * appended: task_flags (is a type a SERVICE?) and event_measure (a measurement by
 * name). It reports what it got through the log, for test_engine_fixes, which
 * registers a SERVICE type "hostapi_svc" and a one-shot "hostapi_one" before loading
 * it. Before 2.8 a plug-in could reach neither: it could not tell a service's
 * per-run terminal events from a one-shot's single one.
 */
#include "gptps.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static gptps_status setup(gptps *e, const gptps_api_routines *api, char **err)
{
    char msg[200];
    uint64_t fs = 0, fo = 0;
    gptps_status s1, s2;
    gptps_event ev;
    gptps_measure m[2];
    const gptps_measure *got;
    (void)err;
    if (api->struct_size <= offsetof(gptps_api_routines, event_measure) ||
        !api->task_flags || !api->event_measure) {
        api->log((gptps_ctx *)NULL, GPTPS_LOG_WARN, "hostapi: the table has no task_flags / event_measure");
        return GPTPS_E_ABI;
    }
    s1 = api->task_flags(e, "hostapi_svc", &fs);
    s2 = api->task_flags(e, "hostapi_one", &fo);
    memset(&ev, 0, sizeof ev);
    memset(m, 0, sizeof m);
    ev.struct_size = sizeof ev;
    m[0].name = "mem.peak";  m[0].value = 4096;
    m[1].name = "exit.code"; m[1].value = 7;
    ev.measures = m; ev.n_measures = 2;
    got = api->event_measure(&ev, "exit.code");
    snprintf(msg, sizeof msg, "hostapi: svc %d 0x%llx one %d 0x%llx measure %lld none %d",
             (int)s1, (unsigned long long)fs, (int)s2, (unsigned long long)fo,
             got ? (long long)got->value : -1LL, api->event_measure(&ev, "no.such") == NULL);
    api->log((gptps_ctx *)NULL, GPTPS_LOG_WARN, msg);
    return GPTPS_OK;
}

GPTPS_ADDON_INIT("hostapi", GPTPS_SEAM_TASK, setup, 0)
