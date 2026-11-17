/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_exec_win_faults.c - the Windows executor and HAL where the system does not
 * behave, or behaves concurrently, in ways a plain test cannot arrange.
 *
 * HOW. This file includes src/exec_win.c and src/hal_win.c themselves (as the single-
 * file amalgamation does), with a few Win32 calls renamed to wrappers defined here.
 * Unarmed, a wrapper passes the call through. The library's own copies of those two
 * files are then never linked: every symbol they define is defined here first.
 *
 *  1) Pipe ends stay with their job [17]: while job A is being set up, job B is
 *     started by another thread (from inside A's CreateJobObjectA). B lives on, and
 *     A must still end as soon as its program does - B must not hold A's stdout.
 *  2) The console probe is not fooled by a stop [18]: while a stop has the host
 *     attached to another job's console, a job started meanwhile must still be
 *     started without a console window of the host's (CREATE_NO_WINDOW), and must
 *     not be given a copy of that console's stderr for its own.
 *  3) Die-with-host is claimed only when it holds [19]: no job object, its limits
 *     refused, or the program not in it - and a WARN says so, and the task type's own
 *     WARN gives that cause, not a missing parent-death signal.
 *  4) Threads that cannot be started fail the attempt with GPTPS_E_NOMEM [20], never
 *     a success with an empty payload or result.
 *  5) The CPU count is the machine's, every processor group's [21]: the CPU cap and
 *     the HAL's cpu_count.
 *  6) The Windows command line keeps an unquoted argument's trailing backslashes [54].
 *  7) Paths are UTF-8 for add-ons and the settings save, and the save is flushed and
 *     written through [9] [25].
 *  8) The output-cap report names the cap only when the cap ended the attempt [53]:
 *     a job past its deadline that writes too much in its grace is a timeout.
 */
#include <windows.h>
#include <psapi.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef HELPER_PATH
#define HELPER_PATH "prog_helper.exe"
#endif

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

/* ---- the wrappers ------------------------------------------------------- */
static volatile LONG w_thread_calls, w_thread_fail_at;      /* fail the Nth CreateThread */
static volatile LONG w_job_fail, w_limits_fail, w_assign_fail;
static volatile LONG w_cpu_rate_seen;                        /* the last CpuRate set */
static volatile LONG w_fake_cpus;                            /* GetActiveProcessorCount, when set */
static volatile LONG w_fake_console, w_attached;             /* the console, when faked */
static volatile LONG w_last_flags;                           /* the last CreateProcessW's flags */
static HANDLE w_ev_attached, w_ev_launched;
static volatile LONG w_flushes, w_move_flags;
static HANDLE w_borrowed;                                     /* the stderr a borrowed console sets */
static volatile LONG w_dup_borrowed;                          /* ...and it was copied for a program */
static void (*w_job_hook)(void);                              /* once, from CreateJobObjectA */

