/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * settings.c - the generic, typed settings registry (Phase 1 of the settings
 * subsystem). It is SCHEMA + ACCESSOR BINDING only: each entry stores metadata
 * plus a target pointer and read/write callbacks. The live engine / add-on state
 * remains the single source of truth, so displayed values never drift. This TU
 * never sees `struct gptps` or any add-on layout - it only ever calls
 * entry->read(target, ...) / entry->write(target, value).
 *
 * Validation (range / enum / type-parseable) happens HERE, before write() is
 * called - the validation that the raw TOML path lacks.
 */
#include "gptps.h"
#include "gptps_hal.h"
#include "gptps_internal.h"

#include <errno.h>
#include <float.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct gptps_setting_entry {
    char                *key;       /* owned */
    char                *desc;      /* owned */
    char                *defval;    /* owned: value rendered at registration */
    gptps_setting_type   type;
    int                  hot, has_range;
    double               min, max;
    const char *const   *choices;   /* borrowed (must be static / outlive engine) */
    void                *target;
    const void          *owner;     /* reg whose unregister removes it, or NULL */
    size_t             (*read)(void *, char *, size_t);
    gptps_status       (*write)(void *, const char *);
    int                  mark;      /* transient, used while serializing */
    const void          *tag;       /* the add-on load that registered it, while one runs */
    int                  dirty;     /* last set live (gptps_settings_set), not by a file */
    int                  nosave;    /* a statistic, not configuration: save leaves it out */
    struct gptps_setting_entry *next;
} gptps_setting_entry;

typedef struct gptps_setting_watcher {
    gptps_settings_cb            cb;
    void                        *ud;
    int                          files;  /* an add-on's: hears a config file's values too */
    const void                  *tag;    /* the add-on load that registered it, while one runs */
    uint32_t                     off;    /* its add-on's setup failed (acquire/release: it is
                                          * read by notifiers without the lock) */
    struct gptps_setting_watcher *next;  /* set once, at creation: notifiers walk it unlocked */
} gptps_setting_watcher;

struct gptps_settings {
    gptps_mutex           *m;
    gptps_setting_entry   *head, *tail;
    size_t                 n;
    gptps_setting_watcher *watchers;
};

static char *dupz(const char *s) { size_t n = strlen(s) + 1; char *o = (char *)gptps_malloc(n); if (o) memcpy(o, s, n); return o; }

gptps_settings *gptps_settings_create(void)
{
    gptps_settings *r = (gptps_settings *)gptps_calloc(1, sizeof *r);
    if (!r) return NULL;
    r->m = gptps_mutex_create();
    if (!r->m) { gptps_free(r); return NULL; }
    return r;
}

void gptps_settings_destroy(gptps_settings *r)
{
    gptps_setting_entry *e;
    if (!r) return;
    e = r->head;
    while (e) { gptps_setting_entry *n = e->next; gptps_free(e->key); gptps_free(e->desc); gptps_free(e->defval); gptps_free(e); e = n; }
    { gptps_setting_watcher *w = r->watchers; while (w) { gptps_setting_watcher *n = w->next; gptps_free(w); w = n; } }
    gptps_mutex_destroy(r->m);
    gptps_free(r);
}

static gptps_setting_entry *setting_find(gptps_settings *r, const char *key);

gptps_status gptps_settings_watch_add(gptps_settings *r, gptps_settings_cb cb, void *ud, int files,
                                      const void *tag)
{
    gptps_setting_watcher *w;
    if (!r || !cb) return GPTPS_E_INVAL;
    w = (gptps_setting_watcher *)gptps_calloc(1, sizeof *w);
    if (!w) return GPTPS_E_NOMEM;
    w->cb = cb; w->ud = ud; w->files = files; w->tag = tag;
    gptps_mutex_lock(r->m); w->next = r->watchers; r->watchers = w; gptps_mutex_unlock(r->m);
    return GPTPS_OK;
}

/* A failed add-on load: its watchers hear nothing more, and its settings go - their
 * accessors and targets are the add-on's, set up by a setup that did not finish.
 * By the load's tag, so another thread's watcher or setting registered meanwhile is
 * left alone. A watcher is switched off, not freed: notifiers walk the list without
 * the lock. (One already past its `off` check may still be calling it.) */
void gptps_settings_forget_tag(gptps_settings *r, const void *tag)
{
    gptps_setting_watcher *w;
    gptps_setting_entry *e, *prev = NULL;
    if (!r || !tag) return;
    gptps_mutex_lock(r->m);
    for (w = r->watchers; w; w = w->next)
        if (w->tag == tag) gptps_hal_store_release_u32(&w->off, 1);
    e = r->head;
    while (e) {
        gptps_setting_entry *next = e->next;
        if (e->tag == tag) {
            if (prev) prev->next = next; else r->head = next;
            if (r->tail == e) r->tail = prev;
            gptps_free(e->key); gptps_free(e->desc); gptps_free(e->defval); gptps_free(e);
            r->n -= 1;
        } else {
            prev = e;
        }
        e = next;
    }
    gptps_mutex_unlock(r->m);
}

/* The add-on load registering `key` now (gptps_register_setting, inside a setup). */
void gptps_settings_set_tag(gptps_settings *r, const char *key, const void *tag)
{
    gptps_setting_entry *e;
    if (!r || !key) return;
    gptps_mutex_lock(r->m);
    if ((e = setting_find(r, key)) != NULL) e->tag = tag;
    gptps_mutex_unlock(r->m);
}


/* caller holds r->m */
static gptps_setting_entry *setting_find(gptps_settings *r, const char *key)
{
    gptps_setting_entry *e;
    for (e = r->head; e; e = e->next) if (strcmp(e->key, key) == 0) return e;
    return NULL;
}


