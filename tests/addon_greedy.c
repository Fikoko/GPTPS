/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * addon_greedy.c - a plug-in that reads PAST the table it was given.
 *
 * Identical to tests/addon_demo.c except for one missing line: it calls
 * api->register_setting (a v1.4 routine) without first checking api->struct_size.
 * Against a current core that works fine, which is exactly why the mistake is easy
 * to make and hard to notice - it only breaks when someone drops the .so into an
 * older host, where that slot is past the end of a shorter table.
 *
 * It exists so tools/gptps_conformance has something it MUST reject. A conformance
 * harness that cannot fail certifies nothing.
 */
#include "gptps.h"
#include <string.h>
#include <stdio.h>

static gptps_status greedy(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_OK; }
static size_t       g_rd(void *t, char *b, size_t c) { (void)t; return (size_t)snprintf(b, c, "%s", "greedy"); }
static gptps_status g_wr(void *t, const char *v) { (void)t; (void)v; return GPTPS_OK; }

static gptps_status greedy_setup(gptps *e, const gptps_api_routines *api, char **err)
{
    gptps_task_def d;
    gptps_setting_def s;
    gptps_status st;
    (void)err;

    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "greedytask"; d.run = greedy; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    st = api->register_task(e, &d);
    if (st != GPTPS_OK) return st;

    /* THE BUG, on purpose: no `api->struct_size > offsetof(...)` guard. */
    memset(&s, 0, sizeof s);
    s.struct_size = sizeof s; s.key = "greedy.flag"; s.type = GPTPS_SETTING_STRING;
    s.desc = "registered without checking the table size"; s.read = g_rd; s.write = g_wr;
    api->register_setting(e, &s);
    return GPTPS_OK;
}

GPTPS_ADDON_INIT("greedy", GPTPS_SEAM_TASK, greedy_setup, 0)