static HANDLE WINAPI fake_CreateThread(LPSECURITY_ATTRIBUTES a, SIZE_T s, LPTHREAD_START_ROUTINE f,
                                       LPVOID p, DWORD fl, LPDWORD id)
{
    LONG n = InterlockedIncrement(&w_thread_calls);
    if (w_thread_fail_at && n == w_thread_fail_at) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    return CreateThread(a, s, f, p, fl, id);
}
static HANDLE WINAPI fake_CreateJobObjectA(LPSECURITY_ATTRIBUTES a, LPCSTR n)
{
    void (*hook)(void) = w_job_hook;
    w_job_hook = NULL;
    if (hook) hook();
    if (w_job_fail) { SetLastError(ERROR_ACCESS_DENIED); return NULL; }
    return CreateJobObjectA(a, n);
}
static BOOL WINAPI fake_SetInformationJobObject(HANDLE j, JOBOBJECTINFOCLASS c, LPVOID p, DWORD n)
{
    if (c == JobObjectExtendedLimitInformation && w_limits_fail) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    if ((int)c == 15 /* JobObjectCpuRateControlInformation */) {
        InterlockedExchange(&w_cpu_rate_seen, (LONG)((const DWORD *)p)[1]);
        return TRUE;                  /* not set for real: a fake 128-CPU rate */
    }
    return SetInformationJobObject(j, c, p, n);
}
static BOOL WINAPI fake_AssignProcessToJobObject(HANDLE j, HANDLE p)
{
    if (w_assign_fail) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return AssignProcessToJobObject(j, p);
}
static DWORD WINAPI fake_apc(WORD group) { (void)group; return (DWORD)w_fake_cpus; }
static FARPROC WINAPI fake_GetProcAddress(HMODULE m, LPCSTR name)
{
    if (w_fake_cpus && strcmp(name, "GetActiveProcessorCount") == 0) return (FARPROC)(void (*)(void))fake_apc;
    return GetProcAddress(m, name);
}
static DWORD WINAPI fake_GetConsoleProcessList(LPDWORD l, DWORD n)
{
    if (w_fake_console) { if (n && w_attached) l[0] = GetCurrentProcessId(); return w_attached ? 1u : 0u; }
    return GetConsoleProcessList(l, n);
}
static BOOL WINAPI fake_AttachConsole(DWORD pid)
{
    if (!w_fake_console) return AttachConsole(pid);
    InterlockedExchange(&w_attached, 1);           /* the host is on another job's console */
    if (w_borrowed) SetStdHandle(STD_ERROR_HANDLE, w_borrowed);   /* which brings its own handles */
    SetEvent(w_ev_attached);
    WaitForSingleObject(w_ev_launched, 1500);      /* a launch that runs now sees it */
    return TRUE;
}
static BOOL WINAPI fake_GenerateConsoleCtrlEvent(DWORD ev, DWORD grp)
{
    if (w_fake_console) return TRUE;
    return GenerateConsoleCtrlEvent(ev, grp);
}
static BOOL WINAPI fake_FreeConsole(void)
{
    if (w_fake_console) { InterlockedExchange(&w_attached, 0); return TRUE; }
    return FreeConsole();
}
static BOOL WINAPI fake_CreateProcessW(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta,
                                       BOOL inh, DWORD fl, LPVOID env, LPCWSTR dir, LPSTARTUPINFOW si,
                                       LPPROCESS_INFORMATION pi)
{
    BOOL ok = CreateProcessW(app, cmd, pa, ta, inh, fl, env, dir, si, pi);
    InterlockedExchange(&w_last_flags, (LONG)fl);
    if (w_fake_console) SetEvent(w_ev_launched);
    return ok;
}
static BOOL WINAPI fake_DuplicateHandle(HANDLE sp, HANDLE s, HANDLE tp, LPHANDLE t, DWORD acc, BOOL inh, DWORD o)
{
    if (w_borrowed && s == w_borrowed) InterlockedExchange(&w_dup_borrowed, 1);
    return DuplicateHandle(sp, s, tp, t, acc, inh, o);
}
static BOOL WINAPI fake_FlushFileBuffers(HANDLE h)
{
    InterlockedIncrement(&w_flushes);
    return FlushFileBuffers(h);
}
static BOOL WINAPI fake_MoveFileExW(LPCWSTR a, LPCWSTR b, DWORD fl)
{
    InterlockedExchange(&w_move_flags, (LONG)fl);
    return MoveFileExW(a, b, fl);
}