gptps_status gptps_settings_add(gptps_settings *r, const gptps_setting_def *def)
{
    return gptps_settings_add_owned(r, def, NULL);
}

gptps_status gptps_settings_add_owned(gptps_settings *r, const gptps_setting_def *def,
                                      const void *owner)
{
    gptps_setting_entry *e;
    if (!r || !def || !def->key || !def->read || !def->write) return GPTPS_E_INVAL;
    gptps_mutex_lock(r->m);
    if (setting_find(r, def->key)) { gptps_mutex_unlock(r->m); return GPTPS_E_DUP; }
    e = (gptps_setting_entry *)gptps_calloc(1, sizeof *e);
    if (e) { e->key = dupz(def->key); e->desc = dupz(def->desc ? def->desc : ""); e->defval = (char *)gptps_malloc(GPTPS_SETTINGS_VALUE_MAX); }
    if (!e || !e->key || !e->desc || !e->defval) {
        if (e) { gptps_free(e->key); gptps_free(e->desc); gptps_free(e->defval); gptps_free(e); }
        gptps_mutex_unlock(r->m);
        return GPTPS_E_NOMEM;
    }
    e->type = def->type; e->hot = def->hot; e->has_range = def->has_range;
    e->min = def->min; e->max = def->max; e->choices = def->choices;
    e->target = def->target; e->read = def->read; e->write = def->write;
    e->owner = owner;
    e->defval[0] = 0;
    e->read(e->target, e->defval, GPTPS_SETTINGS_VALUE_MAX);   /* snapshot the default */
    if (r->tail) r->tail->next = e; else r->head = e;
    r->tail = e; r->n += 1;
    gptps_mutex_unlock(r->m);
    return GPTPS_OK;
}

size_t gptps_settings_size(gptps_settings *r)
{
    size_t n;
    if (!r) return 0;
    gptps_mutex_lock(r->m); n = r->n; gptps_mutex_unlock(r->m);
    return n;
}

