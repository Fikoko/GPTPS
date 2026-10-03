/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * config_toml.c - a small, dependency-free TOML *subset* parser for GPTPS
 * config files. Supports:
 *   - # comments (whole-line and trailing, outside strings)
 *   - [section] and dotted [a.b] tables, a part of the name "quoted" when it holds
 *     anything else: [tasks."resize v2"]
 *   - key = value, where the key is a dotted path of bare (letters, digits, _ -)
 *     and "quoted" parts - a.b, "x.y", a."b c" - and the value is an integer,
 *     float, true/false, "string", or a single-line ["array", "of", "strings"]
 * String escapes handled: \" \\ \n \t. This is intentionally a subset (no
 * multi-line values, no inline tables) - enough for GPTPS config, not full TOML.
 *
 * STRICT. Every line must parse, and one that does not is an error naming the
 * file and the line - not a line skipped. A config that silently loses a line
 * runs on defaults the operator never chose: `max_concurrent_tasks = = 4` used to
 * read as 0, which means "auto", and the engine started one worker per CPU. So a
 * number must be all number, a key may be set once per table, and an array may
 * hold only strings.
 *
 * Each entry keeps its line, its value as TEXT - the form the settings registry
 * validates, so a file value passes through exactly the checks a live
 * gptps_settings_set does - and whether a consumer has claimed it, so the loader
 * can report a key that nothing used (docs/CONFIG.md).
 */
#include "gptps.h"
#include "gptps_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <float.h>

typedef enum { TT_INT, TT_DBL, TT_BOOL, TT_STR, TT_ARR } toml_type;

typedef struct {
    char     *section;   /* "" for top level, else "limits" / "tasks.resize" */
    char     *key;       /* unquoted */
    char     *text;      /* a scalar as the registry spells it: a number as written,
                          * true/false, a string's contents. NULL for an array. */
    toml_type type;
    long long i;
    double    d;
    int       b;
    char    **arr;       /* TT_ARR */
    int       arrn;
    int       line;
    int       claimed;   /* a consumer used it (gptps_toml_claim_at) */
    int       big;       /* an integer past LLONG_MAX: only its text holds it */
} toml_entry;

typedef struct {
    int   line;
    char *name;          /* the [table] name, its quoted parts unquoted */
} toml_table;

struct gptps_toml {
    toml_entry *e;
    size_t      n, cap;
    toml_table *tb;      /* every [table] line, in order (gptps_settings_save edits by them) */
    size_t      ntb, captb;
    char       *path;
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
static int is_bare(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '.';
}

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
static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* Unescape a quoted body [p,end) into a fresh buffer: TOML's escapes - \" \\ \n
 * \t \r \b \f, and \uXXXX / \UXXXXXXXX as UTF-8. Any other backslash is an error:
 * reading "C:\plugins" as C:plugins, as the parser once did, loses the operator's
 * text without a word. NULL with *why set for that, NULL alone out of memory. The
 * result is never longer than the body. */
static char *unescape(const char *p, const char *end, const char **why)
{
    char *o = (char *)gptps_malloc((size_t)(end - p) + 1), *w;
    *why = NULL;
    if (!o) return NULL;
    w = o;
    while (p < end) {
        if (*p == '\\' && p + 1 < end) {
            ++p;
            switch (*p) {
                case 'n': *w++ = '\n'; break;
                case 't': *w++ = '\t'; break;
                case 'r': *w++ = '\r'; break;
                case 'b': *w++ = '\b'; break;
                case 'f': *w++ = '\f'; break;
                case '"': *w++ = '"';  break;
                case '\\': *w++ = '\\'; break;
                case 'u': case 'U': {
                    int k, digits = *p == 'u' ? 4 : 8;
                    unsigned long cp = 0;
                    for (k = 1; k <= digits; ++k) {
                        int h = p + k < end ? hexval((unsigned char)p[k]) : -1;
                        if (h < 0) { *why = "a \\u escape needs 4 hex digits, \\U 8"; gptps_free(o); return NULL; }
                        cp = cp * 16 + (unsigned long)h;
                    }
                    if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
                        *why = "a \\u escape that is not a character (or is NUL)"; gptps_free(o); return NULL;
                    }
                    if (cp < 0x80) *w++ = (char)cp;
                    else if (cp < 0x800) { *w++ = (char)(0xC0 | (cp >> 6)); *w++ = (char)(0x80 | (cp & 0x3F)); }
                    else if (cp < 0x10000) { *w++ = (char)(0xE0 | (cp >> 12)); *w++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                                             *w++ = (char)(0x80 | (cp & 0x3F)); }
                    else { *w++ = (char)(0xF0 | (cp >> 18)); *w++ = (char)(0x80 | ((cp >> 12) & 0x3F));
                           *w++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *w++ = (char)(0x80 | (cp & 0x3F)); }
                    p += digits;
                    break;
                }
                default:
                    *why = "an unknown escape after a backslash - write a backslash itself as \\\\";
                    gptps_free(o);
                    return NULL;
            }
            ++p;
        } else {
            unsigned char c = (unsigned char)*p;
            if ((c < 0x20 && c != '\t') || c == 0x7f) {     /* as TOML: escape them */
                *why = "a control character in a quoted string - write it as an escape, \\u0001";
                gptps_free(o);
                return NULL;
            }
            *w++ = *p++;
        }
    }
    *w = 0;
    return o;
}

