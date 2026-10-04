/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * fuzz_journal.c - the durable queue's journal reader, under a fuzzer.
 *
 * The input is a journal file. gptps_dq_open replays it - resyncing past damage,
 * copying a damaged file aside - and compacts it; then the queue recovers what it
 * read, and the engine runs that, so the observer appends its markers. Each input
 * is read twice: as it is, and with every record's checksum made right, so that a
 * mutation of a length, a type or a seq reaches what the record means, not only the
 * check that turns it away. Held to:
 *   - the journal a compaction wrote reads back clean - nothing reported damaged -
 *     to the same pending and quarantined counts;
 *   - every verdict the engine gives a record's work closes the record: the queue's
 *     counts move by exactly what the verdicts say;
 *   - after the records ran and their markers were appended, the journal still
 *     reads back clean, to the counts the queue had when it closed.
 */
#include "gptps.h"
#include "gptps_durable_queue.h"
#include "fuzz_common.h"

#define REC_MAGIC 0x44515231u   /* "DQR1", little-endian, as the journal has it */

static unsigned g_damaged;   /* "is damaged" warnings since the last reset */

static void sink(gptps_log_level lvl, const char *msg, void *ud)
{
    (void)lvl; (void)ud;
    if (strstr(msg, "is damaged")) ++g_damaged;
}

/* ---- the journal's work in this run, and the verdicts on it ----
 * Its handles: recovered (the resubmit callback names each) or submitted here. A
 * verdict - FINISHED, a DROPPED or DEAD_LETTERED that teardown did not impose, a FAILED
 * its own body cancelled - closes the record, and a dead letter quarantines it. */
static gptps_handle *g_mine;
static size_t        g_nmine, g_capmine;
static size_t        g_closed, g_quarantining;

static void mine_add(gptps_handle h)
{
    if (g_nmine == g_capmine) {
        size_t nc = g_capmine ? g_capmine * 2 : 64;
        gptps_handle *grown = (gptps_handle *)realloc(g_mine, nc * sizeof *grown);
        if (!grown) return;
        g_mine = grown; g_capmine = nc;
    }
    g_mine[g_nmine++] = h;
}

static int mine_has(gptps_handle h)
{
    size_t i;
    for (i = 0; i < g_nmine; ++i) if (g_mine[i] == h) return 1;
    return 0;
}

static void resubmitted(const char *name, const void *payload, size_t len, gptps_handle h, void *ud)
{
    (void)name; (void)payload; (void)len; (void)ud;
    mine_add(h);
}

static int has_flag(const gptps_event *ev, uint32_t f)
{
    return ev->struct_size >= offsetof(gptps_event, flags) + sizeof ev->flags && (ev->flags & f) != 0;
}

