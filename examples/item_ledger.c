/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 nightops00dev. See LICENSE for the full text. */
/*
 * item_ledger.c - the dead-letter ledger a host keeps over the observer seam:
 * one row per business id, closed by a single close_row() that BOTH the
 * observer and the drain call - the first terminal fact closes the row, a
 * later one only confirms it - a re-drive bounded PER ROW that moves the row
 * to its new handle, and a host-side outage switch that stands in for the
 * nightly outage a real re-drive fixes.
 *
 * Why the shape is what it is (argued out in
 * https://github.com/Fikoko/GPTPS/issues/12):
 *   - The engine does not know your business ids; events carry no payload.
 *     The row map is therefore handle -> row, built by the host.
 *   - A handle reaches exactly one terminal event, but the drain can hand
 *     you the same item BEFORE that event is delivered, or after. Closing
 *     must be idempotent: close_row records the first terminal fact that
 *     arrives and only counts a confirmation for every later one - whichever
 *     side arrives first, the row ends with exactly one close.
 *   - The re-drive replaces the handle: from gptps_submit on, the row
 *     belongs to the new handle. Looking the row up BY THE CURRENT HANDLE
 *     is the guard: a late DEAD_LETTERED for the superseded handle finds
 *     no row, exactly as a hash map keyed by the current handle would.
 *   - A re-drive fixes only a failure caused by something OUTSIDE the item:
 *     here the outage switch, on during the night and cleared before the
 *     morning drain. The re-drive re-submits the business id UNCHANGED. Bad
 *     input is different: no re-drive fixes it, which is why bad-1 below is
 *     re-driven once, fails again, and its re-drive bound is what stops the
 *     loop. Idempotency covers partial work, not invalid payloads.
 *
 * The run below teaches four things, and the REQUIREs catch their removal:
 *   1. Close once, confirm after: bad-1 ends dead with one close and one
 *      confirmation, because the declining drain confirms the row its event
 *      already closed.
 *   2. The re-drive bound is per row: bad-1 is re-driven exactly once.
 *   3. The handle lookup is the guard: the late event replayed at the bottom
 *      must not touch id-3's row.
 *   4. The outage switch, not a payload marker, is what makes the re-drive
 *      succeed.
 *
 * MANUAL mode keeps every callback on this thread, which makes the ledger
 * race-free WITHOUT locks and the example deterministic. A threaded host
 * needs the same guards PLUS one lock around the ledger, since events arrive
 * on engine threads while the drain runs on yours - and it must hold that
 * lock from BEFORE gptps_submit until the row has the handle it returned, for
 * the night's submits and for every re-drive. In THREADED mode an item can
 * finish, and its event arrive, before gptps_submit returns: the lookup by
 * handle would find no row and drop the event as a late one, and the row
 * would stay open. Held across the submit, the lock makes that event wait
 * until the row is there. Take it in observe() only AFTER the kind check:
 * QUEUED is delivered inside gptps_submit, on the thread holding the lock.
 * The late-event section at the bottom replays, on purpose, the delivery a
 * threaded host can see.
 *
 *   cc item_ledger.c gptps.c -lpthread -ldl   (amalgamation; macOS: drop -ldl)
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

#define ITEM_N  6
#define MAX_STEPS 64
#define REDRIVE_ROUNDS 1   /* re-drives a row gets before the drain declines it */

typedef struct {
    char id[16];          /* the business id, e.g. "id-3" */
    gptps_handle h;       /* the handle that CURRENTLY owns this row */
    int state;            /* 0 open, 1 finished, 2 dead-lettered */
    int closed;           /* a terminal fact was applied under the current handle */
    int closes;           /* closes under the current handle: exactly 1 */
    int confirmations;    /* terminal arrivals after the close: confirmations */
    int rounds;           /* re-drives spent on this row: bounded per row */
    gptps_status status;  /* recorded at close */
    unsigned attempts;    /* recorded at close */
} row;

typedef struct {
    row r[ITEM_N];
    int n;
} ledger;

static ledger g; /* single host thread in MANUAL mode: no locking below */

/* The nightly outage, as the host sees it: something OUTSIDE the item is
 * down, so every item fails until the host clears the switch. In MANUAL
 * mode the body runs on this thread, so a plain variable is fine. */
static int g_outage;

