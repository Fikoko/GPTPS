/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_settings_order.c - a settings file's outcome does not hang on the order of its
 * keys (gptps.h, THE FILE'S ORDER; ABI 2.10). Two host settings checked against each
 * other - app.lo is refused above app.hi, app.hi below app.lo - in both orders:
 *
 *  1) Defined under gptps_settings_hold, a file raising both applies whole, either
 *     order; without the hold, one key at a time, lo is checked against hi's default
 *     and refused (the one-at-a-time definition the hold exists for).
 *  2) A reload raising both and one lowering both apply whole, either order: the exact
 *     state, no refusal counted or logged, each key's accessor applied once, no ERROR.
 *  3) A file no order applies (lo above the file's own hi): lo is refused ONCE - one
 *     count, one WARN from the accessor, one ERROR, GPTPS_E_CONFIG - the same state in
 *     either order; hi is applied. The accessor's calls: a provisional try, then the
 *     last - or one ordinary try only, when nothing can change before a retry (the
 *     last key of a pass, nothing applied after a refusal in it).
 *  4) A cycle (c taken only when d already holds the value, and d only when c does):
 *     both refused, once each, in either order. A chain x <= y <= z raised in the worst
 *     order (x, y, z) takes a pass per link: applied whole, nothing counted.
 *  5) Memory: an accessor's GPTPS_E_NOMEM is not retried (one call), the reload says
 *     GPTPS_E_NOMEM; a value the registry refuses (type) never reaches the accessor.
 *  6) gptps_settings_provisional is 0 outside a file's apply and for a live set (which
 *     counts its refusal); gptps_settings_hold nests and refuses a let-go it lacks.
 *  7) A host watcher hears none of a file's values (as before) - and the live set once.
 *  8) A binary plug-in (addon_order) through the host table: at open (its setup's
 *     values) and at reloads, either order, its watcher hears each applied key once and
 *     its provisional-aware accessors count only the refusal that stands. A live set its
 *     watcher makes while hearing a provisional try that applied is NOT provisional: its
 *     refusal is counted.
 *  9) Under a hold, a task registered (or a resource defined) takes the file's values
 *     at once - its resource cost is in force before the let-go; a per-task setting
 *     defined under the hold waits for it, as a setting does.
 * 10) At definition time too, a chain x <= y <= z defined under a hold, with the file
 *     raising it in the worst order (x, y, z), takes a pass per link: applied whole,
 *     nothing counted, nothing logged - either order of the file's keys.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gptps.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static const char *CFG = "test_settings_order.toml";

static char logbuf[16384];
static size_t loglen;
static void sink(gptps_log_level lvl, const char *msg, void *u)
{
    size_t n = strlen(msg);
    (void)u;
    if (lvl < GPTPS_LOG_WARN) return;
    if (getenv("ORDER_LOG")) fprintf(stderr, "[log] %s\n", msg);
    if (loglen + n + 2 < sizeof logbuf) { memcpy(logbuf + loglen, msg, n); loglen += n; logbuf[loglen++] = '\n'; logbuf[loglen] = 0; }
}
static void log_clear(void) { loglen = 0; logbuf[0] = 0; }
static int log_count(const char *needle)
{
    int c = 0;
    const char *p = logbuf;
    while ((p = strstr(p, needle)) != NULL) { ++c; p += strlen(needle); }
    return c;
}

static void put(const char *text)
{
    FILE *f = fopen(CFG, "w");
    CHECK(f != NULL);
    if (f) { fputs(text, f); fclose(f); }
}

