/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * demo.c - the GPTPS headline use case: add a resource-aware, recoverable task
 * runner to your own program by linking libgptps. ~30 lines of real usage.
 *
 *   cc demo.c gptps.c -lpthread -ldl   (amalgamation)
 *   or link against the built libgptps.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

/* A task: sum the payload bytes and return the sum as the result. Polls the
 * cancel flag so an in-process timeout can stop it cooperatively. */
static gptps_status sum_task(gptps_ctx *ctx, void *user_data)
{
    size_t n, i;
    const unsigned char *p = (const unsigned char *)gptps_payload(ctx, &n);
    unsigned long sum = 0;
    (void)user_data;
    for (i = 0; i < n; ++i) {
        if (gptps_is_cancelled(ctx)) return GPTPS_E_CANCELLED;
        sum += p[i];
    }
    return gptps_result_set(ctx, &sum, sizeof sum);
}

static void on_event(const gptps_event *ev, void *user_data)
{
    const char *k;
    (void)user_data;
    switch (ev->kind) {
        case GPTPS_EV_QUEUED:        k = "queued";        break;
        case GPTPS_EV_STARTED:       k = "started";       break;
        case GPTPS_EV_FINISHED:      k = "finished";      break;
        case GPTPS_EV_FAILED:        k = "failed";        break;
        case GPTPS_EV_RETRIED:       k = "retried";       break;
        case GPTPS_EV_DEAD_LETTERED: k = "dead-lettered"; break;
        default:                     k = "?";             break;
    }
    printf("  [event] %-13s task=%s handle=%llu status=%s\n",
           k, ev->task_name, (unsigned long long)ev->handle, gptps_strerror(ev->status));
}

int main(void)
{
    static const char *inputs[] = { "hello", "gptps", "embeddable", "task", "processor" };
    const int N = (int)(sizeof inputs / sizeof inputs[0]);
    gptps *engine;
    gptps_task_def def;
    gptps_handle h;
    int i;

    /* 1. open an engine (auto-tunes worker count + memory budget to the host) */
    if (gptps_open(NULL, &engine) != GPTPS_OK) { fprintf(stderr, "gptps_open failed\n"); return 1; }
    gptps_set_event_cb(engine, on_event, NULL);

    /* 2. register a task type, with a cost declaration + failure policy */
    memset(&def, 0, sizeof def);
    def.struct_size = sizeof def;
    def.name = "sum";
    def.run  = sum_task;
    def.exec = GPTPS_EXEC_INPROC;
    def.default_cost.struct_size = sizeof def.default_cost;
    def.default_cost.mem_bytes = 4096;
    def.default_policy.struct_size = sizeof def.default_policy;
    def.default_policy.timeout_seconds = 5;
    def.default_policy.max_retries = 2;
    gptps_register_task(engine, &def);

    /* 3. submit work; it runs on the pool under the memory budget */
    printf("GPTPS demo: submitting %d tasks to libgptps...\n", N);
    for (i = 0; i < N; ++i)
        gptps_submit(engine, "sum", inputs[i], strlen(inputs[i]), &h);

    /* 4. shutdown drains all in-flight + queued work, then returns */
    gptps_shutdown(engine);
    printf("done.\n");
    return 0;
}
