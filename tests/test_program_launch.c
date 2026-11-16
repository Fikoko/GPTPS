/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_program_launch.c - how a GPTPS_EXEC_PROGRAM job is started and how it ended
 * (ABI 2.7), on every platform: POSIX fork + exec, and Windows CreateProcessW.
 *
 *  1) work_dir: the program starts there - a directory with a non-ASCII (UTF-8)
 *     name, which CreateProcessA could not reach on a non-UTF-8 code page - and a
 *     directory that cannot be entered fails the attempt.
 *  2) env is laid over the host's environment: replaces, adds, and a bare "NAME"
 *     removes; a host variable it does not name passes through. Values are UTF-8.
 *  3) argv reaches the program as UTF-8.
 *  4) exit.code on FINISHED and FAILED, exit.signal for a program a signal ended
 *     (POSIX), and neither when GPTPS stopped it (timeout).
 *  5) Registration: an env entry with an empty name is refused; a clone keeps
 *     work_dir and env; a 2.6-sized def leaves both unread.
 *
 * MANUAL mode: gptps_step runs each job to completion on this thread.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L   /* setenv, mkdir */
#endif
#include "gptps.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/stat.h>
#  include <unistd.h>
#endif

#ifndef HELPER_PATH
#define HELPER_PATH "./prog_helper"
#endif

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

#define DIR_U8 "gptps_launch_\xC5\x9F\xC4\xB1"      /* "...şı": not in any Western code page */
#define ARG_U8 "a \xC5\x9F\xC4\x9F\xC3\xBC b"          /* "a şğü b" */

/* the last FINISHED / FAILED */
static int      g_done, g_ok;
static gptps_status g_st;
static char     g_res[512];
static int      g_have_code, g_have_sig;
static uint64_t g_code, g_sig;

static void on_ev(const gptps_event *ev, void *ud)
{
    const gptps_measure *m;
    (void)ud;
    if (ev->kind != GPTPS_EV_FINISHED && ev->kind != GPTPS_EV_FAILED) return;
    g_done = 1; g_ok = ev->kind == GPTPS_EV_FINISHED; g_st = ev->status;
    g_res[0] = 0;
    if (ev->result && ev->result_len < sizeof g_res) { memcpy(g_res, ev->result, ev->result_len); g_res[ev->result_len] = 0; }
    m = gptps_event_measure(ev, GPTPS_M_EXIT_CODE);
    g_have_code = m != NULL; g_code = m ? m->value : 0;
    if (m) CHECK(m->unit == GPTPS_UNIT_CODE);
    m = gptps_event_measure(ev, GPTPS_M_EXIT_SIGNAL);
    g_have_sig = m != NULL; g_sig = m ? m->value : 0;
}

static gptps *open_manual(void)
{
    gptps_config cfg; gptps *e = NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;
    cfg.mode = GPTPS_RUN_MANUAL;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (e) CHECK(gptps_set_event_cb(e, on_ev, NULL) == GPTPS_OK);
    return e;
}

static void def_prog(gptps_task_def *d, const char *name, const char *const *argv)
{
    memset(d, 0, sizeof *d);
    d->struct_size = sizeof *d; d->name = name; d->exec = GPTPS_EXEC_PROGRAM; d->argv = argv;
    d->default_cost.struct_size = sizeof d->default_cost;
    d->default_policy.struct_size = sizeof d->default_policy;
}

/* run one item of `task` to the end; 1 if it ended */
static int run1(gptps *e, const char *task)
{
    size_t n;
    g_done = 0;
    if (gptps_submit(e, task, NULL, 0, NULL) != GPTPS_OK) return 0;
    while (gptps_step(e, &n) == GPTPS_OK && n) { }
    return g_done;
}

