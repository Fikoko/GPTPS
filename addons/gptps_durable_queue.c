/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * durable_queue.c - crash-durable submission for GPTPS (see durable_queue.h).
 *
 * Append-only binary journal, no external dependency:
 *   file header : [u32 magic "GDQ1"][u32 version]
 *   record      : [u32 magic "DQR1"][u8 type 'P'|'D'][u8 pad][u16 name_len]
 *                 [u32 payload_len][u64 seq][name][payload][u32 fnv1a]
 * A 'P'(ending) record is fsync'd on write (durability); a 'D'(one) record is
 * only buffered (losing it on a crash just replays a completed task - harmless
 * under the at-least-once contract). Replay stops at the first torn/short/bad-
 * checksum record (a crash mid-write), so a partial tail never corrupts state.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "gptps_durable_queue.h"
#include "addon_compat.h"   /* portable mutex + fsync */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define DQ_FILE_MAGIC 0x47445131u /* "GDQ1" */
#define DQ_REC_MAGIC  0x44515231u /* "DQR1" */
#define DQ_VERSION    1u
#define DQ_FHDR_LEN   8
#define DQ_RHDR_LEN   20
#define DQ_FNV_SEED   2166136261u
#define DQ_MAX_NAME   4096u
#define DQ_MAX_PAYLOAD (256u * 1024u * 1024u)

typedef struct {
    uint64_t     seq;
    gptps_handle handle;   /* 0 until submitted/recovered this run */
    char        *name;
    void        *payload;
    size_t       len;
    int          done;        /* terminal + discarded (finished or dropped) */
    int          quarantined; /* terminal + RETAINED (dead-lettered): poison kept for inspection */
} dq_rec;

struct gptps_dq {
    gptps          *e;
    char           *path;
    FILE           *fp;        /* append handle */
    apx_mutex       mu;
    uint64_t        next_seq;
    size_t          pending;   /* count of !done recs */
    dq_rec         *recs;
    size_t          n, cap;
};

