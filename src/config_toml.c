/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * config_toml.c - a small, dependency-free TOML *subset* parser for GPTPS
 * config files. Supports:
 *   - # comments (whole-line and trailing, outside strings)
 *   - [section] and dotted [a.b] tables
 *   - key = value where value is an integer, float, true/false, "string",
 *     or a single-line ["array", "of", "strings"]
 * String escapes handled: \" \\ \n \t. This is intentionally a subset (no
 * multi-line values, no inline tables) - enough for GPTPS config, not full TOML.
 */
#include "gptps.h"
#include "gptps_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { TT_INT, TT_DBL, TT_BOOL, TT_STR, TT_ARR } toml_type;

typedef struct {
    char     *section;   /* "" for top level, else "limits" / "tasks.resize" */
    char     *key;
    toml_type type;
    long long i;
    double    d;
    int       b;
    char     *s;         /* TT_STR */
    char    **arr;       /* TT_ARR */
    int       arrn;
} toml_entry;

struct gptps_toml {
    toml_entry *e;
    size_t      n, cap;
    int         oom;     /* an allocation failed mid-parse: the table is INCOMPLETE */
};

/* A settings file is kilobytes. The cap exists because fopen() on a DIRECTORY
 * succeeds on POSIX and then reports ftell() == LONG_MAX - which used to become a
 * ~8 EiB gptps_malloc: NULL on a plain build, a hard abort under a hardened
 * allocator or ASan, for nothing worse than a mistyped path. */
#define GPTPS_TOML_MAX_BYTES (16UL * 1024UL * 1024UL)

/* ---- small helpers ---- */

static char *dupn(const char *s, size_t n)
{
    char *o = (char *)gptps_malloc(n + 1);
    if (!o) return NULL;
    memcpy(o, s, n); o[n] = 0;
    return o;
}

static int is_ws(int c) { return c == ' ' || c == '\t' || c == '\r'; }

static char *trim(char *s)
{
    char *end;
    while (*s && is_ws((unsigned char)*s)) ++s;
    if (!*s) return s;
    end = s + strlen(s) - 1;
    while (end > s && is_ws((unsigned char)*end)) *end-- = 0;
    return s;
}

/* unescape a quoted string body [start,end) into a fresh buffer */
static char *unescape(const char *p, const char *end)
{
    char *o = (char *)gptps_malloc((size_t)(end - p) + 1), *w;
    if (!o) return NULL;
    w = o;
    while (p < end) {
        if (*p == '\\' && p + 1 < end) {
            ++p;
            switch (*p) {
                case 'n': *w++ = '\n'; break;
                case 't': *w++ = '\t'; break;
                case '"': *w++ = '"';  break;
                case '\\': *w++ = '\\'; break;
                default: *w++ = *p; break;
            }
            ++p;
        } else {
            *w++ = *p++;
        }
    }
    *w = 0;
    return o;
}

static toml_entry *push(struct gptps_toml *t)
{
    if (t->n == t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 16;
        toml_entry *ne = (toml_entry *)gptps_realloc(t->e, nc * sizeof *ne);
        if (!ne) return NULL;
        t->e = ne; t->cap = nc;
    }
    memset(&t->e[t->n], 0, sizeof t->e[t->n]);
    return &t->e[t->n++];
}

/* strip a trailing unquoted # comment from a value string (in place) */
static void strip_comment(char *v)
{
    int in_str = 0;
    char *p = v;
    for (; *p; ++p) {
        /* Skip the escaped character, exactly as the value scanner below does.
         * Without this a \" inside a string flipped in_str back to "outside", so
         * `motd = "a\"b#c"` was cut at the '#' and the value silently lost its
         * tail on every save->reload round trip. The `in_str &&` guard mirrors
         * parse_value: a backslash outside a quoted body is not an escape and
         * must not be able to swallow a comment marker. */
        if (in_str && *p == '\\' && p[1]) { ++p; continue; }
        if (*p == '"') in_str = !in_str;
        else if (*p == '#' && !in_str) { *p = 0; break; }
    }
}

/* parse the value text into entry e (section/key already set).
 * Returns 0 on success and -1 when nothing was stored; on an allocation failure
 * it also raises t->oom, because a config silently missing a [limits] key is not
 * the same thing as a config with a line the subset grammar does not cover. */
