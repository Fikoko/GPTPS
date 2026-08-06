/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_service.c - GPTPS_TASK_SERVICE: supervised long-running instances.
 *
 * Covers registration validation (exec/mode/timeout), that an instance starts and
 * runs, that it is RESTARTED when it exits abnormally (crash-restart supervision),
 * that gptps_cancel stops a single instance for good (no restart), that a running
 * service does not hang gptps_shutdown (the sweep proves it - a hang trips the
 * CTest timeout), and that gptps_unregister_task(DRAIN) is auto-upgraded to CANCEL.
 * Threaded, cooperative-cancel services (like test_cancel's task_block). Portable.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static void set0(int *p) { __atomic_store_n(p, 0, __ATOMIC_SEQ_CST); }

static int g_runs;      /* run() entries across all service instances (== (re)starts) */
static int g_crash_budget; /* how many more times svc should "crash" before settling */

/* A well-behaved service: loop until told to stop, then exit cleanly. */
static gptps_status svc_block(gptps_ctx *c, void *u)
{
    (void)u;
    inc(&g_runs);
    while (!gptps_is_cancelled(c)) { /* spin (cooperative, like test_cancel) */ }
    return GPTPS_OK;
}

/* A crashy service: return an error for the first few entries (each triggers a
 * restart), then settle into a normal blocking loop. */
static gptps_status svc_crashy(gptps_ctx *c, void *u)
{
    (void)u;
    inc(&g_runs);
    if (__atomic_sub_fetch(&g_crash_budget, 1, __ATOMIC_SEQ_CST) >= 0)
        return GPTPS_E_TASK;                 /* "crash" -> supervisor restarts us */
    while (!gptps_is_cancelled(c)) { }
    return GPTPS_OK;
}

/* A service that exits CLEANLY (returns GPTPS_OK, uncancelled) a few times: a live
 * service is supervised to stay up, so a clean exit that wasn't a stop request must
 * still restart it - g_runs keeps growing until it settles into the blocking loop. */
static gptps_status svc_ok_then_block(gptps_ctx *c, void *u)
{
    (void)u;
    inc(&g_runs);
    if (__atomic_sub_fetch(&g_crash_budget, 1, __ATOMIC_SEQ_CST) >= 0)
        return GPTPS_OK;                     /* clean, uncancelled exit -> still restarted */
    while (!gptps_is_cancelled(c)) { }
    return GPTPS_OK;
}

/* A service that returns GPTPS_OK immediately (a clean, uncancelled exit). */
static gptps_status svc_ok_immediate(gptps_ctx *c, void *u) { (void)c; (void)u; inc(&g_runs); return GPTPS_OK; }

/* A plain one-shot task, for the negative/registration tests. */
static gptps_status one_shot(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }

static gptps_status open_threaded(gptps **out, uint32_t conc)
{
    gptps_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = conc;
    return gptps_open_ex(&cfg, out);
}

/* register a service (INPROC, no timeout, backoff seconds as given), OR-ing any
 * extra GPTPS_TASK_* flags (e.g. GPTPS_TASK_RETIRE_ON_OK) onto GPTPS_TASK_SERVICE */
static gptps_status reg_service_ex(gptps *e, const char *name, gptps_run_fn fn,
                                   uint32_t backoff_s, uint64_t extra_flags)
{
    gptps_task_def d; memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = fn; d.exec = GPTPS_EXEC_INPROC;
    d.flags = GPTPS_TASK_SERVICE | extra_flags;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.retry_backoff_seconds = backoff_s;
    return gptps_register_task(e, &d);
}
static gptps_status reg_service(gptps *e, const char *name, gptps_run_fn fn, uint32_t backoff_s)
{ return reg_service_ex(e, name, fn, backoff_s, 0); }

/* poll a counter up to a bound, so the test never blocks forever on a bug */
static int wait_ge(int *p, int target, unsigned ms)
{
    uint64_t s = gptps_now_ms(NULL);
    while (get(p) < target && gptps_now_ms(NULL) - s < ms) { }
    return get(p) >= target;
}

/* ---- 1) registration validation -------------------------------------------- */
static void test_registration_rules(void)
{
    gptps *e = NULL;
    gptps_task_def d;
    static const char *const argv[] = { "true", NULL };

    CHECK(open_threaded(&e, 2) == GPTPS_OK);
    if (!e) return;

    /* service + PROGRAM executor -> rejected (v1: INPROC only) */
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "svc_prog"; d.exec = GPTPS_EXEC_PROGRAM;
    d.argv = argv; d.flags = GPTPS_TASK_SERVICE;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_E_INVAL);

    /* service + wall-clock timeout -> rejected */
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "svc_to"; d.run = svc_block; d.exec = GPTPS_EXEC_INPROC;
    d.flags = GPTPS_TASK_SERVICE;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.timeout_seconds = 5;
    CHECK(gptps_register_task(e, &d) == GPTPS_E_INVAL);

    /* a valid service registers, and its policy is normalized to restart-on-exit */
    CHECK(reg_service(e, "svc_ok", svc_block, 0) == GPTPS_OK);
    {
        gptps_task_info info; size_t i, n = gptps_task_count(e); int seen = 0;
        for (i = 0; i < n; ++i) {
            memset(&info, 0, sizeof info); info.struct_size = sizeof info;
            if (gptps_task_get_info(e, i, &info) == GPTPS_OK && strcmp(info.name, "svc_ok") == 0) {
                seen = 1;
                CHECK(info.default_policy.on_failure == GPTPS_ON_FAILURE_REQUEUE);
                CHECK(info.default_policy.timeout_seconds == 0);
            }
        }
        CHECK(seen == 1);
    }

    /* append-safe ABI: a pre-v1.11 caller has no `flags` field and passes a smaller
     * struct_size. It must still register (and be read as non-service) - the old
     * `struct_size < sizeof` guard would have wrongly rejected it. */
    {
        gptps_task_def d2; memset(&d2, 0, sizeof d2);
        d2.struct_size = offsetof(gptps_task_def, flags);    /* as if `flags` didn't exist */
        d2.name = "legacy"; d2.run = one_shot; d2.exec = GPTPS_EXEC_INPROC;
        d2.default_cost.struct_size = sizeof d2.default_cost;
        d2.default_policy.struct_size = sizeof d2.default_policy;
        CHECK(gptps_register_task(e, &d2) == GPTPS_OK);
        CHECK(gptps_submit(e, "legacy", NULL, 0, NULL) == GPTPS_OK);   /* and it runs */
    }
    gptps_shutdown(e);

    /* service + MANUAL mode -> rejected (an infinite loop can't be run to completion
     * by the gptps_step pump) */
    {
        gptps_config cfg; gptps *m = NULL;
        memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg;
        cfg.limits.struct_size = sizeof cfg.limits; cfg.mode = GPTPS_RUN_MANUAL;
        CHECK(gptps_open_ex(&cfg, &m) == GPTPS_OK);
        if (m) {
            CHECK(reg_service(m, "svc_manual", svc_block, 0) == GPTPS_E_INVAL);
            gptps_shutdown(m);
        }
    }
}

