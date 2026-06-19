/*
 * test_scheduler.c - priority ordering + skip-to-fit backfill + reservation.
 *
 *  1) Priority: with one worker, queued tasks START in priority order (not FIFO).
 *  2) Skip-to-fit: a budget-blocked task at the head does NOT block smaller work
 *     behind it - the smaller task backfills while the big one waits.
 *  3) Reservation: with [scheduler] reserve_after_skips = 0, backfill is disabled,
 *     so the smaller task is held (reserved) until the blocked task can run.
 *
 * Tasks are gated by a release flag so the queue state is deterministic: a hog
 * occupies budget/worker while we line up the work that must (or must not) pass it.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static void set(int *p, int v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }

static int g_release;          /* gate: hog/block spin until this is set */
static int g_block_running;    /* set by STARTED for "block" / "hog" */
static int g_small_started;    /* set by STARTED for "small" */

#define MAXORD 16
static int  g_order_n;
static char g_order[MAXORD][8]; /* event-time copies of STARTED task names */

static void reset(void)
{
    set(&g_release, 0); set(&g_block_running, 0); set(&g_small_started, 0);
    set(&g_order_n, 0);
    memset(g_order, 0, sizeof g_order);
}

static void on_ev(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind != GPTPS_EV_STARTED) return;
    {
        int idx = inc(&g_order_n) - 1;
        if (idx >= 0 && idx < MAXORD) {
            strncpy(g_order[idx], ev->task_name, 7);
            g_order[idx][7] = 0;
        }
        if (strcmp(ev->task_name, "small") == 0)               set(&g_small_started, 1);
        if (strcmp(ev->task_name, "block") == 0 ||
            strcmp(ev->task_name, "hog") == 0)                 set(&g_block_running, 1);
    }
}

/* gated blocker: holds its worker (and its budget) until released */
static gptps_status task_block(gptps_ctx *ctx, void *ud)
{
    uint64_t start = gptps_now_ms(ctx);
    (void)ud;
    while (!get(&g_release) && !gptps_is_cancelled(ctx))
        if (gptps_now_ms(ctx) - start > 5000) break;   /* safety: never hang the suite */
    return GPTPS_OK;
}
static gptps_status task_quick(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_OK; }

static void def_init(gptps_task_def *d, const char *name, gptps_run_fn run, uint64_t mem)
{
    memset(d, 0, sizeof *d);
    d->struct_size = sizeof *d; d->name = name; d->run = run; d->exec = GPTPS_EXEC_INPROC;
    d->default_cost.struct_size = sizeof d->default_cost; d->default_cost.mem_bytes = mem;
    d->default_policy.struct_size = sizeof d->default_policy;
}

/* busy spin helpers (tests run briefly; no platform sleep dependency) */
static int wait_until(int *flag, int want, uint64_t timeout_ms)
{
    uint64_t start = gptps_now_ms(NULL);
    while (get(flag) != want) if (gptps_now_ms(NULL) - start > timeout_ms) return 0;
    return 1;
}
static void busy_ms(uint64_t ms) { uint64_t s = gptps_now_ms(NULL); while (gptps_now_ms(NULL) - s < ms) {} }

static int order_index(const char *name)
{
    int i, n = get(&g_order_n);
    for (i = 0; i < n && i < MAXORD; ++i) if (strcmp(g_order[i], name) == 0) return i;
    return -1;
}

/* ---- 1) priority ordering (one worker) ---- */
static void test_priority(void)
{
    gptps_config cfg;
    gptps *e = NULL;
    gptps_task_def d;
    gptps_handle h;

    reset();
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1; cfg.limits.max_memory_bytes = 1u << 20;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (!e) return;
    gptps_set_event_cb(e, on_ev, NULL);

    def_init(&d, "block", task_block, 16);  gptps_register_task(e, &d);
    def_init(&d, "lo",    task_quick, 16);  gptps_register_task(e, &d);
    def_init(&d, "mid",   task_quick, 16);  gptps_register_task(e, &d);
    def_init(&d, "hi",    task_quick, 16);  gptps_register_task(e, &d);
    CHECK(gptps_set_task_priority(e, "lo",  1)  == GPTPS_OK);
    CHECK(gptps_set_task_priority(e, "mid", 5)  == GPTPS_OK);
    CHECK(gptps_set_task_priority(e, "hi",  10) == GPTPS_OK);
    CHECK(gptps_set_task_priority(e, "nope", 1) == GPTPS_E_NOTFOUND);

    /* occupy the single worker first, THEN queue the three in low->high order */
    gptps_submit(e, "block", NULL, 0, &h);
    CHECK(wait_until(&g_block_running, 1, 2000));
    gptps_submit(e, "lo",  NULL, 0, &h);
    gptps_submit(e, "mid", NULL, 0, &h);
    gptps_submit(e, "hi",  NULL, 0, &h);
    busy_ms(50);                 /* let all three settle into intake */
    set(&g_release, 1);
    gptps_shutdown(e);

    /* after the blocker: strictly priority order hi > mid > lo */
    CHECK(order_index("block") == 0);
    CHECK(order_index("hi") == 1);
    CHECK(order_index("mid") == 2);
    CHECK(order_index("lo") == 3);
}

