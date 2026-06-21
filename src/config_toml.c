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
};

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
        if (*p == '"') in_str = !in_str;
        else if (*p == '#' && !in_str) { *p = 0; break; }
    }
}

/* parse the value text into entry e (section/key already set) */
static int parse_value(struct gptps_toml *t, const char *section, const char *key, char *val)
{
    toml_entry *e;
    val = trim(val);
    if (!*val) return -1;

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
            if (str) {
                if (n == capn) { capn = capn ? capn * 2 : 4;
                    arr = (char **)gptps_realloc(arr, (size_t)capn * sizeof *arr); }
                if (arr) arr[n++] = str; else { gptps_free(str); }
            }
        }
        e = push(t);
        if (!e) { int k; for (k = 0; k < n; ++k) gptps_free(arr[k]); gptps_free(arr); return -1; }
        e->section = dupn(section, strlen(section));
        e->key = dupn(key, strlen(key));
        e->type = TT_ARR; e->arr = arr; e->arrn = n;
        return 0;
    }

    e = push(t);
    if (!e) return -1;
    e->section = dupn(section, strlen(section));
    e->key = dupn(key, strlen(key));

    if (val[0] == '"') {
        char *q = val + 1;
        while (*q && *q != '"') { if (*q == '\\' && q[1]) ++q; ++q; }
        e->type = TT_STR; e->s = unescape(val + 1, q);
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
    char *buf, *line, *save;
    struct gptps_toml *t;
    char section[256];

    if (errbuf && errlen) errbuf[0] = 0;
    f = fopen(path, "rb");
    if (!f) { if (errbuf && errlen) snprintf(errbuf, errlen, "cannot open %s", path); return NULL; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    buf = (char *)gptps_malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { /* tolerate short read */ }
    buf[sz] = 0;
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
    for (k = 0; k < t->n; ++k)
        if (strcmp(t->e[k].section, section) == 0 && strcmp(t->e[k].key, key) == 0)
            return &t->e[k];
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
    if (!e || e->type != TT_ARR) { *out = NULL; return 0; }
    *out = (const char *const *)e->arr;
    return e->arrn;
}
