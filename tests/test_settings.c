/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_settings.c - the unified settings registry (Phase 1): registration,
 * introspection, typed get/set with validation, and hot-apply (observed by
 * reading the live value back). Headless / portable.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static gptps_status noop(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_OK; }

/* change-watch capture */
static int  w_fired;
static char w_key[128], w_val[128];
static void on_change(const char *key, const char *value, void *ud)
{
    (void)ud; ++w_fired;
    snprintf(w_key, sizeof w_key, "%s", key);
    snprintf(w_val, sizeof w_val, "%s", value);
}

static void reg(gptps *e, const char *name)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = noop; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

static int has_key(gptps *e, const char *key)
{
    size_t i, n = gptps_settings_count(e);
    for (i = 0; i < n; ++i) {
        gptps_setting_info info; memset(&info, 0, sizeof info); info.struct_size = sizeof info;
        if (gptps_settings_get_info(e, i, &info) == GPTPS_OK && strcmp(info.key, key) == 0) return 1;
    }
    return 0;
}


/* ---- 1.1.0 regressions: numeric range enforcement, and the escaped-quote round
 * trip. Its own engine and its own files, so nothing above depends on it. ---- */
static void test_range_and_escape(void)
{
    gptps *e = NULL;
    char b[GPTPS_SETTINGS_VALUE_MAX];
    FILE *f;

    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    if (!e) return;

    /* (1) A value that fits unsigned long long but NOT the uint32_t its write
     * callback casts to. It used to validate fine and then truncate to 0 - so the
     * intake bound the operator had just set silently became "unbounded", the exact
     * opposite of what was asked for. Assert the refusal AND that the bound already
     * in force survived it. */
    CHECK(gptps_settings_set(e, "limits.max_intake_depth", "4096") == GPTPS_OK);
    CHECK(gptps_settings_set(e, "limits.max_intake_depth", "4294967296") != GPTPS_OK);
    CHECK(gptps_settings_get(e, "limits.max_intake_depth", b, sizeof b) == GPTPS_OK);
    CHECK(strcmp(b, "4096") == 0);

    /* (2) A value strtoull/strtoll SATURATES on. errno == ERANGE was not checked,
     * so the clamped result was applied as if it had been written - and
     * ULLONG_MAX/LLONG_MAX is precisely the value that reads as "no limit". */
    CHECK(gptps_settings_set(e, "limits.max_memory_bytes", "1048576") == GPTPS_OK);
    CHECK(gptps_settings_set(e, "limits.max_memory_bytes", "99999999999999999999999") != GPTPS_OK);
    CHECK(gptps_settings_get(e, "limits.max_memory_bytes", b, sizeof b) == GPTPS_OK);
    CHECK(strcmp(b, "1048576") == 0);

    /* the same on a signed knob, at both ends */
    CHECK(gptps_define_global(e, "app.signed", GPTPS_SETTING_INT, "7", NULL, 0) == GPTPS_OK);
    CHECK(gptps_settings_set(e, "app.signed", "99999999999999999999999") != GPTPS_OK);
    CHECK(gptps_settings_set(e, "app.signed", "-99999999999999999999999") != GPTPS_OK);
    CHECK(gptps_settings_get(e, "app.signed", b, sizeof b) == GPTPS_OK && strcmp(b, "7") == 0);
    /* not over-strict: it is a range check, not a digit-count check */
    CHECK(gptps_settings_set(e, "app.signed", "-2147483648") == GPTPS_OK);

    /* (3) the engine keeps its own copy of that grammar to validate a
     * gptps_define_global default; it had the same gap. */
    CHECK(gptps_define_global(e, "app.huge", GPTPS_SETTING_INT,
                              "99999999999999999999999", NULL, 0) == GPTPS_E_CONFIG);
    CHECK(gptps_define_global(e, "app.hugeu", GPTPS_SETTING_UINT,
                              "99999999999999999999999", NULL, 0) == GPTPS_E_CONFIG);

    /* (4) strip_comment() ignored backslash escapes, so a string value carrying an
     * escaped quote ahead of a '#' was cut at the '#' when the file was read back.
     * The save side was always correct, which is what made it a silent data loss:
     * it only appeared one reload later. */
    CHECK(gptps_define_global(e, "app.motd", GPTPS_SETTING_STRING, "x", NULL, 0) == GPTPS_OK);
    CHECK(gptps_settings_set(e, "app.motd", "a\"b#c") == GPTPS_OK);
    CHECK(gptps_settings_save(e, "settings_esc.toml") == GPTPS_OK);
    CHECK(gptps_settings_set(e, "app.motd", "clobbered") == GPTPS_OK);   /* so the reload must do work */
    CHECK(gptps_settings_reload(e, "settings_esc.toml") == GPTPS_OK);
    CHECK(gptps_settings_get(e, "app.motd", b, sizeof b) == GPTPS_OK);
    CHECK(strcmp(b, "a\"b#c") == 0);                                     /* used to be "a" */
    remove("settings_esc.toml");

    /* (5) the reload path must refuse an out-of-range file value too, and leave the
     * live value standing - it used to install LLONG_MAX over it. */
    f = fopen("settings_range.toml", "wb");
    CHECK(f != NULL);
    if (f) {
        fputs("[limits]\nmax_memory_bytes = 99999999999999999999999\n", f);
        fclose(f);
        CHECK(gptps_settings_reload(e, "settings_range.toml") == GPTPS_E_CONFIG);
        CHECK(gptps_settings_get(e, "limits.max_memory_bytes", b, sizeof b) == GPTPS_OK);
        CHECK(strcmp(b, "1048576") == 0);
        remove("settings_range.toml");
    }

    gptps_shutdown(e);
}