static void verdicts(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (!mine_has(ev->handle)) return;
    switch (ev->kind) {
        case GPTPS_EV_FINISHED:      ++g_closed; break;
        case GPTPS_EV_DROPPED:       if (!has_flag(ev, GPTPS_EV_FLAG_SHUTDOWN)) ++g_closed; break;
        case GPTPS_EV_DEAD_LETTERED: if (!has_flag(ev, GPTPS_EV_FLAG_SHUTDOWN)) { ++g_closed; ++g_quarantining; } break;
        case GPTPS_EV_FAILED:
            if (ev->status == GPTPS_E_CANCELLED && has_flag(ev, GPTPS_EV_FLAG_SELF_CANCELLED)) ++g_closed;
            break;
        default: break;
    }
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* Every record's checksum made right: at a record magic whose lengths fit what is
 * left, the FNV-1a of its bytes goes where the reader looks for it and the walk
 * steps past the record; anywhere else it steps a byte. */
static void repair_checksums(uint8_t *b, size_t n)
{
    size_t at = 8;                                  /* past the file header */
    while (at + 24 <= n) {
        if (rd32(b + at) == REC_MAGIC) {
            size_t nlen = (size_t)b[at + 6] | ((size_t)b[at + 7] << 8);
            size_t plen = rd32(b + at + 8);
            if (nlen <= 4096u && plen <= n && nlen + plen + 24 <= n - at) {
                uint32_t h = fz_hash(b + at, 20 + nlen + plen);
                size_t c = at + 20 + nlen + plen;
                b[c] = (uint8_t)h; b[c + 1] = (uint8_t)(h >> 8); b[c + 2] = (uint8_t)(h >> 16); b[c + 3] = (uint8_t)(h >> 24);
                at += 24 + nlen + plen;
                continue;
            }
        }
        ++at;
    }
}

/* A task the journal's records can name. The payload's first byte picks how the
 * attempt ends, so a record can finish, fail into a dead letter or a drop, or
 * cancel itself - each closes it with a different marker. */
static gptps_status run(gptps_ctx *ctx, void *ud)
{
    size_t len = 0;
    const unsigned char *p = (const unsigned char *)gptps_payload(ctx, &len);
    (void)ud;
    switch (len ? p[0] % 4u : 0u) {
        case 1:  return GPTPS_E_TASK;
        case 2:  return GPTPS_E_CANCELLED;
        case 3:  (void)gptps_result_set(ctx, p, len); return GPTPS_OK;
        default: return GPTPS_OK;
    }
}

static gptps *engine(void)
{
    static const struct { const char *name; gptps_on_failure on_failure; uint32_t retries; } T[] = {
        { "t", GPTPS_ON_FAILURE_DEAD_LETTER, 0 },
        { "d", GPTPS_ON_FAILURE_DROP,        0 },
        { "r", GPTPS_ON_FAILURE_DEAD_LETTER, 1 },
    };
    gptps_config cfg;
    gptps *e = NULL;
    size_t i;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.mode = GPTPS_RUN_MANUAL;              /* no threads: the steps below run the records */
    if (gptps_open_ex(&cfg, &e) != GPTPS_OK) return NULL;
    for (i = 0; i < sizeof T / sizeof T[0]; ++i) {
        gptps_task_def d;
        memset(&d, 0, sizeof d);
        d.struct_size = sizeof d;
        d.name = T[i].name;
        d.exec = GPTPS_EXEC_INPROC;
        d.run = run;
        d.default_policy.struct_size = sizeof d.default_policy;
        d.default_policy.on_failure = T[i].on_failure;
        d.default_policy.max_retries = T[i].retries;
        (void)gptps_register_task(e, &d);
    }
    return e;
}

static void cleanup(const char *path)
{
    char p[600];
    int k;
    remove(path);
    snprintf(p, sizeof p, "%s.tmp", path);     remove(p);
    snprintf(p, sizeof p, "%s.corrupt", path); remove(p);
    for (k = 1; k < 10; ++k) { snprintf(p, sizeof p, "%s.corrupt.%d", path, k); remove(p); }
}

static void drained(const char *name, const void *payload, size_t len, void *ud)
{
    (void)name; (void)payload; (void)len; (void)ud;
}

static void run_journal(const char *path, const uint8_t *data, size_t size, uint32_t h)
{
    gptps *e;
    gptps_dq *dq;
    size_t pending, quarantined, i, ran;
    cleanup(path);
    if (!fz_write(path, data, size)) return;

    /* the input as it is: replayed and compacted */
    if (!(e = engine())) return;
    dq = gptps_dq_open(e, path);
    if (!dq) { gptps_shutdown(e); cleanup(path); return; }   /* a header it does not know */
    pending = gptps_dq_pending(dq);
    quarantined = gptps_dq_quarantined(dq);
    gptps_shutdown(e);
    gptps_dq_close(dq);

    /* the compacted journal: clean, and the same */
    g_damaged = 0;
    if (!(e = engine())) { cleanup(path); return; }
    dq = gptps_dq_open(e, path);
    FZ_ASSERT(dq != NULL, "the journal a compaction wrote does not open");
    FZ_ASSERT(g_damaged == 0, "the journal a compaction wrote reads as damaged");
    FZ_ASSERT(gptps_dq_pending(dq) == pending, "the compacted journal has another count of pending records");
    FZ_ASSERT(gptps_dq_quarantined(dq) == quarantined, "the compacted journal has another count of quarantined records");

    /* what it recovered, run: the observer appends 'S', 'F', 'D' and 'Q' markers;
     * and new work after it, whose seqs follow on from the journal's */
    g_nmine = 0; g_closed = g_quarantining = 0;
    (void)gptps_register_observer(e, verdicts, NULL);
    (void)gptps_dq_set_resubmit_cb(dq, resubmitted, NULL);
    (void)gptps_dq_recover(dq);
    {   /* a task this engine does not have is refused once journaled, and closed */
        static const unsigned char pl[2] = { 0, 1 };
        gptps_handle hs = 0;
        if (gptps_dq_submit(dq, (h & 16u) ? "nope" : "t", pl, 1, &hs) == GPTPS_OK) {
            ++pending;
            mine_add(hs);
            if ((h & 4u) && gptps_dq_cancel(dq, hs) == GPTPS_OK) --pending;   /* retracted: closed */
        }
        if (h & 8u) {
            gptps_dq_item items[2];
            memset(items, 0, sizeof items);
            items[0].task_name = "d"; items[0].payload = pl + 1; items[0].len = 1;
            items[1].task_name = (h & 32u) ? "nope" : "t"; items[1].payload = pl; items[1].len = 2;
            if (gptps_dq_submit_batch(dq, items, 2) == GPTPS_OK)
                for (i = 0; i < 2; ++i) if (items[i].status == GPTPS_OK) { ++pending; mine_add(items[i].handle); }
        }
    }
    for (i = 0; i < 64; ++i) {
        ran = 0;
        if (gptps_step(e, &ran) != GPTPS_OK || ran == 0) break;
    }
    FZ_ASSERT(gptps_dq_pending(dq) == pending - g_closed, "a verdict on a record's work did not close the record");
    FZ_ASSERT(gptps_dq_quarantined(dq) == quarantined + g_quarantining, "a dead letter did not quarantine its record");
    if (h & 1u) (void)gptps_dq_drain_quarantine(dq, drained, NULL);
    if (h & 2u) (void)gptps_dq_compact(dq);
    gptps_shutdown(e);                        /* what was left unstepped stays pending */
    pending = gptps_dq_pending(dq);
    quarantined = gptps_dq_quarantined(dq);
    gptps_dq_close(dq);

    /* ...and that journal, as the next run would read it */
    g_damaged = 0;
    if (!(e = engine())) { cleanup(path); return; }
    dq = gptps_dq_open(e, path);
    FZ_ASSERT(dq != NULL, "the journal does not open after its records ran");
    FZ_ASSERT(g_damaged == 0, "the journal reads as damaged after its records ran");
    FZ_ASSERT(gptps_dq_pending(dq) == pending, "the next run reads another count of pending records");
    FZ_ASSERT(gptps_dq_quarantined(dq) == quarantined, "the next run reads another count of quarantined records");
    gptps_shutdown(e);
    gptps_dq_close(dq);
    cleanup(path);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static int inited = 0;
    char path[512];
    uint8_t *fixed;
    uint32_t h;
    if (size > FZ_MAX_INPUT) return 0;
    if (!inited) { gptps_set_log_sink(sink, NULL); inited = 1; }
    fz_path(path, sizeof path, "fuzz.journal");
    h = fz_hash(data, size);
    run_journal(path, data, size, h);
    fixed = (uint8_t *)malloc(size ? size : 1);
    if (!fixed) return 0;
    if (size) memcpy(fixed, data, size);
    repair_checksums(fixed, size);
    if (size && memcmp(fixed, data, size)) run_journal(path, fixed, size, h >> 2);
    free(fixed);
    return 0;
}
