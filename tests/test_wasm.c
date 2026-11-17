/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_wasm.c - WASM executor add-on, exercised with a MOCK runtime so the
 * GPTPS-side integration (module-as-task: payload in, result out, INPROC + OOP,
 * error propagation, module-path plumbing) is verified end-to-end with no real
 * wasm interpreter. A real runtime (wasm3/wasmtime) drops into the same hook.
 *
 * The mock "runs a module" by upper-casing the payload, and fails for any module
 * whose path contains "bad".
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L   /* nanosleep under -std=c99 */
#endif
#include "gptps.h"
#include "gptps_wasm_exec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#  include <windows.h>
static void nap(void) { Sleep(1); }
#else
#  include <time.h>
static void nap(void) { struct timespec ts = { 0, 1000000L }; nanosleep(&ts, NULL); }
#endif

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }

/* --- mock wasm runtime ---
 * Keys behaviour on the module path: any module containing "bad" fails, others
 * upper-case the payload. So distinct per-task results prove each task run
 * received its own module_path (no separate global needed). */
static gptps_status mock_wasm(const char *module, const void *payload, size_t len,
                              void **out_result, size_t *out_len, void *ud)
{
    char *r; size_t i;
    (void)ud;
    if (strstr(module, "bad")) return GPTPS_E_TASK;    /* simulate a trap/failure */
    r = (char *)malloc(len ? len : 1);
    if (!r) return GPTPS_E_NOMEM;
    for (i = 0; i < len; ++i) {
        unsigned char c = ((const unsigned char *)payload)[i];
        r[i] = (char)((c >= 'a' && c <= 'z') ? c - 32 : c);
    }
    *out_result = r; *out_len = len;
    return GPTPS_OK;
}

/* --- the ctx-aware runtime (gptps_wasm_install_ex) ---
 * A module containing "spin" never returns by itself: like a runaway wasm loop that
 * checks for interruption between slices, it only stops when the ctx is cancelled.
 * With the ctx-less callback there would be nothing to poll, and cancel would hang. */
static int g_spin_started, g_spin_saw_cancel;
static gptps_status mock_wasm_ex(gptps_ctx *ctx, const char *module, const void *payload,
                                 size_t len, void **out_result, size_t *out_len, void *ud)
{
    if (strstr(module, "spin")) {
        unsigned long i;
        inc(&g_spin_started);
        for (i = 0; i < 20000ul; ++i) {      /* ~20 s ceiling: a hang fails, not blocks */
            if (gptps_is_cancelled(ctx)) { inc(&g_spin_saw_cancel); return GPTPS_E_CANCELLED; }
            nap();
        }
        return GPTPS_OK;
    }
    return mock_wasm(module, payload, len, out_result, out_len, ud);
}

/* --- capture results by task name --- */
static char g_up[64];   static int g_up_len,  g_up_done;
static char g_oop[64];  static int g_oop_len, g_oop_done;
static int  g_bad_failed;
static int  g_ex_done, g_spin_cancelled;

static void on_event(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_FINISHED) {
        if (strcmp(ev->task_name, "up") == 0 && ev->result_len <= sizeof g_up) {
            memcpy(g_up, ev->result, ev->result_len); g_up_len = (int)ev->result_len; inc(&g_up_done);
        } else if (strcmp(ev->task_name, "up_oop") == 0 && ev->result_len <= sizeof g_oop) {
            memcpy(g_oop, ev->result, ev->result_len); g_oop_len = (int)ev->result_len; inc(&g_oop_done);
        }
        else if (strcmp(ev->task_name, "up_ex") == 0 && ev->result_len == 2 &&
                 memcmp(ev->result, "OK", 2) == 0)
            inc(&g_ex_done);
    } else if (ev->kind == GPTPS_EV_FAILED && strcmp(ev->task_name, "bad") == 0) {
        inc(&g_bad_failed);
    } else if (ev->kind == GPTPS_EV_FAILED && strcmp(ev->task_name, "spin") == 0 &&
               ev->status == GPTPS_E_CANCELLED) {
        inc(&g_spin_cancelled);
    }
}

