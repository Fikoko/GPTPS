/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_config_strict.c - the config file is checked as it is read, and saved in place.
 *
 *   - every kind of mistake fails gptps_open with GPTPS_E_CONFIG, and the log says
 *     where (file:line), which key, why, and what was probably meant;
 *   - every mistake in a file is reported in one open, not one per attempt;
 *   - a key only a later definition can claim waits for it, is applied then, and
 *     gptps_config_check reports what nothing claimed; without the call, the first
 *     submit warns once;
 *   - the file carries a whole deployment: named resources, per-task costs, bounded mode;
 *   - a reload makes the same checks;
 *   - gptps_settings_save edits the file in place: the values set live are rewritten
 *     on their own lines, a missing one goes next to its siblings, every other byte
 *     is kept, a file that does not parse is left alone, and the result reopens to
 *     the same values;
 *   - gptps_settings_set_ex says why it refuses.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "gptps.h"
#include "gptps_hal.h"        /* threads, for the define/unregister race */
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#  include <sys/stat.h>
#endif

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

#define CFG   "cfg_strict.toml"
#define CFG2  "cfg_strict_2.toml"
#define FRESH "cfg_strict_fresh.toml"

/* ---- the log, captured (the dispatcher writes to it too: hence the lock) ---- */
static char g_log[32768];
static size_t g_loglen;
static gptps_mutex *g_logm;
static void sink(gptps_log_level lvl, const char *msg, void *ud)
{
    int k;
    (void)ud;
    gptps_mutex_lock(g_logm);
    k = snprintf(g_log + g_loglen, sizeof g_log - g_loglen, "%d %s\n", (int)lvl, msg);
    if (k > 0 && g_loglen + (size_t)k < sizeof g_log) g_loglen += (size_t)k;
    gptps_mutex_unlock(g_logm);
}
static void clear_log(void) { gptps_mutex_lock(g_logm); g_loglen = 0; g_log[0] = 0; gptps_mutex_unlock(g_logm); }
static int count_logged(const char *s)
{
    int n = 0;
    const char *p;
    gptps_mutex_lock(g_logm);
    for (p = g_log; (p = strstr(p, s)) != NULL; p += strlen(s)) ++n;
    gptps_mutex_unlock(g_logm);
    return n;
}
static int logged(const char *s) { return count_logged(s) > 0; }

/* ---- files ---- */
static void put_bytes(const char *path, const char *b, size_t n)
{
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (!f) return;
    fwrite(b, 1, n, f);
    fclose(f);
}
static void put(const char *path, const char *text) { put_bytes(path, text, strlen(text)); }
static const char *slurp(const char *path)
{
    static char buf[16384];
    size_t n;
    FILE *f = fopen(path, "rb");
    buf[0] = 0;
    if (!f) return buf;
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

static int count_in(const char *hay, const char *needle)
{
    int n = 0;
    const char *p;
    for (p = hay; (p = strstr(p, needle)) != NULL; p += strlen(needle)) ++n;
    return n;
}

static gptps_status noop(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }
static gptps_status reg(gptps *e, const char *name)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = noop; d.exec = GPTPS_EXEC_INPROC;
    return gptps_register_task(e, &d);
}
static int has_value(gptps *e, const char *key, const char *want)
{
    char v[GPTPS_SETTINGS_VALUE_MAX];
    if (gptps_settings_get(e, key, v, sizeof v) != GPTPS_OK) return 0;
    if (strcmp(v, want) != 0) printf("  %s = %s, want %s\n", key, v, want);
    return strcmp(v, want) == 0;
}

/* gptps_open must refuse `text`, and the log must say `want`. */
static void refused(const char *text, const char *want)
{
    gptps *e = NULL;
    put(CFG, text);
    clear_log();
    CHECK(gptps_open(CFG, &e) == GPTPS_E_CONFIG);
    CHECK(e == NULL);
    if (!logged(want)) printf("  wanted \"%s\" in:\n%s", want, g_log);
    CHECK(logged(want));
    if (e) gptps_shutdown(e);
}

static void test_mistakes(void)
{
    gptps *e = NULL;
    /* the parser */
    refused("[limits\n", CFG ":1: the table name has no closing ]");
    refused("[limits]\nmax_concurrent_tasks = = 4\n", CFG ":2: limits.max_concurrent_tasks: = 4 is not a value");
    refused("[limits]\nmax_concurrent_tasks = 4\nmax_concurrent_tasks = 5\n",
            "limits.max_concurrent_tasks is set twice (first on line 2)");
    {
        static const char nul[] = "[limits]\n\0max_concurrent_tasks = 4\n";
        put_bytes(CFG, nul, sizeof nul - 1);
    }
    clear_log();
    CHECK(gptps_open(CFG, &e) == GPTPS_E_CONFIG);
    CHECK(logged("contains a NUL byte"));
    if (e) { gptps_shutdown(e); e = NULL; }
    /* a value out of range, or of the wrong type */
    refused("[limits]\nmax_concurrent_tasks = 70000\n",
            CFG ":2: limits.max_concurrent_tasks: 70000 must be a whole number between 0 and 65536");
    refused("[limits]\nmax_concurrent_tasks = \"four\"\n",
            "limits.max_concurrent_tasks: expects a whole number, not the string \"four\"\n");
    refused("[limits]\nmax_concurrent_tasks = \"4\"\n",      /* TOML's types: a quoted number is a string */
            "limits.max_concurrent_tasks: expects a whole number, not the string \"4\" - drop the quotes");
    refused("[scheduler]\nreserve_after_skips = true\n", "expects a whole number, not true");
    refused("[tasks.t]\non_failure = 1\n", "tasks.t.on_failure: expects a \"string\" - put 1 in quotes");
    refused("[tasks.t]\nmax_retries = 2.5\n", "tasks.t.max_retries: expects a whole number, not 2.5");
    refused("[limits]\nmax_memory_gb = nan\n", "nan is not a value");
    refused("[limits]\nmax_memory_gb = 0x1.8p1\n", "0x1.8p1 is not a number");
    refused("[limits]\nmax_memory_gb = 1e999\n", "1e999 is too large a number");
    refused("s = \"C:\\plugins\"\n", "an unknown escape after a backslash");
    refused("s = \"a\x01b\"\n", "a control character in a quoted string");
    refused("# notes\r[limits]\rmax_concurrent_tasks = 70000\r",
            CFG ":1: a carriage return inside the line - if the file's lines end in CR alone");
    refused("[limits]\nshutdown_grace_ms = -1\n", "limits.shutdown_grace_ms: -1 must be a whole number");
    refused("[resources]\ngpu = -1\n", "resources.gpu: a resource's budget: -1 must be a whole number");
    refused("[tasks.t.resources]\ngpu = \"x\"\n", "tasks.t.resources.gpu: a resource cost: expects a whole number, not the string \"x\"");
    /* per-task values are checked when the file is read, before the task exists */
    refused("[tasks.t]\non_failure = \"retry\"\n",
            CFG ":2: tasks.t.on_failure: retry must be one of: dead_letter, requeue, drop");
    refused("[tasks.t]\nmax_retries = -1\n", "tasks.t.max_retries: -1 must be a whole number");
    refused("[tasks.t]\npriority = 99999999999\n", "tasks.t.priority: 99999999999 must be a whole number between");
    /* the same key, spelt as one quoted key: checked the same */
    refused("\"tasks.t.max_retries\" = -1\n", CFG ":1: tasks.t.max_retries: -1 must be a whole number");
    /* a key the engine's own tables do not have, or a table one letter off */
    refused("[limits]\nmax_memroy_bytes = 1\n",
            "limits.max_memroy_bytes: [limits] has no such key (did you mean limits.max_memory_bytes?)");
    refused("[task_defaults]\nmax_retrys = 1\n", "[task_defaults] has no such key (did you mean max_retries?)");
    refused("[schedular]\nreserve_after_skips = 4\n",
            "there is no [schedular] table (did you mean [scheduler]?)");
    /* an add-on that does not load */
    refused("addons = [\"./no_such_gptps_addon.so\"]\n", "the add-on ./no_such_gptps_addon.so did not load");
    refused("addons = \"./x.so\"\n", "addons: expects a [\"list\"] of add-on paths");

    /* every mistake in one open, each with its line */
    put(CFG, "[limits]\n"
             "max_concurrent_tasks = 70000\n"
             "max_memroy_bytes = 1\n"
             "\n"
             "[schedular]\n"
             "reserve_after_skips = 4\n"
             "\n"
             "[tasks.resize]\n"
             "on_failure = \"retry\"\n");
    clear_log();
    CHECK(gptps_open(CFG, &e) == GPTPS_E_CONFIG);
    CHECK(logged(CFG ":2: limits.max_concurrent_tasks"));
    CHECK(logged(CFG ":3: limits.max_memroy_bytes"));
    CHECK(logged(CFG ":6: schedular.reserve_after_skips"));
    CHECK(logged(CFG ":9: tasks.resize.on_failure"));
    CHECK(logged(CFG ": 4 errors - the engine was not opened"));
    if (e) { gptps_shutdown(e); e = NULL; }

    /* every line that does not parse, in one attempt; the keys under a [table] line
     * that does not parse are passed over rather than judged in the wrong table */
    put(CFG, "[limits]\n"
             "max_concurrent_tasks = 4\n"
             "max_memory_bytes = lots\n"
             "[scheduler\n"
             "max_concurrent_tasks = 8\n"
             "[tasks.resize]\n"
             "on_failure = \"drop\n"
             "max_retries 3\n");
    clear_log();
    CHECK(gptps_open(CFG, &e) == GPTPS_E_CONFIG);
    CHECK(logged(CFG ":3: limits.max_memory_bytes: lots is not a value - a string needs quotes"));
    CHECK(logged(CFG ":4: the table name has no closing ]"));
    CHECK(!logged(CFG ":5:"));
    CHECK(logged(CFG ":7: tasks.resize.on_failure: the string has no closing quote"));
    CHECK(logged(CFG ":8: expected = after max_retries"));
    CHECK(logged(CFG ": the file does not parse - the engine was not opened"));
    if (e) { gptps_shutdown(e); e = NULL; }
    {   /* past what one message holds, the rest are counted */
        char big[4000];
        size_t k = 0;
        int i;
        for (i = 0; i < 60; ++i) k += (size_t)snprintf(big + k, sizeof big - k, "bad line %d\n", i);
        put(CFG, big);
        clear_log();
        CHECK(gptps_open(CFG, &e) == GPTPS_E_CONFIG);
        CHECK(logged(CFG ":1: expected = after bad"));
        CHECK(logged("more lines that do not parse"));
        if (e) { gptps_shutdown(e); e = NULL; }
    }

    /* and the file the open could not read at all */
    clear_log();
    CHECK(gptps_open("no_such_dir/none.toml", &e) == GPTPS_E_CONFIG);
    CHECK(logged("no_such_dir/none.toml: cannot open the file"));
}

