/*
 * test_deadletter.c - dead-letter drain API.
 *   1) inspect : failed tasks are retained; gptps_dead_letter_count + drain
 *                report them with correct fields/payloads; drain empties the list.
 *   2) reprocess: the drain callback re-submits the work (lock released => no
 *                deadlock) and the re-submitted tasks run to completion.
 *   3) denied  : a constraint DENY is dead-lettered with GPTPS_E_DENIED.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }

static int c_dead, c_ok_finished;
static gptps *g_engine;

static void reset(void) { __atomic_store_n(&c_dead, 0, __ATOMIC_SEQ_CST);
                          __atomic_store_n(&c_ok_finished, 0, __ATOMIC_SEQ_CST); }

static void on_ev(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_DEAD_LETTERED) inc(&c_dead);
    if (ev->kind == GPTPS_EV_FINISHED && strcmp(ev->task_name, "ok") == 0) inc(&c_ok_finished);
}

static gptps_status task_fail(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_E_TASK; }
static gptps_status task_ok(gptps_ctx *ctx, void *ud)   { (void)ctx; (void)ud; return GPTPS_OK; }

static gptps_admit_decision deny_all(const gptps_constraint_input *in, uint32_t *ra, void *ud)
{ (void)in; (void)ra; (void)ud; return GPTPS_DENY; }

static void def_init(gptps_task_def *d, const char *name, gptps_run_fn run,
                     uint32_t retries, gptps_on_failure on_fail)
{
    memset(d, 0, sizeof *d);
    d->struct_size = sizeof *d; d->name = name; d->run = run; d->exec = GPTPS_EXEC_INPROC;
    d->default_cost.struct_size = sizeof d->default_cost; d->default_cost.mem_bytes = 1024;
    d->default_policy.struct_size = sizeof d->default_policy;
    d->default_policy.max_retries = retries;
    d->default_policy.on_failure = on_fail;
}

static int wait_count(int *flag, int want, uint64_t timeout_ms)
{
    uint64_t start = gptps_now_ms(NULL);
    while (get(flag) < want) if (gptps_now_ms(NULL) - start > timeout_ms) return 0;
    return 1;
}

/* ---- drain callbacks ---- */
#define N 5
static int   seen_bytes[256];
static int   inspect_n;
static int   inspect_bad;

static void inspect_cb(const gptps_dead_letter *dl, void *ud)
{
    (void)ud;
    ++inspect_n;
    if (dl->status != GPTPS_E_TASK)              inspect_bad = 1;
    if (strcmp(dl->task_name, "fail") != 0)      inspect_bad = 1;
    if (dl->attempts != 1)                       inspect_bad = 1;
    if (dl->payload_len != 1)                    inspect_bad = 1;
    else seen_bytes[*(const unsigned char *)dl->payload] += 1;
}

static void reprocess_cb(const gptps_dead_letter *dl, void *ud)
{
    gptps_handle h;
    (void)dl; (void)ud;
    /* re-submit equivalent work on a healthy task type (engine lock is released) */
    gptps_submit(g_engine, "ok", NULL, 0, &h);
}

static void test_inspect(void)
{
    gptps *e = NULL;
    gptps_task_def d;
    gptps_handle h;
    unsigned char i;
    size_t drained;

    reset(); memset(seen_bytes, 0, sizeof seen_bytes); inspect_n = inspect_bad = 0;
    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    if (!e) return;
    gptps_set_event_cb(e, on_ev, NULL);
    def_init(&d, "fail", task_fail, 0, GPTPS_ON_FAILURE_DEAD_LETTER);
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);

    for (i = 0; i < N; ++i) { unsigned char b = i; gptps_submit(e, "fail", &b, 1, &h); }
    CHECK(wait_count(&c_dead, N, 2000));
    CHECK(gptps_dead_letter_count(e) == (size_t)N);

    drained = gptps_dead_letter_drain(e, inspect_cb, NULL);
    CHECK(drained == (size_t)N);
    CHECK(inspect_n == N);
    CHECK(inspect_bad == 0);
    for (i = 0; i < N; ++i) CHECK(seen_bytes[i] == 1);   /* every payload accounted for */
    CHECK(gptps_dead_letter_count(e) == 0);              /* drained empties the list */
    CHECK(gptps_dead_letter_drain(e, NULL, NULL) == 0);  /* nothing left; NULL cb ok */

    gptps_shutdown(e);
}

static void test_reprocess(void)
{
    gptps *e = NULL;
    gptps_task_def d;
    gptps_handle h;
    int i;
    size_t drained;

    reset();
    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    if (!e) return;
    g_engine = e;
    gptps_set_event_cb(e, on_ev, NULL);
    def_init(&d, "fail", task_fail, 0, GPTPS_ON_FAILURE_DEAD_LETTER); gptps_register_task(e, &d);
    def_init(&d, "ok",   task_ok,   0, GPTPS_ON_FAILURE_DROP);        gptps_register_task(e, &d);

    for (i = 0; i < N; ++i) gptps_submit(e, "fail", NULL, 0, &h);
    CHECK(wait_count(&c_dead, N, 2000));

    drained = gptps_dead_letter_drain(e, reprocess_cb, NULL); /* re-submits N "ok" */
    CHECK(drained == (size_t)N);

    gptps_shutdown(e); /* drains the re-submitted work */
    CHECK(get(&c_ok_finished) == N);
}

static gptps_status g_denied_status;
static void denied_cb(const gptps_dead_letter *dl, void *ud) { (void)ud; g_denied_status = dl->status; }

static void test_denied(void)
{
    gptps *e = NULL;
    gptps_task_def d;
    gptps_handle h;
    size_t drained;

    reset();
    g_denied_status = GPTPS_OK;
    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    if (!e) return;
    gptps_set_event_cb(e, on_ev, NULL);
    CHECK(gptps_register_constraint(e, deny_all, NULL) == GPTPS_OK);
    def_init(&d, "denied", task_ok, 0, GPTPS_ON_FAILURE_DEAD_LETTER);
    gptps_register_task(e, &d);

    gptps_submit(e, "denied", NULL, 0, &h);
    CHECK(wait_count(&c_dead, 1, 2000));
    CHECK(gptps_dead_letter_count(e) == 1);

    drained = gptps_dead_letter_drain(e, denied_cb, NULL);
    CHECK(drained == 1);
    CHECK(g_denied_status == GPTPS_E_DENIED); /* admission DENY is retained as such */
    CHECK(gptps_dead_letter_count(e) == 0);
    gptps_shutdown(e);
}

int main(void)
{
    test_inspect();
    test_reprocess();
    test_denied();
    if (fails) { printf("%d dead-letter check(s) FAILED\n", fails); return 1; }
    printf("all dead-letter checks passed\n");
    return 0;
}
