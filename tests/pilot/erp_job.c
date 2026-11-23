/* SPDX-License-Identifier: MIT */
/* Copyright (c) nightops00dev */
/* Adapted from nightops00dev's night-window harnesses,
 * https://github.com/nightops00dev/gptps-night-window-harness, MIT; see Fikoko/GPTPS#12 */
/*
 * erp_job.c - a stand-in for one of the ERP's batch executables (pilot passes two
 * and four).
 *
 * Reads one line from stdin: "<ms> <exit_code> <marker>", writes a chatty
 * stderr log ending in a distinctive TAIL-EVIDENCE line (so the engine's
 * stderr_tail_bytes can be seen to keep the LAST bytes, not the first),
 * sleeps <ms>, exits <exit_code>. Exit codes per the vendor convention the
 * pilot described: 0 ok, 1 runtime/lock, 2 bad input, 3 licence.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L   /* nanosleep */
#endif
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void sleep_ms(int ms) { if (ms > 0) Sleep((DWORD)ms); }
#else
#include <time.h>
static void sleep_ms(int ms)
{
    struct timespec ts;
    if (ms <= 0) return;
    ts.tv_sec = (time_t)(ms / 1000); ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0) { }
}
#endif

int main(void)
{
    char line[256] = {0};
    int ms = 0, code = 0, i;
    char marker[64] = {0};

    if (!fgets(line, sizeof line, stdin)) {
        fprintf(stderr, "ERPJOB no-stdin\n");
        return 4;
    }
    if (sscanf(line, "%d %d %63s", &ms, &code, marker) < 2) {
        fprintf(stderr, "ERPJOB bad-input-line\n");
        return 2;
    }

    fprintf(stderr, "ERPJOB %s begin\n", marker);
    for (i = 0; i < 20; ++i)
        fprintf(stderr, "ERPJOB %s filler %02d noise padding xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n",
                marker, i);
    fprintf(stderr, "ERPJOB %s TAIL-EVIDENCE exit=%d marker=%s\n", marker, code, marker);
    fflush(stderr);

    sleep_ms(ms);
    return code;
}