/* Keys only a later definition can claim. */
static void test_deferred(void)
{
    gptps *e = NULL;
    gptps_handle h;
    put(CFG, "[tasks.late]\nmax_retries = 3\n"
             "[app]\nknob = 7\n"
             "[tasks.resise]\npriority = 1\n");
    clear_log();
    CHECK(gptps_open(CFG, &e) == GPTPS_OK);
    if (!e) return;
    CHECK(gptps_config_check(e) == GPTPS_E_CONFIG);
    CHECK(logged("tasks.late.max_retries: no task named late is registered"));
    CHECK(logged("app.knob: nothing has used this key"));
    CHECK(reg(e, "late") == GPTPS_OK);                       /* applied at registration */
    CHECK(has_value(e, "tasks.late.max_retries", "3"));
    CHECK(gptps_define_global(e, "app.knob", GPTPS_SETTING_UINT, "1", "0..10", 0) == GPTPS_OK);
    CHECK(has_value(e, "app.knob", "7"));                    /* applied at definition */
    CHECK(reg(e, "resize") == GPTPS_OK);
    clear_log();
    CHECK(gptps_config_check(e) == GPTPS_E_CONFIG);          /* the typo is still there */
    CHECK(logged("tasks.resise.priority: no task named resise is registered (did you mean resize?)"));
    CHECK(!logged("tasks.late"));
    CHECK(!logged("app.knob"));
    gptps_shutdown(e); e = NULL;

    /* without gptps_config_check, the first submit warns - once */
    clear_log();
    CHECK(gptps_open(CFG, &e) == GPTPS_OK);
    if (!e) return;
    CHECK(reg(e, "late") == GPTPS_OK);
    CHECK(gptps_submit(e, "late", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_submit(e, "late", NULL, 0, &h) == GPTPS_OK);
    gptps_shutdown(e); e = NULL;        /* the dispatcher makes the report; shutdown waits for it */
    CHECK(count_logged("app.knob: nothing has used this key - seen at the first submit") == 1);
    CHECK(count_logged("tasks.resise.priority") == 1);

    /* a deferred value is checked against the definition that claims it */
    put(CFG, "[app]\nknob = 70\n");
    CHECK(gptps_open(CFG, &e) == GPTPS_OK);
    if (!e) return;
    clear_log();
    CHECK(gptps_define_global(e, "app.knob", GPTPS_SETTING_UINT, "1", "0..10", 0) == GPTPS_OK);
    CHECK(logged(CFG ":2: app.knob: 70 must be a whole number between 0 and 10"));
    CHECK(has_value(e, "app.knob", "1"));                    /* the default stands */
    CHECK(gptps_config_check(e) == GPTPS_E_CONFIG);
    gptps_shutdown(e); e = NULL;

    /* TOML's quoted parts: in a table name, and in a dotted key */
    put(CFG, "[tasks.\"resize v2\"]\nmax_retries = 3\n[tasks]\n\"late.x\".priority = 2\n");
    CHECK(gptps_open(CFG, &e) == GPTPS_OK);
    if (!e) return;
    CHECK(reg(e, "resize v2") == GPTPS_OK && reg(e, "late.x") == GPTPS_OK);
    CHECK(has_value(e, "tasks.resize v2.max_retries", "3"));
    CHECK(has_value(e, "tasks.late.x.priority", "2"));
    CHECK(gptps_config_check(e) == GPTPS_OK);
    gptps_shutdown(e); e = NULL;
    refused("[tasks.\"open\n", CFG ":1: a quoted part of the table name has no closing quote");
    refused("[tasks..x]\n", CFG ":1: the table name has an empty part");
    refused("a..b = 1\n", CFG ":1: the key has an empty part");
    refused("x = 2\n[\"\"]\nx = 1\n", CFG ":2: an empty table name");

    /* a key with dots in it means the same in the table name or in quotes */
    put(CFG, "[app]\n\"sub.knob\" = 5\n");
    CHECK(gptps_open(CFG, &e) == GPTPS_OK);
    if (!e) return;
    CHECK(gptps_define_global(e, "app.sub.knob", GPTPS_SETTING_UINT, "1", "0..10", 0) == GPTPS_OK);
    CHECK(has_value(e, "app.sub.knob", "5"));
    CHECK(gptps_config_check(e) == GPTPS_OK);
    gptps_shutdown(e); e = NULL;
    refused("[app.sub]\nknob = 5\n[app]\n\"sub.knob\" = 6\n", "app.sub.knob is set twice (first on line 2)");
}

/* Resources, what tasks cost of them, and bounded mode, from the file alone. */
static void test_deployment(void)
{
    gptps_config cfg;
    gptps *e = NULL;
    gptps_handle h;
    char payload[17];
    int i;
    put(CFG, "[resources]\ngpu = 2\n"
             "[tasks.r.resources]\ngpu = 1\n"
             "[bounded]\nmax_items = 8\nmax_payload_bytes = 16\nmax_result_bytes = 16\n");
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.mode = GPTPS_RUN_MANUAL;
    cfg.config_path = CFG;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (!e) return;
    CHECK(has_value(e, "resources.gpu", "2"));
    CHECK(reg(e, "r") == GPTPS_OK);
    CHECK(has_value(e, "tasks.r.resources.gpu", "1"));
    CHECK(has_value(e, "bounded.max_items", "8"));
    memset(payload, 'x', sizeof payload);
    CHECK(gptps_submit(e, "r", payload, sizeof payload, &h) == GPTPS_E_INVAL);   /* past 16 bytes */
    for (i = 0; i < 8; ++i) CHECK(gptps_submit(e, "r", payload, 16, &h) == GPTPS_OK);
    CHECK(gptps_submit(e, "r", payload, 16, &h) == GPTPS_E_FULL);               /* 8 items alive */
    CHECK(gptps_config_check(e) == GPTPS_OK);
    gptps_shutdown(e);
}

static void test_reload(void)
{
    gptps *e = NULL;
    put(CFG, "addons = []\n[scheduler]\nreserve_after_skips = 4\n");
    CHECK(gptps_open(CFG, &e) == GPTPS_OK);
    if (!e) return;
    CHECK(gptps_settings_set(e, "scheduler.reserve_after_skips", "9") == GPTPS_OK);
    put(CFG, "addons = []\n[scheduler]\nreserve_after_skips = 5\n[limits]\nshutdown_grace_ms = -3\n");
    clear_log();
    CHECK(gptps_settings_reload(e, NULL) == GPTPS_E_CONFIG);        /* the bad value is reported */
    CHECK(logged(CFG ":5: limits.shutdown_grace_ms: -3 must be a whole number"));
    CHECK(has_value(e, "scheduler.reserve_after_skips", "5"));      /* and the good one applied */
    CHECK(has_value(e, "limits.shutdown_grace_ms", "30000"));
    put(CFG, "[scheduler\nreserve_after_skips = 6\n");
    clear_log();
    CHECK(gptps_settings_reload(e, NULL) == GPTPS_E_CONFIG);        /* does not parse: nothing applies */
    CHECK(logged(CFG ":1: the table name has no closing ]"));
    CHECK(has_value(e, "scheduler.reserve_after_skips", "5"));
    put(CFG, "addons = []\n[scheduler]\nreserve_after_skips = 5\n");
    CHECK(gptps_settings_reload(e, NULL) == GPTPS_OK);
    CHECK(gptps_config_check(e) == GPTPS_OK);                       /* addons is claimed at a reload too */
    gptps_shutdown(e);
}

static void test_save_in_place(void)
{
    static const char hand[] =
        "# My deployment - hand-written, keep the notes\n"
        "\n"
        "[limits]\n"
        "max_concurrent_tasks = 0   # auto: one per CPU\n"
        "max_memory_gb = 8          # eight GiB\n"
        "\r\n"
        "# the scheduler\n"
        "[scheduler]\n"
        "reserve_after_skips = 8   # tuned on the old box\r\n"
        "\n"
        "[tasks.t]\n"
        "timeout_seconds = 5\n"
        "\n"
        "# trailing note\n";
    static const char broken[] = "[limits]\nmax_concurrent_tasks = = 4\n# keep me\n";
    gptps *e = NULL;
    const char *out;
    char saved[16384];

    put(CFG, hand);
    CHECK(gptps_open(CFG, &e) == GPTPS_OK);
    if (!e) return;
    CHECK(reg(e, "t") == GPTPS_OK);

    /* nothing changed: the file comes back byte for byte */
    CHECK(gptps_settings_save(e, NULL) == GPTPS_OK);
    CHECK(strcmp(slurp(CFG), hand) == 0);

    CHECK(gptps_settings_set(e, "scheduler.reserve_after_skips", "3") == GPTPS_OK);
    CHECK(gptps_settings_set(e, "limits.max_memory_bytes", "4294967296") == GPTPS_OK);
    CHECK(gptps_settings_set(e, "limits.max_intake_depth", "100") == GPTPS_OK);
    CHECK(gptps_settings_set(e, "tasks.t.priority", "7") == GPTPS_OK);
    CHECK(gptps_settings_set(e, "stats.dead_letters_evicted", "0") == GPTPS_OK);
    CHECK(reg(e, "resize v2") == GPTPS_OK);          /* a name a bare key cannot hold */
    CHECK(gptps_settings_set(e, "tasks.resize v2.max_retries", "2") == GPTPS_OK);
    CHECK(reg(e, "odd]name") == GPTPS_OK);
    CHECK(gptps_settings_set(e, "tasks.odd]name.max_retries", "4") == GPTPS_OK);
    CHECK(gptps_define_global(e, "verbose", GPTPS_SETTING_BOOL, "false", NULL, 0) == GPTPS_OK);
    CHECK(gptps_settings_set(e, "verbose", "true") == GPTPS_OK);     /* a key with no table */
    CHECK(gptps_settings_save(e, NULL) == GPTPS_OK);
    gptps_shutdown(e); e = NULL;

    out = slurp(CFG);
    snprintf(saved, sizeof saved, "%s", out);
    CHECK(strstr(saved, "# My deployment - hand-written, keep the notes\n\n") == saved);
    CHECK(strstr(saved, "max_concurrent_tasks = 0   # auto: one per CPU\n") != NULL);   /* untouched */
    CHECK(strstr(saved, "max_memory_gb = 4          # eight GiB\n") != NULL);           /* its unit kept */
    CHECK(strstr(saved, "max_memory_gb = 4          # eight GiB\nmax_intake_depth = 100\n\r\n") != NULL);
    CHECK(strstr(saved, "reserve_after_skips = 3   # tuned on the old box\r\n") != NULL); /* CRLF kept */
    CHECK(strstr(saved, "[tasks.t]\ntimeout_seconds = 5\npriority = 7\n\n# trailing note\n") != NULL);
    CHECK(strstr(saved, "[tasks.\"resize v2\"]\nmax_retries = 2\n") != NULL);   /* TOML's quoting */
    CHECK(strstr(saved, "[tasks.\"odd]name\"]\nmax_retries = 4\n") != NULL);
    CHECK(strstr(saved, "dead_letters_evicted") == NULL);                                /* a statistic */
    CHECK(strstr(saved, "max_memory_bytes") == NULL);
    /* the key with no table went above the first table and the comment on it */
    CHECK(strstr(saved, "# My deployment - hand-written, keep the notes\n\nverbose = true\n\n[limits]") == saved);

    /* it reopens to the same values, and saving it again changes nothing */
    clear_log();
    CHECK(gptps_open(CFG, &e) == GPTPS_OK);
    if (!e) { printf("%s", g_log); return; }
    CHECK(reg(e, "t") == GPTPS_OK && reg(e, "resize v2") == GPTPS_OK && reg(e, "odd]name") == GPTPS_OK);
    CHECK(gptps_define_global(e, "verbose", GPTPS_SETTING_BOOL, "false", NULL, 0) == GPTPS_OK);
    CHECK(has_value(e, "verbose", "true"));
    CHECK(has_value(e, "scheduler.reserve_after_skips", "3"));
    CHECK(has_value(e, "limits.max_memory_bytes", "4294967296"));
    CHECK(has_value(e, "limits.max_intake_depth", "100"));
    CHECK(has_value(e, "tasks.t.priority", "7"));
    CHECK(has_value(e, "tasks.resize v2.max_retries", "2"));
    CHECK(has_value(e, "tasks.odd]name.max_retries", "4"));
    CHECK(gptps_config_check(e) == GPTPS_OK);
    CHECK(gptps_settings_save(e, NULL) == GPTPS_OK);
    CHECK(strcmp(slurp(CFG), saved) == 0);

    /* a file that does not parse is not overwritten */
    put(CFG2, broken);
    clear_log();
    CHECK(gptps_settings_save(e, CFG2) == GPTPS_E_CONFIG);
    CHECK(strcmp(slurp(CFG2), broken) == 0);
    CHECK(logged("settings not saved: " CFG2 ":2:"));

    /* a new path: a copy of the loaded file - comments and all - with the live changes */
    remove(FRESH);
    CHECK(gptps_settings_set(e, "limits.shutdown_grace_ms", "5000") == GPTPS_OK);
    CHECK(gptps_settings_save(e, FRESH) == GPTPS_OK);
    out = slurp(FRESH);
    CHECK(strstr(out, "# My deployment - hand-written, keep the notes\n") == out);
    CHECK(strstr(out, "max_intake_depth = 100\nshutdown_grace_ms = 5000\n") != NULL);
    CHECK(strcmp(slurp(CFG), saved) == 0);                                 /* the loaded file is untouched */
    remove(FRESH);
    /* if the loaded file is gone, the new one gets what it had set and the live changes */
    remove(CFG);
    clear_log();
    CHECK(gptps_settings_save(e, FRESH) == GPTPS_OK);
    CHECK(logged("without a copy of the loaded config file"));
    out = slurp(FRESH);
    CHECK(strstr(out, "# GPTPS settings, written by gptps_settings_save") == out);
    CHECK(strstr(out, "[limits]\nmax_memory_bytes = 4294967296\nmax_concurrent_tasks = 0\nmax_intake_depth = 100\n"
                      "shutdown_grace_ms = 5000\n") != NULL);     /* 0 = auto, as the file said */
    CHECK(strstr(out, "[scheduler]\nreserve_after_skips = 3\n") != NULL);
    CHECK(strstr(out, "max_dead_letters") == NULL);                        /* nobody set it */
    gptps_shutdown(e); e = NULL;
    CHECK(gptps_open(FRESH, &e) == GPTPS_OK);
    if (e) {
        CHECK(has_value(e, "limits.max_intake_depth", "100"));
        gptps_shutdown(e); e = NULL;
    }
    /* and an engine opened without a file writes only what was set live */
    remove(FRESH);
    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    if (e) {
        CHECK(gptps_settings_set(e, "scheduler.reserve_after_skips", "2") == GPTPS_OK);
        CHECK(gptps_settings_save(e, FRESH) == GPTPS_OK);
        out = slurp(FRESH);
        CHECK(strstr(out, "\n\n[scheduler]\nreserve_after_skips = 2\n") != NULL);
        CHECK(strstr(out, "[limits]") == NULL);
        gptps_shutdown(e); e = NULL;
    }

    /* a value the file set stays in the file's own spelling: 08 is not rewritten as
     * the 8 the engine reads back, since nobody changed it */
    put(CFG2, "[scheduler]\nreserve_after_skips = 08   # as written\n");
    CHECK(gptps_open(CFG2, &e) == GPTPS_OK);
    if (e) {
        CHECK(has_value(e, "scheduler.reserve_after_skips", "8"));
        CHECK(gptps_settings_save(e, NULL) == GPTPS_OK);
        CHECK(strcmp(slurp(CFG2), "[scheduler]\nreserve_after_skips = 08   # as written\n") == 0);
        CHECK(gptps_settings_set(e, "scheduler.reserve_after_skips", "3") == GPTPS_OK);
        CHECK(gptps_settings_save(e, NULL) == GPTPS_OK);
        CHECK(strcmp(slurp(CFG2), "[scheduler]\nreserve_after_skips = 3   # as written\n") == 0);
        gptps_shutdown(e); e = NULL;
    }
    /* an = in a quoted part of the key is not where the value starts */
    put(CFG2, "[tasks]\n\"a=b\".priority = 1   # an = in a quoted part\n");
    CHECK(gptps_open(CFG2, &e) == GPTPS_OK);
    if (e) {
        CHECK(reg(e, "a=b") == GPTPS_OK);
        CHECK(has_value(e, "tasks.a=b.priority", "1"));
        CHECK(gptps_settings_set(e, "tasks.a=b.priority", "5") == GPTPS_OK);
        CHECK(gptps_settings_save(e, NULL) == GPTPS_OK);
        CHECK(strcmp(slurp(CFG2), "[tasks]\n\"a=b\".priority = 5   # an = in a quoted part\n") == 0);
        gptps_shutdown(e); e = NULL;
    }

#if !defined(_WIN32)
    /* a config kept to its owner stays so: the save keeps the file's permissions */
    put(CFG2, "[scheduler]\nreserve_after_skips = 4\n");
    CHECK(chmod(CFG2, 0600) == 0);
    CHECK(gptps_open(CFG2, &e) == GPTPS_OK);
    if (e) {
        struct stat st;
        CHECK(gptps_settings_set(e, "scheduler.reserve_after_skips", "6") == GPTPS_OK);
        CHECK(gptps_settings_save(e, NULL) == GPTPS_OK);
        CHECK(stat(CFG2, &st) == 0 && (st.st_mode & 07777) == 0600);
        CHECK(strcmp(slurp(CFG2), "[scheduler]\nreserve_after_skips = 6\n") == 0);
        gptps_shutdown(e); e = NULL;
    }
#endif
    remove(CFG2);
    remove(FRESH);
}

/* The key decides, not its spelling; and a near miss of an engine table is a typo
 * only when the rest is that table's key - a host's [status] or [tags] waits. */
static void test_spelling(void)
{
    gptps *e = NULL;
    refused("\"limits.max_memroy_bytes\" = 1\n", "[limits] has no such key (did you mean limits.max_memory_bytes?)");
    refused("[limits.extra]\nx = 1\n", "limits.extra.x: [limits] has no such key");
    refused("limits = 5\n", "limits: is one of the engine's tables, not a key - its keys go under [limits]");
    refused("[limit]\nmax_concurrent_tasks = 4\n", "there is no [limit] table (did you mean [limits]?)");
    refused("[task.resize]\npriority = 1\n", "there is no [task] table (did you mean [tasks]?)");
    refused("[resource]\ngpu = 2\n", "there is no [resource] table (did you mean [resources]?)");
    refused("[taks.resize]\npriority = 1\n", "there is no [taks] table (did you mean [tasks]?)");   /* the nearest */
    put(CFG, "\"resources.gpu\" = 2\n[status]\ncode = 5\n[tags]\ncolor = \"red\"\n");
    clear_log();
    CHECK(gptps_open(CFG, &e) == GPTPS_OK);
    if (!e) { printf("%s", g_log); return; }
    CHECK(has_value(e, "resources.gpu", "2"));                    /* a quoted key defines it too */
    CHECK(gptps_config_check(e) == GPTPS_E_CONFIG);
    CHECK(logged("status.code: nothing has used this key (is [status] meant to be [stats]?)"));
    CHECK(gptps_define_global(e, "status.code", GPTPS_SETTING_UINT, "0", NULL, 0) == GPTPS_OK);
    CHECK(gptps_define_global(e, "tags.color", GPTPS_SETTING_STRING, "", NULL, 0) == GPTPS_OK);
    CHECK(has_value(e, "status.code", "5") && has_value(e, "tags.color", "red"));
    CHECK(gptps_config_check(e) == GPTPS_OK);                       /* the host's tables, defined */
    gptps_shutdown(e);
}

/* What a save writes reads back: no value the registry takes can make the file
 * fail to parse or to load. */
static void test_round_trip(void)
{
    gptps *e = NULL;
    char why[256], key[400], v[GPTPS_SETTINGS_VALUE_MAX];
    static const char odd[] = "a\rb\x01c\"d\\e\tf # g\x7f";
    size_t i;
    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    if (!e) return;
    CHECK(gptps_define_global(e, "app.ratio", GPTPS_SETTING_DOUBLE, "0.5", "0..1", 0) == GPTPS_OK);
    CHECK(gptps_define_global(e, "app.knob", GPTPS_SETTING_UINT, "1", NULL, 0) == GPTPS_OK);
    CHECK(gptps_define_global(e, "app.big", GPTPS_SETTING_DOUBLE, "1", NULL, 0) == GPTPS_OK);
    CHECK(gptps_define_global(e, "app.text", GPTPS_SETTING_STRING, "x", NULL, 0) == GPTPS_OK);
    CHECK(gptps_define_global(e, "app.bad", GPTPS_SETTING_DOUBLE, "nan", NULL, 0) == GPTPS_E_CONFIG);
    CHECK(gptps_settings_set_ex(e, "app.ratio", "nan", why, sizeof why) == GPTPS_E_CONFIG);   /* passed 0..1 */
    CHECK(gptps_define_global(e, "app.tenth", GPTPS_SETTING_DOUBLE, "0", "0..0.1", 0) == GPTPS_OK);
    CHECK(gptps_settings_set_ex(e, "app.tenth", "0.2", why, sizeof why) == GPTPS_E_CONFIG);
    CHECK(strcmp(why, "0.2 must be a number between 0 and 0.1") == 0);           /* not 0.10000000000000001 */
    CHECK(gptps_settings_set_ex(e, "app.ratio", "0x1p-1", why, sizeof why) == GPTPS_E_CONFIG);
    CHECK(gptps_settings_set_ex(e, "app.big", "inf", why, sizeof why) == GPTPS_E_CONFIG);
    CHECK(gptps_settings_set_ex(e, "app.big", "1e999", why, sizeof why) == GPTPS_E_CONFIG);
    CHECK(gptps_settings_set_ex(e, "app.knob", "5\n6", why, sizeof why) == GPTPS_E_CONFIG);
    CHECK(gptps_settings_set_ex(e, "app.knob", "\n5", why, sizeof why) == GPTPS_OK);   /* posted with a newline */
    CHECK(has_value(e, "app.knob", "5"));                                          /* stored without it */
    CHECK(gptps_settings_set_ex(e, "app.knob", " 5\t", why, sizeof why) == GPTPS_OK);  /* typed with spaces */
    CHECK(has_value(e, "app.knob", "5"));
    CHECK(gptps_settings_set(e, "app.ratio", "0.25") == GPTPS_OK);
    CHECK(gptps_settings_set(e, "app.text", odd) == GPTPS_OK);
    CHECK(gptps_define_global(e, "app.huge", GPTPS_SETTING_UINT, " 5\n", NULL, 0) == GPTPS_OK);  /* trimmed */
    CHECK(has_value(e, "app.huge", "5"));
    CHECK(gptps_settings_set(e, "app.huge", "18446744073709551615") == GPTPS_OK);  /* past a signed int */
    CHECK(gptps_settings_set(e, "app.big", "123456789012345678901234567890") == GPTPS_OK);
    CHECK(gptps_define_global(e, ".hidden.k", GPTPS_SETTING_UINT, "1", NULL, 0) == GPTPS_OK);
    CHECK(gptps_settings_set(e, ".hidden.k", "4") == GPTPS_OK);
    /* a table name past what a [table] line holds */
    for (i = 0; i < 300; ++i) key[i] = 'k';
    memcpy(key + 300, ".leaf", 6);
    CHECK(gptps_define_global(e, key, GPTPS_SETTING_UINT, "1", NULL, 0) == GPTPS_OK);
    CHECK(gptps_settings_set(e, key, "9") == GPTPS_OK);
    remove(FRESH);
    CHECK(gptps_settings_save(e, FRESH) == GPTPS_OK);
    gptps_shutdown(e); e = NULL;
    clear_log();
    CHECK(gptps_open(FRESH, &e) == GPTPS_OK);
    if (!e) { printf("%s\n%s", g_log, slurp(FRESH)); return; }
    CHECK(gptps_define_global(e, "app.ratio", GPTPS_SETTING_DOUBLE, "0.5", "0..1", 0) == GPTPS_OK);
    CHECK(gptps_define_global(e, "app.knob", GPTPS_SETTING_UINT, "1", NULL, 0) == GPTPS_OK);
    CHECK(gptps_define_global(e, "app.text", GPTPS_SETTING_STRING, "x", NULL, 0) == GPTPS_OK);
    CHECK(gptps_define_global(e, key, GPTPS_SETTING_UINT, "1", NULL, 0) == GPTPS_OK);
    CHECK(gptps_define_global(e, "app.huge", GPTPS_SETTING_UINT, "0", NULL, 0) == GPTPS_OK);
    CHECK(gptps_define_global(e, "app.big", GPTPS_SETTING_DOUBLE, "1", NULL, 0) == GPTPS_OK);
    CHECK(gptps_define_global(e, ".hidden.k", GPTPS_SETTING_UINT, "1", NULL, 0) == GPTPS_OK);
    CHECK(has_value(e, "app.ratio", "0.25") && has_value(e, "app.knob", "5") && has_value(e, key, "9"));
    CHECK(has_value(e, "app.huge", "18446744073709551615"));
    CHECK(has_value(e, "app.big", "123456789012345678901234567890.0"));    /* written as the float it is */
    CHECK(has_value(e, ".hidden.k", "4"));
    CHECK(gptps_settings_set(e, ".hidden.k", "6") == GPTPS_OK);
    CHECK(gptps_settings_save(e, NULL) == GPTPS_OK);                         /* edited in place, not added again */
    CHECK(count_in(slurp(FRESH), "hidden") == 1);
    CHECK(gptps_settings_get(e, "app.text", v, sizeof v) == GPTPS_OK && strcmp(v, odd) == 0);
    CHECK(gptps_config_check(e) == GPTPS_OK);
    gptps_shutdown(e); e = NULL;

    /* max_memory_gb keeps its unit while GiB holds the value exactly; past 2^53 bytes
     * the line is rewritten in bytes, which reads back exactly */
    put(CFG2, "[limits]\nmax_memory_gb = 1   # GiB\n");
    CHECK(gptps_open(CFG2, &e) == GPTPS_OK);
    if (e) {
        CHECK(gptps_settings_set(e, "limits.max_memory_bytes", "9007199254740993") == GPTPS_OK);
        CHECK(gptps_settings_save(e, NULL) == GPTPS_OK);
        CHECK(strcmp(slurp(CFG2), "[limits]\nmax_memory_bytes = 9007199254740993   # GiB\n") == 0);
        gptps_shutdown(e); e = NULL;
    }
    CHECK(gptps_open(CFG2, &e) == GPTPS_OK);
    if (e) {
        CHECK(has_value(e, "limits.max_memory_bytes", "9007199254740993"));
        gptps_shutdown(e); e = NULL;
    }
    remove(CFG2);
    remove(FRESH);
}

/* A plug-in's own key is checked as the plug-in defines it, inside the open; and
 * the first-submit report waits for the open to finish. */
static void test_open_with_addons(void)
{
#if defined(ADDON_NS_PATH) && defined(ADDON_SUBMIT_PATH)
    gptps *e = NULL;
    gptps_handle h;
    char text[1024];
    snprintf(text, sizeof text, "addons = [\"%s\"]\n[nstest]\nlevel = -5\n", ADDON_NS_PATH);
    refused(text, "nstest.level: -5 must be a whole number");
    /* an add-on's own resource: the file may budget it, but its costs are the add-on's */
    snprintf(text, sizeof text, "addons = [\"%s\"]\n[resources]\n\"nstest.slots\" = 2\n"
                                "[tasks.t.resources]\n\"nstest.slots\" = 1\n", ADDON_NS_PATH);
    put(CFG, text);
    CHECK(gptps_open(CFG, &e) == GPTPS_OK);
    if (e) {
        uint64_t budget = 0;
        CHECK(gptps_resource_usage(e, "nstest.slots", NULL, &budget) == GPTPS_OK && budget == 2);
        CHECK(reg(e, "t") == GPTPS_OK);
        clear_log();
        CHECK(gptps_config_check(e) == GPTPS_E_CONFIG);
        CHECK(logged("tasks.t.resources.nstest.slots: the resource nstest.slots belongs to an add-on"));
        gptps_shutdown(e); e = NULL;
    }
    snprintf(text, sizeof text, "addons = [\"%s\"]\n[scheduler]\nreserve_after_skips = 4\n[app]\nx = 1\n"
                                "[sub]\nlevel = 5\n", ADDON_SUBMIT_PATH);
    put(CFG, text);
    clear_log();
    CHECK(gptps_open(CFG, &e) == GPTPS_OK);                         /* the add-on submits as it loads */
    if (!e) { printf("%s", g_log); return; }
    CHECK(!logged("nothing has used this key"));
    CHECK(logged("sub heard sub.level = 5"));   /* its watcher, registered last, heard its own key */
    CHECK(gptps_submit(e, "sub.warmup", NULL, 0, &h) == GPTPS_OK);  /* the host's first */
    gptps_shutdown(e); e = NULL;
    CHECK(count_logged("app.x: nothing has used this key") == 1);
    CHECK(!logged("scheduler.reserve_after_skips: nothing"));

    /* a per-task key is judged by the definition its leaf names: sub.priority is
     * the add-on's choice, not the built-in number its last part spells */
    snprintf(text, sizeof text, "addons = [\"%s\"]\n[tasks.render]\n\"sub.priority\" = \"high\"\n", ADDON_SUBMIT_PATH);
    put(CFG, text);
    clear_log();
    CHECK(gptps_open(CFG, &e) == GPTPS_OK);
    if (e) {
        CHECK(reg(e, "render") == GPTPS_OK);
        CHECK(has_value(e, "tasks.render.sub.priority", "high"));
        CHECK(gptps_config_check(e) == GPTPS_OK);
        gptps_shutdown(e); e = NULL;
    } else printf("%s", g_log);
    snprintf(text, sizeof text, "addons = [\"%s\"]\n[tasks.render.sub]\npriority = 5\n", ADDON_SUBMIT_PATH);
    refused(text, "tasks.render.sub.priority: expects a \"string\" - put 5 in quotes");   /* at open */

    /* An add-on's watcher hears a type's file values on the thread registering it,
     * and may act on the type by name there: it is there for that thread. But not
     * removed - the registration still writes into it, and freeing it under that
     * call was a use-after-free - so the removal is refused with GPTPS_E_BUSY. */
    snprintf(text, sizeof text, "addons = [\"%s\"]\n[tasks.victim]\n\"sub.remove\" = true\n", ADDON_SUBMIT_PATH);
    put(CFG, text);
    clear_log();
    CHECK(gptps_open(CFG, &e) == GPTPS_OK);
    if (e) {
        char want[64];
        CHECK(reg(e, "victim") == GPTPS_OK);
        snprintf(want, sizeof want, "sub removing victim: %d, exists 1", (int)GPTPS_E_BUSY);
        CHECK(logged(want));                                      /* was 0 (OK), and freed */
        CHECK(gptps_task_exists(e, "victim"));
        CHECK(has_value(e, "tasks.victim.sub.remove", "true"));
        CHECK(gptps_unregister_task(e, "victim", GPTPS_REMOVE_CANCEL) == GPTPS_OK);   /* now it may go */
        gptps_shutdown(e); e = NULL;
    } else printf("%s", g_log);
#endif
}

/* A reload replaces the old file's late errors; and a host's watchers hear only
 * gptps_settings_set, never a file's values (an add-on's hear both: test_plugin_tier). */
static int g_watched;
static void on_change(const char *key, const char *value, void *ud) { (void)key; (void)value; (void)ud; ++g_watched; }
static void test_reload_bookkeeping(void)
{
    gptps *e = NULL;
    put(CFG, "[app]\nknob = 70\n[scheduler]\nreserve_after_skips = 4\n");
    CHECK(gptps_open(CFG, &e) == GPTPS_OK);
    if (!e) return;
    CHECK(gptps_settings_watch(e, on_change, NULL) == GPTPS_OK);
    CHECK(gptps_define_global(e, "app.knob", GPTPS_SETTING_UINT, "1", "0..10", 0) == GPTPS_OK);
    CHECK(gptps_config_check(e) == GPTPS_E_CONFIG);                 /* 70 is out of range */
    put(CFG, "[app]\nknob = 7\n[scheduler]\nreserve_after_skips = 6\n");
    g_watched = 0;
    CHECK(gptps_settings_reload(e, NULL) == GPTPS_OK);
    CHECK(has_value(e, "app.knob", "7") && has_value(e, "scheduler.reserve_after_skips", "6"));
    CHECK(g_watched == 0);              /* a host's watcher hears live sets, as in 1.5 */
    CHECK(gptps_config_check(e) == GPTPS_OK);                       /* the fixed file is clean */
    CHECK(gptps_settings_set(e, "scheduler.reserve_after_skips", "5") == GPTPS_OK);
    CHECK(g_watched == 1);
    /* a live change survives a reload that refuses the file's value for it: save
     * still writes it */
    put(CFG, "[app]\nknob = 8\n[limits]\nshutdown_grace_ms = 30000\n");
    CHECK(gptps_settings_reload(e, NULL) == GPTPS_OK);
    CHECK(gptps_settings_set(e, "limits.shutdown_grace_ms", "900") == GPTPS_OK);
    put(CFG, "[app]\nknob = 8\n[limits]\nshutdown_grace_ms = 99999999999\n");
    CHECK(gptps_settings_reload(e, NULL) == GPTPS_E_CONFIG);
    CHECK(has_value(e, "limits.shutdown_grace_ms", "900"));
    CHECK(gptps_settings_save(e, NULL) == GPTPS_OK);
    CHECK(strstr(slurp(CFG), "shutdown_grace_ms = 900") != NULL);
    /* a reload's own refusal stays reported while its file stands */
    put(CFG, "[app]\nknob = 70\n");
    CHECK(gptps_settings_reload(e, NULL) == GPTPS_E_CONFIG);
    CHECK(gptps_config_check(e) == GPTPS_E_CONFIG);
    put(CFG, "[app]\nknob = 8\n");
    CHECK(gptps_settings_reload(e, NULL) == GPTPS_OK);
    CHECK(gptps_config_check(e) == GPTPS_OK);
    gptps_shutdown(e);
}

/* gptps_define_resource against an unregister of the same task, on two threads: no
 * cost setting may outlive the task it belongs to (it would point at freed memory). */
static gptps *g_race;
static int g_stop;                   /* an atomic: both threads read it */
static void *race_unreg(void *a)
{
    int i;
    (void)a;
    for (i = 0; i < 3000 && !__atomic_load_n(&g_stop, __ATOMIC_SEQ_CST); ++i) {
        reg(g_race, "T");
        gptps_unregister_task(g_race, "T", 0);
    }
    __atomic_store_n(&g_stop, 1, __ATOMIC_SEQ_CST);
    return NULL;
}
static void *race_define(void *a)
{
    int i;
    char n[32];
    (void)a;
    for (i = 0; i < 300 && !__atomic_load_n(&g_stop, __ATOMIC_SEQ_CST); ++i) {
        snprintf(n, sizeof n, "r%d", i);
        gptps_define_resource(g_race, n, 1);
    }
    __atomic_store_n(&g_stop, 1, __ATOMIC_SEQ_CST);
    return NULL;
}
/* A race, so a check that can only catch it sometimes: each round is a fresh engine
 * with both threads at full speed, and the window is microseconds wide. Under
 * AddressSanitizer a stale setting is also a use-after-free when it is read. */
static void test_define_race(void)
{
    int round, stale = 0;
    for (round = 0; round < 8; ++round) {
        gptps_thread *a, *b;
        size_t i, n;
        CHECK(gptps_open(NULL, &g_race) == GPTPS_OK);
        if (!g_race) return;
        __atomic_store_n(&g_stop, 0, __ATOMIC_SEQ_CST);
        a = gptps_thread_start(race_unreg, NULL);
        b = gptps_thread_start(race_define, NULL);
        CHECK(a && b);
        if (a) gptps_thread_join(a);
        if (b) gptps_thread_join(b);
        n = gptps_settings_count(g_race);
        for (i = 0; i < n; ++i) {
            gptps_setting_info in;
            memset(&in, 0, sizeof in); in.struct_size = sizeof in;
            if (gptps_settings_get_info(g_race, i, &in) == GPTPS_OK && !strncmp(in.key, "tasks.T.", 8)) ++stale;
        }
        gptps_shutdown(g_race);
    }
    CHECK(stale == 0);
}

/* Two threads saving one file at once, and a third reading it all the while. A save
 * writes "<file>.tmp" and renames it over the file, and every save to that path uses
 * that one name: a second save let in after the first had written it, and before the
 * first renamed it, truncated it and wrote into it. The first then renamed the
 * second's half-written file into place, and the second's own rename found nothing
 * to move - GPTPS_E_IO. The file is some 33 KB, so stdio writes it in several write()
 * calls and a cut-off copy can be caught, and the reader holds every read to the
 * whole file: the lines it started with, a number, and its last line. (gptps_open
 * would not do: it accepts an empty file and one cut at a line's end.) The reader is
 * POSIX only: Windows will not replace a file another handle has open, so there a
 * reader can make a save fail - a rule of the platform, which the engine cannot
 * change. */
#define SV_LINES 400
static gptps *g_sv;
static int g_sv_stop, g_sv_failed, g_sv_reads, g_sv_cut, g_sv_empty;
static char g_sv_head[SV_LINES * 96 + 128];          /* the file up to the number */
static const char g_sv_tail[] = "\n# end\n";          /* and after it */
static void *sv_saver(void *a)
{
    int i;
    char v[24];
    for (i = 0; i < 300; ++i) {
        snprintf(v, sizeof v, "%d", (int)(size_t)a * 1000 + i);    /* a change for every save to write */
        if (gptps_settings_set(g_sv, "app.n", v) != GPTPS_OK || gptps_settings_save(g_sv, NULL) != GPTPS_OK)
            __atomic_add_fetch(&g_sv_failed, 1, __ATOMIC_SEQ_CST);
    }
    return NULL;
}
static void *sv_reader(void *a)
{
    (void)a;
#if !defined(_WIN32)
    {
        static char buf[sizeof g_sv_head + 64];
        size_t hl = strlen(g_sv_head), tl = sizeof g_sv_tail - 1, n, i;
        do {
            FILE *f = fopen(CFG2, "rb");
            int whole = 0;
            n = 0;
            if (f) { n = fread(buf, 1, sizeof buf, f); fclose(f); }
            if (n > hl + tl && memcmp(buf, g_sv_head, hl) == 0 && memcmp(buf + n - tl, g_sv_tail, tl) == 0) {
                for (whole = 1, i = hl; i < n - tl; ++i) if (buf[i] < '0' || buf[i] > '9') whole = 0;
            }
            if (!whole) __atomic_add_fetch(n ? &g_sv_cut : &g_sv_empty, 1, __ATOMIC_SEQ_CST);
            __atomic_add_fetch(&g_sv_reads, 1, __ATOMIC_SEQ_CST);
            /* a HAL call each turn: on a HAL that runs one thread at a time
             * (tests/hal_sim.c), a loop with none keeps the CPU from the savers */
            (void)gptps_now_ms(NULL);
        } while (!__atomic_load_n(&g_sv_stop, __ATOMIC_SEQ_CST));
    }
#else
    __atomic_add_fetch(&g_sv_reads, 1, __ATOMIC_SEQ_CST);
#endif
    return NULL;
}
static void test_concurrent_saves(void)
{
    gptps_thread *a, *b, *rd;
    size_t hl;
    uint64_t t0;
    int i;
    hl = (size_t)snprintf(g_sv_head, sizeof g_sv_head, "# saved by two threads at once\n");
    for (i = 0; i < SV_LINES; ++i)
        hl += (size_t)snprintf(g_sv_head + hl, sizeof g_sv_head - hl,
                               "# line %03d, one of many: a cut-off copy of this file is caught by its last line\n", i);
    snprintf(g_sv_head + hl, sizeof g_sv_head - hl, "[app]\nn = ");
    {
        char *whole = (char *)malloc(strlen(g_sv_head) + sizeof g_sv_tail + 1);
        CHECK(whole != NULL);
        if (!whole) return;
        sprintf(whole, "%s0%s", g_sv_head, g_sv_tail);
        put(CFG2, whole);
        free(whole);
    }
    CHECK(gptps_open(CFG2, &g_sv) == GPTPS_OK);
    if (!g_sv) return;
    CHECK(gptps_define_global(g_sv, "app.n", GPTPS_SETTING_UINT, "0", NULL, 0) == GPTPS_OK);
    __atomic_store_n(&g_sv_stop, 0, __ATOMIC_SEQ_CST);
    rd = gptps_thread_start(sv_reader, NULL);
    CHECK(rd != NULL);
    for (t0 = gptps_now_ms(NULL); rd && !__atomic_load_n(&g_sv_reads, __ATOMIC_SEQ_CST) && gptps_now_ms(NULL) - t0 < 5000; ) { }
    CHECK(__atomic_load_n(&g_sv_cut, __ATOMIC_SEQ_CST) == 0);    /* the file as written: the reader agrees */
    a = gptps_thread_start(sv_saver, (void *)1);
    b = gptps_thread_start(sv_saver, (void *)2);
    CHECK(a && b);
    if (a) gptps_thread_join(a);
    if (b) gptps_thread_join(b);
    __atomic_store_n(&g_sv_stop, 1, __ATOMIC_SEQ_CST);
    if (rd) gptps_thread_join(rd);
    if (g_sv_failed || g_sv_cut || g_sv_empty)
        printf("  %d of 600 saves failed; of %d reads, %d found the file cut off and %d found it empty or gone\n",
               g_sv_failed, g_sv_reads, g_sv_cut, g_sv_empty);
    CHECK(g_sv_failed == 0);         /* was GPTPS_E_IO */
    CHECK(g_sv_cut == 0);            /* was a part of the file */
    CHECK(g_sv_empty == 0);          /* was none of it */
    {   /* and it holds the value set last: each save writes the live value */
        static char last[sizeof g_sv_head + 64];
        size_t n = 0;
        FILE *f = fopen(CFG2, "rb");
        if (f) { n = fread(last, 1, sizeof last - 1, f); fclose(f); }
        last[n] = 0;
        CHECK(n == strlen(g_sv_head) + 4 + sizeof g_sv_tail - 1 && strncmp(last, g_sv_head, strlen(g_sv_head)) == 0);
        CHECK(strstr(last, "\nn = 1299\n# end\n") || strstr(last, "\nn = 2299\n# end\n"));
    }
    gptps_shutdown(g_sv);
    g_sv = NULL;
    remove(CFG2);
    remove(CFG2 ".tmp");
}

/* A save that fails logs why - once it has released the settings lock. A log sink
 * is host code: one that hands the message to a thread of its own, which reads a
 * setting, found that lock still held, so the reader waited on the sink - and a sink
 * that waited for the reader never returned. */
static gptps *g_ls_e;
static int g_ls_go, g_ls_read, g_ls_in_time;
static void *ls_reader(void *a)
{
    char v[64];
    (void)a;
    while (!__atomic_load_n(&g_ls_go, __ATOMIC_SEQ_CST)) (void)gptps_now_ms(NULL);
    if (__atomic_load_n(&g_ls_go, __ATOMIC_SEQ_CST) == 1) (void)gptps_settings_get(g_ls_e, "app.n", v, sizeof v);
    __atomic_store_n(&g_ls_read, 1, __ATOMIC_SEQ_CST);
    return NULL;
}
static void ls_sink(gptps_log_level lvl, const char *msg, void *ud)
{
    uint64_t t0;
    sink(lvl, msg, ud);
    if (!strstr(msg, "settings not saved")) return;
    __atomic_store_n(&g_ls_go, 1, __ATOMIC_SEQ_CST);
    for (t0 = gptps_now_ms(NULL); !__atomic_load_n(&g_ls_read, __ATOMIC_SEQ_CST) && gptps_now_ms(NULL) - t0 < 2000; ) { }
    __atomic_store_n(&g_ls_in_time, __atomic_load_n(&g_ls_read, __ATOMIC_SEQ_CST), __ATOMIC_SEQ_CST);
}
static void test_save_logs_unlocked(void)
{
    gptps_thread *rd;
    CHECK(gptps_open(NULL, &g_ls_e) == GPTPS_OK);
    if (!g_ls_e) return;
    CHECK(gptps_define_global(g_ls_e, "app.n", GPTPS_SETTING_UINT, "0", NULL, 0) == GPTPS_OK);
    CHECK(gptps_settings_set(g_ls_e, "app.n", "2") == GPTPS_OK);
    put(CFG2, "[app\nn = 1\n");                                   /* does not parse */
    rd = gptps_thread_start(ls_reader, NULL);
    CHECK(rd != NULL);
    clear_log();
    gptps_set_log_sink(ls_sink, NULL);
    CHECK(gptps_settings_save(g_ls_e, CFG2) == GPTPS_E_CONFIG);
    gptps_set_log_sink(sink, NULL);
    CHECK(logged("settings not saved: " CFG2 ":1:"));
    CHECK(__atomic_load_n(&g_ls_in_time, __ATOMIC_SEQ_CST) == 1);   /* the reader was not held up */
    if (!__atomic_load_n(&g_ls_go, __ATOMIC_SEQ_CST)) __atomic_store_n(&g_ls_go, 2, __ATOMIC_SEQ_CST);
    if (rd) gptps_thread_join(rd);
    gptps_shutdown(g_ls_e);
    g_ls_e = NULL;
    remove(CFG2);
}

/* Under a locale whose decimal point is a comma - what a GUI toolkit's init sets
 * from the environment - a file still says 1.5, and a save still writes it. */
static void test_locale(void)
{
    static const char *const names[] = { "de_DE.UTF-8", "de_DE.utf8", "en_DK.UTF-8", "en_DK.utf8",
                                         "fr_FR.UTF-8", "fr_FR.utf8", "nl_NL.UTF-8", "German", 0 };
    const char *got = NULL;
    gptps *e = NULL;
    size_t i;
    for (i = 0; names[i] && !got; ++i) got = setlocale(LC_NUMERIC, names[i]);
    if (!got || localeconv()->decimal_point[0] == '.') {
        setlocale(LC_NUMERIC, "C");
        printf("  (no comma-decimal locale on this machine: the locale check is skipped)\n");
        return;
    }
    put(CFG2, "[limits]\nmax_memory_gb = 4.5\n");
    CHECK(gptps_open(CFG2, &e) == GPTPS_OK);
    if (e) {
        CHECK(has_value(e, "limits.max_memory_bytes", "4831838208"));            /* 4.5 GiB */
        CHECK(gptps_settings_set(e, "limits.max_memory_bytes", "1610612736") == GPTPS_OK);
        CHECK(gptps_settings_save(e, NULL) == GPTPS_OK);
        CHECK(strcmp(slurp(CFG2), "[limits]\nmax_memory_gb = 1.5\n") == 0);       /* not 1,5 */
        CHECK(gptps_define_global(e, "app.r", GPTPS_SETTING_DOUBLE, "0.25", "0..0.5", 0) == GPTPS_OK);
        CHECK(gptps_settings_set(e, "app.r", "0.5") == GPTPS_OK);
        gptps_shutdown(e);
    }
    setlocale(LC_NUMERIC, "C");
    remove(CFG2);
}

/* The first submit's report of unused keys must not deadlock when that submit comes
 * from a setting's write accessor, which runs holding the registry's lock. */
static gptps *g_wr_engine;
static char g_wr_val[32] = "0";
static size_t wr_read(void *t, char *b, size_t c) { (void)t; return (size_t)snprintf(b, c, "%s", g_wr_val); }
static gptps_status wr_write(void *t, const char *v)
{
    gptps_handle h;
    (void)t;
    snprintf(g_wr_val, sizeof g_wr_val, "%s", v);
    return gptps_submit(g_wr_engine, "t", NULL, 0, &h);     /* a write that starts work */
}
static void test_submit_from_write(void)
{
    gptps_setting_def d;
    gptps *e = NULL;
    put(CFG, "[app]\nunused = 1\n");
    CHECK(gptps_open(CFG, &e) == GPTPS_OK);
    if (!e) return;
    g_wr_engine = e;
    CHECK(reg(e, "t") == GPTPS_OK);
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.key = "app.go"; d.type = GPTPS_SETTING_UINT; d.desc = "starts a task";
    d.hot = 1; d.read = wr_read; d.write = wr_write;
    CHECK(gptps_register_setting(e, &d) == GPTPS_OK);
    clear_log();
    CHECK(gptps_settings_set(e, "app.go", "1") == GPTPS_OK);    /* the first submit, inside the lock */
    gptps_shutdown(e);
    CHECK(logged("app.unused: nothing has used this key - seen at the first submit"));
}

/* What a file's value runs - a host's write accessor here - is inside the engine's
 * callback mark: a gptps_shutdown from it is refused, not let free the engine the
 * caller is still in. */
static gptps *g_cb_engine;
static gptps_status g_cb_shutdown = GPTPS_OK;
static char g_cb_val[32] = "0";
static size_t cb_read(void *t, char *b, size_t c) { (void)t; return (size_t)snprintf(b, c, "%s", g_cb_val); }
static gptps_status cb_write_shutdown(void *t, const char *v)
{
    (void)t;
    snprintf(g_cb_val, sizeof g_cb_val, "%s", v);
    g_cb_shutdown = gptps_shutdown(g_cb_engine);
    return GPTPS_OK;
}
static int g_stall_started;
static gptps_status stall_task(gptps_ctx *c, void *u) { (void)c; (void)u; __atomic_store_n(&g_stall_started, 1, __ATOMIC_SEQ_CST); return GPTPS_OK; }
static gptps_status cb_write_stall(void *t, const char *v)    /* submits, then waits for the task */
{
    gptps_handle h;
    uint64_t t0;
    (void)t; (void)v;
    if (gptps_submit(g_cb_engine, "stall", NULL, 0, &h) != GPTPS_OK) return GPTPS_E_INVAL;
    t0 = gptps_now_ms(NULL);
    while (!__atomic_load_n(&g_stall_started, __ATOMIC_SEQ_CST) && gptps_now_ms(NULL) - t0 < 3000) { }
    return GPTPS_OK;
}
static int g_stepping;
static void *step_loop(void *a)
{
    gptps *e = (gptps *)a;
    size_t ran;
    while (__atomic_load_n(&g_stepping, __ATOMIC_SEQ_CST)) gptps_step(e, &ran);
    return NULL;
}

static void test_callback_mark(void)
{
    gptps_setting_def d;
    gptps_task_def td;
    gptps *e = NULL;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.type = GPTPS_SETTING_UINT; d.desc = "test"; d.hot = 1; d.read = cb_read;

    put(CFG, "[app]\nboom = 1\n");
    CHECK(gptps_open(CFG, &e) == GPTPS_OK);
    if (!e) return;
    g_cb_engine = e;
    d.key = "app.boom"; d.write = cb_write_shutdown;
    CHECK(gptps_register_setting(e, &d) == GPTPS_OK);         /* takes the file's 1, runs the write */
    CHECK(g_cb_shutdown == GPTPS_E_BUSY);
    CHECK(has_value(e, "app.boom", "1"));
    gptps_shutdown(e); e = NULL;

    /* the first submit's report does not hold up the dispatcher on the registry's
     * lock - which this write accessor holds while it waits for its task */
    put(CFG, "[app]\nunused = 1\n");
    CHECK(gptps_open(CFG, &e) == GPTPS_OK);
    if (!e) return;
    g_cb_engine = e;
    memset(&td, 0, sizeof td);
    td.struct_size = sizeof td; td.name = "stall"; td.run = stall_task; td.exec = GPTPS_EXEC_INPROC;
    CHECK(gptps_register_task(e, &td) == GPTPS_OK);
    d.key = "app.stall"; d.write = cb_write_stall;
    CHECK(gptps_register_setting(e, &d) == GPTPS_OK);
    __atomic_store_n(&g_stall_started, 0, __ATOMIC_SEQ_CST);
    CHECK(gptps_settings_set(e, "app.stall", "1") == GPTPS_OK);
    CHECK(__atomic_load_n(&g_stall_started, __ATOMIC_SEQ_CST) == 1);
    gptps_shutdown(e); e = NULL;

    /* ...nor the stepper of a MANUAL engine, which plays the dispatcher */
    {
        gptps_config cfg;
        gptps_thread *stepper;
        memset(&cfg, 0, sizeof cfg);
        cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
        cfg.mode = GPTPS_RUN_MANUAL; cfg.config_path = CFG;
        CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
        if (!e) return;
        g_cb_engine = e;
        CHECK(gptps_register_task(e, &td) == GPTPS_OK);
        CHECK(gptps_register_setting(e, &d) == GPTPS_OK);
        __atomic_store_n(&g_stall_started, 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&g_stepping, 1, __ATOMIC_SEQ_CST);
        stepper = gptps_thread_start(step_loop, e);
        CHECK(stepper != NULL);
        CHECK(gptps_settings_set(e, "app.stall", "1") == GPTPS_OK);
        CHECK(__atomic_load_n(&g_stall_started, __ATOMIC_SEQ_CST) == 1);
        __atomic_store_n(&g_stepping, 0, __ATOMIC_SEQ_CST);
        if (stepper) gptps_thread_join(stepper);
        gptps_shutdown(e);
    }
}

/* An add-on whose setup failed hears nothing more; a plug-in the host loads after
 * the open still gets its per-task keys from the file. */
static void *load_waiter(void *a)
{
#if defined(ADDON_WAITER_PATH)
    *(gptps_status *)a = gptps_load_addon(g_cb_engine, ADDON_WAITER_PATH);
#else
    (void)a;
#endif
    return NULL;
}

static void test_addon_lifecycle(void)
{
#if defined(ADDON_FAILWATCH_PATH) && defined(ADDON_FAILWATCH_PLAIN_PATH) && defined(ADDON_SUBMIT_PATH) && defined(ADDON_WAITER_PATH)
    gptps *e = NULL;
    int variant;
    for (variant = 0; variant < 2; ++variant) {             /* namespaced, and not */
    put(CFG, "[fw]\nlevel = 4\nown = 7\n");
    CHECK(gptps_open(CFG, &e) == GPTPS_OK);
    if (!e) return;
    clear_log();
    CHECK(gptps_load_addon(e, variant ? ADDON_FAILWATCH_PLAIN_PATH : ADDON_FAILWATCH_PATH) != GPTPS_OK);
    {   /* its settings went with it: the target of fw.own was freed by the setup */
        char v[64];
        CHECK(gptps_settings_get(e, "fw.own", v, sizeof v) == GPTPS_E_NOTFOUND);
        CHECK(gptps_settings_get(e, "fw.level", v, sizeof v) == GPTPS_E_NOTFOUND);
        CHECK(gptps_settings_set(e, "fw.own", "8") == GPTPS_E_NOTFOUND);
    }
    CHECK(gptps_settings_set(e, "scheduler.reserve_after_skips", "3") == GPTPS_OK);   /* any watcher hears */
    put(CFG, "[fw]\nlevel = 6\nown = 9\n[scheduler]\nreserve_after_skips = 5\n");
    CHECK(gptps_settings_reload(e, NULL) == GPTPS_OK);         /* an add-on's watcher would hear this */
    CHECK(!logged("failwatch heard"));                          /* and its watcher heard nothing */
    CHECK(gptps_config_check(e) == GPTPS_E_CONFIG);             /* its keys in the file went unused */
    CHECK(logged("fw.own: nothing has used this key"));
    gptps_shutdown(e); e = NULL;
    }

    /* While an add-on's setup runs on another thread, a task the host registers takes
     * its file values at once (its cost of gpu here), and a reload is refused. */
    put(CFG, "[resources]\ngpu = 1\n[tasks.host.resources]\ngpu = 1\n");
    CHECK(gptps_open(CFG, &e) == GPTPS_OK);
    if (e) {
        gptps_status loaded = GPTPS_E_INVAL;
        gptps_thread *loader;
        uint64_t t0;
        g_cb_engine = e;
        loader = gptps_thread_start(load_waiter, &loaded);
        CHECK(loader != NULL);
        for (t0 = gptps_now_ms(NULL); !gptps_task_exists(e, "waiter.started") && gptps_now_ms(NULL) - t0 < 5000; ) { }
        CHECK(gptps_task_exists(e, "waiter.started"));          /* its setup is running */
        CHECK(reg(e, "host") == GPTPS_OK);
        CHECK(has_value(e, "tasks.host.resources.gpu", "1"));  /* now, not when the setup ends */
        CHECK(gptps_settings_reload(e, NULL) == GPTPS_E_BUSY);
        CHECK(reg(e, "host.done") == GPTPS_OK);                 /* lets the setup finish */
        if (loader) gptps_thread_join(loader);
        CHECK(loaded == GPTPS_OK);
        CHECK(gptps_settings_reload(e, NULL) == GPTPS_OK);
        gptps_shutdown(e); e = NULL;
    }

    put(CFG, "[tasks.render]\n\"sub.priority\" = \"high\"\n");
    clear_log();
    CHECK(gptps_open(CFG, &e) == GPTPS_OK);                     /* the plug-in is not loaded yet */
    if (!e) { printf("%s", g_log); return; }
    CHECK(gptps_load_addon(e, ADDON_SUBMIT_PATH) == GPTPS_OK);
    CHECK(reg(e, "render") == GPTPS_OK);
    CHECK(has_value(e, "tasks.render.sub.priority", "high"));
    CHECK(gptps_config_check(e) == GPTPS_OK);
    gptps_shutdown(e);
#endif
}

/* A failed setup is undone - the task types, observers and constraints it registered
 * removed, the scheduler it set put back - and nothing else. The unwind took
 * everything ahead of each list's head, as the head was when the setup began, to be
 * the setup's own: a type, observer or constraint a host thread registered while the
 * setup ran went with it. Once that head was removed meanwhile, the unwind never met
 * its stopping point and removed every type, observer and constraint in the engine.
 * And it put back the scheduler it had found, over one the host set meanwhile. */
static int g_fs_finished, g_fs_other;
static void fs_obs_a(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_FINISHED) __atomic_add_fetch(&g_fs_finished, 1, __ATOMIC_SEQ_CST);
    else if (ev->kind == GPTPS_EV_FAILED || ev->kind == GPTPS_EV_DEAD_LETTERED || ev->kind == GPTPS_EV_DROPPED)
        __atomic_add_fetch(&g_fs_other, 1, __ATOMIC_SEQ_CST);
}
static void fs_obs_b(const gptps_event *ev, void *ud) { (void)ev; (void)ud; }
static void fs_obs_c(const gptps_event *ev, void *ud) { (void)ev; (void)ud; }
static gptps_admit_decision fs_admit(const gptps_constraint_input *in, uint32_t *r)
{ (void)in; (void)r; return GPTPS_ADMIT; }
static gptps_admit_decision fs_con_a(const gptps_constraint_input *in, uint32_t *r, void *ud) { (void)ud; return fs_admit(in, r); }
static gptps_admit_decision fs_con_b(const gptps_constraint_input *in, uint32_t *r, void *ud) { (void)ud; return fs_admit(in, r); }
static gptps_admit_decision fs_con_c(const gptps_constraint_input *in, uint32_t *r, void *ud) { (void)ud; return fs_admit(in, r); }
static int64_t fs_score(const gptps_sched_input *in, void *ud) { (void)ud; return (int64_t)in->priority; }
static int fs_owner_is(gptps *e, const char *want)
{
    const char *o = gptps_scheduler_owner(e);
    return want ? (o && strcmp(o, want) == 0) : o == NULL;
}
/* MANUAL, so that every event fires on this thread, in gptps_step: unregistering an
 * observer is setup-time, and must not race an engine thread walking the list. */