/* ---- the host's pair, and a cycle ---- */
static gptps *g_e;
static unsigned long lo_v, hi_v, c_v, d_v, x_v, y_v, z_v;
static unsigned chain_refusals;
static unsigned calls_lo, calls_hi, calls_c, calls_d, applied_lo, applied_hi, refusals, prov_seen;
static void reset(void)
{
    lo_v = 1; hi_v = 10; c_v = d_v = 0;
    x_v = 1; y_v = 2; z_v = 3; chain_refusals = 0;
    calls_lo = calls_hi = calls_c = calls_d = applied_lo = applied_hi = refusals = prov_seen = 0;
}
/* a refusal the accessor reports: not while the try is provisional */
static gptps_status refuse(const char *key)
{
    if (gptps_settings_provisional(g_e, key)) { ++prov_seen; return GPTPS_E_INVAL; }
    ++refusals;
    {
        char m[96];
        snprintf(m, sizeof m, "app accessor: %s refused", key);
        gptps_log(NULL, GPTPS_LOG_WARN, m);
    }
    return GPTPS_E_INVAL;
}
static size_t rd_u(char *b, size_t c, unsigned long v) { return (size_t)snprintf(b, c, "%lu", v); }
static size_t rd_lo(void *t, char *b, size_t c) { (void)t; return rd_u(b, c, lo_v); }
static size_t rd_hi(void *t, char *b, size_t c) { (void)t; return rd_u(b, c, hi_v); }
static size_t rd_c(void *t, char *b, size_t c)  { (void)t; return rd_u(b, c, c_v); }
static size_t rd_d(void *t, char *b, size_t c)  { (void)t; return rd_u(b, c, d_v); }
static gptps_status wr_lo(void *t, const char *v)
{
    unsigned long n = strtoul(v, NULL, 10);
    (void)t; ++calls_lo;
    if (n > hi_v) return refuse("app.lo");
    lo_v = n; ++applied_lo;
    return GPTPS_OK;
}
static gptps_status wr_hi(void *t, const char *v)
{
    unsigned long n = strtoul(v, NULL, 10);
    (void)t; ++calls_hi;
    if (n == 999) return GPTPS_E_NOMEM;        /* memory ran out applying it */
    if (n < lo_v) return refuse("app.hi");
    hi_v = n; ++applied_hi;
    return GPTPS_OK;
}
static gptps_status wr_c(void *t, const char *v)
{
    unsigned long n = strtoul(v, NULL, 10);
    (void)t; ++calls_c;
    if (n != d_v) return refuse("app.c");
    c_v = n;
    return GPTPS_OK;
}
static gptps_status wr_d(void *t, const char *v)
{
    unsigned long n = strtoul(v, NULL, 10);
    (void)t; ++calls_d;
    if (n != c_v) return refuse("app.d");
    d_v = n;
    return GPTPS_OK;
}
static void define(gptps *e, const char *key, size_t (*rd)(void *, char *, size_t), gptps_status (*wr)(void *, const char *))
{
    gptps_setting_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.key = key; d.type = GPTPS_SETTING_UINT; d.desc = "test";
    d.read = rd; d.write = wr; d.hot = 1;
    CHECK(gptps_register_setting(e, &d) == GPTPS_OK);
}
/* a chain: x <= y <= z */
static gptps_status chain_refuse(const char *key)
{
    if (!gptps_settings_provisional(g_e, key)) ++chain_refusals;
    return GPTPS_E_INVAL;
}
static size_t rd_x(void *t, char *b, size_t c) { (void)t; return rd_u(b, c, x_v); }
static size_t rd_y(void *t, char *b, size_t c) { (void)t; return rd_u(b, c, y_v); }
static size_t rd_z(void *t, char *b, size_t c) { (void)t; return rd_u(b, c, z_v); }
static gptps_status wr_x(void *t, const char *v)
{ unsigned long n = strtoul(v, NULL, 10); (void)t; if (n > y_v) return chain_refuse("chain.x"); x_v = n; return GPTPS_OK; }
static gptps_status wr_y(void *t, const char *v)
{ unsigned long n = strtoul(v, NULL, 10); (void)t; if (n < x_v || n > z_v) return chain_refuse("chain.y"); y_v = n; return GPTPS_OK; }
static gptps_status wr_z(void *t, const char *v)
{ unsigned long n = strtoul(v, NULL, 10); (void)t; if (n < y_v) return chain_refuse("chain.z"); z_v = n; return GPTPS_OK; }

static void define_all(gptps *e)
{
    define(e, "app.lo", rd_lo, wr_lo);
    define(e, "app.hi", rd_hi, wr_hi);
    define(e, "app.c", rd_c, wr_c);
    define(e, "app.d", rd_d, wr_d);
    define(e, "chain.x", rd_x, wr_x);
    define(e, "chain.y", rd_y, wr_y);
    define(e, "chain.z", rd_z, wr_z);
}

static unsigned heard;
static void watcher(const char *key, const char *value, void *ud) { (void)key; (void)value; (void)ud; ++heard; }

