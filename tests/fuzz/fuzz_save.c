/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * fuzz_save.c - gptps_settings_save editing a file in place, under a fuzzer.
 *
 * The property: a file that parses still parses after a save, and keeps every value
 * it did not change. For an input that parses:
 *   1. an engine opens on it - or, when the file holds values the engine does not
 *      take or names add-ons, opens without it: a save edits any file that parses;
 *   2. tasks are registered - with names that need every kind of quoting, and the
 *      names the file's own [tasks.*] tables use - and settings are set live, a
 *      different selection for each input;
 *   3. the save, and the file parsed again, held to:
 *      - every value set live is in the file, as the setting renders it;
 *      - every other key has the kind and value it had, and no key was added but
 *        the ones set live;
 *      - every line that held no changed value is still there, in order;
 *      - a second save changes nothing;
 *      - when the engine opened the file before the save, a new one opens it after,
 *        and every setting reads the same in both.
 */
#include "gptps.h"
#include "gptps_internal.h"   /* the parser */
#include "fuzz_common.h"

/* What may be set live. The value is fixed; whether it is set comes from the input. */
static const struct { const char *key, *value; } LIVE[] = {
    { "scheduler.reserve_after_skips", "5" },
    { "limits.max_intake_depth",       "7" },
    { "limits.shutdown_grace_ms",      "250" },
    { "limits.max_dead_letters",       "0" },
    { "limits.max_concurrent_tasks",   "3" },
    { "bounded.max_payload_bytes",     "64" },
    { "tasks.fz.on_failure",           "drop" },
    { "tasks.fz.priority",             "-3" },
    { "tasks.fz.weight",               "42" },
    { "tasks.odd]x.max_retries",       "2" },
    { "tasks.sp ace.timeout_seconds",  "9" },
    { "tasks.e=q.max_retries",         "6" },
    { "tasks.a.b.priority",            "4" },
    { "tasks.q\"t.mem_bytes",          "4096" },
    { "tasks.b\\s.retry_backoff_seconds", "1" },
    { "tasks.h#x.on_failure",          "requeue" },
    { "tasks.t\tb.priority",           "1" },
    { "tasks.n\nl.weight",             "3" },
    { "tasks.u\xc3\xa9.priority",      "-1" },
    { "tasks.d\x7f.max_retries",       "8" },
    { "tasks..priority",               "6" },
    { "tasks...on_failure",            "drop" },
    { "app.name",                      "a \"quoted\" # value\twith\\escapes\n\x01 and \xc3\xa9" },
    { "app.level",                     "-4" },
    { "app.ratio",                     "0.25" },
    { "app.on",                        "true" },
    { "app.mode",                      "slow" },
    { "top",                           "5" },
    { "app.huge",                      "1e30" },
};
#define N_LIVE (sizeof LIVE / sizeof LIVE[0])
/* max_memory_bytes, in the three ways a save writes it: exact in GiB, exact in GiB
 * but not whole, and past 2^53, where only bytes say it exactly */
static const char *const MEMS[] = { "2147483648", "1234567", "18446744073709551615" };

#define MAX_SET (N_LIVE + FZ_MAX_FILE_TASKS + 1)
typedef struct {
    char key[GPTPS_TASK_NAME_MAX + 64];
    char want[GPTPS_SETTINGS_VALUE_MAX];   /* as the setting renders it after the set */
} live_set;

static live_set g_set[MAX_SET];
static size_t   g_nset;

static void define_settings(gptps *e)
{
    (void)gptps_define_global(e, "app.name", GPTPS_SETTING_STRING, "x", NULL, 0);
    (void)gptps_define_global(e, "app.level", GPTPS_SETTING_INT, "1", "-5..9", 0);
    (void)gptps_define_global(e, "app.ratio", GPTPS_SETTING_DOUBLE, "0.5", "0..1", 0);
    (void)gptps_define_global(e, "app.on", GPTPS_SETTING_BOOL, "false", NULL, 0);
    (void)gptps_define_global(e, "app.mode", GPTPS_SETTING_ENUM, "fast", "fast|slow", 0);
    (void)gptps_define_global(e, "top", GPTPS_SETTING_UINT, "0", NULL, 0);
    (void)gptps_define_task_setting(e, "weight", GPTPS_SETTING_UINT, "1", "0..100", 0);
}

