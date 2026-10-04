/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * replay.c - the regression corpus, replayed: a main() for a fuzz harness that runs
 * each input once, with no fuzzing. Every file named on the command line, and every
 * file in every directory named, goes through LLVMFuzzerTestOneInput in name order.
 *
 * This is how the inputs the fuzzers kept - the ones that reached new code, and the
 * ones that once crashed - stay in the normal test suite on every platform, so a
 * fixed bug stays fixed. A harness that finds a broken promise aborts, and the last
 * "replay" line printed names the input. Arguments starting with '-' are skipped,
 * so a libFuzzer command line replays too.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "fuzz_common.h"
#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#endif

static unsigned long g_runs;

static void replay_file(const char *path)
{
    FILE *f;
    unsigned char *buf = NULL;
    size_t len = 0, cap = 0, got;
    printf("replay %s\n", path);
    fflush(stdout);
    f = fopen(path, "rb");
    if (!f) { printf("FAIL: cannot open %s\n", path); exit(1); }
    for (;;) {
        if (len == cap) {
            unsigned char *grown;
            cap = cap ? cap * 2 : 4096;
            grown = (unsigned char *)realloc(buf, cap);
            if (!grown) { printf("FAIL: out of memory reading %s\n", path); exit(1); }
            buf = grown;
        }
        got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (got == 0) break;
    }
    fclose(f);
    (void)LLVMFuzzerTestOneInput(buf, len);
    free(buf);
    ++g_runs;
}

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void add_name(char ***names, size_t *n, size_t *cap, const char *name)
{
    if (*n == *cap) {
        char **grown;
        *cap = *cap ? *cap * 2 : 64;
        grown = (char **)realloc(*names, *cap * sizeof **names);
        if (!grown) { printf("FAIL: out of memory listing a directory\n"); exit(1); }
        *names = grown;
    }
    (*names)[*n] = (char *)malloc(strlen(name) + 1);
    if (!(*names)[*n]) { printf("FAIL: out of memory listing a directory\n"); exit(1); }
    strcpy((*names)[(*n)++], name);
}

/* The directory's regular files, in name order so a run is the same everywhere.
 * Returns 0 when `path` is not a directory. */
static int replay_dir(const char *path)
{
    char **names = NULL, full[1024];
    size_t n = 0, cap = 0, i;
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE h;
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    snprintf(full, sizeof full, "%s\\*", path);
    h = FindFirstFileA(full, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
            add_name(&names, &n, &cap, fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    struct stat st;
    DIR *d;
    struct dirent *de;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
    d = opendir(path);
    if (!d) return 0;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        snprintf(full, sizeof full, "%s/%s", path, de->d_name);
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        add_name(&names, &n, &cap, de->d_name);
    }
    closedir(d);
#endif
    if (n) qsort(names, n, sizeof *names, cmp_names);
    for (i = 0; i < n; ++i) {
        snprintf(full, sizeof full, "%s/%s", path, names[i]);
        replay_file(full);
        free(names[i]);
    }
    free(names);
    return 1;
}

int main(int argc, char **argv)
{
    int i;
#if defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);   /* a failure must fail, not wait on a dialog */
#endif
    for (i = 1; i < argc; ++i) {
        if (argv[i][0] == '-') continue;
        if (!replay_dir(argv[i])) replay_file(argv[i]);
    }
    if (g_runs == 0) { printf("FAIL: no inputs to replay\n"); return 1; }
    printf("all fuzz corpus checks passed (%lu inputs)\n", g_runs);
    return 0;
}
