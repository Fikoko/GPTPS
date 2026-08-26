/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_admission_order.c - the exact admission ORDER contract, pinned.
 *
 * The dispatcher admits by sched_score (priority, unless a scheduler hook replaces
 * the key), highest first, and resolves a tie to the OLDER item so submission order
 * survives inside one priority. test_scheduler.c proves the policy in the presence
 * of a budget (skip-to-fit, reservation); this test pins the ordering itself over a
 * deep queue, which is what an intake-queue representation change is most likely to
 * break silently.
 *
 * MANUAL mode with one worker makes it deterministic: no dispatcher thread runs
 * until gptps_step(), so every item is queued before the first admission decision,
 * and admissions are strictly sequential.
 *
 * The later cases exist because the queue is INDEXED (src/engine.c, "intake
 * ordering") to keep an ordered insert O(1). An index is a second thing that can be
 * wrong, and the ways it can be wrong are: overflowing it, and leaving an entry
 * pointing at an item that has been removed and freed. Cases 2-4 do exactly those,
 * so ASan turns a stale entry into a hard failure rather than a subtle mis-ordering.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

#define N 24
#define ORD_MAX 160
static int g_order[ORD_MAX];  /* payload byte of each item, in admission order */
static int g_n;

static gptps_status task_job(gptps_ctx *c, void *u)
{
    size_t len;
    const unsigned char *p = (const unsigned char *)gptps_payload(c, &len);
    (void)u;
    if (g_n < ORD_MAX) g_order[g_n++] = (p && len) ? (int)p[0] : -1;
    return GPTPS_OK;
}

/* the priority handed to item i; deliberately not monotonic, with repeats so the
 * FIFO-within-a-priority tie-break is actually exercised */
static int32_t prio_of(int i)
{
    static const int32_t pat[6] = { 0, 5, -3, 5, 0, 7 };
    return pat[i % 6];
}

/* --- case 1: ordering over a deep queue ---------------------------------- */
static void case_priority_order(void)
{
    gptps_config cfg;
    gptps *e = NULL;
    gptps_task_def d;
    gptps_submit_options o;
    unsigned char pay[1];
    int i, k;
    int32_t want[N];
    size_t ran;

    memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;
    cfg.mode = GPTPS_RUN_MANUAL;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (!e) return;

    memset(&d, 0, sizeof d); d.struct_size = sizeof d;
    d.name = "j"; d.run = task_job; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);

    /* queue every item first: MANUAL mode admits nothing until gptps_step */
    for (i = 0; i < N; ++i) {
        memset(&o, 0, sizeof o); o.struct_size = sizeof o;
        o.flags = GPTPS_SUBMIT_PRIORITY; o.priority = prio_of(i);
        o.policy.struct_size = sizeof o.policy;
        pay[0] = (unsigned char)i;
        CHECK(gptps_submit_ex(e, "j", pay, 1, &o, NULL) == GPTPS_OK);
    }

    while (gptps_step(e, &ran) == GPTPS_OK && ran) { /* drain */ }
    CHECK(g_n == N);

    /* expected: a STABLE sort of 0..N-1 by priority descending */
    k = 0;
    { int32_t lv[4]; size_t li; lv[0] = 7; lv[1] = 5; lv[2] = 0; lv[3] = -3;
      for (li = 0; li < 4; ++li)
          for (i = 0; i < N; ++i)
              if (prio_of(i) == lv[li]) want[k++] = i; }
    CHECK(k == N);

    for (i = 0; i < N && i < g_n; ++i)
        if (g_order[i] != want[i]) {
            printf("FAIL admission slot %d: got item %d (prio %d), want item %d (prio %d)\n",
                   i, g_order[i], (int)prio_of(g_order[i]), (int)want[i], (int)prio_of(want[i]));
            ++fails;
        }

    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

/* --- shared helpers for the index cases ---------------------------------- */

static gptps *open_manual(void)
{
    gptps_config cfg;
    gptps *e = NULL;
    memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;
    cfg.mode = GPTPS_RUN_MANUAL;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    return e;
}

static void reg_job(gptps *e, const char *name)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d); d.struct_size = sizeof d;
    d.name = name; d.run = task_job; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

static gptps_status put(gptps *e, const char *name, int tag, int32_t prio, gptps_handle *h)
{
    gptps_submit_options o;
    unsigned char pay[1];
    memset(&o, 0, sizeof o); o.struct_size = sizeof o;
    o.flags = GPTPS_SUBMIT_PRIORITY; o.priority = prio;
    o.policy.struct_size = sizeof o.policy;
    pay[0] = (unsigned char)tag;
    return gptps_submit_ex(e, name, pay, 1, &o, h);
}

static void drain(gptps *e) { size_t ran; while (gptps_step(e, &ran) == GPTPS_OK && ran) { } }

static void expect(const char *what, const int *want, int n)
{
    int i;
    if (g_n != n) { printf("FAIL %s: ran %d items, want %d\n", what, g_n, n); ++fails; return; }
    for (i = 0; i < n; ++i)
        if (g_order[i] != want[i]) {
            printf("FAIL %s slot %d: got %d, want %d\n", what, i, g_order[i], want[i]);
            ++fails;
        }
}