static int has_prefix(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

size_t gptps_settings_remove_task(gptps_settings *r, const void *owner, const char *prefix,
                                  const char *const *keep, size_t nkeep)
{
    gptps_setting_entry *e, *prev = NULL;
    size_t removed = 0, k;
    if (!r || !owner) return 0;
    gptps_mutex_lock(r->m);
    e = r->head;
    while (e) {
        gptps_setting_entry *next = e->next;
        int drop = (e->owner == owner);
        if (!drop && !e->owner && prefix && has_prefix(e->key, prefix)) {
            drop = 1;                       /* a host's key under this type's prefix */
            for (k = 0; k < nkeep && drop; ++k)
                if (has_prefix(e->key, keep[k])) drop = 0;   /* ...or a live sibling's */
        }
        if (drop) {
            if (prev) prev->next = next; else r->head = next;
            if (r->tail == e) r->tail = prev;
            gptps_free(e->key); gptps_free(e->desc); gptps_free(e->defval); gptps_free(e);
            r->n -= 1; ++removed;
            e = next;        /* prev unchanged */
        } else {
            prev = e; e = next;
        }
    }
    gptps_mutex_unlock(r->m);
    return removed;
}

/* parse + range/enum check; 0 = invalid, 1 = ok */
/* A number as a config file writes it: a sign, digits, and for one that need not be
 * whole a fraction and an exponent - no spaces, no hex, no inf or nan. Every number
 * a setting takes is one of these, so whatever a save writes reads back the same
 * (docs/CONFIG.md). The parser holds the file to the same grammar. */
int gptps_settings_plain_number(const char *v, int whole)
{
    const char *p = v;
    int digits = 0;
    if (!p) return 0;
    if (*p == '+' || *p == '-') ++p;
    while (*p >= '0' && *p <= '9') { ++p; ++digits; }
    if (!whole && *p == '.') { ++p; while (*p >= '0' && *p <= '9') { ++p; ++digits; } }
    if (!digits) return 0;
    if (!whole && (*p == 'e' || *p == 'E')) {
        int ed = 0;
        ++p;
        if (*p == '+' || *p == '-') ++p;
        while (*p >= '0' && *p <= '9') { ++p; ++ed; }
        if (!ed) return 0;
    }
    return *p == 0;
}

/* Numbers as the C locale reads and writes them, whatever LC_NUMERIC the host set
 * (a GUI toolkit's init usually sets it from the environment): a config file says
 * 1.5, where a German locale's strtod stops at the '.' and its printf writes 1,5. */
double gptps_strtod_c(const char *s, char **end)
{
    const char *dp = localeconv()->decimal_point;
    char buf[2 * GPTPS_SETTINGS_VALUE_MAX], *e2;
    size_t dl, n = strlen(s), i, w = 0, dot = (size_t)-1, used;
    double v;
    if (!dp || !*dp || !strcmp(dp, ".") || n + 16 >= sizeof buf) return strtod(s, end);
    dl = strlen(dp);
    for (i = 0; i < n; ++i) {
        if (s[i] == '.' && dot == (size_t)-1) { memcpy(buf + w, dp, dl); w += dl; dot = i; }
        else buf[w++] = s[i];
    }
    buf[w] = 0;
    v = strtod(buf, &e2);
    used = (size_t)(e2 - buf);
    if (dot != (size_t)-1 && used > dot) used -= dl - 1;
    if (end) *end = (char *)s + used;
    return v;
}

void gptps_fmt_double_c(char *buf, size_t cap, double v)
{
    const char *dp = localeconv()->decimal_point;
    char *at;
    int p;
    if (v > -1e15 && v < 1e15 && (double)(long long)v == v) {
        snprintf(buf, cap, "%.0f", v);     /* a whole number as one: 10, not 1e+01 */
    } else {
        for (p = 1; p <= 17; ++p) {        /* the shortest that reads back: 0.1, not 0.10000000000000001 */
            snprintf(buf, cap, "%.*g", p, v);
            if (strtod(buf, NULL) == v) break;   /* in the locale it was written in */
        }
    }
    if (dp && *dp && strcmp(dp, ".") && (at = strstr(buf, dp)) != NULL) {
        size_t dl = strlen(dp);
        *at = '.';
        memmove(at + 1, at + dl, strlen(at + dl) + 1);
    }
}

static int valid_value(const gptps_setting_entry *e, const char *v)
{
    char *end;
    if ((e->type == GPTPS_SETTING_INT || e->type == GPTPS_SETTING_UINT || e->type == GPTPS_SETTING_DOUBLE) &&
        !gptps_settings_plain_number(v, e->type != GPTPS_SETTING_DOUBLE))
        return 0;
    switch (e->type) {
        case GPTPS_SETTING_INT: {
            long long x;
            errno = 0;
            x = strtoll(v, &end, 10);
            if (end == v) return 0;
            /* strtoll SATURATES at LLONG_MIN/MAX instead of failing, so without the
             * ERANGE check a nonsense value was accepted and silently clamped to a
             * number the operator never asked for. A limit that cannot be honoured
             * has to be refused, not guessed at. */
            if (errno == ERANGE) return 0;
            while (*end == ' ' || *end == '\t') ++end;
            if (*end) return 0;
            if (e->has_range && ((double)x < e->min || (double)x > e->max)) return 0;
            return 1;
        }
        case GPTPS_SETTING_UINT: {
            unsigned long long x;
            const char *p = v; while (*p == ' ' || *p == '\t') ++p;
            if (*p == '-') return 0;
            errno = 0;
            x = strtoull(v, &end, 10);
            if (end == v) return 0;
            if (errno == ERANGE) return 0;   /* saturated to ULLONG_MAX; see above */
            while (*end == ' ' || *end == '\t') ++end;
            if (*end) return 0;
            if (e->has_range && ((double)x < e->min || (double)x > e->max)) return 0;
            return 1;
        }
        case GPTPS_SETTING_DOUBLE: {
            double x = gptps_strtod_c(v, &end);
            if (end == v || *end) return 0;
            if (!(x == x) || x > DBL_MAX || x < -DBL_MAX) return 0;     /* 1e999 is inf */
            if (e->has_range && (x < e->min || x > e->max)) return 0;
            return 1;
        }
        case GPTPS_SETTING_BOOL:
            return strcmp(v, "true") == 0 || strcmp(v, "false") == 0;
        case GPTPS_SETTING_ENUM: {
            const char *const *c;
            if (!e->choices) return 0;
            for (c = e->choices; *c; ++c) if (strcmp(*c, v) == 0) return 1;
            return 0;
        }
        case GPTPS_SETTING_STRING:
            return strlen(v) < GPTPS_SETTINGS_VALUE_MAX;
    }
    return 0;
}

/* Why a value of this shape was refused, in the operator's terms. Shared by the
 * live path and the config loader, so both say the same thing. */
void gptps_settings_explain(gptps_setting_type type, int has_range, double min, double max,
                            const char *const *choices, const char *v, char *why, size_t cap)
{
    char range[96];
    range[0] = 0;
    if (!why || !cap) return;
    if (has_range) {
        char a[40], b[40];
        gptps_fmt_double_c(a, sizeof a, min);
        gptps_fmt_double_c(b, sizeof b, max);
        snprintf(range, sizeof range, " between %s and %s", a, b);
    }
    switch (type) {
        case GPTPS_SETTING_INT:    snprintf(why, cap, "%s must be a whole number%s", v, range); break;
        case GPTPS_SETTING_UINT:   snprintf(why, cap, "%s must be a whole number%s%s", v, *range ? "" : " of 0 or more", range); break;
        case GPTPS_SETTING_DOUBLE: snprintf(why, cap, "%s must be a number%s", v, range); break;
        case GPTPS_SETTING_BOOL:   snprintf(why, cap, "%s must be true or false", v); break;
        case GPTPS_SETTING_STRING: snprintf(why, cap, "the value is longer than %d characters", GPTPS_SETTINGS_VALUE_MAX - 1); break;
        case GPTPS_SETTING_ENUM: {
            const char *const *c;
            size_t k = (size_t)snprintf(why, cap, "%s must be one of:", v);
            for (c = choices; c && *c && k < cap; ++c)
                k += (size_t)snprintf(why + k, cap - k, "%s %s", c == choices ? "" : ",", *c);
            break;
        }
    }
}

static void invalid_reason(const gptps_setting_entry *e, const char *v, char *why, size_t cap)
{
    gptps_settings_explain(e->type, e->has_range, e->min, e->max, e->choices, v, why, cap);
}

/* The type of the setting `key`: 1 and *out, or 0 when there is no such setting. */
int gptps_settings_type_of(gptps_settings *r, const char *key, gptps_setting_type *out)
{
    gptps_setting_entry *e;
    if (!r || !key) return 0;
    gptps_mutex_lock(r->m);
    if ((e = setting_find(r, key)) != NULL && out) *out = e->type;
    gptps_mutex_unlock(r->m);
    return e != NULL;
}

int gptps_settings_has(gptps_settings *r, const char *key)
{
    int has;
    if (!r || !key) return 0;
    gptps_mutex_lock(r->m);
    has = setting_find(r, key) != NULL;
    gptps_mutex_unlock(r->m);
    return has;
}

/* Edit distance (insert, delete, substitute), giving up past `cap`: a typo is a
 * near miss, and anything further is not worth suggesting. */
size_t gptps_edit_distance(const char *a, const char *b, size_t cap)
{
    size_t la = strlen(a), lb = strlen(b), i, j, row[257];
    if (la > 256 || lb > 256) return cap + 1;
    if ((la > lb ? la - lb : lb - la) > cap) return cap + 1;
    for (j = 0; j <= lb; ++j) row[j] = j;
    for (i = 1; i <= la; ++i) {
        size_t diag = row[0], best;
        row[0] = i; best = row[0];
        for (j = 1; j <= lb; ++j) {
            size_t up = row[j], v = diag + (a[i - 1] != b[j - 1]);
            if (row[j - 1] + 1 < v) v = row[j - 1] + 1;
            if (up + 1 < v) v = up + 1;
            diag = up; row[j] = v;
            if (v < best) best = v;
        }
        if (best > cap) return cap + 1;
    }
    return row[lb];
}

/* The registered key nearest `key`, if it is a near miss: a typo's likely intent.
 * 1 and `buf` filled, or 0. */
int gptps_settings_closest(gptps_settings *r, const char *key, char *buf, size_t cap)
{
    gptps_setting_entry *e;
    size_t best = (size_t)-1, limit = strlen(key) / 4 < 2 ? 2 : strlen(key) / 4;
    if (!r || !key || !buf || !cap) return 0;
    gptps_mutex_lock(r->m);
    for (e = r->head; e; e = e->next) {
        size_t d = gptps_edit_distance(key, e->key, limit);
        if (d <= limit && d < best) { best = d; snprintf(buf, cap, "%s", e->key); }
    }
    gptps_mutex_unlock(r->m);
    return best != (size_t)-1;
}

gptps_status gptps_settings_get_by(gptps_settings *r, const char *key, char *buf, size_t cap)
{
    gptps_setting_entry *e;
    if (!r || !key || !buf || cap == 0) return GPTPS_E_INVAL;
    gptps_mutex_lock(r->m);
    e = setting_find(r, key);
    if (e) e->read(e->target, buf, cap);
    gptps_mutex_unlock(r->m);
    return e ? GPTPS_OK : GPTPS_E_NOTFOUND;
}

/* validate + apply one entry; caller holds r->m */
static gptps_status apply_entry(gptps_setting_entry *e, const char *value, char *why, size_t whylen)
{
    gptps_status st;
    if (!valid_value(e, value)) {
        if (why && whylen) invalid_reason(e, value, why, whylen);
        return GPTPS_E_CONFIG;
    }
    st = e->write(e->target, value);     /* write_fn takes the target's own lock */
    if (st != GPTPS_OK && why && whylen) snprintf(why, whylen, "%s was refused (%s)", value, gptps_strerror(st));
    return st;
}

/* The one path a value takes, from a file or live (docs/CONFIG.md). `live` records
 * which, for save: a value set live is the operator's change, one a file set is
 * already in a file. */
static gptps_status set_value(gptps_settings *r, const char *key, const char *value,
                              char *why, size_t whylen, int live)
{
    gptps_setting_entry *e;
    gptps_setting_watcher *head = NULL;
    gptps_status st;
    char applied[GPTPS_SETTINGS_VALUE_MAX], typed[GPTPS_SETTINGS_VALUE_MAX];
    int ok = 0;
    if (why && whylen) why[0] = 0;
    if (!r || !key || !value) return GPTPS_E_INVAL;
    gptps_mutex_lock(r->m);
    e = setting_find(r, key);
    if (!e) { gptps_mutex_unlock(r->m); return GPTPS_E_NOTFOUND; }
    if (live && e->type != GPTPS_SETTING_STRING && strlen(value) < sizeof typed) {
        /* What a person types into a dashboard, or a form posts, may carry white
         * space at either end; for anything but a string it is not part of the
         * value. A file's value comes from the parser, which has trimmed it. */
        size_t n;
        while (*value && strchr(" \t\r\n\v\f", *value)) ++value;
        n = strlen(value);
        while (n && strchr(" \t\r\n\v\f", value[n - 1])) --n;
        memcpy(typed, value, n); typed[n] = 0;
        value = typed;
    }
    st = apply_entry(e, value, why, whylen);
    if (st == GPTPS_OK) { applied[0] = 0; e->read(e->target, applied, sizeof applied); ok = 1; e->dirty = live; }
    head = r->watchers;                 /* the list as it is now: watchers are prepended, and a */
    gptps_mutex_unlock(r->m);           /* node's next never changes, so this walks unlocked */
    /* A host's watchers hear the live sets, as they always have: one that saves on a
     * change must not save once per key a reload reads, nor write a stale live value
     * over the file being read. An add-on's watchers hear the config file's values
     * too - that is how a plug-in learns its configuration (gptps.h). */
    if (ok) {   /* notify watchers with the lock RELEASED (register-before-use; may re-enter get) */
        gptps_setting_watcher *w;
        for (w = head; w; w = w->next)
            if ((live || w->files) && !gptps_hal_load_acquire_u32(&w->off)) w->cb(key, applied, w->ud);
    }
    return st;
}

gptps_status gptps_settings_set_by(gptps_settings *r, const char *key, const char *value)
{
    return set_value(r, key, value, NULL, 0, 1);
}

gptps_status gptps_settings_set_live(gptps_settings *r, const char *key, const char *value,
                                     char *why, size_t whylen)
{
    return set_value(r, key, value, why, whylen, 1);
}

gptps_status gptps_settings_set_text(gptps_settings *r, const char *key, const char *value,
                                     char *why, size_t whylen)
{
    return set_value(r, key, value, why, whylen, 0);
}

void gptps_settings_nosave(gptps_settings *r, const char *key)
{
    gptps_setting_entry *e;
    if (!r || !key) return;
    gptps_mutex_lock(r->m);
    if ((e = setting_find(r, key)) != NULL) e->nosave = 1;
    gptps_mutex_unlock(r->m);
}

gptps_status gptps_settings_info_at(gptps_settings *r, size_t index, gptps_setting_info *out)
{
    gptps_setting_entry *e;
    size_t i = 0;
    if (!r || !out) return GPTPS_E_INVAL;
    if (out->struct_size < sizeof *out) return GPTPS_E_INVAL;
    gptps_mutex_lock(r->m);
    for (e = r->head; e && i < index; e = e->next) ++i;
    if (!e) { gptps_mutex_unlock(r->m); return GPTPS_E_NOTFOUND; }
    out->key = e->key; out->type = e->type; out->desc = e->desc;
    out->hot = e->hot; out->has_range = e->has_range; out->min = e->min; out->max = e->max;
    out->choices = e->choices;
    out->value[0] = 0;
    e->read(e->target, out->value, sizeof out->value);
    memcpy(out->defval, e->defval, sizeof out->defval);
    out->defval[sizeof out->defval - 1] = 0;
    gptps_mutex_unlock(r->m);
    return GPTPS_OK;
}

/* ---- TOML serialization (save) ----
 * gptps_settings_save writes the settings an operator changed: the values set live,
 * through gptps_settings_set. A file that exists is updated in place. A changed value
 * is rewritten on its own line, which keeps the line's key, spacing and comment; a
 * changed setting the file lacks is added next to its siblings; every other line -
 * comments, blank lines, order, keys that are not settings, values nobody changed -
 * is copied as it was. So a "0 = auto" stays auto, and a file keeps saying what its
 * author wrote. The file is parsed first, with the loader's own parser; one that does
 * not parse is not touched, since it is someone's hand-written work with a mistake in
 * it. A new file gets the changed values and those the loaded config file set. */

/* leaf = after the last '.'; *seclen = bytes of the section prefix (0 if none) */
static const char *leaf_of(const char *key, size_t *seclen)
{
    const char *dot = strrchr(key, '.');
    if (!dot) { *seclen = 0; return key; }
    *seclen = (size_t)(dot - key);
    return dot + 1;
}

static int save_ws(int c)   { return c == ' ' || c == '\t' || c == '\r'; }
static int save_bare(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                     c == '_' || c == '-' || c == '.'; }

/* A string as a TOML string: quoted, with " and \ escaped and every control
 * character too, so it reads back byte for byte and stays on its line. */
static void put_quoted(FILE *f, const char *s, size_t n)
{
    size_t i;
    fputc('"', f);
    for (i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"': case '\\': fputc('\\', f); fputc(c, f); break;
            case '\n': fputs("\\n", f); break;
            case '\t': fputs("\\t", f); break;
            case '\r': fputs("\\r", f); break;
            default:
                if (c < 0x20 || c == 0x7f) fprintf(f, "\\u%04X", (unsigned)c);
                else fputc(c, f);
        }
    }
    fputc('"', f);
}

