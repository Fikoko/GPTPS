/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_durable_queue.c - crash-durable submission for GPTPS (see the .h).
 *
 * Append-only binary journal, no external dependency:
 *   file header : [u32 magic "GDQ1"][u32 version]
 *   record      : [u32 magic "DQR1"][u8 type][u8 pad][u16 name_len]
 *                 [u32 payload_len][u64 seq][name][payload][u32 fnv1a]
 * A 'P'(ending) record carries the work. Every other type is a marker about the 'P'
 * with the same seq, with no name and no payload except 'K':
 *   'D'(one), 'Q'(uarantined) - its verdict;
 *   'S'(tarted), 'F'(ailed)   - an attempt began, and ended without a verdict (see
 *                               "crash loops");
 *   'K' [u32 count]           - its crash count, carried across a compaction.
 * A reader skips a type it does not know, so this journal still opens in an older
 * version of this file (which only loses the crash counts), and an older one opens
 * here. A 'P' record is fsync'd before it is enqueued (durability); the markers are
 * flushed but not fsync'd. Losing one to a crash replays the record - a completed task
 * runs again, a dead-lettered one runs again (and is quarantined again only if it is
 * dead-lettered again), a refused submit is offered to the engine again - which the
 * at-least-once contract allows. The exception is the 'D' gptps_dq_cancel writes,
 * which is fsync'd: losing THAT one would re-run work the caller retracted. A record
 * torn by a crash at the end of the file is dropped, so a partial tail never corrupts
 * state; damage anywhere else is skipped, preserved and reported (see "reading a
 * damaged journal"). How the fsyncs are made without stalling the engine is "journal
 * writes and group commit".
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#  define _DARWIN_C_SOURCE   /* F_FULLFSYNC: see apx_fsync_fd in addon_compat.h */
#endif
#include "gptps_durable_queue.h"
#include "addon_compat.h"   /* portable mutex, condition variable + fsync */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>

#define DQ_FILE_MAGIC 0x47445131u /* "GDQ1" */
#define DQ_REC_MAGIC  0x44515231u /* "DQR1" */
#define DQ_VERSION    1u
#define DQ_FHDR_LEN   8
#define DQ_RHDR_LEN   20
#define DQ_FNV_SEED   2166136261u
#define DQ_MAX_NAME   4096u
#define DQ_MAX_PAYLOAD (256u * 1024u * 1024u)
/* Seqs count up from 1, one per record, so no journal gets near 2^63: a record with a
 * seq past it is damage (see replay), and a submit that would number one past it
 * returns GPTPS_E_FULL. The bound keeps next_seq from wrapping. A record at 2^64-1 - a
 * stray marker was enough - made it 0, so the next submit took seq 0 and appended it
 * after a higher one. With two or more records in the table, find_by_seq could not
 * find it, and gptps_dq_submit dereferenced the NULL it returned; with one, it was the
 * record at 2^64-1 the search missed, so no verdict closed it. */
#define DQ_MAX_SEQ    (UINT64_MAX >> 1)

/* ---- crash loops ----
 *
 * Quarantine exists so a payload that fails every time is not run forever, and it used
 * to rest on the DEAD_LETTERED event alone. A body that takes the whole process down -
 * a segfault, an abort, the OOM killer - never lets the engine send one: its record
 * stayed pending, the next run recovered it, and it killed that run too, for ever.
 *
 * So each attempt is bracketed in the journal: 'S' when it starts, 'F' when it ends
 * without closing the record ('D' and 'Q' close it). Neither is fsync'd, and neither
 * needs to be: a process that dies keeps what it wrote to the kernel, and power loss is
 * not what a crash loop is. A record whose last 'S' is unanswered was running when the
 * process died, and gptps_dq_open counts that against it ('K' carries the count through
 * compactions). An attempt that ends, however it ends, proves the record did not kill
 * its process that time, and resets the count.
 *
 * Counting alone would convict the innocent. Several records run at once, and recovery
 * replays them in the same order every time, so the records running beside a killer are
 * running at every one of its crashes too - and would reach any threshold with it. So:
 *   1 crash   - recovered as usual. One death proves nothing.
 *   2 crashes - a SUSPECT. Suspects are resubmitted one at a time, each once the
 *               previous one's attempt has ended (or its handle has, unrun), so a crash
 *               that follows is down to one suspect, or to work that was not suspect -
 *               which the crash then counts against, once. An innocent suspect finishes,
 *               or fails like anything else, and its count resets.
 *   3 crashes - quarantined at open instead of recovered, with a warning.
 * The price of attribution is time: a long suspect holds the next one back for as long
 * as it runs. It is paid only after a record has been running at two deaths. */
#define DQ_SUSPECT_AT    2u
#define DQ_QUARANTINE_AT 3u

typedef struct {
    uint64_t     seq;
    gptps_handle handle;      /* 0 until submitted/recovered this run */
    char        *name;
    void        *payload;
    size_t       len;
    int          done;        /* closed: finished, dropped, refused, retracted or drained */
    int          quarantined; /* terminal + RETAINED (dead-lettered): poison kept for inspection */
    int          inflight;    /* an attempt started ('S') and has not ended ('F', 'D', 'Q') */
    int          suspect;     /* resubmitted one at a time this run - see "crash loops" */
    uint32_t     crashes;     /* process deaths it was running at since an attempt last ended */
    int          committing;  /* gptps_dq_submit is making its 'P' durable: not a record yet */
    int          retracting;  /* gptps_dq_cancel calls making a 'D' for it durable */
    char         deferred;    /* the verdict ('D'/'Q') that arrived while it was retracting */
} dq_rec;

/* One slot of the handle index (see "handle index"). */
typedef struct {
    gptps_handle  h;
    uint64_t      seq;
    unsigned char state;      /* 0 empty, 1 in use, 2 deleted */
} dq_slot;

/* A write its caller must see made durable before going on: the 'P' of a submit, or the
 * 'D' of a retraction - or, once such a write has failed, the cut that took it back
 * out of the file (see cut_back). It lives on the caller's stack, and is linked into
 * the queue's list from the moment its bytes are written until the caller has acted on
 * how it ended - which is also what a compaction reads (see "journal writes and group
 * commit"). */
typedef struct dq_waiter {
    uint64_t          ticket;   /* write order */
    uint64_t          seq;      /* the records it is about: seq .. seq_end (a batch) */
    uint64_t          seq_end;
    char              type;     /* 'P' or 'D'; DQ_W_CUT for a cut */
    int               state;    /* DQ_W_* */
    struct dq_waiter *next;
} dq_waiter;
#define DQ_W_WAITING 0
#define DQ_W_DURABLE 1
#define DQ_W_FAILED  2
#define DQ_W_CUT     0          /* the type of a cut's waiter: no record type */

struct gptps_dq {
    gptps          *e;
    char           *path;
    apx_mutex       mu;        /* the table, the index, pending, trial, next_seq */
    uint64_t        next_seq;
    size_t          pending;   /* count of open recs: not done, not quarantined, not committing */
    dq_rec         *recs;
    size_t          n, cap;
    dq_slot        *slots;     /* handle -> seq of an OPEN record with a handle this run */
    size_t          nslots, live, dead, reserved;
    uint64_t        trial;     /* seq of the suspect resubmitted alone right now, or 0 */
    gptps_dq_resubmit_cb resub;   /* gptps_dq_set_resubmit_cb */
    void           *resub_ud;
    apx_mutex       jmu;       /* the journal - see "journal writes and group commit" */
    FILE           *fp;        /* JMU: append handle */
    int             broken;    /* JMU: nothing more is written - see "a broken queue" */
    int             broke_note;   /* JMU: it broke, and nobody has warned of it yet */
    apx_mutex       smu;       /* the group commit below */
    apx_cond        scv;       /* SMU: broadcast when an fsync ends */
    int             fd;        /* SMU, changed under JMU too: fp's descriptor, -1 with none */
    dq_waiter      *waiters;   /* SMU */
    uint64_t        tickets;   /* SMU: durable writes issued */
    long            last_end;  /* SMU: the file offset just past the newest durable write */
    long            synced;    /* SMU: every byte below it is on disk */
    int             syncing;   /* SMU: an fsync is running; its caller holds no lock */
    int             dir_dirty; /* SMU: a compaction's rename is not known durable yet */
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
static int write_file_header(FILE *f)   /* 0 on success */
{
    unsigned char h[DQ_FHDR_LEN];
    put32(h, DQ_FILE_MAGIC); put32(h + 4, DQ_VERSION);
    return fwrite(h, 1, sizeof h, f) == sizeof h ? 0 : -1;
}

/* Append one record. Returns 0 on success. Does not flush/fsync.
 *
 * The size bounds are enforced HERE, on the write side, because they are the
 * reader's bounds: replay() treats nlen > DQ_MAX_NAME or plen > DQ_MAX_PAYLOAD as
 * damage, which would cost this record AND, at the end of the file, every valid
 * record appended after it. Making the writer's contract the reader's contract by
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

/* Roll `f` back to `start` bytes and clear stdio's sticky error flag. Returns 0 once
 * the file ends at `start`, -1 if it could not be cut back there. The cut is not
 * durable until the next fsync (see cut_back).
 * Called after a failed append, and after an fsync that failed. Two things are being
 * repaired:
 *  1. The PARTIAL record a short write left behind. A torn record sitting in the
 *     MIDDLE of the journal is damage: replay skips it and reports it rather than
 *     stopping there, but the record it was meant to hold is gone either way.
 *  2. The error indicator itself, which stdio latches - without clearing it the
 *     stream keeps refusing writes even once the condition (a full disk, say) is
 *     gone, so one transient ENOSPC bricked the queue for the process's lifetime.
 * The DQ_FHDR_LEN floor is a safety net, not an optimisation: every legitimate
 * rollback point is at or past the file header (do_rewrite always writes it
 * first), so an offset below it means ftell lied - an append-mode stream that has
 * not been repositioned reports 0 on the Windows CRT, and ftell returns -1 past
 * LONG_MAX on 32-bit builds. Truncating on such an offset would erase the whole
 * journal; refusing leaves at worst a torn record, which replay() already
 * handles - for a marker. A write whose failure is about to be reported must not stay
 * in the file, so for that one a refusal breaks the queue (see cut_back). */
static int rollback_to(FILE *f, long start)
{
    int rc;
    if (!f) return -1;
    clearerr(f);
    if (start < DQ_FHDR_LEN) return -1;
    fflush(f);
    clearerr(f);
    rc = (apx_truncate(f, start) == 0 && fseek(f, start, SEEK_SET) == 0) ? 0 : -1;
    clearerr(f);
    return rc;
}

/* ---- journal writes and group commit ----
 *
 * One lock used to guard everything, the journal included, and gptps_dq_submit held it
 * across its fsync - a millisecond or more of disk. The observer takes that lock for
 * every event of every task, durable or not, on the engine's own worker and dispatcher
 * threads; so while one thread journaled work back-to-back, the engine all but stopped:
 * plain tasks fell from ~420,000/s to 2-13/s, and of 344 queued 20 ms jobs only 24-30
 * finished during a 3,000-item submit loop that, unhindered, lets all of them through.
 * The mutex is not fair, and the submitter took it back before any waiting worker could.
 *
 * So there are three locks now, always taken in this order and never the reverse:
 *   mu  - the record table, the handle index, pending, trial, next_seq; the observer's;
 *   jmu - the journal FILE*: every write and its position, a truncation, a compaction;
 *         held for the microseconds of a write, never across an fsync - but for the one
 *         that makes a failed fsync's cut durable, below;
 *   smu - the group commit: the waiter list and the offsets below.
 * An fsync runs with NO lock held, by whichever caller finds none running: it makes
 * durable everything written before it began, and settles every waiter that covers. So
 * submitters arriving together share one fsync instead of queueing for one each.
 *
 * An fsync that fails makes nothing after the last good one trustworthy - on Linux a
 * failed fsync may discard the very pages it could not write, and the next one then
 * succeeds - so the file is cut back to `synced` and every write still waiting fails
 * with GPTPS_E_IO, as a single write's failure always did: a submit that was not made
 * durable is not enqueued, and a retraction that was not stays undone. Markers written
 * past that point go too, which a lost marker is allowed to. While a compaction's
 * rename is not known durable, the fsync includes the directory's, and fails the same
 * way (see do_rewrite).
 *
 * But a cut is not durable either until an fsync says so, and the bytes it took out may
 * be: an fsync that began before them may have caught them anyway - it settles only
 * the writes made before it began - and so may the kernel's own writeback, at any time.
 * A power cut then brought back a write whose caller had been told it failed: a submit
 * that returned GPTPS_E_IO was recovered and ran. So a failure is reported only once the
 * cut that took the write out is durable. The failed fsync's cut is fsync'd before
 * anyone is failed, and a write that fails on its way into the file waits, as a cut
 * (cut_back), for the next fsync to begin after it. If even that cannot be done, the
 * queue breaks (see "a broken queue").
 *
 * A submit and a retraction take jmu before letting go of mu, so a compaction - which
 * holds both - never meets a record reserved but not yet written. It first settles
 * every write in flight (quiesce), then writes a committing record only if its 'P'
 * became durable, and drops a retracting one if its 'D' did: what the waiters were told
 * and what the rewritten file says always agree.
 *
 * ---- a broken queue ----
 *
 * What the queue cannot answer for: a write it could not take back out of the journal
 * durably - the cut failed, or the fsync after it did. Its bytes may then be read back
 * by the next open, though its caller is told it failed. From then on nothing more is
 * written, so nothing buries those bytes under valid records, and every durable call
 * fails with GPTPS_E_IO without writing, until a compaction succeeds: it rewrites the
 * journal from the table, which holds exactly what every caller was told, and once that
 * is durable nothing that failed can come back. A compaction that cannot reopen the
 * journal it put in place breaks the queue too: there is no file to write. Each break
 * is reported once, to the core's log sink, by the public call it happened in
 * (warn_if_broke), with no lock held. */

static void break_queue(gptps_dq *dq)   /* caller holds jmu */
{
    if (!dq->broken) dq->broke_note = 1;
    dq->broken = 1;
}

/* The warning, once per break. Caller holds no lock. */
static void warn_if_broke(gptps_dq *dq)
{
    char msg[512];
    int note;
    apx_mutex_lock(&dq->jmu);
    note = dq->broke_note;
    dq->broke_note = 0;
    apx_mutex_unlock(&dq->jmu);
    if (!note) return;
    snprintf(msg, sizeof msg,
             "gptps_durable_queue: %s: a write to the journal failed, and so did undoing it, "
             "or the journal could not be reopened. A call that failed then with GPTPS_E_IO "
             "may still be read back by the next gptps_dq_open. Every durable call fails "
             "until gptps_dq_compact succeeds.", dq->path);
    gptps_log(NULL, GPTPS_LOG_ERROR, msg);
}

/* Append a marker and flush it. A lost marker is harmless (replay just re-runs or
 * re-quarantines that record, or misses one crash), but a TORN one is damage, so this
 * rolls back too. Its cut need not be durable: whether the marker comes back or not,
 * nobody was told otherwise. Caller holds mu, not jmu. */
static void append_marker(gptps_dq *dq, char type, uint64_t seq)
{
    long start;
    apx_mutex_lock(&dq->jmu);
    if (dq->fp && !dq->broken) {   /* else lost: see "a broken queue" */
        fseek(dq->fp, 0, SEEK_END);   /* see write_durable */
        start = ftell(dq->fp);
        if (write_record(dq->fp, type, seq, "", NULL, 0) != 0 || fflush(dq->fp) != 0)
            (void)rollback_to(dq->fp, start);
    }
    apx_mutex_unlock(&dq->jmu);
}

/* Link `w` into the waiter list, for a write of `type` about seq .. seq_end that ends
 * the file at `end` (-1: a cut, which ends nothing). Caller holds jmu. */
static void register_waiter(gptps_dq *dq, dq_waiter *w, char type, uint64_t seq,
                            uint64_t seq_end, long end)
{
    apx_mutex_lock(&dq->smu);
    w->ticket = ++dq->tickets; w->seq = seq; w->seq_end = seq_end; w->type = type;
    w->state = DQ_W_WAITING;
    w->next = dq->waiters; dq->waiters = w;
    if (end >= 0) dq->last_end = end;
    apx_mutex_unlock(&dq->smu);
}

/* A durable write failed after `start`, where it began: cut the file back there, and
 * register `w` as the cut, so its caller waits for an fsync that began after it - one
 * that makes it durable - before reporting the failure (see "group commit"). -1 then;
 * -2 if the file could not be cut back, which breaks the queue, `w` not registered.
 * Caller holds jmu. */
static int cut_back(gptps_dq *dq, dq_waiter *w, long start, uint64_t seq, uint64_t seq_end)
{
    if (rollback_to(dq->fp, start) != 0) { break_queue(dq); return -2; }
    register_waiter(dq, w, DQ_W_CUT, seq, seq_end, -1);
    return -1;
}

/* Write a record that must become durable, and register `w` for it. Caller holds jmu.
 * 0: written, `w` registered for it. -1: the write failed, and `w` is registered for
 * the cut that undid it (cut_back). -2: nothing to wait for - a broken queue, which
 * writes nothing, or a cut that failed. Either way the caller waits for a registered
 * `w` (sync_until) and then unregisters it, and the call succeeded only if it returned
 * 0 and `w` became durable. */
static int write_durable(gptps_dq *dq, dq_waiter *w, char type, uint64_t seq,
                         const char *name, const void *payload, size_t plen)
{
    long start;
    if (!dq->fp || dq->broken) return -2;   /* see "a broken queue" */
    /* Seek to EOF before sampling the rollback point. C99 leaves an append
     * stream's initial position implementation-defined and the Windows CRT
     * documents it as the START of the file until the first I/O, so the first
     * append after open/compact would otherwise roll back to offset 0 - i.e.
     * truncate every recovered record away on a transient ENOSPC. */
    fseek(dq->fp, 0, SEEK_END);
    start = ftell(dq->fp);
    if (write_record(dq->fp, type, seq, name, payload, plen) != 0 || fflush(dq->fp) != 0)
        return cut_back(dq, w, start, seq, seq);
    register_waiter(dq, w, type, seq, seq, ftell(dq->fp));
    return 0;
}

/* write_durable for a batch: n 'P' records with seqs first .. first+n-1, one waiter for
 * all of them, so one fsync settles the batch. All of it, or - cut back - none.
 * Caller holds jmu. */
static int write_durable_batch(gptps_dq *dq, dq_waiter *w, const gptps_dq_item *items,
                               size_t n, uint64_t first)
{
    long start;
    size_t i;
    if (!dq->fp || dq->broken) return -2;
    fseek(dq->fp, 0, SEEK_END);       /* see write_durable */
    start = ftell(dq->fp);
    for (i = 0; i < n; ++i)
        if (write_record(dq->fp, 'P', first + i, items[i].task_name, items[i].payload, items[i].len) != 0)
            return cut_back(dq, w, start, first, first + n - 1);
    if (fflush(dq->fp) != 0) return cut_back(dq, w, start, first, first + n - 1);
    register_waiter(dq, w, 'P', first, first + n - 1, ftell(dq->fp));
    return 0;
}

static int any_waiting(const gptps_dq *dq)   /* caller holds smu */
{
    const dq_waiter *w;
    for (w = dq->waiters; w; w = w->next) if (w->state == DQ_W_WAITING) return 1;
    return 0;
}

static void settle(gptps_dq *dq, uint64_t upto, int state)   /* caller holds smu */
{
    dq_waiter *w;
    for (w = dq->waiters; w; w = w->next)
        if (w->state == DQ_W_WAITING && w->ticket <= upto) w->state = state;
}

/* Return once `w` is settled - or, with w NULL, once no write is left waiting and no
 * fsync is running. Leads an fsync when none is running, holding no lock across it.
 * Caller holds neither jmu nor smu. */
static void sync_until(gptps_dq *dq, const dq_waiter *w)
{
    apx_mutex_lock(&dq->smu);
    for (;;) {
        if (w ? w->state != DQ_W_WAITING : (!dq->syncing && !any_waiting(dq))) break;
        if (!dq->syncing && any_waiting(dq)) {
            uint64_t upto = dq->tickets;
            long     end  = dq->last_end;
            long     good = dq->synced;   /* only this caller moves it until syncing clears */
            int      fd   = dq->fd;
            int      dir  = dq->dir_dirty;
            int      rc;
            dq->syncing = 1;
            apx_mutex_unlock(&dq->smu);
            rc = (fd >= 0) ? apx_fsync_fd(fd) : -1;
            if (rc == 0 && dir) rc = fsync_parent_dir(dq->path);   /* see do_rewrite */
            if (rc == 0) {
                apx_mutex_lock(&dq->smu);
                settle(dq, upto, DQ_W_DURABLE);
                if (end > dq->synced) dq->synced = end;
                if (dir) dq->dir_dirty = 0;   /* no compaction runs while syncing is set */
            } else {
                /* Cut back to the last good fsync, and make the cut durable BEFORE
                 * failing anyone: the bytes past `good` may be on disk already (see
                 * above). jmu is held across that fsync, so no write lands between the
                 * cut and it; it is the failure path, and no other fsync is running. */
                apx_mutex_lock(&dq->jmu);     /* jmu before smu, always */
                if (rollback_to(dq->fp, good) != 0 || apx_fsync(dq->fp) != 0) break_queue(dq);
                apx_mutex_lock(&dq->smu);
                settle(dq, UINT64_MAX, DQ_W_FAILED);
                dq->last_end = good;
                apx_mutex_unlock(&dq->jmu);
            }
            dq->syncing = 0;
            apx_cond_broadcast(&dq->scv);
            continue;
        }
        apx_cond_wait_ms(&dq->scv, &dq->smu, 100);
    }
    apx_mutex_unlock(&dq->smu);
}

/* Settle every write in flight and keep the journal: returns with jmu held, no fsync
 * running and nothing waiting. Caller holds mu, so nothing can start a write - except a
 * writer that already held jmu, which may still register; hence the check under jmu. */
static void quiesce(gptps_dq *dq)
{
    for (;;) {
        int idle;
        sync_until(dq, NULL);
        apx_mutex_lock(&dq->jmu);
        apx_mutex_lock(&dq->smu);
        idle = !dq->syncing && !any_waiting(dq);
        apx_mutex_unlock(&dq->smu);
        if (idle) return;
        apx_mutex_unlock(&dq->jmu);
    }
}

/* How the `type` write for record `seq` ended: DQ_W_DURABLE if any did, else DQ_W_FAILED
 * if any is registered, else -1. Caller holds smu. */
static int waiter_state(const gptps_dq *dq, uint64_t seq, char type)
{
    const dq_waiter *w;
    int st = -1;
    for (w = dq->waiters; w; w = w->next) {
        if (seq < w->seq || seq > w->seq_end || w->type != type) continue;
        if (w->state == DQ_W_DURABLE) return DQ_W_DURABLE;
        st = w->state;
    }
    return st;
}

static void unregister_waiter(gptps_dq *dq, const dq_waiter *w)   /* caller holds mu */
{
    dq_waiter **pp;
    apx_mutex_lock(&dq->smu);
    for (pp = &dq->waiters; *pp; pp = &(*pp)->next)
        if (*pp == w) { *pp = w->next; break; }
    apx_mutex_unlock(&dq->smu);
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
 * monotonic counter under dq->mu and writes its record before the next one can (it
 * takes jmu before letting go of mu), replay() pushes records in file order, and
 * do_rewrite's compaction is stable - so this binary search is exact. It has to be a
 * search, not a scan: replay() calls it once per marker over an array that grows with
 * every 'P' record, which made opening a long-lived journal quadratic (200k records
 * took ~11s before this). */
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

/* ---- handle index ----
 * Every event the observer sees names a handle, and it used to find the record by
 * walking the whole table - O(records since the last compaction) per event, so a
 * recovered backlog of n cost O(n^2) to drain: 0.54s for 40,000 records where the
 * engine alone took 0.004s, four times longer each time the backlog doubled. This is
 * an open-addressing table from handle to seq, the record then found by find_by_seq.
 * It holds only open records that have a handle in this run, so it is bounded by the
 * pending set; a record leaves it when it closes. It keeps a seq rather than a pointer
 * or an index because push_rec moves the table and do_rewrite compacts it, and
 * neither then has to touch this one. A handle is never inserted twice: the engine
 * issues each once, and a record is resubmitted only while it has none.
 * Its growth is reserved BEFORE a record is journaled or resubmitted, for the reason
 * push_rec's is: past that point a failure would leave an open record that no event
 * can find. A reservation is counted until it is used or released, since a submit
 * holds one across its fsync while others reserve and insert. And the table grows
 * with calloc, never realloc. */
static size_t slot_of(gptps_handle h, size_t nslots)
{
    h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 33;
    return (size_t)(h & (nslots - 1));
}

/* Reserve room for `extra` more entries, keeping the load (live + deleted + reserved)
 * at a half at most, so a probe always meets an empty slot. Rehashing drops the deleted
 * ones. 0 on success. */
static int map_reserve(gptps_dq *dq, size_t extra)
{
    dq_slot *nt;
    size_t nn, i, need = dq->live + dq->reserved + extra;
    if ((need + dq->dead) * 2 <= dq->nslots) { dq->reserved += extra; return 0; }
    if (need > ((size_t)-1) / 8) return -1;
    for (nn = 16; nn < need * 4; nn <<= 1) { }
    nt = (dq_slot *)calloc(nn, sizeof *nt);
    if (!nt) return -1;
    for (i = 0; i < dq->nslots; ++i) {
        size_t j;
        if (dq->slots[i].state != 1) continue;
        for (j = slot_of(dq->slots[i].h, nn); nt[j].state; j = (j + 1) & (nn - 1)) { }
        nt[j] = dq->slots[i];
    }
    free(dq->slots);
    dq->slots = nt; dq->nslots = nn; dq->dead = 0;
    dq->reserved += extra;
    return 0;
}

static void map_unreserve(gptps_dq *dq) { dq->reserved -= 1; }

static void map_put(gptps_dq *dq, gptps_handle h, uint64_t seq)   /* uses a reservation */
{
    size_t j = slot_of(h, dq->nslots);
    while (dq->slots[j].state == 1) j = (j + 1) & (dq->nslots - 1);
    if (dq->slots[j].state == 2) dq->dead -= 1;
    dq->slots[j].h = h; dq->slots[j].seq = seq; dq->slots[j].state = 1;
    dq->live += 1;
    dq->reserved -= 1;
}

static dq_slot *map_find(gptps_dq *dq, gptps_handle h)
{
    size_t j, k;
    if (!dq->nslots || h == 0) return NULL;
    for (j = slot_of(h, dq->nslots), k = 0; k < dq->nslots && dq->slots[j].state;
         ++k, j = (j + 1) & (dq->nslots - 1))
        if (dq->slots[j].state == 1 && dq->slots[j].h == h) return &dq->slots[j];
    return NULL;
}

static void map_del(gptps_dq *dq, gptps_handle h)
{
    dq_slot *s = map_find(dq, h);
    if (s) { s->state = 2; dq->live -= 1; dq->dead += 1; }
}

/* The open record handle `h` belongs to, or NULL. */
static dq_rec *find_open(gptps_dq *dq, gptps_handle h)
{
    dq_slot *s = map_find(dq, h);
    dq_rec *rc = s ? find_by_seq(dq, s->seq) : NULL;
    return (rc && !rc->done && !rc->quarantined) ? rc : NULL;
}

/* ---- reading a damaged journal ----
 *
 * A record that does not verify - bad magic, impossible lengths, a short read, a wrong
 * checksum - is usually a TORN TAIL: the write a crash interrupted, the last thing in
 * the file. Replay used to assume it always was and stop there, and the compaction
 * gptps_dq_open runs next then rewrote the journal without everything after it. One
 * flipped bit in record 2 of 5 left one pending record, and destroyed three that had
 * been fsync'd and acknowledged, without a word.
 *
 * So replay now looks past a bad record for the next one that verifies:
 *   - There is none before the end of the file: a torn tail. Stop, silently, as before.
 *   - There is one, and the bad record's header was readable and claimed more bytes
 *     than the file holds. That is exactly what a torn write looks like - unless its
 *     payload carried journal records of its own, which would then be taken for real
 *     ones and run. Nothing after it is applied; it is preserved and reported.
 *   - Otherwise the damage is confined: skip to the record found and go on. A 'P' found
 *     there must also carry a seq above every 'P' read so far, as every real one does,
 *     which rejects the stale journal blocks a file system without data ordering can
 *     expose after a crash.
 * A record that verifies but that no writer makes - a 'P' whose seq does not rise, any
 * seq past DQ_MAX_SEQ - is damage too, wherever it is: skipped whole, and reported.
 * Whatever the damaged bytes held is lost either way. Before the compaction rewrites
 * the file, the original is copied aside (preserve_copy) and a warning goes to the
 * core's log sink (gptps_set_log_sink). Past a cut the copy is the only place the valid
 * records after it are kept, so if no copy can be made the open fails, and leaves the
 * journal as it was: the compaction used to go ahead and delete them. Confined damage
 * loses nothing more by being compacted, so there the open goes on, warning that the
 * copy failed - as it did before.
 *
 * Offsets are longs. Past LONG_MAX (2 GiB with a 32-bit long) ftell cannot report the
 * size, and replay then reads as it always did: up to the first record that does not
 * verify. Running out of memory fails the open instead of compacting away what could
 * not be loaded, and so does a read that fails: a bad sector, EIO, a journal this
 * process may not read. Replay used to take a failed fopen for "no journal yet" and a
 * failed fread for the end of the file or for damage, and the compaction then rewrote
 * the journal without every record it had not read - one transient error at open
 * deleted acknowledged work. A short read is damage only when the stream reports no
 * error; and only a journal that does not exist (ENOENT) is an empty one. */

typedef struct {               /* one record as read, before it is applied */
    char           type;
    uint16_t       nlen;
    uint32_t       plen;
    uint64_t       seq;
    unsigned char *body;       /* name, payload, checksum - malloc'd */
} dq_raw;

#define DQ_READ_OK     1
#define DQ_READ_END    0       /* the clean end of the file */
#define DQ_READ_BAD   -1       /* does not verify */
#define DQ_READ_TORN  -2       /* a readable header claiming more bytes than the file holds */
#define DQ_READ_NOMEM -3
#define DQ_READ_ERR   -4       /* the read failed: nothing is known about the bytes */

/* Read the record at the stream's position, `left` bytes before the end of the file
 * (-1: unknown). On DQ_READ_OK, r->body is the caller's to free. */
static int read_rec(FILE *f, long left, dq_raw *r)
{
    unsigned char hdr[DQ_RHDR_LEN];
    size_t got, blen;
    uint32_t crc;
    r->body = NULL;
    if (left == 0) return DQ_READ_END;
    got = fread(hdr, 1, DQ_RHDR_LEN, f);
    if (ferror(f)) return DQ_READ_ERR;
    if (got == 0 && left < 0) return DQ_READ_END;
    if (got != DQ_RHDR_LEN || get32(hdr) != DQ_REC_MAGIC) return DQ_READ_BAD;
    r->type = (char)hdr[4]; r->nlen = get16(hdr + 6); r->plen = get32(hdr + 8); r->seq = get64(hdr + 12);
    if (r->nlen > DQ_MAX_NAME || r->plen > DQ_MAX_PAYLOAD) return DQ_READ_BAD;
    blen = (size_t)r->nlen + r->plen + 4;
    if (left > 0 && (size_t)left < DQ_RHDR_LEN + blen) return DQ_READ_TORN;
    r->body = (unsigned char *)malloc(blen);
    if (!r->body) return DQ_READ_NOMEM;
    crc = fnv(hdr, DQ_RHDR_LEN, DQ_FNV_SEED);
    if (fread(r->body, 1, blen, f) != blen ||
        fnv(r->body, (size_t)r->nlen + r->plen, crc) != get32(r->body + r->nlen + r->plen)) {
        free(r->body); r->body = NULL;
        return ferror(f) ? DQ_READ_ERR : DQ_READ_BAD;
    }
    return DQ_READ_OK;
}

/* After a record at `bad` that does not verify: the offset of the next record that
 * does, and that may stand there (see above), with the stream left at it; -1 if there
 * is none before `end`; -2 out of memory or a failed read. */
static long resync(FILE *f, long bad, long end, uint64_t last_p)
{
    unsigned char buf[4096];
    long base = bad + 1;
    while (end - base >= DQ_RHDR_LEN + 4) {
        size_t want = sizeof buf, got, i;
        if ((long)want > end - base) want = (size_t)(end - base);
        if (fseek(f, base, SEEK_SET) != 0) return -1;
        got = fread(buf, 1, want, f);
        if (ferror(f)) return -2;
        if (got < 4) return -1;
        for (i = 0; i + 4 <= got; ++i) {
            long at = base + (long)i;
            dq_raw r;
            int k;
            if (get32(buf + i) != DQ_REC_MAGIC) continue;
            if (fseek(f, at, SEEK_SET) != 0) return -1;
            k = read_rec(f, end - at, &r);
            if (k == DQ_READ_NOMEM || k == DQ_READ_ERR) return -2;
            if (k != DQ_READ_OK) continue;
            free(r.body);
            if (r.type == 'P' && r.seq <= last_p) continue;   /* stale, or not a record at all */
            if (fseek(f, at, SEEK_SET) != 0) return -1;
            return at;
        }
        base += (long)got - 3;   /* a magic may straddle the next chunk */
    }
    return -1;
}

/* Apply one verified record to the table. -1 out of memory. */
static int apply_rec(gptps_dq *dq, const dq_raw *r)
{
    dq_rec *rc;
    if (r->seq >= dq->next_seq) dq->next_seq = r->seq + 1;
    if (r->type == 'P') {
        rc = push_rec(dq);
        if (!rc) return -1;
        rc->seq = r->seq; rc->len = r->plen;
        rc->name = (char *)malloc((size_t)r->nlen + 1);
        if (rc->name) { if (r->nlen) memcpy(rc->name, r->body, r->nlen); rc->name[r->nlen] = 0; }
        rc->payload = dup_mem(r->body + r->nlen, r->plen);
        if (!rc->name || (r->plen && !rc->payload)) {
            /* A half-initialised record is worse than a missing one: the compaction
             * would persist it with an empty name (never recoverable, never dropped -
             * it leaks in the journal forever). Pop it; the open fails. */
            free(rc->name); free(rc->payload);
            dq->n -= 1;
            return -1;
        }
        return 0;
    }
    rc = find_by_seq(dq, r->seq);
    if (!rc) return 0;                       /* about a record compacted away */
    switch (r->type) {
    case 'D':
        rc->done = 1;
        /* Release the completed record's buffers now instead of leaving them to
         * do_rewrite: replaying a journal of a million completed records would
         * otherwise hold every payload in RAM at once. NULLing is mandatory - the
         * cleanup paths free these again. */
        free(rc->name); free(rc->payload);
        rc->name = NULL; rc->payload = NULL; rc->len = 0;
        break;
    case 'Q': rc->quarantined = 1; break;    /* dead-lettered: retained, not re-submitted */
    case 'S': rc->inflight = 1; break;
    case 'F': rc->inflight = 0; rc->crashes = 0; break;
    case 'K': if (r->plen == 4 && r->nlen == 0) rc->crashes = get32(r->body); break;
    default: break;                          /* a later version's type: skip it */
    }
    return 0;
}

typedef struct {
    int           regions;     /* damaged regions found */
    long          first;       /* offset of the first */
    unsigned long bytes;       /* damaged bytes skipped, or not read past a cut */
    int           cut;         /* stopped at damage that cannot be told from a torn write */
} dq_damage;

/* Replay the journal at dq->path into dq->recs. Returns 0 (ok / missing file) or
 * -1 (present but corrupt header, unreadable, or out of memory). */
static int replay(gptps_dq *dq, dq_damage *dmg)
{
    FILE *f = fopen(dq->path, "rb");
    unsigned char fh[DQ_FHDR_LEN];
    long end = -1, pos = DQ_FHDR_LEN;
    uint64_t last_p = 0;
    size_t r;
    memset(dmg, 0, sizeof *dmg);
    if (!f) return errno == ENOENT ? 0 : -1;          /* no journal yet; or one we cannot read */
    r = fread(fh, 1, DQ_FHDR_LEN, f);
    if (ferror(f)) { fclose(f); return -1; }
    if (r == 0) { fclose(f); return 0; }              /* empty file */
    if (r < DQ_FHDR_LEN || get32(fh) != DQ_FILE_MAGIC || get32(fh + 4) != DQ_VERSION) {
        fclose(f); return -1;                         /* corrupt header */
    }
    if (fseek(f, 0, SEEK_END) == 0) end = ftell(f);
    if (fseek(f, DQ_FHDR_LEN, SEEK_SET) != 0) { fclose(f); return -1; }
    for (;;) {
        dq_raw rr;
        long next;
        int k = read_rec(f, end < 0 ? -1 : end - pos, &rr);
        if (k == DQ_READ_END) break;
        if (k == DQ_READ_NOMEM || k == DQ_READ_ERR) { fclose(f); return -1; }
        if (k == DQ_READ_OK && (rr.seq > DQ_MAX_SEQ || (rr.type == 'P' && rr.seq <= last_p))) {
            /* A record no writer makes, though it verifies. A 'P' whose seq does not
             * rise: each one is above the last, and find_by_seq's binary search counts on
             * that. Applied, one put the table out of order - the events and markers of
             * other records went unfound, before it as well as after it, so finished
             * work stayed pending and ran again, and a dead-lettered record was not
             * quarantined. And any seq past DQ_MAX_SEQ (see there). Both are damage, as
             * resync already judges such a 'P' it finds: skipped whole, and reported. */
            long len = DQ_RHDR_LEN + (long)rr.nlen + (long)rr.plen + 4;
            free(rr.body);
            if (!dmg->regions++) dmg->first = pos;
            dmg->bytes += (unsigned long)len;
            pos += len;
            continue;
        }
        if (k != DQ_READ_OK) {
            next = (end < 0) ? -1 : resync(f, pos, end, last_p);
            if (next == -2) { fclose(f); return -1; }
            if (next < 0) break;                      /* a torn tail */
            if (!dmg->regions++) dmg->first = pos;
            if (k == DQ_READ_TORN) { dmg->cut = 1; dmg->bytes += (unsigned long)(end - pos); break; }
            dmg->bytes += (unsigned long)(next - pos);
            pos = next;
            continue;
        }
        pos += DQ_RHDR_LEN + (long)rr.nlen + (long)rr.plen + 4;
        if (rr.type == 'P' && rr.seq > last_p) last_p = rr.seq;
        k = apply_rec(dq, &rr);
        free(rr.body);
        if (k != 0) { fclose(f); return -1; }
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

#define DQ_COPIES 10            /* "<path>.corrupt", then ".corrupt.1" to ".corrupt.9" */

static void copy_name(char *out, size_t cap, const char *path, int k)
{
    if (k) snprintf(out, cap, "%s.corrupt.%d", path, k);
    else   snprintf(out, cap, "%s.corrupt", path);
}

/* 1 if files a and b read back the same, byte for byte; 0 if not, or if either cannot
 * be read. */
static int same_bytes(const char *a, const char *b)
{
    unsigned char x[4096], y[4096];
    FILE *fa = fopen(a, "rb"), *fb = fa ? fopen(b, "rb") : NULL;
    int same = (fa && fb);
    while (same) {
        size_t n = fread(x, 1, sizeof x, fa), m = fread(y, 1, sizeof y, fb);
        if (n != m || ferror(fa) || ferror(fb) || memcmp(x, y, n) != 0) same = 0;
        else if (n < sizeof x) break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return same;
}

/* Copy file `from` to `to`, made durable; 0 on success. */
static int copy_file(const char *from, const char *to)
{
    unsigned char buf[8192];
    FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
    int ok = (in && out);
    while (ok) {
        size_t got = fread(buf, 1, sizeof buf, in);
        if (got && fwrite(buf, 1, got, out) != got) ok = 0;
        if (got < sizeof buf) { if (ferror(in)) ok = 0; break; }
    }
    if (out && (fflush(out) != 0 || apx_fsync(out) != 0)) ok = 0;
    if (in)  fclose(in);
    if (out) fclose(out);   /* after the fsync: nothing is left for it to lose */
    return ok ? 0 : -1;
}

/* Copy the journal aside before the open-time compaction rewrites it (see "reading a
 * damaged journal"). The copy goes to the first of "<path>.corrupt", "<path>.corrupt.1"
 * .. ".9" that does not exist, so an earlier incident's copy is kept - whether or not
 * this process may read it: a name counts as free only when the file system says
 * nothing is there. It used to count as free whenever it could not be opened for
 * reading, and a copy this process could not read was overwritten. Once all ten
 * exist, the oldest of them, by modification time, is replaced: the queue still opens
 * after ten incidents nobody cleaned up after. A copy identical to the journal already
 * in one of them IS the copy, so an open retried after a failure does not use up the
 * slots, and with them the copies of earlier incidents. The copy is written to
 * "<path>.tmp", fsync'd, and renamed into place, so a slot only ever holds a whole copy,
 * and an old copy is replaced only by a whole new one. Returns the copy's name
 * (malloc'd) once it and its name are durable, or NULL if no copy could be made. */
static char *preserve_copy(const char *path)
{
    size_t cap = strlen(path) + 16;
    char *name = (char *)malloc(cap), *tmp = (char *)malloc(cap);
    long long jsize = 0, size = 0, mtime = 0, oldest = 0;
    int k, use = -1, old = -1;
    if (!name || !tmp || apx_file_info(path, &jsize, &mtime) != 1) goto none;
    for (k = 0; k < DQ_COPIES; ++k) {
        int st;
        copy_name(name, cap, path, k);
        st = apx_file_info(name, &size, &mtime);
        if (st == 0 && use < 0) use = k;
        if (st != 1) continue;                    /* free, or cannot tell: left alone */
        if (size == jsize && same_bytes(name, path)) {
            free(tmp);                            /* kept already; its name made durable */
            if (fsync_parent_dir(name) != 0) { free(name); return NULL; }
            return name;
        }
        if (old < 0 || mtime < oldest) { old = k; oldest = mtime; }
    }
    if (use < 0) use = old;
    if (use < 0) goto none;
    copy_name(name, cap, path, use);
    snprintf(tmp, cap, "%s.tmp", path);
    if (copy_file(path, tmp) != 0 || dq_rename_replace(tmp, name) != 0) { remove(tmp); goto none; }
    free(tmp);
    if (fsync_parent_dir(name) != 0) { free(name); return NULL; }
    return name;
none:
    free(name); free(tmp);
    return NULL;
}

/* Warn about the damage replay found, and where the original went. Past a cut, the
 * copy is the only place the valid records after it are kept, so without one the open
 * does not go on (see gptps_dq_open). */
static void report_damage(const gptps_dq *dq, const dq_damage *d, const char *copy)
{
    char msg[1024];
    const char *kept = copy ? "The original is preserved as "
                     : d->cut ? "The original could not be copied aside, so the journal is left "
                                "as it is and the queue is not opened."
                              : "The original could not be preserved.";
    if (d->cut)
        snprintf(msg, sizeof msg,
                 "gptps_durable_queue: %s is damaged at byte %ld, where a record claims more "
                 "bytes than the file holds, and valid records follow; the %lu bytes from there "
                 "were not read. %s%s",
                 dq->path, d->first, d->bytes, kept, copy ? copy : "");
    else
        snprintf(msg, sizeof msg,
                 "gptps_durable_queue: %s is damaged: %lu byte(s) in %d place(s), the first at "
                 "byte %ld - unreadable, or a record numbered as no writer numbers one - were "
                 "skipped and every valid record after them kept; whatever they held is lost. %s%s",
                 dq->path, d->bytes, d->regions, d->first, kept, copy ? copy : "");
    gptps_log(NULL, GPTPS_LOG_WARN, msg);
}

/* (Re)open the append handle and publish its descriptor. Caller holds jmu. */
static void reopen_append(gptps_dq *dq)
{
    dq->fp = fopen(dq->path, "ab");
    if (dq->fp) fseek(dq->fp, 0, SEEK_END);   /* an append stream's position is not portable */
    apx_mutex_lock(&dq->smu);
    dq->fd = dq->fp ? apx_fileno(dq->fp) : -1;
    apx_mutex_unlock(&dq->smu);
}

/* Rewrite the journal to contain only still-pending records, then reopen the append
 * handle and drop completed records from memory. Caller holds mu (not jmu): every
 * write in flight is settled first (see "journal writes and group commit").
 * GPTPS_OK once the new journal is in place, durable, and open for appending. Else
 * GPTPS_E_NOMEM or GPTPS_E_IO, and *replaced says whether the rename happened. If not,
 * the old journal stands as it was. If so, either the directory could not be synced
 * (dir_dirty: see below) or the journal could not be reopened, which breaks the queue. */
static gptps_status do_rewrite(gptps_dq *dq, int *replaced)
{
    size_t tn = strlen(dq->path) + 5, i, keep = 0;
    char *tmp = (char *)malloc(tn);
    FILE *t;
    long end;
    int reopened, dirty, had;
    *replaced = 0;
    if (!tmp) return GPTPS_E_NOMEM;
    snprintf(tmp, tn, "%s.tmp", dq->path);
    quiesce(dq);                      /* from here on jmu is held */
    t = fopen(tmp, "wb");
    if (!t) { apx_mutex_unlock(&dq->jmu); free(tmp); return GPTPS_E_IO; }
    if (write_file_header(t) != 0) {  /* unchecked, a short write here installed a journal no open could read */
        fclose(t); remove(tmp); apx_mutex_unlock(&dq->jmu); free(tmp);
        return GPTPS_E_IO;
    }
    apx_mutex_lock(&dq->smu);         /* waiter_state below; nothing can change it now */
    for (i = 0; i < dq->n; ++i) {
        const dq_rec *rc = &dq->recs[i];
        unsigned char kb[4];
        if (rc->done) continue;
        /* A submit still finishing up is written only if its 'P' became durable, and a
         * record whose retraction did is gone already: the file agrees with what those
         * callers were told. Both stay in memory for their callers to settle. */
        if (rc->committing && waiter_state(dq, rc->seq, 'P') != DQ_W_DURABLE) continue;
        if (rc->retracting && waiter_state(dq, rc->seq, 'D') == DQ_W_DURABLE) continue;
        /* Retain both still-pending and quarantined (dead-lettered) records; a
         * quarantined one is rewritten as P (to keep its poison payload) plus a Q
         * marker so replay reclassifies it without re-submitting it. The crash count
         * travels as a K marker, and an attempt running right now as its S: a crash
         * just after a mid-run compaction must still count. */
        put32(kb, rc->crashes);
        if (write_record(t, 'P', rc->seq, rc->name, rc->payload, rc->len) != 0 ||
            (rc->crashes     && write_record(t, 'K', rc->seq, "", kb, sizeof kb) != 0) ||
            (rc->inflight    && write_record(t, 'S', rc->seq, "", NULL, 0) != 0) ||
            (rc->quarantined && write_record(t, 'Q', rc->seq, "", NULL, 0) != 0)) {
            apx_mutex_unlock(&dq->smu);
            fclose(t); remove(tmp); apx_mutex_unlock(&dq->jmu); free(tmp);
            return GPTPS_E_IO;
        }
    }
    apx_mutex_unlock(&dq->smu);
    if (fflush(t) != 0 || apx_fsync(t) != 0) {
        fclose(t); remove(tmp); apx_mutex_unlock(&dq->jmu); free(tmp);
        return GPTPS_E_IO;
    }
    fclose(t);

    had = (dq->fp != NULL);
    if (dq->fp) { fclose(dq->fp); dq->fp = NULL; }
    if (dq_rename_replace(tmp, dq->path) != 0) {
        /* The original journal is untouched on disk (the rename never happened),
         * so put the append handle back before reporting the error: this return
         * is documented as an ordinary recoverable GPTPS_E_IO, and leaving
         * dq->fp NULL would turn the caller's next submit - or the next terminal
         * event, on an engine worker thread - into a NULL-FILE* crash. Everything
         * written to it so far is on disk: quiesce saw to that. Only a handle there
         * was: gptps_dq_open has none yet, and opening one would create the
         * journal of a new queue whose open is about to fail. */
        remove(tmp); free(tmp);
        if (had) {
            reopen_append(dq);
            if (!dq->fp) break_queue(dq);
        }
        apx_mutex_unlock(&dq->jmu);
        return GPTPS_E_IO;
    }
    *replaced = 1;
    /* Make the rename's directory entry durable. Until it is, a power cut can put the
     * journal's name back on the old file, and lose everything written to the new one:
     * submits acknowledged as durable, retractions. This fsync's result used to be
     * ignored. Now a failure is reported, and until one succeeds the group commit makes
     * it again before it settles any write (sync_until), so nothing written to the new
     * file is acknowledged while its name could still be lost. A directory that cannot
     * be synced at all - one this process may not read, a file system with no fsync
     * for a directory - is not a failure (apx_dir_fsync): there the queue goes on as
     * it always has, and a power cut can still undo a rename. */
    dirty = fsync_parent_dir(dq->path) != 0;
    free(tmp);
    reopen_append(dq);
    reopened = (dq->fp != NULL);
    end = reopened ? ftell(dq->fp) : -1;
    apx_mutex_lock(&dq->smu);
    dq->synced = dq->last_end = end;   /* the whole new file was fsync'd before the rename */
    dq->dir_dirty = dirty;
    apx_mutex_unlock(&dq->smu);
    if (!reopened) break_queue(dq);  /* no file to write: see "a broken queue" */
    else if (!dirty) dq->broken = dq->broke_note = 0;   /* repaired: the table is the journal */
    apx_mutex_unlock(&dq->jmu);
    if (!reopened) return GPTPS_E_IO;

    /* compact memory: drop done records, keep pending + quarantined */
    for (i = 0; i < dq->n; ++i) {
        if (dq->recs[i].done) { free(dq->recs[i].name); free(dq->recs[i].payload); }
        else dq->recs[keep++] = dq->recs[i];
    }
    dq->n = keep;
    return dirty ? GPTPS_E_IO : GPTPS_OK;
}

/* ---- resubmission ---- */

/* Offer the engine an open record that has no execution in this run. Caller holds
 * dq->mu, which the observer also takes, so the handle is in the index before any
 * event for it can be looked up. */
static gptps_status resubmit(gptps_dq *dq, dq_rec *rc)
{
    gptps_handle h = 0;
    gptps_status st;
    if (map_reserve(dq, 1) != 0) return GPTPS_E_NOMEM;
    st = gptps_submit(dq->e, rc->name, rc->payload, rc->len, &h);
    if (st == GPTPS_OK) {
        rc->handle = h;
        map_put(dq, h, rc->seq);
        if (dq->resub) dq->resub(rc->name, rc->payload, rc->len, h, dq->resub_ud);
    } else {
        map_unreserve(dq);
    }
    return st;
}

/* Hand the engine the next suspect waiting for its turn (see "crash loops"), oldest
 * first. One the engine refuses stays pending and the next is tried, so a type this
 * run never registered cannot hold the others back. Returns 1 if one was submitted.
 * Caller holds dq->mu, and calls only where gptps_submit may run with it held - the
 * QUEUED that submit delivers on this thread must not call back into the queue (the
 * header's re-entrancy rule). */
static size_t trial_next(gptps_dq *dq)
{
    size_t i;
    for (i = 0; i < dq->n; ++i) {
        dq_rec *rc = &dq->recs[i];
        if (rc->done || rc->quarantined || !rc->suspect || rc->handle != 0) continue;
        if (resubmit(dq, rc) == GPTPS_OK) { dq->trial = rc->seq; return 1; }
    }
    return 0;
}

/* Close an open record with its verdict: 'D' (gone) or 'Q' (quarantined). */
static void close_rec(gptps_dq *dq, dq_rec *rc, char type)
{
    if (type == 'Q') rc->quarantined = 1; else rc->done = 1;
    rc->inflight = 0;
    append_marker(dq, type, rc->seq);
    map_del(dq, rc->handle);
    if (dq->pending) dq->pending -= 1;
}

/* A record's verdict - unless gptps_dq_cancel is making its retraction durable right
 * now, in which case the retraction decides: the verdict is kept for the case that it
 * fails, and dropped if it succeeds. */
static void close_or_defer(gptps_dq *dq, dq_rec *rc, char type)
{
    if (rc->retracting) rc->deferred = type;
    else close_rec(dq, rc, type);
}

/* ---- observer: bracket attempts, and close a record when its task terminates ----
 *
 * A DEAD_LETTERED or DROPPED that teardown imposed - the grace expired on work
 * still queued or waiting in backoff, or the drain refused a REQUEUE item another
 * cycle - carries GPTPS_EV_FLAG_SHUTDOWN (see gptps_shutdown). Neither is a
 * verdict on the work: quarantining it would file healthy work as poison, and
 * closing it would lose it, so the record stays pending and the next run's
 * gptps_dq_recover re-submits it, like everything else shutdown abandons (which
 * the engine reports as FAILED / GPTPS_E_CANCELLED without
 * GPTPS_EV_FLAG_SELF_CANCELLED, an event this observer never counts as terminal).
 *
 * The flag, not the status, decides. Those events carry GPTPS_E_SHUTDOWN, but so
 * does the disposition of a body that itself returned it - forwarding a remote
 * worker's shutdown, say - and that one IS the verdict: kept pending, it would re-run
 * on every restart. It carries the status without the flag, and is judged by its
 * policy like any other failure.
 *
 * A body that returns GPTPS_E_CANCELLED itself ends its item (GPTPS_EV_FLAG_SELF_
 * CANCELLED on its FAILED). That is the work's own outcome too, not a stop from
 * outside, so the record closes as on FINISHED or DROPPED - not quarantined, since a
 * cancel is not a failure to retain. Kept pending, a body that always cancels itself
 * would re-run on every restart. A FAILED / GPTPS_E_CANCELLED without the flag - a
 * gptps_cancel, a removal, a teardown - still leaves the record pending.
 *
 * Every FAILED of an attempt that STARTED ends that attempt, the process alive: an
 * 'F', and the crash count resets (see "crash loops"). A FAILED with no attempt
 * running is a cancel that reached the item while it waited. Either way it ends the
 * turn of a suspect on trial, as does any verdict, and the next one goes. */
static int has_flag(const gptps_event *ev, uint32_t f)
{
    return ev->struct_size >= offsetof(gptps_event, flags) + sizeof ev->flags &&
           (ev->flags & f) != 0;
}

static void dq_on_event(const gptps_event *ev, void *ud)
{
    gptps_dq *dq = (gptps_dq *)ud;
    dq_rec *rc;
    int over = 0;
    /* Nothing to do for these. QUEUED in particular must not take the lock:
     * gptps_dq_submit, gptps_dq_recover and trial_next deliver it on their own
     * thread while they hold dq->mu. */
    if (ev->kind == GPTPS_EV_QUEUED || ev->kind == GPTPS_EV_RETRIED) return;
    apx_mutex_lock(&dq->mu);
    rc = find_open(dq, ev->handle);
    if (rc) {
        int on_trial = (rc->seq == dq->trial);
        switch (ev->kind) {
        case GPTPS_EV_STARTED:
            rc->inflight = 1;
            append_marker(dq, 'S', rc->seq);
            break;
        case GPTPS_EV_FAILED:
            if (rc->inflight) {
                rc->inflight = 0; rc->crashes = 0; rc->suspect = 0;
                append_marker(dq, 'F', rc->seq);
            }
            if (ev->status == GPTPS_E_CANCELLED && has_flag(ev, GPTPS_EV_FLAG_SELF_CANCELLED))
                close_or_defer(dq, rc, 'D');   /* cancelled by its own body: gone */
            over = on_trial;
            break;
        case GPTPS_EV_FINISHED:
            close_or_defer(dq, rc, 'D');
            over = on_trial;
            break;
        case GPTPS_EV_DEAD_LETTERED:
        case GPTPS_EV_DROPPED:
            if (!has_flag(ev, GPTPS_EV_FLAG_SHUTDOWN))   /* teardown's: stays pending */
                close_or_defer(dq, rc, ev->kind == GPTPS_EV_DEAD_LETTERED ? 'Q' : 'D');
            over = on_trial;
            break;
        default:                             /* a kind a later engine added */
            break;
        }
    }
    if (over) { dq->trial = 0; trial_next(dq); }
    apx_mutex_unlock(&dq->mu);
}

/* ---- public API ---- */
gptps_dq *gptps_dq_open(gptps *e, const char *journal_path)
{
    gptps_dq *dq;
    dq_damage dmg;
    size_t i;
    int registered = 0, replaced = 0, dirty;
    if (!e || !journal_path) return NULL;
    dq = (gptps_dq *)calloc(1, sizeof *dq);
    if (!dq) return NULL;
    apx_mutex_init(&dq->mu);
    apx_mutex_init(&dq->jmu);
    apx_mutex_init(&dq->smu);
    apx_cond_init(&dq->scv);
    dq->e = e; dq->next_seq = 1; dq->fd = -1;
    dq->path = dup_str(journal_path);
    if (!dq->path) goto fail;

    if (replay(dq, &dmg) != 0) goto fail;    /* corrupt header, unreadable, or out of memory */
    if (dmg.regions) {                       /* BEFORE do_rewrite replaces the file */
        char *copy = preserve_copy(dq->path);
        int kept = (copy != NULL);
        report_damage(dq, &dmg, copy);
        free(copy);
        if (!kept && dmg.cut) goto fail;     /* see "reading a damaged journal" */
    }
    for (i = 0; i < dq->n; ++i) {
        dq_rec *rc = &dq->recs[i];
        if (rc->done || rc->quarantined) continue;
        if (rc->inflight) {                  /* running when the process died */
            rc->inflight = 0;
            if (rc->crashes < UINT32_MAX) rc->crashes += 1;
        }
        if (rc->crashes >= DQ_QUARANTINE_AT) {
            char msg[512];
            rc->quarantined = 1;
            snprintf(msg, sizeof msg,
                     "gptps_durable_queue: %s: record %llu (task '%s') was running each of the "
                     "last %lu times the process died; quarantined instead of recovered",
                     dq->path, (unsigned long long)rc->seq, rc->name ? rc->name : "?",
                     (unsigned long)rc->crashes);
            gptps_log(NULL, GPTPS_LOG_WARN, msg);
            continue;
        }
        rc->suspect = (rc->crashes >= DQ_SUSPECT_AT);
        dq->pending += 1;                    /* quarantined records are retained, not pending */
    }
    /* Every way the open can fail comes before the compaction, which leaves the journal
     * as it was until its rename. Once that has happened the open does not fail: the
     * observer is registered first, and a directory that could not be synced (the
     * group commit syncs it before it acknowledges anything) or a journal that could
     * not be reopened (a broken queue) is reported instead. */
    if (gptps_register_observer(e, dq_on_event, dq) != GPTPS_OK) goto fail;
    registered = 1;
    apx_mutex_lock(&dq->mu);
    (void)do_rewrite(dq, &replaced);
    apx_mutex_unlock(&dq->mu);
    if (!replaced) goto fail;
    apx_mutex_lock(&dq->smu);
    dirty = dq->dir_dirty;
    apx_mutex_unlock(&dq->smu);
    if (dirty) {
        char msg[512];
        snprintf(msg, sizeof msg,
                 "gptps_durable_queue: %s was compacted, but its directory could not be "
                 "synced, so a power cut could still bring back the journal as it was. Nothing "
                 "is acknowledged until a sync of the directory succeeds.", dq->path);
        gptps_log(NULL, GPTPS_LOG_WARN, msg);
    }
    if (dq->next_seq > DQ_MAX_SEQ) {         /* only replay can bring it here */
        char msg[512];
        snprintf(msg, sizeof msg,
                 "gptps_durable_queue: %s holds a record or marker numbered 2^63-1, the last "
                 "number there is, so every submit to this queue returns GPTPS_E_FULL. An open "
                 "of the journal once that record has closed and been compacted away clears it.",
                 dq->path);
        gptps_log(NULL, GPTPS_LOG_WARN, msg);
    }
    warn_if_broke(dq);
    return dq;

fail:
    if (registered) gptps_unregister_observer(e, dq_on_event, dq);
    if (dq->fp) fclose(dq->fp);
    for (i = 0; i < dq->n; ++i) { free(dq->recs[i].name); free(dq->recs[i].payload); }
    free(dq->recs); free(dq->slots); free(dq->path);
    apx_cond_destroy(&dq->scv);
    apx_mutex_destroy(&dq->smu);
    apx_mutex_destroy(&dq->jmu);
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
    dq_waiter w;
    uint64_t seq;
    char *nm;
    void *pl;
    int wrote;
    if (!dq || !task_name) return GPTPS_E_INVAL;
    /* Refuse anything the replayer would reject rather than writing it: a record
     * past these bounds is classified as damage on the next open, and at the end of
     * the file it is dropped with every record appended after it - while this call
     * returned GPTPS_OK and its durability promise. The bounds are exactly replay()'s. */
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
    /* A seq past DQ_MAX_SEQ is one the next open would call damage. Only a damaged
     * journal brings next_seq near it, but then the queue is full, not wrong. */
    if (dq->next_seq > DQ_MAX_SEQ) { apx_mutex_unlock(&dq->mu); free(nm); free(pl); return GPTPS_E_FULL; }
    /* Reserve the in-memory slot, and its place in the handle index, BEFORE
     * journaling, for the reason nm and pl are duplicated first: the reverse order
     * has no rollback. A push_rec that failed after the 'P' was fsync'd returned
     * GPTPS_E_NOMEM - "not submitted" - for a record the next gptps_dq_recover then
     * ran anyway. */
    if (map_reserve(dq, 1) != 0) { apx_mutex_unlock(&dq->mu); free(nm); free(pl); return GPTPS_E_NOMEM; }
    rc = push_rec(dq);
    if (!rc) {
        map_unreserve(dq);
        apx_mutex_unlock(&dq->mu);
        free(nm); free(pl);
        return GPTPS_E_NOMEM;
    }
    seq = dq->next_seq++;
    rc->seq = seq; rc->name = nm; rc->payload = pl; rc->len = len;
    rc->committing = 1;               /* not a record, and not pending, until it is durable */
    apx_mutex_lock(&dq->jmu);         /* before letting go of mu: see "group commit" */
    apx_mutex_unlock(&dq->mu);
    wrote = write_durable(dq, &w, 'P', seq, task_name, payload, len);
    apx_mutex_unlock(&dq->jmu);
    /* Durable before we enqueue: a swallowed fsync error would be a false durability
     * claim. The wait holds no lock the engine's threads need, and it shares its fsync
     * with any other submit in flight. If the write or the fsync fails, the journal is
     * cut back, so a transient full disk costs this submit rather than every submit
     * for the rest of the process's life - and the wait goes on until that cut is
     * durable, so the record cannot come back (see "group commit"). */
    if (wrote != -2) sync_until(dq, &w);

    apx_mutex_lock(&dq->mu);
    rc = find_by_seq(dq, seq);        /* the table may have moved or been compacted since */
    rc->committing = 0;
    if (wrote != -2) unregister_waiter(dq, &w);
    if (wrote != 0 || w.state != DQ_W_DURABLE) {
        rc->done = 1;                 /* never journaled, or cut away: not a record */
        map_unreserve(dq);
        apx_mutex_unlock(&dq->mu);
        warn_if_broke(dq);
        return GPTPS_E_IO;
    }
    dq->pending += 1;
    st = gptps_submit(dq->e, task_name, payload, len, &h);
    if (st == GPTPS_OK) {
        rc->handle = h;
        map_put(dq, h, seq);
        if (out_handle) *out_handle = h;
    } else {
        /* engine refused it: mark done so recovery won't replay a rejected task */
        rc->done = 1; if (dq->pending) dq->pending -= 1;
        map_unreserve(dq);
        append_marker(dq, 'D', seq);
    }
    apx_mutex_unlock(&dq->mu);
    return st;
}

gptps_status gptps_dq_submit_batch(gptps_dq *dq, gptps_dq_item *items, size_t n)
{
    char **nm = NULL;
    void **pl = NULL;
    dq_waiter w;
    uint64_t first;
    size_t i, pushed;
    int wrote, durable;
    gptps_status bad = GPTPS_OK;
    if (!dq || (n && !items)) return GPTPS_E_INVAL;
    if (n == 0) return GPTPS_OK;
    /* gptps_dq_submit's bounds, for every item, before anything is written: a batch is
     * journaled whole or not at all. */
    for (i = 0; i < n && bad == GPTPS_OK; ++i)
        if (!items[i].task_name || items[i].len > DQ_MAX_PAYLOAD ||
            strlen(items[i].task_name) > DQ_MAX_NAME || (items[i].len && !items[i].payload))
            bad = GPTPS_E_INVAL;
    if (bad == GPTPS_OK) {
        nm = (char **)calloc(n, sizeof *nm);
        pl = (void **)calloc(n, sizeof *pl);
        if (!nm || !pl) bad = GPTPS_E_NOMEM;
    }
    for (i = 0; i < n && bad == GPTPS_OK; ++i) {       /* duplicated first: see gptps_dq_submit */
        nm[i] = dup_str(items[i].task_name);
        pl[i] = dup_mem(items[i].payload, items[i].len);
        if (!nm[i] || (items[i].len && !pl[i])) bad = GPTPS_E_NOMEM;
    }
    if (bad != GPTPS_OK) goto refuse;

    apx_mutex_lock(&dq->mu);
    /* The batch takes next_seq .. next_seq + n - 1, and the last must not pass
     * DQ_MAX_SEQ (see gptps_dq_submit). In this order nothing wraps, and n is never
     * compared with a 64-bit constant a 32-bit size_t cannot reach. */
    if (dq->next_seq > DQ_MAX_SEQ || (uint64_t)n - 1 > DQ_MAX_SEQ - dq->next_seq) {
        apx_mutex_unlock(&dq->mu); bad = GPTPS_E_FULL; goto refuse;
    }
    if (map_reserve(dq, n) != 0) { apx_mutex_unlock(&dq->mu); bad = GPTPS_E_NOMEM; goto refuse; }
    for (pushed = 0; pushed < n && push_rec(dq); ++pushed) { }
    if (pushed < n) {                                 /* reserved before journaling, all or none */
        dq->n -= pushed;
        dq->reserved -= n;
        apx_mutex_unlock(&dq->mu);
        bad = GPTPS_E_NOMEM;
        goto refuse;
    }
    first = dq->next_seq;
    dq->next_seq += n;
    for (i = 0; i < n; ++i) {
        dq_rec *rc = &dq->recs[dq->n - n + i];
        rc->seq = first + i; rc->name = nm[i]; rc->payload = pl[i]; rc->len = items[i].len;
        rc->committing = 1;
        nm[i] = NULL; pl[i] = NULL;                   /* the table owns them now */
    }
    apx_mutex_lock(&dq->jmu);                         /* before letting go of mu: see "group commit" */
    apx_mutex_unlock(&dq->mu);
    wrote = write_durable_batch(dq, &w, items, n, first);
    apx_mutex_unlock(&dq->jmu);
    if (wrote != -2) sync_until(dq, &w);              /* one fsync for the lot */
    durable = (wrote == 0 && w.state == DQ_W_DURABLE);

    apx_mutex_lock(&dq->mu);
    if (wrote != -2) unregister_waiter(dq, &w);
    for (i = 0; i < n; ++i) {
        dq_rec *rc = find_by_seq(dq, first + i);
        gptps_handle h = 0;
        rc->committing = 0;
        items[i].handle = 0;
        if (!durable) {                               /* not journaled: not records */
            rc->done = 1;
            map_unreserve(dq);
            items[i].status = GPTPS_E_IO;
            continue;
        }
        dq->pending += 1;
        items[i].status = gptps_submit(dq->e, rc->name, rc->payload, rc->len, &h);
        if (items[i].status == GPTPS_OK) {
            rc->handle = h;
            map_put(dq, h, rc->seq);
            items[i].handle = h;
        } else {                                      /* refused: closed, as gptps_dq_submit does */
            rc->done = 1; if (dq->pending) dq->pending -= 1;
            map_unreserve(dq);
            append_marker(dq, 'D', rc->seq);
        }
    }
    apx_mutex_unlock(&dq->mu);
    free(nm); free(pl);
    if (!durable) warn_if_broke(dq);
    return durable ? GPTPS_OK : GPTPS_E_IO;

refuse:
    for (i = 0; i < n; ++i) {
        items[i].handle = 0; items[i].status = bad;
        if (nm) free(nm[i]);
        if (pl) free(pl[i]);
    }
    free(nm); free(pl);
    return bad;
}

gptps_status gptps_dq_set_resubmit_cb(gptps_dq *dq, gptps_dq_resubmit_cb cb, void *user_data)
{
    if (!dq) return GPTPS_E_INVAL;
    apx_mutex_lock(&dq->mu);
    dq->resub = cb; dq->resub_ud = user_data;
    apx_mutex_unlock(&dq->mu);
    return GPTPS_OK;
}

size_t gptps_dq_recover(gptps_dq *dq)
{
    size_t i, count = 0;
    if (!dq) return 0;
    apx_mutex_lock(&dq->mu);
    for (i = 0; i < dq->n; ++i) {
        dq_rec *rc = &dq->recs[i];
        /* Quarantined records are TERMINAL-but-retained, not incomplete. Re-submitting
         * one re-runs the exact poison payload dead-lettering exists to contain, and it
         * never converges: dq_on_event skips an already-quarantined record, so no new Q
         * marker is written and the same payload runs again on every restart.
         * Suspects wait for trial_next, below, which hands them over one at a time,
         * and a record gptps_dq_submit is still making durable is that call's. */
        if (rc->done || rc->quarantined || rc->handle != 0 || rc->suspect || rc->committing)
            continue; /* completed, quarantined, already live, a suspect, or not yet one */
        if (resubmit(dq, rc) == GPTPS_OK) ++count;
        /* on failure leave it pending: a later call, or a later run with the task
         * registered, can recover it */
    }
    if (!dq->trial) count += trial_next(dq);
    apx_mutex_unlock(&dq->mu);
    return count;
}

gptps_status gptps_dq_cancel(gptps_dq *dq, gptps_handle h)
{
    dq_rec *rc;
    dq_waiter w;
    uint64_t seq;
    int wrote, durable;
    if (!dq || h == 0) return GPTPS_E_INVAL;   /* 0: a record not yet (re)submitted */
    apx_mutex_lock(&dq->mu);
    rc = find_open(dq, h);
    if (!rc) { apx_mutex_unlock(&dq->mu); return GPTPS_E_NOTFOUND; }
    /* Durable, unlike the observer's markers, and BEFORE the engine is told: a
     * retraction lost to a crash would have the next gptps_dq_recover re-run work
     * the caller withdrew. The record is marked as retracting meanwhile, so a verdict
     * that arrives now waits for the outcome (close_or_defer). */
    seq = rc->seq;
    rc->retracting += 1;
    apx_mutex_lock(&dq->jmu);         /* before letting go of mu: see "group commit" */
    apx_mutex_unlock(&dq->mu);
    wrote = write_durable(dq, &w, 'D', seq, "", NULL, 0);
    apx_mutex_unlock(&dq->jmu);
    if (wrote != -2) sync_until(dq, &w);
    durable = (wrote == 0 && w.state == DQ_W_DURABLE);

    apx_mutex_lock(&dq->mu);
    if (wrote != -2) unregister_waiter(dq, &w);
    rc = find_by_seq(dq, seq);        /* NULL: retracted by a concurrent call and compacted away */
    if (rc) {
        rc->retracting -= 1;
        if (durable) {
            rc->deferred = 0;         /* withdrawn: whatever the engine said no longer matters */
            if (!rc->done && !rc->quarantined) {
                rc->done = 1; rc->inflight = 0;
                map_del(dq, rc->handle);
                if (dq->pending) dq->pending -= 1;
                if (rc->seq == dq->trial) { dq->trial = 0; trial_next(dq); }   /* its turn is over */
            }
        } else if (rc->done) {
            durable = 1;              /* a concurrent retraction made it */
        } else if (!rc->retracting && rc->deferred) {
            /* Not withdrawn after all: the verdict that waited applies, as it would
             * have if this call had never been made. If it cannot be made durable the
             * record stays open and the execution is left running. */
            char v = rc->deferred;
            rc->deferred = 0;
            close_rec(dq, rc, v);
        }
    } else {
        durable = 1;
    }
    apx_mutex_unlock(&dq->mu);
    if (!durable) { warn_if_broke(dq); return GPTPS_E_IO; }
    /* Outside dq->mu: gptps_cancel delivers the terminal event on this thread for
     * an item still queued or between attempts, and a callback reacting to it may
     * call back into this queue. The retraction stands whatever it returns.
     * NOTFOUND is still OK - the execution had already ended (an earlier
     * gptps_cancel, or a terminal event in the window since the lock was released),
     * so nothing is left to stop. GPTPS_E_SHUTDOWN is passed on: the engine is
     * tearing down and did not stop the execution, which may yet run. */
    return gptps_cancel(dq->e, h) == GPTPS_E_SHUTDOWN ? GPTPS_E_SHUTDOWN : GPTPS_OK;
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

size_t gptps_dq_drain_quarantine_ex(gptps_dq *dq, gptps_dq_quarantine_cb cb,
                                    void *user_data, gptps_status *out_compact)
{
    size_t i, n = 0;
    if (out_compact) *out_compact = GPTPS_OK;
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
    /* Compact the drained records out of the journal.
     *
     * A failure here is genuinely NOT a failed drain: cb has already seen every
     * payload, and the return value counts what cb saw. What it does mean is that
     * the records are not known to be out of the journal - the old one still stands,
     * or a power cut can bring it back while its directory is not synced - so a
     * restart can re-quarantine them and cb see them AGAIN: this add-on's
     * at-least-once contract, applied to the drain callback. A host whose cb is not idempotent (it bills, it emails, it
     * files a ticket) has to know that happened, and used to have no way to find
     * out. Reported through out_compact rather than the return value, so the count
     * keeps meaning "records drained". */
    if (n) {
        int replaced;
        gptps_status cst = do_rewrite(dq, &replaced);
        if (out_compact) *out_compact = cst;
    }
    apx_mutex_unlock(&dq->mu);
    if (n) warn_if_broke(dq);
    return n;
}

size_t gptps_dq_drain_quarantine(gptps_dq *dq, gptps_dq_quarantine_cb cb, void *user_data)
{
    return gptps_dq_drain_quarantine_ex(dq, cb, user_data, NULL);
}

gptps_status gptps_dq_compact(gptps_dq *dq)
{
    gptps_status st;
    int replaced;
    if (!dq) return GPTPS_E_INVAL;
    apx_mutex_lock(&dq->mu);
    st = do_rewrite(dq, &replaced);
    apx_mutex_unlock(&dq->mu);
    warn_if_broke(dq);
    return st;
}

void gptps_dq_close(gptps_dq *dq)
{
    size_t i;
    if (!dq) return;
    /* Caller contract: the engine is already shut down, so no event fires here, and
     * no submit or retraction is in flight. */
    if (dq->fp) fclose(dq->fp);
    for (i = 0; i < dq->n; ++i) { free(dq->recs[i].name); free(dq->recs[i].payload); }
    free(dq->recs); free(dq->slots); free(dq->path);
    apx_cond_destroy(&dq->scv);
    apx_mutex_destroy(&dq->smu);
    apx_mutex_destroy(&dq->jmu);
    apx_mutex_destroy(&dq->mu);
    free(dq);
}
