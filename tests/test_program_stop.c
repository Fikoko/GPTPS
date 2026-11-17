/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_program_stop.c - how GPTPS stops a GPTPS_EXEC_PROGRAM job, and what is left to
 * say about one that failed (ABI 2.7 / 2.8), on POSIX and Windows.
 *
 *  1) stop_grace_ms on every stop GPTPS decides - a timeout, gptps_cancel, and
 *     gptps_shutdown - gives the program its stop signal (SIGTERM; Ctrl+Break on
 *     Windows) and time to clean up. Without a grace there is none.
 *  2) The grace is the WHOLE job's: a launcher that dies at once does not cost its
 *     worker its clean-up.
 *  3) A program that writes a lot while cleaning up is still read, so it cannot block.
 *  4) Windows: all of it with and without a console in the host - a service has none.
 *  5) A dead letter carries the last attempt's measurements: exit.code, start.error.
 *  6) GPTPS_TASK_DIE_WITH_HOST: a host that dies takes its jobs with it - the WHOLE
 *     job, a launcher's workers too (every POSIX system; Windows always); without
 *     the flag a POSIX job outlives it; a job that ends leaves no watcher behind;
 *     where it cannot be enforced (an OOP job off Linux / FreeBSD), one WARN per
 *     task type.
 *
 * A threaded engine: the stops come from another thread, as they do in a host.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "gptps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#  include <windows.h>
#else
#  include <signal.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

#ifndef HELPER_PATH
#define HELPER_PATH "./prog_helper"
#endif

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int  inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int  get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static void put(int *p, int v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }

static void sleep_ms(unsigned ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u); ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) != 0) { }
#endif
}

#define CLEAN_FILE "gptps_stop_cleaned.txt"
#define PID_FILE   "gptps_stop_pid.txt"

static int g_started, g_done, g_status;
static void on_ev(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_STARTED) inc(&g_started);
    if (ev->kind == GPTPS_EV_FINISHED || ev->kind == GPTPS_EV_FAILED) { put(&g_status, (int)ev->status); inc(&g_done); }
}

static int g_warn_die, g_warn_grace;
static void on_log(gptps_log_level lvl, const char *msg, void *ud)
{
    (void)ud;
    if (lvl != GPTPS_LOG_WARN || !msg) return;
    if (strstr(msg, "DIE_WITH_HOST")) inc(&g_warn_die);
    if (strstr(msg, "stop_grace_ms")) inc(&g_warn_grace);
}

static int wait_for(int *p, int want, unsigned ms)
{
    unsigned waited = 0;
    while (get(p) < want && waited < ms) { sleep_ms(10); waited += 10; }
    return get(p) >= want;
}

static int cleaned(void)
{
    char got[16] = { 0 };
    FILE *f = fopen(CLEAN_FILE, "rb");
    if (!f) return 0;
    if (fread(got, 1, sizeof got - 1, f) == 0) got[0] = 0;
    fclose(f);
    return strcmp(got, "cleaned") == 0;
}

static gptps *open_engine(uint32_t shutdown_grace_ms)
{
    gptps_config cfg; gptps *e = NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 2;
    cfg.shutdown_grace_ms = shutdown_grace_ms;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (e) CHECK(gptps_set_event_cb(e, on_ev, NULL) == GPTPS_OK);
    return e;
}

static void reg(gptps *e, const char *name, const char *const *argv, uint32_t timeout_s,
                uint32_t grace_ms, uint64_t flags)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.exec = GPTPS_EXEC_PROGRAM; d.argv = argv;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.timeout_seconds = timeout_s;
    d.stop_grace_ms = grace_ms;
    d.flags = flags;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

enum { BY_TIMEOUT, BY_CANCEL, BY_SHUTDOWN };

/* Run `argv` and stop it `how`; 1 if it cleaned up */
static int stop_case(const char *const *argv, int how, uint32_t grace_ms)
{
    gptps *e = open_engine(200);
    gptps_handle h = 0;
    int did;
    if (!e) return 0;
    remove(CLEAN_FILE);
    put(&g_started, 0); put(&g_done, 0); put(&g_status, 0);
    reg(e, "job", argv, how == BY_TIMEOUT ? 1u : 0u, grace_ms, 0);
    CHECK(gptps_submit(e, "job", NULL, 0, &h) == GPTPS_OK);
    CHECK(wait_for(&g_started, 1, 10000));
    sleep_ms(400);                              /* its stop handler is in place */
    if (how == BY_CANCEL) CHECK(gptps_cancel(e, h) == GPTPS_OK);
    if (how != BY_SHUTDOWN) {
        CHECK(wait_for(&g_done, 1, 15000));
        CHECK(get(&g_status) == (how == BY_TIMEOUT ? (int)GPTPS_E_TIMEOUT : (int)GPTPS_E_CANCELLED));
    }
    CHECK(gptps_shutdown(e) == GPTPS_OK);       /* BY_SHUTDOWN: the grace runs out, then the stop */
    CHECK(get(&g_done) == 1);
    did = cleaned();
    remove(CLEAN_FILE);
    return did;
}