/* ---- 2) starts, restarts on crash, cancel stops for good -------------------- */
static void test_restart_and_cancel(void)
{
    gptps *e = NULL;
    gptps_handle h = 0;
    int runs_after_cancel;

    set0(&g_runs);
    __atomic_store_n(&g_crash_budget, 3, __ATOMIC_SEQ_CST);  /* crash 3x, then block */

    CHECK(open_threaded(&e, 2) == GPTPS_OK);
    if (!e) return;
    CHECK(reg_service(e, "svc", svc_crashy, 0) == GPTPS_OK);

    /* start one instance; it should be (re)started >3 times, proving supervision */
    CHECK(gptps_submit(e, "svc", NULL, 0, &h) == GPTPS_OK);
    CHECK(wait_ge(&g_runs, 4, 3000));            /* 3 crashes + the settled run */

    /* the handle is stable across restarts: cancelling it stops the instance for good */
    CHECK(gptps_cancel(e, h) == GPTPS_OK);
    { uint64_t s = gptps_now_ms(NULL); while (gptps_now_ms(NULL) - s < 150) { } }  /* let it settle */
    runs_after_cancel = get(&g_runs);
    { uint64_t s = gptps_now_ms(NULL); while (gptps_now_ms(NULL) - s < 150) { } }
    CHECK(get(&g_runs) == runs_after_cancel);    /* no further restarts after cancel */

    CHECK(gptps_shutdown(e) == GPTPS_OK);        /* returns => nothing left spinning */
}