static int parse_value(struct gptps_toml *t, const char *section, const char *key, char *val)
{
    toml_entry *e;
    char *sec, *k;
    val = trim(val);
    if (!*val) return -1;

    /* Name the row BEFORE push() publishes it. push() has already bumped t->n by
     * the time it returns, so a dup that failed afterwards left a LIVE entry with
     * section == NULL - and find() strcmp()s that field on the very next lookup.
     * Under a pool allocator (examples/embedded.c runs on a 256 KiB arena) that
     * is a startup SEGV, not a theoretical OOM. */
    sec = dupn(section, strlen(section));
    k   = dupn(key, strlen(key));
    if (!sec || !k) { gptps_free(sec); gptps_free(k); t->oom = 1; return -1; }

    if (val[0] == '[') {
        /* single-line array of strings */
        char *p = val + 1;
        char **arr = NULL; int n = 0, capn = 0;
        for (;;) {
            char *q, *str;
            while (*p && (is_ws((unsigned char)*p) || *p == ',')) ++p;
            if (*p == ']' || !*p) break;
            if (*p != '"') { /* only string arrays supported */ }
            if (*p == '"') {
                q = ++p;
                while (*q && *q != '"') { if (*q == '\\' && q[1]) ++q; ++q; }
                str = unescape(p, q);
                p = (*q == '"') ? q + 1 : q;
            } else {
                q = p; while (*q && *q != ',' && *q != ']') ++q;
                str = dupn(p, (size_t)(q - p)); if (str) trim(str);
                p = q;
            }
            if (!str) { t->oom = 1; goto arr_fail; }
            if (n == capn) {
                int nc = capn ? capn * 2 : 4;
                /* Grow through a TEMPORARY. `arr = gptps_realloc(arr, ...)` drops
                 * the only pointer to the old block on failure - leaking it and
                 * every string in it - and the old code then carried on with
                 * arr == NULL while n stayed put, publishing an (arr = NULL,
                 * arrn = n) pair that gptps_toml_str_array hands to callers
                 * verbatim: a NULL deref in gptps_open's addon loop. capn is
                 * likewise raised only once the growth actually succeeded; bumping
                 * it first is what stopped the `n == capn` retry from ever firing
                 * again. */
                char **na = (char **)gptps_realloc(arr, (size_t)nc * sizeof *arr);
                if (!na) { gptps_free(str); t->oom = 1; goto arr_fail; }
                arr = na; capn = nc;
            }
            arr[n++] = str;
        }
        e = push(t);
        if (!e) { t->oom = 1; goto arr_fail; }
        e->section = sec; e->key = k;
        e->type = TT_ARR; e->arr = arr; e->arrn = n;
        return 0;

    arr_fail:
        { int j; for (j = 0; j < n; ++j) gptps_free(arr[j]); }
        gptps_free(arr); gptps_free(sec); gptps_free(k);
        return -1;                 /* nothing published: the key is simply absent */
    }

    e = push(t);
    if (!e) { gptps_free(sec); gptps_free(k); t->oom = 1; return -1; }
    e->section = sec; e->key = k;

    if (val[0] == '"') {
        char *q = val + 1;
        while (*q && *q != '"') { if (*q == '\\' && q[1]) ++q; ++q; }
        e->type = TT_STR; e->s = unescape(val + 1, q);
        if (!e->s) t->oom = 1;     /* the row exists but lost its value */
    } else if (strcmp(val, "true") == 0) {
        e->type = TT_BOOL; e->b = 1;
    } else if (strcmp(val, "false") == 0) {
        e->type = TT_BOOL; e->b = 0;
    } else if (strchr(val, '.') || strchr(val, 'e') || strchr(val, 'E')) {
        e->type = TT_DBL; e->d = strtod(val, NULL);
    } else {
        e->type = TT_INT; e->i = strtoll(val, NULL, 10);
    }
    return 0;
}

/* ---- public-ish (internal) API ---- */

