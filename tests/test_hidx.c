/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_hidx.c - the handle index, white-box, against a reference model.
 *
 * gptps_cancel finds its item through an open-addressing index (see "finding an
 * item" in src/engine.c). Deleting from it leaves no tombstone: the gap is closed by
 * moving later entries of the probe run back. Get that wrong and a lookup stops at
 * a hole and reports a live item as gone - a cancel that silently does nothing - or
 * finds one that was freed. So this drives the engine's own index functions with
 * random inserts, deletes, lookups, growth and shrinking, on small tables where runs
 * wrap past the end, and after every step checks two things against a plain array
 * of what should be there: every live handle is found, with its item, and no
 * deleted one is; and no probe run has a hole in it.
 *
 * It includes engine.c directly - the index is static - and links the rest of the
 * core beside it.
 */
#include "../src/engine.c"
#include <stdio.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

enum { MAXLIVE = 700, OPS = 300000 };

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next_rand(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}

static gptps_item items[MAXLIVE];   /* the model: items[0 .. nlive) are in the index */
static size_t     nlive;

/* Every occupied slot is reachable from its home without crossing an empty one. */
static int runs_have_no_holes(const gptps *e)
{
    size_t j, mask = e->nhidx - 1, occupied = 0;
    for (j = 0; j < e->nhidx; ++j) {
        size_t k;
        if (!e->hidx[j].h) continue;
        ++occupied;
        for (k = hidx_slot(e->hidx[j].h, e->nhidx); k != j; k = (k + 1) & mask)
            if (!e->hidx[k].h) return 0;
    }
    return occupied == e->hidx_live;
}

static int model_agrees(gptps *e, const gptps_handle *gone, size_t ngone)
{
    size_t i;
    for (i = 0; i < nlive; ++i) {
        gptps_hslot *s = hidx_find(e, items[i].handle);
        if (!s || s->it != &items[i]) return 0;
    }
    for (i = 0; i < ngone; ++i)
        if (hidx_find(e, gone[i])) return 0;
    return e->hidx_live == nlive;
}

int main(void)
{
    static gptps e;                  /* only the index fields are used */
    static gptps_handle gone[64];
    size_t ngone = 0, op, bad_runs = 0, bad_model = 0, peak_slots = 0;
    gptps_handle next = 1;
    long inserts = 0, deletes = 0, rebuilds_seen = 0;
    size_t last_n = 0;

    memset(&e, 0, sizeof e);
    for (op = 0; op < OPS; ++op) {
        unsigned r = (unsigned)(next_rand() % 100);
        /* Phases: fill towards MAXLIVE, then drain towards 0, so the table both
         * grows and shrinks (hidx_trim) many times, with churn at every size. */
        int filling = ((op / 20000) % 2) == 0;
        if (nlive < MAXLIVE && (nlive == 0 || r < (filling ? 60u : 35u))) {
            gptps_item *it = &items[nlive];
            memset(it, 0, sizeof *it);
            next += 1 + next_rand() % 7;        /* gaps vary the homes */
            it->handle = next;
            if (hidx_reserve(&e) != 0) { printf("FAIL: hidx_reserve out of memory\n"); return 1; }
            hidx_put(&e, it);
            ++nlive; ++inserts;
        } else if (nlive) {
            size_t v = (size_t)(next_rand() % nlive);
            gptps_handle h = items[v].handle;
            hidx_del(&e, &items[v]);
            CHECK(!items[v].indexed);
            hidx_del(&e, &items[v]);            /* idempotent */
            items[v] = items[nlive - 1];        /* keep the model dense... */
            --nlive;
            if (v < nlive) {                    /* ...and the index pointing at the moved item */
                gptps_hslot *s = hidx_find(&e, items[v].handle);
                if (s) s->it = &items[v];
            }
            gone[ngone++ % 64] = h;
            ++deletes;
        }
        if (op % 7 == 0) hidx_trim(&e);         /* what each dispatcher pass does */
        if (e.nhidx != last_n) { ++rebuilds_seen; last_n = e.nhidx; }
        if (e.nhidx > peak_slots) peak_slots = e.nhidx;
        if (!runs_have_no_holes(&e)) ++bad_runs;
        if (!model_agrees(&e, gone, ngone < 64 ? ngone : 64)) ++bad_model;
        if (bad_runs + bad_model > 5) break;
    }
    printf("%ld inserts, %ld deletes; table sizes changed %ld times, peak %lu slots\n",
           inserts, deletes, rebuilds_seen, (unsigned long)peak_slots);
    if (bad_runs)  printf("FAIL: a probe run had a hole in it, %lu times\n", (unsigned long)bad_runs);
    if (bad_model) printf("FAIL: the index disagreed with the model, %lu times\n", (unsigned long)bad_model);
    CHECK(bad_runs == 0 && bad_model == 0);
    CHECK(rebuilds_seen > 10);                  /* it really did grow and shrink */
    gptps_free(e.hidx);
    printf("test_hidx: %s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