static gptps_status process_item(gptps_ctx *ctx, void *ud)
{
    size_t n;
    const char *p = (const char *)gptps_payload(ctx, &n);
    unsigned long sum = 0;
    (void)ud;
    if (g_outage) return GPTPS_E_TASK; /* the called system is down: not the
        item's fault; a re-drive after the outage clears will succeed */
    if (n >= 3 && memcmp(p, "bad", 3) == 0) return GPTPS_E_TASK; /* bad input:
        no re-drive fixes this; idempotency is about partial work, not this */
    while (n--) sum += (unsigned char)*p++;
    return gptps_result_set(ctx, &sum, sizeof sum);
}

/* --- the ledger ---------------------------------------------------------- */

/* The lookup by the CURRENT handle is the handle guard: a late event for a
 * superseded handle finds no row here, exactly as a hash map keyed by the
 * current handle would - no separate comparison is needed. */
static row *row_by_handle(gptps_handle h)
{
    int i;
    for (i = 0; i < g.n; ++i) if (g.r[i].h == h) return &g.r[i];
    return NULL;
}

static row *row_by_id(const char *id)
{
    int i;
    for (i = 0; i < g.n; ++i) if (strcmp(g.r[i].id, id) == 0) return &g.r[i];
    return NULL;
}

/* The ONE close. The observer calls it with the event's fields; the drain
 * calls it with dl->status and dl->attempts whenever it declines a re-drive.
 * The first call records the terminal fact; a later arrival for the same
 * attempt only counts a confirmation - neither may rewrite the facts. */
static void close_row(row *r, int state, gptps_status status, unsigned attempts)
{
    if (r->closed) { ++r->confirmations; return; }
    r->closed = 1;
    ++r->closes;
    r->state = state;
    r->status = status;
    r->attempts = attempts;
}

static void observe(const gptps_event *ev, void *ud)
{
    row *r;
    (void)ud;
    if (ev->kind != GPTPS_EV_FINISHED && ev->kind != GPTPS_EV_DEAD_LETTERED)
        return;
    r = row_by_handle(ev->handle);
    if (!r) return; /* superseded handle: late event, the guard above caught it */
    close_row(r, ev->kind == GPTPS_EV_FINISHED ? 1 : 2, ev->status, ev->attempt);
}

/* The morning re-drive. Runs with the engine lock released, so re-submitting
 * here is safe. The payload is valid only for this call. A row's re-drives
 * are bounded PER ROW: past the bound, or if the re-submit itself fails, the
 * drain closes the row from its own facts - which, when the event arrived
 * first, is exactly the confirmation path. So does a business id too long for
 * the copy: re-submitting it truncated would re-drive a different item. */
static void redrive(const gptps_dead_letter *dl, void *ud)
{
    gptps *e = (gptps *)ud;
    char id[16];
    size_t n = dl->payload_len < sizeof id - 1 ? dl->payload_len : sizeof id - 1;
    gptps_handle h;
    row *r;

    memcpy(id, dl->payload, n);
    id[n] = '\0';
    r = row_by_handle(dl->handle);
    if (!r) return; /* superseded, like a late event */
    if (dl->payload_len >= sizeof id || r->rounds >= REDRIVE_ROUNDS
        || gptps_submit(e, "item", id, strlen(id), &h) != GPTPS_OK) {
        close_row(r, 2, dl->status, dl->attempts); /* declined: dead from the drain's facts */
        printf("  [drain ] '%s' left dead (%s)\n", id, gptps_strerror(dl->status));
        return;
    }
    r->h = h;          /* the row now belongs to the new handle */
    ++r->rounds;
    r->closed = 0;     /* reopened under the new handle: a fresh close count */
    r->closes = 0;
    r->state = 0;
    printf("  [drain ] re-submitted '%s' (was %s)\n", id,
           gptps_strerror(dl->status));
}

/* Pump until every row is closed or the step budget runs out. */
static int pump(gptps *e)
{
    int step;
    size_t ran = 1;
    for (step = 0; step < MAX_STEPS && ran; ++step) {
        int i, all = 1;
        if (gptps_step(e, &ran) != GPTPS_OK) return 0;
        for (i = 0; i < g.n; ++i) if (!g.r[i].closed) all = 0;
        if (all) return 1;
    }
    return 0;
}