gptps_toml *gptps_toml_parse_file(const char *path, char *errbuf, size_t errlen)
{
    FILE *f;
    long sz;
    size_t got;
    char *buf, *line, *save;
    struct gptps_toml *t;
    char section[256];

    if (errbuf && errlen) errbuf[0] = 0;
    f = fopen(path, "rb");
    if (!f) { if (errbuf && errlen) snprintf(errbuf, errlen, "cannot open %s", path); return NULL; }
    /* Each step below is checked AND fills errbuf. Unchecked, a mistyped path
     * surfaced as gptps_open's E_CONFIG with a blank error string - and for the
     * commonest typo of all, a directory, as an ~8 EiB allocation request (see
     * GPTPS_TOML_MAX_BYTES). */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); if (errbuf && errlen) snprintf(errbuf, errlen, "cannot size %s", path); return NULL; }
    sz = ftell(f);
    if (sz < 0 || (unsigned long)sz > GPTPS_TOML_MAX_BYTES) {
        fclose(f);
        if (errbuf && errlen) snprintf(errbuf, errlen, "%s is not a readable config file", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); if (errbuf && errlen) snprintf(errbuf, errlen, "cannot rewind %s", path); return NULL; }
    buf = (char *)gptps_malloc((size_t)sz + 1);
    if (!buf) { fclose(f); if (errbuf && errlen) snprintf(errbuf, errlen, "out of memory reading %s", path); return NULL; }
    /* Terminate at what we actually read, not at what ftell promised: an editor
     * that truncates-and-rewrites the file under a SIGHUP-driven reload would
     * otherwise leave the tail of the buffer uninitialised - and parsed. */
    got = fread(buf, 1, (size_t)sz, f);
    buf[got] = 0;
    fclose(f);

    t = (struct gptps_toml *)gptps_calloc(1, sizeof *t);
    if (!t) { gptps_free(buf); return NULL; }
    section[0] = 0;

    for (line = buf, save = buf; ; ++save) {
        if (*save == '\n' || *save == 0) {
            char eol = *save;
            char *raw;
            *save = 0;
            raw = trim(line);
            strip_comment(raw);
            raw = trim(raw);
            if (*raw) {
                if (raw[0] == '[') {
                    char *close = strchr(raw, ']');
                    if (close) {
                        size_t L = (size_t)(close - raw - 1);
                        if (L >= sizeof section) L = sizeof section - 1;
                        memcpy(section, raw + 1, L); section[L] = 0;
                        { char *s2 = trim(section); if (s2 != section) memmove(section, s2, strlen(s2) + 1); }
                    }
                } else {
                    char *eq = strchr(raw, '=');
                    if (eq) {
                        char *key;
                        *eq = 0;
                        key = trim(raw);
                        parse_value(t, section, key, eq + 1);
                    }
                }
            }
            if (eol == 0) break;
            line = save + 1;
        }
    }
    gptps_free(buf);
    /* An incomplete config is not a valid config: a dropped [limits] key would
     * silently fall back to a compiled-in default the operator did not choose.
     * Only a genuine allocation failure sets this - the "line I cannot parse"
     * path leaves oom clear, so the subset grammar stays as tolerant as before. */
    if (t->oom) {
        gptps_toml_free(t);
        if (errbuf && errlen) snprintf(errbuf, errlen, "out of memory parsing %s", path);
        return NULL;
    }
    return t;
}

void gptps_toml_free(gptps_toml *t)
{
    size_t k; int j;
    if (!t) return;
    for (k = 0; k < t->n; ++k) {
        gptps_free(t->e[k].section); gptps_free(t->e[k].key); gptps_free(t->e[k].s);
        if (t->e[k].arr) { for (j = 0; j < t->e[k].arrn; ++j) gptps_free(t->e[k].arr[j]); gptps_free(t->e[k].arr); }
    }
    gptps_free(t->e); gptps_free(t);
}

static const toml_entry *find(const gptps_toml *t, const char *section, const char *key)
{
    size_t k;
    for (k = 0; k < t->n; ++k) {
        /* Defence in depth: parse_value now only ever publishes a fully named
         * row, but a NULL here is a strcmp() crash rather than a missed key. */
        if (!t->e[k].section || !t->e[k].key) continue;
        if (strcmp(t->e[k].section, section) == 0 && strcmp(t->e[k].key, key) == 0)
            return &t->e[k];
    }
    return NULL;
}

int gptps_toml_int(const gptps_toml *t, const char *section, const char *key, long long *out)
{
    const toml_entry *e = t ? find(t, section, key) : NULL;
    if (!e) return 0;
    if (e->type == TT_INT) { *out = e->i; return 1; }
    if (e->type == TT_DBL) { *out = (long long)e->d; return 1; }
    return 0;
}
int gptps_toml_double(const gptps_toml *t, const char *section, const char *key, double *out)
{
    const toml_entry *e = t ? find(t, section, key) : NULL;
    if (!e) return 0;
    if (e->type == TT_DBL) { *out = e->d; return 1; }
    if (e->type == TT_INT) { *out = (double)e->i; return 1; }
    return 0;
}
int gptps_toml_bool(const gptps_toml *t, const char *section, const char *key, int *out)
{
    const toml_entry *e = t ? find(t, section, key) : NULL;
    if (!e || e->type != TT_BOOL) return 0;
    *out = e->b; return 1;
}
const char *gptps_toml_str(const gptps_toml *t, const char *section, const char *key)
{
    const toml_entry *e = t ? find(t, section, key) : NULL;
    return (e && e->type == TT_STR) ? e->s : NULL;
}
int gptps_toml_str_array(const gptps_toml *t, const char *section, const char *key, const char *const **out)
{
    const toml_entry *e = t ? find(t, section, key) : NULL;
    /* !e->arr as well as the type: a caller loops out[0..n), so a non-zero count
     * beside a NULL base is worse than reporting no array at all. */
    if (!e || e->type != TT_ARR || !e->arr) { *out = NULL; return 0; }
    *out = (const char *const *)e->arr;
    return e->arrn;
}
