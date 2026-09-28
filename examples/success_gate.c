/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * success_gate.c - a success-only dependency belongs to the host:
 * gptps_orch_after releases on ANY terminal outcome, not just success. This
 * example needs only the core API.
 *
 * MANUAL mode keeps the host state on one thread. The prerequisite models an
 * acknowledged operation: return OK only after the acknowledgement, not merely
 * after sending a request. A real asynchronous acknowledgement needs its own
 * host state machine. This is not a persistent graph or a threaded recipe.
 *
 * Observe FINISHED, then submit from the host loop, outside the callback. A
 * failed submission is an error here, not a released dependency. In production
 * the host must retain unsubmitted work and choose a retry/abort policy.
 *
 *   cc success_gate.c gptps.c -lpthread -ldl   (amalgamation; macOS: drop -ldl)
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    gptps_handle prerequisite;
    int fail_prerequisite;
    int attempts, acknowledged, released;
    int dependent_runs, independent_runs, terminal_failures, errors;
} host;

static gptps_status prerequisite(gptps_ctx *ctx, void *ud)
{
    host *h = (host *)ud;
    (void)ctx;
    ++h->attempts;
    /* Exercise a nonterminal failure too: it must NOT release the dependent. */
    return h->fail_prerequisite || h->attempts == 1 ? GPTPS_E_TASK : GPTPS_OK;
}

static gptps_status dependent(gptps_ctx *ctx, void *ud)
{
    host *h = (host *)ud;
    (void)ctx;
    ++h->dependent_runs;
    if (!h->acknowledged) { ++h->errors; return GPTPS_E_TASK; }
    return GPTPS_OK;
}

static gptps_status independent(gptps_ctx *ctx, void *ud)
{
    host *h = (host *)ud;
    (void)ctx;
    ++h->independent_runs;
    return GPTPS_OK;
}

static void observe(const gptps_event *ev, void *ud)
{
    host *h = (host *)ud;
    if (ev->handle != h->prerequisite) return;
    if (ev->kind == GPTPS_EV_FINISHED) h->acknowledged = 1;
    if (ev->kind == GPTPS_EV_DEAD_LETTERED) ++h->terminal_failures;
    /* FAILED can be an individual attempt; neither it nor a terminal failure
     * acknowledges success. Other jobs' FINISHED events do not open this gate. */
}

static int run_case(int fail_prerequisite)
{
    host h;
    gptps *e = NULL;
    gptps_config cfg;
    gptps_task_def d;
    gptps_handle handle;
    size_t ran;
    int i, ok = 0;
    memset(&h, 0, sizeof h);
    memset(&cfg, 0, sizeof cfg);
    memset(&d, 0, sizeof d);
    h.fail_prerequisite = fail_prerequisite;
    cfg.struct_size = sizeof cfg;
    cfg.mode = GPTPS_RUN_MANUAL;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;
    cfg.limits.max_memory_bytes = 1024;
    cfg.limits.max_intake_depth = 4;
#define REQUIRE(expr) do { if (!(expr)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); goto cleanup; \
} } while (0)
    REQUIRE(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    d.struct_size = sizeof d;
    d.exec = GPTPS_EXEC_INPROC;
    d.user_data = &h;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_cost.mem_bytes = 1024;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = 1;
    d.default_policy.on_failure = GPTPS_ON_FAILURE_DEAD_LETTER;
    d.name = "prerequisite"; d.run = prerequisite;
    REQUIRE(gptps_register_task(e, &d) == GPTPS_OK);
    d.default_policy.max_retries = 0;
    d.name = "dependent"; d.run = dependent;
    REQUIRE(gptps_register_task(e, &d) == GPTPS_OK);
    d.name = "independent"; d.run = independent;
    REQUIRE(gptps_register_task(e, &d) == GPTPS_OK);
    REQUIRE(gptps_register_observer(e, observe, &h) == GPTPS_OK);
    REQUIRE(gptps_submit(e, "prerequisite", NULL, 0, &h.prerequisite) == GPTPS_OK);
    REQUIRE(gptps_submit(e, "independent", NULL, 0, &handle) == GPTPS_OK);

    /* Fixed bound: no sleeps, unbounded waits, or shutdown to manufacture progress.
     * Extra steps also check that an acknowledged gate submits only once. */
    for (i = 0; i < 12; ++i) {
        REQUIRE(gptps_step(e, &ran) == GPTPS_OK);
        if (h.acknowledged && !h.released) {
            REQUIRE(gptps_submit(e, "dependent", NULL, 0, &handle) == GPTPS_OK);
            h.released = 1;
        }
    }
    REQUIRE(h.errors == 0 && h.attempts == 2 && h.independent_runs == 1);
    REQUIRE(h.acknowledged == !fail_prerequisite);
    REQUIRE(h.released == !fail_prerequisite);
    REQUIRE(h.dependent_runs == !fail_prerequisite);
    REQUIRE(h.terminal_failures == fail_prerequisite);
    ok = 1;
cleanup:
    if (e && gptps_shutdown(e) != GPTPS_OK) ok = 0;
    if (h.errors || h.dependent_runs != !fail_prerequisite) ok = 0;
    printf("prerequisite=%s acknowledged=%d dependent=%d independent=%d: %s\n",
           fail_prerequisite ? "failed" : "succeeded", h.acknowledged,
           h.dependent_runs, h.independent_runs, ok ? "PASS" : "FAIL");
#undef REQUIRE
    return ok;
}

int main(void)
{
    int success = run_case(0);
    int failure = run_case(1);
    return success && failure ? 0 : 1;
}
