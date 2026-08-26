/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_toml.c - config-file milestone. Two layers:
 *   (1) the TOML-subset parser in isolation (int/float/bool/string/array, dotted
 *       tables, comments);
 *   (2) gptps_open(path) end-to-end: [limits] feed the engine, addons[] auto-load,
 *       and [task_defaults] / [tasks.<name>] override compiled-in task defaults.
 */
#include "gptps.h"
#include "gptps_internal.h"   /* internal TOML parser API */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ADDON_DEMO_PATH
#define ADDON_DEMO_PATH "./addon_demo.so"
#endif

#define CFG_PATH "gptps_toml_test.toml"
#define ESC_PATH "gptps_toml_esc.toml"
#define BAD_PATH "gptps_toml_bad.toml"

/* Any single allocation the parser makes is bounded by the file-size cap it
 * enforces (16 MiB); this is that, with generous headroom. It is a REGRESSION
 * threshold, not a tuning knob - see test_dir_as_config_path. */
#define SANE_ALLOC_MAX (64UL * 1024UL * 1024UL)

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

/* ---- event counters (shared; reset between phases) ---- */
static int c_started, c_finished, c_failed, c_retried, c_dead;
static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static void reset(void)
{
    __atomic_store_n(&c_started, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&c_finished, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&c_failed, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&c_retried, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&c_dead, 0, __ATOMIC_SEQ_CST);
}
static void on_ev(const gptps_event *ev, void *ud)
{
    (void)ud;
    switch (ev->kind) {
        case GPTPS_EV_STARTED:       inc(&c_started);  break;
        case GPTPS_EV_FINISHED:      inc(&c_finished); break;
        case GPTPS_EV_FAILED:        inc(&c_failed);   break;
        case GPTPS_EV_RETRIED:       inc(&c_retried);  break;
        case GPTPS_EV_DEAD_LETTERED: inc(&c_dead);     break;
        default: break;
    }
}

static gptps_status task_fail(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_E_TASK; }

static void def_init(gptps_task_def *d, const char *name, uint32_t retries, gptps_on_failure on_fail)
{
    memset(d, 0, sizeof *d);
    d->struct_size = sizeof *d; d->name = name; d->run = task_fail; d->exec = GPTPS_EXEC_INPROC;
    d->default_cost.struct_size = sizeof d->default_cost; d->default_cost.mem_bytes = 1024;
    d->default_policy.struct_size = sizeof d->default_policy;
    d->default_policy.max_retries = retries;
    d->default_policy.on_failure = on_fail;
}

static int write_cfg(void)
{
    FILE *f = fopen(CFG_PATH, "wb");
    if (!f) return -1;
    fprintf(f,
        "# GPTPS config (subset TOML)\n"
        "addons = [\"%s\"]   # auto-loaded at open\n"
        "\n"
        "[limits]\n"
        "max_concurrent_tasks = 3\n"
        "max_memory_gb = 1.5\n"
        "\n"
        "[misc]\n"
        "verbose = true\n"
        "\n"
        "[task_defaults]\n"
        "max_retries = 0\n"
        "on_failure  = \"drop\"\n"
        "\n"
        "[tasks.failer]\n"
        "max_retries    = 2\n"
        "on_failure     = \"dead_letter\"\n"
        "timeout_seconds = 7\n",
        ADDON_DEMO_PATH);
    fclose(f);
    return 0;
}