/* The end of a quoted body starting just past its opening quote: the closing
 * quote, or NULL when the line ends first. Escapes are skipped, as unescape reads
 * them. */
static const char *close_quote(const char *p)
{
    while (*p && *p != '"') { if (*p == '\\' && p[1]) ++p; ++p; }
    return *p == '"' ? p : NULL;
}

/* strip a trailing unquoted # comment from a line (in place) */
static void strip_comment(char *v)
{
    int in_str = 0;
    char *p = v;
    for (; *p; ++p) {
        /* Skip the escaped character, exactly as the value scanner does. Without
         * this a \" inside a string flipped in_str back to "outside", so
         * `motd = "a\"b#c"` was cut at the '#' and the value silently lost its
         * tail on every save->reload round trip. The `in_str &&` guard: a
         * backslash outside a quoted body is not an escape and must not be able to
         * swallow a comment marker. */
        if (in_str && *p == '\\' && p[1]) { ++p; continue; }
        if (*p == '"') in_str = !in_str;
        else if (*p == '#' && !in_str) { *p = 0; break; }
    }
}

static int fail(char *errbuf, size_t errlen, const char *path, int line, const char *fmt, ...)
{
    if (errbuf && errlen) {
        va_list ap;
        int k = line ? snprintf(errbuf, errlen, "%s:%d: ", path, line) : snprintf(errbuf, errlen, "%s: ", path);
        if (k < 0) k = 0;
        if ((size_t)k < errlen) {
            va_start(ap, fmt);
            vsnprintf(errbuf + k, errlen - (size_t)k, fmt, ap);
            va_end(ap);
        }
    }
    return -1;
}

static const toml_entry *find(const gptps_toml *t, const char *section, const char *key)
{
    size_t k;
    for (k = 0; k < t->n; ++k)
        if (strcmp(t->e[k].section, section) == 0 && strcmp(t->e[k].key, key) == 0)
            return &t->e[k];
    return NULL;
}

