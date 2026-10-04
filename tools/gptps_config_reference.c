/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * gptps_config_reference.c - write docs/CONFIG.md from the settings registry.
 *
 *   gptps_config_reference                     the reference, to stdout
 *   gptps_config_reference --write FILE        ... into FILE
 *   gptps_config_reference --check FILE        exit 1 if FILE is not what it would write
 *
 * A hand-written list of config keys drifts: a key is added, renamed or given a
 * new range, and the document goes on describing the old one. This one is read off
 * the engine itself. It opens an engine, registers a task named "<task>" and
 * defines a resource named "<name>", so the keys those create come out with their
 * placeholders already in them, installs the shipped add-ons that have settings,
 * and prints every key the registry holds, with its type, range, default, when a
 * change applies, and its description. CTest runs --check (config_reference), so a
 * change to a key fails the build until the document is regenerated.
 *
 * The keys the file reads but the registry does not hold - addons, max_memory_gb,
 * [task_defaults] - are written from the table below; [task_defaults] mirrors the
 * registry's own per-task keys, so it cannot drift either.
 */
#include "gptps.h"
#include "gptps_tui.h"
#include "gptps_gpu_quota.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *p; size_t n, cap; } out_t;

static void put(out_t *o, const char *fmt, ...)
{
    va_list ap;
    int k;
    for (;;) {
        size_t room = o->cap - o->n;
        va_start(ap, fmt);
        k = vsnprintf(o->p ? o->p + o->n : NULL, o->p ? room : 0, fmt, ap);
        va_end(ap);
        if (k < 0) { fprintf(stderr, "format error\n"); exit(2); }
        if (o->p && (size_t)k < room) { o->n += (size_t)k; return; }
        o->cap = o->cap * 2 + (size_t)k + 4096;
        o->p = (char *)realloc(o->p, o->cap);
        if (!o->p) { fprintf(stderr, "out of memory\n"); exit(2); }
    }
}

/* Markdown table cell text: a | would end the cell. */
static void cell(out_t *o, const char *s)
{
    for (; *s; ++s) { if (*s == '|') put(o, "\\|"); else put(o, "%c", *s); }
}

static gptps_status noop(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }

static char *dup(const char *s)
{
    char *d = (char *)malloc(strlen(s) + 1);
    if (!d) { fprintf(stderr, "out of memory\n"); exit(2); }
    return strcpy(d, s);
}

/* The values the reference must not show as this machine's: resolved at open from
 * the hardware, or set by whoever defines the key. */
static const struct { const char *key, *shown; } DEFAULT_NOTE[] = {
    { "limits.max_memory_bytes",     "`0` (auto: 3/4 of the memory the machine has)" },
    { "limits.max_concurrent_tasks", "`0` (auto: one worker per logical CPU)" },
    { "resources.<name>",            "the budget the resource is defined with" },
    { "gpu_quota.total_units",       "the units `gptps_gpu_quota_install` is given" },
};

/* What each table is for, ahead of its keys. */
static const struct { const char *section, *intro; } SECTIONS[] = {
    { "",
      "Top-level keys, before any `[table]`." },
    { "limits",
      "The engine's size and its admission budget. The keys that size the engine are read at "
      "open, and a value the host passes in `gptps_config` wins over the file's. So does a "
      "`max_dead_letters` or `shutdown_grace_ms` passed there, where 0 means not set and "
      "`GPTPS_LIMIT_NONE` means no limit, which is 0 here." },
    { "scheduler",
      "How the dispatcher chooses among work that waits." },
    { "bounded",
      "Bounded mode: no allocation once work starts ([BOUNDED.md](BOUNDED.md)). "
      "`max_items = 0`, the default, is the classic engine." },
    { "resources",
      "Named resources: each key defines one with the budget the engine admits work "
      "against - GPUs, licences, connections - or re-budgets one already defined, by the "
      "host or an add-on. The name is yours; `[resources] gpu = 4` defines `gpu`." },
    { "task_defaults",
      "Values for every task, applied as it registers. A value in `[tasks.<task>]` wins "
      "over these, and these win over the task's compiled-in definition." },
    { "tasks.<task>",
      "One task's values, applied when a task of that name registers. A table for a task "
      "that never registers is reported by `gptps_config_check`." },
    { "tasks.<task>.resources",
      "What one run of the task costs of each named resource; admission waits until it fits." },
    { "stats",
      "Counters. They read through the settings API; `gptps_settings_save` never writes them." },
};

