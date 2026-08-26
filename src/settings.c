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
    size_t             (*read)(void *, char *, size_t);
    gptps_status       (*write)(void *, const char *);
    int                  mark;      /* transient, used while serializing */
    struct gptps_setting_entry *next;
} gptps_setting_entry;

typedef struct gptps_setting_watcher {
    gptps_settings_cb            cb;
    void                        *ud;
    struct gptps_setting_watcher *next;
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

gptps_status gptps_settings_watch_add(gptps_settings *r, gptps_settings_cb cb, void *ud)
{
    gptps_setting_watcher *w;
    if (!r || !cb) return GPTPS_E_INVAL;
    w = (gptps_setting_watcher *)gptps_calloc(1, sizeof *w);
    if (!w) return GPTPS_E_NOMEM;
    w->cb = cb; w->ud = ud;
    gptps_mutex_lock(r->m); w->next = r->watchers; r->watchers = w; gptps_mutex_unlock(r->m);
    return GPTPS_OK;
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

size_t gptps_settings_remove_prefix(gptps_settings *r, const char *prefix)
{
    gptps_setting_entry *e, *prev = NULL;
    size_t plen, removed = 0;
    if (!r || !prefix) return 0;
    plen = strlen(prefix);
    gptps_mutex_lock(r->m);
    e = r->head;
    while (e) {
        gptps_setting_entry *next = e->next;
        if (strncmp(e->key, prefix, plen) == 0) {
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
static int valid_value(const gptps_setting_entry *e, const char *v)
{
    char *end;
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
            double x = strtod(v, &end);
            if (end == v) return 0;
            while (*end == ' ' || *end == '\t') ++end;
            if (*end) return 0;
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
static gptps_status apply_entry(gptps_setting_entry *e, const char *value)
{
    if (!valid_value(e, value)) return GPTPS_E_CONFIG;
    return e->write(e->target, value);   /* write_fn takes the target's own lock */
}

gptps_status gptps_settings_set_by(gptps_settings *r, const char *key, const char *value)
{
    gptps_setting_entry *e;
    gptps_status st;
    char applied[GPTPS_SETTINGS_VALUE_MAX];
    int ok = 0;
    if (!r || !key || !value) return GPTPS_E_INVAL;
    gptps_mutex_lock(r->m);
    e = setting_find(r, key);
    if (!e) { gptps_mutex_unlock(r->m); return GPTPS_E_NOTFOUND; }
    st = apply_entry(e, value);
    if (st == GPTPS_OK) { applied[0] = 0; e->read(e->target, applied, sizeof applied); ok = 1; }
    gptps_mutex_unlock(r->m);
    if (ok) {   /* notify watchers with the lock RELEASED (register-before-use; may re-enter get) */
        gptps_setting_watcher *w;
        for (w = r->watchers; w; w = w->next) w->cb(key, applied, w->ud);
    }
    return st;
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

/* ---- TOML serialization (save) ---- */
/* leaf = after the last '.'; *seclen = bytes of the section prefix (0 if none) */
static const char *leaf_of(const char *key, size_t *seclen)
{
    const char *dot = strrchr(key, '.');
    if (!dot) { *seclen = 0; return key; }
    *seclen = (size_t)(dot - key);
    return dot + 1;
}

static void emit_kv(FILE *f, gptps_setting_entry *e, const char *leaf)
{
    char val[GPTPS_SETTINGS_VALUE_MAX];
    val[0] = 0;
    e->read(e->target, val, sizeof val);
    if (e->type == GPTPS_SETTING_STRING || e->type == GPTPS_SETTING_ENUM) {
        const char *p;
        fputs(leaf, f); fputs(" = \"", f);
        for (p = val; *p; ++p) {
            switch (*p) {
                case '"': case '\\': fputc('\\', f); fputc(*p, f); break;
                case '\n': fputs("\\n", f); break;
                case '\t': fputs("\\t", f); break;
                default: fputc(*p, f);
            }
        }
        fputs("\"\n", f);
    } else {
        fputs(leaf, f); fputs(" = ", f); fputs(val, f); fputc('\n', f);
    }
}

gptps_status gptps_settings_save_to(gptps_settings *r, const char *path)
{
    char *tmp; size_t tn; FILE *f; gptps_setting_entry *e;
    gptps_status st;
    if (!r || !path) return GPTPS_E_INVAL;
    tn = strlen(path) + 5;
    tmp = (char *)gptps_malloc(tn);
    if (!tmp) return GPTPS_E_NOMEM;
    snprintf(tmp, tn, "%s.tmp", path);

    gptps_mutex_lock(r->m);
    f = fopen(tmp, "wb");
    if (!f) { gptps_mutex_unlock(r->m); gptps_free(tmp); return GPTPS_E_IO; }
    fputs("# GPTPS settings - regenerated by gptps_settings_save (comments not preserved)\n", f);
    for (e = r->head; e; e = e->next) e->mark = 0;
    /* top-level (dot-less) keys first, then one [section] block per prefix */
    for (e = r->head; e; e = e->next) {
        size_t sl; const char *leaf = leaf_of(e->key, &sl);
        if (sl == 0 && !e->mark) { emit_kv(f, e, leaf); e->mark = 1; }
    }
    for (e = r->head; e; e = e->next) {
        size_t sl;
        gptps_setting_entry *g;
        if (e->mark) continue;
        leaf_of(e->key, &sl);     /* only need the section length here */
        fputc('\n', f); fputc('[', f); fwrite(e->key, 1, sl, f); fputs("]\n", f);
        for (g = e; g; g = g->next) {
            size_t gl; const char *gleaf = leaf_of(g->key, &gl);
            if (!g->mark && gl == sl && memcmp(g->key, e->key, sl) == 0) { emit_kv(f, g, gleaf); g->mark = 1; }
        }
    }
    fflush(f);
    st = (fclose(f) == 0) ? GPTPS_OK : GPTPS_E_IO;
    gptps_mutex_unlock(r->m);

    if (st == GPTPS_OK) st = gptps_hal_atomic_replace(tmp, path);
    if (st != GPTPS_OK) remove(tmp);
    gptps_free(tmp);
    return st;
}

/* ---- reload: re-apply known keys from a parsed TOML (validated) ---- */
gptps_status gptps_settings_apply_toml(gptps_settings *r, const gptps_toml *t)
{
    gptps_setting_entry *e;
    gptps_status first = GPTPS_OK;
    if (!r || !t) return GPTPS_E_INVAL;
    gptps_mutex_lock(r->m);
    for (e = r->head; e; e = e->next) {
        size_t sl; const char *leaf = leaf_of(e->key, &sl);
        char sec[256], val[GPTPS_SETTINGS_VALUE_MAX];
        gptps_status st = GPTPS_OK;
        int have = 0;
        if (sl >= sizeof sec) continue;
        memcpy(sec, e->key, sl); sec[sl] = 0;
        switch (e->type) {
            case GPTPS_SETTING_INT: case GPTPS_SETTING_UINT: {
                long long ll;
                if (gptps_toml_int(t, sec, leaf, &ll)) { snprintf(val, sizeof val, "%lld", ll); have = 1; }
                break;
            }
            case GPTPS_SETTING_DOUBLE: {
                double d;
                if (gptps_toml_double(t, sec, leaf, &d)) { snprintf(val, sizeof val, "%g", d); have = 1; }
                break;
            }
            case GPTPS_SETTING_BOOL: {
                int b;
                if (gptps_toml_bool(t, sec, leaf, &b)) { snprintf(val, sizeof val, "%s", b ? "true" : "false"); have = 1; }
                break;
            }
            case GPTPS_SETTING_ENUM: case GPTPS_SETTING_STRING: {
                const char *s = gptps_toml_str(t, sec, leaf);
                if (s) { snprintf(val, sizeof val, "%s", s); have = 1; }
                break;
            }
        }
        if (have) st = apply_entry(e, val);   /* validated + applied */
        if (st != GPTPS_OK && first == GPTPS_OK) first = st;
    }
    gptps_mutex_unlock(r->m);
    return first;
}