/* The entry whose section + "." + key is `dotted` (see gptps_toml_find_dotted). */
static const toml_entry *find_dotted(const gptps_toml *t, const char *dotted)
{
    size_t k, dl = strlen(dotted);
    for (k = 0; k < t->n; ++k) {
        const toml_entry *e = &t->e[k];
        size_t sl = strlen(e->section), kl = strlen(e->key);
        if (sl == 0) { if (kl == dl && memcmp(e->key, dotted, dl) == 0) return e; continue; }
        if (sl + 1 + kl == dl && memcmp(e->section, dotted, sl) == 0 && dotted[sl] == '.' &&
            memcmp(e->key, dotted + sl + 1, kl) == 0)
            return e;
    }
    return NULL;
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

static void entry_free(toml_entry *e)
{
    int j;
    gptps_free(e->section); gptps_free(e->key); gptps_free(e->text);
    if (e->arr) { for (j = 0; j < e->arrn; ++j) gptps_free(e->arr[j]); gptps_free(e->arr); }
}

/* Parse the value text `val` into a fresh entry. 0 on success; -1 with errbuf set. */
static int parse_value(struct gptps_toml *t, const char *section, const char *key, char *val,
                       int line, char *errbuf, size_t errlen)
{
    toml_entry tmp, *e;
    const toml_entry *dup;
    const char *why;
    char dotted[512];
    memset(&tmp, 0, sizeof tmp);
    tmp.line = line;
    snprintf(dotted, sizeof dotted, "%s%s%s", section, *section ? "." : "", key);
    val = trim(val);
    if (!*val) return fail(errbuf, errlen, t->path, line, "%s: missing value after =", dotted);
    /* Set twice, however it is spelled: [a.b] c = 1 and [a] "b.c" = 2 are one key. */
    if ((dup = find_dotted(t, dotted)) != NULL)
        return fail(errbuf, errlen, t->path, line, "%s is set twice (first on line %d)", dotted, dup->line);

    if (val[0] == '"') {                                  /* "string" */
        const char *q = close_quote(val + 1);
        if (!q) return fail(errbuf, errlen, t->path, line, "%s: the string has no closing quote", dotted);
        if (*trim((char *)q + 1))
            return fail(errbuf, errlen, t->path, line, "%s: unexpected text after the string", dotted);
        tmp.type = TT_STR;
        if (!(tmp.text = unescape(val + 1, q, &why))) {
            if (why) return fail(errbuf, errlen, t->path, line, "%s: %s", dotted, why);
            goto oom;
        }
    } else if (val[0] == '[') {                           /* ["array", "of", "strings"] */
        char *p = val + 1;
        int capn = 0;
        tmp.type = TT_ARR;
        for (;;) {
            const char *q;
            char *str;
            while (*p && is_ws((unsigned char)*p)) ++p;
            if (*p == ']') { ++p; break; }
            if (!*p) { entry_free(&tmp); return fail(errbuf, errlen, t->path, line, "%s: the array has no closing ]", dotted); }
            if (*p != '"') { entry_free(&tmp); return fail(errbuf, errlen, t->path, line, "%s: an array may hold only \"strings\"", dotted); }
            if (!(q = close_quote(p + 1))) { entry_free(&tmp); return fail(errbuf, errlen, t->path, line, "%s: a string in the array has no closing quote", dotted); }
            if (!(str = unescape(p + 1, q, &why))) {
                if (why) { entry_free(&tmp); return fail(errbuf, errlen, t->path, line, "%s: %s", dotted, why); }
                goto oom;
            }
            if (tmp.arrn == capn) {
                /* Grow through a TEMPORARY: `arr = realloc(arr, ...)` drops the only
                 * pointer to the old block on failure, leaking it and every string
                 * in it. */
                int nc = capn ? capn * 2 : 4;
                char **na = (char **)gptps_realloc(tmp.arr, (size_t)nc * sizeof *na);
                if (!na) { gptps_free(str); goto oom; }
                tmp.arr = na; capn = nc;
            }
            tmp.arr[tmp.arrn++] = str;
            p = (char *)q + 1;
            while (*p && is_ws((unsigned char)*p)) ++p;
            if (*p == ',') ++p;
            else if (*p != ']') { entry_free(&tmp); return fail(errbuf, errlen, t->path, line, "%s: expected , or ] in the array", dotted); }
        }
        if (*trim(p)) { entry_free(&tmp); return fail(errbuf, errlen, t->path, line, "%s: unexpected text after the array", dotted); }
    } else if (strcmp(val, "true") == 0 || strcmp(val, "false") == 0) {
        tmp.type = TT_BOOL; tmp.b = (val[0] == 't');
        if (!(tmp.text = dupn(val, strlen(val)))) goto oom;
    } else if ((val[0] >= '0' && val[0] <= '9') || val[0] == '-' || val[0] == '+' || val[0] == '.') {
        char *end;                                        /* a number, all of it */
        if (gptps_settings_plain_number(val, 1)) {        /* the grammar a setting takes */
            errno = 0;
            tmp.i = strtoll(val, &end, 10);
            /* strtoll saturates at LLONG_MIN/MAX and says so only via errno, so
             * `max_memory_bytes = 99999999999999999999999` used to install
             * LLONG_MAX - a limit the operator never wrote. Past a signed 64-bit
             * integer, an unsigned one still reads: the settings take byte counts and
             * budgets up to 2^64-1, so a saved one must read back. Its text is what the
             * loader uses; gptps_toml_int, which returns a signed value, says no. */
            if (errno == ERANGE) {
                errno = 0;
                if (val[0] == '-' || (strtoull(val, &end, 10), errno == ERANGE))
                    return fail(errbuf, errlen, t->path, line, "%s: %s is too large a number", dotted, val);
                tmp.big = 1;
            }
            tmp.type = TT_INT;
        } else if (gptps_settings_plain_number(val, 0)) { /* no hex, no inf, no nan */
            tmp.d = gptps_strtod_c(val, &end);
            if (!(tmp.d == tmp.d) || tmp.d > DBL_MAX || tmp.d < -DBL_MAX)
                return fail(errbuf, errlen, t->path, line, "%s: %s is too large a number", dotted, val);
            tmp.type = TT_DBL;
        } else {
            return fail(errbuf, errlen, t->path, line, "%s: %s is not a number", dotted, val);
        }
        if (!(tmp.text = dupn(val, strlen(val)))) goto oom;
    } else {
        return fail(errbuf, errlen, t->path, line,
                    "%s: %s is not a value - a string needs quotes (\"%s\"); otherwise use a number, "
                    "true or false, or a [\"list\"]", dotted, val, val);
    }

    if (!(tmp.section = dupn(section, strlen(section))) || !(tmp.key = dupn(key, strlen(key))) ||
        !(e = push(t)))
        goto oom;
    *e = tmp;
    return 0;

oom:
    entry_free(&tmp);
    return fail(errbuf, errlen, t->path, line, "out of memory");
}

/* A dotted path - a.b, "x.y", a."b c".d - read from *pp: its parts joined by '.',
 * each part bare or "quoted" (any text, with the string escapes), spaces allowed
 * around a dot. A key's bare part holds letters, digits, _ and -. A [table] name's
 * bare part may hold anything but . ] and ", trimmed, so [tasks.resize v2] reads
 * as it always did. Returns the path (gptps_free it) with *pp past it, or NULL
 * with *why saying what is wrong (out of memory when *why is NULL). */
static char *parse_path(const char **pp, int table, const char **why)
{
    const char *p = *pp;
    char *out = NULL;
    size_t n = 0, parts = 0;
    *why = NULL;
    for (;;) {
        const char *seg;
        char *quoted = NULL, *grown;
        size_t sl;
        while (is_ws((unsigned char)*p)) ++p;
        if (*p == '"') {
            const char *q = close_quote(p + 1);
            if (!q) { *why = table ? "a quoted part of the table name has no closing quote"
                                   : "the quoted key has no closing quote"; break; }
            if (!(quoted = unescape(p + 1, q, why))) break;
            seg = quoted; sl = strlen(quoted);
            p = q + 1;
        } else {
            seg = p;
            if (table) while (*p && *p != '.' && *p != ']' && *p != '"') ++p;
            else       while (*p && *p != '.' && is_bare((unsigned char)*p)) ++p;
            sl = (size_t)(p - seg);
            while (sl && is_ws((unsigned char)seg[sl - 1])) --sl;
            if (!sl) {
                *why = table ? (parts || *p == '.' ? "the table name has an empty part - two dots, or a dot at an end"
                                                   : "an empty table name")
                             : (parts || *p == '.' ? "the key has an empty part - two dots, or a dot at an end"
                                                   : NULL);
                if (!*why) *why = *p == '=' ? "missing key before =" : "expected key = value";
                break;
            }
        }
        grown = (char *)gptps_realloc(out, n + sl + 2);
        if (!grown) { gptps_free(quoted); break; }
        out = grown;
        if (parts++) out[n++] = '.';            /* "".a is .a: an empty first part keeps its dot */
        memcpy(out + n, seg, sl);
        n += sl;
        out[n] = 0;
        gptps_free(quoted);
        while (is_ws((unsigned char)*p)) ++p;
        if (*p != '.') { *pp = p; return out; }
        ++p;
    }
    gptps_free(out);
    return NULL;
}

/* One non-blank, comment-stripped line. 0 on success; -1 with errbuf set. */
static int parse_line(struct gptps_toml *t, char *raw, int line, char *section, size_t seccap,
                      char *errbuf, size_t errlen)
{
    const char *p, *why;
    char *rest, *key;
    if (raw[0] == '[') {
        char *name;
        if (raw[1] == '[') return fail(errbuf, errlen, t->path, line, "[[arrays of tables]] are not supported");
        p = raw + 1;
        if (!(name = parse_path(&p, 1, &why))) return fail(errbuf, errlen, t->path, line, "%s", why ? why : "out of memory");
        if (*p != ']') {
            gptps_free(name);
            return fail(errbuf, errlen, t->path, line, *p ? "unexpected text in the table name" : "the table name has no closing ]");
        }
        if (*trim((char *)p + 1)) { gptps_free(name); return fail(errbuf, errlen, t->path, line, "unexpected text after the table name"); }
        if (!*name) {                      /* [""]: a table named "" - which would read as the top level */
            gptps_free(name);
            return fail(errbuf, errlen, t->path, line, "an empty table name");
        }
        if (strlen(name) >= seccap) { gptps_free(name); return fail(errbuf, errlen, t->path, line, "the table name is too long"); }
        if (t->ntb == t->captb) {
            size_t nc = t->captb ? t->captb * 2 : 8;
            toml_table *g = (toml_table *)gptps_realloc(t->tb, nc * sizeof *g);
            if (!g) { gptps_free(name); return fail(errbuf, errlen, t->path, line, "out of memory"); }
            t->tb = g; t->captb = nc;
        }
        t->tb[t->ntb].line = line;
        t->tb[t->ntb].name = name;
        ++t->ntb;
        memcpy(section, name, strlen(name) + 1);
        return 0;
    }
    p = raw;
    if (!(key = parse_path(&p, 0, &why))) return fail(errbuf, errlen, t->path, line, "%s", why ? why : "out of memory");
    rest = trim((char *)p);
    if (*rest != '=') {
        int rc = *rest
            ? fail(errbuf, errlen, t->path, line, "expected = after %s (a bare key holds only letters, digits, _ - "
                   "and .; put any other key in \"quotes\")", key)
            : fail(errbuf, errlen, t->path, line, "%s: expected = and a value", key);
        gptps_free(key);
        return rc;
    }
    {
        int rc = parse_value(t, section, key, rest + 1, line, errbuf, errlen);
        gptps_free(key);
        return rc;
    }
}

/* ---- public-ish (internal) API ---- */

char *gptps_toml_read_file(const char *path, char *errbuf, size_t errlen)
{
    FILE *f;
    long sz;
    size_t got;
    char *buf;

    if (errbuf && errlen) errbuf[0] = 0;
    f = fopen(path, "rb");
    if (!f) { fail(errbuf, errlen, path, 0, "cannot open the file (%s)", strerror(errno)); return NULL; }
    /* Each step below is checked AND fills errbuf. Unchecked, a mistyped path
     * surfaced as gptps_open's E_CONFIG with a blank error string - and for the
     * commonest typo of all, a directory, as an ~8 EiB allocation request (see
     * GPTPS_TOML_MAX_BYTES). */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); fail(errbuf, errlen, path, 0, "cannot size the file"); return NULL; }
    sz = ftell(f);
    if (sz < 0 || (unsigned long)sz > GPTPS_TOML_MAX_BYTES) {
        fclose(f);
        fail(errbuf, errlen, path, 0, "not a readable config file");
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); fail(errbuf, errlen, path, 0, "cannot rewind the file"); return NULL; }
    buf = (char *)gptps_malloc((size_t)sz + 1);
    if (!buf) { fclose(f); fail(errbuf, errlen, path, 0, "out of memory reading the file"); return NULL; }
    /* Terminate at what we actually read, not at what ftell promised: an editor
     * that truncates-and-rewrites the file under a SIGHUP-driven reload would
     * otherwise leave the tail of the buffer uninitialised - and parsed. */
    got = fread(buf, 1, (size_t)sz, f);
    /* A SHORT read is fine (the truncate-and-rewrite case above); a read ERROR is
     * not. This is what makes "the config path is a directory" portable: glibc's
     * ftell on a directory reports LLONG_MAX, so the size check above already
     * rejects it - but on macOS/BSD ftell returns a plausible size, the allocation
     * succeeds, and only the read fails with EISDIR. */
    if (ferror(f)) {
        fclose(f); gptps_free(buf);
        fail(errbuf, errlen, path, 0, "cannot read the file");
        return NULL;
    }
    buf[got] = 0;
    fclose(f);
    /* A NUL inside the text would end the parse there and hide the rest of the
     * file - from the loader, and from a save that rewrites the file. */
    if (memchr(buf, 0, got)) {
        gptps_free(buf);
        fail(errbuf, errlen, path, 0, "the file contains a NUL byte - it is not a text file");
        return NULL;
    }
    return buf;
}