static void test_parser(void)
{
    gptps_toml *t = gptps_toml_parse_file(CFG_PATH, NULL, 0);
    const char *const *arr = NULL;
    long long ll = 0; double d = 0; int b = 0; const char *s;

    CHECK(t != NULL);
    if (!t) return;

    /* top-level string array */
    CHECK(gptps_toml_str_array(t, "", "addons", &arr) == 1);
    CHECK(arr && strcmp(arr[0], ADDON_DEMO_PATH) == 0);

    /* ints + floats under [limits] */
    CHECK(gptps_toml_int(t, "limits", "max_concurrent_tasks", &ll) && ll == 3);
    CHECK(gptps_toml_double(t, "limits", "max_memory_gb", &d) && d > 1.49 && d < 1.51);

    /* bool */
    CHECK(gptps_toml_bool(t, "misc", "verbose", &b) && b == 1);

    /* dotted [tasks.failer] table */
    CHECK(gptps_toml_int(t, "tasks.failer", "max_retries", &ll) && ll == 2);
    CHECK(gptps_toml_int(t, "tasks.failer", "timeout_seconds", &ll) && ll == 7);
    s = gptps_toml_str(t, "tasks.failer", "on_failure");
    CHECK(s && strcmp(s, "dead_letter") == 0);

    /* [task_defaults] */
    s = gptps_toml_str(t, "task_defaults", "on_failure");
    CHECK(s && strcmp(s, "drop") == 0);

    /* absent keys report not-found */
    CHECK(gptps_toml_int(t, "limits", "nope", &ll) == 0);
    CHECK(gptps_toml_str(t, "tasks.missing", "on_failure") == NULL);

    gptps_toml_free(t);
}


/* ---- recording allocator: the witness for the directory-as-config-path bug ----
 * gptps_toml_parse_file used to trust fseek/ftell, and ftell on a directory reports
 * LONG_MAX on POSIX, so the parser asked for an ~8 EiB buffer. The RETURN VALUE does
 * not distinguish the bug from the fix - a refused huge malloc surfaces as the same
 * NULL - so the only honest witness is the size that was REQUESTED. These hooks
 * record the peak request and refuse anything absurd rather than handing it to
 * libc, so a regression fails a check instead of taking the process down with it. */
static size_t a_peak;
static void *rec_malloc(size_t n, void *ud)
{
    (void)ud;
    if (n > a_peak) a_peak = n;
    return (n > SANE_ALLOC_MAX) ? NULL : malloc(n);
}
static void *rec_realloc(void *p, size_t n, void *ud)
{
    (void)ud;
    if (n > a_peak) a_peak = n;
    return (n > SANE_ALLOC_MAX) ? NULL : realloc(p, n);
}
static void rec_free(void *p, void *ud) { (void)ud; free(p); }

/* A directory passed as the config path must be refused as a config error - and
 * refused by INSPECTION, not by an allocator that happened to say no. Simply
 * reaching the end of this function is the other half of the assertion: the old
 * code neither hung nor aborted here only by the grace of malloc returning NULL,
 * and under a sanitizer (or a host allocator that aborts on failure) it did. */
static void test_dir_as_config_path(void)
{
    gptps_allocator a;
    gptps *e = NULL;
    gptps_toml *t;
    char err[128];

    memset(&a, 0, sizeof a);
    a.struct_size = sizeof a;
    a.malloc_fn = rec_malloc; a.realloc_fn = rec_realloc; a.free_fn = rec_free;
    a_peak = 0;
    CHECK(gptps_set_allocator(&a) == GPTPS_OK);

    /* the repro's own path, then "." - the second is a directory on every platform,
     * so the guard is still exercised where /tmp does not exist */
    err[0] = 0;
    t = gptps_toml_parse_file("/tmp", err, sizeof err);
    CHECK(t == NULL);
    gptps_toml_free(t);
    err[0] = 0;
    t = gptps_toml_parse_file(".", err, sizeof err);
    CHECK(t == NULL);
    CHECK(err[0] != 0);            /* and it must SAY why, not fail silently */
    gptps_toml_free(t);

    CHECK(gptps_open("/tmp", &e) == GPTPS_E_CONFIG);
    if (e) { gptps_shutdown(e); e = NULL; }
    CHECK(gptps_open(".", &e) == GPTPS_E_CONFIG);
    if (e) { gptps_shutdown(e); e = NULL; }

    CHECK(gptps_set_allocator(NULL) == GPTPS_OK);
    CHECK(a_peak <= SANE_ALLOC_MAX);   /* the ~8 EiB request must never have happened */
}

