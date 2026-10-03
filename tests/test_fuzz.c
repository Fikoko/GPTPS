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

/* ---- settings save, editing a file in place ----
 * gptps_settings_save rewrites hand-written text, so it gets hand-written text of
 * every shape the parser accepts. The property: a file that parses still parses
 * after a save, and holds the values set live; nothing else in it may change
 * meaning. Random files are built from line templates - tables, dotted and quoted
 * keys, comments, CRLF, strings with # in them - so most of them parse. */
static unsigned g_saved;   /* files that parsed, so the property was checked */

static gptps_status fz_noop(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }

static void save_into(gptps *e, const char *text)
{
    static const struct { const char *key, *want; } LIVE[] = {
        { "scheduler.reserve_after_skips", "5" },
        { "limits.max_intake_depth",       "7" },
        { "tasks.fz.on_failure",           "drop" },
        { "tasks.fz.priority",             "-3" },
        { "tasks.odd]x.max_retries",       "2" },
        { "tasks.sp ace.timeout_seconds",  "9" },
        { "tasks.e=q.max_retries",         "6" },
    };
    gptps_toml *before, *after;
    char err[4096];
    size_t i, k, n;
    write_file(FZ_TOML, text, strlen(text));
    before = gptps_toml_parse_file(FZ_TOML, NULL, 0);
    if (!before) return;                            /* save refuses those: test_config_strict */
    ++g_saved;
    CHECK(gptps_settings_save(e, FZ_TOML) == GPTPS_OK);
    after = gptps_toml_parse_file(FZ_TOML, err, sizeof err);
    CHECK(after != NULL);
    if (!after) {
        printf("  after a save of:\n---\n%s\n---\n  %s\n  the file became:\n---\n", text, err);
        { FILE *f = fopen(FZ_TOML, "rb"); int c; if (f) { while ((c = fgetc(f)) != EOF) putchar(c); fclose(f); } }
        printf("---\n");
        gptps_toml_free(before);
        return;
    }
    for (i = 0; i < sizeof LIVE / sizeof LIVE[0]; ++i) {
        long j = gptps_toml_find_dotted(after, LIVE[i].key);
        const char *v = j >= 0 ? gptps_toml_text_at(after, (size_t)j) : NULL;
        CHECK(v != NULL && strcmp(v, LIVE[i].want) == 0);
    }
    /* every other key the file had is still there, with the same value */
    n = gptps_toml_count(before);
    for (k = 0; k < n; ++k) {
        char d[512];
        const char *was = gptps_toml_text_at(before, k), *now;
        long j;
        int live = 0;
        gptps_toml_dotted_at(before, k, d, sizeof d);
        for (i = 0; i < sizeof LIVE / sizeof LIVE[0]; ++i) if (!strcmp(d, LIVE[i].key)) live = 1;
        if (live) continue;
        j = gptps_toml_find_dotted(after, d);
        CHECK(j >= 0);
        if (j < 0) { printf("  lost %s from:\n%s\n", d, text); continue; }
        now = gptps_toml_text_at(after, (size_t)j);
        CHECK((was == NULL) == (now == NULL) && (!was || strcmp(was, now) == 0));
    }
    gptps_toml_free(before);
    gptps_toml_free(after);
}

static void fuzz_save(void)
{
    static const char *const LINES[] = {
        "", "\r", "# a comment", "   # indented comment", "[scheduler]", "[ limits ]  # spaced",
        "[tasks.fz]", "[tasks]", "[tasks.fz.resources]", "[app]", "[app.sub]",
        "reserve_after_skips = 1", "max_intake_depth = 0   # note", "on_failure = \"requeue\"",
        "priority = 4\r", "\"fz.priority\" = 1", "\"odd]x.max_retries\" = 0", "k%u = 1",
        "s%u = \"a # not a comment\"  # but this is", "a%u = [\"x\", \"y # z\"]", "b%u = true",
        "q%u.r = 2.5", "\"quoted %u\" = \"v\"", "  indented%u   =   3   ",
        "[tasks.\"sp ace\"]", "[tasks.\"odd]x\"]  # a ] in a quoted part", "[ tasks . fz ]",
        "timeout_seconds = 1", "max_retries = 1", "m%u.\"x = y\" = 1", "\"h#%u\".z = \"w # w\"",
        "\"e=q\".max_retries = 3  # = in a quoted part", "[tasks.\"e=q\"]",
    };
    static const char *crafted[] = {
        "", "\n", "# only a comment\n", "[scheduler]\n", "[scheduler]\nreserve_after_skips = 8 # was\n",
        "x = 1", "[tasks.fz]\npriority = 1\n[tasks.fz.resources]\n", "\"scheduler.reserve_after_skips\" = 1\n",
        "[a]\n[b]\n[c]\n", "top = 1\n\n# header note\n[limits]\n",
    };
    gptps *e = NULL;
    gptps_task_def d;
    size_t i;
    if (gptps_open(NULL, &e) != GPTPS_OK) { CHECK(0); return; }
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.exec = GPTPS_EXEC_INPROC; d.run = fz_noop;
    {
        static const char *const names[] = { "fz", "odd]x", "sp ace", "e=q" };
        for (i = 0; i < 4; ++i) {
            d.name = names[i];
            CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        }
    }
    CHECK(gptps_settings_set(e, "scheduler.reserve_after_skips", "5") == GPTPS_OK);
    CHECK(gptps_settings_set(e, "limits.max_intake_depth", "7") == GPTPS_OK);
    CHECK(gptps_settings_set(e, "tasks.fz.on_failure", "drop") == GPTPS_OK);
    CHECK(gptps_settings_set(e, "tasks.fz.priority", "-3") == GPTPS_OK);
    CHECK(gptps_settings_set(e, "tasks.odd]x.max_retries", "2") == GPTPS_OK);
    CHECK(gptps_settings_set(e, "tasks.sp ace.timeout_seconds", "9") == GPTPS_OK);
    CHECK(gptps_settings_set(e, "tasks.e=q.max_retries", "6") == GPTPS_OK);
    for (i = 0; i < sizeof crafted / sizeof crafted[0]; ++i) save_into(e, crafted[i]);
    for (i = 0; i < 600; ++i) {
        char text[4096];
        size_t k = 0, lines = lcg() % 14, m;
        for (m = 0; m < lines && k + 200 < sizeof text; ++m) {
            const char *tpl = LINES[lcg() % (sizeof LINES / sizeof LINES[0])];
            k += (size_t)snprintf(text + k, sizeof text - k, tpl, (unsigned)(lcg() % 1000));
            if ((lcg() & 7) || m + 1 < lines) text[k++] = '\n';   /* sometimes no final newline */
            text[k] = 0;
        }
        text[k] = 0;
        save_into(e, text);
    }
    /* most generated files parse; if a change to the generator stopped that, the
     * property above would pass by checking nothing */
    CHECK(g_saved >= 300);
    printf("save: %u files edited in place and re-read\n", g_saved);
    gptps_shutdown(e);
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
    fuzz_save();
    fuzz_journal();
    if (fails) { printf("%d fuzz check(s) FAILED\n", fails); return 1; }
    printf("all fuzz checks passed (run under ASan/UBSan for full value)\n");
    return 0;
}
