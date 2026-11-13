/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_redrive_opts.c - a dead letter remembers the options it was submitted with,
 * and the dashboard uses them.
 *
 *  1) gptps_dead_letter.options holds exactly the GPTPS_SUBMIT_* the submit applied,
 *     with what the item ran under: priority, policy, timeout, and the due time as
 *     what is left of it at the drain (0 once passed). A plain submit's is empty.
 *  2) The dashboard's dead-letter re-run ('l' then 'r') re-submits each with those
 *     options - but not a due time that has passed.
 *  3) A hotkey bound with gptps_tui_add_task_ex submits with its options, the due time
 *     counting from the press.
 *
 * A scheduler records what each re-submitted item carries, by the payload's tag byte.
 * MANUAL mode, so the queue is exactly what each step leaves.
 */
#include "gptps.h"
#include "gptps_tui.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static uint64_t g_dl[256], g_enq[256];
static int32_t  g_prio[256];
static int      g_seen[256];
static int      g_fail = 1;     /* "job" fails while set */

static int64_t spy(const gptps_sched_input *in, void *ud)
{
    unsigned char t;
    (void)ud;
    if (!in->payload_len) return in->priority;
    t = ((const unsigned char *)in->payload)[0];
    g_seen[t] = 1; g_dl[t] = in->deadline_ms; g_enq[t] = in->enqueue_ms; g_prio[t] = in->priority;
    return in->priority;
}
static gptps_status body(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return g_fail ? GPTPS_E_TASK : GPTPS_OK; }

/* what the first drain hands back, by tag */
static gptps_submit_options g_got[256];
static int g_has[256];
static void grab(const gptps_dead_letter *dl, void *ud)
{
    unsigned char t;
    (void)ud;
    if (!dl->payload_len) return;
    t = ((const unsigned char *)dl->payload)[0];
    CHECK(dl->struct_size >= offsetof(gptps_dead_letter, options) + sizeof dl->options);
    g_got[t] = dl->options; g_has[t] = 1;
}

static void drain(gptps *e) { size_t n; while (gptps_step(e, &n) == GPTPS_OK && n) { } }
static void spin_ms(uint64_t ms) { uint64_t t = gptps_now_ms(NULL); while (gptps_now_ms(NULL) - t < ms) { } }

static gptps *open_engine(void)
{
    gptps_config cfg; gptps *e = NULL; gptps_task_def d;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1; cfg.limits.max_memory_bytes = 1u << 20;
    cfg.mode = GPTPS_RUN_MANUAL;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "job"; d.run = body;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;   /* no retries: dead-lettered at once */
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(gptps_set_scheduler(e, spy, NULL) == GPTPS_OK);
    return e;
}

static gptps_submit_options full(uint64_t due_in, int32_t prio)
{
    gptps_submit_options o;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.flags = GPTPS_SUBMIT_PRIORITY | GPTPS_SUBMIT_POLICY | GPTPS_SUBMIT_DEADLINE | GPTPS_SUBMIT_TIMEOUT_MS;
    o.priority = prio; o.deadline_in_ms = due_in; o.timeout_ms = 4000;
    o.policy.struct_size = sizeof o.policy; o.policy.max_retries = 0;
    o.policy.on_failure = GPTPS_ON_FAILURE_DEAD_LETTER; o.policy.timeout_seconds = 9;
    return o;
}

