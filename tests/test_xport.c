/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_xport.c - the worker-process transport (scale-out). Proves work submitted
 * here executes in a SEPARATE process (the result carries the worker's pid, which
 * differs from the parent's) and fans out across all worker processes, and that the
 * handler's status and result round-trip correctly. POSIX only (fork + socketpair).
 */
#define _POSIX_C_SOURCE 200809L
#include "gptps_xport.h"
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

/* Async replies land on the reader thread; count them atomically. */
typedef struct { int calls; int io_errs; } replies;
static void count_reply(uint64_t id, gptps_status io, gptps_status ts, const void *res,
                        size_t len, void *u)
{
    replies *r = (replies *)u; (void)id; (void)ts; (void)res; (void)len;
    if (io != GPTPS_OK) __atomic_add_fetch(&r->io_errs, 1, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&r->calls, 1, __ATOMIC_SEQ_CST);
}
static int load(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static void store(int *p, int v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }

/* A reply callback that re-enters the transport, both ways. */
static gptps_xport *reenter_xp;
static int reenter_blocking = -1, reenter_async = -1, reenter_done;
static replies nested = {0, 0};
static void reenter_reply(uint64_t id, gptps_status io, gptps_status ts, const void *res,
                          size_t len, void *u)
{
    void *r = NULL; size_t rl = 0; gptps_status t = GPTPS_OK;
    (void)id; (void)io; (void)ts; (void)res; (void)len; (void)u;
    store(&reenter_blocking, (int)gptps_xport_submit(reenter_xp, "upper", "b", 1, &r, &rl, &t));
    free(r);
    store(&reenter_async, (int)gptps_xport_submit_async(reenter_xp, "upper", "c", 1,
                                                         count_reply, &nested, NULL));
    store(&reenter_done, 1);
}

/* An E_IO callback that retries synchronously. */
static int dead_io = -1, dead_retry = -1;
static void retry_on_io(uint64_t id, gptps_status io, gptps_status ts, const void *res,
                        size_t len, void *u)
{
    void *r = NULL; size_t rl = 0; gptps_status t = GPTPS_E_IO;
    (void)id; (void)ts; (void)res; (void)len; (void)u;
    store(&dead_io, (int)io);
    store(&dead_retry, (int)gptps_xport_submit(reenter_xp, "upper", "d", 1, &r, &rl, &t));
    free(r);
    store(&reenter_done, 1);
}

/* handler run IN THE WORKER PROCESS:
 *   "upper" -> [int32 worker-pid][payload upper-cased]   (proves separate process)
 *   "fail"  -> GPTPS_E_TASK, no result
 *   "empty" -> GPTPS_OK, NULL result */
static gptps_status xrun(const char *task, const void *payload, size_t len,
                         void **out, size_t *outlen, void *ud)
{
    (void)ud;
    /* Kill this worker mid-frame: the parent is waiting on a reply that will never
     * come, which is exactly how a real link breaks. */
    if (strcmp(task, "die") == 0)   _exit(1);
    /* Die LATER, after lingering long enough that the parent is stuck writing the
     * next frame into a full socket - so that write fails as this process exits. */
    if (strcmp(task, "stall_die") == 0) {
        struct timespec ts = {0, 20000000L};           /* 20 ms */
        nanosleep(&ts, NULL);
        _exit(1);
    }
    if (strcmp(task, "fail") == 0)  return GPTPS_E_TASK;
    if (strcmp(task, "empty") == 0) { *out = NULL; *outlen = 0; return GPTPS_OK; }
    {
        int32_t pid = (int32_t)getpid();
        size_t rlen = 4 + len, i;
        unsigned char *r = (unsigned char *)malloc(rlen ? rlen : 1);
        if (!r) return GPTPS_E_NOMEM;
        memcpy(r, &pid, 4);
        for (i = 0; i < len; ++i) {
            unsigned char c = ((const unsigned char *)payload)[i];
            r[4 + i] = (c >= 'a' && c <= 'z') ? (unsigned char)(c - 32) : c;
        }
        *out = r; *outlen = rlen;
        return GPTPS_OK;
    }
}

int main(void)
{
    gptps_xport *xp;
    int parent = (int)getpid();
    int seen[3]; int nseen = 0;
    int k;

    xp = gptps_xport_open(3, xrun, NULL);
    CHECK(xp); if (!xp) { printf("open failed\n"); return 1; }
    CHECK(gptps_xport_count(xp) == 3);

    /* six round-robin submits => all 3 workers used, each result from a real
     * separate process (distinct pid, none equal to the parent's) */
    for (k = 0; k < 6; ++k) {
        void *res = NULL; size_t rl = 0; gptps_status ts = GPTPS_E_IO;
        gptps_status st = gptps_xport_submit(xp, "upper", "hello", 5, &res, &rl, &ts);
        CHECK(st == GPTPS_OK);
        CHECK(ts == GPTPS_OK);
        CHECK(rl == 9);
        if (res && rl == 9) {
            int32_t wpid; int j, found = 0;
            memcpy(&wpid, res, 4);
            CHECK((int)wpid != parent);                          /* ran out-of-process */
            CHECK(memcmp((char *)res + 4, "HELLO", 5) == 0);     /* transform round-tripped */
            for (j = 0; j < nseen; ++j) if (seen[j] == (int)wpid) found = 1;
            if (!found && nseen < 3) seen[nseen++] = (int)wpid;
        }
        free(res);
    }
    CHECK(nseen == 3);   /* work fanned out across all three worker processes */

    /* handler error status round-trips (transport OK, task status is the error) */
    {
        void *res = NULL; size_t rl = 0; gptps_status ts = GPTPS_OK;
        CHECK(gptps_xport_submit(xp, "fail", NULL, 0, &res, &rl, &ts) == GPTPS_OK);
        CHECK(ts == GPTPS_E_TASK);
        CHECK(res == NULL);
        free(res);
    }
    /* empty result round-trips */
    {
        void *res = (void *)1; size_t rl = 99; gptps_status ts = GPTPS_E_IO;
        CHECK(gptps_xport_submit(xp, "empty", NULL, 0, &res, &rl, &ts) == GPTPS_OK);
        CHECK(ts == GPTPS_OK);
        CHECK(res == NULL);
        CHECK(rl == 0);
    }
    /* bad args */
    CHECK(gptps_xport_submit(NULL, "x", NULL, 0, NULL, NULL, NULL) == GPTPS_E_INVAL);

    gptps_xport_close(xp);   /* returns => all workers reaped cleanly */

    /* A retired worker must leave the ROTATION, not just refuse work.
     *
     * A link that breaks mid-frame cannot be resynchronised - the next submit would
     * read this frame's leftovers as its own status and hand back a fabricated reply
     * - so the worker is retired permanently. But the round-robin used to hand it
     * every Nth submit anyway, so one dead worker in four turned a quarter of all
     * subsequent work into GPTPS_E_IO while three healthy workers sat idle. Measured
     * on the pre-fix build: 10 of 40 submits failed. It must now be 0, and
     * gptps_xport_live() must report the real remaining capacity. */
    {
        gptps_xport *xd = gptps_xport_open(4, xrun, NULL);
        CHECK(xd != NULL);
        if (xd) {
            void *res = NULL; size_t rl = 0; gptps_status ts = GPTPS_OK;
            int i, io = 0;
            CHECK(gptps_xport_count(xd) == 4);
            CHECK(gptps_xport_live(xd) == 4);

            (void)gptps_xport_submit(xd, "die", NULL, 0, &res, &rl, &ts);
            free(res);
            CHECK(gptps_xport_live(xd) == 3);       /* retired, and counted as such */
            CHECK(gptps_xport_count(xd) == 4);      /* pool SIZE does not change */

            for (i = 0; i < 40; ++i) {
                res = NULL; rl = 0;
                if (gptps_xport_submit(xd, "upper", "x", 1, &res, &rl, &ts) != GPTPS_OK) ++io;
                free(res);
            }
            if (io != 0) {
                printf("FAIL %d/40 submits hit the retired worker\n", io);
                ++fails;
            }
            CHECK(gptps_xport_live(xd) == 3);       /* nothing else died */
            gptps_xport_close(xd);
        }
    }

    /* An async submit whose frame WRITE fails because the worker died under it has
     * exactly one outcome: an error from submit_async, or the callback - not both,
     * not neither.
     *
     * The worker lingers in "stall_die" and never reads the second frame, so the
     * parent blocks writing a payload far larger than the socket's send buffer; then
     * the worker exits. The submitter's send() fails, and the reader sees the same
     * death as EOF and fails everything outstanding - two threads racing for one
     * record. When the reader got it first, the submitter read the record
     * fail_all() was reporting and freeing, returned E_IO, and submit_async freed it
     * as well - one failure reported twice, and a use-after-free plus a double free
     * in whichever order the two threads ran. Measured before the fix: every run of
     * this block failed - Release aborted on heap corruption, ASan reported a
     * heap-use-after-free in send_request or fail_all, or a double free.
     *   Either winner is legal. The reader winning is the side that used to crash, so
     * the loop runs on past 40 rounds (to at most 200) until it has won at least
     * once; on one CPU or under TSan it wins almost every round. */
    {
        /* AF_UNIX send buffers default to ~208 KiB on Linux and 8 KiB on macOS; a host
         * tuned so the whole frame fits would never fail the write, so leave room. */
        size_t big = (size_t)32 << 20;
        char *buf = (char *)calloc(1, big);
        int round, by_submit = 0, by_reader = 0;
        CHECK(buf != NULL);
        for (round = 0; buf && (round < 40 || (by_reader == 0 && round < 200)); ++round) {
            replies first = {0, 0}, second = {0, 0};
            gptps_xport *xd = gptps_xport_open(1, xrun, NULL);
            gptps_status st;
            int outcomes;
            CHECK(xd != NULL);
            if (!xd) break;
            CHECK(gptps_xport_submit_async(xd, "stall_die", NULL, 0, count_reply, &first, NULL) == GPTPS_OK);
            st = gptps_xport_submit_async(xd, "upper", buf, big, count_reply, &second, NULL);
            gptps_xport_close(xd);      /* joins the reader: every callback has run */

            CHECK(load(&first.calls) == 1 && load(&first.io_errs) == 1);
            outcomes = (st != GPTPS_OK) + load(&second.calls);
            if (outcomes != 1) {
                printf("FAIL round %d: submit_async returned %d and its callback ran %d time(s)\n",
                       round, (int)st, load(&second.calls));
                ++fails;
            }
            if (st == GPTPS_OK) { CHECK(load(&second.io_errs) == 1); ++by_reader; }
            else                { CHECK(st == GPTPS_E_IO); ++by_submit; }
        }
        free(buf);
        if (by_reader == 0)             /* legal, but then this run proved less */
            printf("note: the reader never won in %d rounds; the formerly crashing side went unexercised\n", round);
        printf("write-failure race: %d/%d reported by submit_async, %d by the callback\n",
               by_submit, round, by_reader);
    }

    /* A reply callback runs on its link's reader thread, and only that thread can
     * complete the link's requests. A BLOCKING submit from the callback that lands on
     * the same link - always, with one worker - waited for a reply only it could
     * deliver, and never returned; the header used to invite exactly that call. It is
     * refused now, and submit_async still works from there. Before the fix the
     * callback never comes back, and close() then hangs joining its thread. */
    {
        fflush(stdout);                 /* the fork below must not inherit unwritten output */
        reenter_xp = gptps_xport_open(1, xrun, NULL);
        CHECK(reenter_xp != NULL);
        if (reenter_xp) {
            struct timespec ts = {0, 1000000L};   /* 1 ms */
            int spins = 0;
            CHECK(gptps_xport_submit_async(reenter_xp, "upper", "a", 1, reenter_reply, NULL, NULL) == GPTPS_OK);
            while (!load(&reenter_done) && spins++ < 5000) nanosleep(&ts, NULL);
            CHECK(load(&reenter_done) == 1);
            CHECK(load(&reenter_blocking) == GPTPS_E_BUSY);
            CHECK(load(&reenter_async) == GPTPS_OK);
            if (!load(&reenter_done)) {         /* the callback is stuck: close would hang */
                printf("FAIL the reply callback never returned\n");
                fflush(stdout);
                return 1;
            }
            gptps_xport_close(reenter_xp);          /* drains: the nested reply lands */
            CHECK(load(&nested.calls) == 1 && load(&nested.io_errs) == 0);
        }
    }

    /* ...except from an E_IO callback: its link is already dead, so nothing can be
     * waiting on this reader, and a synchronous retry there lands on a live link. That
     * worked before the refusal existed and must keep working. */
    {
        fflush(stdout);
        reenter_xp = gptps_xport_open(2, xrun, NULL);
        CHECK(reenter_xp != NULL);
        if (reenter_xp) {
            struct timespec ts = {0, 1000000L};
            int spins = 0;
            store(&reenter_done, 0); store(&dead_io, -1); store(&dead_retry, -1);
            CHECK(gptps_xport_submit_async(reenter_xp, "die", NULL, 0, retry_on_io, NULL, NULL) == GPTPS_OK);
            while (!load(&reenter_done) && spins++ < 5000) nanosleep(&ts, NULL);
            CHECK(load(&reenter_done) == 1);
            CHECK(load(&dead_io) == GPTPS_E_IO);
            CHECK(load(&dead_retry) == GPTPS_OK);   /* retried on the live worker */
            if (!load(&reenter_done)) { printf("FAIL the E_IO callback never returned\n"); fflush(stdout); return 1; }
            gptps_xport_close(reenter_xp);
        }
    }

    if (fails) { printf("%d xport check(s) FAILED\n", fails); return 1; }
    printf("all xport (scale-out) checks passed\n");
    return 0;
}