static gptps_status fs_open(gptps **e)
{
    gptps_config c;
    memset(&c, 0, sizeof c);
    c.struct_size = sizeof c; c.limits.struct_size = sizeof c.limits; c.mode = GPTPS_RUN_MANUAL;
    return gptps_open_ex(&c, e);
}
/* Neither the add-on's constraint, which denies probe.denied, nor its observer, which
 * logs, is there: an item of probe.denied runs, and nobody else hears it. */
static int fs_addon_gone(gptps *e)
{
    uint64_t t0;
    size_t ran;
    __atomic_store_n(&g_fs_finished, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_fs_other, 0, __ATOMIC_SEQ_CST);
    clear_log();
    if (gptps_submit(e, "probe.denied", NULL, 0, NULL) != GPTPS_OK) return 0;
    for (t0 = gptps_now_ms(NULL); !__atomic_load_n(&g_fs_finished, __ATOMIC_SEQ_CST) &&
         !__atomic_load_n(&g_fs_other, __ATOMIC_SEQ_CST) && gptps_now_ms(NULL) - t0 < 5000; )
        if (gptps_step(e, &ran) != GPTPS_OK) return 0;
    return __atomic_load_n(&g_fs_finished, __ATOMIC_SEQ_CST) == 1 && __atomic_load_n(&g_fs_other, __ATOMIC_SEQ_CST) == 0 &&
           !logged("waiter's observer heard");
}
static int fs_setup_running(gptps *e)
{
    uint64_t t0;
    for (t0 = gptps_now_ms(NULL); !gptps_task_exists(e, "waiter.started") && gptps_now_ms(NULL) - t0 < 5000; ) { }
    return gptps_task_exists(e, "waiter.started");
}
static void test_failed_setup_undoes_only_its_own(void)
{
#if defined(ADDON_WAITER_PATH)
    gptps *e = NULL;
    gptps_status loaded = GPTPS_OK;
    gptps_thread *loader;

    /* The host registers, unregisters and takes the seam while the setup runs. */
    CHECK(fs_open(&e) == GPTPS_OK);
    if (!e) return;
    g_cb_engine = e;
    CHECK(reg(e, "probe.denied") == GPTPS_OK);
    CHECK(reg(e, "keep") == GPTPS_OK);
    CHECK(reg(e, "head") == GPTPS_OK);                       /* the head as the setup begins */
    CHECK(gptps_register_observer(e, fs_obs_a, NULL) == GPTPS_OK);
    CHECK(gptps_register_observer(e, fs_obs_b, NULL) == GPTPS_OK);                /* the head */
    CHECK(gptps_register_constraint(e, fs_con_a, NULL) == GPTPS_OK);
    CHECK(gptps_register_constraint(e, fs_con_b, NULL) == GPTPS_OK);              /* the head */
    CHECK(gptps_set_scheduler_ex(e, fs_score, NULL, "host", 0) == GPTPS_OK);     /* the setup finds it taken */
    loader = gptps_thread_start(load_waiter, &loaded);
    CHECK(loader != NULL);
    CHECK(fs_setup_running(e));
    CHECK(fs_owner_is(e, "host"));                           /* it asked for the seam, and was refused */
    CHECK(reg(e, "mine") == GPTPS_OK);                       /* the host's, meanwhile */
    CHECK(gptps_unregister_task(e, "head", GPTPS_REMOVE_REJECT_IF_BUSY) == GPTPS_OK);
    CHECK(gptps_register_observer(e, fs_obs_c, NULL) == GPTPS_OK);
    CHECK(gptps_unregister_observer(e, fs_obs_b, NULL) == GPTPS_OK);
    CHECK(gptps_register_constraint(e, fs_con_c, NULL) == GPTPS_OK);
    CHECK(gptps_unregister_constraint(e, fs_con_b, NULL) == GPTPS_OK);
    CHECK(gptps_set_scheduler_ex(e, fs_score, NULL, "host2", GPTPS_SCHED_REPLACE) == GPTPS_OK);
    CHECK(reg(e, "host.fail") == GPTPS_OK);                  /* its cue to give up */
    if (loader) gptps_thread_join(loader);
    CHECK(loaded == GPTPS_E_TASK);
    CHECK(!gptps_task_exists(e, "waiter.started"));          /* its own type is undone */
    CHECK(gptps_task_exists(e, "mine"));                     /* was removed with it */
    CHECK(gptps_task_exists(e, "host.fail"));                /* so was this */
    CHECK(gptps_task_exists(e, "keep"));                     /* and this, older than the load */
    CHECK(fs_owner_is(e, "host2"));                          /* was "host" again */
    CHECK(fs_addon_gone(e));
    CHECK(gptps_unregister_observer(e, fs_obs_c, NULL) == GPTPS_OK);      /* was removed with its own */
    CHECK(gptps_unregister_observer(e, fs_obs_a, NULL) == GPTPS_OK);      /* so was this, older than the load */
    CHECK(gptps_unregister_constraint(e, fs_con_c, NULL) == GPTPS_OK);    /* and these */
    CHECK(gptps_unregister_constraint(e, fs_con_a, NULL) == GPTPS_OK);
    gptps_shutdown(e);

    /* The host does nothing meanwhile: the seam the setup took is put back as it was. */
    e = NULL;
    loaded = GPTPS_OK;
    CHECK(fs_open(&e) == GPTPS_OK);
    if (!e) return;
    g_cb_engine = e;
    CHECK(reg(e, "probe.denied") == GPTPS_OK);
    CHECK(gptps_register_observer(e, fs_obs_a, NULL) == GPTPS_OK);
    loader = gptps_thread_start(load_waiter, &loaded);
    CHECK(loader != NULL);
    CHECK(fs_setup_running(e));
    CHECK(fs_owner_is(e, "waiter"));                         /* it took the seam */
    CHECK(reg(e, "host.fail") == GPTPS_OK);
    if (loader) gptps_thread_join(loader);
    CHECK(loaded == GPTPS_E_TASK);
    CHECK(fs_owner_is(e, NULL));
    CHECK(fs_addon_gone(e));
    CHECK(gptps_unregister_observer(e, fs_obs_a, NULL) == GPTPS_OK);
    gptps_shutdown(e);
#endif
}

