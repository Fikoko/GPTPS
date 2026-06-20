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
    struct gptps_setting_entry *next;
} gptps_setting_entry;

struct gptps_settings {
    gptps_mutex         *m;
    gptps_setting_entry *head, *tail;
    size_t               n;
};

static char *dupz(const char *s) { size_t n = strlen(s) + 1; char *o = (char *)malloc(n); if (o) memcpy(o, s, n); return o; }

gptps_settings *gptps_settings_create(void)
{
    gptps_settings *r = (gptps_settings *)calloc(1, sizeof *r);
    if (!r) return NULL;
    r->m = gptps_mutex_create();
    if (!r->m) { free(r); return NULL; }
    return r;
}

void gptps_settings_destroy(gptps_settings *r)
{
    gptps_setting_entry *e;
    if (!r) return;
    e = r->head;
    while (e) { gptps_setting_entry *n = e->next; free(e->key); free(e->desc); free(e->defval); free(e); e = n; }
    gptps_mutex_destroy(r->m);
    free(r);
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
    e = (gptps_setting_entry *)calloc(1, sizeof *e);
    if (e) { e->key = dupz(def->key); e->desc = dupz(def->desc ? def->desc : ""); e->defval = (char *)malloc(GPTPS_SETTINGS_VALUE_MAX); }
    if (!e || !e->key || !e->desc || !e->defval) {
        if (e) { free(e->key); free(e->desc); free(e->defval); free(e); }
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

/* parse + range/enum check; 0 = invalid, 1 = ok */
static int valid_value(const gptps_setting_entry *e, const char *v)
{
    char *end;
    switch (e->type) {
        case GPTPS_SETTING_INT: {
            long long x = strtoll(v, &end, 10);
            if (end == v) return 0;
            while (*end == ' ' || *end == '\t') ++end;
            if (*end) return 0;
            if (e->has_range && ((double)x < e->min || (double)x > e->max)) return 0;
            return 1;
        }
        case GPTPS_SETTING_UINT: {
            unsigned long long x;
            const char *p = v; while (*p == ' ' || *p == '\t') ++p;
            if (*p == '-') return 0;
            x = strtoull(v, &end, 10);
            if (end == v) return 0;
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

gptps_status gptps_settings_set_by(gptps_settings *r, const char *key, const char *value)
{
    gptps_setting_entry *e;
    gptps_status st;
    if (!r || !key || !value) return GPTPS_E_INVAL;
    gptps_mutex_lock(r->m);
    e = setting_find(r, key);
    if (!e) { gptps_mutex_unlock(r->m); return GPTPS_E_NOTFOUND; }
    if (!valid_value(e, value)) { gptps_mutex_unlock(r->m); return GPTPS_E_CONFIG; }
    st = e->write(e->target, value);     /* write_fn takes the target's own lock */
    gptps_mutex_unlock(r->m);
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