/* ---- 3) a running service must not hang shutdown (the sweep) ---------------- */
static void test_shutdown_stops_services(void)
{
    gptps *e = NULL;
    gptps_handle h1 = 0, h2 = 0, h3 = 0;

    set0(&g_runs);
    CHECK(open_threaded(&e, 4) == GPTPS_OK);
    if (!e) return;
    CHECK(reg_service(e, "svc", svc_block, 0) == GPTPS_OK);

    /* a small pool of never-cancelled instances */
    CHECK(gptps_submit(e, "svc", NULL, 0, &h1) == GPTPS_OK);
    CHECK(gptps_submit(e, "svc", NULL, 0, &h2) == GPTPS_OK);
    CHECK(gptps_submit(e, "svc", NULL, 0, &h3) == GPTPS_OK);
    CHECK(wait_ge(&g_runs, 3, 3000));            /* all three running */

    /* we never cancel them: a clean return here is the proof the sweep works */
    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

/* ---- 4) unregister(DRAIN) on a service is auto-upgraded to CANCEL ----------- */
static void test_unregister_drain_upgrades(void)
{
    gptps *e = NULL;
    gptps_handle h = 0;

    set0(&g_runs);
    CHECK(open_threaded(&e, 2) == GPTPS_OK);
    if (!e) return;
    CHECK(reg_service(e, "svc", svc_block, 0) == GPTPS_OK);
    CHECK(gptps_submit(e, "svc", NULL, 0, &h) == GPTPS_OK);
    CHECK(wait_ge(&g_runs, 1, 3000));

    /* DRAIN would block forever on a service; the engine upgrades it to CANCEL so
     * this returns. The name is then free to re-register. */
    CHECK(gptps_unregister_task(e, "svc", GPTPS_REMOVE_DRAIN) == GPTPS_OK);
    CHECK(gptps_task_exists(e, "svc") == 0);
    CHECK(reg_service(e, "svc", svc_block, 0) == GPTPS_OK);   /* re-registrable */

    /* a plain one-shot still coexists and works normally */
    {
        gptps_task_def d; memset(&d, 0, sizeof d);
        d.struct_size = sizeof d; d.name = "job"; d.run = one_shot; d.exec = GPTPS_EXEC_INPROC;
        d.default_cost.struct_size = sizeof d.default_cost;
        d.default_policy.struct_size = sizeof d.default_policy;
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        CHECK(gptps_submit(e, "job", NULL, 0, NULL) == GPTPS_OK);
    }
    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

/* ---- 5) a clean (GPTPS_OK, uncancelled) exit still restarts a live service ---- */
static void test_ok_exit_restarts(void)
{
    gptps *e = NULL;
    gptps_handle h = 0;

    set0(&g_runs);
    __atomic_store_n(&g_crash_budget, 2, __ATOMIC_SEQ_CST);  /* 2 clean exits, then block */

    CHECK(open_threaded(&e, 2) == GPTPS_OK);
    if (!e) return;
    CHECK(reg_service(e, "svc", svc_ok_then_block, 0) == GPTPS_OK);
    CHECK(gptps_submit(e, "svc", NULL, 0, &h) == GPTPS_OK);
    CHECK(wait_ge(&g_runs, 3, 3000));            /* restarted across the clean exits */
    CHECK(gptps_cancel(e, h) == GPTPS_OK);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

/* ---- 6) service + named-resource cost: reserve/release stays balanced across
 *         restarts (and, under ASan, no res_reserved leak on each restart) -------- */
static void test_service_with_resource(void)
{
    gptps *e = NULL;
    gptps_handle h = 0;
    uint64_t reserved = 999, budget = 0;

    set0(&g_runs);
    __atomic_store_n(&g_crash_budget, 3, __ATOMIC_SEQ_CST);  /* crash 3x (each reserves+releases), then block */

    CHECK(open_threaded(&e, 2) == GPTPS_OK);
    if (!e) return;
    CHECK(gptps_define_resource(e, "gpu", 4) == GPTPS_OK);
    CHECK(reg_service(e, "svc", svc_crashy, 0) == GPTPS_OK);
    CHECK(gptps_set_task_resource_cost(e, "svc", "gpu", 1) == GPTPS_OK);

    CHECK(gptps_submit(e, "svc", NULL, 0, &h) == GPTPS_OK);
    CHECK(wait_ge(&g_runs, 4, 3000));            /* survived 3 reserving/releasing restarts */

    /* the settled, blocking instance holds exactly its 1 unit */
    CHECK(gptps_resource_usage(e, "gpu", &reserved, &budget) == GPTPS_OK);
    CHECK(budget == 4);
    CHECK(reserved == 1);

    CHECK(gptps_cancel(e, h) == GPTPS_OK);
    { uint64_t s = gptps_now_ms(NULL); while (gptps_now_ms(NULL) - s < 200) { } }
    CHECK(gptps_resource_usage(e, "gpu", &reserved, NULL) == GPTPS_OK);
    CHECK(reserved == 0);                         /* released on terminal, balanced */

    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

/* ---- 7) GPTPS_TASK_RETIRE_ON_OK: clean OK exit retires; failure still restarts -- */
static void test_retire_on_ok(void)
{
    gptps *e = NULL;
    gptps_handle h = 0;

    set0(&g_runs);
    CHECK(open_threaded(&e, 2) == GPTPS_OK);
    if (!e) return;

    /* a clean GPTPS_OK return retires the instance (opts out of restart-always) */
    CHECK(reg_service_ex(e, "once", svc_ok_immediate, 0, GPTPS_TASK_RETIRE_ON_OK) == GPTPS_OK);
    CHECK(gptps_submit(e, "once", NULL, 0, &h) == GPTPS_OK);
    CHECK(wait_ge(&g_runs, 1, 3000));
    { uint64_t s = gptps_now_ms(NULL); while (gptps_now_ms(NULL) - s < 200) { } }  /* settle */
    CHECK(get(&g_runs) == 1);                        /* ran exactly once - NOT restarted */
    CHECK(gptps_cancel(e, h) == GPTPS_E_NOTFOUND);   /* the instance is gone (retired) */

    /* but a RETIRE_ON_OK service that FAILS is still restarted (restart-on-failure) */
    set0(&g_runs);
    __atomic_store_n(&g_crash_budget, 3, __ATOMIC_SEQ_CST);
    CHECK(reg_service_ex(e, "crashy", svc_crashy, 0, GPTPS_TASK_RETIRE_ON_OK) == GPTPS_OK);
    CHECK(gptps_submit(e, "crashy", NULL, 0, &h) == GPTPS_OK);
    CHECK(wait_ge(&g_runs, 4, 3000));                /* 3 crashes still restart */
    CHECK(gptps_cancel(e, h) == GPTPS_OK);

    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

int main(void)
{
    test_registration_rules();
    test_restart_and_cancel();
    test_shutdown_stops_services();
    test_unregister_drain_upgrades();
    test_ok_exit_restarts();
    test_service_with_resource();
    test_retire_on_ok();

    if (fails) { printf("%d service check(s) FAILED\n", fails); return 1; }
    printf("all service checks passed\n");
    return 0;
}
