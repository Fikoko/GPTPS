/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * addon_order.c - a binary plug-in with two settings checked against each other, for
 * test_settings_order (gptps.h, THE FILE'S ORDER): order.lo is refused above order.hi,
 * order.hi below order.lo. Its accessors ask the host table's settings_provisional
 * (ABI 2.10) and count a refusal only when the try is not provisional; its watcher
 * (settings_watch: it hears a config file's values) counts what each key applied.
 * order.stats reads the counts back: "calls_lo calls_hi refusals heard_lo heard_hi
 * provisional".
 * order.n: refused at 77 (counted unless provisional); its watcher, hearing n = 50,
 * live-sets n = 77 through the host table - a live set made from a watcher of a
 * provisional try that applied, which must count its refusal (the provisional state
 * covers the accessor's call only). order.leak reads "n_refusals nested_status
 * nested_provisional".
 */
#include "gptps.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const gptps_api_routines *g_api;
static gptps *g_e;
static unsigned long g_lo = 1, g_hi = 10;
static unsigned g_calls_lo, g_calls_hi, g_refusals, g_heard_lo, g_heard_hi, g_prov;
static unsigned long g_n;
static unsigned g_n_refusals, g_nested_prov;
static int g_nested_st = -1, g_in_nested;

static int prov(const char *key)
{
    int p = g_api->settings_provisional(g_e, key);
    if (p) ++g_prov;
    return p;
}
static size_t rd_lo(void *t, char *b, size_t c) { (void)t; return (size_t)snprintf(b, c, "%lu", g_lo); }
static size_t rd_hi(void *t, char *b, size_t c) { (void)t; return (size_t)snprintf(b, c, "%lu", g_hi); }
static gptps_status wr_lo(void *t, const char *v)
{
    unsigned long n = strtoul(v, NULL, 10);
    (void)t;
    ++g_calls_lo;
    if (n > g_hi) {
        if (!prov("order.lo")) {
            ++g_refusals;
            g_api->log((gptps_ctx *)NULL, GPTPS_LOG_WARN, "addon_order: order.lo refused");
        }
        return GPTPS_E_INVAL;
    }
    g_lo = n;
    return GPTPS_OK;
}
static gptps_status wr_hi(void *t, const char *v)
{
    unsigned long n = strtoul(v, NULL, 10);
    (void)t;
    ++g_calls_hi;
    if (n < g_lo) {
        if (!prov("order.hi")) {
            ++g_refusals;
            g_api->log((gptps_ctx *)NULL, GPTPS_LOG_WARN, "addon_order: order.hi refused");
        }
        return GPTPS_E_INVAL;
    }
    g_hi = n;
    return GPTPS_OK;
}
static size_t rd_stats(void *t, char *b, size_t c)
{
    (void)t;
    return (size_t)snprintf(b, c, "%u %u %u %u %u %u", g_calls_lo, g_calls_hi, g_refusals, g_heard_lo, g_heard_hi, g_prov);
}
static gptps_status wr_stats(void *t, const char *v) { (void)t; (void)v; return GPTPS_E_INVAL; }
static size_t rd_n(void *t, char *b, size_t c) { (void)t; return (size_t)snprintf(b, c, "%lu", g_n); }
static gptps_status wr_n(void *t, const char *v)
{
    unsigned long n = strtoul(v, NULL, 10);
    (void)t;
    if (n == 77) {
        int p = g_api->settings_provisional(g_e, "order.n");
        if (g_in_nested && p) ++g_nested_prov;
        if (!p) ++g_n_refusals;
        return GPTPS_E_INVAL;
    }
    g_n = n;
    return GPTPS_OK;
}
static size_t rd_leak(void *t, char *b, size_t c)
{
    (void)t;
    return (size_t)snprintf(b, c, "%u %d %u", g_n_refusals, g_nested_st, g_nested_prov);
}

static void heard(const char *key, const char *value, void *ud)
{
    (void)ud;
    if (!strcmp(key, "order.lo")) ++g_heard_lo;
    else if (!strcmp(key, "order.hi")) ++g_heard_hi;
    else if (!strcmp(key, "order.n") && !strcmp(value, "50") && !g_in_nested) {
        g_in_nested = 1;
        g_nested_st = (int)g_api->settings_set(g_e, "order.n", "77");
        g_in_nested = 0;
    }
}

static gptps_status setup(gptps *e, const gptps_api_routines *api, char **err)
{
    static const struct { const char *key; gptps_setting_type ty; size_t (*rd)(void *, char *, size_t);
                          gptps_status (*wr)(void *, const char *); } S[] = {
        { "order.lo", GPTPS_SETTING_UINT, rd_lo, wr_lo },
        { "order.hi", GPTPS_SETTING_UINT, rd_hi, wr_hi },
        { "order.stats", GPTPS_SETTING_STRING, rd_stats, wr_stats },
        { "order.n", GPTPS_SETTING_UINT, rd_n, wr_n },
        { "order.leak", GPTPS_SETTING_STRING, rd_leak, wr_stats },
    };
    size_t i;
    (void)err;
    if (api->struct_size <= offsetof(gptps_api_routines, settings_hold) || !api->settings_provisional ||
        !api->settings_hold || !api->settings_watch || !api->register_setting)
        return GPTPS_E_ABI;
    /* the host table's settings_hold is gptps_settings_hold: it nests on this thread,
     * and a let-go it lacks is refused (a failure here fails the load, and the test) */
    if (api->settings_hold(e, 1) != GPTPS_OK || api->settings_hold(e, 1) != GPTPS_OK ||
        api->settings_hold(e, 0) != GPTPS_OK || api->settings_hold(e, 0) != GPTPS_OK ||
        api->settings_hold(e, 0) != GPTPS_E_INVAL)
        return GPTPS_E_INVAL;
    g_api = api; g_e = e;
    g_lo = 1; g_hi = 10;          /* a library a load left mapped keeps its statics: start over */
    g_calls_lo = g_calls_hi = g_refusals = g_heard_lo = g_heard_hi = g_prov = 0;
    g_n = 0; g_n_refusals = g_nested_prov = 0; g_nested_st = -1; g_in_nested = 0;
    if (api->settings_watch(e, heard, NULL) != GPTPS_OK) return GPTPS_E_NOMEM;
    for (i = 0; i < sizeof S / sizeof S[0]; ++i) {
        gptps_setting_def d;
        gptps_status st;
        memset(&d, 0, sizeof d);
        d.struct_size = sizeof d; d.key = S[i].key; d.type = S[i].ty; d.desc = "addon_order";
        d.read = S[i].rd; d.write = S[i].wr; d.hot = 1;
        if ((st = api->register_setting(e, &d)) != GPTPS_OK) return st;
    }
    return GPTPS_OK;
}

GPTPS_ADDON_INIT("order", GPTPS_SEAM_TASK, setup, 0)
