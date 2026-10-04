/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * addon_submit.c - an add-on that starts work as it loads, for test_config_strict.
 *
 * It registers a task and submits it from its setup, inside gptps_open, and waits
 * there until the task has started - so the dispatcher has made a pass while the
 * open is still reading the file. The first-submit report of unused config keys
 * must not be made then: no key is claimed yet, and it would call every one of
 * them unused. And it
 * defines a per-task setting whose last part is a built-in's name - "sub.priority",
 * a choice where the built-in "priority" is a number - so the loader must judge
 * tasks.<name>.sub.priority by its definition, not by its last part. And it
 * defines a global, sub.level, and only then registers a watcher, which logs what
 * it hears: its own keys must take the file's values after its setup returns, or
 * that watcher would never hear them.
 *
 * And a per-task sub.remove: hearing tasks.<name>.sub.remove = true, the watcher
 * tries to remove <name> and logs what came back. A file value reaches it on the
 * thread registering <name>, while that registration still writes into the type - so
 * the removal must be refused, not free the type under it.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

static int g_started;
static const gptps_api_routines *g_api;

static void heard(const char *key, const char *value, void *ud)
{
    char msg[300];
    gptps *e = (gptps *)ud;
    size_t n = strlen(key), tl = sizeof ".sub.remove" - 1;
    snprintf(msg, sizeof msg, "sub heard %s = %s", key, value);
    g_api->log((gptps_ctx *)NULL, GPTPS_LOG_WARN, msg);
    if (strncmp(key, "tasks.", 6) == 0 && n > 6 + tl && strcmp(key + n - tl, ".sub.remove") == 0 &&
        strcmp(value, "true") == 0) {
        char task[128];
        gptps_status st;
        snprintf(task, sizeof task, "%.*s", (int)(n - 6 - tl), key + 6);
        st = g_api->unregister_task(e, task, GPTPS_REMOVE_CANCEL);
        snprintf(msg, sizeof msg, "sub removing %s: %d, exists %d", task, (int)st, g_api->task_exists(e, task));
        g_api->log((gptps_ctx *)NULL, GPTPS_LOG_WARN, msg);
    }
}

static gptps_status job(gptps_ctx *ctx, void *ud)
{
    (void)ctx; (void)ud;
    __atomic_store_n(&g_started, 1, __ATOMIC_SEQ_CST);
    return GPTPS_OK;
}

static gptps_status setup(gptps *e, const gptps_api_routines *api, char **err)
{
    gptps_task_def d;
    gptps_handle h;
    gptps_status st;
    (void)err;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "sub.warmup"; d.run = job; d.exec = GPTPS_EXEC_INPROC;
    if ((st = api->register_task(e, &d)) != GPTPS_OK) return st;
    g_api = api;
    if ((st = api->define_task_setting(e, "sub.priority", GPTPS_SETTING_ENUM, "low", "low|high", 0)) != GPTPS_OK)
        return st;
    if ((st = api->define_task_setting(e, "sub.remove", GPTPS_SETTING_BOOL, "false", 0, 0)) != GPTPS_OK)
        return st;
    if ((st = api->define_global(e, "sub.level", GPTPS_SETTING_UINT, "1", 0, 0)) != GPTPS_OK) return st;
    if ((st = api->settings_watch(e, heard, e)) != GPTPS_OK) return st;      /* last, on purpose */
    __atomic_store_n(&g_started, 0, __ATOMIC_SEQ_CST);
    if ((st = api->submit(e, "sub.warmup", NULL, 0, &h)) != GPTPS_OK) return st;
    {   /* a threaded engine runs it now; a manual one only when stepped, so give up soon */
        uint64_t t0 = api->now_ms(NULL);
        while (!__atomic_load_n(&g_started, __ATOMIC_SEQ_CST) && api->now_ms(NULL) - t0 < 2000) { }
    }
    return GPTPS_OK;
}

GPTPS_ADDON_INIT_NS("submitter", "sub", GPTPS_SEAM_TASK, setup, 0, 0)