int main(void)
{
    gptps *e;
    gptps_submit_options o;
    unsigned char t;

    /* 1) what a dead letter hands back */
    e = open_engine();
    t = 'f'; o = full(600000, 7);  CHECK(gptps_submit_ex(e, "job", &t, 1, &o, NULL) == GPTPS_OK);
    t = 'l'; o = full(1, 5);       CHECK(gptps_submit_ex(e, "job", &t, 1, &o, NULL) == GPTPS_OK);
    t = 'p';                       CHECK(gptps_submit(e, "job", &t, 1, NULL) == GPTPS_OK);
    drain(e);
    spin_ms(20);                                         /* 'l' is past due by the drain */
    CHECK(gptps_dead_letter_count(e) == 3);
    CHECK(gptps_dead_letter_drain(e, grab, NULL) == 3);
    CHECK(g_has['f'] && g_has['l'] && g_has['p']);
    CHECK(g_got['f'].flags == (GPTPS_SUBMIT_PRIORITY | GPTPS_SUBMIT_POLICY | GPTPS_SUBMIT_DEADLINE | GPTPS_SUBMIT_TIMEOUT_MS));
    CHECK(g_got['f'].priority == 7 && g_got['f'].timeout_ms == 4000);
    CHECK(g_got['f'].policy.timeout_seconds == 9 && g_got['f'].policy.on_failure == GPTPS_ON_FAILURE_DEAD_LETTER);
    CHECK(g_got['f'].deadline_in_ms > 590000 && g_got['f'].deadline_in_ms <= 600000);   /* what is left */
    CHECK((g_got['l'].flags & GPTPS_SUBMIT_DEADLINE) && g_got['l'].deadline_in_ms == 0);   /* passed */
    CHECK(g_got['p'].flags == 0);                        /* a plain submit applied none */
    CHECK(gptps_shutdown(e) == GPTPS_OK);

    /* 2) and 3) the dashboard */
    {
        gptps_tui_config tc;
        gptps_tui *ui;
        memset(g_seen, 0, sizeof g_seen);
        e = open_engine();
        memset(&tc, 0, sizeof tc);
        tc.struct_size = sizeof tc; tc.interactive = 0; tc.color = 0;
        ui = gptps_tui_install(e, &tc);
        CHECK(ui != NULL);
        if (ui) {
            t = 'f'; o = full(600000, 7); CHECK(gptps_submit_ex(e, "job", &t, 1, &o, NULL) == GPTPS_OK);
            t = 'l'; o = full(1, 5);      CHECK(gptps_submit_ex(e, "job", &t, 1, &o, NULL) == GPTPS_OK);
            drain(e);
            spin_ms(20);
            CHECK(gptps_dead_letter_count(e) == 2);
            g_fail = 0;                                   /* the re-run succeeds */
            memset(g_seen, 0, sizeof g_seen);
            gptps_tui_press(ui, 'l');                     /* the dead-letter view */
            gptps_tui_press(ui, 'r');                     /* re-run them all */
            drain(e);
            CHECK(g_seen['f'] && g_prio['f'] == 7);
            CHECK(g_dl['f'] - g_enq['f'] > 590000 && g_dl['f'] - g_enq['f'] <= 600000);
            CHECK(g_seen['l'] && g_prio['l'] == 5 && g_dl['l'] == 0);   /* passed: re-run without one */
            CHECK(gptps_dead_letter_count(e) == 0);

            /* 3) a hotkey with options */
            gptps_tui_press(ui, 27);                      /* back to the dashboard */
            t = 'h'; o = full(5000, 4);
            CHECK(gptps_tui_add_task_ex(ui, "job", "hot", 'k', &t, 1, &o) == GPTPS_OK);
            o.struct_size = offsetof(gptps_submit_options, timeout_ms);
            CHECK(gptps_tui_add_task_ex(ui, "job", "hot", 'k', &t, 1, &o) == GPTPS_E_INVAL);
            CHECK(gptps_tui_press(ui, 'k') == 1);
            drain(e);
            CHECK(g_seen['h'] && g_prio['h'] == 4 && g_dl['h'] - g_enq['h'] == 5000);
        }
        CHECK(gptps_shutdown(e) == GPTPS_OK);
        gptps_tui_close(ui);
    }

    if (fails) { printf("%d re-drive check(s) FAILED\n", fails); return 1; }
    printf("re-drive options: all checks passed\n");
    return 0;
}
