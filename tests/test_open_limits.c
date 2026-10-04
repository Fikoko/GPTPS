/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_open_limits.c - gptps_config.max_dead_letters and .shutdown_grace_ms (ABI 2.4).
 *
 * Both limits have a 0 of their own - keep every dead letter, wait forever - and a
 * default that is not 0, so gptps_limits, where 0 means "not set", could not carry
 * them: a host that opened with gptps_open_ex could not set either one at open. In
 * gptps_config, 0 means "not set", GPTPS_LIMIT_NONE means no limit, and anything else
 * is the limit. This holds each of those to the header's word:
 *   - 0 gives the defaults, 1024 and 30000;
 *   - an explicit value reaches the setting, and the cap evicts at it;
 *   - GPTPS_LIMIT_NONE keeps every dead letter (5,000 failures, none evicted, where
 *     the default keeps 1,024 and evicts 3,976) and a shutdown that waits (the
 *     setting reads 0, and a running body is never cancelled);
 *   - an older caller, whose struct_size ends at max_result_bytes with garbage after
 *     it, has neither field read, with or without a config file;
 *   - the config file: the struct's value wins; the file's applies when the struct
 *     says 0, and the file's 0 still means no limit; the file's value is checked even
 *     when the struct wins; a reload applies the file's value;
 *   - a live gptps_settings_set still changes both;
 *   - a bounded engine with no cap holds dead letters until max_items is reached,
 *     and then gptps_submit returns GPTPS_E_FULL until a drain frees them.
 */
#include "gptps.h"
#include "gptps_hal.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

#define CFG "open_limits.toml"

/* The log, captured: the open must say what it refused. */
static char g_log[4096];
static void sink(gptps_log_level lvl, const char *msg, void *ud)
{
    size_t n = strlen(g_log);
    (void)lvl; (void)ud;
    snprintf(g_log + n, sizeof g_log - n, "%s\n", msg);
}

/* The struct as a caller built before these two fields saw it: it ends at
 * max_result_bytes. */
#define PRE_SIZE (offsetof(gptps_config, max_result_bytes) + sizeof(uint32_t))

static void put(const char *text)
{
    FILE *f = fopen(CFG, "wb");
    CHECK(f != NULL);
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static unsigned long long setting(gptps *e, const char *key)
{
    char v[GPTPS_SETTINGS_VALUE_MAX];
    if (gptps_settings_get(e, key, v, sizeof v) != GPTPS_OK) return 0xDEADull;
    return strtoull(v, NULL, 10);
}
static unsigned long long cap_of(gptps *e)     { return setting(e, "limits.max_dead_letters"); }
static unsigned long long grace_of(gptps *e)   { return setting(e, "limits.shutdown_grace_ms"); }
static unsigned long long evicted_of(gptps *e) { return setting(e, "stats.dead_letters_evicted"); }

static void cfg_init(gptps_config *cfg, gptps_run_mode mode)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->struct_size = sizeof *cfg;
    cfg->limits.struct_size = sizeof cfg->limits;
    cfg->mode = mode;
}

static gptps *open_with(uint32_t dead, uint32_t grace, const char *path)
{
    gptps_config cfg;
    gptps *e = NULL;
    cfg_init(&cfg, GPTPS_RUN_MANUAL);
    cfg.config_path = path;
    cfg.max_dead_letters = dead;
    cfg.shutdown_grace_ms = grace;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    return e;
}

/* --- failing work, in MANUAL mode --------------------------------------------- */

static gptps_status task_fail(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_E_TASK; }

static void reg_fail(gptps *e)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "invoice"; d.run = task_fail; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.on_failure = GPTPS_ON_FAILURE_DEAD_LETTER;    /* no retries: one attempt each */
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

static void step_all(gptps *e)
{
    size_t ran;
    while (gptps_step(e, &ran) == GPTPS_OK && ran) { }
}

/* Submit n failing items, numbered from `first`, and run them all to their end. */
static void fail_n(gptps *e, uint32_t first, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; ++i) {
        uint32_t id = first + i;
        CHECK(gptps_submit(e, "invoice", &id, sizeof id, NULL) == GPTPS_OK);
    }
    step_all(e);
}