#if defined(_WIN32)
#  define STOP_MODE "brk"
#else
#  define STOP_MODE "term"
#endif

static void stops(int check_delivery)
{
    static const char *const a_clean[]    = { HELPER_PATH, STOP_MODE, CLEAN_FILE, NULL };
    static const char *const a_launcher[] = { HELPER_PATH, "spawn", STOP_MODE, CLEAN_FILE, "400", NULL };
    static const char *const a_noisy[]    = { HELPER_PATH, STOP_MODE, CLEAN_FILE, "0", "300000", NULL };
    int r;
    /* 1) every stop GPTPS decides */
    r = stop_case(a_clean, BY_TIMEOUT, 5000);  if (check_delivery) CHECK(r);
    r = stop_case(a_clean, BY_CANCEL, 5000);   if (check_delivery) CHECK(r);
    r = stop_case(a_clean, BY_SHUTDOWN, 5000); if (check_delivery) CHECK(r);
    /* 2) the whole job: the launcher dies at once, its worker takes 400 ms */
    r = stop_case(a_launcher, BY_CANCEL, 5000); if (check_delivery) CHECK(r);
    /* 3) 300 000 bytes on stdout while cleaning up - more than any pipe holds */
    r = stop_case(a_noisy, BY_CANCEL, 5000);    if (check_delivery) CHECK(r);
    /* 1) no grace: no clean-up */
    CHECK(!stop_case(a_clean, BY_CANCEL, 0));
}

