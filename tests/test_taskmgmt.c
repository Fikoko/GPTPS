/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_taskmgmt.c - runtime task management + generic settings (v1.8):
 *   - enumeration (count / get_info / exists)
 *   - enable / disable (pause)
 *   - unregister: REJECT_IF_BUSY / DRAIN / CANCEL, threaded + manual
 *   - re-register after removal; dead-letter survives removal
 *   - clone
 *   - generic global settings (define / validate / round-trip)
 *   - generic per-task settings (define before+after registration / accessor)
 * Headless / portable.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }

static int started, finished;
static long captured_quality = -1;

static void obs(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_STARTED)  inc(&started);
    if (ev->kind == GPTPS_EV_FINISHED) {
        inc(&finished);
        if (strcmp(ev->task_name, "img") == 0 && ev->result && ev->result_len == sizeof(long)) {
            long q; memcpy(&q, ev->result, sizeof q);
            __atomic_store_n(&captured_quality, q, __ATOMIC_SEQ_CST);
        }
    }
}

static gptps_status task_fast(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_OK; }

/* ~30ms of cancellable work, so DRAIN has something to wait on */
static gptps_status task_work(gptps_ctx *ctx, void *ud)
{
    uint64_t s = gptps_now_ms(ctx); (void)ud;
    while (!gptps_is_cancelled(ctx)) if (gptps_now_ms(ctx) - s > 30) break;
    return GPTPS_OK;
}

/* spins until cancelled - the CANCEL / REJECT-busy target */
static gptps_status task_spin(gptps_ctx *ctx, void *ud)
{
    (void)ud;
    while (!gptps_is_cancelled(ctx)) { /* busy-wait for the cancel flag */ }
    return GPTPS_E_CANCELLED;
}

/* always fails -> dead-letters under the default policy */
static gptps_status task_fail(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_E_TASK; }

/* reads its per-task "quality" setting and returns it as the result */
static gptps_status task_img(gptps_ctx *ctx, void *ud)
{
    long q = -1; (void)ud;
    gptps_task_setting_int(ctx, "quality", &q);
    return gptps_result_set(ctx, &q, sizeof q);
}

static void reg(gptps *e, const char *name, gptps_run_fn fn)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = fn; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

static int find_info(gptps *e, const char *name, gptps_task_info *out)
{
    size_t i, n = gptps_task_count(e);
    for (i = 0; i < n; ++i) {
        memset(out, 0, sizeof *out); out->struct_size = sizeof *out;
        if (gptps_task_get_info(e, i, out) == GPTPS_OK && out->name && strcmp(out->name, name) == 0) return 1;
    }
    return 0;
}

static int has_setting(gptps *e, const char *key)
{
    char b[GPTPS_SETTINGS_VALUE_MAX];
    return gptps_settings_get(e, key, b, sizeof b) == GPTPS_OK;
}

static void wait_started(int target)
{
    uint64_t s = gptps_now_ms(NULL);
    while (get(&started) < target && gptps_now_ms(NULL) - s < 2000) { }
}

/* Wait until a task type has no queued/in-flight work (or is gone), so a
 * REJECT_IF_BUSY unregister is deterministic instead of timing-dependent. */
static void wait_idle(gptps *e, const char *name)
{
    gptps_task_info info;
    uint64_t s = gptps_now_ms(NULL);
    while (gptps_now_ms(NULL) - s < 2000) {
        if (!find_info(e, name, &info)) break;            /* gone => idle */
        if (info.queued == 0 && info.running == 0) break; /* nothing outstanding */
    }
}

static void dl_capture(const gptps_dead_letter *dl, void *ud)
{ char *out = (char *)ud; snprintf(out, 64, "%s", dl->task_name); }

