/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * config_file.c - configure an engine from a TOML file (no recompile to re-tune).
 *
 * gptps_open(path) reads [limits], [scheduler], [task_defaults]/[tasks.<name>]
 * (per-task policy + priority), and a top-level addons list. Here we write a
 * small config, open from it, register two task types, and submit work. The
 * tasks' retry policy and scheduling priority come entirely from the file - the
 * code below sets none of it, so re-tuning is just an edit to the .toml.
 *
 *   cc config_file.c gptps.c -lpthread -ldl   (amalgamation)
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

#define CFG_PATH "gptps_example.toml"

static int g_finished = 0;   /* FINISHED fires on worker threads: count atomically */
static gptps_status noop(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_OK; }

static void on_event(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_FINISHED) __atomic_add_fetch(&g_finished, 1, __ATOMIC_SEQ_CST);
}

static void reg(gptps *e, const char *name)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = noop; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = 4096;
    d.default_policy.struct_size = sizeof d.default_policy;
    gptps_register_task(e, &d);   /* priority/policy are layered on from the file */
}

int main(void)
{
    gptps *engine;
    gptps_handle h;
    int i;
    FILE *f;

    /* write a config file (you'd normally ship this alongside your app) */
    f = fopen(CFG_PATH, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", CFG_PATH); return 1; }
    fputs("[limits]\n"
          "max_concurrent_tasks = 2\n"
          "max_memory_gb        = 1.0\n"
          "\n"
          "[scheduler]\n"
          "reserve_after_skips = 8\n"
          "\n"
          "[task_defaults]\n"
          "max_retries = 1\n"
          "on_failure  = \"dead_letter\"\n"
          "\n"
          "[tasks.urgent]\n"
          "priority = 10        # admitted ahead of lower-priority work when both wait\n"
          "\n"
          "[tasks.bulk]\n"
          "priority = 1\n", f);
    fclose(f);

    if (gptps_open(CFG_PATH, &engine) != GPTPS_OK) { fprintf(stderr, "open(%s) failed\n", CFG_PATH); return 1; }
    printf("GPTPS config demo: engine configured from %s\n", CFG_PATH);
    gptps_set_event_cb(engine, on_event, NULL);

    reg(engine, "urgent");
    reg(engine, "bulk");

    /* The engine now runs under the file's limits, and "urgent" outranks "bulk"
     * at admission - all without a single tuning value in this source file. */
    for (i = 0; i < 3; ++i) {
        gptps_submit(engine, "bulk",   NULL, 0, &h);
        gptps_submit(engine, "urgent", NULL, 0, &h);
    }

    gptps_shutdown(engine);
    remove(CFG_PATH);
    {
        int done = __atomic_load_n(&g_finished, __ATOMIC_SEQ_CST);
        printf("processed %d tasks under config from file.\n", done);
        return (done == 6) ? 0 : 1;
    }
}