#define CreateThread              fake_CreateThread
#define CreateJobObjectA          fake_CreateJobObjectA
#define SetInformationJobObject   fake_SetInformationJobObject
#define AssignProcessToJobObject  fake_AssignProcessToJobObject
#define GetProcAddress            fake_GetProcAddress
#define GetConsoleProcessList     fake_GetConsoleProcessList
#define AttachConsole             fake_AttachConsole
#define GenerateConsoleCtrlEvent  fake_GenerateConsoleCtrlEvent
#define FreeConsole               fake_FreeConsole
#define CreateProcessW            fake_CreateProcessW
#define FlushFileBuffers          fake_FlushFileBuffers
#define MoveFileExW               fake_MoveFileExW
#define DuplicateHandle           fake_DuplicateHandle
#include "../src/hal_win.c"
#include "../src/exec_win.c"
#undef CreateThread
#undef CreateJobObjectA
#undef SetInformationJobObject
#undef AssignProcessToJobObject
#undef GetProcAddress
#undef GetConsoleProcessList
#undef AttachConsole
#undef GenerateConsoleCtrlEvent
#undef FreeConsole
#undef CreateProcessW
#undef FlushFileBuffers
#undef MoveFileExW
#undef DuplicateHandle

/* ---- helpers ------------------------------------------------------------ */
static volatile LONG g_warn_job, g_warn_die_type, g_warn_die_wrong, g_warn_out;
static void on_log(gptps_log_level lvl, const char *msg, void *ud)
{
    (void)ud;
    if (lvl != GPTPS_LOG_WARN || !msg) return;
    if (strstr(msg, "is not enforced for this job")) InterlockedIncrement(&g_warn_die_type);    /* the type's */
    else if (strstr(msg, "kill-on-close job object")) InterlockedIncrement(&g_warn_job);       /* the executor's */
    if (strstr(msg, "parent-death")) InterlockedIncrement(&g_warn_die_wrong);
    if (strstr(msg, "the most a result may be")) InterlockedIncrement(&g_warn_out);
}

static volatile LONG g_done;
static void on_ev(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_FINISHED || ev->kind == GPTPS_EV_FAILED) InterlockedIncrement(&g_done);
}

static void def_prog(gptps_task_def *d, const char *const *argv, uint64_t flags)
{
    memset(d, 0, sizeof *d);
    d->struct_size = sizeof *d; d->name = "t"; d->exec = GPTPS_EXEC_PROGRAM; d->argv = argv; d->flags = flags;
}

/* Run one attempt; *res gets the result as a string (or ""). */
static gptps_status run_tm(const char *const *argv, const char *payload, uint32_t grace_ms, size_t err_cap,
                           uint32_t timeout_s, gptps_exec_caps *caps, char *res, size_t rn)
{
    gptps_task_def d;
    gptps_exec_meter m;
    void *out = NULL;
    size_t len = 0;
    uint32_t cancel = 0;
    gptps_status st;
    def_prog(&d, argv, GPTPS_TASK_DIE_WITH_HOST);
    memset(caps, 0, sizeof *caps);
    caps->stop_grace_ms = grace_ms;
    memset(&m, 0, sizeof m);
    m.err_cap = err_cap;
    st = gptps_program_execute(&d, payload, payload ? strlen(payload) : 0, caps, timeout_s, &cancel, &out, &len, &m);
    if (res && rn) {
        size_t n = (out && len < rn) ? len : 0;
        if (n) memcpy(res, out, n);
        res[n] = 0;
    }
    gptps_free(out);
    gptps_free(m.err);
    return st;
}
static gptps_status run(const char *const *argv, const char *payload, uint32_t grace_ms, size_t err_cap,
                        gptps_exec_caps *caps, char *res, size_t rn)
{
    return run_tm(argv, payload, grace_ms, err_cap, 20, caps, res, rn);
}

