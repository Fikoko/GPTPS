/*
 * demo_freestanding.c - proof that the GPTPS C99 core runs "on any hardware".
 *
 * Built with -ffreestanding -fno-builtin against the stub HAL (hal_stub.c) and
 * linked with NO -lpthread / -ldl: the engine runs a task in MANUAL mode
 * (gptps_step, no worker threads), with a static memory pool (no libc heap) and
 * a silent diagnostic sink (no stdio output). Exit code 0 means the engine
 * registered, submitted, ran, and delivered a correct result through nothing but
 * the HAL seam. This is the concrete form of the "embeddable / bare-metal" claim.
 */
#include "gptps.h"
#include <string.h>   /* memset/memcpy: pure libc, available even under -ffreestanding on a hosted toolchain */

/* --- static memory pool: prove the core needs no libc heap ---------------- */
static union { unsigned char b[256 * 1024]; long double align_; } g_pool;
static size_t g_used = 0;

static void *pool_malloc(size_t n, void *u)
{
    unsigned char *p; size_t need;
    (void)u;
    need = ((n + 15u) & ~(size_t)15u) + 16u;          /* 16-byte size header + padded body */
    if (g_used + need > sizeof g_pool.b) return 0;     /* pool exhausted */
    p = &g_pool.b[g_used]; g_used += need;
    memcpy(p, &n, sizeof n);                            /* stash the request size for realloc */
    return p + 16;
}
static void *pool_realloc(void *ptr, size_t n, void *u)
{
    size_t old; void *np;
    if (!ptr) return pool_malloc(n, u);
    memcpy(&old, (unsigned char *)ptr - 16, sizeof old);
    np = pool_malloc(n, u);
    if (np) memcpy(np, ptr, old < n ? old : n);
    return np;
}
static void pool_free(void *ptr, void *u) { (void)ptr; (void)u; }  /* bump arena: freed at process exit */

/* --- a task and a silent diagnostic sink ---------------------------------- */
static unsigned long g_result;
static int           g_got;

static void on_event(const gptps_event *ev, void *u)
{
    (void)u;
    if (ev->kind == GPTPS_EV_FINISHED && ev->result && ev->result_len == sizeof(unsigned long)) {
        memcpy(&g_result, ev->result, sizeof g_result);
        g_got = 1;
    }
}
static void silent_log(gptps_log_level lvl, const char *msg, void *u) { (void)lvl; (void)msg; (void)u; }

static gptps_status sum(gptps_ctx *ctx, void *u)
{
    size_t n, i; const unsigned char *p = (const unsigned char *)gptps_payload(ctx, &n);
    unsigned long s = 0; (void)u;
    for (i = 0; i < n; ++i) s += p[i];
    return gptps_result_set(ctx, &s, sizeof s);
}

int main(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    gptps_allocator al;
    gptps_task_def d;
    gptps_handle h;
    size_t ran;
    int guard;
    static const char payload[5] = "hello";       /* 104+101+108+108+111 = 532 */
    unsigned long expected = 0;
    int i;
    for (i = 0; i < 5; ++i) expected += (unsigned char)payload[i];

    /* no libc heap, no stdio diagnostics */
    memset(&al, 0, sizeof al); al.struct_size = sizeof al;
    al.malloc_fn = pool_malloc; al.realloc_fn = pool_realloc; al.free_fn = pool_free;
    if (gptps_set_allocator(&al) != GPTPS_OK) return 2;
    gptps_set_log_sink(silent_log, 0);

    /* MANUAL mode: no worker threads; the caller drives the engine */
    memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.mode = GPTPS_RUN_MANUAL;
    if (gptps_open_ex(&cfg, &e) != GPTPS_OK || !e) return 3;
    gptps_set_event_cb(e, on_event, 0);

    memset(&d, 0, sizeof d); d.struct_size = sizeof d;
    d.name = "sum"; d.run = sum; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    if (gptps_register_task(e, &d) != GPTPS_OK) return 4;

    if (gptps_submit(e, "sum", payload, 5, &h) != GPTPS_OK) return 5;

    /* pump the engine to completion on this thread (no workers exist) */
    guard = 0;
    do { if (gptps_step(e, &ran) != GPTPS_OK) return 6; } while ((ran || !g_got) && ++guard < 1000);

    gptps_shutdown(e);

    if (!g_got || g_result != expected) return 7;
    return 0;   /* the core ran a task through only the HAL seam: no threads, dlopen, fork, or heap */
}