static gptps *g_wl_engine;
static int g_wl_stop;
static void noop_watch(const char *k, const char *v, void *u) { (void)k; (void)v; (void)u; }
static void *watch_adder(void *a)
{
    int i;
    (void)a;
    for (i = 0; i < 300; ++i) gptps_settings_watch(g_wl_engine, noop_watch, NULL);
    __atomic_store_n(&g_wl_stop, 1, __ATOMIC_SEQ_CST);
    return NULL;
}
static void test_watch_while_setting(void)
{
    gptps_thread *t;
    int i;
    CHECK(gptps_open(NULL, &g_wl_engine) == GPTPS_OK);
    if (!g_wl_engine) return;
    __atomic_store_n(&g_wl_stop, 0, __ATOMIC_SEQ_CST);
    t = gptps_thread_start(watch_adder, NULL);
    for (i = 0; i < 5000 && !__atomic_load_n(&g_wl_stop, __ATOMIC_SEQ_CST); ++i)
        CHECK(gptps_settings_set(g_wl_engine, "scheduler.reserve_after_skips", (i & 1) ? "3" : "4") == GPTPS_OK);
    if (t) gptps_thread_join(t);
    gptps_shutdown(g_wl_engine);
}

/* Where save puts a value, held to the byte: each case is one a mutation run
 * (tools/mutate.py) showed no test pinned. */