/* One more message for errbuf, on a line of its own; 0 when it no longer fits. */
static int add_error(char *errbuf, size_t errlen, size_t *used, const char *msg)
{
    size_t n = strlen(msg);
    if (!errbuf || !errlen) return 1;
    if (*used + n + 2 > errlen) return 0;
    if (*used) errbuf[(*used)++] = '\n';
    memcpy(errbuf + *used, msg, n + 1);
    *used += n;
    return 1;
}

/* Parse `buf` (NUL-terminated; consumed and freed). A line that does not parse is
 * reported and the parse goes on, so one attempt names every such line: errbuf
 * gets one message per line, as many as fit. After a [table] line that does not
 * parse, the keys up to the next table are passed over - their table is unknown,
 * and judging them in the one before would only report errors that are not there. */
static gptps_toml *parse_buf(const char *path, char *buf, char *errbuf, size_t errlen)
{
    char *line, *save;
    struct gptps_toml *t;
    char section[256], one[600];
    int lineno = 1, lost = 0;
    unsigned bad = 0, shown = 0;
    size_t used = 0;

    if (errbuf && errlen) errbuf[0] = 0;
    {   /* A carriage return ends a line only before a newline (CRLF). A file whose
         * lines end in CR alone reads as ONE line, and when that line starts with a
         * comment, every value in the file is silently ignored. */
        const char *c;
        int ln = 1;
        for (c = buf; *c; ++c) {
            if (*c == '\n') ++ln;
            else if (*c == '\r' && c[1] != '\n' && c[1] != 0) {
                fail(errbuf, errlen, path, ln, "a carriage return inside the line - if the file's lines end in CR "
                     "alone, save it with LF or CRLF line endings");
                gptps_free(buf);
                return NULL;
            }
        }
    }
    t = (struct gptps_toml *)gptps_calloc(1, sizeof *t);
    if (!t || !(t->path = dupn(path, strlen(path)))) {
        gptps_free(t); gptps_free(buf);
        fail(errbuf, errlen, path, 0, "out of memory");
        return NULL;
    }
    section[0] = 0;

    for (line = buf, save = buf; ; ++save) {
        if (*save == '\n' || *save == 0) {
            char eol = *save;
            char *raw;
            *save = 0;
            raw = trim(line);
            strip_comment(raw);
            raw = trim(raw);
            if (*raw && (!lost || raw[0] == '[')) {
                int header = raw[0] == '[';
                if (parse_line(t, raw, lineno, section, sizeof section, one, sizeof one) != 0) {
                    ++bad;   /* room is kept for the line that counts the rest */
                    if (errlen > 320 && add_error(errbuf, errlen - 320, &used, one)) ++shown;
                    lost = header;
                } else if (header) {
                    lost = 0;
                }
            }
            if (eol == 0) break;
            line = save + 1;
            ++lineno;
        }
    }
    gptps_free(buf);
    if (bad) {
        if (shown < bad) {
            fail(one, sizeof one, path, 0, "and %u more line%s that do%s not parse",
                 bad - shown, bad - shown == 1 ? "" : "s", bad - shown == 1 ? "es" : "");
            add_error(errbuf, errlen, &used, one);
        }
        gptps_toml_free(t);
        return NULL;
    }
    return t;
}

