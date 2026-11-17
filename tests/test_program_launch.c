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
 *  6) stderr_tail_bytes: a failing program's last stderr bytes arrive on FAILED and
 *     on its dead letter - and a program that writes far more than a pipe holds
 *     does not block; nothing on FINISHED; nothing when the type does not ask.
 *  7) stop_grace_ms (POSIX): a stopped program gets SIGTERM and time to clean up;
 *     one that ignores it is SIGKILLed when the grace runs out. Windows: stopped at
 *     once, and the first attempt logs that the grace is not in force.
 *  8) Stdout EOF is not the end: a program that points its stdout elsewhere and runs
 *     on (`exec >>job.log 2>&1`) runs to its own exit, with no deadline, and is
 *     reported by its exit code; a deadline still ends it.
 *  9) POSIX: the program starts with no signal blocked and none ignored, whatever
 *     the host's thread had - so a host that blocks SIGTERM for a sigwait thread
 *     still gets its programs a working stop_grace_ms.
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
#  include <direct.h>    /* _getcwd */
#else
#  include <signal.h>
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
static int      g_have_start;
static uint64_t g_start;
static char     g_start_method[32];
static char     g_tail[128];
static size_t   g_tail_len;
static int      g_has_tail;

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
    m = gptps_event_measure(ev, GPTPS_M_START_ERROR);
    g_have_start = m != NULL; g_start = m ? m->value : 0; g_start_method[0] = 0;
    if (m) { CHECK(m->unit == GPTPS_UNIT_CODE); strncpy(g_start_method, m->method, sizeof g_start_method - 1); }
    g_has_tail = ev->stderr_tail != NULL; g_tail_len = ev->stderr_tail_len; g_tail[0] = 0;
    if (ev->stderr_tail && ev->stderr_tail_len < sizeof g_tail) {
        memcpy(g_tail, ev->stderr_tail, ev->stderr_tail_len); g_tail[ev->stderr_tail_len] = 0;
    }
}

static char   g_dl_tail[128];
static size_t g_dl_len;
static void on_dl(const gptps_dead_letter *dl, void *ud)
{
    (void)ud;
    g_dl_len = dl->stderr_tail_len;
    if (dl->stderr_tail && dl->stderr_tail_len < sizeof g_dl_tail) {
        memcpy(g_dl_tail, dl->stderr_tail, dl->stderr_tail_len); g_dl_tail[dl->stderr_tail_len] = 0;
    }
}

#if defined(_WIN32)
static int g_grace_warns;
static void on_log(gptps_log_level lvl, const char *msg, void *ud)
{
    (void)ud;
    if (lvl == GPTPS_LOG_WARN && strstr(msg, "stop_grace_ms")) ++g_grace_warns;
}
#endif

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
/* Copy the helper to DIR_U8/bin/prog_helper_copy<exe>; `bin` gets that directory's
 * absolute path. 1 on success. */
