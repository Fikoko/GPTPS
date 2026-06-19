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
#include "durable_queue.h"
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
    int          done;
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

/* ---- record I/O ---- */
static void write_file_header(FILE *f)
{
    unsigned char h[DQ_FHDR_LEN];
    put32(h, DQ_FILE_MAGIC); put32(h + 4, DQ_VERSION);
    fwrite(h, 1, sizeof h, f);
}

/* Append one record. Returns 0 on success. Does not flush/fsync. */
static int write_record(FILE *f, char type, uint64_t seq,
                        const char *name, const void *payload, uint32_t plen)
{
    uint16_t nlen = name ? (uint16_t)strlen(name) : 0;
    size_t total = DQ_RHDR_LEN + nlen + plen + 4;
    unsigned char *buf = (unsigned char *)malloc(total);
    uint32_t crc;
    size_t w;
    if (!buf) return -1;
    put32(buf, DQ_REC_MAGIC);
    buf[4] = (unsigned char)type; buf[5] = 0;
    put16(buf + 6, nlen);
    put32(buf + 8, plen);
    put64(buf + 12, seq);
    if (nlen) memcpy(buf + DQ_RHDR_LEN, name, nlen);
    if (plen) memcpy(buf + DQ_RHDR_LEN + nlen, payload, plen);
    crc = fnv(buf, DQ_RHDR_LEN + nlen + plen, DQ_FNV_SEED);
    put32(buf + DQ_RHDR_LEN + nlen + plen, crc);
    w = fwrite(buf, 1, total, f);
    free(buf);
    return (w == total) ? 0 : -1;
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

static dq_rec *find_by_seq(gptps_dq *dq, uint64_t seq)
{
    size_t i;
    for (i = 0; i < dq->n; ++i) if (dq->recs[i].seq == seq) return &dq->recs[i];
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
            if (rc) {
                rc->seq = seq; rc->done = 0; rc->handle = 0; rc->len = plen;
                rc->name = (char *)malloc(nlen + 1);
                if (rc->name) { if (nlen) memcpy(rc->name, body, nlen); rc->name[nlen] = 0; }
                rc->payload = dup_mem(body + nlen, plen);
            }
        } else if (type == 'D') {
            dq_rec *rc = find_by_seq(dq, seq);
            if (rc) rc->done = 1;
        }
        free(body);
    }
    fclose(f);
    return 0;
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
    for (i = 0; i < dq->n; ++i)
        if (!dq->recs[i].done)
            write_record(t, 'P', dq->recs[i].seq, dq->recs[i].name, dq->recs[i].payload, (uint32_t)dq->recs[i].len);
    fflush(t); apx_fsync(t); fclose(t);

    if (dq->fp) { fclose(dq->fp); dq->fp = NULL; }
    if (rename(tmp, dq->path) != 0) { free(tmp); return GPTPS_E_IO; }
    free(tmp);
    dq->fp = fopen(dq->path, "ab");
    if (!dq->fp) return GPTPS_E_IO;

    /* compact memory: keep only !done */
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
    if (ev->kind != GPTPS_EV_FINISHED && ev->kind != GPTPS_EV_DEAD_LETTERED) return;
    apx_mutex_lock(&dq->mu);
    for (i = 0; i < dq->n; ++i) {
        if (!dq->recs[i].done && dq->recs[i].handle == ev->handle) {
            dq->recs[i].done = 1;
            if (dq->pending) dq->pending -= 1;
            write_record(dq->fp, 'D', dq->recs[i].seq, "", NULL, 0);
            fflush(dq->fp); /* DONE: no fsync (a lost DONE just replays = harmless) */
            break;
        }
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
    /* pending count after replay */
    { size_t i; for (i = 0; i < dq->n; ++i) if (!dq->recs[i].done) dq->pending += 1; }
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
    if (!dq || !task_name) return GPTPS_E_INVAL;

    apx_mutex_lock(&dq->mu);
    seq = dq->next_seq;
    if (write_record(dq->fp, 'P', seq, task_name, payload, (uint32_t)len) != 0) {
        apx_mutex_unlock(&dq->mu);
        return GPTPS_E_IO;
    }
    fflush(dq->fp); apx_fsync(dq->fp);   /* durable before we enqueue */
    dq->next_seq = seq + 1;

    rc = push_rec(dq);
    if (!rc) { apx_mutex_unlock(&dq->mu); return GPTPS_E_NOMEM; }
    rc->seq = seq; rc->done = 0; rc->handle = 0; rc->len = len;
    rc->name = dup_str(task_name);
    rc->payload = dup_mem(payload, len);
    dq->pending += 1;

    st = gptps_submit(dq->e, task_name, payload, len, &h);
    if (st == GPTPS_OK) {
        rc->handle = h;
        if (out_handle) *out_handle = h;
    } else {
        /* engine refused it: mark done so recovery won't replay a rejected task */
        rc->done = 1; if (dq->pending) dq->pending -= 1;
        write_record(dq->fp, 'D', seq, "", NULL, 0); fflush(dq->fp);
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
        if (dq->recs[i].done || dq->recs[i].handle != 0) continue; /* completed or already live */
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
