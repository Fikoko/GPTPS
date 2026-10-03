/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * addon_failwatch.c - an add-on whose setup fails after it registered a watcher and
 * a setting of its own, for test_config_strict. Its code stays mapped (a failed
 * add-on is never unloaded), so the engine must not call into it again: the setup
 * that would have made it ready never finished. The watcher logs each call, which
 * the test's log sink sees; and the setting's target is freed before the setup
 * fails, as a setup that cleans up after itself would - so a write to it after
 * the failure is a use-after-free, which AddressSanitizer reports. (Freeing a
 * registered setting's target is safe here only because nothing else runs while
 * the test loads it: it is a trap for the engine, not a pattern for an add-on.)
 */
#include "gptps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const gptps_api_routines *g_api;

static void heard(const char *key, const char *value, void *ud)
{
    char msg[300];
    snprintf(msg, sizeof msg, "failwatch heard %s = %s", key, value);
    g_api->log((gptps_ctx *)NULL, GPTPS_LOG_WARN, msg);
    (void)ud;
}

static size_t own_read(void *t, char *b, size_t c) { return (size_t)snprintf(b, c, "%d", *(int *)t); }
static gptps_status own_write(void *t, const char *v) { *(int *)t = atoi(v); return GPTPS_OK; }

static gptps_status setup(gptps *e, const gptps_api_routines *api, char **err)
{
    gptps_setting_def d;
    gptps_status st;
    int *own;
    (void)err;
    g_api = api;
    if ((st = api->define_global(e, "fw.level", GPTPS_SETTING_UINT, "1", 0, 0)) != GPTPS_OK) return st;
    if ((st = api->settings_watch(e, heard, NULL)) != GPTPS_OK) return st;
    if (!(own = (int *)malloc(sizeof *own))) return GPTPS_E_NOMEM;
    *own = 0;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.key = "fw.own"; d.type = GPTPS_SETTING_UINT; d.desc = "the add-on's own";
    d.hot = 1; d.target = own; d.read = own_read; d.write = own_write;
    if ((st = api->register_setting(e, &d)) != GPTPS_OK) { free(own); return st; }
    free(own);                              /* cleaning up after itself... */
    return GPTPS_E_INVAL;                   /* ...and then it fails */
}

/* Built twice: namespaced, and (FW_PLAIN) not - what a setup registers is undone
 * either way. */
#if defined(FW_PLAIN)
GPTPS_ADDON_INIT("failwatch plain", GPTPS_SEAM_TASK, setup, 0)
#else
GPTPS_ADDON_INIT_NS("failwatch", "fw", GPTPS_SEAM_TASK, setup, 0, 0)
#endif
