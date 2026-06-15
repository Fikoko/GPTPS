/*
 * addon_demo.c - a valid GPTPS add-on built as a shared library, compiled
 * against the frozen header. It registers a task via the host-table ABI
 * (never linking core symbols directly). Loading + running it is the ABI
 * regression test: it proves the frozen wtps.h/gptps.h ABI still works.
 */
#include "gptps.h"
#include <string.h>

static gptps_status plugintask(gptps_ctx *ctx, void *ud)
{
    (void)ctx; (void)ud;
    return GPTPS_OK;
}

static gptps_status demo_setup(gptps *e, const gptps_api_routines *api, char **err)
{
    gptps_task_def d;
    (void)err;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.name = "plugintask";
    d.run = plugintask;
    d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_cost.mem_bytes = 512;
    d.default_policy.struct_size = sizeof d.default_policy;
    return api->register_task(e, &d); /* call the core ONLY through the table */
}

GPTPS_ADDON_INIT("demo", GPTPS_SEAM_TASK, demo_setup, 0)
