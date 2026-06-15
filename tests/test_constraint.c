/*
 * test_constraint.c - constraint hooks (admit/deny/defer) + observers.
 * Proves the modular admission/observer seams are wired into the dispatcher.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int g_finished, g_dead, g_denied, g_defers, g_primary, g_obs;
static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static void reset(void) { g_finished=g_dead=g_denied=g_defers=g_primary=g_obs=0; }

static gptps_status noop(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_OK; }

static void cb(const gptps_event *ev, void *ud)
{
    (void)ud;
    inc(&g_primary);
    if (ev->kind == GPTPS_EV_FINISHED) inc(&g_finished);
    else if (ev->kind == GPTPS_EV_DEAD_LETTERED) { inc(&g_dead); if (ev->status == GPTPS_E_DENIED) inc(&g_denied); }
}
static void observer(const gptps_event *ev, void *ud) { (void)ev; (void)ud; inc(&g_obs); }

/* defer the first two admission attempts, then admit */
static gptps_admit_decision defer_then_admit(const char *t, const gptps_cost *c, uint32_t *ra, void *ud)
{
    (void)t; (void)c; (void)ud;
    if (inc(&g_defers) <= 2) { *ra = 1; return GPTPS_DEFER; }
    return GPTPS_ADMIT;
}
static gptps_admit_decision always_deny(const char *t, const gptps_cost *c, uint32_t *ra, void *ud)
{
    (void)t; (void)c; (void)ra; (void)ud; return GPTPS_DENY;
}

static void reg_noop(gptps *e)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "noop"; d.run = noop; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = 1024;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

int main(void)
{
    gptps *e;
    gptps_handle h;
    int i;

    /* 1) DEFER then ADMIT: task is held twice, then runs */
    reset();
    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    gptps_set_event_cb(e, cb, NULL);
    CHECK(gptps_register_constraint(e, defer_then_admit, NULL) == GPTPS_OK);
    reg_noop(e);
    CHECK(gptps_submit(e, "noop", NULL, 0, &h) == GPTPS_OK);
    gptps_shutdown(e);
    CHECK(get(&g_finished) == 1);
    CHECK(get(&g_defers) == 3);  /* 2 defers + 1 admit */

    /* 2) DENY -> dead-lettered with E_DENIED, never runs */
    reset();
    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    gptps_set_event_cb(e, cb, NULL);
    CHECK(gptps_register_constraint(e, always_deny, NULL) == GPTPS_OK);
    reg_noop(e);
    CHECK(gptps_submit(e, "noop", NULL, 0, &h) == GPTPS_OK);
    gptps_shutdown(e);
    CHECK(get(&g_dead) == 1);
    CHECK(get(&g_denied) == 1);
    CHECK(get(&g_finished) == 0);

    /* 3) observer receives the same events as the primary callback */
    reset();
    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    gptps_set_event_cb(e, cb, NULL);
    CHECK(gptps_register_observer(e, observer, NULL) == GPTPS_OK);
    reg_noop(e);
    for (i = 0; i < 3; ++i) CHECK(gptps_submit(e, "noop", NULL, 0, &h) == GPTPS_OK);
    gptps_shutdown(e);
    CHECK(get(&g_obs) == get(&g_primary)); /* observer saw everything the primary did */
    CHECK(get(&g_obs) > 0);

    if (fails) { printf("%d constraint check(s) FAILED\n", fails); return 1; }
    printf("all constraint/observer checks passed\n");
    return 0;
}