/* The keys the file reads that the registry does not hold. */
static const struct { const char *section, *leaf, *type, *def, *applies, *desc; } FILE_ONLY[] = {
    { "", "addons", "a list of \"paths\"", "`[]`", "next start",
      "shared-library add-ons to load at open, in order; each path goes to the platform "
      "loader as written, and one that does not load fails the open" },
    { "limits", "max_memory_gb", "number, 0 to 1000000000", "`0` (auto)", "at once",
      "max_memory_bytes in GiB (2^30 bytes), for a file that would rather not count bytes; "
      "if both are set, max_memory_bytes wins; 0 = auto: 3/4 of the machine's memory" },
};

/* The keys gptps_config sets at open, where 0 means "not set". Each must be in the
 * registry and say what its own 0 means, or the generator stops: the table built
 * from them would go wrong without a word. */
static const struct { const char *key, *field; } STRUCT_FIELD[] = {
    { "limits.max_concurrent_tasks", "`limits.max_concurrent_tasks`" },
    { "limits.max_memory_bytes",     "`limits.max_memory_bytes`" },
    { "limits.max_intake_depth",     "`limits.max_intake_depth`" },
    { "limits.max_dead_letters",     "`max_dead_letters`; no limit is `GPTPS_LIMIT_NONE`" },
    { "limits.shutdown_grace_ms",    "`shutdown_grace_ms`; waiting forever is `GPTPS_LIMIT_NONE`" },
    { "bounded.max_items",           "`max_items`" },
    { "bounded.max_payload_bytes",   "`max_payload_bytes`" },
    { "bounded.max_result_bytes",    "`max_result_bytes`" },
};

/* What a description says 0 means: the text after its "0 = ", or NULL. */
static const char *zero_means(const char *desc)
{
    const char *z = desc ? strstr(desc, "0 = ") : NULL;
    return z ? z + 4 : NULL;
}

static void zero_row(out_t *o, const char *key, const char *meaning)
{
    size_t i;
    put(o, "| `%s` | ", key);
    cell(o, meaning);
    put(o, " | ");
    for (i = 0; i < sizeof STRUCT_FIELD / sizeof STRUCT_FIELD[0]; ++i)
        if (!strcmp(STRUCT_FIELD[i].key, key)) break;
    put(o, "%s |\n", i < sizeof STRUCT_FIELD / sizeof STRUCT_FIELD[0] ? STRUCT_FIELD[i].field : "-");
}

static void kind(out_t *o, const gptps_setting_info *in)
{
    char range[128];
    range[0] = 0;
    if (in->has_range) snprintf(range, sizeof range, ", %.17g to %.17g", in->min, in->max);
    switch (in->type) {
        case GPTPS_SETTING_INT:    put(o, "whole number%s", range); break;
        case GPTPS_SETTING_UINT:   put(o, "whole number%s", *range ? range : ", 0 or more"); break;
        case GPTPS_SETTING_DOUBLE: put(o, "number%s", range); break;
        case GPTPS_SETTING_BOOL:   put(o, "true or false"); break;
        case GPTPS_SETTING_STRING: put(o, "a \"string\""); break;
        case GPTPS_SETTING_ENUM: default: {
            const char *const *c;
            put(o, "one of");
            for (c = in->choices; c && *c; ++c) put(o, "%s `\"%s\"`", c == in->choices ? "" : ",", *c);
            break;
        }
    }
}

static void def_value(out_t *o, const gptps_setting_info *in, const char *key)
{
    size_t i;
    for (i = 0; i < sizeof DEFAULT_NOTE / sizeof DEFAULT_NOTE[0]; ++i)
        if (!strcmp(DEFAULT_NOTE[i].key, key)) { put(o, "%s", DEFAULT_NOTE[i].shown); return; }
    if (in->type == GPTPS_SETTING_STRING || in->type == GPTPS_SETTING_ENUM) put(o, "`\"%s\"`", in->defval);
    else put(o, "`%s`", in->defval);
}

/* The registry, in the order it holds the keys. */
typedef struct { gptps_setting_info info; char key[320]; int addon; } row_t;

static void table_head(out_t *o)
{
    put(o, "| Key | Type | Default | Applies | What it does |\n");
    put(o, "|---|---|---|---|---|\n");
}

static void row(out_t *o, const row_t *r, size_t seclen)
{
    const char *leaf = r->key + (seclen ? seclen + 1 : 0);
    put(o, "| `%s` | ", leaf);
    kind(o, &r->info);
    put(o, " | ");
    def_value(o, &r->info, r->key);
    put(o, " | %s | ", r->info.hot ? "at once" : "next start");
    cell(o, r->info.desc ? r->info.desc : "");
    put(o, " |\n");
}

