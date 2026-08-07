/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_fuzz.c - robustness of the two hand-rolled parsers that consume
 * untrusted file bytes: the TOML-subset config parser and the durable-queue
 * journal reader. Throws crafted-malformed and pseudo-random input at both and
 * asserts they never crash, overflow, or leak. The point of this test is to be
 * run under AddressSanitizer/UBSan (the `asan` CI job); a clean exit there is
 * the pass. Deterministic (fixed-seed LCG) so failures reproduce.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "gptps.h"
#include "gptps_internal.h"   /* internal TOML parser */
#include "gptps_durable_queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

/* deterministic PRNG (no rand(); reproducible) */
static uint32_t g_state = 0x9e3779b9u;
static uint32_t lcg(void) { g_state = g_state * 1103515245u + 12345u; return g_state; }

static void write_file(const char *path, const void *bytes, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (f) { if (n) fwrite(bytes, 1, n, f); fclose(f); }
}

#define FZ_TOML "fuzz_test.toml"
#define FZ_JRNL "fuzz_test.journal"

/* ---- TOML parser ---- */
static void parse_toml_bytes(const void *b, size_t n)
{
    gptps_toml *t;
    long long ll; double d; int bo; const char *const *arr;
    write_file(FZ_TOML, b, n);
    t = gptps_toml_parse_file(FZ_TOML, NULL, 0);
    if (!t) return;
    /* exercise every query path against whatever was parsed */
    (void)gptps_toml_int(t, "limits", "max_concurrent_tasks", &ll);
    (void)gptps_toml_double(t, "limits", "max_memory_gb", &d);
    (void)gptps_toml_bool(t, "misc", "verbose", &bo);
    (void)gptps_toml_str(t, "tasks.x", "on_failure");
    (void)gptps_toml_str_array(t, "", "addons", &arr);
    gptps_toml_free(t);
}

static void fuzz_toml(void)
{
    static const char *crafted[] = {
        "", "\n\n\n", "# only a comment\n",
        "no_equals_here", "key =", "= value", "key = ", "  key  =  ",
        "[unterminated", "[]", "[a.b.c.d.e]\nk=1",
        "s = \"unterminated", "s = \"esc \\\" \\\\ \\n \\t end\"",
        "a = [\"x\", \"y\"", "a = [", "a = ]", "a = [,,,]",
        "n = 999999999999999999999999999999", "f = 1.2.3.4", "f = .e.",
        "b = tru", "b = TRUE",
        "[tasks.\xff\xfe]\npriority = -2147483648\n",
        "k = \"\"\nj = []\n[s]\n",
        "addons = [\"a\",\"b\",\"c\",\"d\",\"e\"]\n",
        "key.with.dots = 1\n[a]\n[a]\nk=1\nk=2\n",
        "###\n[#]\n#=#\n\"#\" = \"#\"\n",
    };
    size_t i;
    for (i = 0; i < sizeof crafted / sizeof crafted[0]; ++i)
        parse_toml_bytes(crafted[i], strlen(crafted[i]));

    /* random bytes, biased toward TOML-structural characters for branch reach */
    for (i = 0; i < 400; ++i) {
        unsigned char buf[400];
        size_t n = lcg() % sizeof buf, j;
        for (j = 0; j < n; ++j) {
            uint32_t r = lcg();
            if (r & 3) { static const char s[] = "[]=\"#.\n\t abcXY012_,-"; buf[j] = (unsigned char)s[r % (sizeof s - 1)]; }
            else buf[j] = (unsigned char)(r >> 8);
        }
        parse_toml_bytes(buf, n);
    }
    remove(FZ_TOML);
}

/* ---- durable-queue journal ---- */
static void open_journal_bytes(const void *b, size_t n)
{
    gptps *e = NULL;
    gptps_dq *dq;
    write_file(FZ_JRNL, b, n);
    if (gptps_open(NULL, &e) != GPTPS_OK || !e) return;
    dq = gptps_dq_open(e, FZ_JRNL);   /* must reject a bad header / stop at torn records, never crash */
    if (dq) {
        (void)gptps_dq_recover(dq);
        (void)gptps_dq_pending(dq);
        (void)gptps_dq_compact(dq);
    }
    gptps_shutdown(e);
    if (dq) gptps_dq_close(dq);        /* after shutdown */
}

static void fuzz_journal(void)
{
    /* a valid 8-byte file header (magic "GDQ1" + version 1, little-endian) */
    static const unsigned char H[8] = { 0x31, 0x51, 0x44, 0x47, 0x01, 0x00, 0x00, 0x00 };
    unsigned char buf[512];
    size_t i;

    open_journal_bytes(NULL, 0);                 /* empty file */
    open_journal_bytes("garbage!", 8);           /* bad file magic */
    open_journal_bytes(H, 8);                    /* header only, no records */

    /* header + a record magic then a truncated/oversized header */
    memcpy(buf, H, 8);
    buf[8] = 0x31; buf[9] = 0x52; buf[10] = 0x51; buf[11] = 0x44; /* "DQR1" rec magic */
    memset(buf + 12, 0xFF, 40);                  /* implausible lengths => torn */
    open_journal_bytes(buf, 52);

    /* header + random record bytes */
    for (i = 0; i < 200; ++i) {
        size_t n, j;
        memcpy(buf, H, 8);
        n = 8 + (lcg() % (sizeof buf - 8));
        for (j = 8; j < n; ++j) {
            uint32_t r = lcg();
            /* sprinkle real record magic bytes to push past the magic check */
            if ((r & 7) == 0) buf[j] = (unsigned char)"\x31\x52\x51\x44"[r % 4];
            else buf[j] = (unsigned char)(r >> 5);
        }
        open_journal_bytes(buf, n);
    }
    remove(FZ_JRNL);
}

int main(void)
{
    fuzz_toml();
    fuzz_journal();
    if (fails) { printf("%d fuzz check(s) FAILED\n", fails); return 1; }
    printf("all fuzz checks passed (run under ASan/UBSan for full value)\n");
    return 0;
}
