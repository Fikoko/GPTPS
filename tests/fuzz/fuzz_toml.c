/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * fuzz_toml.c - the TOML-subset parser (src/config_toml.c) under a fuzzer.
 *
 * The input is a config file. It is parsed, and what parses is read back through
 * every accessor and held to what the loader and gptps_settings_save assume of it:
 *   - a key is set once: every entry's dotted key finds that entry and no other;
 *   - entries and [table] lines come in file order, one to a line, on lines the file
 *     has - save maps them back to the text by line number;
 *   - each value reads back through the accessor its kind names, as the same value;
 *   - a refusal says why, and an error buffer of any size is never overrun.
 */
#include "gptps.h"
#include "gptps_internal.h"   /* the parser */
#include "fuzz_common.h"
#include <errno.h>

/* The parse again, into an error buffer of exactly `cap` bytes on the heap, so a
 * write past it is caught; and with none at all. Both must agree with the first. */
static void parse_small_errbuf(const char *text, size_t cap, int parsed)
{
    char *err = cap ? (char *)malloc(cap) : NULL;
    gptps_toml *t;
    if (cap && !err) return;
    t = gptps_toml_parse_text("fuzz.toml", text, err, cap);
    FZ_ASSERT((t != NULL) == parsed, "the size of the error buffer changes whether a file parses");
    if (cap) FZ_ASSERT(memchr(err, 0, cap) != NULL, "the error message is not terminated within its buffer");
    gptps_toml_free(t);
    free(err);
    t = gptps_toml_parse_text("fuzz.toml", text, NULL, 0);
    FZ_ASSERT((t != NULL) == parsed, "parsing without an error buffer changes whether a file parses");
    gptps_toml_free(t);
}

static void check_value(const gptps_toml *t, size_t i)
{
    const char *sec = gptps_toml_section_at(t, i), *key = gptps_toml_key_at(t, i);
    const char *text = gptps_toml_text_at(t, i);
    long long ll = 0;
    double d = 0;
    int b = -1, n, k;
    const char *const *arr = NULL;
    switch (gptps_toml_kind_at(t, i)) {
        case GPTPS_TOML_INT:
            FZ_ASSERT(text && gptps_settings_plain_number(text, 1), "an integer's text is a whole number");
            FZ_ASSERT(gptps_toml_double(t, sec, key, &d), "an integer reads as a double");
            if (gptps_toml_int(t, sec, key, &ll)) {
                errno = 0;
                FZ_ASSERT(strtoll(text, NULL, 10) == ll && errno == 0, "an integer reads back as its text");
            } else {
                /* only an integer past LLONG_MAX - its text holds it, unsigned */
                errno = 0;
                (void)strtoll(text, NULL, 10);
                FZ_ASSERT(errno == ERANGE && text[0] != '-', "gptps_toml_int refuses an integer it holds");
            }
            break;
        case GPTPS_TOML_FLOAT:
            FZ_ASSERT(text && gptps_settings_plain_number(text, 0), "a float's text is a plain number");
            FZ_ASSERT(gptps_toml_double(t, sec, key, &d), "a float reads back");
            FZ_ASSERT(d == d && d <= 1.7976931348623157e308 && d >= -1.7976931348623157e308, "a float is finite");
            FZ_ASSERT(!gptps_toml_int(t, sec, key, &ll), "a float reads as an integer");
            break;
        case GPTPS_TOML_BOOL:
            FZ_ASSERT(text && (!strcmp(text, "true") || !strcmp(text, "false")), "a bool's text is true or false");
            FZ_ASSERT(gptps_toml_bool(t, sec, key, &b) && b == (text[0] == 't'), "a bool reads back");
            break;
        case GPTPS_TOML_STRING:
            FZ_ASSERT(text != NULL && gptps_toml_str(t, sec, key) == text, "a string reads back");
            FZ_ASSERT(!gptps_toml_bool(t, sec, key, &b) && !gptps_toml_double(t, sec, key, &d),
                      "a string reads as another kind");
            break;
        case GPTPS_TOML_ARRAY:
            FZ_ASSERT(text == NULL, "an array has no scalar text");
            n = gptps_toml_str_array(t, sec, key, &arr);
            FZ_ASSERT(n >= 0 && (n == 0 || arr != NULL), "an array reads back");
            for (k = 0; k < n; ++k) FZ_ASSERT(arr[k] != NULL, "an array element is missing");
            FZ_ASSERT(gptps_toml_str(t, sec, key) == NULL, "an array reads as a string");
            break;
    }
}