/* ---- 1) pipe ends stay with their job ------------------------------------ */
typedef struct { uint32_t cancel; gptps_status st; } sib_ctx;
static sib_ctx g_sib;
static HANDLE g_sib_thread;
#define SIB_PID_FILE "gptps_winfaults_sib.txt"
static unsigned __stdcall sib_main(void *p)
{
    static const char *const a_sleep[] = { HELPER_PATH, "pidsleep", SIB_PID_FILE, NULL };
    gptps_task_def d;
    gptps_exec_caps caps;
    void *out = NULL;
    size_t len = 0;
    (void)p;
    def_prog(&d, a_sleep, 0);
    memset(&caps, 0, sizeof caps);
    g_sib.st = gptps_program_execute(&d, NULL, 0, &caps, 60, &g_sib.cancel, &out, &len, NULL);
    gptps_free(out);
    return 0;
}
static void start_sibling(void)            /* from inside job A's set-up */
{
    int i;
    FILE *f;
    remove(SIB_PID_FILE);
    g_sib_thread = (HANDLE)_beginthreadex(NULL, 0, sib_main, NULL, 0, NULL);
    for (i = 0; i < 1000; ++i) {           /* until B's program runs */
        if ((f = fopen(SIB_PID_FILE, "rb")) != NULL) { fclose(f); break; }
        Sleep(10);
    }
}
static void pipes_stay(void)
{
    static const char *const a_cat[] = { HELPER_PATH, "cat", NULL };
    gptps_exec_caps caps;
    char res[64];
    uint64_t t0, took;
    gptps_status st;
    memset(&g_sib, 0, sizeof g_sib);
    w_job_hook = start_sibling;
    t0 = gptps_hal_monotonic_ms();
    st = run(a_cat, "hello", 0, 64, &caps, res, sizeof res);   /* stdout AND stderr pipes */
    took = gptps_hal_monotonic_ms() - t0;
    CHECK(g_sib_thread != NULL);
    CHECK(st == GPTPS_OK && strcmp(res, "hello") == 0);
    CHECK(took < 3000);            /* B holding A's pipe ends cost A 5-10 s */
    if (took >= 3000) printf("job A took %lu ms while job B lived\n", (unsigned long)took);
    InterlockedExchange((volatile LONG *)&g_sib.cancel, 1);
    if (g_sib_thread) { WaitForSingleObject(g_sib_thread, 30000); CloseHandle(g_sib_thread); }
    CHECK(g_sib.st == GPTPS_E_CANCELLED);
    remove(SIB_PID_FILE);
}

/* ---- 2) the console probe and a stop ------------------------------------ */
static unsigned __stdcall stopper(void *p)
{
    (void)p;
    (void)win_ctrl_break(4242, 0);         /* a service host's stop of some other job */
    return 0;
}
static void console_probe(void)
{
    static const char *const a_exit0[] = { HELPER_PATH, "exit", "0", NULL };
    gptps_exec_caps caps;
    HANDLE t;
    w_ev_attached = CreateEventA(NULL, TRUE, FALSE, NULL);
    w_ev_launched = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE own_err = GetStdHandle(STD_ERROR_HANDLE);
    w_borrowed = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(w_borrowed != INVALID_HANDLE_VALUE);
    if (w_borrowed == INVALID_HANDLE_VALUE) w_borrowed = NULL;
    InterlockedExchange(&w_dup_borrowed, 0);
    InterlockedExchange(&w_fake_console, 1);       /* a host with no console: a service */
    t = (HANDLE)_beginthreadex(NULL, 0, stopper, NULL, 0, NULL);
    CHECK(t != NULL);
    CHECK(WaitForSingleObject(w_ev_attached, 10000) == WAIT_OBJECT_0);
    /* with a stop grace, and stderr not kept: the program gets the host's stderr */
    CHECK(run(a_exit0, NULL, 1000, 0, &caps, NULL, 0) == GPTPS_OK);
    CHECK(((DWORD)w_last_flags & CREATE_NO_WINDOW) != 0);   /* the host has no console of its own */
    CHECK(w_dup_borrowed == 0);            /* ...nor the borrowed console's stderr */
    if (t) { WaitForSingleObject(t, 10000); CloseHandle(t); }
    CHECK(GetStdHandle(STD_ERROR_HANDLE) == own_err);       /* the stop put the host's back */
    InterlockedExchange(&w_fake_console, 0);
    if (w_borrowed) { CloseHandle(w_borrowed); w_borrowed = NULL; }
    CloseHandle(w_ev_attached); CloseHandle(w_ev_launched);
}