/* --- case 2: more distinct priorities than the index can hold ------------- */
/* GPTPS_INTAKE_RUNS caps the cached runs at 32. With 70 live priorities the index
 * overflows and evicts, so most inserts miss and fall back to walking - the order
 * must come out identical anyway, because the cache is only ever an accelerator. */
static void case_index_overflow(void)
{
    gptps *e = open_manual();
    int want[140], k = 0, i;
    int32_t lv;

    if (!e) return;
    reg_job(e, "j");
    g_n = 0;
    for (i = 0; i < 140; ++i) CHECK(put(e, "j", i, (int32_t)(i % 70), NULL) == GPTPS_OK);
    drain(e);

    for (lv = 69; lv >= 0; --lv)
        for (i = 0; i < 140; ++i) if (i % 70 == lv) want[k++] = i;
    CHECK(k == 140);
    expect("index_overflow", want, 140);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

/* --- case 3: cancelling the item the index points at ---------------------- */
/* The index caches the TAIL of each equal-priority run. Cancel that exact item and
 * the entry now names freed memory; the next submit at the same priority splices
 * onto it. If the cancel path does not drop the entry, this is a use-after-free. */
static void case_cancel_run_tail(void)
{
    gptps *e = open_manual();
    gptps_handle h0 = 0, h1 = 0, h2 = 0;
    int want[5];

    if (!e) return;
    reg_job(e, "j");
    g_n = 0;
    CHECK(put(e, "j", 10, 5, &h0) == GPTPS_OK);
    CHECK(put(e, "j", 11, 5, &h1) == GPTPS_OK);
    CHECK(put(e, "j", 12, 5, &h2) == GPTPS_OK);   /* tail of the priority-5 run */
    CHECK(put(e, "j", 20, 1, NULL) == GPTPS_OK);
    (void)h0; (void)h1;
    CHECK(gptps_cancel(e, h2) == GPTPS_OK);       /* free the item the index names */
    CHECK(put(e, "j", 13, 5, NULL) == GPTPS_OK);  /* would splice onto it */
    CHECK(put(e, "j", 14, 5, NULL) == GPTPS_OK);
    drain(e);

    want[0] = 10; want[1] = 11; want[2] = 13; want[3] = 14; want[4] = 20;
    expect("cancel_run_tail", want, 5);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

/* --- case 4: unregistering a type out from under the index ---------------- */
/* GPTPS_REMOVE_CANCEL drops a whole type's queued backlog in one bulk unlink, which
 * can free several run tails at once. Same hazard as case 3, different path in. */
static void case_unregister_drops_queued(void)
{
    gptps *e = open_manual();
    int want[4];

    if (!e) return;
    reg_job(e, "a");
    reg_job(e, "b");
    g_n = 0;
    CHECK(put(e, "a", 30, 5, NULL) == GPTPS_OK);
    CHECK(put(e, "b", 31, 5, NULL) == GPTPS_OK);   /* tail of the priority-5 run */
    CHECK(put(e, "a", 32, 2, NULL) == GPTPS_OK);
    CHECK(put(e, "b", 33, 2, NULL) == GPTPS_OK);   /* tail of the priority-2 run */
    CHECK(gptps_unregister_task(e, "b", GPTPS_REMOVE_CANCEL) == GPTPS_OK);
    CHECK(put(e, "a", 34, 5, NULL) == GPTPS_OK);   /* splices onto a freed tail? */
    CHECK(put(e, "a", 35, 2, NULL) == GPTPS_OK);
    drain(e);

    want[0] = 30; want[1] = 34; want[2] = 32; want[3] = 35;
    expect("unregister_drops_queued", want, 4);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

/* --- case 5: a scheduler hook restamps every key ------------------------- */
/* With a hook installed the ordering key is recomputed for every queued item on
 * every pass, so the queue has to be re-ordered and the index thrown away. Here the
 * hook inverts priority, and equal keys must still come out oldest-first. */
static int64_t sched_invert(const gptps_sched_input *in, void *ud)
{ (void)ud; return -(int64_t)in->priority; }

static void case_scheduler_hook_reorders(void)
{
    gptps *e = open_manual();
    int want[6];

    if (!e) return;
    reg_job(e, "j");
    g_n = 0;
    CHECK(gptps_set_scheduler(e, sched_invert, NULL) == GPTPS_OK);
    CHECK(put(e, "j", 40, 5, NULL) == GPTPS_OK);
    CHECK(put(e, "j", 41, 1, NULL) == GPTPS_OK);
    CHECK(put(e, "j", 42, 9, NULL) == GPTPS_OK);
    CHECK(put(e, "j", 43, 1, NULL) == GPTPS_OK);   /* ties with 41 under the hook */
    CHECK(put(e, "j", 44, 5, NULL) == GPTPS_OK);
    CHECK(put(e, "j", 45, 9, NULL) == GPTPS_OK);
    drain(e);

    /* inverted: priority 1 first, then 5, then 9 - oldest first inside each */
    want[0] = 41; want[1] = 43; want[2] = 40; want[3] = 44; want[4] = 42; want[5] = 45;
    expect("scheduler_hook_reorders", want, 6);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

int main(void)
{
    case_priority_order();
    case_index_overflow();
    case_cancel_run_tail();
    case_unregister_drops_queued();
    case_scheduler_hook_reorders();
    printf("test_admission_order: %s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
