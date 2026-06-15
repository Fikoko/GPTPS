/*
 * test_result.c - result delivery: the FINISHED event carries the task's
 * result bytes, for both the in-process and out-of-process executors.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static unsigned long g_result;
static int g_got;

static void on_ev(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_FINISHED && ev->result && ev->result_len == sizeof(unsigned long)) {
        memcpy(&g_result, ev->result, sizeof g_result);
        __atomic_store_n(&g_got, 1, __ATOMIC_SEQ_CST);
    }
}

/* sum the payload bytes and return the sum as the result */
static gptps_status sum_task(gptps_ctx *ctx, void *ud)
{
    size_t n, i;
    const unsigned char *p = (const unsigned char *)gptps_payload(ctx, &n);
    unsigned long sum = 0;
    (void)ud;
    for (i = 0; i < n; ++i) sum += p[i];
    return gptps_result_set(ctx, &sum, sizeof sum);
}

static void def_init(gptps_task_def *d, const char *name, gptps_exec_kind exec)
{
    memset(d, 0, sizeof *d);
    d->struct_size = sizeof *d; d->name = name; d->run = sum_task; d->exec = exec;
    d->default_cost.struct_size = sizeof d->default_cost; d->default_cost.mem_bytes = 1024 * 1024;
    d->default_policy.struct_size = sizeof d->default_policy;
}

static int run_one(gptps_exec_kind exec, const char *payload, unsigned long expect)
{
    gptps *e = NULL;
    gptps_task_def d;
    gptps_handle h;
    g_result = 0; __atomic_store_n(&g_got, 0, __ATOMIC_SEQ_CST);
    CHECK(gptps_open(NULL, &e) == GPTPS_OK);
    if (!e) return 0;
    gptps_set_event_cb(e, on_ev, NULL);
    def_init(&d, "sum", exec);
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(gptps_submit(e, "sum", payload, strlen(payload), &h) == GPTPS_OK);
    gptps_shutdown(e);
    CHECK(__atomic_load_n(&g_got, __ATOMIC_SEQ_CST) == 1);
    CHECK(g_result == expect);
    return 1;
}

int main(void)
{
    /* "ABC" -> 65+66+67 = 198 ; "AB" -> 65+66 = 131 */
    run_one(GPTPS_EXEC_INPROC, "ABC", 198);   /* in-process result delivery */
    run_one(GPTPS_EXEC_OOP,    "AB",  131);   /* result marshalled back from the child */

    if (fails) { printf("%d result check(s) FAILED\n", fails); return 1; }
    printf("all result-delivery checks passed\n");
    return 0;
}