/* ---- 3) die-with-host only when it holds -------------------------------- */
static void die_claim(void)
{
    static const char *const a_cat[] = { HELPER_PATH, "cat", NULL };
    gptps_exec_caps caps;
    char res[64];
    InterlockedExchange(&g_warn_job, 0);
    CHECK(run(a_cat, "ok", 0, 0, &caps, res, sizeof res) == GPTPS_OK && strcmp(res, "ok") == 0);
    CHECK((caps.applied & GPTPS_CAP_DIE) != 0);
    CHECK(g_warn_job == 0);
    w_assign_fail = 1;                     /* e.g. Windows 7 with the host already in a job */
    CHECK(run(a_cat, "ok", 0, 0, &caps, res, sizeof res) == GPTPS_OK && strcmp(res, "ok") == 0);
    CHECK((caps.applied & GPTPS_CAP_DIE) == 0);
    w_assign_fail = 0;
    w_limits_fail = 1;                     /* in the job, but without kill-on-close */
    CHECK(run(a_cat, "ok", 0, 0, &caps, res, sizeof res) == GPTPS_OK && strcmp(res, "ok") == 0);
    CHECK((caps.applied & GPTPS_CAP_DIE) == 0);
    w_limits_fail = 0;
    w_job_fail = 1;                        /* no job object at all */
    CHECK(run(a_cat, "ok", 0, 0, &caps, res, sizeof res) == GPTPS_OK && strcmp(res, "ok") == 0);
    CHECK((caps.applied & GPTPS_CAP_DIE) == 0);
    w_job_fail = 0;
    CHECK(g_warn_job == 1);                /* said, once */
    {   /* through the engine: the type's own WARN gives the Windows cause */
        gptps_config cfg;
        gptps_task_def d;
        gptps *e = NULL;
        int i;
        memset(&cfg, 0, sizeof cfg);
        cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
        cfg.limits.max_concurrent_tasks = 1;
        CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
        if (e) CHECK(gptps_set_event_cb(e, on_ev, NULL) == GPTPS_OK);
        def_prog(&d, a_cat, GPTPS_TASK_DIE_WITH_HOST);
        d.name = "diejob";
        d.default_cost.struct_size = sizeof d.default_cost;
        d.default_policy.struct_size = sizeof d.default_policy;
        InterlockedExchange(&g_warn_die_type, 0);
        InterlockedExchange(&g_warn_die_wrong, 0);
        InterlockedExchange(&g_done, 0);
        w_job_fail = 1;
        if (e && gptps_register_task(e, &d) == GPTPS_OK) {
            for (i = 0; i < 2; ++i) CHECK(gptps_submit(e, "diejob", "x", 1, NULL) == GPTPS_OK);
            for (i = 0; i < 3000 && g_done < 2; ++i) Sleep(10);
            CHECK(g_done == 2);
        } else {
            CHECK(!"engine");
        }
        if (e) CHECK(gptps_shutdown(e) == GPTPS_OK);
        w_job_fail = 0;
        CHECK(g_warn_die_type == 1);       /* once for the type */
        CHECK(g_warn_die_wrong == 0);      /* and not the "no parent-death signal" of POSIX */
    }
}

