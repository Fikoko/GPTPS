/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_sched_addon.c - addons/gptps_sched: the orders each discipline promises.
 *
 *  1) EDF: due items earliest first, then the rest by priority; and the seam is not
 *     taken from an owner (GPTPS_E_BUSY), as the core asks of add-ons.
 *  2) Aging: one level of priority is worth ms_per_level of waiting.
 *  3) Fair share: equal tenants alternate; weights split admissions 2:1; an idle
 *     tenant comes back at par, not with banked turns; and a pass that admits two at
 *     once gives one to each tenant.
 *  4) shed_late: an item past due by more than the grace is refused (dead-lettered,
 *     GPTPS_E_DENIED); one without a due time, or not yet due, runs.
 *
 * MANUAL mode, so the queue is exactly what each case lines up.
 */
#include "gptps.h"
#include "gptps_sched.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

#define MAXO 64
static char g_order[MAXO + 1];       /* first payload byte of each STARTED, in order */
static int  g_n, g_denied;
static gptps_handle g_tag[MAXO];     /* handle -> payload byte, for STARTED lookup */
static char g_tag_c[MAXO];

static void on_ev(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_STARTED && g_n < MAXO) {
        int i;
        for (i = 0; i < MAXO; ++i) if (g_tag[i] == ev->handle) { g_order[g_n++] = g_tag_c[i]; break; }
        g_order[g_n] = 0;
    }
    if (ev->kind == GPTPS_EV_DEAD_LETTERED && ev->status == GPTPS_E_DENIED) g_denied++;
}

static gptps_status ok_task(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_OK; }

static gptps *open_manual(uint32_t slots)
{
    gptps_config cfg; gptps *e = NULL; gptps_task_def d;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = slots;
    cfg.limits.max_memory_bytes = 1u << 20;
    cfg.mode = GPTPS_RUN_MANUAL;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "t"; d.run = ok_task; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(gptps_set_event_cb(e, on_ev, NULL) == GPTPS_OK);
    g_n = 0; g_order[0] = 0; g_denied = 0;
    memset(g_tag, 0, sizeof g_tag);
    return e;
}

/* submit one item tagged `c` (also its payload, so a tenant fn can read it) */
static void sub(gptps *e, char c, int32_t prio, int due, uint64_t due_in)
{
    gptps_submit_options o; gptps_handle h = 0; int i;
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.flags = GPTPS_SUBMIT_PRIORITY | (due ? GPTPS_SUBMIT_DEADLINE : 0u);
    o.priority = prio; o.deadline_in_ms = due_in;
    CHECK(gptps_submit_ex(e, "t", &c, 1, &o, &h) == GPTPS_OK);
    for (i = 0; i < MAXO; ++i) if (!g_tag[i]) { g_tag[i] = h; g_tag_c[i] = c; break; }
}

static void drain(gptps *e) { size_t n; while (gptps_step(e, &n) == GPTPS_OK && n) { } }
static void spin_ms(uint64_t ms) { uint64_t t = gptps_now_ms(NULL); while (gptps_now_ms(NULL) - t < ms) { } }

static uint64_t by_payload(const gptps_sched_input *in, void *ud)
{
    (void)ud;
    return in->payload_len ? (uint64_t)((const unsigned char *)in->payload)[0] : 0u;
}

