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
 *             "eofsleep": write "x", then point stdout elsewhere (`exec >/dev/null`, as a
 *                       wrapper that logs to a file does), sleep argv[2] ms, write
 *                       "late" to stderr and exit argv[3] (default 0) - a program
 *                       that is NOT done when its stdout reaches EOF.
 *             "sigstate": POSIX: print "term=<1 if SIGTERM is blocked> pipe=<1 if
 *                       SIGPIPE is ignored>" - the signal state it was started with.
 *                       Elsewhere "term=0 pipe=0".
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
 *             "cwdfile": print the file argv[2], opened relative to the current
 *                       directory - where the program was started (work_dir).
 *             "env"   : for each name in argv[2..], print "NAME=value\n", or
 *                       "NAME unset\n" - the environment the program was given.
 *             "arg"   : print argv[2] exactly as received, as UTF-8 (on Windows
 *                       read from the wide command line, which the ANSI argv of
 *                       main() would make lossy).
 *             "sig"   : end by a signal (POSIX: SIGTERM, raised on itself).
 *             "errout": write argv[2] bytes of '.' to stderr, then the line argv[3],
 *                       then exit argv[4] (default 3) - a chatty program that fails.
 *             "term"  : POSIX: on SIGTERM write "cleaned" to the file argv[2] and
 *                       exit 0; until then, sleep - a program that cleans up. Before
 *                       the file: wait argv[3] ms, then write argv[4] bytes to stdout
 *                       (both default 0) - a slow clean-up, a noisy one.
 *             "termignore": ignore SIGTERM and sleep - one that never does.
 *             "brk"   : Windows: on Ctrl+Break write "cleaned" to the file argv[2]
 *                       and exit 0; until then, sleep - "term", the Windows way, with
 *                       the same argv[3] / argv[4].
 *             "brkignore": Windows: on Ctrl+Break write "seen" to the file argv[2]
 *                       and carry on sleeping - the handler swallows the event (returns
 *                       TRUE without exiting), as a batch program that will not be
 *                       interrupted does. "termignore", the Windows way: one that the
 *                       executor must end itself when the grace runs out.
 *             "spawn" : a launcher: start this helper again as argv[2..] (in the same
 *                       process group, and console), then sleep with the default stop
 *                       action - so the launcher dies at once and its worker cleans up.
 *             "pidsleep": write this process's id to the file argv[2], then sleep.
 *             "diskwrite": write argv[3] MiB to the file argv[2] past the cache
 *                       (Windows: unbuffered, write-through; POSIX: fsync), then
 *                       delete it - I/O that a bandwidth cap has to slow down.
 *             "flood" : the stop handler of "term" ("brk" on Windows) for the file
 *                       argv[2], then argv[3] bytes on stdout, then sleep - a program
 *                       that logs to stdout past the 16 MiB result cap.
 *             "lateflood": sleep argv[2] ms, then argv[3] bytes on stdout, then
 *                       sleep - output past the cap that comes only after a stop
 *                       for another reason has begun.
 *             "envsorted": Windows: print "sorted" if this process's environment
 *                       block is in Windows' order (names, ordinal, ignoring case),
 *                       else "unsorted at NAME". Elsewhere: "sorted".
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
#  include <shellapi.h>   /* CommandLineToArgvW */
#else
#  include <signal.h>
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

#if !defined(_WIN32)
/* "term": what a program that cleans up does on SIGTERM (open/write/_exit are
 * async-signal-safe) */
#  include <fcntl.h>
static const char *g_term_file;
static unsigned long g_clean_ms, g_clean_spam;
static void on_term(int sig)
{
    int fd;
    static char xs[4096];
    unsigned long left = g_clean_spam;
    (void)sig;
    if (g_clean_ms) {
        struct timespec ts;
        ts.tv_sec = (time_t)(g_clean_ms / 1000u); ts.tv_nsec = (long)(g_clean_ms % 1000u) * 1000000L;
        while (nanosleep(&ts, &ts) != 0) { }
    }
    memset(xs, 'x', sizeof xs);
    while (left) {                          /* noisy: a blocked write would hang us here */
        size_t n = left < sizeof xs ? (size_t)left : sizeof xs;
        ssize_t w = write(STDOUT_FILENO, xs, n);
        if (w <= 0) break;
        left -= (unsigned long)w;
    }
    if (g_term_file && (fd = open(g_term_file, O_WRONLY | O_CREAT | O_TRUNC, 0600)) >= 0) {
        ssize_t w = write(fd, "cleaned", 7);
        (void)w;
        close(fd);
    }
    _exit(0);
}
#endif