/* ---- little-endian + checksum helpers ---- */
static void put16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v) { int i; for (i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
static void put64(unsigned char *p, uint64_t v) { int i; for (i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
static uint16_t get16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t get64(const unsigned char *p) { uint64_t v = 0; int i; for (i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i); return v; }
static uint32_t fnv(const void *d, size_t n, uint32_t h)
{ const unsigned char *p = (const unsigned char *)d; while (n--) { h ^= *p++; h *= 16777619u; } return h; }

static char *dup_str(const char *s) { size_t n = strlen(s) + 1; char *o = (char *)malloc(n); if (o) memcpy(o, s, n); return o; }
static void *dup_mem(const void *s, size_t n) { void *o; if (!n) return NULL; o = malloc(n); if (o) memcpy(o, s, n); return o; }

/* fsync the directory holding `path` so a rename of a file in it is durable. */
static int fsync_parent_dir(const char *path)
{
    const char *slash = strrchr(path, '/');
    int rc;
    if (!slash) return apx_dir_fsync(".");
    {
        size_t n = (size_t)(slash - path);
        char *dir = (char *)malloc(n + 1);
        if (!dir) return -1;
        memcpy(dir, path, n); dir[n] = 0;
        rc = apx_dir_fsync(n ? dir : "/");
        free(dir);
    }
    return rc;
}

/* ---- record I/O ---- */
static void write_file_header(FILE *f)
{
    unsigned char h[DQ_FHDR_LEN];
    put32(h, DQ_FILE_MAGIC); put32(h + 4, DQ_VERSION);
    fwrite(h, 1, sizeof h, f);
}

/* Append one record. Returns 0 on success. Does not flush/fsync.
 *
 * The size bounds are enforced HERE, on the write side, because they are the
 * reader's bounds: replay() treats nlen > DQ_MAX_NAME or plen > DQ_MAX_PAYLOAD as
 * a torn record and stops, which would discard this record AND every valid record
 * appended after it. Making the writer's contract the reader's contract by
 * construction is what keeps the "a partial tail never corrupts state" invariant
 * true; a caller-side check alone regresses the moment a new caller appears. */
static int write_record(FILE *f, char type, uint64_t seq,
                        const char *name, const void *payload, size_t plen)
{
    size_t nl = name ? strlen(name) : 0;
    uint16_t nlen;
    size_t total;
    unsigned char *buf;
    uint32_t crc;
    size_t w;
    if (!f) return -1;
    if (nl > DQ_MAX_NAME || plen > DQ_MAX_PAYLOAD) return -1; /* replay would call it torn */
    if (plen && !payload) return -1;   /* a failed dup_mem: would memcpy from NULL */
    nlen = (uint16_t)nl;
    total = DQ_RHDR_LEN + nlen + plen + 4;
    buf = (unsigned char *)malloc(total);
    if (!buf) return -1;
    put32(buf, DQ_REC_MAGIC);
    buf[4] = (unsigned char)type; buf[5] = 0;
    put16(buf + 6, nlen);
    put32(buf + 8, (uint32_t)plen);
    put64(buf + 12, seq);
    if (nlen) memcpy(buf + DQ_RHDR_LEN, name, nlen);
    if (plen) memcpy(buf + DQ_RHDR_LEN + nlen, payload, plen);
    crc = fnv(buf, DQ_RHDR_LEN + nlen + plen, DQ_FNV_SEED);
    put32(buf + DQ_RHDR_LEN + nlen + plen, crc);
    w = fwrite(buf, 1, total, f);
    free(buf);
    return (w == total) ? 0 : -1;
}

/* Roll `f` back to `start` bytes and clear stdio's sticky error flag.
 * Called after a failed append. Two things are being repaired:
 *  1. The PARTIAL record a short write left behind. Replay stops at the first
 *     record whose magic or checksum does not verify, so a torn record sitting in
 *     the MIDDLE of the journal silently discards every valid record after it.
 *  2. The error indicator itself, which stdio latches - without clearing it the
 *     stream keeps refusing writes even once the condition (a full disk, say) is
 *     gone, so one transient ENOSPC bricked the queue for the process's lifetime.
 * The DQ_FHDR_LEN floor is a safety net, not an optimisation: every legitimate
 * rollback point is at or past the file header (do_rewrite always writes it
 * first), so an offset below it means ftell lied - an append-mode stream that has
 * not been repositioned reports 0 on the Windows CRT, and ftell returns -1 past
 * LONG_MAX on 32-bit builds. Truncating on such an offset would erase the whole
 * journal; refusing leaves at worst a torn tail, which replay() already
 * handles by stopping at it. */
static void rollback_to(FILE *f, long start)
{
    if (!f) return;
    clearerr(f);
    if (start < DQ_FHDR_LEN) return;
    fflush(f);
    clearerr(f);
    if (apx_truncate(f, start) == 0) fseek(f, start, SEEK_SET);
    clearerr(f);
}

/* Append one record and make it durable, rolling the journal back to its previous
 * length if either step fails. Returns 0 on success. */
static int append_durable(FILE *f, char type, uint64_t seq,
                          const char *name, const void *payload, size_t plen)
{
    long start;
    if (!f) return -1;   /* a compaction that could not reopen the journal */
    /* Seek to EOF before sampling the rollback point. C99 leaves an append
     * stream's initial position implementation-defined and the Windows CRT
     * documents it as the START of the file until the first I/O, so the first
     * append after open/compact would otherwise roll back to offset 0 - i.e.
     * truncate every recovered record away on a transient ENOSPC. */
    fseek(f, 0, SEEK_END);
    start = ftell(f);
    if (write_record(f, type, seq, name, payload, plen) != 0) { rollback_to(f, start); return -1; }
    if (fflush(f) != 0 || apx_fsync(f) != 0)                  { rollback_to(f, start); return -1; }
    return 0;
}

/* Append a state MARKER ('D'one / 'Q'uarantined) and flush it. A lost marker is
 * harmless (replay just re-runs or re-quarantines that record), but a TORN one is
 * not - it would truncate the journal's tail at replay - so this rolls back too. */
static void append_marker(FILE *f, char type, uint64_t seq)
{
    long start;
    if (!f) return;   /* lost marker: harmless, replay just re-runs the record */
    fseek(f, 0, SEEK_END);   /* see append_durable */
    start = ftell(f);
    if (write_record(f, type, seq, "", NULL, 0) != 0 || fflush(f) != 0) rollback_to(f, start);
}

/* ---- in-memory record list ---- */
static dq_rec *push_rec(gptps_dq *dq)
{
    if (dq->n == dq->cap) {
        size_t nc = dq->cap ? dq->cap * 2 : 16;
        dq_rec *nr = (dq_rec *)realloc(dq->recs, nc * sizeof *nr);
        if (!nr) return NULL;
        dq->recs = nr; dq->cap = nc;
    }
    memset(&dq->recs[dq->n], 0, sizeof dq->recs[dq->n]);
    return &dq->recs[dq->n++];
}

/* dq->recs is ASCENDING in seq by construction - gptps_dq_submit takes seq from a
 * monotonic counter under dq->mu before appending, replay() pushes records in file
 * order, and do_rewrite's compaction is stable - so this binary search is exact.
 * It has to be a search, not a scan: replay() calls it once per 'D'/'Q' marker over
 * an array that grows with every 'P' record, which made opening a long-lived
 * journal quadratic (200k records took ~11s before this). */
static dq_rec *find_by_seq(gptps_dq *dq, uint64_t seq)
{
    size_t lo = 0, hi = dq->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (dq->recs[mid].seq == seq) return &dq->recs[mid];
        if (dq->recs[mid].seq < seq) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

/* Replay the journal at dq->path into dq->recs. Returns 0 (ok / missing file) or
 * -1 (present but corrupt header). Stops at the first torn record. */
static int replay(gptps_dq *dq)
{
    FILE *f = fopen(dq->path, "rb");
    unsigned char fh[DQ_FHDR_LEN], hdr[DQ_RHDR_LEN];
    size_t r;
    if (!f) return 0;                                 /* no journal yet */
    r = fread(fh, 1, DQ_FHDR_LEN, f);
    if (r == 0) { fclose(f); return 0; }              /* empty file */
    if (r < DQ_FHDR_LEN || get32(fh) != DQ_FILE_MAGIC || get32(fh + 4) != DQ_VERSION) {
        fclose(f); return -1;                         /* corrupt header */
    }
    for (;;) {
        uint16_t nlen; uint32_t plen, crc_stored, crc; uint64_t seq; char type;
        unsigned char *body; size_t blen;
        r = fread(hdr, 1, DQ_RHDR_LEN, f);
        if (r != DQ_RHDR_LEN) break;                  /* clean EOF or torn header */
        if (get32(hdr) != DQ_REC_MAGIC) break;
        type = (char)hdr[4]; nlen = get16(hdr + 6); plen = get32(hdr + 8); seq = get64(hdr + 12);
        if (nlen > DQ_MAX_NAME || plen > DQ_MAX_PAYLOAD) break; /* implausible => torn */
        blen = (size_t)nlen + plen + 4;
        body = (unsigned char *)malloc(blen);
        if (!body) break;
        if (fread(body, 1, blen, f) != blen) { free(body); break; } /* torn tail */
        crc_stored = get32(body + nlen + plen);
        crc = fnv(hdr, DQ_RHDR_LEN, DQ_FNV_SEED);
        crc = fnv(body, (size_t)nlen + plen, crc);
        if (crc != crc_stored) { free(body); break; } /* corrupt => stop */
        if (seq >= dq->next_seq) dq->next_seq = seq + 1;
        if (type == 'P') {
            dq_rec *rc = push_rec(dq);
            if (!rc) { free(body); break; }            /* OOM: stop, as with a torn tail */
            rc->seq = seq; rc->done = 0; rc->handle = 0; rc->len = plen;
            rc->name = (char *)malloc((size_t)nlen + 1);
            if (rc->name) { if (nlen) memcpy(rc->name, body, nlen); rc->name[nlen] = 0; }
            rc->payload = dup_mem(body + nlen, plen);
            if (!rc->name || (plen && !rc->payload)) {
                /* A half-initialised record is worse than a missing one: the
                 * do_rewrite below would persist it with an empty name (never
                 * recoverable, never dropped - it leaks in the journal forever).
                 * Pop it and stop, the same policy as a record we cannot trust. */
                free(rc->name); free(rc->payload);
                dq->n -= 1;
                free(body);
                break;
            }
        } else if (type == 'D') {
            dq_rec *rc = find_by_seq(dq, seq);
            if (rc) {
                rc->done = 1;
                /* Release the completed record's buffers now instead of leaving
                 * them to do_rewrite: replaying a journal of a million completed
                 * records would otherwise hold every payload in RAM at once.
                 * NULLing is mandatory - the cleanup paths free these again. */
                free(rc->name); free(rc->payload);
                rc->name = NULL; rc->payload = NULL; rc->len = 0;
            }
        } else if (type == 'Q') {
            dq_rec *rc = find_by_seq(dq, seq);
            if (rc) rc->quarantined = 1;   /* dead-lettered: retained, not re-submitted */
        }
        free(body);
    }
    fclose(f);
    return 0;
}

/* rename() over an EXISTING file is undefined in C99 and fails outright on
 * Windows, where the CRT reports EEXIST - which would make every compaction fail
 * there, and with it gptps_dq_open on any pre-existing journal (crash recovery,
 * this add-on's whole point). MoveFileExA with MOVEFILE_REPLACE_EXISTING is the
 * documented atomic replace. */
static int dq_rename_replace(const char *from, const char *to)
{
#if defined(_WIN32)
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(from, to);
#endif
}

/* Rewrite the journal to contain only still-pending records, then reopen the
 * append handle and drop completed records from memory. Caller serializes. */
static gptps_status do_rewrite(gptps_dq *dq)
{
    size_t tn = strlen(dq->path) + 5, i, keep = 0;
    char *tmp = (char *)malloc(tn);
    FILE *t;
    if (!tmp) return GPTPS_E_NOMEM;
    snprintf(tmp, tn, "%s.tmp", dq->path);
    t = fopen(tmp, "wb");
    if (!t) { free(tmp); return GPTPS_E_IO; }
    write_file_header(t);
    for (i = 0; i < dq->n; ++i) {
        if (dq->recs[i].done) continue;
        /* retain both still-pending and quarantined (dead-lettered) records; a
         * quarantined one is rewritten as P (to keep its poison payload) plus a Q
         * marker so replay reclassifies it without re-submitting it. */
        if (write_record(t, 'P', dq->recs[i].seq, dq->recs[i].name, dq->recs[i].payload, dq->recs[i].len) != 0 ||
            (dq->recs[i].quarantined && write_record(t, 'Q', dq->recs[i].seq, "", NULL, 0) != 0)) {
            fclose(t); remove(tmp); free(tmp); return GPTPS_E_IO;
        }
    }
    if (fflush(t) != 0 || apx_fsync(t) != 0) { fclose(t); remove(tmp); free(tmp); return GPTPS_E_IO; }
    fclose(t);

    if (dq->fp) { fclose(dq->fp); dq->fp = NULL; }
    if (dq_rename_replace(tmp, dq->path) != 0) {
        /* The original journal is untouched on disk (the rename never happened),
         * so put the append handle back before reporting the error: this return
         * is documented as an ordinary recoverable GPTPS_E_IO, and leaving
         * dq->fp NULL would turn the caller's next submit - or the next terminal
         * event, on an engine worker thread - into a NULL-FILE* crash. */
        remove(tmp); free(tmp);
        dq->fp = fopen(dq->path, "ab");
        if (dq->fp) fseek(dq->fp, 0, SEEK_END);
        return GPTPS_E_IO;
    }
    fsync_parent_dir(dq->path);   /* make the rename's directory entry durable */
    free(tmp);
    dq->fp = fopen(dq->path, "ab");
    if (!dq->fp) return GPTPS_E_IO;   /* last resort; append_* degrade to an error */
    fseek(dq->fp, 0, SEEK_END);       /* an append stream's position is not portable */

    /* compact memory: drop done records, keep pending + quarantined */
    for (i = 0; i < dq->n; ++i) {
        if (dq->recs[i].done) { free(dq->recs[i].name); free(dq->recs[i].payload); }
        else dq->recs[keep++] = dq->recs[i];
    }
    dq->n = keep;
    return GPTPS_OK;
}

/* ---- observer: mark a record done when its task terminates ---- */
static void dq_on_event(const gptps_event *ev, void *ud)
{
    gptps_dq *dq = (gptps_dq *)ud;
    size_t i;
    if (ev->kind != GPTPS_EV_FINISHED && ev->kind != GPTPS_EV_DROPPED &&
        ev->kind != GPTPS_EV_DEAD_LETTERED) return;
    apx_mutex_lock(&dq->mu);
    for (i = 0; i < dq->n; ++i) {
        if (dq->recs[i].done || dq->recs[i].quarantined) continue;
        if (dq->recs[i].handle != ev->handle) continue;
        if (ev->kind == GPTPS_EV_DEAD_LETTERED) {
            /* dead-lettered: RETAIN the poison payload (quarantine), don't drop it */
            dq->recs[i].quarantined = 1;
            append_marker(dq->fp, 'Q', dq->recs[i].seq);
        } else {
            /* finished or dropped: terminally gone, discard */
            dq->recs[i].done = 1;
            append_marker(dq->fp, 'D', dq->recs[i].seq);
        }
        if (dq->pending) dq->pending -= 1;
        break;
    }
    apx_mutex_unlock(&dq->mu);
}

/* ---- public API ---- */
gptps_dq *gptps_dq_open(gptps *e, const char *journal_path)
{
    gptps_dq *dq;
    if (!e || !journal_path) return NULL;
    dq = (gptps_dq *)calloc(1, sizeof *dq);
    if (!dq) return NULL;
    apx_mutex_init(&dq->mu);
    dq->e = e; dq->next_seq = 1;
    dq->path = dup_str(journal_path);
    if (!dq->path) { apx_mutex_destroy(&dq->mu); free(dq); return NULL; }

    if (replay(dq) != 0) goto fail;          /* corrupt journal header */
    /* pending count after replay (quarantined records are retained, not pending) */
    { size_t i; for (i = 0; i < dq->n; ++i) if (!dq->recs[i].done && !dq->recs[i].quarantined) dq->pending += 1; }
    if (do_rewrite(dq) != GPTPS_OK) goto fail;
    if (gptps_register_observer(e, dq_on_event, dq) != GPTPS_OK) goto fail;
    return dq;

fail:
    if (dq->fp) fclose(dq->fp);
    { size_t i; for (i = 0; i < dq->n; ++i) { free(dq->recs[i].name); free(dq->recs[i].payload); } }
    free(dq->recs); free(dq->path);
    apx_mutex_destroy(&dq->mu);
    free(dq);
    return NULL;
}

gptps_status gptps_dq_submit(gptps_dq *dq, const char *task_name,
                             const void *payload, size_t len, gptps_handle *out_handle)
{
    gptps_status st;
    gptps_handle h = 0;
    dq_rec *rc;
    uint64_t seq;
    char *nm;
    void *pl;
    if (!dq || !task_name) return GPTPS_E_INVAL;
    /* Refuse anything the replayer would reject rather than writing it: a record
     * past these bounds is classified as a torn tail on the next open, which
     * discards it AND every record appended after it - while this call returned
     * GPTPS_OK and its durability promise. The bounds are exactly replay()'s. */
    if (len > DQ_MAX_PAYLOAD) return GPTPS_E_INVAL;
    if (strlen(task_name) > DQ_MAX_NAME) return GPTPS_E_INVAL;
    if (len && !payload) return GPTPS_E_INVAL;   /* would memcpy from NULL below */

    /* Duplicate BEFORE journaling. The reverse order has no rollback: a failed
     * dup_str leaves a persisted record whose in-memory name is NULL, and the
     * next compaction rewrites it with an empty name - unrecoverable, undroppable,
     * and counted as pending forever. dup_mem returns NULL for len == 0 by
     * design, so only a non-zero len makes a NULL payload an error. */
    nm = dup_str(task_name);
    pl = dup_mem(payload, len);
    if (!nm || (len && !pl)) { free(nm); free(pl); return GPTPS_E_NOMEM; }

    apx_mutex_lock(&dq->mu);
    seq = dq->next_seq;
    /* Durable before we enqueue: a swallowed fsync error would be a false
     * durability claim. On failure the journal is rolled back to its previous
     * length, so a transient full disk costs this one submit rather than every
     * submit for the rest of the process's life. */
    if (append_durable(dq->fp, 'P', seq, task_name, payload, len) != 0) {
        apx_mutex_unlock(&dq->mu);
        free(nm); free(pl);
        return GPTPS_E_IO;
    }
    dq->next_seq = seq + 1;

    rc = push_rec(dq);
    if (!rc) { apx_mutex_unlock(&dq->mu); free(nm); free(pl); return GPTPS_E_NOMEM; }
    rc->seq = seq; rc->done = 0; rc->handle = 0; rc->len = len;
    rc->name = nm;
    rc->payload = pl;
    dq->pending += 1;

    st = gptps_submit(dq->e, task_name, payload, len, &h);
    if (st == GPTPS_OK) {
        rc->handle = h;
        if (out_handle) *out_handle = h;
    } else {
        /* engine refused it: mark done so recovery won't replay a rejected task */
        rc->done = 1; if (dq->pending) dq->pending -= 1;
        append_marker(dq->fp, 'D', seq);
    }
    apx_mutex_unlock(&dq->mu);
    return st;
}

size_t gptps_dq_recover(gptps_dq *dq)
{
    size_t i, count = 0;
    if (!dq) return 0;
    apx_mutex_lock(&dq->mu);
    for (i = 0; i < dq->n; ++i) {
        gptps_handle h = 0;
        /* Quarantined records are TERMINAL-but-retained, not incomplete. Re-submitting
         * one re-runs the exact poison payload dead-lettering exists to contain, and it
         * never converges: dq_on_event skips an already-quarantined record, so no new Q
         * marker is written and the same payload runs again on every restart. */
        if (dq->recs[i].done || dq->recs[i].quarantined || dq->recs[i].handle != 0)
            continue; /* completed, quarantined, or already live */
        if (gptps_submit(dq->e, dq->recs[i].name, dq->recs[i].payload, dq->recs[i].len, &h) == GPTPS_OK) {
            dq->recs[i].handle = h;
            ++count;
        }
        /* on failure leave it pending: a later run with the task registered can recover it */
    }
    apx_mutex_unlock(&dq->mu);
    return count;
}

size_t gptps_dq_pending(gptps_dq *dq)
{
    size_t n;
    if (!dq) return 0;
    apx_mutex_lock(&dq->mu);
    n = dq->pending;
    apx_mutex_unlock(&dq->mu);
    return n;
}

size_t gptps_dq_quarantined(gptps_dq *dq)
{
    size_t i, n = 0;
    if (!dq) return 0;
    apx_mutex_lock(&dq->mu);
    for (i = 0; i < dq->n; ++i) if (dq->recs[i].quarantined) ++n;
    apx_mutex_unlock(&dq->mu);
    return n;
}

size_t gptps_dq_drain_quarantine(gptps_dq *dq, gptps_dq_quarantine_cb cb, void *user_data)
{
    size_t i, n = 0;
    if (!dq) return 0;
    apx_mutex_lock(&dq->mu);
    for (i = 0; i < dq->n; ++i) {
        if (!dq->recs[i].quarantined) continue;
        /* payload valid only for this call; cb must NOT re-enter this dq (lock held) */
        if (cb) cb(dq->recs[i].name, dq->recs[i].payload, dq->recs[i].len, user_data);
        dq->recs[i].quarantined = 0;
        dq->recs[i].done = 1;        /* drained => terminally gone */
        ++n;
    }
    /* Compact the drained records out of the journal. A failure here is not a
     * failed drain - cb has already seen every payload - and there is no channel
     * to report it through, so it is deliberately dropped: the journal simply
     * stays uncompacted and a restart re-quarantines these records for another
     * drain, which is this add-on's at-least-once contract applied to cb. */
    if (n) (void)do_rewrite(dq);
    apx_mutex_unlock(&dq->mu);
    return n;
}

gptps_status gptps_dq_compact(gptps_dq *dq)
{
    gptps_status st;
    if (!dq) return GPTPS_E_INVAL;
    apx_mutex_lock(&dq->mu);
    st = do_rewrite(dq);
    apx_mutex_unlock(&dq->mu);
    return st;
}

void gptps_dq_close(gptps_dq *dq)
{
    size_t i;
    if (!dq) return;
    /* Caller contract: the engine is already shut down, so no event fires here. */
    if (dq->fp) fclose(dq->fp);
    for (i = 0; i < dq->n; ++i) { free(dq->recs[i].name); free(dq->recs[i].payload); }
    free(dq->recs); free(dq->path);
    apx_mutex_destroy(&dq->mu);
    free(dq);
}
