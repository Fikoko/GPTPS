/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * addon_emit.c - an add-on that calls back into its host from every place the
 * engine runs it on the host's own thread: an event through the host table from
 * setup(), teardown() and addon_emit_now() (which the host calls directly), and a
 * hook the host hands it (addon_emit_set_hook) from setup() and disable(), after
 * which each keeps using the engine. tests/test_addon.c has the event callback and
 * the hook call gptps_shutdown, which must be refused with GPTPS_E_BUSY instead of
 * freeing the engine under the call still using it. Each path is the only guard
 * of one bracket: addon_emit_now's emit is api_emit_event's alone, and the hook
 * calls are gptps_load_addon's and gptps_addon_disable's.
 */
#include "gptps.h"
#include <string.h>

static const gptps_api_routines *g_api;
static gptps *g_e;

static void emit(gptps *e, const char *name)
{
    gptps_event ev;
    memset(&ev, 0, sizeof ev);
    ev.struct_size = sizeof ev;
    ev.kind = GPTPS_EV_FINISHED;
    ev.task_name = name;
    ev.status = GPTPS_OK;
    g_api->emit_event(e, &ev);
}

static void (*g_hook)(gptps *);
GPTPS_ADDON_EXPORT void addon_emit_set_hook(void (*fn)(gptps *)) { g_hook = fn; }

static gptps_status emit_setup(gptps *e, const gptps_api_routines *api, char **err)
{
    (void)err;
    g_api = api;
    g_e = e;
    if (g_hook) g_hook(e);
    (void)api->task_exists(e, "emit.after");
    emit(e, "emit.setup");
    return GPTPS_OK;
}

static void emit_teardown(gptps *e) { emit(e, "emit.teardown"); g_e = NULL; }

GPTPS_ADDON_EXPORT void addon_emit_now(void) { if (g_e) emit(g_e, "emit.now"); }

static gptps_status emit_disable(gptps *e)
{
    if (g_hook) g_hook(e);
    (void)g_api->task_exists(e, "emit.after");
    return GPTPS_OK;
}

GPTPS_ADDON_INIT_NS("emit", 0, GPTPS_SEAM_TASK, emit_setup, emit_teardown, emit_disable)
