/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_durable_offset.c - the durable queue's journal offsets past 2 GiB.
 *
 * Offsets used to be `long`, read with ftell. Where long is 32 bits (Windows, MSVC and
 * mingw alike, and 32-bit POSIX) ftell returns -1 once the journal passes 2 GiB.
 * register_waiter then skipped the update, so the group commit's `synced` froze at
 * the last offset below 2 GiB while later fsyncs still acknowledged submits. The
 * first fsync that failed after that cut the journal back to the frozen offset -
 * and fsync'd the cut - deleting every record acknowledged since the crossing.
 *
 * Two parts:
 *   1. The 64-bit helpers (addon_compat.h: apx_ftell / apx_fseek / apx_truncate) on a
 *      sparse file past 4 GiB: position, read back and truncate there. Skipped, with
 *      a note, where the file system refuses a file that large.
 *   2. The group commit when an offset cannot be read. A 2 GiB journal is too slow to
 *      write in a test, so the add-on is compiled into this file with its offset
 *      read (and, for the code before the fix, ftell) redirected: while
 *      g_tell_fails is set, reading the journal's offset fails as it did past 2 GiB.
 *      A submit is acknowledged in that state, then the next fsync fails once. The
 *      acknowledged record must still be in the journal file. Before the fix it was
 *      cut away.
 * Portable: it runs on Windows (and under Wine) as well.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#  define _FILE_OFFSET_BITS 64
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "addon_compat.h"

static FILE *g_journal_fp;  /* the journal's append handle, once open */
static int   g_tell_fails;  /* set: reading the journal's offset fails, as past 2 GiB */
static int   g_fsync_fails; /* > 0: that many fsyncs of the journal fail (EIO) */
static int   g_fsyncs_failed;

static apx_off dq_test_ftell(FILE *f)
{
    if (g_tell_fails && f == g_journal_fp) return -1;
    return apx_ftell(f);
}
static long dq_test_ftell_long(FILE *f)     /* the code before the fix called ftell */
{
    if (g_tell_fails && f == g_journal_fp) return -1L;
    return ftell(f);
}
static int dq_test_fsync_fd(int fd)
{
    if (g_fsync_fails > 0 && g_journal_fp && fd == apx_fileno(g_journal_fp)) {
        g_fsync_fails -= 1; g_fsyncs_failed += 1;
        return -1;
    }
    return apx_fsync_fd(fd);
}
#define apx_ftell    dq_test_ftell
#define ftell        dq_test_ftell_long
#define apx_fsync_fd dq_test_fsync_fd
#include "../addons/gptps_durable_queue.c"
#undef apx_ftell
#undef ftell
#undef apx_fsync_fd

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

#define JOURNAL "dq_test_offset.journal"
#define BIGFILE "dq_test_offset.sparse"

static gptps_status task_nop(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_OK; }

static gptps *open_manual(void)
{
    gptps_config cfg; gptps *e = NULL; gptps_task_def d;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1; cfg.limits.max_memory_bytes = 64u << 20;
    cfg.mode = GPTPS_RUN_MANUAL;
    if (gptps_open_ex(&cfg, &e) != GPTPS_OK) return NULL;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "work"; d.run = task_nop; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    gptps_register_task(e, &d);
    return e;
}

/* whether the file at `path` holds the bytes `needle` */
static int file_has(const char *path, const char *needle)
{
    FILE *f = fopen(path, "rb");
    char *buf; long n; size_t k = strlen(needle), i; int found = 0;
    if (!f) return 0;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = (char *)malloc(n > 0 ? (size_t)n : 1);
    if (buf && n > 0 && fread(buf, 1, (size_t)n, f) == (size_t)n)
        for (i = 0; i + k <= (size_t)n && !found; ++i) found = memcmp(buf + i, needle, k) == 0;
    free(buf); fclose(f);
    return found;
}

static void helpers_past_4gib(void)
{
    const apx_off big = (apx_off)5 << 30;              /* 5 GiB: past 32 bits, not just 31 */
    FILE *f;
    remove(BIGFILE);
    f = fopen(BIGFILE, "w+b");
    CHECK(f != NULL);
    if (!f) return;
    if (apx_fseek(f, big, SEEK_SET) != 0 || fputc('x', f) == EOF || fflush(f) != 0) {
        printf("note: this file system refuses a sparse 5 GiB file; helper check skipped\n");
        fclose(f); remove(BIGFILE);
        return;
    }
    CHECK(apx_ftell(f) == big + 1);
    CHECK(apx_fseek(f, 0, SEEK_END) == 0 && apx_ftell(f) == big + 1);
    CHECK(apx_truncate(f, big - 7) == 0);                /* cut inside the hole */
    CHECK(apx_fseek(f, 0, SEEK_END) == 0 && apx_ftell(f) == big - 7);
    CHECK(apx_fseek(f, big - 8, SEEK_SET) == 0 && fgetc(f) == 0);
    CHECK(apx_truncate(f, -1) != 0);                     /* never a negative length */
    fclose(f);
    remove(BIGFILE);
}

static void unknown_offset_then_failed_fsync(void)
{
    gptps *e; gptps_dq *dq; gptps_handle h = 0;
    remove(JOURNAL);
    e = open_manual(); CHECK(e != NULL); if (!e) return;
    dq = gptps_dq_open(e, JOURNAL); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }
    g_journal_fp = dq->fp;

    CHECK(gptps_dq_submit(dq, "work", "REC-A-before", 12, &h) == GPTPS_OK);
    g_tell_fails = 1;                                    /* the journal "passed 2 GiB" */
    CHECK(gptps_dq_submit(dq, "work", "REC-B-acked!", 12, &h) == GPTPS_OK);
    CHECK(file_has(JOURNAL, "REC-B-acked!"));
    g_fsync_fails = 1;                                   /* one transient fsync failure */
    CHECK(gptps_dq_submit(dq, "work", "REC-C-failed", 12, &h) == GPTPS_E_IO);
    CHECK(g_fsyncs_failed == 1);
    /* The failure must not cut away what was acknowledged. With the offset unknown,
     * the queue breaks instead (and says so) - before the fix, B was gone. */
    CHECK(file_has(JOURNAL, "REC-A-before"));
    CHECK(file_has(JOURNAL, "REC-B-acked!"));
    g_tell_fails = 0;
    gptps_shutdown(e);
    gptps_dq_close(dq);
    g_journal_fp = NULL;

    /* and the next run recovers B */
    e = open_manual(); CHECK(e != NULL); if (!e) return;
    dq = gptps_dq_open(e, JOURNAL); CHECK(dq != NULL);
    if (dq) {
        size_t i; int b = 0;
        for (i = 0; i < dq->n; ++i)
            if (!dq->recs[i].done && dq->recs[i].payload && dq->recs[i].len == 12 &&
                memcmp(dq->recs[i].payload, "REC-B-acked!", 12) == 0) b = 1;
        CHECK(b);
    }
    gptps_shutdown(e);
    if (dq) gptps_dq_close(dq);
    remove(JOURNAL);
}

int main(void)
{
    (void)dq_test_ftell_long;   /* used only by the code before the fix */
    helpers_past_4gib();
    unknown_offset_then_failed_fsync();
    if (fails) { printf("%d durable-queue offset check(s) FAILED\n", fails); return 1; }
    printf("all durable-queue offset checks passed\n");
    return 0;
}
