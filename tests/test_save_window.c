/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_save_window.c - a reload cannot swap the loaded file out from under a save.
 *
 * A save to a new path writes a copy of the config file the engine loaded, and a value
 * in it the engine refused gets the setting's live value instead: the copy has to open
 * again. gptps_settings_save names the loaded file under the engine lock, releases it,
 * and only then copies the file, asking for each value whether the engine refused it -
 * of whatever file was loaded by then. A reload that swapped in another file in that
 * window had the other file answer, so a refused value went into the copy as it was.
 * A loop of saves against a thread reloading two files met it twice in about 47,000.
 *
 * The window is between two locks, with no call between them a test could catch. So
 * the linker's --wrap puts a wrapper in front of gptps_mutex_lock (the static library's
 * calls; Linux, like exec_faults), and at the main thread's second lock inside the
 * save - the settings lock, taken after the engine lock is released - it runs a reload
 * of another file on a second thread and waits for it. A reload must now be refused
 * with GPTPS_E_BUSY while a save runs, and the copy must carry the live value.
 * Without that, 20 of 20 copies kept the refused value.
 */
#include "gptps.h"
#include "gptps_hal.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

#define GOOD "save_window_good.toml"
#define BAD  "save_window_bad.toml"
#define OUT  "save_window_out.toml"

void __real_gptps_mutex_lock(gptps_mutex *m);

static gptps   *g_e;
static uint64_t g_main;              /* the thread that saves */
static int      g_armed;             /* counts the main thread's locks down to the window */
static int      g_hits;              /* reloads run in the window */
static gptps_status g_reload = GPTPS_OK;

static void *reload_good(void *arg)
{
    (void)arg;
    g_reload = gptps_settings_reload(g_e, GOOD);
    return NULL;
}

void __wrap_gptps_mutex_lock(gptps_mutex *m)
{
    if (g_armed && gptps_hal_thread_id() == g_main && --g_armed == 0) {
        gptps_thread *t = gptps_thread_start(reload_good, NULL);
        CHECK(t != NULL);
        if (t) { gptps_thread_join(t); ++g_hits; }
    }
    __real_gptps_mutex_lock(m);
}

static void quiet(gptps_log_level lvl, const char *msg, void *ud) { (void)lvl; (void)msg; (void)ud; }

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static void slurp(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(buf, 1, cap - 1, f) : 0;
    buf[n] = 0;
    if (f) fclose(f);
}

int main(void)
{
    gptps_config c;
    gptps_status st;
    char out[512];
    int i;

    gptps_set_log_sink(quiet, NULL);
    g_main = gptps_hal_thread_id();
    put(GOOD, "[host]\nknob = 7\n");
    put(BAD,  "[host]\nknob = 999\n");             /* out of range: refused */
    memset(&c, 0, sizeof c);
    c.struct_size = sizeof c;
    c.limits.struct_size = sizeof c.limits;
    c.mode = GPTPS_RUN_MANUAL;
    c.config_path = GOOD;
    CHECK(gptps_open_ex(&c, &g_e) == GPTPS_OK);
    if (!g_e) return 1;
    CHECK(gptps_define_global(g_e, "host.knob", GPTPS_SETTING_UINT, "5", "0..100", 0) == GPTPS_OK);

    for (i = 0; i < 20; ++i) {
        CHECK(gptps_settings_reload(g_e, BAD) == GPTPS_E_CONFIG);   /* loaded: BAD, knob refused */
        remove(OUT);
        g_reload = GPTPS_OK;
        g_armed = 2;            /* 1: the engine lock, naming the file; 2: the settings lock */
        st = gptps_settings_save(g_e, OUT);                        /* a new path: a copy of BAD */
        g_armed = 0;
        CHECK(st == GPTPS_OK);
        CHECK(g_reload == GPTPS_E_BUSY);         /* the save holds the loaded file */
        slurp(OUT, out, sizeof out);
        CHECK(strstr(out, "knob = 7") != NULL);  /* the live value, where BAD said 999 */
        CHECK(strstr(out, "999") == NULL);
        if (strstr(out, "999")) printf("  the copy: %s", out);
    }
    CHECK(g_hits == 20);                         /* the wrapper found the window every time */
    CHECK(gptps_settings_reload(g_e, GOOD) == GPTPS_OK);   /* and a reload after the save works */

    gptps_shutdown(g_e);
    remove(GOOD); remove(BAD); remove(OUT);
    if (fails) { printf("%d save_window check(s) FAILED\n", fails); return 1; }
    printf("all save_window checks passed\n");
    return 0;
}