/* ---- 2) skip-to-fit: small backfills past a budget-blocked big task ---- */
static void test_skip_to_fit(void)
{
    gptps_config cfg;
    gptps *e = NULL;
    gptps_task_def d;
    gptps_handle h;

    reset();
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 4; cfg.limits.max_memory_bytes = 100;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (!e) return;
    gptps_set_event_cb(e, on_ev, NULL);

    def_init(&d, "hog",   task_block, 50); gptps_register_task(e, &d); /* occupies 50/100 */
    def_init(&d, "big2",  task_quick, 80); gptps_register_task(e, &d); /* 80 > 50 free: blocked */
    def_init(&d, "small", task_quick, 20); gptps_register_task(e, &d); /* 20 <= 50 free: fits */

    gptps_submit(e, "hog", NULL, 0, &h);
    CHECK(wait_until(&g_block_running, 1, 2000));
    gptps_submit(e, "big2",  NULL, 0, &h);   /* head, doesn't fit */
    gptps_submit(e, "small", NULL, 0, &h);   /* behind it, fits */

    /* small must backfill while hog still holds the budget */
    CHECK(wait_until(&g_small_started, 1, 2000));
    set(&g_release, 1);
    gptps_shutdown(e);

    CHECK(order_index("small") >= 0 && order_index("big2") >= 0);
    CHECK(order_index("small") < order_index("big2")); /* small ran before the big head */
}

/* ---- 3) reservation: reserve_after_skips=0 disables backfill ---- */
#define SCHED_CFG "gptps_sched_test.toml"
static void test_reservation(void)
{
    gptps *e = NULL;
    gptps_task_def d;
    gptps_handle h;
    FILE *f;
    int small_during;

    reset();
    f = fopen(SCHED_CFG, "wb");
    CHECK(f != NULL);
    if (!f) return;
    fprintf(f,
        "[limits]\n"
        "max_concurrent_tasks = 4\n"
        "max_memory_bytes = 100\n"
        "[scheduler]\n"
        "reserve_after_skips = 0\n");
    fclose(f);

    CHECK(gptps_open(SCHED_CFG, &e) == GPTPS_OK);
    if (!e) { remove(SCHED_CFG); return; }
    gptps_set_event_cb(e, on_ev, NULL);

    def_init(&d, "hog",   task_block, 50); gptps_register_task(e, &d);
    def_init(&d, "big2",  task_quick, 80); gptps_register_task(e, &d);
    def_init(&d, "small", task_quick, 20); gptps_register_task(e, &d);

    gptps_submit(e, "hog", NULL, 0, &h);
    CHECK(wait_until(&g_block_running, 1, 2000));
    gptps_submit(e, "big2",  NULL, 0, &h);
    gptps_submit(e, "small", NULL, 0, &h);

    /* with backfill disabled, small must NOT start while hog holds the budget */
    busy_ms(300);
    small_during = get(&g_small_started);
    CHECK(small_during == 0);          /* reserved for big2; small held back */

    set(&g_release, 1);
    gptps_shutdown(e);
    CHECK(get(&g_small_started) == 1); /* but it does run once the budget frees */
    remove(SCHED_CFG);
}

int main(void)
{
    test_priority();
    test_skip_to_fit();
    test_reservation();
    if (fails) { printf("%d scheduler check(s) FAILED\n", fails); return 1; }
    printf("all scheduler checks passed\n");
    return 0;
}
