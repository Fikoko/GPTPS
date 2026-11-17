/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_retry_forget.c - addons/gptps_retry when more failed items wait for a decision
 * than it keeps: never silent.
 *
 * Reaching 4096 pending decisions through real programs would take thousands of
 * process starts, so this includes the add-on's source and feeds its observer and
 * constraint the events and admissions an engine would.
 *
 *  1) Past the bound the OLDEST decision is dropped - that item is admitted as its
 *     policy says - the newest are kept, gptps_retry_forgotten counts each drop, and
 *     the first drop logs one WARN, the rest none.
 *  2) A terminal event forgets its item's decision, so finished work never fills it.
 */
#include "../addons/gptps_retry.c"
#include <stdio.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int g_warns;
static void sink(gptps_log_level lvl, const char *msg, void *ud)
{
    (void)ud;
    if (lvl == GPTPS_LOG_WARN && strstr(msg, "gptps_retry")) ++g_warns;
}

static void failed(gptps_retry *r, gptps_handle h, uint64_t code)
{
    gptps_measure m;
    gptps_event ev;
    memset(&m, 0, sizeof m);
    m.name = GPTPS_M_EXIT_CODE; m.value = code; m.unit = GPTPS_UNIT_CODE;
    m.kind = GPTPS_MEASURE_CURRENT; m.method = "test";
    memset(&ev, 0, sizeof ev);
    ev.struct_size = sizeof ev; ev.kind = GPTPS_EV_FAILED; ev.handle = h; ev.task_name = "t";
    ev.measures = &m; ev.n_measures = 1;
    retry_obs(&ev, r);
}

static gptps_admit_decision admit(gptps_retry *r, gptps_handle h)
{
    gptps_constraint_input in;
    uint32_t after = 0;
    memset(&in, 0, sizeof in);
    in.struct_size = sizeof in; in.task_name = "t"; in.handle = h; in.now_ms = 1000;
    return retry_fn(&in, &after, r);
}

int main(void)
{
    gptps_retry *r = (gptps_retry *)calloc(1, sizeof *r);
    gptps_handle h;
    gptps_event ev;
    if (!r) return 1;
    apx_mutex_init(&r->mu);
    gptps_set_log_sink(sink, NULL);
    CHECK(gptps_retry_on_exit(r, NULL, 2, GPTPS_RETRY_NEVER) == GPTPS_OK);

    /* 1) 4096 + 10 items fail with 2 and none has come back for its retry yet */
    for (h = 1; h <= MAX_PENDING + 10; ++h) failed(r, h, 2);
    CHECK(gptps_retry_forgotten(r) == 10);
    CHECK(g_warns == 1);                                  /* said once, not ten times */
    CHECK(admit(r, 1) == GPTPS_ADMIT);                    /* the oldest: forgotten, as its policy says */
    CHECK(admit(r, 10) == GPTPS_ADMIT);
    CHECK(admit(r, 11) == GPTPS_DENY);                    /* the rest: kept */
    CHECK(admit(r, MAX_PENDING + 10) == GPTPS_DENY);
    CHECK(gptps_retry_refused(r) == 2);

    /* 2) a terminal event forgets: nothing is left to refuse */
    memset(&ev, 0, sizeof ev);
    ev.struct_size = sizeof ev; ev.kind = GPTPS_EV_DEAD_LETTERED; ev.handle = 12; ev.task_name = "t";
    retry_obs(&ev, r);
    CHECK(admit(r, 12) == GPTPS_ADMIT);

    gptps_set_log_sink(NULL, NULL);
    gptps_retry_close(r);
    if (fails) { printf("%d retry-forget check(s) FAILED\n", fails); return 1; }
    printf("retry forget: all checks passed\n");
    return 0;
}
