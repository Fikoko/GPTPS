/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * embedded.c - GPTPS on a single thread with a fixed memory pool.
 *
 * The "bare-metal shape": NO worker/dispatcher threads (GPTPS_RUN_MANUAL, driven
 * by gptps_step) and a static-arena allocator (gptps_set_allocator routes every
 * CORE allocation into the arena instead of the libc heap). Built as a hosted
 * program so CI can run it, it still links the stock POSIX HAL, whose own
 * primitives (the per-item cancel flag, the mutex) use libc malloc - so this
 * particular binary is not literally libc-heap-free. A real MCU/RTOS port swaps
 * in a HAL backend (hal_<target>.c); for a genuinely no-libc-heap, no-pthread/dl
 * build see the freestanding/ reference (stub HAL + static pool, -ffreestanding).
 *
 * The arena allocator below is a compact first-fit free list with coalescing -
 * illustrative, not optimized.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ----- a tiny static-pool allocator (stands in for your MCU's RAM) ---------- */
#define ARENA_SIZE (256u * 1024u)
#define ALIGN      16u
#define HDR        ((sizeof(struct blk) + ALIGN - 1u) & ~(ALIGN - 1u))

static unsigned char arena[ARENA_SIZE + ALIGN];
struct blk { size_t size; int free; };   /* payload of `size` bytes follows; blocks are contiguous */

static unsigned char *pool;     /* aligned base */
static size_t         pool_len;
static int            inited;

static void pool_init(void)
{
    uintptr_t off = (uintptr_t)arena % ALIGN;
    struct blk *b;
    pool = arena + (off ? ALIGN - off : 0);
    pool_len = ARENA_SIZE;
    b = (struct blk *)pool;
    b->size = pool_len - HDR;
    b->free = 1;
    inited = 1;
}

static struct blk *next_blk(struct blk *b)
{
    unsigned char *n = (unsigned char *)b + HDR + b->size;
    return (n < pool + pool_len) ? (struct blk *)n : NULL;
}

static void coalesce(void)
{
    struct blk *b = (struct blk *)pool, *n;
    while (b) {
        if (b->free && (n = next_blk(b)) != NULL && n->free) { b->size += HDR + n->size; continue; }
        b = next_blk(b);
    }
}

static void *pool_malloc(size_t want, void *ud)
{
    struct blk *b;
    (void)ud;
    if (!inited) pool_init();
    want = (want + ALIGN - 1u) & ~(ALIGN - 1u);
    if (want == 0) want = ALIGN;
    for (b = (struct blk *)pool; b; b = next_blk(b)) {
        if (!b->free || b->size < want) continue;
        if (b->size >= want + HDR + ALIGN) {            /* split off the remainder */
            struct blk *rest = (struct blk *)((unsigned char *)b + HDR + want);
            rest->size = b->size - want - HDR;
            rest->free = 1;
            b->size = want;
        }
        b->free = 0;
        return (unsigned char *)b + HDR;
    }
    return NULL;   /* arena exhausted */
}

static void pool_free(void *p, void *ud)
{
    struct blk *b;
    (void)ud;
    if (!p) return;
    b = (struct blk *)((unsigned char *)p - HDR);
    b->free = 1;
    coalesce();
}

static void *pool_realloc(void *p, size_t want, void *ud)
{
    struct blk *b;
    void *q;
    if (!p) return pool_malloc(want, ud);
    b = (struct blk *)((unsigned char *)p - HDR);
    if (b->size >= want) return p;
    q = pool_malloc(want, ud);
    if (q) { memcpy(q, p, b->size); pool_free(p, ud); }
    return q;
}

/* ----- a trivial task: uppercase its payload --------------------------------- */
static gptps_status upper(gptps_ctx *ctx, void *ud)
{
    size_t n, i; const unsigned char *in; unsigned char buf[64];
    (void)ud;
    in = (const unsigned char *)gptps_payload(ctx, &n);
    if (n > sizeof buf) n = sizeof buf;
    for (i = 0; i < n; ++i) buf[i] = (in[i] >= 'a' && in[i] <= 'z') ? (unsigned char)(in[i] - 32) : in[i];
    return gptps_result_set(ctx, buf, n);
}

static int g_done;
static void on_event(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_FINISHED) {
        ++g_done;
        printf("  finished #%llu: %.*s\n", (unsigned long long)ev->handle,
               (int)ev->result_len, ev->result ? (const char *)ev->result : "");
    }
}

int main(void)
{
    gptps_allocator a;
    gptps_config cfg;
    gptps *e = NULL;
    gptps_handle h;
    size_t ran, total = 0;
    const char *words[] = { "embedded", "no", "threads", "no", "libc", "heap" };
    int i;

    /* 1) route ALL core allocation into the static arena (before opening) */
    memset(&a, 0, sizeof a);
    a.struct_size = sizeof a;
    a.malloc_fn = pool_malloc; a.realloc_fn = pool_realloc; a.free_fn = pool_free;
    if (gptps_set_allocator(&a) != GPTPS_OK) { printf("set_allocator failed\n"); return 1; }

    /* 2) open in MANUAL mode: no dispatcher/worker threads are created */
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 4;     /* batch size per step */
    cfg.limits.max_memory_bytes = ARENA_SIZE;
    cfg.mode = GPTPS_RUN_MANUAL;
    if (gptps_open_ex(&cfg, &e) != GPTPS_OK) { printf("open failed\n"); return 1; }

    gptps_set_event_cb(e, on_event, NULL);
    { gptps_task_def d; memset(&d, 0, sizeof d);
      d.struct_size = sizeof d; d.name = "upper"; d.run = upper; d.exec = GPTPS_EXEC_INPROC;
      d.default_cost.struct_size = sizeof d.default_cost;
      d.default_policy.struct_size = sizeof d.default_policy;
      gptps_register_task(e, &d); }

    for (i = 0; i < (int)(sizeof words / sizeof words[0]); ++i)
        gptps_submit(e, "upper", words[i], strlen(words[i]), &h);

    /* 3) pump on THIS thread until the queue drains */
    printf("pumping (single thread, %u-byte arena):\n", ARENA_SIZE);
    do { gptps_step(e, &ran); total += ran; } while (ran);

    gptps_shutdown(e);   /* MANUAL: nothing to join */
    printf("ran %lu tasks, %d finished, no threads, no libc heap.\n",
           (unsigned long)total, g_done);
    return (total == 6 && g_done == 6) ? 0 : 1;
}
