/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * prog_helper.c - a tiny, portable external program used by test_program_helper
 * to exercise GPTPS_EXEC_PROGRAM on every platform (no /bin/sh dependency).
 *   argv[1] = "cat"   : copy stdin -> stdout
 *             "upper" : copy stdin -> stdout, upper-casing a-z
 *             "exit"  : exit with code argv[2]
 *             "hang"  : spin forever (until the executor hard-kills it on timeout)
 *             "eofhang": write a byte, CLOSE stdout, then spin forever. Stdout EOF
 *                       tells the parent "the child is done" while the process is
 *                       very much alive - the shape that used to leave the executor
 *                       blocked in waitpid() with no deadline to rescue it.
 *             "zeros" : write exactly argv[2] zero bytes, then exit 0 - a result of
 *                       a chosen size, for the 16 MiB result cap.
 *             "mem"   : take argv[2] MiB, writing every page so it is resident and
 *                       committed, hold it argv[3] ms, then exit argv[4] (default 0) -
 *                       a job of known size, for test_measure.
 *             "memstep": "mem" whose size depends on the attempt: argv[2] names a
 *                       file; the run that finds it missing creates it and takes
 *                       argv[3] MiB, every later run takes argv[4] MiB; then exit
 *                       argv[5] (default 0) - attempts told apart, for test_xport_measure.
 *             "spin"  : burn argv[2] ms of CPU time, then exit 0.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L   /* nanosleep under -std=c99 */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#  include <io.h>
#  include <fcntl.h>
#  include <windows.h>
#else
#  include <unistd.h>
#endif

/* Kept reachable, so the compiler cannot drop the allocation or its writes, nor the
 * "spin" loop's work. */
char *g_held;
volatile unsigned long g_spins;

/* CPU time this process has used, in ms. clock() measures that on POSIX, but on the
 * Windows CRT it is wall time since the process started. */
static unsigned long cpu_ms(void)
{
#if defined(_WIN32)
    FILETIME c, e, k, u;
    ULARGE_INTEGER a, b;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return (unsigned long)-1;
    a.LowPart = k.dwLowDateTime; a.HighPart = k.dwHighDateTime;
    b.LowPart = u.dwLowDateTime; b.HighPart = u.dwHighDateTime;
    return (unsigned long)((a.QuadPart + b.QuadPart) / 10000u);   /* 100 ns units */
#else
    return (unsigned long)((double)clock() * 1000.0 / (double)CLOCKS_PER_SEC);
#endif
}

/* Take `mb` MiB, every page written so it is resident and committed. */
static int take_mb(size_t mb)
{
    g_held = (char *)malloc(mb ? mb << 20 : 1);
    if (!g_held) return -1;
    memset(g_held, 1, mb << 20);
    return 0;
}

static void sleep_ms(unsigned long ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u); ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) != 0) { }
#endif
}

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "cat";
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);   /* exact byte round-trip (no CRLF translation) */
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (strcmp(mode, "exit") == 0) return (argc > 2) ? atoi(argv[2]) : 0;
    if (strcmp(mode, "hang") == 0) { for (;;) { /* killed by the deadline */ } }
    if (strcmp(mode, "eofhang") == 0) {
        putchar('x'); fflush(stdout);
#if defined(_WIN32)
        _close(_fileno(stdout));
#else
        close(STDOUT_FILENO);
#endif
        for (;;) { /* alive but silent: the parent must not wait for us forever */ }
    }
    if (strcmp(mode, "mem") == 0) {
        size_t mb = (argc > 2) ? (size_t)strtoul(argv[2], NULL, 10) : 0;
        unsigned long hold = (argc > 3) ? strtoul(argv[3], NULL, 10) : 0;
        if (take_mb(mb) != 0) return 3;
        sleep_ms(hold);
        return (argc > 4) ? atoi(argv[4]) : 0;
    }
    if (strcmp(mode, "memstep") == 0) {
        FILE *f = (argc > 2) ? fopen(argv[2], "rb") : NULL;
        int first = (f == NULL);
        size_t mb;
        if (f) fclose(f);
        else if (argc > 2 && (f = fopen(argv[2], "wb")) != NULL) fclose(f);
        mb = (size_t)strtoul(argc > (first ? 3 : 4) ? argv[first ? 3 : 4] : "0", NULL, 10);
        if (take_mb(mb) != 0) return 3;
        return (argc > 5) ? atoi(argv[5]) : 0;
    }
    if (strcmp(mode, "spin") == 0) {
        unsigned long ms = (argc > 2) ? strtoul(argv[2], NULL, 10) : 0;
        while (cpu_ms() < ms) g_spins = g_spins + 1;
        return 0;
    }
    if (strcmp(mode, "zeros") == 0) {
        static char chunk[65536];                    /* zero-initialised */
        unsigned long left = (argc > 2) ? strtoul(argv[2], NULL, 10) : 0;
        while (left) {
            size_t n = (left < sizeof chunk) ? (size_t)left : sizeof chunk;
            if (fwrite(chunk, 1, n, stdout) != n) return 1;
            left -= (unsigned long)n;
        }
        return fflush(stdout) == 0 ? 0 : 1;
    }
    {
        int up = (strcmp(mode, "upper") == 0), c;
        while ((c = getchar()) != EOF) {
            if (up && c >= 'a' && c <= 'z') c -= 32;
            putchar(c);
        }
    }
    fflush(stdout);
    return 0;
}
