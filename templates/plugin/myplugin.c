/* SPDX-License-Identifier: MIT */
/*
 * myplugin.c - a complete GPTPS binary plug-in. Copy this directory and edit.
 *
 * A plug-in is loaded at runtime and calls the core ONLY through the versioned
 * table it is handed. It links no core symbols at all - which is what lets one .so
 * work against a host that linked GPTPS statically, dynamically, or as the
 * amalgamated two-file drop-in.
 *
 * Everything below that looks like ceremony is load-bearing. The comments say why.
 */
#include "gptps.h"
#include <string.h>
#include <stddef.h>
#include <stdio.h>

/* Your namespace token. Declaring one CLAIMS it: no other plug-in may take it, and
 * in exchange everything you register during setup() must be prefixed "<ns>.".
 * That is what stops two unrelated plug-ins colliding on a task name or a settings
 * key. Optional - pass 0 below to opt out - but strongly recommended out of tree. */
#define MYNS "myplugin"

/* The table, stashed by the entry-point macro. Never call a core function directly:
 * in a static or amalgamated host those symbols live in the executable behind the
 * core's own namespacing, precisely so an add-on cannot capture them. */
static const gptps_api_routines *g_api;

/* The work. Note the cancellation poll: an in-process task is COOPERATIVE, so if you
 * loop you must ask. Without it your task cannot honour a timeout, gptps_cancel, or
 * shutdown - and the engine's guarantee that gptps_shutdown always returns becomes
 * your bug. */
static gptps_status myrun(gptps_ctx *ctx, void *ud)
{
    size_t n = 0;
    const void *in = g_api->payload(ctx, &n);
    (void)ud; (void)in;

    if (g_api->is_cancelled(ctx)) return GPTPS_E_CANCELLED;

    return g_api->result_set(ctx, "ok", 2);
}

static gptps_status mysetup(gptps *e, const gptps_api_routines *api, char **err_out)
{
    gptps_task_def d;
    gptps_status st;
    (void)err_out;
    g_api = api;

    /* GUARD BEFORE YOU CALL. The table only ever grows, and `struct_size` tells you
     * how much of it this host actually has. Reading past that is undefined - so
     * check the LAST routine you intend to use. Refusing to load beats registering a
     * task you would not be able to cancel. */
    if (api->struct_size <= offsetof(gptps_api_routines, is_cancelled) ||
        !api->is_cancelled) {
        return GPTPS_E_ABI;   /* host is older than ABI 2.1 */
    }

    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;          /* ALWAYS: this is how the core reads you */
    d.name        = MYNS ".work";      /* prefixed, because we claimed a namespace */
    d.run         = myrun;
    d.exec        = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size   = sizeof d.default_cost;
    d.default_cost.mem_bytes     = 1024;
    d.default_policy.struct_size = sizeof d.default_policy;

    st = api->register_task(e, &d);
    if (st != GPTPS_OK) return st;

    /* A knob an operator can turn without recompiling anything. This is the whole
     * reason to be a plug-in rather than a compiled-in module. */
    return api->define_global(e, MYNS ".level", GPTPS_SETTING_UINT, "1", 0, 0);
}

/* Called at gptps_shutdown, after every worker thread is joined - so no task of
 * yours can still be running here. Free what you allocated; do not touch the engine. */
static void myteardown(gptps *e) { (void)e; }

/* Optional: stop participating without being unloaded. Your code stays mapped
 * (deliberately - there is no unload, because a settings entry would be left
 * pointing into an unmapped library), so this just hands back what you took. */
static gptps_status mydisable(gptps *e)
{
    return g_api->unregister_task(e, MYNS ".work", 0);
}

/* Exactly one exported symbol. That is the whole ABI surface of a plug-in. */
GPTPS_ADDON_INIT_NS("my plugin", MYNS, GPTPS_SEAM_TASK, mysetup, myteardown, mydisable)
