/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * addon_demo.c - a valid GPTPS add-on built as a shared library, compiled
 * against the frozen header. It registers a task via the host-table ABI
 * (never linking core symbols directly). Loading + running it is the ABI
 * regression test: it proves the frozen wtps.h/gptps.h ABI still works.
 */
#include "gptps.h"
#include <string.h>
#include <stdio.h>
#include <stddef.h>

static gptps_status plugintask(gptps_ctx *ctx, void *ud)
{
    (void)ctx; (void)ud;
    return GPTPS_OK;
}

/* a host-table-registered setting (exercises the v1.4 register_setting routine) */
static size_t       demo_rd(void *t, char *b, size_t c) { (void)t; return (size_t)snprintf(b, c, "%s", "demo"); }
static gptps_status demo_wr(void *t, const char *v) { (void)t; (void)v; return GPTPS_OK; }

static gptps_status demo_setup(gptps *e, const gptps_api_routines *api, char **err)
{
    gptps_task_def d;
    gptps_status st;
    (void)err;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.name = "plugintask";
    d.run = plugintask;
    d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_cost.mem_bytes = 512;
    d.default_policy.struct_size = sizeof d.default_policy;
    st = api->register_task(e, &d); /* call the core ONLY through the table */
    if (st != GPTPS_OK) return st;

    /* register a setting through the host table, guarded by the table's size so
     * older cores (without register_setting) are handled gracefully */
    if (api->struct_size > offsetof(gptps_api_routines, register_setting) && api->register_setting) {
        gptps_setting_def s;
        memset(&s, 0, sizeof s);
        s.struct_size = sizeof s; s.key = "demo.flag"; s.type = GPTPS_SETTING_STRING;
        s.desc = "demo add-on setting (host-table registered)"; s.read = demo_rd; s.write = demo_wr;
        api->register_setting(e, &s);
    }
    return GPTPS_OK;
}

GPTPS_ADDON_INIT("demo", GPTPS_SEAM_TASK, demo_setup, 0)