#if defined(_WIN32)
static const char *g_brk_file;
static unsigned long g_clean_ms, g_clean_spam;
static BOOL WINAPI on_brk(DWORD kind)
{
    static char xs[4096];
    unsigned long left = g_clean_spam;
    if (kind != CTRL_BREAK_EVENT) return FALSE;
    if (g_clean_ms) Sleep((DWORD)g_clean_ms);
    memset(xs, 'x', sizeof xs);
    while (left) {
        DWORD w = 0, n = (DWORD)(left < sizeof xs ? left : sizeof xs);
        if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), xs, n, &w, NULL) || !w) break;
        left -= w;
    }
    if (g_brk_file) {
        FILE *f = fopen(g_brk_file, "wb");
        if (f) { fputs("cleaned", f); fclose(f); }
    }
    ExitProcess(0);
    return TRUE;
}

/* "brkignore": a separate handler, so "brk" stays exactly what it was. Swallows
 * Ctrl+Break and keeps the process running; leaves evidence that it did. */
static const char *g_brk_seen_file;
static BOOL WINAPI on_brk_ignore(DWORD kind)
{
    if (kind != CTRL_BREAK_EVENT) return FALSE;
    if (g_brk_seen_file) {
        FILE *f = fopen(g_brk_seen_file, "wb");
        if (f) { fputs("seen", f); fclose(f); }
    }
    return TRUE;                  /* handled - and nothing else: the program runs on */
}

/* a wide string, written to stdout as UTF-8 */
static void put_w(const wchar_t *w)
{
    char buf[4096];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)sizeof buf, NULL, NULL);
    if (n > 1) fwrite(buf, 1, (size_t)n - 1, stdout);
}
#endif

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "cat";
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);   /* exact byte round-trip (no CRLF translation) */
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (strcmp(mode, "cwdfile") == 0) {
        FILE *f = (argc > 2) ? fopen(argv[2], "rb") : NULL;
        int c;
        if (!f) return 4;
        while ((c = fgetc(f)) != EOF) putchar(c);
        fclose(f);
        fflush(stdout);
        return 0;
    }
    if (strcmp(mode, "env") == 0) {
        int i;
        for (i = 2; i < argc; ++i) {
#if defined(_WIN32)
            wchar_t name[256], val[4096];
            DWORD n;
            MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, name, 256);
            n = GetEnvironmentVariableW(name, val, 4096);
            fputs(argv[i], stdout);
            if (n == 0 || n >= 4096) fputs(" unset\n", stdout);
            else { putchar('='); put_w(val); putchar('\n'); }
#else
            const char *v = getenv(argv[i]);
            if (v) printf("%s=%s\n", argv[i], v); else printf("%s unset\n", argv[i]);
#endif
        }
        fflush(stdout);
        return 0;
    }
    if (strcmp(mode, "arg") == 0) {
#if defined(_WIN32)
        int wn = 0;
        wchar_t **wv = CommandLineToArgvW(GetCommandLineW(), &wn);
        if (!wv || wn < 3) return 5;
        put_w(wv[2]);
        LocalFree(wv);
#else
        if (argc < 3) return 5;
        fputs(argv[2], stdout);
#endif
        fflush(stdout);
        return 0;
    }
    if (strcmp(mode, "errout") == 0) {
        static char dots[65536];
        unsigned long left = (argc > 2) ? strtoul(argv[2], NULL, 10) : 0;
        memset(dots, '.', sizeof dots);
        while (left) {
            size_t n = (left < sizeof dots) ? (size_t)left : sizeof dots;
            if (fwrite(dots, 1, n, stderr) != n) return 1;
            left -= (unsigned long)n;
        }
        if (argc > 3) { fputs(argv[3], stderr); fputc('\n', stderr); }
        fflush(stderr);
        return (argc > 4) ? atoi(argv[4]) : 3;
    }
