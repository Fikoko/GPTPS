/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * wasm_exec.c - run WebAssembly modules as GPTPS tasks (see wasm_exec.h).
 *
 * Each registered module becomes a GPTPS task whose run-fn (wasm_dispatch) reads
 * the payload, calls the pluggable runtime, and hands the output back as the task
 * result. INPROC or OOP (forked + OS-capped) is chosen per registration. No core
 * ABI change - this is pure public-API glue.
 */
#define _POSIX_C_SOURCE 200809L
#include "wasm_exec.h"

#include <stdlib.h>
#include <string.h>

typedef struct wasm_task {
    gptps_wasm       *owner;
    char             *module_path;
    struct wasm_task *next;
} wasm_task;

struct gptps_wasm {
    gptps            *e;
    gptps_wasm_run_fn run;
    void             *user_data;
    wasm_task        *tasks;   /* per-registration bookkeeping, freed at close */
};

static char *dup_str(const char *s) { size_t n = strlen(s) + 1; char *o = (char *)malloc(n); if (o) memcpy(o, s, n); return o; }

/* The task body: payload -> wasm runtime -> result. Used for INPROC and (in a
 * forked child) OOP; both inherit `wt` and the runtime fn pointer. */
static gptps_status wasm_dispatch(gptps_ctx *ctx, void *ud)
{
    wasm_task *wt = (wasm_task *)ud;
    size_t plen = 0;
    const void *payload = gptps_payload(ctx, &plen);
    void *res = NULL;
    size_t rlen = 0;
    gptps_status st = wt->owner->run(wt->module_path, payload, plen, &res, &rlen, wt->owner->user_data);
    if (st == GPTPS_OK && res && rlen)
        return gptps_result_set_nocopy(ctx, res, rlen, free); /* engine takes ownership */
    free(res);
    return st;
}

gptps_wasm *gptps_wasm_install(gptps *e, gptps_wasm_run_fn run, void *user_data)
{
    gptps_wasm *w;
    if (!e || !run) return NULL;
    w = (gptps_wasm *)calloc(1, sizeof *w);
    if (!w) return NULL;
    w->e = e; w->run = run; w->user_data = user_data;
    return w;
}

gptps_status gptps_wasm_register(gptps_wasm *w, const char *name, const char *module_path,
                                 int oop, const gptps_cost *cost, const gptps_failure_policy *policy)
{
    wasm_task *wt;
    gptps_task_def d;
    gptps_status st;
    if (!w || !name || !module_path) return GPTPS_E_INVAL;

    wt = (wasm_task *)calloc(1, sizeof *wt);
    if (!wt) return GPTPS_E_NOMEM;
    wt->owner = w;
    wt->module_path = dup_str(module_path);
    if (!wt->module_path) { free(wt); return GPTPS_E_NOMEM; }

    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.name = name;
    d.run = wasm_dispatch;
    d.user_data = wt;
    d.exec = oop ? GPTPS_EXEC_OOP : GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    if (cost) d.default_cost = *cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    if (policy) d.default_policy = *policy;

    st = gptps_register_task(w->e, &d);   /* copies the def (incl. user_data ptr) */
    if (st != GPTPS_OK) { free(wt->module_path); free(wt); return st; }

    wt->next = w->tasks;   /* keep wt alive until close (the engine holds its pointer) */
    w->tasks = wt;
    return GPTPS_OK;
}

void gptps_wasm_close(gptps_wasm *w)
{
    wasm_task *t;
    if (!w) return;
    /* Caller contract: engine already shut down, so no task references wt anymore. */
    t = w->tasks;
    while (t) { wasm_task *n = t->next; free(t->module_path); free(t); t = n; }
    free(w);
}