/* strip_comment() used to flip its in-string flag on the ESCAPED quote of
 * `"a\"b#c"`, conclude the following '#' started a comment and cut the line there.
 * The value came back as `a`: silent truncation of any string carrying an escaped
 * quote ahead of a '#', on every save->reload round trip (the round trip itself is
 * pinned in test_settings.c). The comment stripper must still strip real ones. */
static void test_comment_escapes(void)
{
    gptps_toml *t;
    const char *s;
    long long ll = 0;
    FILE *f = fopen(ESC_PATH, "wb");

    CHECK(f != NULL);
    if (!f) return;
    fputs("[app]\n"
          "motd = \"a\\\"b#c\"\n"        /* motd = "a\"b#c"   -> a"b#c    */
          "say  = \"hi\" #1\n"           /* a genuine comment still goes  */
          "x    = 5 # five\n"
          "p    = \"c:\\\\tmp\" # win\n", f);   /* escaped backslash, then a comment */
    fclose(f);

    t = gptps_toml_parse_file(ESC_PATH, NULL, 0);
    CHECK(t != NULL);
    if (t) {
        s = gptps_toml_str(t, "app", "motd");
        CHECK(s && strcmp(s, "a\"b#c") == 0);     /* used to be "a" */
        s = gptps_toml_str(t, "app", "say");
        CHECK(s && strcmp(s, "hi") == 0);         /* the fix must not eat comments */
        s = gptps_toml_str(t, "app", "p");
        CHECK(s && strcmp(s, "c:\\tmp") == 0);
        CHECK(gptps_toml_int(t, "app", "x", &ll) && ll == 5);
    }
    gptps_toml_free(t);
    remove(ESC_PATH);
}

static int write_bad(const char *body)
{
    FILE *f = fopen(BAD_PATH, "wb");
    if (!f) return -1;
    fputs(body, f);
    fclose(f);
    return 0;
}

/* [limits] read from a FILE used to be cast into its field, not checked against it.
 * Each case below installed a limit the operator never wrote - and in every case
 * the wrong value reads as "no limit at all", which is the dangerous direction. */
static void test_limits_range(void)
{
    gptps *e = NULL;
    char b[GPTPS_SETTINGS_VALUE_MAX];

    /* strtoll saturates and reports it only via errno: this used to become
     * LLONG_MAX, i.e. an unlimited memory budget. */
    CHECK(write_bad("[limits]\nmax_memory_bytes = 99999999999999999999999\n") == 0);
    CHECK(gptps_open(BAD_PATH, &e) == GPTPS_E_CONFIG);
    if (e) { gptps_shutdown(e); e = NULL; }

    /* -1 used to become 4294967295 and the engine then tried to start that many OS
     * threads (observed: spawns until RLIMIT_NPROC, then hangs). */
    CHECK(write_bad("[limits]\nmax_concurrent_tasks = -1\n") == 0);
    CHECK(gptps_open(BAD_PATH, &e) == GPTPS_E_CONFIG);
    if (e) { gptps_shutdown(e); e = NULL; }

    /* a sign test alone is not enough: a POSITIVE value wider than the uint32_t
     * destination truncates - 4294967296 became 0, i.e. unbounded intake. */
    CHECK(write_bad("[limits]\nmax_intake_depth = 4294967296\n") == 0);
    CHECK(gptps_open(BAD_PATH, &e) == GPTPS_E_CONFIG);
    if (e) { gptps_shutdown(e); e = NULL; }

    CHECK(write_bad("[limits]\nmax_memory_bytes = -1\n") == 0);
    CHECK(gptps_open(BAD_PATH, &e) == GPTPS_E_CONFIG);
    if (e) { gptps_shutdown(e); e = NULL; }

    /* ... and the guard must not be over-strict. An ordinary config still opens,
     * carrying exactly the values it asked for. */
    CHECK(write_bad("[limits]\n"
                    "max_memory_bytes = 1073741824\n"
                    "max_intake_depth = 4096\n"
                    "max_concurrent_tasks = 2\n") == 0);
    CHECK(gptps_open(BAD_PATH, &e) == GPTPS_OK);
    if (e) {
        CHECK(gptps_settings_get(e, "limits.max_memory_bytes", b, sizeof b) == GPTPS_OK);
        CHECK(strcmp(b, "1073741824") == 0);
        CHECK(gptps_settings_get(e, "limits.max_intake_depth", b, sizeof b) == GPTPS_OK);
        CHECK(strcmp(b, "4096") == 0);
        CHECK(gptps_settings_get(e, "limits.max_concurrent_tasks", b, sizeof b) == GPTPS_OK);
        CHECK(strcmp(b, "2") == 0);
        gptps_shutdown(e); e = NULL;
    }
    remove(BAD_PATH);
}

