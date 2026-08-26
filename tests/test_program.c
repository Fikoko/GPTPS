/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_program.c - generalized exec executor (GPTPS_EXEC_PROGRAM): run an
 * arbitrary external program as a task. Payload -> stdin, stdout -> result,
 * exit code -> status, hard-killed on timeout. Language-agnostic by construction.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE          /* expose POSIX sigaction under -std=c99 */
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#  define _DARWIN_C_SOURCE
#endif
#include "gptps.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>            /* dup2 / close: cases I and J run with fd 0/1 free */
#include <fcntl.h>             /* fcntl(F_DUPFD, 3): park the saved stdio fds above fd 2 */

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int  g_finished, g_failed, g_timeout, g_rlen;
static char g_result[256];
static int  inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int  get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static void reset(void) { g_finished = g_failed = g_timeout = g_rlen = 0; g_result[0] = 0; }

static void on_ev(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_FINISHED) {
        inc(&g_finished);
        if (ev->result && ev->result_len < sizeof g_result) {
            memcpy(g_result, ev->result, ev->result_len);
            g_result[ev->result_len] = 0;
            __atomic_store_n(&g_rlen, (int)ev->result_len, __ATOMIC_SEQ_CST);
        }
    } else if (ev->kind == GPTPS_EV_FAILED) {
        inc(&g_failed);
        if (ev->status == GPTPS_E_TIMEOUT) inc(&g_timeout);
    }
}

/* ---- large-payload / cancel regression harness (cases E/F/G) --------------- *
 * Results here are far bigger than g_result, so track length + a rolling checksum
 * and a STARTED count instead of copying the bytes. */
#define BIG_LEN (256u * 1024u)   /* > any pipe buffer, so the old "write all stdin
                                  * then read stdout" path would deadlock */
static unsigned char   g_big[BIG_LEN];
static int             g_started, g_big_finished, g_big_timeout;
static unsigned long   g_big_sum; static size_t g_big_len;
static unsigned long   sum_bytes(const unsigned char *p, size_t n)
{ unsigned long s = 1469598103u; size_t i; for (i = 0; i < n; ++i) s = (s ^ p[i]) * 16777619u; return s; }
static void reset_big(void) { g_started = g_big_finished = g_big_timeout = 0; g_big_sum = 0; g_big_len = 0; }
static void on_ev_big(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_STARTED) inc(&g_started);
    else if (ev->kind == GPTPS_EV_FINISHED) {
        __atomic_store_n(&g_big_len, ev->result_len, __ATOMIC_SEQ_CST);
        __atomic_store_n(&g_big_sum, sum_bytes((const unsigned char *)ev->result, ev->result_len), __ATOMIC_SEQ_CST);
        inc(&g_big_finished);
    } else if (ev->kind == GPTPS_EV_FAILED && ev->status == GPTPS_E_TIMEOUT) inc(&g_big_timeout);
}
static void fill_big(void) { size_t i; for (i = 0; i < BIG_LEN; ++i) g_big[i] = (unsigned char)((i * 1103515245u + 12345u) >> 16); }
static int wait_started(unsigned ms) { uint64_t s = gptps_now_ms(NULL); while (get(&g_started) < 1 && gptps_now_ms(NULL) - s < ms) { } return get(&g_started) >= 1; }

static gptps *open_prog(const char *name, const char *const *argv, unsigned timeout_s)
{
    gptps *e = NULL;
    gptps_task_def d;
    if (gptps_open(NULL, &e) != GPTPS_OK) return NULL;
    gptps_set_event_cb(e, on_ev, NULL);
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.exec = GPTPS_EXEC_PROGRAM; d.argv = argv;
    d.default_cost.struct_size = sizeof d.default_cost;       /* mem 0 => no AS cap, programs run free */
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.timeout_seconds = timeout_s;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    return e;
}

