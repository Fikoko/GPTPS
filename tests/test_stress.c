/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_stress.c - concurrent-producer stress on the submit path. Many threads
 * hammer gptps_submit at once with real payloads; every item must complete with
 * its bytes intact. This exercises the shortened critical section (the payload copy
 * + item allocation now happen OFF the engine lock) under contention - the case
 * ThreadSanitizer is meant to catch. POSIX only (spawns raw pthreads).
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#define NPROD   8
#define PER     400
#define PAYLEN  1024
#define TOTAL   (NPROD * PER)

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int g_done, g_good;
static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }

/* verify the payload arrived intact (0xAB * PAYLEN), then count it */
static gptps_status task_check(gptps_ctx *c, void *u)
{
    size_t len; const unsigned char *p = (const unsigned char *)gptps_payload(c, &len);
    int ok = (len == PAYLEN), i;
    (void)u;
    for (i = 0; ok && i < (int)len; ++i) if (p[i] != 0xAB) ok = 0;
    if (ok) inc(&g_good);
    inc(&g_done);
    return GPTPS_OK;
}

static void obs(const gptps_event *ev, void *ud) { (void)ev; (void)ud; }

static void *producer(void *arg)
{
    gptps *e = (gptps *)arg;
    unsigned char buf[PAYLEN];
    int i;
    memset(buf, 0xAB, sizeof buf);
    for (i = 0; i < PER; ++i) {
        while (gptps_submit(e, "q", buf, sizeof buf, NULL) == GPTPS_E_FULL) { }  /* respect backpressure */
    }
    return NULL;
}

int main(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    pthread_t th[NPROD];
    int i;
    uint64_t s;

    memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 4;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK); if (!e) return 1;
    gptps_register_observer(e, obs, NULL);
    {
        gptps_task_def d; memset(&d, 0, sizeof d);
        d.struct_size = sizeof d; d.name = "q"; d.run = task_check; d.exec = GPTPS_EXEC_INPROC;
        d.default_cost.struct_size = sizeof d.default_cost;
        d.default_policy.struct_size = sizeof d.default_policy;
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    }

    for (i = 0; i < NPROD; ++i) CHECK(pthread_create(&th[i], NULL, producer, e) == 0);
    for (i = 0; i < NPROD; ++i) pthread_join(th[i], NULL);

    s = gptps_now_ms(NULL);
    while (get(&g_done) < TOTAL && gptps_now_ms(NULL) - s < 15000) { }
    CHECK(get(&g_done) == TOTAL);   /* every concurrently-submitted item ran */
    CHECK(get(&g_good) == TOTAL);   /* every payload arrived intact (no corruption off-lock) */

    gptps_shutdown(e);

    if (fails) { printf("%d stress check(s) FAILED\n", fails); return 1; }
    printf("all stress checks passed (%d items, %d threads)\n", TOTAL, NPROD);
    return 0;
}
