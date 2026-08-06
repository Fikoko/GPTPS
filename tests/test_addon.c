/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_addon.c - add-on loader + ABI regression (T7). Loads a pre-compiled
 * plugin shared library via the host-table ABI, runs its task, and checks the
 * magic/version gate rejects a bad add-on and a missing file.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

#ifndef ADDON_DEMO_PATH
#define ADDON_DEMO_PATH "./addon_demo.so"
#endif
#ifndef ADDON_BAD_PATH
#define ADDON_BAD_PATH  "./addon_bad.so"
#endif

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int c_finished = 0;
static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }

static void on_ev(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_FINISHED) inc(&c_finished);
}

int main(void)
{
    gptps *e = NULL;
    gptps_handle h;

    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    if (!e) { printf("open failed\n"); return 1; }
    gptps_set_event_cb(e, on_ev, NULL);

    /* missing file -> I/O error */
    CHECK(gptps_load_addon(e, "/no/such/path/addon.so") == GPTPS_E_IO);
    /* wrong magic -> ABI rejection */
    CHECK(gptps_load_addon(e, ADDON_BAD_PATH) == GPTPS_E_ABI);
    /* valid add-on loads via the host-table ABI */
    CHECK(gptps_load_addon(e, ADDON_DEMO_PATH) == GPTPS_OK);

    /* the add-on registered a setting through the host-table register_setting routine */
    {
        char b[GPTPS_SETTINGS_VALUE_MAX];
        CHECK(gptps_settings_get(e, "demo.flag", b, sizeof b) == GPTPS_OK);
        CHECK(strcmp(b, "demo") == 0);
    }

    /* the add-on-registered task runs end-to-end */
    CHECK(gptps_submit(e, "plugintask", NULL, 0, &h) == GPTPS_OK);
    gptps_shutdown(e);
    CHECK(get(&c_finished) == 1);

    if (fails) { printf("%d addon check(s) FAILED\n", fails); return 1; }
    printf("all addon checks passed\n");
    return 0;
}