/* A host's own number setting that renders as digits however large it is (%.0f): a
 * value past 64 bits set live must be saved as the float it is, or the next read
 * would refuse it as too large an integer. One per engine, by slot. */
static double g_huge[2];
static size_t huge_rd(void *t, char *b, size_t c)
{
    int n = snprintf(b, c, "%.0f", *(const double *)t);
    return n < 0 ? 0 : (size_t)n;
}
static gptps_status huge_wr(void *t, const char *v) { *(double *)t = strtod(v, NULL); return GPTPS_OK; }

static void setup(gptps *e, char names[][GPTPS_TASK_NAME_MAX + 1], size_t nnames, int slot)
{
    gptps_setting_def d;
    size_t i;
    for (i = 0; i < FZ_N_ODD_TASKS; ++i) (void)fz_register(e, FZ_ODD_TASKS[i]);
    for (i = 0; i < nnames; ++i) (void)fz_register(e, names[i]);
    define_settings(e);
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.key = "app.huge"; d.type = GPTPS_SETTING_DOUBLE; d.desc = "a number as digits"; d.hot = 1;
    g_huge[slot] = 0;
    d.target = &g_huge[slot]; d.read = huge_rd; d.write = huge_wr;
    (void)gptps_register_setting(e, &d);
}

static void set_one(gptps *e, const char *key, const char *value)
{
    live_set *s;
    size_t i;
    if (g_nset == MAX_SET || strlen(key) >= sizeof g_set[0].key) return;
    if (gptps_settings_set(e, key, value) != GPTPS_OK) return;
    for (i = 0; i < g_nset && strcmp(g_set[i].key, key); ++i) { }   /* set twice: the last one counts */
    s = &g_set[i];
    snprintf(s->key, sizeof s->key, "%s", key);
    s->want[0] = 0;
    if (gptps_settings_get(e, key, s->want, sizeof s->want) != GPTPS_OK) return;
    if (i == g_nset) ++g_nset;
}

/* The live sets for this input: each of LIVE with odds of 3 in 4, max_memory_bytes
 * one way of three, and a priority for every task the file names. */
static void set_live(gptps *e, uint32_t h, char names[][GPTPS_TASK_NAME_MAX + 1], size_t nnames)
{
    size_t i;
    uint32_t r = h | 1u;
    g_nset = 0;
    for (i = 0; i < N_LIVE; ++i) {
        r = r * 1103515245u + 12345u;
        if ((r >> 16) & 3u) set_one(e, LIVE[i].key, LIVE[i].value);
    }
    r = r * 1103515245u + 12345u;
    if ((r >> 16) % 4u) set_one(e, "limits.max_memory_bytes", MEMS[(r >> 20) % 3u]);
    for (i = 0; i < nnames; ++i) {
        char key[GPTPS_TASK_NAME_MAX + 64];
        snprintf(key, sizeof key, "tasks.%.*s.priority", GPTPS_TASK_NAME_MAX, names[i]);
        set_one(e, key, "7");
    }
}

static const live_set *live_find(const char *key)
{
    size_t i;
    for (i = 0; i < g_nset; ++i) if (!strcmp(g_set[i].key, key)) return &g_set[i];
    return NULL;
}

static char *dotted_of(const gptps_toml *t, size_t i)
{
    size_t cap = strlen(gptps_toml_section_at(t, i)) + strlen(gptps_toml_key_at(t, i)) + 2;
    char *d = (char *)malloc(cap);
    if (d) gptps_toml_dotted_at(t, i, d, cap);
    return d;
}