/* The drain's view: how many, and the lowest and highest number among them. */
static unsigned long g_n, g_lo, g_hi, g_order_bad, g_last;
static void dl_seen(const gptps_dead_letter *dl, void *ud)
{
    uint32_t id;
    (void)ud;
    if (dl->payload_len != sizeof id) { ++g_order_bad; return; }
    memcpy(&id, dl->payload, sizeof id);
    if (g_n && id <= g_last) ++g_order_bad;                 /* oldest first */
    if (!g_n || id < g_lo) g_lo = id;
    if (!g_n || id > g_hi) g_hi = id;
    g_last = id;
    ++g_n;
}
static size_t drain_seen(gptps *e)
{
    g_n = g_lo = g_hi = g_order_bad = g_last = 0;
    return gptps_dead_letter_drain(e, dl_seen, NULL);
}

/* --- 1, 2, 3: defaults, explicit values, no limit -------------------------- */

static void test_struct_values(void)
{
    gptps *e;

    e = open_with(0, 0, NULL);                       /* 0 = not set: the defaults */
    if (!e) return;
    CHECK(cap_of(e) == 1024);
    CHECK(grace_of(e) == 30000);
    gptps_shutdown(e);

    e = open_with(7, 1234, NULL);                    /* explicit */
    if (!e) return;
    CHECK(cap_of(e) == 7);
    CHECK(grace_of(e) == 1234);
    reg_fail(e);
    fail_n(e, 0, 10);
    CHECK(gptps_dead_letter_count(e) == 7);
    CHECK(evicted_of(e) == 3);
    CHECK(drain_seen(e) == 7);
    CHECK(g_lo == 3 && g_hi == 9 && g_order_bad == 0);  /* the three oldest went */
    gptps_shutdown(e);

    e = open_with(1, 1, NULL);                       /* the smallest limits are limits */
    if (!e) return;
    CHECK(cap_of(e) == 1);
    CHECK(grace_of(e) == 1);
    gptps_shutdown(e);

    e = open_with(GPTPS_LIMIT_NONE - 1, GPTPS_LIMIT_NONE - 1, NULL);   /* the largest */
    if (!e) return;
    CHECK(cap_of(e) == 4294967294ull);
    CHECK(grace_of(e) == 4294967294ull);
    gptps_shutdown(e);

    e = open_with(GPTPS_LIMIT_NONE, GPTPS_LIMIT_NONE, NULL);           /* no limit */
    if (!e) return;
    CHECK(cap_of(e) == 0);                           /* the setting's own "no limit" */
    CHECK(grace_of(e) == 0);                         /* the setting's own "wait forever" */
    reg_fail(e);
    fail_n(e, 0, 3000);
    CHECK(gptps_dead_letter_count(e) == 3000);
    CHECK(evicted_of(e) == 0);
    /* a live set still changes it, and the cap bites at once */
    CHECK(gptps_settings_set(e, "limits.max_dead_letters", "3") == GPTPS_OK);
    fail_n(e, 3000, 5);
    CHECK(gptps_dead_letter_count(e) == 3);
    CHECK(evicted_of(e) == 3002);
    CHECK(drain_seen(e) == 3);
    CHECK(g_lo == 3002 && g_hi == 3004 && g_order_bad == 0);
    CHECK(gptps_settings_set(e, "limits.shutdown_grace_ms", "250") == GPTPS_OK);
    CHECK(grace_of(e) == 250);
    CHECK(gptps_settings_set(e, "limits.max_dead_letters", "0") == GPTPS_OK);  /* and back */
    fail_n(e, 4000, 1500);
    CHECK(gptps_dead_letter_count(e) == 1500);
    gptps_shutdown(e);
}

/* The reported night: 5,000 invoices fail with no one draining. By default the list
 * keeps the newest 1,024 and drops 3,976, counted only in stats.dead_letters_evicted.
 * With GPTPS_LIMIT_NONE every one is kept, in order. */