/* The table a key sits in: everything before its last dot. */
static size_t section_len(const char *key)
{
    const char *dot = strrchr(key, '.');
    return dot ? (size_t)(dot - key) : 0;
}

static int in_section(const char *key, const char *section)
{
    size_t sl = strlen(section);
    return section_len(key) == sl && strncmp(key, section, sl) == 0;
}

static void file_only_rows(out_t *o, const char *section)
{
    size_t i;
    for (i = 0; i < sizeof FILE_ONLY / sizeof FILE_ONLY[0]; ++i) {
        if (strcmp(FILE_ONLY[i].section, section) != 0) continue;
        put(o, "| `%s` | %s | %s | %s | ", FILE_ONLY[i].leaf, FILE_ONLY[i].type, FILE_ONLY[i].def, FILE_ONLY[i].applies);
        cell(o, FILE_ONLY[i].desc);
        put(o, " (file only) |\n");
    }
}

static void section(out_t *o, const char *name, const row_t *rows, size_t n, int addon, const char *level)
{
    size_t i, s;
    int any = 0;
    for (i = 0; i < n; ++i) if (rows[i].addon == addon && in_section(rows[i].key, name)) any = 1;
    for (i = 0; i < sizeof FILE_ONLY / sizeof FILE_ONLY[0]; ++i) if (!strcmp(FILE_ONLY[i].section, name)) any = 1;
    if (!strcmp(name, "task_defaults")) any = 1;
    if (!any) return;
    put(o, "\n%s %s%s%s\n\n", level, *name ? "[" : "", *name ? name : "Top level", *name ? "]" : "");
    for (s = 0; s < sizeof SECTIONS / sizeof SECTIONS[0]; ++s)
        if (!strcmp(SECTIONS[s].section, name)) put(o, "%s\n\n", SECTIONS[s].intro);
    table_head(o);
    if (!strcmp(name, "task_defaults")) {            /* the per-task keys, for every task */
        for (i = 0; i < n; ++i) {
            if (!in_section(rows[i].key, "tasks.<task>")) continue;
            row(o, &rows[i], strlen("tasks.<task>"));
        }
        return;
    }
    for (i = 0; i < n; ++i) {
        if (rows[i].addon != addon || !in_section(rows[i].key, name)) continue;
        row(o, &rows[i], strlen(name));
        if (!strcmp(rows[i].key, "limits.max_memory_bytes")) file_only_rows(o, "limits");
    }
    if (strcmp(name, "limits") != 0) file_only_rows(o, name);
}

static int seen(const row_t *rows, size_t n, const char *key)
{
    size_t i;
    for (i = 0; i < n; ++i) if (!strcmp(rows[i].key, key)) return 1;
    return 0;
}