/* Whether `s` (n bytes) can be written bare as a dotted path: every part between
 * dots non-empty and of bare characters only. */
static int bare_path(const char *s, size_t n)
{
    size_t i;
    if (n == 0 || s[0] == '.' || s[n - 1] == '.') return 0;
    for (i = 0; i < n; ++i) {
        if (!save_bare((unsigned char)s[i])) return 0;
        if (s[i] == '.' && s[i + 1] == '.') return 0;
    }
    return 1;
}

/* A key: bare when it can be, quoted otherwise. */
static void put_key(FILE *f, const char *k)
{
    size_t n = strlen(k);
    if (bare_path(k, n)) fputs(k, f);
    else put_quoted(f, k, n);
}

/* A [table] line for the name's first n bytes: each part between dots bare when it
 * can be, quoted when not - [tasks."resize v2"]. */
static void put_table(FILE *f, const char *name, size_t n)
{
    size_t i = 0;
    fputc('[', f);
    for (;;) {
        size_t j = i;
        while (j < n && name[j] != '.') ++j;
        if (bare_path(name + i, j - i)) fwrite(name + i, 1, j - i, f);
        else put_quoted(f, name + i, j - i);
        if (j >= n) break;
        fputc('.', f);
        i = j + 1;
    }
    fputs("]\n", f);
}