static void test_reported_night(void)
{
    static const uint32_t caps[2] = { 0, GPTPS_LIMIT_NONE };
    int k;
    for (k = 0; k < 2; ++k) {
        gptps *e = open_with(caps[k], 0, NULL);
        size_t kept;
        unsigned long long evicted;
        if (!e) return;
        reg_fail(e);
        fail_n(e, 0, 5000);
        kept = gptps_dead_letter_count(e);
        evicted = evicted_of(e);
        printf("  5000 failures, max_dead_letters = %s: %lu kept, %llu evicted\n",
               k ? "GPTPS_LIMIT_NONE" : "0 (default)", (unsigned long)kept, evicted);
        CHECK(drain_seen(e) == kept);
        CHECK(g_order_bad == 0);
        if (k == 0) {
            CHECK(kept == 1024 && evicted == 3976);
            CHECK(g_lo == 3976 && g_hi == 4999);
        } else {
            CHECK(kept == 5000 && evicted == 0);
            CHECK(g_lo == 0 && g_hi == 4999);
        }
        gptps_shutdown(e);
    }
}

/* --- a shutdown that waits ---------------------------------------------------- */

static gptps_mutex *g_m;
static int g_started, g_saw_cancel, g_finished, g_ended;
static uint64_t g_body_ms;

static void on_ev(const gptps_event *ev, void *ud)
{
    (void)ud;
    gptps_mutex_lock(g_m);
    if (ev->kind == GPTPS_EV_STARTED)  ++g_started;
    if (ev->kind == GPTPS_EV_FINISHED) { ++g_finished; ++g_ended; }
    if (ev->kind == GPTPS_EV_FAILED)   ++g_ended;
    gptps_mutex_unlock(g_m);
}
static int read_int(int *p) { int v; gptps_mutex_lock(g_m); v = *p; gptps_mutex_unlock(g_m); return v; }

/* Runs for g_body_ms unless cancelled, and says which. */
static gptps_status task_slow(gptps_ctx *ctx, void *ud)
{
    uint64_t t0 = gptps_now_ms(NULL);
    (void)ud;
    while (gptps_now_ms(NULL) - t0 < g_body_ms) {
        if (gptps_is_cancelled(ctx)) {
            gptps_mutex_lock(g_m); ++g_saw_cancel; gptps_mutex_unlock(g_m);
            return GPTPS_E_CANCELLED;
        }
    }
    return GPTPS_OK;
}

/* One body, running for up to body_ms, when gptps_shutdown is called. */
static void shutdown_with_one_running(uint32_t grace, uint64_t body_ms)
{
    gptps_config cfg;
    gptps *e = NULL;
    gptps_task_def d;
    uint64_t t0;
    cfg_init(&cfg, GPTPS_RUN_THREADED);
    cfg.limits.max_concurrent_tasks = 1;
    cfg.shutdown_grace_ms = grace;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (!e) return;
    g_body_ms = body_ms;
    if (grace == GPTPS_LIMIT_NONE) CHECK(grace_of(e) == 0);
    else CHECK(grace_of(e) == grace);
    gptps_mutex_lock(g_m); g_started = g_saw_cancel = g_finished = g_ended = 0; gptps_mutex_unlock(g_m);
    CHECK(gptps_set_event_cb(e, on_ev, NULL) == GPTPS_OK);
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "slow"; d.run = task_slow; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(gptps_submit(e, "slow", NULL, 0, NULL) == GPTPS_OK);
    t0 = gptps_now_ms(NULL);
    while (read_int(&g_started) < 1 && gptps_now_ms(NULL) - t0 < 10000) { }
    CHECK(read_int(&g_started) == 1);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    CHECK(read_int(&g_ended) == 1);
}

static void test_grace(void)
{
    /* an explicit 30 ms grace, from the struct, cancels a body that would run 20 s */
    shutdown_with_one_running(30, 20000);
    CHECK(read_int(&g_saw_cancel) == 1);
    CHECK(read_int(&g_finished) == 0);
    /* GPTPS_LIMIT_NONE waits for one to finish on its own */
    shutdown_with_one_running(GPTPS_LIMIT_NONE, 300);
    CHECK(read_int(&g_saw_cancel) == 0);
    CHECK(read_int(&g_finished) == 1);
}

/* --- an older caller ---------------------------------------------------------- */

/* A caller built before the two fields: struct_size ends at max_result_bytes, and the
 * bytes after it are whatever its stack held. */