int main(void)
{
    gptps *e = NULL;
    gptps_handle h;
    gptps_task_def d;

    CHECK(write_cfg() == 0);

    /* missing file -> config error */
    CHECK(gptps_open("/no/such/dir/none.toml", &e) == GPTPS_E_CONFIG);

    test_parser();

    /* 1.1.0 config hardening (each pins a fix that shipped with its own repro) */
    test_dir_as_config_path();
    test_comment_escapes();
    test_limits_range();

    /* Phase A: addons[] auto-load -> the plugin's task runs end-to-end */
    reset();
    CHECK(gptps_open(CFG_PATH, &e) == GPTPS_OK);
    if (e) {
        gptps_set_event_cb(e, on_ev, NULL);
        CHECK(gptps_submit(e, "plugintask", NULL, 0, &h) == GPTPS_OK);
        gptps_shutdown(e); e = NULL;
        CHECK(get(&c_finished) == 1);
        CHECK(get(&c_dead) == 0);
    }

    /* Phase B: [tasks.failer] overrides the def (DROP/0 retries) with
     * dead_letter + 2 retries -> 3 attempts, then dead-lettered. */
    reset();
    CHECK(gptps_open(CFG_PATH, &e) == GPTPS_OK);
    if (e) {
        gptps_set_event_cb(e, on_ev, NULL);
        def_init(&d, "failer", 0, GPTPS_ON_FAILURE_DROP);
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        CHECK(gptps_submit(e, "failer", NULL, 0, &h) == GPTPS_OK);
        gptps_shutdown(e); e = NULL;
        CHECK(get(&c_started) == 3);   /* 1 + 2 retries (override) */
        CHECK(get(&c_retried) == 2);
        CHECK(get(&c_failed)  == 3);
        CHECK(get(&c_dead)    == 1);   /* dead_letter, not drop (override) */
    }

    /* Phase C: a task with NO [tasks.*] entry takes [task_defaults]
     * (max_retries=0, drop), overriding its compiled-in dead_letter/5-retries. */
    reset();
    CHECK(gptps_open(CFG_PATH, &e) == GPTPS_OK);
    if (e) {
        gptps_set_event_cb(e, on_ev, NULL);
        def_init(&d, "plain", 5, GPTPS_ON_FAILURE_DEAD_LETTER);
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        CHECK(gptps_submit(e, "plain", NULL, 0, &h) == GPTPS_OK);
        gptps_shutdown(e); e = NULL;
        CHECK(get(&c_started) == 1);   /* retries forced to 0 by task_defaults */
        CHECK(get(&c_retried) == 0);
        CHECK(get(&c_dead)    == 0);   /* drop, not dead_letter */
    }

    remove(CFG_PATH);
    if (fails) { printf("%d toml check(s) FAILED\n", fails); return 1; }
    printf("all toml checks passed\n");
    return 0;
}