static void save_case(const char *file, const char *key, const char *value, const char *want)
{
    gptps *e = NULL;
    put(CFG2, file);
    CHECK(gptps_open(CFG2, &e) == GPTPS_OK);
    if (!e) return;
    if (!strcmp(key, "verbose")) CHECK(gptps_define_global(e, "verbose", GPTPS_SETTING_BOOL, "false", NULL, 0) == GPTPS_OK);
    if (!strncmp(key, "tasks.render.", 13)) {
        CHECK(reg(e, "render") == GPTPS_OK);
        CHECK(gptps_define_resource(e, "cpu", 8) == GPTPS_OK);
    }
    CHECK(gptps_settings_set(e, key, value) == GPTPS_OK);
    CHECK(gptps_settings_save(e, NULL) == GPTPS_OK);
    if (strcmp(slurp(CFG2), want) != 0) printf("  saved:\n%s---\n  wanted:\n%s---\n", slurp(CFG2), want);
    CHECK(strcmp(slurp(CFG2), want) == 0);
    gptps_shutdown(e);
    e = NULL;
    CHECK(gptps_open(CFG2, &e) == GPTPS_OK);                   /* and it reopens */
    if (e) gptps_shutdown(e);
}

static void test_save_placement(void)
{
    /* next to a sibling written as a dotted key, in that key's table */
    save_case("[resources]\ngpu = 1\n[tasks.render]\nresources.gpu = 1\n", "tasks.render.resources.cpu", "3",
              "[resources]\ngpu = 1\n[tasks.render]\nresources.gpu = 1\nresources.cpu = 3\n");
    /* next to a sibling at the top level, on the file's first line, not after what follows */
    save_case("limits.max_intake_depth = 5\n# note\n", "limits.shutdown_grace_ms", "700",
              "limits.max_intake_depth = 5\nlimits.shutdown_grace_ms = 700\n# note\n");
    /* but not next to a key that only has its dot in the same place */
    save_case("resources.gpu = 1\n", "scheduler.reserve_after_skips", "3",
              "resources.gpu = 1\n\n[scheduler]\nreserve_after_skips = 3\n");
    /* after the table's last key - even when that key is the file's first entry */
    save_case("[limits]\nmax_intake_depth = 5\n[scheduler]\nreserve_after_skips = 4\n", "limits.shutdown_grace_ms", "700",
              "[limits]\nmax_intake_depth = 5\nshutdown_grace_ms = 700\n[scheduler]\nreserve_after_skips = 4\n");
    /* under a table with no keys that is the file's last line, newline or not */
    save_case("[limits]\nmax_intake_depth = 5\n[scheduler]", "scheduler.reserve_after_skips", "3",
              "[limits]\nmax_intake_depth = 5\n[scheduler]\nreserve_after_skips = 3\n");
    /* a table the file lacks: at the end, after a blank line */
    save_case("[limits]\nmax_intake_depth = 5\n", "scheduler.reserve_after_skips", "3",
              "[limits]\nmax_intake_depth = 5\n\n[scheduler]\nreserve_after_skips = 3\n");
    /* a key with no table: above the first table and the comment on it... */
    save_case("# settings\n[limits]\nmax_intake_depth = 5\n", "verbose", "true",
              "verbose = true\n\n# settings\n[limits]\nmax_intake_depth = 5\n");
    /* ...or at the end of a file with no table at all */
    save_case("# just a note\n", "verbose", "true", "# just a note\nverbose = true\n");
    /* the byte count where the file has neither of its spellings... */
    save_case("[limits]\nmax_intake_depth = 5\n", "limits.max_memory_bytes", "1073741824",
              "[limits]\nmax_intake_depth = 5\nmax_memory_bytes = 1073741824\n");
    /* ...and in GiB where that is the file's first entry */
    save_case("[limits]\nmax_memory_gb = 2\n", "limits.max_memory_bytes", "1073741824",
              "[limits]\nmax_memory_gb = 1\n");
    /* with both spellings the byte count wins, so only its line changes */
    save_case("limits.max_memory_bytes = 2147483648\nlimits.max_memory_gb = 2\n", "limits.max_memory_bytes", "1073741824",
              "limits.max_memory_bytes = 1073741824\nlimits.max_memory_gb = 2\n");
}