#if !defined(_WIN32)
    if (strcmp(mode, "term") == 0) {
        g_term_file = (argc > 2) ? argv[2] : NULL;
        g_clean_ms = (argc > 3) ? strtoul(argv[3], NULL, 10) : 0;
        g_clean_spam = (argc > 4) ? strtoul(argv[4], NULL, 10) : 0;
        signal(SIGTERM, on_term);
        for (;;) sleep_ms(10);
    }
    if (strcmp(mode, "termignore") == 0) {
        signal(SIGTERM, SIG_IGN);
        for (;;) sleep_ms(10);
    }
#endif
    if (strcmp(mode, "flood") == 0 && argc > 3) {
        /* the stop handler of "term" / "brk" for the file argv[2], then argv[3] bytes of
         * 'y' on stdout, then sleep - a program that logs to stdout, past the result cap */
        static char ys[65536];
        unsigned long left = strtoul(argv[3], NULL, 10);
#if defined(_WIN32)
        g_brk_file = argv[2];
        SetConsoleCtrlHandler(on_brk, TRUE);
#else
        g_term_file = argv[2];
        signal(SIGTERM, on_term);
#endif
        memset(ys, 'y', sizeof ys);
        while (left) {
            size_t n = (left < sizeof ys) ? (size_t)left : sizeof ys;
            if (fwrite(ys, 1, n, stdout) != n) break;
            left -= (unsigned long)n;
        }
        fflush(stdout);
        for (;;) sleep_ms(10);
    }
    if (strcmp(mode, "lateflood") == 0 && argc > 3) {
        static char ys[65536];
        unsigned long left = strtoul(argv[3], NULL, 10);
        sleep_ms(strtoul(argv[2], NULL, 10));
        memset(ys, 'y', sizeof ys);
        while (left) {
            size_t n = (left < sizeof ys) ? (size_t)left : sizeof ys;
            if (fwrite(ys, 1, n, stdout) != n) break;
            left -= (unsigned long)n;
        }
        fflush(stdout);
        for (;;) sleep_ms(10);
    }
#if defined(_WIN32)
    if (strcmp(mode, "brk") == 0) {
        g_brk_file = (argc > 2) ? argv[2] : NULL;
        g_clean_ms = (argc > 3) ? strtoul(argv[3], NULL, 10) : 0;
        g_clean_spam = (argc > 4) ? strtoul(argv[4], NULL, 10) : 0;
        SetConsoleCtrlHandler(on_brk, TRUE);
        for (;;) sleep_ms(10);
    }
    if (strcmp(mode, "brkignore") == 0) {
        g_brk_seen_file = (argc > 2) ? argv[2] : NULL;
        SetConsoleCtrlHandler(on_brk_ignore, TRUE);
        for (;;) sleep_ms(10);
    }