static void test_older_caller(void)
{
    union { gptps_config c; unsigned char b[sizeof(gptps_config) + 16]; } u;
    gptps *e = NULL;
    CHECK(offsetof(gptps_config, max_dead_letters) >= PRE_SIZE);   /* not in old padding */
    CHECK(sizeof(gptps_config) > PRE_SIZE);
    memset(&u, 0xA5, sizeof u);                     /* 0xA5A5A5A5 would be a cap of 2779096485 */
    memset(&u.c, 0, PRE_SIZE);
    u.c.struct_size = PRE_SIZE;
    u.c.limits.struct_size = sizeof u.c.limits;
    u.c.mode = GPTPS_RUN_MANUAL;
    CHECK(u.c.max_dead_letters == 0xA5A5A5A5u && u.c.shutdown_grace_ms == 0xA5A5A5A5u);

    CHECK(gptps_open_ex(&u.c, &e) == GPTPS_OK);    /* no file: open_engine reads the caller's struct */
    if (!e) return;
    CHECK(cap_of(e) == 1024);
    CHECK(grace_of(e) == 30000);
    gptps_shutdown(e); e = NULL;

    put("[scheduler]\nreserve_after_skips = 3\n");
    u.c.config_path = CFG;                          /* with a file: through the open's copy */
    CHECK(gptps_open_ex(&u.c, &e) == GPTPS_OK);
    if (!e) return;
    CHECK(cap_of(e) == 1024);
    CHECK(grace_of(e) == 30000);
    gptps_shutdown(e); e = NULL;

    put("[limits]\nmax_dead_letters = 5\nshutdown_grace_ms = 600\n");
    CHECK(gptps_open_ex(&u.c, &e) == GPTPS_OK);    /* the file's values apply */
    if (!e) return;
    CHECK(cap_of(e) == 5);
    CHECK(grace_of(e) == 600);
    CHECK(gptps_config_check(e) == GPTPS_OK);
    gptps_shutdown(e);
}

/* --- the config file ---------------------------------------------------------- */

static void test_file(void)
{
    gptps *e;

    put("[limits]\nmax_dead_letters = 9\nshutdown_grace_ms = 900\n");
    e = open_with(7, 700, CFG);                      /* the struct wins */
    if (!e) return;
    CHECK(cap_of(e) == 7);
    CHECK(grace_of(e) == 700);
    CHECK(gptps_config_check(e) == GPTPS_OK);        /* the file's keys count as read */
    CHECK(gptps_settings_reload(e, NULL) == GPTPS_OK);   /* a reload applies the file */
    CHECK(cap_of(e) == 9);
    CHECK(grace_of(e) == 900);
    gptps_shutdown(e);

    e = open_with(GPTPS_LIMIT_NONE, GPTPS_LIMIT_NONE, CFG);   /* no limit wins too */
    if (!e) return;
    CHECK(cap_of(e) == 0);
    CHECK(grace_of(e) == 0);
    gptps_shutdown(e);

    e = open_with(0, 0, CFG);                        /* not set: the file's */
    if (!e) return;
    CHECK(cap_of(e) == 9);
    CHECK(grace_of(e) == 900);
    gptps_shutdown(e);

    e = open_with(7, 0, CFG);                        /* each field on its own */
    if (!e) return;
    CHECK(cap_of(e) == 7);
    CHECK(grace_of(e) == 900);
    gptps_shutdown(e);

    put("[limits]\nmax_dead_letters = 0\nshutdown_grace_ms = 0\n");
    e = open_with(0, 0, CFG);                        /* the file's 0 is still "no limit" */
    if (!e) return;
    CHECK(cap_of(e) == 0);
    CHECK(grace_of(e) == 0);
    gptps_shutdown(e);
    e = open_with(7, 700, CFG);
    if (!e) return;
    CHECK(cap_of(e) == 7);
    CHECK(grace_of(e) == 700);
    gptps_shutdown(e);

    put("[limits]\nmax_dead_letters = 4294967295\n");
    e = open_with(0, 0, CFG);                        /* the file can still say exactly that */
    if (!e) return;
    CHECK(cap_of(e) == 4294967295ull);
    gptps_shutdown(e);

    put("[limits]\nmax_dead_letters = -1\n");        /* checked even when the struct wins */
    e = NULL;
    gptps_set_log_sink(sink, NULL);
    {
        gptps_config cfg;
        cfg_init(&cfg, GPTPS_RUN_MANUAL);
        cfg.config_path = CFG;
        cfg.max_dead_letters = 7;
        g_log[0] = 0;
        CHECK(gptps_open_ex(&cfg, &e) == GPTPS_E_CONFIG);
        CHECK(e == NULL);
        CHECK(strstr(g_log, CFG ":2: limits.max_dead_letters: -1 must be a whole number between 0 and 4294967295") != NULL);
        cfg.max_dead_letters = 0;                    /* the same words when the file's would apply */
        g_log[0] = 0;
        CHECK(gptps_open_ex(&cfg, &e) == GPTPS_E_CONFIG);
        CHECK(e == NULL);
        CHECK(strstr(g_log, CFG ":2: limits.max_dead_letters: -1 must be a whole number between 0 and 4294967295") != NULL);
    }
    put("[limits]\nshutdown_grace_ms = 4294967296\n");
    {
        gptps_config cfg;
        cfg_init(&cfg, GPTPS_RUN_MANUAL);
        cfg.config_path = CFG;
        cfg.shutdown_grace_ms = 700;
        g_log[0] = 0;
        CHECK(gptps_open_ex(&cfg, &e) == GPTPS_E_CONFIG);
        CHECK(e == NULL);
        CHECK(strstr(g_log, CFG ":2: limits.shutdown_grace_ms: 4294967296 must be a whole number between 0 and 4294967295") != NULL);
    }
    gptps_set_log_sink(NULL, NULL);
}