/* The parser's edges, as the same mutation run found them unpinned. */
static void test_parser_edges(void)
{
    static const struct { const char *esc, *bytes; } UTF8[] = {
        { "\\u0080", "\xc2\x80" }, { "\\u07FF", "\xdf\xbf" }, { "\\u0800", "\xe0\xa0\x80" },
        { "\\uD7FF", "\xed\x9f\xbf" }, { "\\uE000", "\xee\x80\x80" }, { "\\uFFFF", "\xef\xbf\xbf" },
        { "\\U00010000", "\xf0\x90\x80\x80" }, { "\\U0010FFFF", "\xf4\x8f\xbf\xbf" },
    };
    static const char *const NOT_CHARS[] = { "\\uD800", "\\uDFFF", "\\U00110000", "\\u0000" };
    gptps *e = NULL;
    char text[600], name[300];
    size_t i;
    for (i = 0; i < sizeof UTF8 / sizeof UTF8[0]; ++i) {        /* each code point's UTF-8, to the byte */
        snprintf(text, sizeof text, "[app]\ns = \"%s\"\n", UTF8[i].esc);
        put(CFG, text);
        CHECK(gptps_open(CFG, &e) == GPTPS_OK);
        if (!e) continue;
        CHECK(gptps_define_global(e, "app.s", GPTPS_SETTING_STRING, "", NULL, 0) == GPTPS_OK);
        CHECK(has_value(e, "app.s", UTF8[i].bytes));
        gptps_shutdown(e); e = NULL;
    }
    for (i = 0; i < sizeof NOT_CHARS / sizeof NOT_CHARS[0]; ++i) {  /* surrogates, past U+10FFFF, NUL */
        snprintf(text, sizeof text, "[app]\ns = \"%s\"\n", NOT_CHARS[i]);
        refused(text, "a \\u escape that is not a character");
    }
    /* numbers as a file may spell them */
    put(CFG, "[app]\nn = +8\nf = .5\ni = -1\nbig = 18446744073709551615\n");
    CHECK(gptps_open(CFG, &e) == GPTPS_OK);
    if (e) {
        CHECK(gptps_define_global(e, "app.n", GPTPS_SETTING_UINT, "0", NULL, 0) == GPTPS_OK);
        CHECK(gptps_define_global(e, "app.f", GPTPS_SETTING_DOUBLE, "0", NULL, 0) == GPTPS_OK);
        CHECK(gptps_define_global(e, "app.i", GPTPS_SETTING_INT, "0", NULL, 0) == GPTPS_OK);
        CHECK(gptps_define_global(e, "app.big", GPTPS_SETTING_UINT, "0", NULL, 0) == GPTPS_OK);
        CHECK(has_value(e, "app.n", "+8") && has_value(e, "app.f", ".5") && has_value(e, "app.i", "-1"));
        CHECK(has_value(e, "app.big", "18446744073709551615"));            /* 2^64 - 1 */
        CHECK(gptps_config_check(e) == GPTPS_OK);
        gptps_shutdown(e); e = NULL;
    }
    put(CFG, "[app]\nf = 1.7976931348623157e308\ng = -1.7976931348623157e308\n");   /* +-DBL_MAX */
    CHECK(gptps_open(CFG, &e) == GPTPS_OK);
    if (e) { gptps_shutdown(e); e = NULL; }
    refused("f = 1.8e308\n", CFG ":1: f: 1.8e308 is too large a number");
    refused("big = 18446744073709551616\n", CFG ":1: big: 18446744073709551616 is too large a number");   /* 2^64 */
    refused("big = -9223372036854775809\n", "is too large a number");
    /* an empty part of a key or table, where the empty part comes first */
    refused(".a = 1\n", CFG ":1: the key has an empty part");
    refused("[.a]\nk = 1\n", CFG ":1: the table name has an empty part");
    /* a table name as long as the parser holds, and one past it */
    memset(name, 'n', sizeof name);
    name[255] = 0;
    snprintf(text, sizeof text, "[%s]\nk = 1\n", name);
    put(CFG, text);
    CHECK(gptps_open(CFG, &e) == GPTPS_OK);                    /* 255 bytes: kept, waiting */
    if (e) { gptps_shutdown(e); e = NULL; }
    name[255] = 'n'; name[256] = 0;
    snprintf(text, sizeof text, "[%s]\nk = 1\n", name);
    refused(text, CFG ":1: the table name is too long");
    /* a key set twice at the top level, not only in a table */
    refused("x = 1\nx = 2\n", CFG ":2: x is set twice (first on line 1)");
    /* a line with no key, said two ways */
    refused("= 1\n", CFG ":1: missing key before =");
    refused("!x = 1\n", CFG ":1: expected key = value");
    /* every bad line shown: no "and N more" */
    refused("[limits\n[scheduler\n", CFG ":2: the table name has no closing ]");
    CHECK(logged(CFG ":1: the table name has no closing ]") && !logged("more line"));
    /* an empty file, and one of comments, are a config that sets nothing */
    put(CFG, "");
    CHECK(gptps_open(CFG, &e) == GPTPS_OK);
    if (e) { gptps_shutdown(e); e = NULL; }
    put(CFG, "# nothing yet\n\n");
    CHECK(gptps_open(CFG, &e) == GPTPS_OK);
    if (e) { gptps_shutdown(e); e = NULL; }
}