int main(void)
{
    gptps *e = NULL;
    gptps_task_info info;
    gptps_handle h;
    char b[GPTPS_SETTINGS_VALUE_MAX];
    int i;

    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    if (!e) return 1;
    gptps_register_observer(e, obs, NULL);

    /* ---- enumeration ---- */
    CHECK(gptps_task_count(e) == 0);
    reg(e, "fast", task_fast);
    reg(e, "work", task_work);
    CHECK(gptps_task_count(e) == 2);
    CHECK(gptps_task_exists(e, "fast") == 1);
    CHECK(gptps_task_exists(e, "nope") == 0);
    CHECK(find_info(e, "fast", &info));
    CHECK(info.exec == GPTPS_EXEC_INPROC && info.enabled == 1 && info.removed == 0);

    /* ---- enable / disable (pause) ---- */
    CHECK(gptps_set_task_enabled(e, "fast", 0) == GPTPS_OK);
    CHECK(gptps_task_exists(e, "fast") == 0);                 /* disabled => not "available" */
    CHECK(gptps_submit(e, "fast", NULL, 0, &h) == GPTPS_E_NOTFOUND);
    CHECK(gptps_set_task_enabled(e, "fast", 1) == GPTPS_OK);
    CHECK(gptps_submit(e, "fast", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_set_task_enabled(e, "ghost", 1) == GPTPS_E_NOTFOUND);

    /* ---- REJECT_IF_BUSY: busy then idle; CANCEL with mixed running/queued state ---- */
    reg(e, "spin", task_spin);
    __atomic_store_n(&started, 0, __ATOMIC_SEQ_CST);
    /* submit several spinners: some run (in-flight), the rest queue (intake/ready).
     * CANCEL must terminate them all without hanging the drain - a ready item whose
     * cancel flag the worker reset would spin forever and trip the CTest timeout. */
    for (i = 0; i < 8; ++i) CHECK(gptps_submit(e, "spin", NULL, 0, &h) == GPTPS_OK);
    wait_started(1);
    CHECK(gptps_unregister_task(e, "spin", GPTPS_REMOVE_REJECT_IF_BUSY) == GPTPS_E_BUSY);  /* in-flight */
    CHECK(gptps_unregister_task(e, "spin", GPTPS_REMOVE_CANCEL) == GPTPS_OK);              /* cancels all */
    CHECK(gptps_task_exists(e, "spin") == 0);
    CHECK(has_setting(e, "tasks.spin.timeout_seconds") == 0);  /* settings torn down */

    /* an idle task removes cleanly with REJECT_IF_BUSY (the default).
     * Wait for the "fast" submit above (line ~129) to actually finish first:
     * under suite load it may still be in-flight, and REJECT_IF_BUSY would then
     * correctly refuse with E_BUSY (a test race, not an engine bug). */
    wait_idle(e, "fast");
    CHECK(gptps_unregister_task(e, "fast", 0) == GPTPS_OK);
    CHECK(gptps_task_count(e) == 1);                           /* only "work" remains */
    CHECK(gptps_submit(e, "fast", NULL, 0, &h) == GPTPS_E_NOTFOUND);

    /* re-register a removed name: clean, with fresh settings */
    reg(e, "fast", task_fast);
    CHECK(gptps_task_exists(e, "fast") == 1);
    CHECK(has_setting(e, "tasks.fast.max_retries"));

    /* ---- DRAIN: queued work finishes, then the type is gone ---- */
    __atomic_store_n(&finished, 0, __ATOMIC_SEQ_CST);
    CHECK(gptps_submit(e, "work", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_submit(e, "work", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_submit(e, "work", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_unregister_task(e, "work", GPTPS_REMOVE_DRAIN) == GPTPS_OK);  /* blocks until drained */
    CHECK(get(&finished) == 3);                               /* all queued work completed */
    CHECK(gptps_task_exists(e, "work") == 0);

    /* ---- dead-letter survives removal ---- */
    reg(e, "fail", task_fail);                                /* default policy = dead_letter, 0 retries */
    CHECK(gptps_submit(e, "fail", NULL, 0, &h) == GPTPS_OK);
    { uint64_t s = gptps_now_ms(NULL); while (gptps_dead_letter_count(e) < 1 && gptps_now_ms(NULL) - s < 2000) {} }
    CHECK(gptps_dead_letter_count(e) == 1);
    CHECK(gptps_unregister_task(e, "fail", GPTPS_REMOVE_REJECT_IF_BUSY) == GPTPS_OK);  /* DL doesn't count as busy */
    CHECK(gptps_dead_letter_count(e) == 1);                   /* retained across removal */
    { char nm[64] = ""; CHECK(gptps_dead_letter_drain(e, dl_capture, nm) == 1);
      CHECK(strcmp(nm, "fail") == 0); }                       /* name still resolves after free */

    /* DRAIN a task whose items fail and dead-letter DURING the drain: the dead-letter
     * event name must not alias the reg the unregister thread frees (UAF/race guard;
     * meaningful under TSan/ASan). */
    reg(e, "failr", task_fail);
    for (i = 0; i < 6; ++i) CHECK(gptps_submit(e, "failr", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_unregister_task(e, "failr", GPTPS_REMOVE_DRAIN) == GPTPS_OK);
    CHECK(gptps_task_exists(e, "failr") == 0);
    gptps_dead_letter_drain(e, NULL, NULL);                    /* discard what dead-lettered */

    /* ---- clone ---- */
    CHECK(gptps_set_task_priority(e, "fast", 3) == GPTPS_OK);
    CHECK(gptps_clone_task(e, "fast", "fast_hi") == GPTPS_OK);
    CHECK(gptps_clone_task(e, "fast", "fast_hi") == GPTPS_E_DUP);
    CHECK(gptps_clone_task(e, "ghost", "x") == GPTPS_E_NOTFOUND);
    CHECK(gptps_task_exists(e, "fast_hi") == 1);
    CHECK(gptps_submit(e, "fast_hi", NULL, 0, &h) == GPTPS_OK);  /* clone shares the run fn */
    CHECK(find_info(e, "fast_hi", &info) && info.priority == 3); /* priority carried over */
    CHECK(has_setting(e, "tasks.fast_hi.priority"));            /* its own settings namespace */

    /* ---- generic GLOBAL settings ---- */
    CHECK(gptps_define_global(e, "app.max_upload_mb", GPTPS_SETTING_UINT, "10", "0..4096", 0) == GPTPS_OK);
    CHECK(gptps_define_global(e, "app.max_upload_mb", GPTPS_SETTING_UINT, "10", "0..4096", 0) == GPTPS_E_DUP);
    CHECK(gptps_settings_get(e, "app.max_upload_mb", b, sizeof b) == GPTPS_OK && strcmp(b, "10") == 0);
    CHECK(gptps_settings_set(e, "app.max_upload_mb", "20") == GPTPS_OK);
    CHECK(gptps_settings_get(e, "app.max_upload_mb", b, sizeof b) == GPTPS_OK && strcmp(b, "20") == 0);
    CHECK(gptps_settings_set(e, "app.max_upload_mb", "5000") == GPTPS_E_CONFIG);  /* range */
    CHECK(gptps_settings_set(e, "app.max_upload_mb", "-1") == GPTPS_E_CONFIG);    /* unsigned */
    CHECK(gptps_define_global(e, "app.mode", GPTPS_SETTING_ENUM, "fast", "fast|slow|auto", 0) == GPTPS_OK);
    CHECK(gptps_settings_set(e, "app.mode", "slow") == GPTPS_OK);
    CHECK(gptps_settings_set(e, "app.mode", "bogus") == GPTPS_E_CONFIG);
    CHECK(gptps_define_global(e, "app.bad_enum", GPTPS_SETTING_ENUM, NULL, NULL, 0) == GPTPS_E_CONFIG);

    /* ---- generic PER-TASK settings (define AFTER an existing task; applies to it + future) ---- */
    CHECK(gptps_define_task_setting(e, "quality", GPTPS_SETTING_UINT, "75", "0..100", 0) == GPTPS_OK);
    CHECK(gptps_define_task_setting(e, "quality", GPTPS_SETTING_UINT, "75", "0..100", 0) == GPTPS_E_DUP);
    CHECK(gptps_define_task_setting(e, "with.dot", GPTPS_SETTING_UINT, "1", NULL, 0) == GPTPS_E_INVAL);
    CHECK(has_setting(e, "tasks.fast.quality"));               /* materialized on the existing task */
    CHECK(gptps_settings_get(e, "tasks.fast.quality", b, sizeof b) == GPTPS_OK && strcmp(b, "75") == 0);
    reg(e, "img", task_img);                                   /* future task gets it too */
    CHECK(has_setting(e, "tasks.img.quality"));
    CHECK(gptps_settings_set(e, "tasks.img.quality", "90") == GPTPS_OK);
    CHECK(gptps_settings_set(e, "tasks.img.quality", "200") == GPTPS_E_CONFIG);   /* range */
    CHECK(gptps_settings_set(e, "tasks.fast.quality", "40") == GPTPS_OK);         /* per-instance */
    CHECK(gptps_settings_get(e, "tasks.img.quality", b, sizeof b) == GPTPS_OK && strcmp(b, "90") == 0);

    /* the accessor reads THIS instance's resolved value inside run() */
    __atomic_store_n(&captured_quality, -1, __ATOMIC_SEQ_CST);
    CHECK(gptps_submit(e, "img", NULL, 0, &h) == GPTPS_OK);
    { uint64_t s = gptps_now_ms(NULL);
      while (__atomic_load_n(&captured_quality, __ATOMIC_SEQ_CST) < 0 && gptps_now_ms(NULL) - s < 2000) {} }
    CHECK(__atomic_load_n(&captured_quality, __ATOMIC_SEQ_CST) == 90);

    /* removing a task takes its generic per-task setting with it, leaving others */
    CHECK(gptps_unregister_task(e, "img", GPTPS_REMOVE_DRAIN) == GPTPS_OK);
    CHECK(has_setting(e, "tasks.img.quality") == 0);
    CHECK(has_setting(e, "tasks.fast.quality") == 1);

    gptps_shutdown(e);

    /* ---- generic global round-trips through TOML ---- */
    {
        gptps *e2 = NULL;
        CHECK(gptps_open(NULL, &e2) == GPTPS_OK);
        if (e2) {
            CHECK(gptps_define_global(e2, "app.threshold", GPTPS_SETTING_DOUBLE, "0.5", "0..1", 0) == GPTPS_OK);
            CHECK(gptps_settings_set(e2, "app.threshold", "0.8") == GPTPS_OK);
            CHECK(gptps_settings_save(e2, "taskmgmt_rt.toml") == GPTPS_OK);
            gptps_shutdown(e2);
        }
        {
            gptps *e3 = NULL;
            CHECK(gptps_open("taskmgmt_rt.toml", &e3) == GPTPS_OK);
            if (e3) {
                CHECK(gptps_define_global(e3, "app.threshold", GPTPS_SETTING_DOUBLE, "0.5", "0..1", 0) == GPTPS_OK);
                CHECK(gptps_settings_reload(e3, NULL) == GPTPS_OK);     /* applies the saved value */
                CHECK(gptps_settings_get(e3, "app.threshold", b, sizeof b) == GPTPS_OK);
                CHECK(strncmp(b, "0.8", 3) == 0);
                gptps_shutdown(e3);
            }
            remove("taskmgmt_rt.toml");
        }
    }

    /* ---- MANUAL mode: removal between steps ---- */
    {
        gptps *em = NULL;
        gptps_config cfg;
        size_t ran = 0;
        memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg;
        cfg.limits.struct_size = sizeof cfg.limits; cfg.mode = GPTPS_RUN_MANUAL;
        CHECK(gptps_open_ex(&cfg, &em) == GPTPS_OK);
        if (em) {
            reg(em, "m", task_fast);
            /* queued but not stepped => DRAIN refuses, CANCEL drops + removes */
            CHECK(gptps_submit(em, "m", NULL, 0, &h) == GPTPS_OK);
            CHECK(gptps_unregister_task(em, "m", GPTPS_REMOVE_DRAIN) == GPTPS_E_BUSY);
            CHECK(gptps_unregister_task(em, "m", GPTPS_REMOVE_CANCEL) == GPTPS_OK);
            CHECK(gptps_task_exists(em, "m") == 0);
            /* fresh task, stepped to completion, then removed idle */
            reg(em, "m2", task_fast);
            CHECK(gptps_submit(em, "m2", NULL, 0, &h) == GPTPS_OK);
            while (gptps_step(em, &ran) == GPTPS_OK && ran) { }
            CHECK(gptps_unregister_task(em, "m2", GPTPS_REMOVE_REJECT_IF_BUSY) == GPTPS_OK);
            gptps_shutdown(em);
        }
    }

    if (fails) { printf("%d taskmgmt check(s) FAILED\n", fails); return 1; }
    printf("all taskmgmt checks passed\n");
    return 0;
}