int main(void)
{
    gptps *e = NULL;
    char buf[GPTPS_SETTINGS_VALUE_MAX];
    size_t i, n, core_n;

    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    if (!e) return 1;

    /* Core settings present before any task. Asserted by KEY plus a per-task DELTA
     * rather than an absolute total: the total is not a contract, so pinning it made
     * every new core knob look like a regression here. What must hold is that the
     * documented keys exist and that each task contributes exactly six. */
    core_n = gptps_settings_count(e);
    CHECK(core_n >= 4);
    CHECK(has_key(e, "limits.max_memory_bytes"));
    CHECK(has_key(e, "limits.max_concurrent_tasks"));
    CHECK(has_key(e, "limits.max_intake_depth"));
    CHECK(has_key(e, "limits.shutdown_grace_ms"));
    CHECK(has_key(e, "limits.max_dead_letters"));
    CHECK(has_key(e, "stats.dead_letters_evicted"));
    CHECK(has_key(e, "scheduler.reserve_after_skips"));

    /* a task adds its six knobs */
    reg(e, "work");
    CHECK(gptps_settings_count(e) == core_n + 6);
    CHECK(has_key(e, "tasks.work.timeout_seconds"));
    CHECK(has_key(e, "tasks.work.on_failure"));
    reg(e, "bulk");
    CHECK(gptps_settings_count(e) == core_n + 12);

    /* get a default */
    CHECK(gptps_settings_get(e, "scheduler.reserve_after_skips", buf, sizeof buf) == GPTPS_OK);
    CHECK(strcmp(buf, "8") == 0);

    /* set + hot-apply (read back the live value) */
    CHECK(gptps_settings_set(e, "scheduler.reserve_after_skips", "3") == GPTPS_OK);
    CHECK(gptps_settings_get(e, "scheduler.reserve_after_skips", buf, sizeof buf) == GPTPS_OK);
    CHECK(strcmp(buf, "3") == 0);

    /* per-task set + read back */
    CHECK(gptps_settings_set(e, "tasks.work.priority", "5") == GPTPS_OK);
    CHECK(gptps_settings_get(e, "tasks.work.priority", buf, sizeof buf) == GPTPS_OK);
    CHECK(strcmp(buf, "5") == 0);
    CHECK(gptps_settings_set(e, "tasks.work.on_failure", "drop") == GPTPS_OK);
    CHECK(gptps_settings_get(e, "tasks.work.on_failure", buf, sizeof buf) == GPTPS_OK);
    CHECK(strcmp(buf, "drop") == 0);

    /* validation: bad enum / bad type / out-of-range / unknown key are rejected,
     * and a rejected set leaves the value unchanged */
    CHECK(gptps_settings_set(e, "tasks.work.on_failure", "bogus") == GPTPS_E_CONFIG);
    CHECK(gptps_settings_get(e, "tasks.work.on_failure", buf, sizeof buf) == GPTPS_OK);
    CHECK(strcmp(buf, "drop") == 0);                                  /* unchanged */
    CHECK(gptps_settings_set(e, "scheduler.reserve_after_skips", "abc") == GPTPS_E_CONFIG);
    CHECK(gptps_settings_set(e, "limits.max_concurrent_tasks", "0") == GPTPS_E_CONFIG); /* min 1 */
    CHECK(gptps_settings_set(e, "nope.nope", "1") == GPTPS_E_NOTFOUND);
    CHECK(gptps_settings_get(e, "nope.nope", buf, sizeof buf) == GPTPS_E_NOTFOUND);

    /* introspection metadata: hot flags + value/defval populated */
    n = gptps_settings_count(e);
    for (i = 0; i < n; ++i) {
        gptps_setting_info info; memset(&info, 0, sizeof info); info.struct_size = sizeof info;
        CHECK(gptps_settings_get_info(e, i, &info) == GPTPS_OK);
        CHECK(info.key && info.key[0]);
        if (strcmp(info.key, "scheduler.reserve_after_skips") == 0) {
            CHECK(info.hot == 1);
            CHECK(strcmp(info.value, "3") == 0);     /* current */
            CHECK(strcmp(info.defval, "8") == 0);    /* default snapshotted at registration */
        }
        if (strcmp(info.key, "limits.max_concurrent_tasks") == 0)
            CHECK(info.hot == 0);                    /* restart-only */
    }

    /* ---- change-watch: fires on a successful set, not on a rejected one ---- */
    CHECK(gptps_settings_watch(e, on_change, NULL) == GPTPS_OK);
    w_fired = 0;
    CHECK(gptps_settings_set(e, "tasks.bulk.timeout_seconds", "42") == GPTPS_OK);
    CHECK(w_fired == 1);
    CHECK(strcmp(w_key, "tasks.bulk.timeout_seconds") == 0 && strcmp(w_val, "42") == 0);
    w_fired = 0;
    CHECK(gptps_settings_set(e, "tasks.bulk.timeout_seconds", "-1") == GPTPS_E_CONFIG); /* uint rejects */
    CHECK(w_fired == 0);

    /* ---- Phase 2: persistence round-trip ---- */
    CHECK(gptps_settings_set(e, "limits.max_memory_bytes", "12345678") == GPTPS_OK);
    CHECK(gptps_settings_save(e, "settings_rt.toml") == GPTPS_OK);
    CHECK(gptps_settings_save(e, NULL) == GPTPS_E_INVAL);   /* engine opened without a path */
    {
        FILE *f = fopen("settings_rt.toml", "rb"); char rd[8192]; size_t got = 0;
        CHECK(f != NULL);
        if (f) { got = fread(rd, 1, sizeof rd - 1, f); rd[got] = 0; fclose(f); }
        CHECK(strstr(rd, "[scheduler]") != NULL);
        CHECK(strstr(rd, "reserve_after_skips = 3") != NULL);
        CHECK(strstr(rd, "[tasks.work]") != NULL);
        CHECK(strstr(rd, "priority = 5") != NULL);
        CHECK(strstr(rd, "on_failure = \"drop\"") != NULL);
        CHECK(strstr(rd, "max_memory_bytes = 12345678") != NULL);
    }
    { FILE *t = fopen("settings_rt.toml.tmp", "rb"); CHECK(t == NULL); if (t) fclose(t); } /* atomic: no temp left */
    gptps_shutdown(e);

    /* reopen a fresh engine on the saved file => values applied; then reload reverts a live edit */
    {
        gptps *e2 = NULL; char b[GPTPS_SETTINGS_VALUE_MAX];
        CHECK(gptps_open("settings_rt.toml", &e2) == GPTPS_OK);
        if (e2) {
            reg(e2, "work");   /* registering applies [tasks.work] from the reopened file */
            CHECK(gptps_settings_get(e2, "scheduler.reserve_after_skips", b, sizeof b) == GPTPS_OK && strcmp(b, "3") == 0);
            CHECK(gptps_settings_get(e2, "limits.max_memory_bytes", b, sizeof b) == GPTPS_OK && strcmp(b, "12345678") == 0);
            CHECK(gptps_settings_get(e2, "tasks.work.priority", b, sizeof b) == GPTPS_OK && strcmp(b, "5") == 0);
            CHECK(gptps_settings_get(e2, "tasks.work.on_failure", b, sizeof b) == GPTPS_OK && strcmp(b, "drop") == 0);
            CHECK(gptps_settings_set(e2, "scheduler.reserve_after_skips", "99") == GPTPS_OK);
            CHECK(gptps_settings_reload(e2, NULL) == GPTPS_OK);   /* uses the open path */
            CHECK(gptps_settings_get(e2, "scheduler.reserve_after_skips", b, sizeof b) == GPTPS_OK && strcmp(b, "3") == 0);
            CHECK(gptps_settings_save(e2, NULL) == GPTPS_OK);     /* default-path save works */
            gptps_shutdown(e2);
        }
        remove("settings_rt.toml");
    }

    test_range_and_escape();

    if (fails) { printf("%d settings check(s) FAILED\n", fails); return 1; }
    printf("all settings checks passed\n");
    return 0;
}