static gptps *open_with(const char *text)
{
    gptps_config cfg;
    gptps *e = NULL;
    put(text);
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1; cfg.limits.max_memory_bytes = 1u << 20;
    cfg.mode = GPTPS_RUN_MANUAL; cfg.config_path = CFG;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    return e;
}
static gptps_status reload(gptps *e, const char *text)
{
    log_clear();
    put(text);
    return gptps_settings_reload(e, CFG);
}

static const char *const RAISE[2] = { "[app]\nlo = 50\nhi = 100\n", "[app]\nhi = 100\nlo = 50\n" };
static const char *const UP[2]    = { "[app]\nlo = 500\nhi = 1000\n", "[app]\nhi = 1000\nlo = 500\n" };
static const char *const DOWN[2]  = { "[app]\nlo = 5\nhi = 8\n", "[app]\nhi = 8\nlo = 5\n" };
/* lo above the file's own hi: no order applies it */
static const char *const NOFIT[2] = { "[app]\nlo = 200\nhi = 150\n", "[app]\nhi = 150\nlo = 200\n" };
static const char *const CYCLE[2] = { "[app]\nc = 5\nd = 5\n", "[app]\nd = 5\nc = 5\n" };

static void test_host(int o)
{
    gptps *e;
    unsigned cl, ch;
    reset();
    /* 1) defined together, under a hold */
    e = open_with(RAISE[o]);
    if (!e) return;
    g_e = e;
    log_clear();
    CHECK(gptps_settings_hold(e, 1) == GPTPS_OK);
    define_all(e);
    CHECK(lo_v == 1 && hi_v == 10);                    /* the values wait while it holds */
    CHECK(gptps_settings_hold(e, 0) == GPTPS_OK);
    CHECK(lo_v == 50 && hi_v == 100 && refusals == 0 && applied_lo == 1 && applied_hi == 1);
    CHECK(prov_seen == (o == 0 ? 1u : 0u));            /* lo before hi: one provisional refusal */
    CHECK(log_count("refused") == 0 && gptps_config_check(e) == GPTPS_OK);
    CHECK(gptps_settings_watch(e, watcher, NULL) == GPTPS_OK);
    heard = 0;

    /* 2) reloads that raise and lower both, either order */
    reset(); lo_v = 50; hi_v = 100;
    CHECK(reload(e, UP[o]) == GPTPS_OK);
    CHECK(lo_v == 500 && hi_v == 1000 && refusals == 0 && applied_lo == 1 && applied_hi == 1);
    CHECK(log_count("refused") == 0 && gptps_config_check(e) == GPTPS_OK);
    reset(); lo_v = 500; hi_v = 1000;
    CHECK(reload(e, DOWN[o]) == GPTPS_OK);
    CHECK(lo_v == 5 && hi_v == 8 && refusals == 0 && applied_lo == 1 && applied_hi == 1);
    CHECK(prov_seen == (o == 0 ? 0u : 1u));            /* hi before lo: one provisional refusal */
    CHECK(log_count("refused") == 0 && gptps_config_check(e) == GPTPS_OK);
    reset(); lo_v = 5; hi_v = 8;
    CHECK(reload(e, UP[1 - o]) == GPTPS_OK && lo_v == 500 && hi_v == 1000 && refusals == 0);
    reset(); lo_v = 500; hi_v = 1000;
    CHECK(reload(e, DOWN[1 - o]) == GPTPS_OK && lo_v == 5 && hi_v == 8 && refusals == 0);
    CHECK(log_count("refused") == 0);
    CHECK(heard == 0);                                 /* 7) a host watcher hears no file */

    /* 3) a file no order applies: lo refused once, hi applied */
    reset(); lo_v = 5; hi_v = 8;
    CHECK(reload(e, NOFIT[o]) == GPTPS_E_CONFIG);
    CHECK(lo_v == 5 && hi_v == 150 && applied_hi == 1 && applied_lo == 0 && refusals == 1);
    CHECK(log_count("app accessor: app.lo refused") == 1 && log_count("app.lo: 200 was refused") == 1);
    CHECK(log_count("refused") == 2 && log_count("1 error in the reload") == 1);
    /* lo first: a provisional try, then the last (after hi applied); hi first: lo is the
     * pass's last key and nothing applied after a refusal - one ordinary try, as in 1.12 */
    CHECK(calls_lo == (o == 0 ? 2u : 1u) && calls_hi == 1);
    CHECK(gptps_config_check(e) == GPTPS_E_CONFIG);

    /* 4) a cycle: neither can go first */
    reset(); lo_v = 5; hi_v = 150;
    CHECK(reload(e, CYCLE[o]) == GPTPS_E_CONFIG);
    /* the first listed: provisional, then the last; the second: the pass's last, ordinary */
    CHECK(c_v == 0 && d_v == 0 && refusals == 2 && calls_c + calls_d == 3 && (o == 0 ? calls_c : calls_d) == 2);
    CHECK(log_count("app.c: 5 was refused") == 1 && log_count("app.d: 5 was refused") == 1 &&
          log_count("2 errors in the reload") == 1);

    /* 4) a chain, raised in the order that needs a pass per link - and lowered in the other */
    reset(); lo_v = 5; hi_v = 150;
    CHECK(reload(e, o ? "[chain]\nz = 30\ny = 20\nx = 10\n" : "[chain]\nx = 10\ny = 20\nz = 30\n") == GPTPS_OK);
    CHECK(x_v == 10 && y_v == 20 && z_v == 30 && chain_refusals == 0 && log_count("refused") == 0);
    reset(); lo_v = 5; hi_v = 150; x_v = 10; y_v = 20; z_v = 30;
    CHECK(reload(e, o ? "[chain]\nx = 1\ny = 2\nz = 3\n" : "[chain]\nz = 3\ny = 2\nx = 1\n") == GPTPS_OK);
    CHECK(x_v == 1 && y_v == 2 && z_v == 3 && chain_refusals == 0 && log_count("refused") == 0);

    /* 5) memory: not retried, and said; the type never reaches the accessor */
    reset(); lo_v = 5; hi_v = 150;
    CHECK(reload(e, o ? "[app]\nhi = 999\nlo = 600\n" : "[app]\nlo = 600\nhi = 999\n") == GPTPS_E_NOMEM);
    CHECK(calls_hi == 1 && lo_v == 5 && hi_v == 150 && refusals == 1);
    CHECK(log_count("app.hi: 999 was refused (out of memory") == 1 && log_count("app.lo: 600 was refused") == 1);
    reset(); lo_v = 5; hi_v = 150;
    CHECK(reload(e, "[app]\nhi = \"x\"\nlo = 7\n") == GPTPS_E_CONFIG);
    CHECK(calls_hi == 0 && calls_lo == 1 && lo_v == 7 && refusals == 0 && log_count("app.hi") == 1);

    /* 6) provisional is 0 outside an apply, and for a live set */
    reset(); lo_v = 7; hi_v = 150;
    CHECK(gptps_settings_provisional(e, "app.lo") == 0 && gptps_settings_provisional(NULL, "app.lo") == 0 &&
          gptps_settings_provisional(e, NULL) == 0);
    heard = 0;
    CHECK(gptps_settings_set(e, "app.lo", "151") == GPTPS_E_INVAL && refusals == 1 && prov_seen == 0);
    CHECK(gptps_settings_set(e, "app.lo", "9") == GPTPS_OK && heard == 1);   /* 7) the live set: once */
    CHECK(gptps_settings_hold(e, 0) == GPTPS_E_INVAL && gptps_settings_hold(NULL, 1) == GPTPS_E_INVAL);
    CHECK(gptps_settings_hold(e, 1) == GPTPS_OK && gptps_settings_hold(e, 1) == GPTPS_OK);
    CHECK(gptps_settings_hold(e, 0) == GPTPS_OK && gptps_settings_hold(e, 0) == GPTPS_OK);
    CHECK(gptps_settings_hold(e, 0) == GPTPS_E_INVAL);
    gptps_shutdown(e);

    /* 1) again, without the hold: one key at a time, lo meets hi's default (10) - refused
     * with ONE ordinary try (a setting defined alone: no other key can apply before a
     * retry, so its accessor is called once, as before 1.13), the definition-time order
     * the hold removes */
    reset();
    e = open_with(RAISE[o]);
    if (!e) return;
    g_e = e;
    log_clear();
    cl = calls_lo; ch = calls_hi;
    define(e, "app.lo", rd_lo, wr_lo);
    define(e, "app.hi", rd_hi, wr_hi);
    CHECK(lo_v == 1 && hi_v == 100 && refusals == 1 && calls_lo - cl == 1 && calls_hi - ch == 1 && prov_seen == 0);
    CHECK(log_count("app.lo: 50 was refused") == 1 && gptps_config_check(e) == GPTPS_E_CONFIG);
    gptps_shutdown(e);
}