static void test_set_ex(void)
{
    gptps *e = NULL;
    char why[256];
    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    if (!e) return;
    CHECK(gptps_settings_set_ex(e, "limits.max_concurrent_task", "4", why, sizeof why) == GPTPS_E_NOTFOUND);
    CHECK(strcmp(why, "no setting is named limits.max_concurrent_task (did you mean limits.max_concurrent_tasks?)") == 0);
    CHECK(gptps_settings_set_ex(e, "limits.max_concurrent_tasks", "70000", why, sizeof why) == GPTPS_E_CONFIG);
    CHECK(strcmp(why, "70000 must be a whole number between 0 and 65536") == 0);
    CHECK(gptps_settings_set_ex(e, "limits.max_concurrent_tasks", "4", why, sizeof why) == GPTPS_OK);
    CHECK(why[0] == 0);
    CHECK(gptps_settings_set_ex(e, "limits.max_concurrent_tasks", "5", NULL, 0) == GPTPS_OK);
    /* the edges a mutation run (tools/mutate.py) found untested: a NULL argument; a
     * string's own spaces, which only a number loses; a value as long as the buffer
     * the trim copies it into */
    CHECK(gptps_settings_set_ex(e, NULL, "4", why, sizeof why) == GPTPS_E_INVAL);
    CHECK(gptps_settings_set_ex(e, "limits.max_concurrent_tasks", NULL, why, sizeof why) == GPTPS_E_INVAL);
    CHECK(gptps_define_global(e, "app.label", GPTPS_SETTING_STRING, "x", NULL, 0) == GPTPS_OK);
    CHECK(gptps_settings_set_ex(e, "app.label", "  padded  ", why, sizeof why) == GPTPS_OK);
    CHECK(has_value(e, "app.label", "  padded  "));
    {
        char longv[GPTPS_SETTINGS_VALUE_MAX + 1];
        memset(longv, '1', GPTPS_SETTINGS_VALUE_MAX);
        longv[GPTPS_SETTINGS_VALUE_MAX] = 0;                  /* 256 digits: past the buffer by one */
        CHECK(gptps_settings_set_ex(e, "limits.max_concurrent_tasks", longv, why, sizeof why) == GPTPS_E_CONFIG);
        longv[GPTPS_SETTINGS_VALUE_MAX - 1] = 0;              /* 255: fits, and still out of range */
        CHECK(gptps_settings_set_ex(e, "limits.max_concurrent_tasks", longv, why, sizeof why) == GPTPS_E_CONFIG);
    }
    /* a live set refuses what would truncate: the per-task keys declare their width */
    CHECK(reg(e, "t") == GPTPS_OK);
    CHECK(gptps_settings_set_ex(e, "tasks.t.timeout_seconds", "4294967296", why, sizeof why) == GPTPS_E_CONFIG);
    CHECK(has_value(e, "tasks.t.timeout_seconds", "0"));
    gptps_shutdown(e);
}

int main(void)
{
    g_logm = gptps_mutex_create();
    gptps_set_log_sink(sink, NULL);
    test_mistakes();
    test_deferred();
    test_deployment();
    test_reload();
    test_save_in_place();
    test_set_ex();
    test_spelling();
    test_round_trip();
    test_open_with_addons();
    test_reload_bookkeeping();
    test_define_race();
    test_concurrent_saves();
    test_save_logs_unlocked();
    test_locale();
    test_submit_from_write();
    test_callback_mark();
    test_addon_lifecycle();
    test_failed_setup_undoes_only_its_own();
    test_watch_while_setting();
    test_save_placement();
    test_parser_edges();
    gptps_set_log_sink(NULL, NULL);
    remove(CFG);
    if (fails) { printf("%d config check(s) FAILED\n", fails); return 1; }
    printf("all config checks passed\n");
    return 0;
}