int main(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    gptps_task_def d;
    /* id-3 fails only because of the outage; bad-1 is bad input. */
    static const char *ids[ITEM_N] = { "id-1", "id-2", "id-3",
                                       "bad-1", "id-5", "id-6" };
    gptps_handle old_h = 0;
    int ok = 0, i;

#define REQUIRE(expr) do { if (!(expr)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); goto cleanup; \
} } while (0)

    memset(&g, 0, sizeof g);
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.mode = GPTPS_RUN_MANUAL;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 2;
    cfg.limits.max_memory_bytes = 16u << 20;
    REQUIRE(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    REQUIRE(gptps_register_observer(e, observe, NULL) == GPTPS_OK);

    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.name = "item";
    d.run = process_item;
    d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_cost.mem_bytes = 1024;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = 1; /* one retry, then the dead-letter list */
    d.default_policy.on_failure = GPTPS_ON_FAILURE_DEAD_LETTER;
    REQUIRE(gptps_register_task(e, &d) == GPTPS_OK);

    /* The night: the outage is on, so the whole batch fails - the items are
     * sound, the systems they call are not. Submit and run to quiescence:
     * every item dead-letters, and every DEAD_LETTERED event closes its row. */
    g_outage = 1;
    for (i = 0; i < ITEM_N; ++i) {
        gptps_handle h;
        REQUIRE(gptps_submit(e, "item", ids[i], strlen(ids[i]), &h) == GPTPS_OK);
        snprintf(g.r[g.n].id, sizeof g.r[g.n].id, "%s", ids[i]);
        g.r[g.n].h = h;
        ++g.n;
    }
    REQUIRE(pump(e));
    for (i = 0; i < ITEM_N; ++i) {
        REQUIRE(g.r[i].state == 2);
        REQUIRE(g.r[i].closes == 1);
    }
    REQUIRE(gptps_dead_letter_count(e) == ITEM_N);

    /* The morning: the outage cleared BEFORE the drain - that, not a payload
     * change, is what a real re-drive fixes. Drain #1 re-drives everything,
     * business ids unchanged, each row moving to its new handle. */
    g_outage = 0;
    old_h = row_by_id("id-3")->h;
    REQUIRE(gptps_dead_letter_drain(e, redrive, e) == ITEM_N);
    REQUIRE(row_by_id("id-3")->h != old_h);
    REQUIRE(gptps_dead_letter_count(e) == 0);
    for (i = 0; i < ITEM_N; ++i) REQUIRE(g.r[i].rounds == 1);

    /* Run again: the five sound items finish (one close each under their
     * new handles); bad-1 fails again - bad input - and its event closes
     * it a second time. */
    REQUIRE(pump(e));
    for (i = 0; i < ITEM_N; ++i)
        if (strcmp(g.r[i].id, "bad-1") != 0) REQUIRE(g.r[i].state == 1);
    REQUIRE(row_by_id("bad-1")->state == 2);
    REQUIRE(gptps_dead_letter_count(e) == 1);

    /* Drain #2: bad-1's re-drive bound is spent, so the drain declines it
     * and confirms the row its event already closed. */
    REQUIRE(gptps_dead_letter_drain(e, redrive, e) == 1);
    REQUIRE(gptps_dead_letter_count(e) == 0);
    REQUIRE(row_by_id("bad-1")->state == 2);
    REQUIRE(row_by_id("bad-1")->closes == 1);
    REQUIRE(row_by_id("bad-1")->confirmations == 1);
    REQUIRE(row_by_id("bad-1")->status == GPTPS_E_TASK);
    REQUIRE(row_by_id("id-3")->closes == 1);
    REQUIRE(row_by_id("id-3")->confirmations == 0);

    /* The threaded case, replayed on purpose: a DEAD_LETTERED for id-3's
     * OLD handle can be delivered after the re-drive. The lookup by the
     * current handle is the guard: it must find no row and change nothing. */
    {
        gptps_event late;
        row *r3 = row_by_id("id-3");
        memset(&late, 0, sizeof late);
        late.struct_size = sizeof late;
        late.kind = GPTPS_EV_DEAD_LETTERED;
        late.handle = old_h; /* the superseded handle */
        late.status = GPTPS_E_TASK;
        observe(&late, NULL);
        REQUIRE(r3->state == 1);           /* still finished, not re-opened */
        REQUIRE(r3->closes == 1);          /* the close count never moved */
    }

    /* Every row closed exactly once under its current handle. */
    for (i = 0; i < ITEM_N; ++i)
        REQUIRE(g.r[i].closes == 1);

    printf("ledger: %d items, 5 recovered, bad-1 dead (1 close + 1 confirmation): PASS\n",
           ITEM_N);
    ok = 1;

cleanup:
    if (e) gptps_shutdown(e);
    printf("item_ledger: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
