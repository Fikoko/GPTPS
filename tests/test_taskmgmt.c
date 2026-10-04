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
 *   - a type being registered is there for the calls that name it only once it is set up,
 *     parked in that window and raced through it
 * Headless / portable.
 */
#include "gptps.h"
#include "gptps_hal.h"     /* threads, for the registration window */
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

/* a host-registered setting: gptps_register_setting, no owner */
static char host_cell[GPTPS_SETTINGS_VALUE_MAX] = "1";
static size_t host_rd(void *t, char *b, size_t c) { (void)t; return (size_t)snprintf(b, c, "%s", host_cell); }
static gptps_status host_wr(void *t, const char *v) { (void)t; snprintf(host_cell, sizeof host_cell, "%s", v); return GPTPS_OK; }
static gptps_status host_setting(gptps *e, const char *key)
{
    gptps_setting_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.key = key; d.type = GPTPS_SETTING_STRING; d.hot = 1;
    d.desc = "host"; d.read = host_rd; d.write = host_wr;
    return gptps_register_setting(e, &d);
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

/* A type is there for other threads' calls that name it only once its registration
 * is done. gptps_register_task links the type in - its name taken at once - and only then
 * adds its settings, its per-task cells among them. An item submitted in between ran
 * without them: it read its own per-task setting as GPTPS_E_NOTFOUND (or, past a
 * definition with a file value, its default). And an unregister in between freed the
 * type while the registration was still writing into it, a use-after-free that ASan
 * reports - from a clone and an unregister of one name, both live control-plane calls.
 * The window is microseconds wide; a host write accessor that holds the settings
 * registry's lock parks the registration in it, which makes it certain here. Two
 * clones race for the name, so the one that is refused with GPTPS_E_DUP shows the
 * other has linked it in and is parked. Found by tests/test_stress_api.c. */
static gptps *g_rw_e;
static int g_rw_hold, g_rw_holding, g_rw_read, g_rw_dup, g_rw_done[2];
static gptps_status g_rw_clone_st[2], g_rw_st[2];
static gptps_status rw_body(gptps_ctx *c, void *u)
{
    long q = -1;
    gptps_status st = gptps_task_setting_int(c, "quality", &q);
    (void)u;
    __atomic_store_n(&g_rw_read, (st == GPTPS_OK && q == 7) ? 1 : 2, __ATOMIC_SEQ_CST);
    return GPTPS_OK;
}
static size_t rw_rd(void *t, char *b, size_t c) { (void)t; return (size_t)snprintf(b, c, "0"); }
static gptps_status rw_wr(void *t, const char *v)          /* holds the registry's lock until released */
{
    (void)t; (void)v;
    __atomic_store_n(&g_rw_holding, 1, __ATOMIC_SEQ_CST);
    /* A HAL call each turn: on a HAL that runs one thread at a time (tests/hal_sim.c),
     * a loop with no call in it keeps the CPU and nothing else ever runs. */
    while (__atomic_load_n(&g_rw_hold, __ATOMIC_SEQ_CST)) (void)gptps_now_ms(NULL);
    return GPTPS_OK;
}
static void *rw_setter(void *a) { (void)a; gptps_settings_set(g_rw_e, "app.hold", "1"); return NULL; }
static void *rw_clone(void *a)
{
    int i = (int)(size_t)a;
    g_rw_clone_st[i] = gptps_clone_task(g_rw_e, "src", "late");     /* read after the join */
    if (g_rw_clone_st[i] == GPTPS_E_DUP) __atomic_store_n(&g_rw_dup, 1, __ATOMIC_SEQ_CST);
    return NULL;
}
/* The two calls that, had they found "late", would go on to wait for the settings
 * lock: a clone from it registers the copy, and an unregister takes its settings
 * down - then frees it under the registration. Each runs on a thread of its own, so
 * that without the fix the test fails instead of waiting on itself. */
static void *rw_clone_from(void *a)
{
    (void)a;
    g_rw_st[0] = gptps_clone_task(g_rw_e, "late", "late2");
    __atomic_store_n(&g_rw_done[0], 1, __ATOMIC_SEQ_CST);
    return NULL;
}
static void *rw_unregister(void *a)
{
    (void)a;
    g_rw_st[1] = gptps_unregister_task(g_rw_e, "late", GPTPS_REMOVE_CANCEL);
    __atomic_store_n(&g_rw_done[1], 1, __ATOMIC_SEQ_CST);
    return NULL;
}
static int rw_wait(int *flag, int want)
{
    uint64_t s = gptps_now_ms(NULL);
    while (__atomic_load_n(flag, __ATOMIC_SEQ_CST) != want) if (gptps_now_ms(NULL) - s > 5000) return 0;
    return 1;
}
/* Runs op i on a thread of its own: 1 if it answered GPTPS_E_NOTFOUND at once. */
static int rw_not_found(void *(*op)(void *), int i, gptps_thread **t)
{
    *t = gptps_thread_start(op, NULL);
    if (!*t || !rw_wait(&g_rw_done[i], 1)) return 0;        /* waiting on the settings lock */
    return g_rw_st[i] == GPTPS_E_NOTFOUND;
}
static void test_registration_window(void)
{
    gptps_setting_def d;
    gptps_task_info info;
    gptps_thread *setter, *cl[2], *op[2] = { NULL, NULL };
    gptps_status st;
    uint64_t fl;
    int parked, i;

    CHECK(gptps_open(NULL, &g_rw_e) == GPTPS_OK);
    if (!g_rw_e) return;
    CHECK(gptps_define_task_setting(g_rw_e, "quality", GPTPS_SETTING_INT, "7", NULL, 0) == GPTPS_OK);
    CHECK(gptps_define_resource(g_rw_e, "gpu", 4) == GPTPS_OK);
    reg(g_rw_e, "src", rw_body);
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.key = "app.hold"; d.type = GPTPS_SETTING_UINT; d.hot = 1;
    d.desc = "holds the registry's lock"; d.read = rw_rd; d.write = rw_wr;
    CHECK(gptps_register_setting(g_rw_e, &d) == GPTPS_OK);

    __atomic_store_n(&g_rw_hold, 1, __ATOMIC_SEQ_CST);
    setter = gptps_thread_start(rw_setter, NULL);
    CHECK(setter != NULL && rw_wait(&g_rw_holding, 1));
    cl[0] = gptps_thread_start(rw_clone, (void *)0);   /* one links "late", then waits for the lock; */
    cl[1] = gptps_thread_start(rw_clone, (void *)1);   /* the other finds the name taken */
    CHECK(cl[0] != NULL && cl[1] != NULL);
    parked = rw_wait(&g_rw_dup, 1);
    CHECK(parked);

    if (parked) {   /* "late" is mid-registration: no other thread that names it finds it */
        st = gptps_submit(g_rw_e, "late", NULL, 0, NULL);
        CHECK(st == GPTPS_E_NOTFOUND);                      /* was OK, and the item read no "quality" */
        if (st == GPTPS_OK) CHECK(rw_wait(&g_rw_read, 1));
        CHECK(gptps_task_exists(g_rw_e, "late") == 0);
        CHECK(gptps_task_flags(g_rw_e, "late", &fl) == GPTPS_E_NOTFOUND);
        CHECK(gptps_set_task_enabled(g_rw_e, "late", 0) == GPTPS_E_NOTFOUND);
        CHECK(gptps_set_task_priority(g_rw_e, "late", 5) == GPTPS_E_NOTFOUND);
        CHECK(gptps_set_task_resource_cost(g_rw_e, "late", "gpu", 1) == GPTPS_E_NOTFOUND);
        CHECK(gptps_clone_task(g_rw_e, "src", "late") == GPTPS_E_DUP);          /* but its name is taken */
        CHECK(gptps_task_count(g_rw_e) == 1);               /* "src" alone */
        CHECK(!find_info(g_rw_e, "late", &info));
        CHECK(rw_not_found(rw_clone_from, 0, &op[0]));      /* not a source yet */
        CHECK(rw_not_found(rw_unregister, 1, &op[1]));      /* was OK, after freeing it */
    }

    __atomic_store_n(&g_rw_hold, 0, __ATOMIC_SEQ_CST);
    if (setter) gptps_thread_join(setter);
    for (i = 0; i < 2; ++i) if (cl[i]) gptps_thread_join(cl[i]);
    for (i = 0; i < 2; ++i) if (op[i]) gptps_thread_join(op[i]);
    CHECK((g_rw_clone_st[0] == GPTPS_OK && g_rw_clone_st[1] == GPTPS_E_DUP) ||
          (g_rw_clone_st[0] == GPTPS_E_DUP && g_rw_clone_st[1] == GPTPS_OK));

    /* registered: there for every call, and it takes work with its settings in place */
    __atomic_store_n(&g_rw_read, 0, __ATOMIC_SEQ_CST);
    CHECK(gptps_task_exists(g_rw_e, "late") == 1);
    CHECK(gptps_task_flags(g_rw_e, "late", &fl) == GPTPS_OK);
    CHECK(gptps_task_count(g_rw_e) == 2 && find_info(g_rw_e, "late", &info));
    CHECK(gptps_submit(g_rw_e, "late", NULL, 0, NULL) == GPTPS_OK);
    CHECK(rw_wait(&g_rw_read, 1));
    CHECK(gptps_unregister_task(g_rw_e, "late", GPTPS_REMOVE_DRAIN) == GPTPS_OK);
    gptps_shutdown(g_rw_e);
}

/* The same window, raced rather than parked: one thread clones "src" into "x" while
 * another unregisters "x", over and over. An unregister that came in while the clone
 * was still registering freed "x" under it: without the fix, ASan reported the
 * heap-use-after-free in each of 10 runs of this file. Now it finds no "x" until the
 * clone is done. */
static int g_cr_stop, g_cr_clones, g_cr_unregs, g_cr_bad;
static void *cr_cloner(void *a)
{
    (void)a;
    while (!__atomic_load_n(&g_cr_stop, __ATOMIC_SEQ_CST)) {
        gptps_status st = gptps_clone_task(g_rw_e, "src", "x");
        if (st == GPTPS_OK) __atomic_add_fetch(&g_cr_clones, 1, __ATOMIC_SEQ_CST);
        else if (st != GPTPS_E_DUP) __atomic_add_fetch(&g_cr_bad, 1, __ATOMIC_SEQ_CST);
    }
    return NULL;
}
static void *cr_remover(void *a)
{
    (void)a;
    while (!__atomic_load_n(&g_cr_stop, __ATOMIC_SEQ_CST)) {
        gptps_status st = gptps_unregister_task(g_rw_e, "x", GPTPS_REMOVE_CANCEL);
        if (st == GPTPS_OK) __atomic_add_fetch(&g_cr_unregs, 1, __ATOMIC_SEQ_CST);
        else if (st != GPTPS_E_NOTFOUND) __atomic_add_fetch(&g_cr_bad, 1, __ATOMIC_SEQ_CST);
    }
    return NULL;
}
static void test_clone_unregister_race(void)
{
    gptps_thread *a, *b;
    uint64_t t0;
    CHECK(gptps_open(NULL, &g_rw_e) == GPTPS_OK);
    if (!g_rw_e) return;
    CHECK(gptps_define_task_setting(g_rw_e, "quality", GPTPS_SETTING_INT, "7", NULL, 0) == GPTPS_OK);
    reg(g_rw_e, "src", rw_body);
    a = gptps_thread_start(cr_cloner, NULL);
    b = gptps_thread_start(cr_remover, NULL);
    CHECK(a != NULL && b != NULL);
    for (t0 = gptps_now_ms(NULL); gptps_now_ms(NULL) - t0 < 1000 && __atomic_load_n(&g_cr_unregs, __ATOMIC_SEQ_CST) < 5000; ) { }
    __atomic_store_n(&g_cr_stop, 1, __ATOMIC_SEQ_CST);
    if (a) gptps_thread_join(a);
    if (b) gptps_thread_join(b);
    printf("  clone/unregister race: %d clones, %d unregisters\n", g_cr_clones, g_cr_unregs);
    CHECK(g_cr_bad == 0);
    CHECK(g_cr_clones > 0 && g_cr_unregs > 0);            /* each happened: the race was run */
    CHECK(g_cr_clones - g_cr_unregs == gptps_task_exists(g_rw_e, "x"));
    gptps_shutdown(g_rw_e);
}

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

    /* ---- a task name longer than GPTPS_TASK_NAME_MAX is refused AT REGISTRATION ----
     *
     * Regression: an over-long name used to register fine and then be permanently
     * unregisterable - gptps_unregister_task bounds the name against its own
     * "tasks.<name>." buffer and refuses anything longer, so the type could never
     * be removed again. Worse, the six per-task settings all truncate into the
     * SAME key, so five of them were silently lost to E_DUP. The bound belongs at
     * registration, which is the one place it can still be reported to the caller.
     * At exactly the limit everything must still work, including the settings keys
     * that motivated the bound - hence the timeout_seconds lookup below. */
    {
        gptps *en = NULL;
        gptps_task_def d;
        char toolong[201], atmax[GPTPS_TASK_NAME_MAX + 1], key[GPTPS_TASK_NAME_MAX + 32];

        memset(toolong, 'a', sizeof toolong - 1); toolong[sizeof toolong - 1] = '\0';
        memset(atmax,   'b', sizeof atmax   - 1); atmax[sizeof atmax   - 1] = '\0';
        CHECK(strlen(atmax) == GPTPS_TASK_NAME_MAX);

        CHECK(gptps_open(NULL, &en) == GPTPS_OK);
        if (en) {
            memset(&d, 0, sizeof d);
            d.struct_size = sizeof d; d.run = task_fast; d.exec = GPTPS_EXEC_INPROC;
            d.default_cost.struct_size = sizeof d.default_cost;
            d.default_policy.struct_size = sizeof d.default_policy;

            d.name = toolong;
            CHECK(gptps_register_task(en, &d) == GPTPS_E_INVAL);
            CHECK(gptps_task_count(en) == 0);          /* refused, not half-registered */
            CHECK(gptps_task_exists(en, toolong) == 0);

            d.name = atmax;                            /* exactly at the limit: accepted */
            CHECK(gptps_register_task(en, &d) == GPTPS_OK);
            CHECK(gptps_task_exists(en, atmax) == 1);
            snprintf(key, sizeof key, "tasks.%s.timeout_seconds", atmax);
            CHECK(has_setting(en, key));               /* the key did not truncate away */
            /* and it is still removable - the half of the bug that had no workaround */
            CHECK(gptps_unregister_task(en, atmax, GPTPS_REMOVE_REJECT_IF_BUSY) == GPTPS_OK);
            CHECK(gptps_task_count(en) == 0);
            gptps_shutdown(en);
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

    /* ---- removal takes a type's own settings, not a sibling's ----
     * A type named "x.y" keeps its keys under "tasks.x.", the same prefix as type
     * "x"'s, and removing "x" used to delete every key with that prefix: the live
     * sibling was left with no tunable settings at all. Removal is now by owner. */
    {
        gptps *es = NULL;
        uint64_t fl = 99;
        CHECK(gptps_open(NULL, &es) == GPTPS_OK);
        if (es) {
            reg(es, "x", task_fast);
            reg(es, "x.y", task_fast);
            CHECK(gptps_define_task_setting(es, "lim", GPTPS_SETTING_UINT, "5", NULL, 0) == GPTPS_OK);
            CHECK(host_setting(es, "tasks.x.hostk") == GPTPS_OK);     /* a host's own keys */
            CHECK(host_setting(es, "tasks.x.y.hostk") == GPTPS_OK);
            CHECK(has_setting(es, "tasks.x.max_retries") && has_setting(es, "tasks.x.lim"));
            CHECK(has_setting(es, "tasks.x.y.max_retries") && has_setting(es, "tasks.x.y.lim"));
            CHECK(gptps_unregister_task(es, "x", GPTPS_REMOVE_REJECT_IF_BUSY) == GPTPS_OK);
            CHECK(!has_setting(es, "tasks.x.max_retries") && !has_setting(es, "tasks.x.lim"));
            CHECK(!has_setting(es, "tasks.x.hostk"));           /* still removed with "x" */
            CHECK(has_setting(es, "tasks.x.y.max_retries"));   /* was deleted with "x" */
            CHECK(has_setting(es, "tasks.x.y.lim"));
            CHECK(has_setting(es, "tasks.x.y.hostk"));         /* the sibling's host key too */
            reg(es, "x", task_fast);                            /* a successor gets its own */
            CHECK(has_setting(es, "tasks.x.max_retries") && has_setting(es, "tasks.x.lim"));

            /* gptps_task_flags: by name, copied under the lock */
            CHECK(gptps_task_flags(es, "x.y", &fl) == GPTPS_OK && fl == 0);
            CHECK(gptps_task_flags(es, "nope", &fl) == GPTPS_E_NOTFOUND);
            CHECK(gptps_task_flags(es, NULL, &fl) == GPTPS_E_INVAL);
            CHECK(gptps_task_flags(es, "x.y", NULL) == GPTPS_E_INVAL);
            CHECK(gptps_unregister_task(es, "x.y", GPTPS_REMOVE_REJECT_IF_BUSY) == GPTPS_OK);
            CHECK(gptps_task_flags(es, "x.y", &fl) == GPTPS_E_NOTFOUND);
            gptps_shutdown(es);
        }
    }

    test_registration_window();
    test_clone_unregister_race();

    if (fails) { printf("%d taskmgmt check(s) FAILED\n", fails); return 1; }
    printf("all taskmgmt checks passed\n");
    return 0;
}