/* --- bounded mode ------------------------------------------------------------- */

static void test_bounded(void)
{
    static const uint32_t caps[2] = { GPTPS_LIMIT_NONE, 0 };   /* no limit, and 1024 > max_items */
    int k;
    for (k = 0; k < 2; ++k) {
        gptps_config cfg;
        gptps *e = NULL;
        uint32_t id = 99;
        cfg_init(&cfg, GPTPS_RUN_MANUAL);
        cfg.max_items = 8;
        cfg.max_payload_bytes = sizeof(uint32_t);
        cfg.max_dead_letters = caps[k];
        CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
        if (!e) return;
        reg_fail(e);
        fail_n(e, 0, 8);
        CHECK(gptps_dead_letter_count(e) == 8);       /* every item is a dead letter */
        CHECK(evicted_of(e) == 0);
        CHECK(gptps_submit(e, "invoice", &id, sizeof id, NULL) == GPTPS_E_FULL);
        CHECK(drain_seen(e) == 8);
        CHECK(g_lo == 0 && g_hi == 7 && g_order_bad == 0);
        fail_n(e, 8, 8);                               /* the drain gave them back */
        CHECK(gptps_dead_letter_count(e) == 8);
        gptps_shutdown(e);
    }
    {   /* a cap below max_items: dead letters hold at most that many items */
        gptps_config cfg;
        gptps *e = NULL;
        uint32_t id = 99;
        cfg_init(&cfg, GPTPS_RUN_MANUAL);
        cfg.max_items = 8;
        cfg.max_payload_bytes = sizeof(uint32_t);
        cfg.max_dead_letters = 3;
        CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
        if (!e) return;
        reg_fail(e);
        fail_n(e, 0, 8);
        CHECK(gptps_dead_letter_count(e) == 3);
        CHECK(evicted_of(e) == 5);
        fail_n(e, 8, 5);                               /* the other five are free */
        CHECK(gptps_dead_letter_count(e) == 3);
        CHECK(evicted_of(e) == 10);
        for (id = 0; id < 5; ++id) CHECK(gptps_submit(e, "invoice", &id, sizeof id, NULL) == GPTPS_OK);
        CHECK(gptps_submit(e, "invoice", &id, sizeof id, NULL) == GPTPS_E_FULL);
        step_all(e);
        CHECK(gptps_dead_letter_count(e) == 3);
        CHECK(drain_seen(e) == 3);
        gptps_shutdown(e);
    }
}

int main(void)
{
    g_m = gptps_mutex_create();
    if (!g_m) { printf("no mutex\n"); return 1; }
    test_struct_values();
    test_reported_night();
    test_grace();
    test_older_caller();
    test_file();
    test_bounded();
    remove(CFG);
    gptps_mutex_destroy(g_m);
    if (fails) { printf("%d open-limit check(s) FAILED\n", fails); return 1; }
    printf("all open-limit checks passed\n");
    return 0;
}