static void check_entries(gptps_toml *t, const char *text)
{
    size_t n = gptps_toml_count(t), nt = gptps_toml_table_count(t), i, j;
    int lines = 1, last = 0;
    const char *c;
    for (c = text; *c; ++c) if (*c == '\n') ++lines;
    FZ_ASSERT(!strcmp(gptps_toml_path(t), "fuzz.toml"), "the parse keeps the file's name");

    for (i = 0; i < n; ++i) {
        const char *sec = gptps_toml_section_at(t, i), *key = gptps_toml_key_at(t, i);
        size_t sl, kl, cap;
        char *dotted, small[8];
        int line = gptps_toml_line_at(t, i);
        FZ_ASSERT(sec != NULL && key != NULL, "an entry has a section and a key");
        FZ_ASSERT(line > last && line <= lines, "entries are in file order, one to a line, on lines the file has");
        last = line;
        FZ_ASSERT(!gptps_toml_claimed_at(t, i), "an entry starts claimed");
        gptps_toml_claim_at(t, i);
        FZ_ASSERT(gptps_toml_claimed_at(t, i), "a claim does not hold");

        sl = strlen(sec); kl = strlen(key);
        cap = sl + kl + 2;
        dotted = (char *)malloc(cap);
        if (!dotted) continue;
        gptps_toml_dotted_at(t, i, dotted, cap);
        FZ_ASSERT(strlen(dotted) == (sl ? sl + 1 + kl : kl), "the dotted key is the section and the key");
        /* a key is set once, however it is spelt: [a.b] c and [a] "b.c" are one key */
        FZ_ASSERT(gptps_toml_find_dotted(t, dotted) == (long)i,
                  "a dotted key finds another entry: the same key is set twice");
        for (j = 1; j <= sizeof small; ++j) {          /* a buffer too small is cut, not overrun */
            gptps_toml_dotted_at(t, i, small, j);
            FZ_ASSERT(strlen(small) < j && !strncmp(small, dotted, strlen(small)), "a short buffer is not a prefix");
        }
        free(dotted);
        check_value(t, i);
    }

    /* [table] lines: in order, on lines of their own, named */
    last = 0;
    for (i = 0, j = 0; i < nt; ++i) {
        int line = gptps_toml_table_line_at(t, i);
        const char *name = gptps_toml_table_at(t, i);
        FZ_ASSERT(line > last && line <= lines, "tables are in file order, on lines the file has");
        FZ_ASSERT(name && *name && strlen(name) < 256, "a table has a name, and the parser's bound holds");
        last = line;
        while (j < n && gptps_toml_line_at(t, j) < line) ++j;
        FZ_ASSERT(j >= n || gptps_toml_line_at(t, j) != line, "a table and a key share a line");
    }

    /* what is not there reads as not there: no file has a table name of 256 bytes */
    {
        long long ll; double d; int b; const char *const *arr;
        char none[300], dotted[310];
        memset(none, 'n', sizeof none - 1);
        none[sizeof none - 1] = 0;
        snprintf(dotted, sizeof dotted, "%s.x", none);
        FZ_ASSERT(gptps_toml_find_dotted(t, dotted) < 0 || gptps_toml_section_at(t, (size_t)gptps_toml_find_dotted(t, dotted))[0] == 0,
                  "a key in a table no file can name is found");
        FZ_ASSERT(!gptps_toml_int(t, none, "x", &ll) && !gptps_toml_double(t, none, "x", &d) &&
                  !gptps_toml_bool(t, none, "x", &b) && !gptps_toml_str(t, none, "x") &&
                  !gptps_toml_str_array(t, none, "x", &arr), "a key in a table no file can name reads");
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char *text, err[4096];
    gptps_toml *t;
    uint32_t h;
    if (size > FZ_MAX_INPUT) return 0;
    text = (char *)malloc(size + 1);
    if (!text) return 0;
    if (size) memcpy(text, data, size);
    text[size] = 0;                     /* a NUL inside ends the text: gptps_toml_read_file refuses those */
    err[0] = 0;
    t = gptps_toml_parse_text("fuzz.toml", text, err, sizeof err);
    if (t) {
        check_entries(t, text);
        gptps_toml_free(t);
    } else {
        FZ_ASSERT(err[0] != 0, "a refusal says why");
        FZ_ASSERT(memchr(err, 0, sizeof err) != NULL, "the error message is not terminated");
    }
    /* again into an error buffer of an input-chosen size, 0 to 700 bytes */
    h = fz_hash(data, size);
    parse_small_errbuf(text, h % 701u, t != NULL);
    free(text);
    return 0;
}
