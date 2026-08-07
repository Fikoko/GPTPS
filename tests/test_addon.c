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
#ifndef ADDON_CANCEL_PATH
#define ADDON_CANCEL_PATH "./addon_cancel.so"
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

/* For the ABI-2.1 engine below. c_deadline_ok records a verdict the PLUGIN
 * computed on the far side of the host table, so the assertion is about the
 * routines actually working, not merely about the plugin surviving the call. */
static int c_deadline_ok = 0, c_cancelled = 0;
static void on_ev2(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_FINISHED && strcmp(ev->task_name, "deadline") == 0 &&
        ev->result && ev->result_len == 1 && ((const char *)ev->result)[0] == '1')
        inc(&c_deadline_ok);
    if (ev->kind == GPTPS_EV_FAILED && ev->status == GPTPS_E_CANCELLED &&
        strcmp(ev->task_name, "spinner") == 0)
        inc(&c_cancelled);
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

    /* ===== ABI 2.1: cancellation must cross the host table =====
     *
     * Everything above this line is satisfied by a task that returns immediately,
     * which is why the missing is_cancelled routine went unnoticed for so long. A
     * plugin whose task LOOPS can only be stopped through the table - so if that
     * routine is ever dropped, or a plugin reads past its declared floor, these
     * cases hang and the CTest timeout reports it instead of a silent pass. */
    {
        gptps *e2 = NULL;
        gptps_handle hs = 0;
        gptps_config cfg;
        uint64_t t_shutdown;

        memset(&cfg, 0, sizeof cfg);
        cfg.struct_size = sizeof cfg;
        cfg.limits.struct_size = sizeof cfg.limits;
        cfg.limits.max_concurrent_tasks = 4;
        CHECK(gptps_open_ex(&cfg, &e2) == GPTPS_OK);
        if (!e2) { printf("open failed\n"); return 1; }
        /* Shorten the shutdown drain bound. The default is 30000 ms, so a spinner
         * still running at shutdown holds the drain for 30 seconds - which is
         * exactly CTest's timeout for this test, making the result a coin flip on
         * whether the second spinner had been admitted yet. It is a live setting
         * rather than a gptps_limits field, so it is set here.
         *
         * This does not weaken what is being tested. The engine still waits out the
         * grace and then raises the cancel flag, and the ONLY thing that can end a
         * cooperative in-process task is the plugin OBSERVING that flag through the
         * host table. A short grace just makes the proof fast and deterministic. */
        CHECK(gptps_settings_set(e2, "limits.shutdown_grace_ms", "500") == GPTPS_OK);
        gptps_set_event_cb(e2, on_ev2, NULL);
        CHECK(gptps_load_addon(e2, ADDON_CANCEL_PATH) == GPTPS_OK);
        CHECK(gptps_task_exists(e2, "spinner") == 1);

        /* 1) an explicit gptps_cancel reaches a looping plugin task */
        CHECK(gptps_submit(e2, "spinner", NULL, 0, &hs) == GPTPS_OK);
        CHECK(gptps_cancel(e2, hs) == GPTPS_OK);

        /* 2) the other tranche-A accessors agree, as judged BY THE PLUGIN */
        {
            gptps_handle hd = 0;
            CHECK(gptps_submit(e2, "deadline", NULL, 0, &hd) == GPTPS_OK);
        }

        /* 3) shutdown must RETURN. It stops a still-looping plugin task via the
         *    same flag - this is the liveness guarantee tests/test_hang.c enforces
         *    for in-tree tasks, now enforced across the ABI boundary too. If
         *    is_cancelled ever stops reaching the plugin, this hangs and CTest's
         *    timeout reports it rather than the suite passing on a lie. */
        CHECK(gptps_submit(e2, "spinner", NULL, 0, &hs) == GPTPS_OK);
        t_shutdown = gptps_now_ms(NULL);
        gptps_shutdown(e2);
        /* Bounded, not merely finite. grace is 500ms, so anything near the 30s
         * default would mean the flag never reached the plugin and the engine fell
         * back on its own hard bound. */
        CHECK(gptps_now_ms(NULL) - t_shutdown < 10000);

        CHECK(get(&c_deadline_ok) == 1);  /* now_ms/deadline_ms worked, per the plugin */
        CHECK(get(&c_cancelled)  >= 1);   /* at least the explicit cancel landed */
    }

    if (fails) { printf("%d addon check(s) FAILED\n", fails); return 1; }
    printf("all addon checks passed\n");
    return 0;
}
