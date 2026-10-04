/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * fuzz_common.h - what the fuzz harnesses in tests/fuzz share.
 *
 * Each harness is one LLVMFuzzerTestOneInput, the libFuzzer entry point, so the
 * same file runs under three drivers: tests/fuzz/driver.c (coverage-guided, GCC or
 * Clang), libFuzzer itself (Clang), and tests/fuzz/replay.c, which feeds it the
 * regression corpus once, file by file, in the normal test suite.
 *
 * A harness that finds a broken promise - not a refused input, which is the
 * parsers' job, but a crash, a leak or a property that does not hold - says what
 * it found and aborts, which every driver reports as a crash and saves the input.
 */
#ifndef GPTPS_FUZZ_COMMON_H
#define GPTPS_FUZZ_COMMON_H

#include "gptps.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#  include <process.h>
#  define fz_getpid() ((long)_getpid())
#else
#  include <unistd.h>
#  define fz_getpid() ((long)getpid())
#endif

#if defined(__GNUC__)
#  define FZ_UNUSED __attribute__((unused))
#else
#  define FZ_UNUSED
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Inputs past this are skipped: a config file is kilobytes, a journal record that
 * matters is too, and a fuzzer that grows inputs without bound only gets slower. */
#define FZ_MAX_INPUT (64u * 1024u)

/* A broken promise: print it and abort, so the driver keeps the input. */
#define FZ_ASSERT(c, what) do { if (!(c)) fz_fail(__FILE__, __LINE__, #c, (what)); } while (0)

static FZ_UNUSED void fz_fail(const char *file, int line, const char *cond, const char *what)
{
    fprintf(stderr, "\nFUZZ PROPERTY FAILED %s:%d: %s\n  (%s)\n", file, line, what, cond);
    fflush(stderr);
    abort();
}

/* A path for this harness's scratch file `name`, in GPTPS_FUZZ_TMPDIR when it is set
 * (a tmpfs makes the journal's fsyncs free), else in the current directory. The
 * process id keeps two fuzzers sharing a directory apart. */
static FZ_UNUSED void fz_path(char *buf, size_t cap, const char *name)
{
    const char *dir = getenv("GPTPS_FUZZ_TMPDIR");
    if (dir && *dir) snprintf(buf, cap, "%s/gptps_fz_%ld_%s", dir, fz_getpid(), name);
    else             snprintf(buf, cap, "gptps_fz_%ld_%s", fz_getpid(), name);
}

static FZ_UNUSED int fz_write(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f) return 0;
    ok = (n == 0 || fwrite(data, 1, n, f) == n);
    if (fclose(f) != 0) ok = 0;
    return ok;
}

/* The whole file, NUL-terminated (free it); NULL if it cannot be read. */
static FZ_UNUSED char *fz_slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL, *grown;
    size_t len = 0, cap = 0, got;
    if (!f) return NULL;
    for (;;) {
        if (cap - len < 4096) {
            cap = cap ? cap * 2 : 8192;
            grown = (char *)realloc(buf, cap + 1);
            if (!grown) { free(buf); fclose(f); return NULL; }
            buf = grown;
        }
        got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (got == 0) break;
    }
    fclose(f);
    buf[len] = 0;
    if (out_len) *out_len = len;
    return buf;
}

/* FNV-1a over the input: a stable, input-derived choice where a harness needs one. */
static FZ_UNUSED uint32_t fz_hash(const uint8_t *d, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--) { h ^= *d++; h *= 16777619u; }
    return h;
}

/* The engine's log, kept quiet: a fuzzer runs millions of inputs, and most of them
 * are refused with a message. */
static FZ_UNUSED void fz_quiet_sink(gptps_log_level lvl, const char *msg, void *ud)
{
    (void)lvl; (void)msg; (void)ud;
}

static FZ_UNUSED gptps_status fz_noop_run(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_OK; }

/* Task names that need every kind of quoting a save can write: a ] or = or # in a
 * table name, a space, a dot, a quote, a backslash, control characters, UTF-8 - and
 * none at all, or a lone dot, which make empty parts in a dotted key. */
static FZ_UNUSED const char *const FZ_ODD_TASKS[] = {
    "fz", "odd]x", "sp ace", "e=q", "a.b", "q\"t", "b\\s", "h#x", "t\tb", "n\nl", "u\xc3\xa9", "d\x7f",
    "", ".",
};
#define FZ_N_ODD_TASKS (sizeof FZ_ODD_TASKS / sizeof FZ_ODD_TASKS[0])

static FZ_UNUSED gptps_status fz_register(gptps *e, const char *name)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.name = name;
    d.exec = GPTPS_EXEC_INPROC;
    d.run = fz_noop_run;
    return gptps_register_task(e, &d);
}

#if defined(GPTPS_INTERNAL_H)
/* The task names a parsed file configures - the part after "tasks." in a [table] or
 * a key - up to `max` of them: registered, they make the file's per-task keys into
 * settings, so a save writes under names the fuzzer chose. */
#define FZ_MAX_FILE_TASKS 8
static FZ_UNUSED size_t fz_file_task_names(const gptps_toml *t, char names[][GPTPS_TASK_NAME_MAX + 1], size_t max)
{
    size_t n = 0, i, k, total = gptps_toml_table_count(t) + gptps_toml_count(t);
    for (i = 0; i < total && n < max; ++i) {
        char d[GPTPS_TASK_NAME_MAX + 8];
        const char *s, *dot;
        size_t len;
        if (i < gptps_toml_table_count(t)) s = gptps_toml_table_at(t, i);
        else { gptps_toml_dotted_at(t, i - gptps_toml_table_count(t), d, sizeof d); s = d; }
        if (strncmp(s, "tasks.", 6) != 0) continue;
        s += 6;
        dot = strchr(s, '.');
        len = dot ? (size_t)(dot - s) : strlen(s);
        if (len == 0 || len > GPTPS_TASK_NAME_MAX) continue;
        for (k = 0; k < n; ++k) if (strlen(names[k]) == len && !memcmp(names[k], s, len)) break;
        if (k < n) continue;
        memcpy(names[n], s, len);
        names[n][len] = 0;
        ++n;
    }
    return n;
}
#endif

#endif /* GPTPS_FUZZ_COMMON_H */