/* A setting's current value: strings and enums quoted, the rest as they render. */
static void put_value(FILE *f, gptps_setting_entry *e)
{
    char val[GPTPS_SETTINGS_VALUE_MAX];
    val[0] = 0;
    e->read(e->target, val, sizeof val);
    if (e->type == GPTPS_SETTING_STRING || e->type == GPTPS_SETTING_ENUM) { put_quoted(f, val, strlen(val)); return; }
    fputs(val, f);
    if (e->type == GPTPS_SETTING_DOUBLE && gptps_settings_plain_number(val, 1)) {
        /* 1e30 written out as digits: an integer past what the parser reads (64
         * bits), so it is written as the float it is */
        int over;
        errno = 0;
        if (val[0] == '-') { (void)strtoll(val, NULL, 10); over = errno == ERANGE; }
        else               { (void)strtoull(val, NULL, 10); over = errno == ERANGE; }
        if (over) fputs(".0", f);
    }
}

/* limits.max_memory_gb is the file's other spelling of limits.max_memory_bytes
 * (engine.c, cfg_open_keys); a file that uses it keeps using it - while GiB can
 * say the value exactly. A count of bytes is exact as a double below 2^53, and so
 * is that count over 2^30; past it the line is rewritten in bytes. */
#define SAVE_GB_KEY    "limits.max_memory_gb"
#define SAVE_BYTES_KEY "limits.max_memory_bytes"
#define SAVE_GB_EXACT  9007199254740992ULL      /* 2^53 */
static unsigned long long setting_ull(gptps_setting_entry *e)
{
    char val[GPTPS_SETTINGS_VALUE_MAX];
    val[0] = 0;
    e->read(e->target, val, sizeof val);
    return strtoull(val, NULL, 10);
}

