/*
 * test_cancel.c - gptps_cancel(handle): cancel a queued item before it runs,
 * cancel an in-flight item (cooperative), and no-op on unknown / already-terminal
 * handles. Single worker makes the queueing deterministic. Headless / portable.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int started, fin_noop, fin_block, cancelled;
static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }

static void obs(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_STARTED) inc(&started);
    if (ev->kind == GPTPS_EV_FINISHED) {
        if (strcmp(ev->task_name, "noop")  == 0) inc(&fin_noop);
        if (strcmp(ev->task_name, "block") == 0) inc(&fin_block);
    }
    if (ev->kind == GPTPS_EV_FAILED && ev->status == GPTPS_E_CANCELLED) inc(&cancelled);
}

static gptps_status task_noop(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }
/* spins until cancelled; no timeout, so only gptps_cancel can stop it */
static gptps_status task_block(gptps_ctx *c, void *u) { (void)u; while (!gptps_is_cancelled(c)) { } return GPTPS_E_CANCELLED; }

static void reg(gptps *e, const char *name, gptps_run_fn fn)
{
    gptps_task_def d; memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = fn; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

int main(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    gptps_handle hb = 0, hn = 0;
    uint64_t s;

    memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;          /* strictly sequential => deterministic queueing */
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (!e) return 1;
    gptps_register_observer(e, obs, NULL);
    reg(e, "block", task_block);
    reg(e, "noop",  task_noop);

    /* the single worker is occupied by "block" */
    CHECK(gptps_submit(e, "block", NULL, 0, &hb) == GPTPS_OK);
    s = gptps_now_ms(NULL);
    while (get(&started) < 1 && gptps_now_ms(NULL) - s < 2000) { }
    CHECK(get(&started) == 1);

    /* "noop" queues behind the busy worker; cancel it before it can run */
    CHECK(gptps_submit(e, "noop", NULL, 0, &hn) == GPTPS_OK);
    CHECK(gptps_cancel(e, hn) == GPTPS_OK);            /* queued -> removed + terminal event */
    CHECK(gptps_cancel(e, hn) == GPTPS_E_NOTFOUND);    /* already gone */
    CHECK(gptps_cancel(e, 999999u) == GPTPS_E_NOTFOUND); /* unknown handle */

    /* cancel the in-flight "block": without this, shutdown would hang on the
     * spinning task, so a clean shutdown is the proof that in-flight cancel works */
    CHECK(gptps_cancel(e, hb) == GPTPS_OK);

    gptps_shutdown(e);

    CHECK(get(&fin_noop)  == 0);   /* the cancelled queued item never ran */
    CHECK(get(&fin_block) == 0);   /* block was cancelled, not finished-OK */
    CHECK(get(&cancelled) >= 1);   /* the queued noop emitted FAILED/E_CANCELLED */

    if (fails) { printf("%d cancel check(s) FAILED\n", fails); return 1; }
    printf("all cancel checks passed\n");
    return 0;
}