int main(void)
{
    gptps_sched *s, *s2;
    gptps *e;

    /* 1) EDF */
    e = open_manual(1);
    CHECK(gptps_sched_edf(e, &s) == GPTPS_OK);
    CHECK(gptps_sched_aging(e, 10, &s2) == GPTPS_E_BUSY && s2 == NULL);   /* not taken from its owner */
    CHECK(strcmp(gptps_scheduler_owner(e), "sched.edf") == 0);
    sub(e, 'x', 5, 0, 0);          /* no due time, high priority */
    sub(e, 'c', 0, 1, 30000);
    sub(e, 'a', 0, 1, 1000);
    sub(e, 'y', 9, 0, 0);
    sub(e, 'b', 0, 1, 5000);
    drain(e);
    CHECK(strcmp(g_order, "abcyx") == 0);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    gptps_sched_close(s);

    /* 2) aging: 'L' (prio 0) waits 40 ms before 'H' (prio 1) arrives */
    e = open_manual(1);
    CHECK(gptps_sched_aging(e, 10, &s) == GPTPS_OK);     /* 40 ms of waiting > one level */
    sub(e, 'L', 0, 0, 0); spin_ms(40); sub(e, 'H', 1, 0, 0);
    drain(e);
    CHECK(strcmp(g_order, "LH") == 0);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    gptps_sched_close(s);

    e = open_manual(1);
    CHECK(gptps_sched_aging(e, 1000000, &s) == GPTPS_OK); /* a level is worth far more */
    sub(e, 'L', 0, 0, 0); spin_ms(40); sub(e, 'H', 1, 0, 0);
    drain(e);
    CHECK(strcmp(g_order, "HL") == 0);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    gptps_sched_close(s);

    /* 3a) fair share, equal weights: A queued six before B queued three */
    e = open_manual(1);
    CHECK(gptps_sched_fair(e, by_payload, NULL, 0, &s) == GPTPS_OK);
    sub(e, 'A', 0, 0, 0); sub(e, 'A', 0, 0, 0); sub(e, 'A', 0, 0, 0);
    sub(e, 'A', 0, 0, 0); sub(e, 'A', 0, 0, 0); sub(e, 'A', 0, 0, 0);
    sub(e, 'B', 9, 0, 0); sub(e, 'B', 9, 0, 0); sub(e, 'B', 9, 0, 0);   /* priority does not buy share */
    drain(e);
    CHECK(strcmp(g_order, "ABABABAAA") == 0);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    gptps_sched_close(s);

    /* 3b) weights 2:1 */
    e = open_manual(1);
    CHECK(gptps_sched_fair(e, by_payload, NULL, 0, &s) == GPTPS_OK);
    CHECK(gptps_sched_fair_weight(s, 'A', 2) == GPTPS_OK);
    CHECK(gptps_sched_fair_weight(s, 'A', 0) == GPTPS_E_INVAL);
    { int i; for (i = 0; i < 6; ++i) sub(e, 'A', 0, 0, 0); for (i = 0; i < 3; ++i) sub(e, 'B', 0, 0, 0); }
    drain(e);
    CHECK(strcmp(g_order, "ABAABAABA") == 0);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    gptps_sched_close(s);

    /* 3c) no banking: A runs alone for a while; B, arriving later, gets its share from
     * now on - not a run of every turn it missed */
    e = open_manual(1);
    CHECK(gptps_sched_fair(e, by_payload, NULL, 0, &s) == GPTPS_OK);
    { int i; for (i = 0; i < 5; ++i) sub(e, 'A', 0, 0, 0); }
    drain(e);
    g_n = 0; g_order[0] = 0;
    { int i; for (i = 0; i < 3; ++i) sub(e, 'A', 0, 0, 0); for (i = 0; i < 3; ++i) sub(e, 'B', 0, 0, 0); }
    drain(e);
    /* A's next tags start at its finish (5), B's at the virtual time (4, A's last start) */
    CHECK(strcmp(g_order, "BABABA") == 0);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    gptps_sched_close(s);

    /* 3d) one pass, two slots: one each, not both to the tenant that queued first */
    e = open_manual(2);
    CHECK(gptps_sched_fair(e, by_payload, NULL, 0, &s) == GPTPS_OK);
    { int i; for (i = 0; i < 4; ++i) sub(e, 'A', 0, 0, 0); for (i = 0; i < 4; ++i) sub(e, 'B', 0, 0, 0); }
    { size_t n; CHECK(gptps_step(e, &n) == GPTPS_OK); }
    CHECK(g_n == 2);
    CHECK(strcmp(g_order, "AB") == 0);
    drain(e);
    CHECK(g_n == 8);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    gptps_sched_close(s);

    /* 3e) the default tenant is the task name: one task type is one tenant */
    e = open_manual(1);
    CHECK(gptps_sched_fair(e, NULL, NULL, 4, &s) == GPTPS_OK);
    sub(e, 'p', 0, 0, 0); sub(e, 'q', 0, 0, 0);
    drain(e);
    CHECK(strcmp(g_order, "pq") == 0);                     /* FIFO within the one tenant */
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    gptps_sched_close(s);

    /* 4) shed late work, composed with EDF */
    e = open_manual(1);
    CHECK(gptps_sched_edf(e, &s) == GPTPS_OK);
    CHECK(gptps_sched_shed_late(e, 5, &s2) == GPTPS_OK);
    sub(e, 'n', 0, 0, 0);           /* no due time: never shed */
    sub(e, 'l', 0, 1, 0);           /* due now: late by the time it comes up */
    sub(e, 'f', 0, 1, 60000);       /* due in a minute */
    spin_ms(30);
    drain(e);
    CHECK(strcmp(g_order, "fn") == 0);
    CHECK(g_denied == 1);
    CHECK(gptps_sched_shed_count(s2) == 1);
    CHECK(gptps_sched_shed_count(s) == 0);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    gptps_sched_close(s);
    gptps_sched_close(s2);
    gptps_sched_close(NULL);

    if (fails) { printf("%d sched add-on check(s) FAILED\n", fails); return 1; }
    printf("sched add-on: all checks passed\n");
    return 0;
}