typedef struct {
    const char *start;          /* the line, in the file's text */
    size_t      len;            /* without its '\n' */
    long        header;         /* a [table] line: its index in the parser's tables; else -1 */
    int         comment;        /* a line with only a comment */
    long        entry;          /* key = value: its index in the parsed file; else -1 */
    size_t      block;          /* 0 before the first header; each header starts the next */
} save_line;

typedef struct {
    size_t      first_line;     /* its header, or 0 */
    long        last_key;       /* its last key line; -1 if it has none */
    char        section[256];
} save_block;

typedef struct {
    gptps_setting_entry *e;
    long                 after; /* written after this line (-1: before the first) */
    int                  at_end;/* or in a new section at the end */
    size_t               skip;  /* bytes of e->key the written key leaves out */
    int                  file;  /* write `val`, the loaded file's value, not the current one */
    char                 val[GPTPS_SETTINGS_VALUE_MAX];
} save_add;

/* The value's span in a key = value line: after '=' and its spaces, before a
 * trailing comment and the spaces before it. Quotes and escapes are read as the
 * parser reads them: an '=' in a quoted part of the key, or a '#' inside a string,
 * is not where they end. */
static void value_span(const char *l, size_t len, size_t *vs, size_t *vl)
{
    size_t i, end;
    int in_str = 0;
    for (i = 0; i < len; ++i) {                         /* the key: to the first '=' outside quotes */
        if (in_str) { if (l[i] == '\\' && i + 1 < len) ++i; else if (l[i] == '"') in_str = 0; continue; }
        if (l[i] == '"') in_str = 1;
        else if (l[i] == '=') break;
    }
    in_str = 0;
    for (++i; i < len && save_ws((unsigned char)l[i]); ++i) { }
    *vs = i;
    for (end = i; end < len; ++end) {
        if (in_str && l[end] == '\\' && end + 1 < len) { ++end; continue; }
        if (l[end] == '"') in_str = !in_str;
        else if (l[end] == '#' && !in_str) break;
    }
    while (end > i && save_ws((unsigned char)l[end - 1])) --end;
    *vl = end - i;
}

/* Whether a save writes this setting where the file does not have it: one set live,
 * with its current value; or, in a new file, one the loaded config file set, with
 * the value that file gives it - not what it resolved to here, so a "0 = auto"
 * stays auto - unless that value is not one the setting takes. */
static int save_wants(gptps_setting_entry *e, const gptps_toml *t, gptps_settings_in_file_fn in_file, void *ud,
                      save_add *A)
{
    A->file = 0;
    if (e->nosave) return 0;
    if (e->dirty) return 1;
    if (t || !in_file) return 0;
    A->val[0] = 0;
    if (!in_file(e->key, ud, A->val, sizeof A->val)) return 0;
    A->file = valid_value(e, A->val);
    return 1;
}

static void put_add(FILE *f, const save_add *A)
{
    put_key(f, A->e->key + A->skip);
    fputs(" = ", f);
    if (!A->file) put_value(f, A->e);
    else if (A->e->type == GPTPS_SETTING_STRING || A->e->type == GPTPS_SETTING_ENUM) put_quoted(f, A->val, strlen(A->val));
    else fputs(A->val, f);
    fputc('\n', f);
}

/* `text` (`t` parsed; NULL for a new file) with the changed settings, to `f`.
 * Caller holds r->m. 0, or -1 out of memory. */