/* 5) the dead letter says why */
static const gptps_measure *dl_m; static size_t dl_n; static int dl_seen;
static uint64_t dl_code, dl_start; static int dl_has_code, dl_has_start;
static void on_dl(const gptps_dead_letter *dl, void *ud)
{
    size_t i;
    (void)ud;
    dl_seen += 1; dl_m = dl->measures; dl_n = dl->n_measures;
    for (i = 0; i < dl->n_measures; ++i) {
        if (strcmp(dl->measures[i].name, GPTPS_M_EXIT_CODE) == 0)   { dl_has_code = 1; dl_code = dl->measures[i].value; }
        if (strcmp(dl->measures[i].name, GPTPS_M_START_ERROR) == 0) { dl_has_start = 1; dl_start = dl->measures[i].value; }
    }
}
static void dead_letters(void)
{
    static const char *const a_exit3[] = { HELPER_PATH, "exit", "3", NULL };
    static const char *const a_none[]  = { "gptps_stop_no_such_program_xyz", NULL };
    gptps *e = open_engine(0);
    if (!e) return;
    reg(e, "three", a_exit3, 0, 0, 0);
    reg(e, "none", a_none, 0, 0, 0);
    put(&g_done, 0);
    CHECK(gptps_submit(e, "three", NULL, 0, NULL) == GPTPS_OK);
    CHECK(wait_for(&g_done, 1, 10000));
    dl_seen = 0; dl_has_code = dl_has_start = 0;
    CHECK(gptps_dead_letter_drain(e, on_dl, NULL) == 1);
    CHECK(dl_has_code && dl_code == 3 && !dl_has_start && dl_n > 0);
    CHECK(gptps_submit(e, "none", NULL, 0, NULL) == GPTPS_OK);
    CHECK(wait_for(&g_done, 2, 10000));
    dl_seen = 0; dl_has_code = dl_has_start = 0;
    CHECK(gptps_dead_letter_drain(e, on_dl, NULL) == 1);
    CHECK(dl_has_start && dl_start != 0 && !dl_has_code);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

/* 6) a host that dies */
#if !defined(_WIN32)
static long read_pid(void)
{
    long pid = 0;
    FILE *f = fopen(PID_FILE, "r");
    if (!f) return 0;
    if (fscanf(f, "%ld", &pid) != 1) pid = 0;
    fclose(f);
    return pid;
}
/* A host that starts a job and is then killed outright. Returns the pid the job
 * wrote: its leader's, or with `launcher` the pid of the worker the leader started. */
static long crash_host(int die_flag, int launcher)
{
    pid_t host;
    int st;
    remove(PID_FILE);
    host = fork();
    if (host < 0) return 0;
    if (host == 0) {
        static const char *const a_pid[] = { HELPER_PATH, "pidsleep", PID_FILE, NULL };
        static const char *const a_spawn[] = { HELPER_PATH, "spawn", "pidsleep", PID_FILE, NULL };
        gptps *e = open_engine(0);
        if (!e) _exit(2);
        reg(e, "victim", launcher ? a_spawn : a_pid, 0, 0, die_flag ? GPTPS_TASK_DIE_WITH_HOST : 0u);
        if (gptps_submit(e, "victim", NULL, 0, NULL) != GPTPS_OK) _exit(3);
        { int i; for (i = 0; i < 1000 && !read_pid(); ++i) sleep_ms(10); }
        kill(getpid(), SIGKILL);                 /* a crash: no shutdown, no cleanup */
        _exit(4);
    }
    while (waitpid(host, &st, 0) < 0) { }
    return read_pid();
}
static int alive_after(long pid, unsigned ms)
{
    unsigned waited = 0;
    while (waited < ms) {
        if (kill((pid_t)pid, 0) != 0) return 0;
        sleep_ms(20); waited += 20;
    }
    return 1;
}
#endif

#if !defined(_WIN32) && !defined(__linux__) && !defined(__FreeBSD__)
static gptps_status oop_ok(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }
#endif

static void die_with_host(void)
{
#if !defined(_WIN32)
    long pid;
    /* without the flag the job outlives its host - which some hosts want */
    pid = crash_host(0, 0);
    CHECK(pid > 0);
    if (pid > 0) { CHECK(alive_after(pid, 500)); kill((pid_t)pid, SIGKILL); }
    pid = crash_host(1, 0);
    CHECK(pid > 0);
    if (pid > 0) {
        int alive = alive_after(pid, 3000);
        CHECK(!alive);                           /* killed with its host */
        if (alive) kill((pid_t)pid, SIGKILL);
    }
    /* the whole job: a launcher's worker - which a parent-death signal, cleared at
     * fork, never reaches - goes with the host too */
    pid = crash_host(1, 1);
    CHECK(pid > 0);
    if (pid > 0) {
        int alive = alive_after(pid, 3000);
        CHECK(!alive);
        if (alive) { pid_t g = getpgid((pid_t)pid); if (g > 1) kill(-g, SIGKILL); }
    }
#endif
    {   /* where it cannot be enforced, the first attempt of the type says so - once */
        static const char *const a_exit0[] = { HELPER_PATH, "exit", "0", NULL };
        gptps *e = open_engine(0);
        if (!e) return;
        put(&g_warn_die, 0); put(&g_done, 0);
        reg(e, "die", a_exit0, 0, 0, GPTPS_TASK_DIE_WITH_HOST);
        CHECK(gptps_submit(e, "die", NULL, 0, NULL) == GPTPS_OK);
        CHECK(gptps_submit(e, "die", NULL, 0, NULL) == GPTPS_OK);
        CHECK(wait_for(&g_done, 2, 10000));
        CHECK(gptps_shutdown(e) == GPTPS_OK);
        CHECK(get(&g_warn_die) == 0);            /* PROGRAM: enforced everywhere */
#if !defined(_WIN32)
        {   /* and the jobs' watchers are gone with them: this process has no child left */
            int st;
            CHECK(waitpid(-1, &st, WNOHANG) < 0);
        }
#endif
    }
#if !defined(_WIN32) && !defined(__linux__) && !defined(__FreeBSD__)
    {   /* an OOP job is the host's own forked copy: only a parent-death signal holds it */
        gptps *e = open_engine(0);
        gptps_task_def d;
        if (!e) return;
        put(&g_warn_die, 0); put(&g_done, 0);
        memset(&d, 0, sizeof d);
        d.struct_size = sizeof d; d.name = "dieoop"; d.exec = GPTPS_EXEC_OOP; d.run = oop_ok;
        d.default_cost.struct_size = sizeof d.default_cost;
        d.default_policy.struct_size = sizeof d.default_policy;
        d.flags = GPTPS_TASK_DIE_WITH_HOST;
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        CHECK(gptps_submit(e, "dieoop", NULL, 0, NULL) == GPTPS_OK);
        CHECK(gptps_submit(e, "dieoop", NULL, 0, NULL) == GPTPS_OK);
        CHECK(wait_for(&g_done, 2, 10000));
        CHECK(gptps_shutdown(e) == GPTPS_OK);
        CHECK(get(&g_warn_die) == 1);
    }
#endif
}

int main(void)
{
    gptps_set_log_sink(on_log, NULL);
#if defined(_WIN32)
    {
        /* Wine cannot deliver Ctrl+Break to a process group: there only the rest is checked */
        int wine = GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version") != NULL;
        if (wine) printf("(Wine: stop-signal delivery not checked)\n");
        /* 4) a host with no console - a service - and then one with a console */
        FreeConsole();
        put(&g_warn_grace, 0);
        stops(!wine);
        CHECK(get(&g_warn_grace) == 0);          /* the grace was given, not refused */
        CHECK(AllocConsole());
        if (GetConsoleWindow()) ShowWindow(GetConsoleWindow(), SW_HIDE);
        stops(!wine);
        CHECK(get(&g_warn_grace) == 0);
        FreeConsole();
    }
#else
    stops(1);
#endif
    dead_letters();
    die_with_host();
    gptps_set_log_sink(NULL, NULL);
    remove(CLEAN_FILE); remove(PID_FILE);
    if (fails) { printf("%d program-stop check(s) FAILED\n", fails); return 1; }
    printf("program stop: all checks passed\n");
    return 0;
}