gptps_toml *gptps_toml_parse_file(const char *path, char *errbuf, size_t errlen)
{
    char *buf = gptps_toml_read_file(path, errbuf, errlen);
    return buf ? parse_buf(path, buf, errbuf, errlen) : NULL;
}

gptps_toml *gptps_toml_parse_text(const char *path, const char *text, char *errbuf, size_t errlen)
{
    char *buf = dupn(text, strlen(text));
    if (!buf) { fail(errbuf, errlen, path, 0, "out of memory"); return NULL; }
    return parse_buf(path, buf, errbuf, errlen);
}

void gptps_toml_free(gptps_toml *t)
{
    size_t k;
    if (!t) return;
    for (k = 0; k < t->n; ++k) entry_free(&t->e[k]);
    for (k = 0; k < t->ntb; ++k) gptps_free(t->tb[k].name);
    gptps_free(t->e); gptps_free(t->tb); gptps_free(t->path); gptps_free(t);
}

int gptps_toml_int(const gptps_toml *t, const char *section, const char *key, long long *out)
{
    const toml_entry *e = t ? find(t, section, key) : NULL;
    if (!e || e->type != TT_INT || e->big) return 0;
    *out = e->i;
    return 1;
}
int gptps_toml_double(const gptps_toml *t, const char *section, const char *key, double *out)
{
    const toml_entry *e = t ? find(t, section, key) : NULL;
    if (!e) return 0;
    if (e->type == TT_DBL) { *out = e->d; return 1; }
    if (e->type == TT_INT) { *out = e->big ? gptps_strtod_c(e->text, NULL) : (double)e->i; return 1; }
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
    return (e && e->type == TT_STR) ? e->text : NULL;
}
int gptps_toml_str_array(const gptps_toml *t, const char *section, const char *key, const char *const **out)
{
    const toml_entry *e = t ? find(t, section, key) : NULL;
    if (!e || e->type != TT_ARR || !e->arr) { *out = NULL; return 0; }
    *out = (const char *const *)e->arr;
    return e->arrn;
}