static int write_settings(gptps_settings *r, FILE *f, const char *text, const gptps_toml *t,
                          gptps_settings_in_file_fn in_file, void *ud)
{
    save_line  *lines = NULL;
    save_block *blocks = NULL;
    save_add   *adds = NULL;
    size_t nlines = 0, nblocks = 1, nadds = 0, cap = 0, i, j, wrote = 0;
    long top_at = -2;                   /* block 0 has no keys: where its additions go */
    int  top_gap = 0;                   /* ... and whether a blank line follows them */
    int  gb_alias = t && gptps_toml_find_dotted(t, SAVE_GB_KEY) >= 0 && gptps_toml_find_dotted(t, SAVE_BYTES_KEY) < 0;
    const char *p = text;
    gptps_setting_entry *e;
    int rc = -1;

    /* the lines, and the blocks the headers divide them into */
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p), k = 0;
        save_line *L;
        if (nlines == cap) {
            size_t nc = cap ? cap * 2 : 64;
            save_line *grown = (save_line *)gptps_realloc(lines, nc * sizeof *grown);
            if (!grown) goto out;
            lines = grown; cap = nc;
        }
        L = &lines[nlines];
        L->start = p; L->len = len; L->header = -1; L->comment = 0; L->entry = -1;
        while (k < len && save_ws((unsigned char)p[k])) ++k;
        if (k < len && p[k] == '#') L->comment = 1;
        ++nlines;
        p = nl ? nl + 1 : p + len;
    }
    /* which line holds which key, and which starts which table: the parser's own
     * reading, by line number (1-based), so nothing here re-reads TOML */
    for (i = 0; t && i < gptps_toml_count(t); ++i) {
        int ln = gptps_toml_line_at(t, i);
        if (ln >= 1 && (size_t)ln <= nlines) lines[ln - 1].entry = (long)i;
    }
    for (i = 0; t && i < gptps_toml_table_count(t); ++i) {
        int ln = gptps_toml_table_line_at(t, i);
        if (ln >= 1 && (size_t)ln <= nlines) { lines[ln - 1].header = (long)i; ++nblocks; }
    }
    blocks = (save_block *)gptps_calloc(nblocks, sizeof *blocks);
    adds = (save_add *)gptps_calloc(r->n ? r->n : 1, sizeof *adds);
    if (!blocks || !adds) goto out;
    for (j = 0; j < nblocks; ++j) blocks[j].last_key = -1;
    for (i = 0, j = 0; i < nlines; ++i) {
        save_block *B;
        if (lines[i].header >= 0) {
            const char *name = gptps_toml_table_at(t, (size_t)lines[i].header);
            B = &blocks[++j];
            if (strlen(name) < sizeof B->section) memcpy(B->section, name, strlen(name) + 1);
            B->first_line = i;
        } else {
            B = &blocks[j];
            if (lines[i].entry >= 0) B->last_key = (long)i;
        }
        lines[i].block = j;
    }

    /* the settings to add, and where each goes */
    for (e = r->head; e; e = e->next) {
        size_t sl;
        save_add *A;
        long target = -1;                               /* a block */
        if (t && (gptps_toml_find_dotted(t, e->key) >= 0 || (gb_alias && !strcmp(e->key, SAVE_BYTES_KEY)))) continue;
        A = &adds[nadds];
        if (!save_wants(e, t, in_file, ud, A)) continue;
        ++nadds;
        leaf_of(e->key, &sl);
        A->e = e;
        if (sl == 0 || sl >= 255) {
            target = 0; A->skip = 0;                    /* a key with no table - or one whose table
                                                         * name is past what a [table] line holds */
        } else {
            for (j = 1; j < nblocks; ++j)               /* its own [section] */
                if (strlen(blocks[j].section) == sl && memcmp(blocks[j].section, e->key, sl) == 0) {
                    target = (long)j; A->skip = sl + 1; break;
                }
            for (i = 0; target < 0 && i < nlines; ++i) {    /* its siblings, as dotted keys */
                char d[512];
                const char *sec;
                size_t secl;
                if (lines[i].entry < 0) continue;
                gptps_toml_dotted_at(t, (size_t)lines[i].entry, d, sizeof d);
                if (strncmp(d, e->key, sl) != 0 || d[sl] != '.') continue;
                /* only in a table the key lies under: [tasks.x.resources] holds the
                 * sibling tasks.x.resources.gpu, but cannot hold tasks.x.priority */
                sec = blocks[lines[i].block].section;
                secl = strlen(sec);
                if (secl && (secl > sl || strncmp(e->key, sec, secl) != 0 || e->key[secl] != '.')) continue;
                target = (long)lines[i].block;
                A->skip = secl ? secl + 1 : 0;
            }
            if (target < 0) { A->at_end = 1; A->skip = sl + 1; continue; }
        }
        if (blocks[target].last_key >= 0) A->after = blocks[target].last_key;
        else if (target > 0) A->after = (long)blocks[target].first_line;
        else {
            /* block 0 has no keys: before the first header and the comment above it */
            if (top_at == -2) {
                if (nblocks > 1) {
                    size_t h = blocks[1].first_line;
                    while (h > 0 && lines[h - 1].comment) --h;   /* the comment over the first table */
                    top_at = (long)h - 1; top_gap = 1;
                } else {
                    top_at = (long)nlines - 1;
                }
            }
            A->after = top_at;
        }
    }

    /* the file, with the changed values rewritten and the additions in place */
    for (i = 0; i <= nlines; ++i) {
        long at = (long)i - 1;                          /* additions after line i-1 */
        int any = 0;
        for (j = 0; j < nadds; ++j) {
            if (adds[j].at_end || adds[j].after != at) continue;
            put_add(f, &adds[j]);
            any = 1; ++wrote;
        }
        if (any && at == top_at && top_gap) fputc('\n', f);
        if (i == nlines) break;
        if (lines[i].entry >= 0) {
            char d[512];
            int gb;
            gptps_toml_dotted_at(t, (size_t)lines[i].entry, d, sizeof d);
            gb = gb_alias && !strcmp(d, SAVE_GB_KEY);
            e = setting_find(r, gb ? SAVE_BYTES_KEY : d);
            if (e && e->dirty && !e->nosave) {
                size_t vs, vl;
                value_span(lines[i].start, lines[i].len, &vs, &vl);
                if (gb && setting_ull(e) > SAVE_GB_EXACT) {
                    /* the key as the line spells it, its last part in bytes instead */
                    const char *k = gptps_toml_key_at(t, (size_t)lines[i].entry);
                    char nk[512];
                    size_t ind = 0, kl = strlen(k);
                    while (ind < lines[i].len && save_ws((unsigned char)lines[i].start[ind])) ++ind;
                    snprintf(nk, sizeof nk, "%.*smax_memory_bytes", (int)(kl - 13), k);
                    fwrite(lines[i].start, 1, ind, f);
                    put_key(f, nk);
                    fputs(" = ", f);
                    put_value(f, e);
                } else {
                    fwrite(lines[i].start, 1, vs, f);
                    if (gb) {
                        char g[40];
                        gptps_fmt_double_c(g, sizeof g, (double)setting_ull(e) / 1073741824.0);
                        fputs(g, f);
                    } else {
                        put_value(f, e);
                    }
                }
                fwrite(lines[i].start + vs + vl, 1, lines[i].len - vs - vl, f);
                fputc('\n', f);
                continue;
            }
        }
        fwrite(lines[i].start, 1, lines[i].len, f);
        fputc('\n', f);
    }
    /* sections the file does not have, at the end, in the order they were registered */
    for (j = 0; j < nadds; ++j) {
        size_t sl, k;
        if (!adds[j].at_end) continue;
        leaf_of(adds[j].e->key, &sl);
        if (nlines || wrote) fputc('\n', f);
        put_table(f, adds[j].e->key, sl);
        for (k = j; k < nadds; ++k) {
            size_t kl;
            if (!adds[k].at_end) continue;
            leaf_of(adds[k].e->key, &kl);
            if (kl != sl || memcmp(adds[k].e->key, adds[j].e->key, sl) != 0) continue;
            put_add(f, &adds[k]);
            ++wrote;
            if (k != j) adds[k].at_end = 0;
        }
        adds[j].at_end = 0;
    }
    rc = 0;