/* ---- 9) a hold does not keep a task's values ---- */
static gptps_status noop_run(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }
static void test_hold_task(int o)
{
    gptps *e;
    gptps_task_def d;
    char b[64];
    reset();
    e = open_with(o ? "[resources]\ngpu = 4\n[tasks.t]\nknob = 7\n[tasks.t.resources]\ngpu = 2\n[app]\nhi = 100\nlo = 50\n"
                    : "[resources]\ngpu = 4\n[tasks.t]\nknob = 7\n[tasks.t.resources]\ngpu = 2\n[app]\nlo = 50\nhi = 100\n");
    if (!e) return;
    g_e = e;
    log_clear();
    CHECK(gptps_settings_hold(e, 1) == GPTPS_OK);
    define(e, "app.lo", rd_lo, wr_lo);
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "t"; d.run = noop_run; d.exec = GPTPS_EXEC_INPROC;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    /* the task's resource cost is in force at once, while the hold is on */
    CHECK(gptps_settings_get(e, "tasks.t.resources.gpu", b, sizeof b) == GPTPS_OK && !strcmp(b, "2"));
    /* a per-task setting defined under the hold: its value waits, as a setting's does */
    CHECK(gptps_define_task_setting(e, "knob", GPTPS_SETTING_UINT, "1", NULL, 0) == GPTPS_OK);
    CHECK(gptps_settings_get(e, "tasks.t.knob", b, sizeof b) == GPTPS_OK && !strcmp(b, "1"));
    define(e, "app.hi", rd_hi, wr_hi);
    CHECK(lo_v == 1 && hi_v == 10);
    CHECK(gptps_settings_hold(e, 0) == GPTPS_OK);
    CHECK(lo_v == 50 && hi_v == 100 && refusals == 0);
    CHECK(gptps_settings_get(e, "tasks.t.knob", b, sizeof b) == GPTPS_OK && !strcmp(b, "7"));
    CHECK(gptps_settings_get(e, "tasks.t.resources.gpu", b, sizeof b) == GPTPS_OK && !strcmp(b, "2"));
    CHECK(log_count("refused") == 0 && gptps_config_check(e) == GPTPS_OK);
    gptps_shutdown(e);
}