static void generate(out_t *o)
{
    static const char *const ORDER[] = { "", "limits", "scheduler", "bounded", "resources", "task_defaults",
                                         "tasks.<task>", "tasks.<task>.resources", "stats" };
    gptps *e;
    gptps_config cfg;
    gptps_task_def d;
    gptps_tui *tui;
    gptps_gpu_quota *gq;
    row_t *rows;
    size_t n, i, core, k;

    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    if (gptps_open_ex(&cfg, &e) != GPTPS_OK) { fprintf(stderr, "cannot open an engine\n"); exit(2); }
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "<task>"; d.run = noop; d.exec = GPTPS_EXEC_INPROC;
    if (gptps_register_task(e, &d) != GPTPS_OK || gptps_define_resource(e, "<name>", 0) != GPTPS_OK) {
        fprintf(stderr, "cannot register the placeholder task and resource\n"); exit(2);
    }
    core = gptps_settings_count(e);
    tui = gptps_tui_install(e, NULL);
    gq = gptps_gpu_quota_install(e, 0, 0);
    if (!tui || !gq) { fprintf(stderr, "cannot install the add-ons\n"); exit(2); }

    n = gptps_settings_count(e);
    rows = (row_t *)calloc(n ? n : 1, sizeof *rows);
    if (!rows) { fprintf(stderr, "out of memory\n"); exit(2); }
    for (i = 0, k = 0; i < n; ++i) {
        row_t *r = &rows[k];
        r->info.struct_size = sizeof r->info;
        if (gptps_settings_get_info(e, i, &r->info) != GPTPS_OK) continue;
        snprintf(r->key, sizeof r->key, "%s", r->info.key);
        r->info.key = r->key;               /* the copy outlives the engine */
        r->addon = i >= core;
        /* the gpu quota's resource is one more <name>: its own keys are what it adds */
        if (r->addon && (!strncmp(r->key, "resources.", 10) || !strncmp(r->key, "tasks.", 6))) continue;
        if (seen(rows, k, r->key)) continue;
        r->info.desc = dup(r->info.desc ? r->info.desc : "");
        ++k;
    }
    n = k;

    put(o, "# Configuration reference\n\n");
    put(o, "<!-- Generated by tools/gptps_config_reference.c from the engine's settings registry.\n"
           "     Do not edit it by hand: run `gptps_config_reference --write docs/CONFIG.md`.\n"
           "     The config_reference test fails while this file and the code disagree. -->\n\n");
    put(o, "Every key a GPTPS config file can hold. This page is generated from the engine's own\n"
           "settings registry, so it lists exactly the keys the code has. How the file is read,\n"
           "checked, reloaded and saved is in the [Readme](../Readme.md#configuration-file-optional).\n\n");
    put(o, "- **Type** is what the value must be, written as TOML writes it: a number bare, `true`\n"
           "  or `false`, a string in `\"quotes\"` - so `\"4\"` is a string, not a number. A value\n"
           "  that is not what its key takes fails `gptps_open` with `GPTPS_E_CONFIG`, and the log\n"
           "  names the file, the line and the key. So does a key the engine's own tables do not\n"
           "  have. A key in any other table waits for the host or a plug-in to define it, and\n"
           "  `gptps_config_check` reports it if nothing does.\n");
    put(o, "- **Default** is the value when neither the file nor the host sets one.\n");
    put(o, "- **Applies** says when a change takes effect while the engine runs - through\n"
           "  `gptps_settings_set`, the dashboard, or `gptps_settings_reload`: at once, or at the\n"
           "  next start.\n");
    put(o, "- Units are in the names: `_bytes`, `_ms`, `_seconds`.\n");
    put(o, "- `<task>` stands for a task's name and `<name>` for a resource's. A part of a\n"
           "  name that holds anything but letters, digits, `_` and `-` goes in quotes, as\n"
           "  TOML has it: `[tasks.\"resize v2\"]`, `\"odd name\" = 1`.\n");
    put(o, "- Every key is also a setting: `gptps_settings_get` and `gptps_settings_set` read and\n"
           "  change it by its full name, such as `limits.max_intake_depth`. The keys marked\n"
           "  *file only* are read from the file and are not settings.\n");

    /* What 0 means, key by key, from the descriptions the tables below show. */
    for (i = 0; i < sizeof STRUCT_FIELD / sizeof STRUCT_FIELD[0]; ++i) {
        size_t j;
        for (j = 0; j < n; ++j) if (!strcmp(rows[j].key, STRUCT_FIELD[i].key)) break;
        if (j == n || !zero_means(rows[j].info.desc)) {
            fprintf(stderr, "%s: %s\n", STRUCT_FIELD[i].key, j == n ? "not in the settings registry"
                                                                    : "its description does not say what 0 means (\"0 = ...\")");
            exit(2);
        }
    }
    put(o, "\n## What 0 means\n\n"
           "A 0 does not mean the same thing everywhere, so look it up before you write one:\n\n"
           "- In `gptps_config` and its `limits`, 0 always means *not set*: the engine takes the\n"
           "  config file's value, or else the default.\n"
           "- In the config file, and in a live `gptps_settings_set`, 0 is a value, and each key\n"
           "  below gives it a meaning of its own. A key not listed takes 0 at face value.\n"
           "- For `max_dead_letters` and `shutdown_grace_ms` the file's 0 is not the default, so\n"
           "  `gptps_config` writes \"no limit\" as `GPTPS_LIMIT_NONE`.\n\n"
           "The meanings are those the tables below give, from the same descriptions.\n\n");
    put(o, "| Key | 0 in the file, or set live | In `gptps_config` (0: not set) |\n");
    put(o, "|---|---|---|\n");
    for (i = 0; i < n; ++i) {
        const char *z = zero_means(rows[i].info.desc);
        if (!rows[i].addon && z) zero_row(o, rows[i].key, z);
        if (!strcmp(rows[i].key, "limits.max_memory_bytes")) {       /* its file-only spelling */
            size_t f;
            for (f = 0; f < sizeof FILE_ONLY / sizeof FILE_ONLY[0]; ++f) {
                char key[320];
                if (strcmp(FILE_ONLY[f].section, "limits") || !(z = zero_means(FILE_ONLY[f].desc))) continue;
                snprintf(key, sizeof key, "limits.%s", FILE_ONLY[f].leaf);
                zero_row(o, key, z);
            }
        }
    }
    for (i = 0; i < n; ++i) {                     /* the add-ons' keys, after the engine's */
        const char *z = zero_means(rows[i].info.desc);
        if (rows[i].addon && z) zero_row(o, rows[i].key, z);
    }

    for (i = 0; i < sizeof ORDER / sizeof ORDER[0]; ++i) section(o, ORDER[i], rows, n, 0, "##");
    for (i = 0; i < n; ++i) {                     /* any table the list above lacks */
        size_t j, sl = section_len(rows[i].key);
        char name[320];
        int known = 0;
        if (rows[i].addon) continue;
        memcpy(name, rows[i].key, sl); name[sl] = 0;
        for (j = 0; j < sizeof ORDER / sizeof ORDER[0]; ++j) if (!strcmp(ORDER[j], name)) known = 1;
        for (j = 0; j < i && !known; ++j)
            if (!rows[j].addon && section_len(rows[j].key) == sl && !strncmp(rows[j].key, name, sl)) known = 1;
        if (!known) section(o, name, rows, n, 0, "##");
    }

    put(o, "\n## Add-ons\n\n"
           "The shipped add-ons that have settings add these keys when they are installed. A\n"
           "plug-in or host can add its own: `gptps_register_setting`, `define_global` and\n"
           "`define_task_setting` make keys the file sets, and `gptps_config_check` reports any\n"
           "key nothing claimed.\n");
    for (i = 0; i < n; ++i) {
        size_t j, sl = section_len(rows[i].key);
        char name[320];
        int done = 0;
        if (!rows[i].addon) continue;
        memcpy(name, rows[i].key, sl); name[sl] = 0;
        for (j = 0; j < i; ++j)
            if (rows[j].addon && section_len(rows[j].key) == sl && !strncmp(rows[j].key, name, sl)) done = 1;
        if (!done) section(o, name, rows, n, 1, "###");
    }

    for (i = 0; i < n; ++i) free((void *)rows[i].info.desc);
    free(rows);
    gptps_shutdown(e);
    gptps_gpu_quota_close(gq);
    gptps_tui_close(tui);
}