out:
    gptps_free(adds);
    gptps_free(blocks);
    gptps_free(lines);
    return rc;
}

gptps_status gptps_settings_save_to(gptps_settings *r, const char *path, const char *base,
                                    gptps_settings_in_file_fn in_file, void *ud)
{
    char err[4096], msg[900], *tmp = NULL, *text = NULL;
    gptps_toml *t = NULL;
    gptps_log_level lvl = GPTPS_LOG_ERROR;
    size_t tn;
    FILE *f;
    int made = 0;
    gptps_status st = GPTPS_OK;
    if (!r || !path) return GPTPS_E_INVAL;
    msg[0] = 0;                            /* what to log, once the lock is released */
    /* One save at a time, from reading the file to renaming its replacement into place.
     * Every save to a path writes the same "<path>.tmp": a second save let in between
     * the first's write and its rename truncated that file and wrote into it, so the
     * first renamed a half-written file into place, and the second's rename found
     * nothing left to move (GPTPS_E_IO). Reading under the lock too means a save edits
     * the file the last one left rather than a copy from before it - and, on Windows,
     * which will not replace a file another handle has open, that one save is never
     * reading the file while another renames over it.
     *
     * From here every way out goes through `out`, which releases the lock, frees what
     * was read and logs: a return in between would leave r->m held, and the next save
     * would wait for it forever. */
    gptps_mutex_lock(r->m);
    f = fopen(path, "rb");
    if (!f && errno == ENOENT && base && strcmp(base, path) != 0) {
        /* A new file starts as a copy of the config file the engine loaded - its
         * add-ons, [task_defaults], comments and all - with the live changes made
         * in it as in place. If that file cannot be read or no longer parses, the
         * settings alone are written, as for an engine opened without a file. */
        text = gptps_toml_read_file(base, err, sizeof err);
        if (text && !(t = gptps_toml_parse_text(base, text, err, sizeof err))) { gptps_free(text); text = NULL; }
        if (!t) {
            char *nl = strchr(err, '\n');
            if (nl) *nl = 0;
            snprintf(msg, sizeof msg, "settings saved to %.300s without a copy of the loaded config file: %.500s",
                     path, err);
            lvl = GPTPS_LOG_WARN;           /* logged only if the save then succeeds */
        }
    } else if (f) {
        fclose(f);
        text = gptps_toml_read_file(path, err, sizeof err);
        if (text) t = gptps_toml_parse_text(path, text, err, sizeof err);
        if (!t) {
            char *nl = strchr(err, '\n');    /* the first problem says enough */
            if (nl) *nl = 0;
            snprintf(msg, sizeof msg, "settings not saved: %.700s%s - the file was left as it is; "
                     "fix it, or save to another path", err, nl ? " (and more)" : "");
            st = text ? GPTPS_E_CONFIG : GPTPS_E_IO;
            goto out;
        }
    } else if (errno != ENOENT) {
        st = GPTPS_E_IO;                    /* there, but not readable: leave it */
        goto out;
    }
    tn = strlen(path) + 5;
    tmp = (char *)gptps_malloc(tn);
    if (!tmp) { st = GPTPS_E_NOMEM; goto out; }
    snprintf(tmp, tn, "%s.tmp", path);

    f = fopen(tmp, "wb");
    if (!f) st = GPTPS_E_IO;
    else {
        made = 1;
        if (!text)
            fputs("# GPTPS settings, written by gptps_settings_save: the values changed live and\n"
                  "# the ones the config file set. Edit freely - a later save updates the values\n"
                  "# you change and keeps the rest, comments included. Every key: docs/CONFIG.md.\n\n", f);
        if (write_settings(r, f, text ? text : "", t, in_file, ud) != 0) st = GPTPS_E_NOMEM;
        if (fflush(f) != 0 && st == GPTPS_OK) st = GPTPS_E_IO;
        if (fclose(f) != 0 && st == GPTPS_OK) st = GPTPS_E_IO;
    }
    if (st == GPTPS_OK) st = gptps_hal_atomic_replace(tmp, path);
    if (st != GPTPS_OK && made) remove(tmp);
out:
    gptps_mutex_unlock(r->m);
    gptps_toml_free(t);
    gptps_free(text);
    gptps_free(tmp);
    /* After the unlock: a log sink is host code, and one that hands the message to a
     * thread of its own, which reads a setting, must not find the lock still held. */
    if (msg[0] && (lvl == GPTPS_LOG_ERROR || st == GPTPS_OK)) gptps_log(NULL, lvl, msg);
    return st;
}
