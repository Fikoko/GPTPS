/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * fake_infer.c - a stand-in for one inference job, so the edge-AI demo runs on
 * any Linux machine: it takes memory the way a model does, and holds it a while.
 * It does no inference and needs no GPU. It is the JOB, not the host, so it does
 * not link GPTPS.
 *
 *   fake_infer --mb N [--ms T] [--fail-first FILE] [--runaway]
 *
 *   --mb N             take N MB, in 1 MB blocks, writing to every page so the
 *                      memory is really resident (RSS grows, not just the address
 *                      space). The blocks arrive evenly over the first quarter of
 *                      the run, as a model loading its weights would.
 *   --ms T             run for about T ms in all, then exit 0 (default 1000).
 *   --fail-first FILE  fail the first attempt on purpose (exit 1), to show a retry.
 *                      The first attempt writes the pid of the process that started
 *                      it (edge_admission) to FILE; an attempt that finds that pid
 *                      there removes FILE and runs normally. A FILE left by an
 *                      earlier run holds another pid, so it counts as absent.
 *   --runaway          a job that outgrows its declaration: after N MB it keeps
 *                      taking blocks at the same pace. Something has to stop it -
 *                      the cap GPTPS puts on the job (RLIMIT_AS refuses the
 *                      allocation; the cgroup's memory.max OOM-kills it), or the
 *                      kernel's OOM killer. If nothing does, it gives up 1024 MB
 *                      past N and exits 4, so even an unconfined run ends.
 *
 * Exit status: 0 done; 1 the planned first failure; 2 usage; 3 an allocation was
 * refused (what RLIMIT_AS does); 4 a runaway that nothing stopped. It prints
 * nothing when it succeeds, and one line on stderr when it does not.
 */
/* feature-test macros before any header (match hal_posix.c): clock_gettime,
 * nanosleep and getppid under -std=c99. On macOS _POSIX_C_SOURCE alone would hide
 * clock_gettime, so it is not used. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#  define _DARWIN_C_SOURCE
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MB             ((size_t)1 << 20)
#define RUNAWAY_EXTRA  1024ul           /* MB past --mb before a runaway gives up */

/* The blocks hang off a global with external linkage, so the compiler cannot prove
 * them unused and drop the allocations, or the writes that make them resident. */
char **held;

typedef unsigned long long msec;        /* 64 bits even where long is 32 */

static msec now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (msec)ts.tv_sec * 1000u + (msec)(ts.tv_nsec / 1000000L);
}

static void sleep_until(msec t)
{
    msec now = now_ms();
    struct timespec ts;
    if (t <= now) return;
    ts.tv_sec = (time_t)((t - now) / 1000u);
    ts.tv_nsec = (long)((t - now) % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
}

static int parse_ul(const char *s, unsigned long *out)
{
    char *end;
    if (!s || !*s || *s == '-') return 0;
    errno = 0;
    *out = strtoul(s, &end, 10);
    return errno == 0 && *end == '\0';
}

/* 1 if this is the attempt that should fail. The parent pid ties the mark to one
 * run of the host: the host forks every attempt itself, so a retry sees the same
 * parent, and the next run of the demo does not. */
static int first_attempt(const char *file)
{
    long me = (long)getppid(), was = -1;
    FILE *f = fopen(file, "r");
    if (f) {
        if (fscanf(f, "%ld", &was) != 1) was = -1;
        fclose(f);
    }
    if (was == me) {                     /* the attempt before this one failed: run */
        remove(file);
        return 0;
    }
    f = fopen(file, "w");
    if (!f || fprintf(f, "%ld\n", me) < 0) {
        fprintf(stderr, "fake_infer: --fail-first: cannot write %s, so every attempt will fail\n", file);
    }
    if (f) fclose(f);
    return 1;
}

int main(int argc, char **argv)
{
    unsigned long mb = 0, ms = 1000, i, limit;
    msec t0;
    const char *fail_file = NULL;
    int runaway = 0, a;

    for (a = 1; a < argc; ++a) {
        if (!strcmp(argv[a], "--mb") && a + 1 < argc && parse_ul(argv[a + 1], &mb)) ++a;
        else if (!strcmp(argv[a], "--ms") && a + 1 < argc && parse_ul(argv[a + 1], &ms)) ++a;
        else if (!strcmp(argv[a], "--fail-first") && a + 1 < argc) fail_file = argv[++a];
        else if (!strcmp(argv[a], "--runaway")) runaway = 1;
        else {
            fprintf(stderr, "usage: fake_infer --mb N [--ms T] [--fail-first FILE] [--runaway]\n");
            return 2;
        }
    }
    if (fail_file && first_attempt(fail_file)) {
        fprintf(stderr, "fake_infer: --fail-first: failing this first attempt on purpose\n");
        return 1;
    }

    limit = runaway ? mb + RUNAWAY_EXTRA : mb;
    held = (char **)calloc(limit ? limit : 1, sizeof *held);
    if (!held) { fprintf(stderr, "fake_infer: allocation refused before starting\n"); return 3; }

    /* Block i is due at t0 + (T/4) * i / N: N MB over the first quarter of the run,
     * and a runaway keeps the same pace past N. */
    t0 = now_ms();
    for (i = 0; i < limit; ++i) {
        if (mb) sleep_until(t0 + (msec)ms / 4 * i / mb);
        held[i] = (char *)malloc(MB);
        if (!held[i]) {
            fprintf(stderr, "fake_infer: %sallocation refused at %lu MB\n",
                    runaway ? "--runaway: " : "", i);
            return 3;
        }
        memset(held[i], (int)(i & 0x7f) + 1, MB);     /* touch every page: resident */
    }
    if (runaway) {
        fprintf(stderr, "fake_infer: --runaway: nothing stopped it at %lu MB; giving up\n", limit);
        return 4;
    }
    sleep_until(t0 + ms);
    return 0;   /* exit returns the memory; freeing 1 MB blocks one by one buys nothing */
}
