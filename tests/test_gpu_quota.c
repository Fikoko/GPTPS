/*
 * test_gpu_quota.c - GPU-unit admission quota add-on.
 *
 * Budget = 4 units; each "gpu" task declares gpu_units = 2 and blocks until
 * released. With concurrency set high (8 workers), the GPU quota - not the
 * worker pool - is the limiter: at most 2 tasks (2 x 2 = 4) may run at once,
 * the rest deferred until units free. We submit 4, confirm exactly 2 run
 * concurrently while blocked, release, and confirm all 4 complete with the
 * budget fully returned.
 */
#include "gptps.h"
#include "gpu_quota.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int dec(int *p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }

static int g_running, g_peak, g_done, g_release;

static void bump_peak(int cur)
{
    int p = get(&g_peak);
    while (cur > p && !__atomic_compare_exchange_n(&g_peak, &p, cur, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        ; /* p reloaded by CAS on failure */
}

static gptps_status task_gpu(gptps_ctx *ctx, void *ud)
{
    uint64_t start = gptps_now_ms(ctx);
    int cur = inc(&g_running);
    (void)ud;
    bump_peak(cur);
    while (!get(&g_release) && !gptps_is_cancelled(ctx))
        if (gptps_now_ms(ctx) - start > 5000) break;   /* safety */
    dec(&g_running);
    inc(&g_done);
    return GPTPS_OK;
}

static int wait_until(int *flag, int want, uint64_t timeout_ms)
{
    uint64_t start = gptps_now_ms(NULL);
    while (get(flag) != want) if (gptps_now_ms(NULL) - start > timeout_ms) return 0;
    return 1;
}
static void busy_ms(uint64_t ms) { uint64_t s = gptps_now_ms(NULL); while (gptps_now_ms(NULL) - s < ms) {} }

int main(void)
{
    gptps_config cfg;
    gptps *e = NULL;
    gptps_gpu_quota *q;
    gptps_task_def d;
    gptps_handle h;
    int i;

    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 8;          /* workers are NOT the limiter */
    cfg.limits.max_memory_bytes = 64u << 20;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (!e) return 1;

    q = gptps_gpu_quota_install(e, 4, 20);        /* 4 units total, 2 per task => 2 concurrent */
    CHECK(q != NULL);
    CHECK(gptps_gpu_quota_total(q) == 4);

    /* the budget is exposed + retunable through the unified settings registry */
    {
        char b[GPTPS_SETTINGS_VALUE_MAX];
        CHECK(gptps_settings_get(e, "gpu_quota.total_units", b, sizeof b) == GPTPS_OK && strcmp(b, "4") == 0);
        CHECK(gptps_settings_set(e, "gpu_quota.total_units", "9") == GPTPS_OK);
        CHECK(gptps_gpu_quota_total(q) == 9);     /* live retune via the registry */
        CHECK(gptps_settings_set(e, "gpu_quota.total_units", "4") == GPTPS_OK); /* back for the test below */
    }

    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "gpu"; d.run = task_gpu; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_cost.mem_bytes = 4096; d.default_cost.gpu_units = 2;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);

    for (i = 0; i < 4; ++i) CHECK(gptps_submit(e, "gpu", NULL, 0, &h) == GPTPS_OK);

    /* exactly 2 should run; the other 2 are deferred by the quota */
    CHECK(wait_until(&g_running, 2, 2000));
    busy_ms(200);                                  /* give a 3rd a chance to (wrongly) start */
    CHECK(get(&g_running) == 2);
    CHECK(get(&g_peak) == 2);
    CHECK(gptps_gpu_quota_in_use(q) == 4);         /* both reservations held */

    __atomic_store_n(&g_release, 1, __ATOMIC_SEQ_CST);
    gptps_shutdown(e);                             /* drains the rest */

    CHECK(get(&g_done) == 4);                      /* all four ran */
    CHECK(get(&g_peak) == 2);                       /* never more than 2 at once */
    CHECK(gptps_gpu_quota_in_use(q) == 0);         /* budget fully returned */

    gptps_gpu_quota_close(q);                        /* after shutdown */
    if (fails) { printf("%d gpu-quota check(s) FAILED\n", fails); return 1; }
    printf("all gpu-quota checks passed\n");
    return 0;
}