/* ---- 4) threads that cannot be started ---------------------------------- */
static void thread_faults(void)
{
    static const char *const a_cat[] = { HELPER_PATH, "cat", NULL };
    gptps_exec_caps caps;
    char res[64];
    uint64_t t0;
    /* with a payload: 1 writer, 2 reader, 3 stderr */
    LONG n;
    for (n = 1; n <= 3; ++n) {
        InterlockedExchange(&w_thread_calls, 0);
        InterlockedExchange(&w_thread_fail_at, n);
        t0 = gptps_hal_monotonic_ms();
        CHECK(run(a_cat, "payload", 0, 64, &caps, res, sizeof res) == GPTPS_E_NOMEM);
        CHECK(res[0] == 0);
        CHECK(gptps_hal_monotonic_ms() - t0 < 3000);
    }
    /* no payload: no writer, the reader is the first */
    InterlockedExchange(&w_thread_calls, 0);
    InterlockedExchange(&w_thread_fail_at, 1);
    CHECK(run(a_cat, NULL, 0, 0, &caps, res, sizeof res) == GPTPS_E_NOMEM);
    InterlockedExchange(&w_thread_fail_at, 0);
    /* and unarmed, all is well */
    CHECK(run(a_cat, "payload", 0, 64, &caps, res, sizeof res) == GPTPS_OK && strcmp(res, "payload") == 0);
}

/* ---- 5) every processor group ------------------------------------------- */
static void cpu_groups(void)
{
    gptps_hwinfo hw;
    HANDLE job = CreateJobObjectA(NULL, NULL);
    CHECK(job != NULL);
    InterlockedExchange(&w_fake_cpus, 128);        /* two groups of 64 */
    InterlockedExchange(&w_cpu_rate_seen, 0);
    CHECK(win_cpu_rate(job, 4000) == 1u);
    CHECK(w_cpu_rate_seen == 313);         /* 4 of 128 CPUs: 3.125%, rounded up */
    if (w_cpu_rate_seen != 313) printf("CpuRate for 4000 millicpus of 128 CPUs: %ld\n", (long)w_cpu_rate_seen);
    CHECK(gptps_hal_detect(&hw) == GPTPS_OK && hw.cpu_count == 128u);
    InterlockedExchange(&w_fake_cpus, 0);
    CHECK(gptps_hal_detect(&hw) == GPTPS_OK && hw.cpu_count >= 1u);
    if (job) CloseHandle(job);
}

/* ---- 6) the command line ------------------------------------------------- */
static void cmdline(void)
{
    static const struct { const wchar_t *arg, *want; } c[] = {
        { L"/out=D:\\Reports\\", L"/out=D:\\Reports\\" },     /* unquoted: as it is */
        { L"a b\\",              L"\"a b\\\\\"" },            /* quoted: doubled before the quote */
        { L"a\\\"b",             L"\"a\\\\\\\"b\"" },
        { L"",                   L"\"\"" },
        { L"\\\\srv\\x\\",       L"\\\\srv\\x\\" },
    };
    size_t i;
    for (i = 0; i < sizeof c / sizeof c[0]; ++i) {
        wchar_t *v[1];
        wchar_t *line;
        v[0] = (wchar_t *)c[i].arg;
        line = build_cmdline_w(v, 1);
        CHECK(line && wcscmp(line, c[i].want) == 0);
        gptps_free(line);
    }
}