static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *b = NULL;
    size_t cap = 0, n = 0, k;
    if (!f) return NULL;
    do {
        if (cap - n < 4096) {
            cap = cap * 2 + 65536;
            b = (char *)realloc(b, cap);
            if (!b) { fclose(f); return NULL; }
        }
        k = fread(b + n, 1, cap - n - 1, f);
        n += k;
    } while (k > 0);
    fclose(f);
    b[n] = 0;
    *len = n;
    return b;
}

/* CRLF as LF: a Windows checkout may convert the file. */
static size_t lf(char *s, size_t n)
{
    size_t i, w = 0;
    for (i = 0; i < n; ++i) if (!(s[i] == '\r' && i + 1 < n && s[i + 1] == '\n')) s[w++] = s[i];
    s[w] = 0;
    return w;
}

int main(int argc, char **argv)
{
    out_t o;
    int rc = 2;
    memset(&o, 0, sizeof o);
    generate(&o);
    if (argc == 1) {
        rc = fwrite(o.p, 1, o.n, stdout) == o.n ? 0 : 2;
    } else if (argc == 3 && !strcmp(argv[1], "--write")) {
        FILE *f = fopen(argv[2], "wb");
        int ok = f && fwrite(o.p, 1, o.n, f) == o.n;
        if (f && fclose(f) != 0) ok = 0;
        if (ok) rc = 0;
        else fprintf(stderr, "cannot write %s\n", argv[2]);
    } else if (argc == 3 && !strcmp(argv[1], "--check")) {
        size_t n = 0, i, line = 1;
        char *have = read_all(argv[2], &n);
        if (!have) {
            fprintf(stderr, "cannot read %s\n", argv[2]);
        } else {
            n = lf(have, n);
            if (n == o.n && memcmp(have, o.p, n) == 0) {
                printf("%s matches the settings registry\n", argv[2]);
                rc = 0;
            } else {
                for (i = 0; i < n && i < o.n && have[i] == o.p[i]; ++i) if (have[i] == '\n') ++line;
                fprintf(stderr, "%s is out of date from line %lu: the settings registry changed.\n"
                                "Regenerate it: gptps_config_reference --write %s\n",
                        argv[2], (unsigned long)line, argv[2]);
                rc = 1;
            }
            free(have);
        }
    } else {
        fprintf(stderr, "usage: gptps_config_reference [--write FILE | --check FILE]\n");
    }
    free(o.p);
    return rc;
}