/* ---- entry-wise access, for the loader (see docs/CONFIG.md) ---- */

const char *gptps_toml_path(const gptps_toml *t)               { return t ? t->path : ""; }
size_t      gptps_toml_count(const gptps_toml *t)              { return t ? t->n : 0; }
const char *gptps_toml_section_at(const gptps_toml *t, size_t i) { return t->e[i].section; }
const char *gptps_toml_key_at(const gptps_toml *t, size_t i)   { return t->e[i].key; }
const char *gptps_toml_text_at(const gptps_toml *t, size_t i)  { return t->e[i].text; }
int         gptps_toml_line_at(const gptps_toml *t, size_t i)  { return t->e[i].line; }
int         gptps_toml_claimed_at(const gptps_toml *t, size_t i) { return t->e[i].claimed; }
size_t      gptps_toml_table_count(const gptps_toml *t)        { return t ? t->ntb : 0; }
gptps_toml_kind gptps_toml_kind_at(const gptps_toml *t, size_t i)
{
    switch (t->e[i].type) {
        case TT_INT:  return GPTPS_TOML_INT;
        case TT_DBL:  return GPTPS_TOML_FLOAT;
        case TT_BOOL: return GPTPS_TOML_BOOL;
        case TT_STR:  return GPTPS_TOML_STRING;
        default:      return GPTPS_TOML_ARRAY;
    }
}
int         gptps_toml_table_line_at(const gptps_toml *t, size_t i) { return t->tb[i].line; }
const char *gptps_toml_table_at(const gptps_toml *t, size_t i) { return t->tb[i].name; }
void        gptps_toml_claim_at(gptps_toml *t, size_t i)       { t->e[i].claimed = 1; }

void gptps_toml_dotted_at(const gptps_toml *t, size_t i, char *buf, size_t cap)
{
    const toml_entry *e = &t->e[i];
    if (*e->section) snprintf(buf, cap, "%s.%s", e->section, e->key);
    else             snprintf(buf, cap, "%s", e->key);
}

/* A dotted settings key matches section + "." + key wherever the dots fall, so
 * [tasks.render.gpuq] units = 2 and [tasks.render] "gpuq.units" = 2 are both
 * tasks.render.gpuq.units. -1 when no entry is that key. */
long gptps_toml_find_dotted(const gptps_toml *t, const char *dotted)
{
    size_t k, dl;
    if (!t || !dotted) return -1;
    dl = strlen(dotted);
    for (k = 0; k < t->n; ++k) {
        const toml_entry *e = &t->e[k];
        size_t sl = strlen(e->section), kl = strlen(e->key);
        if (sl == 0) { if (kl == dl && memcmp(e->key, dotted, dl) == 0) return (long)k; continue; }
        if (sl + 1 + kl == dl && memcmp(e->section, dotted, sl) == 0 && dotted[sl] == '.' &&
            memcmp(e->key, dotted + sl + 1, kl) == 0)
            return (long)k;
    }
    return -1;
}