/* ---- 8) the plug-in ---- */
#ifdef ADDON_ORDER_PATH
static int stats(gptps *e, unsigned v[6])
{
    char b[GPTPS_SETTINGS_VALUE_MAX];
    if (gptps_settings_get(e, "order.stats", b, sizeof b) != GPTPS_OK) return 0;
    return sscanf(b, "%u %u %u %u %u %u", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6;
}
static int lohi(gptps *e, const char *lo, const char *hi)
{
    char a[64], b[64];
    return gptps_settings_get(e, "order.lo", a, sizeof a) == GPTPS_OK &&
           gptps_settings_get(e, "order.hi", b, sizeof b) == GPTPS_OK && !strcmp(a, lo) && !strcmp(b, hi);
}
static int leak(gptps *e, unsigned *refusals_n, int *nested_st, unsigned *nested_prov)
{
    char b[GPTPS_SETTINGS_VALUE_MAX];
    if (gptps_settings_get(e, "order.leak", b, sizeof b) != GPTPS_OK) return 0;
    return sscanf(b, "%u %d %u", refusals_n, nested_st, nested_prov) == 3;
}
static void test_plugin(int o)
{
    char text[1024];
    gptps *e;
    unsigned v[6], nr = 0, np = 0;
    int ns = -1;
    /* order.n first: its try is provisional (other keys follow it in the pass), it
     * applies, and the watcher live-sets n = 77 - refused, and counted */
    snprintf(text, sizeof text, "addons = [\"%s\"]\n%s", ADDON_ORDER_PATH,
             o ? "[order]\nn = 50\nhi = 100\nlo = 50\n" : "[order]\nn = 50\nlo = 50\nhi = 100\n");
    log_clear();
    e = open_with(text);
    if (!e) return;
    CHECK(lohi(e, "50", "100"));
    CHECK(leak(e, &nr, &ns, &np) && nr == 1 && ns != GPTPS_OK && ns != -1 && np == 0);
    CHECK(stats(e, v) && v[2] == 0 && v[3] == 1 && v[4] == 1 && v[5] == (o ? 0u : 1u));
    CHECK(log_count("refused") == 0 && gptps_config_check(e) == GPTPS_OK);
    /* reloads, either order */
    snprintf(text, sizeof text, "addons = [\"%s\"]\n%s", ADDON_ORDER_PATH,
             o ? "[order]\nlo = 500\nhi = 1000\n" : "[order]\nhi = 1000\nlo = 500\n");
    CHECK(reload(e, text) == GPTPS_OK && lohi(e, "500", "1000"));
    CHECK(stats(e, v) && v[2] == 0 && v[3] == 2 && v[4] == 2 && log_count("refused") == 0);
    snprintf(text, sizeof text, "addons = [\"%s\"]\n%s", ADDON_ORDER_PATH,
             o ? "[order]\nlo = 5\nhi = 8\n" : "[order]\nhi = 8\nlo = 5\n");
    CHECK(reload(e, text) == GPTPS_OK && lohi(e, "5", "8"));
    CHECK(stats(e, v) && v[2] == 0 && v[3] == 3 && v[4] == 3 && log_count("refused") == 0);
    /* a file no order applies: one refusal, the applied key heard once */
    snprintf(text, sizeof text, "addons = [\"%s\"]\n%s", ADDON_ORDER_PATH,
             o ? "[order]\nhi = 150\nlo = 200\n" : "[order]\nlo = 200\nhi = 150\n");
    CHECK(reload(e, text) == GPTPS_E_CONFIG && lohi(e, "5", "150"));
    CHECK(stats(e, v) && v[2] == 1 && v[3] == 3 && v[4] == 4);
    CHECK(log_count("addon_order: order.lo refused") == 1 && log_count("order.lo: 200 was refused") == 1);
    /* the watcher's live set at a reload, too: n back to 5, then 50 listed first */
    snprintf(text, sizeof text, "addons = [\"%s\"]\n[order]\nn = 5\nlo = 5\nhi = 150\n", ADDON_ORDER_PATH);
    CHECK(reload(e, text) == GPTPS_OK);
    snprintf(text, sizeof text, "addons = [\"%s\"]\n[order]\nn = 50\nlo = 5\nhi = 150\n", ADDON_ORDER_PATH);
    CHECK(reload(e, text) == GPTPS_OK);
    CHECK(leak(e, &nr, &ns, &np) && nr == 2 && ns != GPTPS_OK && np == 0);
    gptps_shutdown(e);
}
#endif

/* ---- 10) a chain at definition time ---- */
static void test_chain_define(int o)
{
    gptps *e;
    reset();
    e = open_with(o ? "[chain]\nz = 30\ny = 20\nx = 10\n" : "[chain]\nx = 10\ny = 20\nz = 30\n");
    if (!e) return;
    g_e = e;
    log_clear();
    CHECK(gptps_settings_hold(e, 1) == GPTPS_OK);
    define(e, "chain.x", rd_x, wr_x);
    define(e, "chain.y", rd_y, wr_y);
    define(e, "chain.z", rd_z, wr_z);
    CHECK(x_v == 1 && y_v == 2 && z_v == 3);           /* the values wait while it holds */
    CHECK(gptps_settings_hold(e, 0) == GPTPS_OK);
    CHECK(x_v == 10 && y_v == 20 && z_v == 30);
    CHECK(chain_refusals == 0 && log_count("refused") == 0 && gptps_config_check(e) == GPTPS_OK);
    gptps_shutdown(e);
    g_e = NULL;
}

int main(void)
{
    int o;
    gptps_set_log_sink(sink, NULL);
    for (o = 0; o < 2; ++o) {
        test_host(o);
        test_hold_task(o);
        test_chain_define(o);
#ifdef ADDON_ORDER_PATH
        test_plugin(o);
#endif
    }
    gptps_set_log_sink(NULL, NULL);
    remove(CFG);
    if (fails) { printf("test_settings_order: %d FAILED\n", fails); return 1; }
    printf("test_settings_order: OK\n");
    return 0;
}