int main(void)
{
    gptps *e;
    gptps_handle h;
    static const char *echo[]  = { "/bin/sh", "-c", "cat", (const char *)0 };
    static const char *upper[] = { "/bin/sh", "-c", "tr a-z A-Z", (const char *)0 };
    static const char *fail[]  = { "/bin/sh", "-c", "exit 7", (const char *)0 };
    static const char *hang[]  = { "/bin/sh", "-c", "sleep 10", (const char *)0 };
    static const char *inf[]   = { "/bin/sh", "-c", "sleep 100000", (const char *)0 };
    static const char *catp[]  = { "/bin/cat", (const char *)0 };   /* case I: no shell in between */

    /* register-time validation: PROGRAM needs an argv */
    {
        gptps_task_def bad; gptps *t = NULL;
        CHECK(gptps_open(NULL, &t) == GPTPS_OK);
        memset(&bad, 0, sizeof bad); bad.struct_size = sizeof bad; bad.name = "x"; bad.exec = GPTPS_EXEC_PROGRAM;
        CHECK(gptps_register_task(t, &bad) == GPTPS_E_INVAL); /* no argv */
        gptps_shutdown(t);
    }

    /* A) echo: payload -> stdin -> stdout -> result */
    reset();
    e = open_prog("echo", echo, 5); CHECK(e);
    CHECK(gptps_submit(e, "echo", "hello gptps", 11, &h) == GPTPS_OK);
    gptps_shutdown(e);
    CHECK(get(&g_finished) == 1);
    CHECK(get(&g_rlen) == 11);
    CHECK(strcmp(g_result, "hello gptps") == 0);

    /* B) arbitrary transform: uppercase via `tr` */
    reset();
    e = open_prog("upper", upper, 5); CHECK(e);
    CHECK(gptps_submit(e, "upper", "abc", 3, &h) == GPTPS_OK);
    gptps_shutdown(e);
    CHECK(get(&g_finished) == 1);
    CHECK(strcmp(g_result, "ABC") == 0);

    /* C) non-zero exit -> E_TASK */
    reset();
    e = open_prog("fail", fail, 5); CHECK(e);
    CHECK(gptps_submit(e, "fail", NULL, 0, &h) == GPTPS_OK);
    gptps_shutdown(e);
    CHECK(get(&g_failed) == 1);
    CHECK(get(&g_finished) == 0);

    /* D) hung program hard-killed on timeout -> E_TIMEOUT */
    reset();
    e = open_prog("hang", hang, 1); CHECK(e);
    CHECK(gptps_submit(e, "hang", NULL, 0, &h) == GPTPS_OK);
    gptps_shutdown(e);
    CHECK(get(&g_timeout) >= 1);
    CHECK(get(&g_finished) == 0);

    /* E) large streaming round-trip through `cat`: the deadlock regression - a
     * >pipe-buffer payload through a child that emits as it reads. Must complete
     * with every byte echoed back (checksum), not hang. */
    reset_big(); fill_big();
    e = open_prog("bigcat", echo, 10); CHECK(e);
    gptps_set_event_cb(e, on_ev_big, NULL);
    CHECK(gptps_submit(e, "bigcat", g_big, BIG_LEN, &h) == GPTPS_OK);
    gptps_shutdown(e);
    CHECK(get(&g_big_finished) == 1);
    CHECK(g_big_len == BIG_LEN);
    CHECK(g_big_sum == sum_bytes(g_big, BIG_LEN));

    /* F) large payload to a child that never reads stdin (`sleep`): the old code
     * blocked forever writing stdin BEFORE it ever enforced the deadline. Must
     * time out, not hang. */
    reset_big(); fill_big();
    e = open_prog("bighang", hang, 1); CHECK(e);
    gptps_set_event_cb(e, on_ev_big, NULL);
    CHECK(gptps_submit(e, "bighang", g_big, BIG_LEN, &h) == GPTPS_OK);
    gptps_shutdown(e);
    CHECK(get(&g_big_timeout) >= 1);

    /* G) cancel a NO-timeout program mid-flight: without cancel plumbing the parent
     * waited forever (poll timeout -1) and shutdown could never return. */
    reset_big();
    e = open_prog("nocancel", inf, 0); CHECK(e);   /* timeout 0 => no deadline */
    gptps_set_event_cb(e, on_ev_big, NULL);
    CHECK(gptps_submit(e, "nocancel", NULL, 0, &h) == GPTPS_OK);
    CHECK(wait_started(3000));
    CHECK(gptps_cancel(e, h) == GPTPS_OK);
    gptps_shutdown(e);                             /* returns => the cancel killed the child */

    /* H) the program executor must NOT mutate the host's process-wide SIGPIPE
     * disposition: it suppresses SIGPIPE per-thread / per-fd now, not with a global
     * signal(SIGPIPE, SIG_IGN). Set a known disposition, run a program task, verify
     * it is untouched (the old global-ignore code would flip it to SIG_IGN). */
    {
        struct sigaction want, got;
        memset(&want, 0, sizeof want); want.sa_handler = SIG_DFL; sigemptyset(&want.sa_mask);
        CHECK(sigaction(SIGPIPE, &want, NULL) == 0);
        reset();
        e = open_prog("dispo", echo, 5); CHECK(e);
        CHECK(gptps_submit(e, "dispo", "hi", 2, &h) == GPTPS_OK);
        gptps_shutdown(e);
        CHECK(sigaction(SIGPIPE, NULL, &got) == 0);
        CHECK(got.sa_handler == SIG_DFL);          /* not SIG_IGN => host disposition preserved */
    }

    /* I) the host daemonised - it closed fd 0 and/or fd 1 before submitting - so
     * pipe() hands the executor those very descriptors back. The child then did
     * dup2(inp[0], 0) with inp[0] == 0 (a no-op) and the blind close(inp[0]) right
     * after shut fd 0 outright: the program exec'd with NO stdin, read EOF at once
     * and the payload was silently dropped (FINISHED with an empty result). With
     * both 0 and 1 free it is worse - inp == {0,1}, so close(inp[1]) closes the
     * stdout that was just dup2'd onto fd 1. The fix hoists every pipe end above
     * fd 2 before any dup2; assert the payload round-trips in all three shapes.
     *
     * Every descriptor is restored before a single CHECK runs: with fd 1 closed
     * printf() writes into a pipe the executor owns, so a failure reported inside
     * the window would be invisible (or corrupt the child's stdout stream). */
    {
        const char *msg = "daemonised-payload";
        int mode;
        for (mode = 0; mode < 3; ++mode) {   /* 0: stdin free, 1: stdout free, 2: both */
            int save0 = -1, save1 = -1, sub, fin, rl;
            reset();
            e = open_prog("catio", catp, 5); /* opened BEFORE the close, so the engine's
                                              * own descriptors are already allocated and
                                              * only the executor's pipes land low */
            CHECK(e);
            if (!e) continue;
            /* Park both copies above fd 2 BEFORE freeing anything: a plain dup() of
             * stdout with fd 0 already closed would hand back fd 0 itself, and
             * restoring stdin would then clobber the only copy of stdout. */
            if (mode != 1) save0 = fcntl(STDIN_FILENO,  F_DUPFD, 3);
            if (mode != 0) save1 = fcntl(STDOUT_FILENO, F_DUPFD, 3);
            if (mode != 1) close(STDIN_FILENO);
            if (mode != 0) close(STDOUT_FILENO);
            sub = gptps_submit(e, "catio", msg, strlen(msg), &h);
            gptps_shutdown(e);               /* blocks until the task is terminal */
            fin = get(&g_finished); rl = get(&g_rlen);
            if (save0 >= 0) { dup2(save0, STDIN_FILENO);  close(save0); }
            if (save1 >= 0) { dup2(save1, STDOUT_FILENO); close(save1); }
            CHECK(sub == GPTPS_OK);
            CHECK(fin == 1);
            CHECK(rl == (int)strlen(msg));   /* 0 => the payload never reached the program */
            CHECK(strcmp(g_result, msg) == 0);
        }
    }

    /* J) a host that runs signal(SIGCHLD, SIG_IGN) - the standard daemon idiom -
     * has the kernel auto-reap our children, so the executor's waitpid() fails with
     * ECHILD and never fills wstatus. The old code read the zero initialiser:
     * WIFEXITED(0) is true with WEXITSTATUS(0) == 0, so a program that really
     * exited 7 was reported as GPTPS_OK - inverting the "non-zero exit => E_TASK"
     * contract and silently skipping retries and dead-lettering. An unknowable exit
     * status is now E_TASK. Note the documented consequence (include/gptps.h): on a
     * SIG_IGN host EVERY program task reports E_TASK, successes included - so this
     * asserts "not reported as success", not any particular success. */
    {
        struct sigaction ign, old_chld;
        int fin, fail_n;
        memset(&ign, 0, sizeof ign); ign.sa_handler = SIG_IGN; sigemptyset(&ign.sa_mask);
        CHECK(sigaction(SIGCHLD, &ign, &old_chld) == 0);
        reset();
        e = open_prog("nochld", fail, 5); CHECK(e);   /* `fail` really exits 7 */
        CHECK(gptps_submit(e, "nochld", NULL, 0, &h) == GPTPS_OK);
        gptps_shutdown(e);
        fin = get(&g_finished); fail_n = get(&g_failed);
        CHECK(sigaction(SIGCHLD, &old_chld, NULL) == 0);  /* leave the host's disposition as found */
        CHECK(fin == 0);                                  /* was 1: "child exited 0" */
        CHECK(fail_n >= 1);
    }

    if (fails) { printf("%d program check(s) FAILED\n", fails); return 1; }
    printf("all program-executor checks passed\n");
    return 0;
}
