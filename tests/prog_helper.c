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
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#  include <io.h>
#  include <fcntl.h>
#else
#  include <unistd.h>
#endif

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