/* ---- 7) UTF-8 paths, durable save ---------------------------------------- */
#define DIR_W  L"gptps_winfaults_\x015E\x0131rket"          /* "Şırket" */
#define DIR_U8 "gptps_winfaults_\xC5\x9E\xC4\xB1rket"
static int write_w(const wchar_t *p, const char *s)
{
    FILE *f = _wfopen(p, L"wb");
    if (!f) return 0;
    fputs(s, f);
    return fclose(f) == 0;
}
static int read_is(const wchar_t *p, const char *s)
{
    char buf[64] = { 0 };
    FILE *f = _wfopen(p, L"rb");
    if (!f) return 0;
    if (fread(buf, 1, sizeof buf - 1, f) == 0) buf[0] = 0;
    fclose(f);
    return strcmp(buf, s) == 0;
}
static void utf8_paths(void)
{
    CreateDirectoryW(DIR_W, NULL);
    CHECK(write_w(DIR_W L"\\gptps.toml", "old"));
    CHECK(write_w(DIR_W L"\\gptps.toml.tmp", "new"));
    InterlockedExchange(&w_flushes, 0);
    InterlockedExchange(&w_move_flags, 0);
    CHECK(gptps_hal_atomic_replace(DIR_U8 "\\gptps.toml.tmp", DIR_U8 "\\gptps.toml") == GPTPS_OK);
    CHECK(read_is(DIR_W L"\\gptps.toml", "new"));
    CHECK(GetFileAttributesW(DIR_W L"\\gptps.toml.tmp") == INVALID_FILE_ATTRIBUTES);
    CHECK(w_flushes >= 1);                                 /* the data reached the disk first */
    CHECK(((DWORD)w_move_flags & MOVEFILE_WRITE_THROUGH) != 0);
    CHECK(((DWORD)w_move_flags & MOVEFILE_REPLACE_EXISTING) != 0);
    CHECK(gptps_hal_atomic_replace(DIR_U8 "\\no_such.tmp", DIR_U8 "\\gptps.toml") == GPTPS_E_IO);
    CHECK(read_is(DIR_W L"\\gptps.toml", "new"));          /* a failed save keeps the file */
#if defined(ADDON_PATH)
    {   /* an add-on under a non-ASCII path, named in UTF-8 as a TOML file names it */
        wchar_t src[MAX_PATH];
        gptps_dl *h;
        CHECK(MultiByteToWideChar(CP_UTF8, 0, ADDON_PATH, -1, src, MAX_PATH) > 0);
        CHECK(CopyFileW(src, DIR_W L"\\eklenti.dll", FALSE));
        h = gptps_dl_open(DIR_U8 "\\eklenti.dll");
        CHECK(h != NULL);
        if (h) { CHECK(gptps_dl_sym(h, "gptps_addon_init") != NULL); gptps_dl_close(h); }
        DeleteFileW(DIR_W L"\\eklenti.dll");
    }
#endif
    DeleteFileW(DIR_W L"\\gptps.toml");
    RemoveDirectoryW(DIR_W);
}

/* ---- 8) the output cap, named only when it is the cause ---------------- */
static void out_cap(void)
{
    static const char *const a_zeros[] = { HELPER_PATH, "zeros", "17000000", NULL };
    static const char *const a_late[]  = { HELPER_PATH, "lateflood", "1500", "17000000", NULL };
    gptps_exec_caps caps;
    /* past the cap on its own: E_IO, and the cap is named */
    CHECK(run(a_zeros, NULL, 0, 0, &caps, NULL, 0) == GPTPS_E_IO);
    CHECK(caps.out_capped != 0);
    /* past its deadline first, then too much output in its grace: a timeout, and the
     * cap is not what ended it. A service host, so the stop goes through a borrowed
     * console (faked: Wine cannot deliver Ctrl+Break) and the grace is given. */
    w_ev_attached = CreateEventA(NULL, TRUE, TRUE, NULL);
    w_ev_launched = CreateEventA(NULL, TRUE, TRUE, NULL);
    InterlockedExchange(&w_fake_console, 1);
    CHECK(run_tm(a_late, NULL, 4000, 0, 1, &caps, NULL, 0) == GPTPS_E_TIMEOUT);
    CHECK(caps.out_capped == 0);
    if (caps.out_capped) printf("a timed-out job's output past the cap was reported as the cause\n");
    InterlockedExchange(&w_fake_console, 0);
    CloseHandle(w_ev_attached); CloseHandle(w_ev_launched);
}

int main(void)
{
    gptps_set_log_sink(on_log, NULL);
    printf("(ANSI code page %u)\n", (unsigned)GetACP());
    cmdline();
    pipes_stay();
    console_probe();
    die_claim();
    thread_faults();
    cpu_groups();
    utf8_paths();
    out_cap();
    gptps_set_log_sink(NULL, NULL);
    if (fails) { printf("%d Windows executor fault check(s) FAILED\n", fails); return 1; }
    printf("Windows executor faults: all checks passed\n");
    return 0;
}
