/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_cb_cap.c - the callback records past GPTPS_CB_THREADS_MAX. Built against a
 * core compiled with GPTPS_CB_THREADS_MAX=4 (CMake), so eight live host threads
 * are past the cap. They submit one at a time, so no more than one is ever inside
 * a callback: every one must still be guarded, by taking over an idle record -
 * from another bucket too, relinked onto its own. The QUEUED observer calls
 * gptps_step on the submitting thread (MANUAL): GPTPS_E_BUSY when guarded.
 */
#define _POSIX_C_SOURCE 200809L   /* nanosleep under -std=c99 */
#include "gptps.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define NTHREADS 8
static gptps *E;
static pthread_mutex_t ser = PTHREAD_MUTEX_INITIALIZER;
static int n_busy, n_other;

/* A barrier from a mutex and a condvar: macOS has no pthread_barrier_t. */
static pthread_mutex_t bm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bc = PTHREAD_COND_INITIALIZER;
static int b_count, b_gen;
static void barrier_wait(void)
{
    int gen;
    pthread_mutex_lock(&bm);
    gen = b_gen;
    if (++b_count == NTHREADS) { b_count = 0; ++b_gen; pthread_cond_broadcast(&bc); }
    else while (gen == b_gen) pthread_cond_wait(&bc, &bm);
    pthread_mutex_unlock(&bm);
}

static gptps_status body(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }
static void obs(const gptps_event *ev, void *ud)
{
    size_t ran = 0;
    (void)ud;
    if (ev->kind != GPTPS_EV_QUEUED) return;
    __atomic_add_fetch(gptps_step(E, &ran) == GPTPS_E_BUSY ? &n_busy : &n_other, 1, __ATOMIC_RELAXED);
}
static void *host(void *a)
{
    (void)a;
    barrier_wait();                        /* all alive at once: eight distinct ids */
    pthread_mutex_lock(&ser);
    gptps_submit(E, "t", NULL, 0, NULL);
    pthread_mutex_unlock(&ser);
    barrier_wait();                        /* none exits (and frees its id) early */
    return NULL;
}
/* Two detached threads, one after the other, with nothing ordering the first's
 * end before the second's start (a sleep only paces them; neither flag is read
 * until both have run): the OS hands the second the first's id, and so its
 * record. Only the depth's acquire / release orders the two, and ThreadSanitizer
 * reports a plain access here. */
static int done_a, done_b;
static void *detached(void *a)
{
    gptps_submit(E, "t", NULL, 0, NULL);
    __atomic_store_n((int *)a, 1, __ATOMIC_RELEASE);
    return NULL;
}
static void run_detached(void)
{
    pthread_t t; pthread_attr_t at; struct timespec ts = {0, 100000000};
    pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, detached, &done_a)) done_a = 1;
    nanosleep(&ts, NULL);                  /* the first has exited: its id is free */
    if (pthread_create(&t, &at, detached, &done_b)) done_b = 1;
    pthread_attr_destroy(&at);
    while (!__atomic_load_n(&done_a, __ATOMIC_ACQUIRE) || !__atomic_load_n(&done_b, __ATOMIC_ACQUIRE))
        nanosleep(&ts, NULL);
}

int main(void)
{
    gptps_config cfg; gptps_task_def d; pthread_t th[NTHREADS]; size_t ran = 0; int i;
    memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.mode = GPTPS_RUN_MANUAL;
    if (gptps_open_ex(&cfg, &E) != GPTPS_OK) return 2;
    memset(&d, 0, sizeof d); d.struct_size = sizeof d; d.name = "t"; d.run = body; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_policy.struct_size = sizeof d.default_policy;
    if (gptps_register_task(E, &d) != GPTPS_OK) return 3;
    gptps_register_observer(E, obs, NULL);
    for (i = 0; i < NTHREADS; ++i) if (pthread_create(&th[i], NULL, host, NULL)) return 4;
    for (i = 0; i < NTHREADS; ++i) pthread_join(th[i], NULL);
    run_detached();
    while (gptps_step(E, &ran) == GPTPS_OK && ran) { }
    if (gptps_shutdown(E) != GPTPS_OK) { puts("shutdown failed"); return 1; }
    if (n_busy != NTHREADS + 2 || n_other != 0) {
        printf("FAIL: %d of %d threads guarded past the cap (%d not)\n", n_busy, NTHREADS + 2, n_other);
        return 1;
    }
    puts("all cb-cap checks passed");
    return 0;
}