#endif
    if (strcmp(mode, "diskwrite") == 0 && argc > 3) {
        unsigned long mb = strtoul(argv[3], NULL, 10), i;
#if defined(_WIN32)
        DWORD w;
        void *buf = VirtualAlloc(NULL, 1u << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);  /* page-aligned */
        HANDLE f = CreateFileA(argv[2], GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, NULL);
        if (!buf || f == INVALID_HANDLE_VALUE) return 8;
        memset(buf, 7, 1u << 20);
        for (i = 0; i < mb; ++i) if (!WriteFile(f, buf, 1u << 20, &w, NULL) || w != (1u << 20)) { CloseHandle(f); return 9; }
        CloseHandle(f);
        DeleteFileA(argv[2]);
#else
        static char buf[1u << 20];
        FILE *f = fopen(argv[2], "wb");
        if (!f) return 8;
        memset(buf, 7, sizeof buf);
        for (i = 0; i < mb; ++i) if (fwrite(buf, 1, sizeof buf, f) != sizeof buf) { fclose(f); return 9; }
        fflush(f); fsync(fileno(f)); fclose(f);
        remove(argv[2]);
#endif
        return 0;
    }
    if (strcmp(mode, "envsorted") == 0) {
#if defined(_WIN32)
        wchar_t *blk = GetEnvironmentStringsW(), *p, *prev = NULL;
        for (p = blk; p && *p; p += wcslen(p) + 1) {
            if (_wcsnicmp(p, L"WINE", 4) == 0) continue;     /* Wine adds its own, unsorted */
            if (prev && p[0] != L'=' && prev[0] != L'=') {   /* the per-drive "=C:" entries lead */
                const wchar_t *a = wcschr(prev + 1, L'='), *b = wcschr(p + 1, L'=');
                int la = a ? (int)(a - prev) : (int)wcslen(prev), lb = b ? (int)(b - p) : (int)wcslen(p);
                if (CompareStringOrdinal(prev, la, p, lb, TRUE) == CSTR_GREATER_THAN) {
                    fputs("unsorted at ", stdout); put_w(p); fputs(" after ", stdout); put_w(prev); fflush(stdout);
                    return 0;
                }
            }
            prev = p;
        }
        if (blk) FreeEnvironmentStringsW(blk);
#endif
        fputs("sorted", stdout);
        fflush(stdout);
        return 0;
    }
    if (strcmp(mode, "pidsleep") == 0) {
        FILE *f = (argc > 2) ? fopen(argv[2], "w") : NULL;
        if (!f) return 4;
#if defined(_WIN32)
        fprintf(f, "%lu", (unsigned long)GetCurrentProcessId());
#else
        fprintf(f, "%ld", (long)getpid());
#endif
        fclose(f);
        for (;;) sleep_ms(10);
    }
    if (strcmp(mode, "spawn") == 0 && argc > 2) {
#if defined(_WIN32)
        /* the worker shares this console and process group (no flags): a Ctrl+Break
         * to the group reaches both, and this launcher's default handler ends it */
        char cmd[2048];
        int i;
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        snprintf(cmd, sizeof cmd, "\"%s\"", argv[0]);
        for (i = 2; i < argc; ++i) {
            size_t L = strlen(cmd);
            snprintf(cmd + L, sizeof cmd - L, " \"%s\"", argv[i]);
        }
        memset(&si, 0, sizeof si); si.cb = sizeof si;
        memset(&pi, 0, sizeof pi);
        if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 7;
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
#else
        /* the worker stays in this process group: SIGTERM to the group reaches both,
         * and this launcher has SIGTERM's default action */
        pid_t c = fork();
        if (c < 0) return 7;
        if (c == 0) { argv[1] = argv[0]; execv(argv[0], argv + 1); _exit(127); }
#endif
        for (;;) sleep_ms(10);
    }
    if (strcmp(mode, "sig") == 0) {
#if !defined(_WIN32)
        raise(SIGTERM);
#endif
        return 6;
    }
    if (strcmp(mode, "exit") == 0) return (argc > 2) ? atoi(argv[2]) : 0;
    if (strcmp(mode, "hang") == 0) { for (;;) { /* killed by the deadline */ } }
    if (strcmp(mode, "eofsleep") == 0) {
        unsigned long ms = (argc > 2) ? strtoul(argv[2], NULL, 10) : 0;
        putchar('x'); fflush(stdout);
#if defined(_WIN32)
        _close(_fileno(stdout));
#else
        {
            int nul = open("/dev/null", O_WRONLY);
            if (nul < 0 || dup2(nul, STDOUT_FILENO) < 0) return 9;
            close(nul);
        }
#endif
        sleep_ms(ms);
        fputs("late\n", stderr); fflush(stderr);
        return (argc > 3) ? atoi(argv[3]) : 0;
    }
    if (strcmp(mode, "sigstate") == 0) {
        int term = 0, pipe_ign = 0;
#if !defined(_WIN32)
        sigset_t cur;
        struct sigaction sa;
        sigemptyset(&cur);
        if (sigprocmask(SIG_BLOCK, NULL, &cur) == 0) term = sigismember(&cur, SIGTERM) == 1;
        if (sigaction(SIGPIPE, NULL, &sa) == 0) pipe_ign = sa.sa_handler == SIG_IGN;
#endif
        printf("term=%d pipe=%d", term, pipe_ign);
        fflush(stdout);
        return 0;
    }
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