static int same_value(const gptps_toml *a, size_t i, const gptps_toml *b, size_t j)
{
    const char *x = gptps_toml_text_at(a, i), *y = gptps_toml_text_at(b, j);
    const char *const *ax, *const *bx;
    int n, m, k;
    if (gptps_toml_kind_at(a, i) != gptps_toml_kind_at(b, j)) return 0;
    if (x || y) return x && y && !strcmp(x, y);
    n = gptps_toml_str_array(a, gptps_toml_section_at(a, i), gptps_toml_key_at(a, i), &ax);
    m = gptps_toml_str_array(b, gptps_toml_section_at(b, j), gptps_toml_key_at(b, j), &bx);
    if (n != m) return 0;
    for (k = 0; k < n; ++k) if (strcmp(ax[k], bx[k])) return 0;
    return 1;
}

/* Line `n` (1-based) of `text`: its start, and its length without the '\n'. */
static const char *line_of(const char *text, int n, size_t *len)
{
    const char *p = text, *nl;
    while (--n > 0) { nl = strchr(p, '\n'); if (!nl) return NULL; p = nl + 1; }
    nl = strchr(p, '\n');
    *len = nl ? (size_t)(nl - p) : strlen(p);
    return p;
}

static void check_saved(const gptps_toml *before, const char *text, const char *path)
{
    char *saved;
    size_t saved_len = 0, i, n;
    gptps_toml *after;
    int gb_alias = gptps_toml_find_dotted(before, "limits.max_memory_gb") >= 0 &&
                   gptps_toml_find_dotted(before, "limits.max_memory_bytes") < 0;
    const live_set *mem = live_find("limits.max_memory_bytes");
    char err[1024];

    saved = fz_slurp(path, &saved_len);
    FZ_ASSERT(saved != NULL, "the saved file cannot be read");
    after = gptps_toml_parse_text("saved.toml", saved, err, sizeof err);
    if (!after) fprintf(stderr, "the saved file:\n---\n%s---\n%s\n", saved, err);
    FZ_ASSERT(after != NULL, "a file that parsed does not parse after a save");

    /* every value set live is there, as the setting renders it */
    for (i = 0; i < g_nset; ++i) {
        long j = gptps_toml_find_dotted(after, g_set[i].key);
        const char *v;
        if (gb_alias && &g_set[i] == mem && j < 0) {
            /* the file keeps saying it in GiB, while GiB can say it exactly */
            long g = gptps_toml_find_dotted(after, "limits.max_memory_gb");
            FZ_ASSERT(g >= 0 && gptps_toml_text_at(after, (size_t)g), "max_memory_gb is gone after a save");
            FZ_ASSERT((unsigned long long)(gptps_strtod_c(gptps_toml_text_at(after, (size_t)g), NULL) * 1073741824.0) ==
                      strtoull(g_set[i].want, NULL, 10), "max_memory_gb does not say the value set live");
            continue;
        }
        if (j < 0) fprintf(stderr, "lost the live key [%s] (= %s) from:\n---\n%s---\nsaved:\n---\n%s---\n",
                           g_set[i].key, g_set[i].want, text, saved);
        FZ_ASSERT(j >= 0, "a value set live is not in the saved file");
        v = gptps_toml_text_at(after, (size_t)j);
        if (v && !strcmp(g_set[i].key, "app.huge")) {      /* past 64 bits it is written as a float */
            FZ_ASSERT(strtod(v, NULL) == strtod(g_set[i].want, NULL), "a large number set live is saved as another");
            continue;
        }
        if (!v || strcmp(v, g_set[i].want))
            fprintf(stderr, "[%s]: want [%s], saved [%s]\n", g_set[i].key, g_set[i].want, v ? v : "(array)");
        FZ_ASSERT(v && !strcmp(v, g_set[i].want), "a value set live is saved as another value");
    }

    /* every other key keeps its value */
    n = gptps_toml_count(before);
    for (i = 0; i < n; ++i) {
        char *d = dotted_of(before, i);
        long j;
        if (!d) continue;
        if (live_find(d) || (gb_alias && mem && !strcmp(d, "limits.max_memory_gb"))) { free(d); continue; }
        j = gptps_toml_find_dotted(after, d);
        if (j < 0) fprintf(stderr, "lost [%s] from:\n---\n%s---\nsaved:\n---\n%s---\n", d, text, saved);
        FZ_ASSERT(j >= 0, "a save lost a key it did not change");
        if (!same_value(before, i, after, (size_t)j)) fprintf(stderr, "[%s] changed in:\n---\n%s---\n", d, saved);
        FZ_ASSERT(same_value(before, i, after, (size_t)j), "a save changed a value nobody set");
        free(d);
    }
    /* ...and nothing was added but the live values */
    n = gptps_toml_count(after);
    for (i = 0; i < n; ++i) {
        char *d = dotted_of(after, i);
        if (!d) continue;
        if (gptps_toml_find_dotted(before, d) < 0 && !live_find(d))
            fprintf(stderr, "added [%s] to:\n---\n%s---\nsaved:\n---\n%s---\n", d, text, saved);
        FZ_ASSERT(gptps_toml_find_dotted(before, d) >= 0 || live_find(d), "a save added a key nobody set");
        free(d);
    }

    /* every line that held no changed value is still there, in order */
    {
        const char *out = saved;
        int line, lines = 1;
        const char *c;
        for (c = text; *c; ++c) if (*c == '\n') ++lines;
        for (line = 1; line <= lines; ++line) {
            size_t len = 0, olen;
            const char *l = line_of(text, line, &len);
            int changed = 0;
            for (i = 0; i < gptps_toml_count(before) && !changed; ++i) {
                char *d;
                if (gptps_toml_line_at(before, i) != line) continue;
                d = dotted_of(before, i);
                changed = d && (live_find(d) || (gb_alias && mem && !strcmp(d, "limits.max_memory_gb")));
                free(d);
            }
            if (!l || changed || (line == lines && len == 0)) continue;
            for (;;) {                       /* the next output line that is this one */
                const char *nl;
                if (!*out) {
                    fprintf(stderr, "line %d [%.*s] is gone from:\n---\n%s---\n", line, (int)len, l, saved);
                    FZ_ASSERT(0, "a save dropped or reordered a line that held no changed value");
                }
                nl = strchr(out, '\n');
                olen = nl ? (size_t)(nl - out) : strlen(out);
                if (olen == len && !memcmp(out, l, len)) { out += olen + (nl ? 1 : 0); break; }
                out += olen + (nl ? 1 : 0);
            }
        }
    }

    gptps_toml_free(after);
    free(saved);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static int quiet = 0;
    char path[512], *text, *first, *second;
    char names[FZ_MAX_FILE_TASKS][GPTPS_TASK_NAME_MAX + 1];
    size_t nnames, first_len = 0, second_len = 0;
    gptps_toml *before;
    gptps_config cfg;
    gptps *e = NULL;
    int opened, addons;
    uint32_t h;
    if (size > FZ_MAX_INPUT) return 0;
    if (!quiet) { gptps_set_log_sink(fz_quiet_sink, NULL); quiet = 1; }
    text = (char *)malloc(size + 1);
    if (!text) return 0;
    if (size) memcpy(text, data, size);
    text[size] = 0;
    if (strlen(text) != size) { free(text); return 0; }     /* a NUL: no file that parses has one */
    before = gptps_toml_parse_text("before.toml", text, NULL, 0);
    if (!before) { free(text); return 0; }                  /* refused: fuzz_config holds a save to that */
    {   /* A file that names add-ons is not opened - the loader would dlopen whatever
         * the fuzzer wrote - but an engine opened without it still saves into it. */
        const char *const *arr;
        addons = gptps_toml_find_dotted(before, "addons") >= 0 && gptps_toml_str_array(before, "", "addons", &arr) > 0;
    }
    fz_path(path, sizeof path, "save.toml");
    if (!fz_write(path, data, size)) { gptps_toml_free(before); free(text); return 0; }
    nnames = fz_file_task_names(before, names, FZ_MAX_FILE_TASKS);
    h = fz_hash(data, size);

    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.mode = GPTPS_RUN_MANUAL;
    cfg.config_path = path;
    opened = !addons && gptps_open_ex(&cfg, &e) == GPTPS_OK;
    if (!opened) {
        cfg.config_path = NULL;
        if (gptps_open_ex(&cfg, &e) != GPTPS_OK) { gptps_toml_free(before); free(text); remove(path); return 0; }
    }
    setup(e, names, nnames, 0);
    set_live(e, h, names, nnames);

    FZ_ASSERT(gptps_settings_save(e, path) == GPTPS_OK, "a file that parses is not saved");
    check_saved(before, text, path);
    /* a second save, with nothing changed since, writes the same bytes */
    first = fz_slurp(path, &first_len);
    FZ_ASSERT(gptps_settings_save(e, path) == GPTPS_OK, "a second save fails");
    second = fz_slurp(path, &second_len);
    if (first && second && (first_len != second_len || memcmp(first, second, first_len)))
        fprintf(stderr, "first save:\n---\n%s---\nsecond save:\n---\n%s---\n", first, second);
    FZ_ASSERT(first && second && first_len == second_len && !memcmp(first, second, first_len),
              "a second save, with nothing changed, changed the file");

    /* the file the engine opened, saved: a new engine opens it to the same values */
    if (opened) {
        gptps *e2 = NULL;
        size_t i, n = gptps_settings_count(e);
        gptps_status st;
        char items1[GPTPS_SETTINGS_VALUE_MAX], items2[GPTPS_SETTINGS_VALUE_MAX];
        int classic;
        cfg.config_path = path;
        st = gptps_open_ex(&cfg, &e2);
        if (st != GPTPS_OK) fprintf(stderr, "the saved file does not open:\n---\n%s---\n", second ? second : "");
        FZ_ASSERT(st == GPTPS_OK, "a file the engine opened does not open after a save");
        setup(e2, names, nnames, 1);
        /* A payload or result slot means something only in bounded mode: with
         * max_items 0, open_engine takes neither, and both read 0 whatever the file
         * says. So they are compared only when both engines are bounded. */
        items1[0] = items2[0] = 0;
        (void)gptps_settings_get(e, "bounded.max_items", items1, sizeof items1);
        (void)gptps_settings_get(e2, "bounded.max_items", items2, sizeof items2);
        classic = !strcmp(items1, "0") || !strcmp(items2, "0");
        for (i = 0; i < n; ++i) {
            gptps_setting_info info;
            char now[GPTPS_SETTINGS_VALUE_MAX];
            memset(&info, 0, sizeof info);
            info.struct_size = sizeof info;
            if (gptps_settings_get_info(e, i, &info) != GPTPS_OK) continue;
            if (classic && (!strcmp(info.key, "bounded.max_payload_bytes") || !strcmp(info.key, "bounded.max_result_bytes")))
                continue;
            now[0] = 0;
            FZ_ASSERT(gptps_settings_get(e2, info.key, now, sizeof now) == GPTPS_OK, "a setting is missing after a reopen");
            if (strcmp(now, info.value))
                fprintf(stderr, "[%s] was [%s], reopened [%s]; saved:\n---\n%s---\n", info.key, info.value, now,
                        second ? second : "");
            FZ_ASSERT(!strcmp(now, info.value), "a setting reads differently after a save and a reopen");
        }
        gptps_shutdown(e2);
    }
    free(first);
    free(second);
    gptps_shutdown(e);
    gptps_toml_free(before);
    free(text);
    {
        char tmp[600];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        remove(tmp);
    }
    remove(path);
    return 0;
}