/* make DIR_U8 holding marker.txt = "here" */
static int make_dir(void)
{
#if defined(_WIN32)
    wchar_t wd[128], wf[160];
    HANDLE h; DWORD w;
    MultiByteToWideChar(CP_UTF8, 0, DIR_U8, -1, wd, 128);
    CreateDirectoryW(wd, NULL);
    wcscpy(wf, wd); wcscat(wf, L"\\marker.txt");
    h = CreateFileW(wf, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { printf("CreateFileW: error %lu\n", (unsigned long)GetLastError()); return 0; }
    WriteFile(h, "here", 4, &w, NULL);
    CloseHandle(h);
    return w == 4;
#else
    FILE *f;
    mkdir(DIR_U8, 0700);
    f = fopen(DIR_U8 "/marker.txt", "wb");
    if (!f) return 0;
    fputs("here", f);
    return fclose(f) == 0;
#endif
}
static void remove_dir(void)
{
#if defined(_WIN32)
    wchar_t wd[128], wf[160];
    MultiByteToWideChar(CP_UTF8, 0, DIR_U8, -1, wd, 128);
    wcscpy(wf, wd); wcscat(wf, L"\\marker.txt");
    DeleteFileW(wf); RemoveDirectoryW(wd);
#else
    remove(DIR_U8 "/marker.txt"); rmdir(DIR_U8);
#endif
}
static void host_setenv(const char *k, const char *v)
{
#if defined(_WIN32)
    SetEnvironmentVariableA(k, v);
#else
    setenv(k, v, 1);
#endif
}

int main(void)
{
    gptps *e = open_manual();
    gptps_task_def d;
    static const char *const a_cwd[]  = { HELPER_PATH, "cwdfile", "marker.txt", NULL };
    static const char *const a_env[]  = { HELPER_PATH, "env", "GPTPS_T_A", "GPTPS_T_B", "GPTPS_T_GONE", "GPTPS_T_KEEP", NULL };
    static const char *const a_arg[]  = { HELPER_PATH, "arg", ARG_U8, NULL };
    static const char *const a_exit[] = { HELPER_PATH, "exit", "7", NULL };
    static const char *const a_cat[]  = { HELPER_PATH, "cat", NULL };
    static const char *const a_hang[] = { HELPER_PATH, "hang", NULL };
    static const char *const env[]    = { "GPTPS_T_A=one", "GPTPS_T_B=x\xC5\x9Fy", "GPTPS_T_GONE",
                                          "GPTPS_T_A=two", NULL };   /* the last A wins */
    if (!e) return 1;
    CHECK(make_dir());
    host_setenv("GPTPS_T_GONE", "present");
    host_setenv("GPTPS_T_KEEP", "kept");
    host_setenv("GPTPS_T_A", "host");

    /* 1) work_dir */
    def_prog(&d, "cwd", a_cwd); d.work_dir = DIR_U8;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(run1(e, "cwd") && g_ok && strcmp(g_res, "here") == 0);
    CHECK(g_have_code && g_code == 0 && !g_have_sig);
    def_prog(&d, "nowd", a_cat); d.work_dir = "gptps_launch_no_such_dir";
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(run1(e, "nowd") && !g_ok && g_st == GPTPS_E_TASK);
#if !defined(_WIN32)
    CHECK(g_have_code && g_code == 127);   /* Windows: CreateProcessW itself refuses */
#endif
    /* 5) a clone keeps work_dir (and env) */
    CHECK(gptps_clone_task(e, "cwd", "cwd2") == GPTPS_OK);
    CHECK(run1(e, "cwd2") && g_ok && strcmp(g_res, "here") == 0);
    /* 5) a 2.6 caller's def ends before work_dir: whatever lies past it is not read */
    {
        struct { gptps_task_def d; } big;
        def_prog(&big.d, "old", a_cwd);
        big.d.work_dir = "gptps_launch_no_such_dir";      /* would fail if it were read */
        big.d.struct_size = offsetof(gptps_task_def, work_dir);
        CHECK(gptps_register_task(e, &big.d) == GPTPS_OK);
        CHECK(run1(e, "old") && !g_ok);                   /* no marker.txt here: not in DIR_U8 */
        CHECK(g_have_code && g_code == 4);                /* the helper's "cannot open" */
    }

    /* 2) env */
    def_prog(&d, "env", a_env); d.env = env;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(run1(e, "env") && g_ok);
    CHECK(strcmp(g_res, "GPTPS_T_A=two\nGPTPS_T_B=x\xC5\x9Fy\nGPTPS_T_GONE unset\nGPTPS_T_KEEP=kept\n") == 0);
    if (fails) printf("env result: [%s]\n", g_res);
    CHECK(gptps_clone_task(e, "env", "env2") == GPTPS_OK);
    CHECK(run1(e, "env2") && g_ok && strstr(g_res, "GPTPS_T_GONE unset") != NULL);
    {   /* refused: an empty name */
        static const char *const bad1[] = { "=x", NULL };
        static const char *const bad2[] = { "", NULL };
        def_prog(&d, "bad", a_cat); d.env = bad1;
        CHECK(gptps_register_task(e, &d) == GPTPS_E_INVAL);
        d.env = bad2;
        CHECK(gptps_register_task(e, &d) == GPTPS_E_INVAL);
    }

    /* 3) a UTF-8 argument */
    def_prog(&d, "arg", a_arg);
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(run1(e, "arg") && g_ok && strcmp(g_res, ARG_U8) == 0);
    if (strcmp(g_res, ARG_U8) != 0) printf("arg result: [%s]\n", g_res);

    /* 4) how it ended */
    def_prog(&d, "exit7", a_exit);
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(run1(e, "exit7") && !g_ok && g_st == GPTPS_E_TASK);
    CHECK(g_have_code && g_code == 7 && !g_have_sig);
#if !defined(_WIN32)
    {
        static const char *const a_sig[] = { HELPER_PATH, "sig", NULL };
        def_prog(&d, "sig", a_sig);
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        CHECK(run1(e, "sig") && !g_ok && g_st == GPTPS_E_TASK);
        CHECK(g_have_sig && g_sig == 15 && !g_have_code);
    }
#endif
    def_prog(&d, "hang", a_hang); d.default_policy.timeout_seconds = 1;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(run1(e, "hang") && !g_ok && g_st == GPTPS_E_TIMEOUT);
    CHECK(!g_have_code && !g_have_sig);                   /* GPTPS ended it: the status says why */

    CHECK(gptps_shutdown(e) == GPTPS_OK);
    remove_dir();
    if (fails) { printf("%d program-launch check(s) FAILED\n", fails); return 1; }
    printf("program launch: all checks passed\n");
    return 0;
}