int main(void)
{
    gptps_config cfg;
    gptps *e = NULL;
    gptps_wasm *w;
    gptps_handle h;

    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 2; cfg.limits.max_memory_bytes = 64u << 20;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (!e) return 1;
    gptps_set_event_cb(e, on_event, NULL);

    w = gptps_wasm_install(e, mock_wasm, NULL);
    CHECK(w != NULL);
    CHECK(gptps_wasm_install(e, NULL, NULL) == NULL);   /* NULL runtime rejected */
    if (!w) { gptps_shutdown(e); return 1; }

    CHECK(gptps_wasm_register(w, "up",     "/modules/upper.wasm", 0, NULL, NULL) == GPTPS_OK);
    CHECK(gptps_wasm_register(w, "bad",    "/modules/bad.wasm",   0, NULL, NULL) == GPTPS_OK);

    CHECK(gptps_submit(e, "up",     "hello world", 11, &h) == GPTPS_OK);
    CHECK(gptps_submit(e, "bad",    "x",            1, &h) == GPTPS_OK);
#if !defined(_WIN32)
    /* OOP mode forks (POSIX-only); INPROC + error paths are portable */
    CHECK(gptps_wasm_register(w, "up_oop", "/modules/upper.wasm", 1, NULL, NULL) == GPTPS_OK);
    CHECK(gptps_submit(e, "up_oop", "abc123",       6, &h) == GPTPS_OK);
#endif

    gptps_shutdown(e);   /* drains; joins threads => results are visible */

    CHECK(get(&g_up_done) == 1);
    CHECK(g_up_len == 11 && memcmp(g_up, "HELLO WORLD", 11) == 0);     /* INPROC module ran */
#if !defined(_WIN32)
    CHECK(get(&g_oop_done) == 1);
    CHECK(g_oop_len == 6 && memcmp(g_oop, "ABC123", 6) == 0);          /* OOP (forked) module ran */
#endif
    CHECK(get(&g_bad_failed) == 1);  /* the "bad" module path failed while "upper" succeeded:
                                        per-task module_path plumbing + error propagation proven */

    gptps_wasm_close(w);   /* after shutdown */

    /* In-process cancel reaches a module only through gptps_wasm_install_ex's ctx. */
    {
        gptps *e2 = NULL;
        gptps_wasm *w2;
        unsigned long waited;
        CHECK(gptps_open_ex(&cfg, &e2) == GPTPS_OK);
        if (!e2) return 1;
        gptps_set_event_cb(e2, on_event, NULL);
        CHECK(gptps_wasm_install_ex(e2, NULL, NULL) == NULL);
        w2 = gptps_wasm_install_ex(e2, mock_wasm_ex, NULL);
        CHECK(w2 != NULL);
        if (!w2) { gptps_shutdown(e2); return 1; }
        CHECK(gptps_wasm_register(w2, "up_ex", "/modules/upper.wasm", 0, NULL, NULL) == GPTPS_OK);
        CHECK(gptps_wasm_register(w2, "spin",  "/modules/spin.wasm",  0, NULL, NULL) == GPTPS_OK);
        CHECK(gptps_submit(e2, "up_ex", "ok", 2, &h) == GPTPS_OK);
        CHECK(gptps_submit(e2, "spin", "x", 1, &h) == GPTPS_OK);
        for (waited = 0; !get(&g_spin_started) && waited < 10000ul; ++waited) nap();
        CHECK(get(&g_spin_started) == 1);
        CHECK(gptps_cancel(e2, h) == GPTPS_OK);
        for (waited = 0; !get(&g_spin_cancelled) && waited < 10000ul; ++waited) nap();
        CHECK(get(&g_spin_saw_cancel) == 1);   /* the runtime saw the cancel... */
        CHECK(get(&g_spin_cancelled) == 1);    /* ...and the item ended CANCELLED, promptly */
        gptps_shutdown(e2);
        CHECK(get(&g_ex_done) == 1);           /* the ctx-aware path still runs modules */
        gptps_wasm_close(w2);
    }
    if (fails) { printf("%d wasm check(s) FAILED\n", fails); return 1; }
    printf("all wasm-executor checks passed\n");
    return 0;
}
