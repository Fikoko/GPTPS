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
#include <string.h>

#ifndef ADDON_DEMO_PATH
#define ADDON_DEMO_PATH "./addon_demo.so"
#endif

#define CFG_PATH "gptps_toml_test.toml"

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

int main(void)
{
    gptps *e = NULL;
    gptps_handle h;
    gptps_task_def d;

    CHECK(write_cfg() == 0);

    /* missing file -> config error */
    CHECK(gptps_open("/no/such/dir/none.toml", &e) == GPTPS_E_CONFIG);

    test_parser();

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
