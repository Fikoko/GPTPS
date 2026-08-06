/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_program_helper.c - GPTPS_EXEC_PROGRAM via a built helper binary, so the
 * external-program executor is verified on every platform (POSIX exec_oop_posix.c
 * AND the Win32 exec_win.c: CreateProcess + Job Object). Mirrors the four core
 * scenarios: echo, transform, non-zero exit, and timeout hard-kill.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

#ifndef HELPER_PATH
#define HELPER_PATH "./prog_helper"
#endif

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

/* ---- large-payload / cancel regression harness (cases E/F/G) --------------- */
#define BIG_LEN (256u * 1024u)   /* > any pipe buffer */
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
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.timeout_seconds = timeout_s;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    return e;
}

int main(void)
{
    gptps *e;
    gptps_handle h;
    static const char *echo[]  = { HELPER_PATH, "cat",   (const char *)0 };
    static const char *upper[] = { HELPER_PATH, "upper", (const char *)0 };
    static const char *failp[] = { HELPER_PATH, "exit", "7", (const char *)0 };
    static const char *hang[]  = { HELPER_PATH, "hang",  (const char *)0 };

    /* A) echo: payload -> stdin -> stdout -> result */
    reset();
    e = open_prog("echo", echo, 5); CHECK(e);
    CHECK(gptps_submit(e, "echo", "hello gptps", 11, &h) == GPTPS_OK);
    gptps_shutdown(e);
    CHECK(get(&g_finished) == 1);
    CHECK(get(&g_rlen) == 11);
    CHECK(strcmp(g_result, "hello gptps") == 0);

    /* B) transform: uppercase */
    reset();
    e = open_prog("upper", upper, 5); CHECK(e);
    CHECK(gptps_submit(e, "upper", "abc", 3, &h) == GPTPS_OK);
    gptps_shutdown(e);
    CHECK(get(&g_finished) == 1);
    CHECK(strcmp(g_result, "ABC") == 0);

    /* C) non-zero exit -> E_TASK (failed, not finished) */
    reset();
    e = open_prog("fail", failp, 5); CHECK(e);
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

    /* E) large streaming round-trip through the helper `cat`: deadlock regression
     * (POSIX single-thread pump; on Windows the writer/reader threads already avoid
     * it, but this still exercises the >pipe-buffer path). */
    reset_big(); fill_big();
    e = open_prog("bigcat", echo, 10); CHECK(e);
    gptps_set_event_cb(e, on_ev_big, NULL);
    CHECK(gptps_submit(e, "bigcat", g_big, BIG_LEN, &h) == GPTPS_OK);
    gptps_shutdown(e);
    CHECK(get(&g_big_finished) == 1);
    CHECK(g_big_len == BIG_LEN);
    CHECK(g_big_sum == sum_bytes(g_big, BIG_LEN));

    /* F) large payload to a stdin-ignoring child (`hang`): must time out, not hang. */
    reset_big(); fill_big();
    e = open_prog("bighang", hang, 1); CHECK(e);
    gptps_set_event_cb(e, on_ev_big, NULL);
    CHECK(gptps_submit(e, "bighang", g_big, BIG_LEN, &h) == GPTPS_OK);
    gptps_shutdown(e);
    CHECK(get(&g_big_timeout) >= 1);

    /* G) cancel a NO-timeout program mid-flight: shutdown must return (the cancel
     * flag hard-kills the child; validates the Win32 bounded-wait path too). */
    reset_big();
    e = open_prog("nocancel", hang, 0); CHECK(e);   /* timeout 0 => no deadline */
    gptps_set_event_cb(e, on_ev_big, NULL);
    CHECK(gptps_submit(e, "nocancel", NULL, 0, &h) == GPTPS_OK);
    CHECK(wait_started(3000));
    CHECK(gptps_cancel(e, h) == GPTPS_OK);
    gptps_shutdown(e);

    if (fails) { printf("%d program-helper check(s) FAILED\n", fails); return 1; }
    printf("all program-helper checks passed\n");
    return 0;
}