static int copy_helper_into_bin(char *bin, size_t n, const char *exe)
{
    char cwd[700], dst[1200];
    FILE *in, *out;
    char buf[65536];
    size_t got;
    int ok = 1;
#if defined(_WIN32)
    wchar_t wd[1024];
    if (!_getcwd(cwd, sizeof cwd)) return 0;
    snprintf(bin, n, "%s\\" DIR_U8 "\\bin", cwd);
    MultiByteToWideChar(CP_UTF8, 0, bin, -1, wd, 1024);
    CreateDirectoryW(wd, NULL);
    snprintf(dst, sizeof dst, "%s\\prog_helper_copy%s", bin, exe);
    {
        wchar_t wdst[1200], wsrc[1200];
        MultiByteToWideChar(CP_UTF8, 0, dst, -1, wdst, 1200);
        MultiByteToWideChar(CP_UTF8, 0, HELPER_PATH, -1, wsrc, 1200);
        (void)in; (void)out; (void)buf; (void)got; (void)ok;
        return CopyFileW(wsrc, wdst, FALSE) != 0;
    }
#else
    if (!getcwd(cwd, sizeof cwd)) return 0;
    snprintf(bin, n, "%s/" DIR_U8 "/bin", cwd);
    mkdir(bin, 0700);
    snprintf(dst, sizeof dst, "%s/prog_helper_copy%s", bin, exe);
    in = fopen(HELPER_PATH, "rb");
    out = fopen(dst, "wb");
    if (!in || !out) ok = 0;
    while (ok && (got = fread(buf, 1, sizeof buf, in)) > 0) if (fwrite(buf, 1, got, out) != got) ok = 0;
    if (in) fclose(in);
    if (out && fclose(out) != 0) ok = 0;
    if (ok && chmod(dst, 0700) != 0) ok = 0;
    return ok;
#endif
}
static void remove_bin_copy(const char *exe)
{
#if defined(_WIN32)
    wchar_t w[300];
    char p[300];
    snprintf(p, sizeof p, DIR_U8 "\\bin\\prog_helper_copy%s", exe);
    MultiByteToWideChar(CP_UTF8, 0, p, -1, w, 300); DeleteFileW(w);
    MultiByteToWideChar(CP_UTF8, 0, DIR_U8 "\\bin", -1, w, 300); RemoveDirectoryW(w);
#else
    char p[300];
    snprintf(p, sizeof p, DIR_U8 "/bin/prog_helper_copy%s", exe);
    remove(p);
    rmdir(DIR_U8 "/bin");
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
    /* ABI 2.8: never started - an OS error, not an exit code */
    CHECK(g_have_start && !g_have_code);
#if !defined(_WIN32)
    CHECK(g_start == 2 /* ENOENT */ && strcmp(g_start_method, "posix.chdir") == 0);
#else
    CHECK(strcmp(g_start_method, "win.createprocess") == 0);
#endif
    {   /* a program that does not exist: the same, from the exec step */
        static const char *const a_none[] = { "gptps_launch_no_such_program_xyz", NULL };
        def_prog(&d, "noprog", a_none);
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        CHECK(run1(e, "noprog") && !g_ok && g_st == GPTPS_E_TASK);
        CHECK(g_have_start && !g_have_code);
#if !defined(_WIN32)
        CHECK(g_start == 2 /* ENOENT */ && strcmp(g_start_method, "posix.exec") == 0);
#else
        CHECK(g_start == 2 /* ERROR_FILE_NOT_FOUND */);
#endif
    }
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

    /* E) the program is found the same way everywhere (ABI 2.8): a relative path is
     * relative to work_dir, and a bare name is looked up in the PATH the program will
     * see - here a copy of the helper in DIR_U8/bin */
    {
        char bin[1024], prog[1200], pathvar[1300];
        static const char *a_rel[] = { NULL, "arg", "found", NULL };
        static const char *a_bare[] = { NULL, "arg", "found", NULL };
        static const char *env_path[] = { NULL, NULL };
#if defined(_WIN32)
        static const char *rel = "bin\\prog_helper_copy";            /* no .exe: Windows adds it */
        const char *exe = ".exe";
#else
        static const char *rel = "bin/prog_helper_copy";
        const char *exe = "";
#endif
        CHECK(copy_helper_into_bin(bin, sizeof bin, exe));
        snprintf(prog, sizeof prog, "%s", rel);
        a_rel[0] = prog;
        def_prog(&d, "rel", a_rel); d.work_dir = DIR_U8;
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        CHECK(run1(e, "rel") && g_ok && strcmp(g_res, "found") == 0);
        snprintf(pathvar, sizeof pathvar, "PATH=%s", bin);
        env_path[0] = pathvar;
        a_bare[0] = "prog_helper_copy";
        def_prog(&d, "bare", a_bare); d.env = env_path;
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        CHECK(run1(e, "bare") && g_ok && strcmp(g_res, "found") == 0);
        remove_bin_copy(exe);
    }

    /* D) one argument that is not UTF-8 does not drag the others into the ANSI code
     * page with it (Windows; POSIX passes bytes as they are) */
    {
        static const char *const a_mixed[] = { HELPER_PATH, "arg", ARG_U8, "\xFE\xFF", NULL };
        def_prog(&d, "mixed", a_mixed);
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        CHECK(run1(e, "mixed") && g_ok && strcmp(g_res, ARG_U8) == 0);
    }
    /* D) Windows: the block the program gets is in Windows' order, overrides included */
    {
        static const char *const a_sorted[] = { HELPER_PATH, "envsorted", NULL };
        static const char *const env_s[] = { "zzz_gptps_last=1", "AAA_GPTPS_FIRST=1", "mmm_gptps=1", NULL };
        def_prog(&d, "sorted", a_sorted); d.env = env_s;
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        CHECK(run1(e, "sorted") && g_ok);
#if defined(_WIN32)
        /* Wine hands every program an environment of its own making, unsorted */
        if (!GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version"))
#endif
        {
            CHECK(strcmp(g_res, "sorted") == 0);
            if (strcmp(g_res, "sorted") != 0) printf("envsorted: [%s]\n", g_res);
        }
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

    /* 6) the stderr tail: 200 000 bytes - three times what a pipe holds - then the
     * line that says why; only the last 32 bytes come back */
    {
        static const char *const a_err[] = { HELPER_PATH, "errout", "200000", "FATAL: bad input", "3", NULL };
        static const char *const a_errok[] = { HELPER_PATH, "errout", "100", "fine", "0", NULL };
        static const char *const a_errhost[] = { HELPER_PATH, "errout", "0",
                                                 "(test_program_launch: this line on stderr is expected)", "3", NULL };
#if defined(_WIN32)
        const char *want = "..............FATAL: bad input\r\n"; /* the last 32 bytes, raw: */
#else                                                             /* the CRT's text-mode stderr */
        const char *want = "...............FATAL: bad input\n";   /* the last 32 bytes */
#endif
        gptps_dead_letter_drain(e, NULL, NULL);               /* the earlier failures' */
        def_prog(&d, "err", a_err); d.stderr_tail_bytes = 32;
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        CHECK(run1(e, "err") && !g_ok && g_st == GPTPS_E_TASK && g_code == 3);
        CHECK(g_has_tail && g_tail_len == 32);
        CHECK(strcmp(g_tail, want) == 0);
        g_dl_len = 0; g_dl_tail[0] = 0;
        CHECK(gptps_dead_letter_drain(e, on_dl, NULL) == 1);
        CHECK(g_dl_len == 32 && strcmp(g_dl_tail, g_tail) == 0);
        /* succeeded: no evidence needed */
        def_prog(&d, "errok", a_errok); d.stderr_tail_bytes = 32;
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        CHECK(run1(e, "errok") && g_ok && !g_has_tail);
        /* not asked: stderr is the host's, as before */
        def_prog(&d, "errhost", a_errhost);
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        CHECK(run1(e, "errhost") && !g_ok && !g_has_tail);
        CHECK(gptps_dead_letter_drain(e, on_dl, NULL) == 1);
    }

    /* 7) stop_grace_ms */
#if !defined(_WIN32)
    {
        static const char *const a_term[] = { HELPER_PATH, "term", "gptps_launch_cleaned.txt", NULL };
        static const char *const a_tign[] = { HELPER_PATH, "termignore", NULL };
        FILE *f;
        char got[16] = { 0 };
        uint64_t t0;
        remove("gptps_launch_cleaned.txt");
        def_prog(&d, "term", a_term); d.default_policy.timeout_seconds = 1; d.stop_grace_ms = 5000;
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        CHECK(run1(e, "term") && !g_ok && g_st == GPTPS_E_TIMEOUT);
        f = fopen("gptps_launch_cleaned.txt", "rb");
        CHECK(f != NULL);
        if (f) { CHECK(fread(got, 1, sizeof got - 1, f) == 7); fclose(f); }
        CHECK(strcmp(got, "cleaned") == 0);                   /* it got its SIGTERM, and used it */
        remove("gptps_launch_cleaned.txt");
        /* no grace: SIGKILL, no chance to clean up */
        def_prog(&d, "termnow", a_term); d.default_policy.timeout_seconds = 1;
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        CHECK(run1(e, "termnow") && !g_ok && g_st == GPTPS_E_TIMEOUT);
        f = fopen("gptps_launch_cleaned.txt", "rb");
        CHECK(f == NULL);
        if (f) fclose(f);
        /* ignores SIGTERM: killed when the grace runs out - bounded */
        def_prog(&d, "tign", a_tign); d.default_policy.timeout_seconds = 1; d.stop_grace_ms = 400;
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        t0 = gptps_now_ms(NULL);
        CHECK(run1(e, "tign") && !g_ok && g_st == GPTPS_E_TIMEOUT);
        CHECK(gptps_now_ms(NULL) - t0 >= 1300 && gptps_now_ms(NULL) - t0 < 4000);
    }
#else
    {   /* Windows, both cases every run, whatever console ctest gave us. This process
         * is the host here, so it does what the WARN tells a host to do. */
        static const char *const a_brk[] = { HELPER_PATH, "brk", "gptps_launch_cleaned.txt", NULL };
        /* Wine cannot deliver Ctrl+Break to a process group; there only the
         * no-console case is checked in full */
        int wine = GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version") != NULL;
        FILE *f;
        gptps_set_log_sink(on_log, NULL);

        /* a) no console (a service), ABI 2.8: the job gets a hidden console of its own,
         * and the host reaches it there - Ctrl+Break and the grace, with no setup */
        FreeConsole();
        remove("gptps_launch_cleaned.txt");
        def_prog(&d, "grace_nc", a_brk); d.default_policy.timeout_seconds = 1; d.stop_grace_ms = 5000;
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        CHECK(run1(e, "grace_nc") && !g_ok && g_st == GPTPS_E_TIMEOUT);
        CHECK(g_grace_warns == 0);
        f = fopen("gptps_launch_cleaned.txt", "rb");
        if (!wine) {
            char got[16] = { 0 };
            CHECK(f != NULL);
            if (f) { CHECK(fread(got, 1, sizeof got - 1, f) == 7); }
            CHECK(strcmp(got, "cleaned") == 0);
        }
        if (f) fclose(f);

        /* b) a host with a (hidden) console of its own: the job shares it */
        CHECK(AllocConsole());
        if (GetConsoleWindow()) ShowWindow(GetConsoleWindow(), SW_HIDE);
        remove("gptps_launch_cleaned.txt");
        g_grace_warns = 0;
        def_prog(&d, "grace_c", a_brk); d.default_policy.timeout_seconds = 1; d.stop_grace_ms = 5000;
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        CHECK(run1(e, "grace_c") && !g_ok && g_st == GPTPS_E_TIMEOUT);
        CHECK(g_grace_warns == 0);                            /* the grace was in force */
        f = fopen("gptps_launch_cleaned.txt", "rb");
        if (wine) {
            printf("(Wine: Ctrl+Break delivery not checked)\n");
        } else {
            char got[16] = { 0 };
            CHECK(f != NULL);                                 /* it got its Ctrl+Break, and used it */
            if (f) { CHECK(fread(got, 1, sizeof got - 1, f) == 7); }
            CHECK(strcmp(got, "cleaned") == 0);
        }
        if (f) fclose(f);
        remove("gptps_launch_cleaned.txt");
        FreeConsole();
        gptps_set_log_sink(NULL, NULL);
    }
#endif

    /* 8) stdout EOF, and the program runs on: no deadline, so it ends when it exits -
     * up to 1.9 POSIX SIGKILLed it 2 s after the EOF and reported a timeout */
    {
        static const char *const a_eof0[] = { HELPER_PATH, "eofsleep", "3000", "0", NULL };
        static const char *const a_eof5[] = { HELPER_PATH, "eofsleep", "2500", "5", NULL };
        static const char *const a_eofd[] = { HELPER_PATH, "eofsleep", "30000", "0", NULL };
        uint64_t t0;
        def_prog(&d, "eof0", a_eof0); d.stderr_tail_bytes = 16;
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        t0 = gptps_now_ms(NULL);
        CHECK(run1(e, "eof0") && g_ok && g_st == GPTPS_OK && strcmp(g_res, "x") == 0);
        CHECK(g_have_code && g_code == 0);
        CHECK(gptps_now_ms(NULL) - t0 >= 2900);               /* it ran its course */
        /* its exit code says how it went, and what it wrote after the EOF is kept */
        def_prog(&d, "eof5", a_eof5); d.stderr_tail_bytes = 16;
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        CHECK(run1(e, "eof5") && !g_ok && g_st == GPTPS_E_TASK);
        CHECK(g_have_code && g_code == 5);
        CHECK(g_has_tail && strstr(g_tail, "late") != NULL);
        gptps_dead_letter_drain(e, NULL, NULL);
        /* a deadline still ends it, and cancel / shutdown still do (test_exec_faults,
         * test_hang: "eofhang") */
        def_prog(&d, "eofd", a_eofd); d.default_policy.timeout_seconds = 1; d.stderr_tail_bytes = 16;
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        t0 = gptps_now_ms(NULL);
        CHECK(run1(e, "eofd") && !g_ok && g_st == GPTPS_E_TIMEOUT);
        CHECK(gptps_now_ms(NULL) - t0 < 5000);
        gptps_dead_letter_drain(e, NULL, NULL);
    }

#if !defined(_WIN32)
    /* 9) the host's signal state does not reach the program. MANUAL mode runs the
     * job on this thread, so this thread's mask is the one a worker would have. */
    {
        static const char *const a_sig[]  = { HELPER_PATH, "sigstate", NULL };
        static const char *const a_term[] = { HELPER_PATH, "term", "gptps_launch_cleaned.txt", NULL };
        sigset_t block, old;
        struct sigaction ign, oldpipe;
        FILE *f;
        char got[16] = { 0 };
        sigemptyset(&block); sigaddset(&block, SIGTERM); sigaddset(&block, SIGINT);
        CHECK(sigprocmask(SIG_BLOCK, &block, &old) == 0);    /* the sigwait-thread pattern */
        memset(&ign, 0, sizeof ign); ign.sa_handler = SIG_IGN; sigemptyset(&ign.sa_mask);
        CHECK(sigaction(SIGPIPE, &ign, &oldpipe) == 0);       /* a server's usual setup */
        def_prog(&d, "sigstate", a_sig);
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        CHECK(run1(e, "sigstate") && g_ok && strcmp(g_res, "term=0 pipe=0") == 0);
        if (strcmp(g_res, "term=0 pipe=0") != 0) printf("sigstate: [%s]\n", g_res);
        /* and so its SIGTERM arrives: the grace is used, not waited out */
        remove("gptps_launch_cleaned.txt");
        def_prog(&d, "termblk", a_term); d.default_policy.timeout_seconds = 1; d.stop_grace_ms = 5000;
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        CHECK(run1(e, "termblk") && !g_ok && g_st == GPTPS_E_TIMEOUT);
        f = fopen("gptps_launch_cleaned.txt", "rb");
        CHECK(f != NULL);
        if (f) { CHECK(fread(got, 1, sizeof got - 1, f) == 7); fclose(f); }
        CHECK(strcmp(got, "cleaned") == 0);
        remove("gptps_launch_cleaned.txt");
        gptps_dead_letter_drain(e, NULL, NULL);
        CHECK(sigaction(SIGPIPE, &oldpipe, NULL) == 0);
        CHECK(sigprocmask(SIG_SETMASK, &old, NULL) == 0);
    }
#endif

    CHECK(gptps_shutdown(e) == GPTPS_OK);
    remove_dir();
    if (fails) { printf("%d program-launch check(s) FAILED\n", fails); return 1; }
    printf("program launch: all checks passed\n");
    return 0;
}
