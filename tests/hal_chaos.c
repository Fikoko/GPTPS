/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * hal_chaos.c - the weakest HAL the contract allows, for testing the CORE.
 *
 * tests/test_hal_conformance.c holds a HAL to the contract in gptps_hal.h. This
 * file tests the other side: that the core needs nothing more. It wraps the POSIX
 * HAL and takes every freedom the contract gives a port:
 *
 *   - a cond wait may return without a signal (a spurious wakeup), so the core
 *     must re-check every predicate it waits for;
 *   - a timed wait may return early, or up to a clock step late;
 *   - a signal may wake every waiter instead of one;
 *   - the clock moves in 16 ms steps, as Windows' does, so two readings can be
 *     equal across real time;
 *   - a lock may yield before it is taken, so threads interleave differently;
 *   - a new thread may start late.
 *
 * Build the suite on it and run it - CI's hal_chaos job does exactly this:
 *
 *   cmake -S . -B build-chaos -DGPTPS_HAL_SOURCE=$PWD/tests/hal_chaos.c
 *   cmake --build build-chaos -j && ctest --test-dir build-chaos
 *
 * One in GPTPS_CHAOS_RATE calls (default 4) takes the freedom, choosing by a
 * counter mixed with GPTPS_CHAOS_SEED. The interleavings still vary run to run:
 * this is a fuzzer for the core's assumptions, not a replay. POSIX only - the
 * core is the same C on every platform, so one platform is enough to test it.
 */
#define gptps_mutex_lock       chaos_real_mutex_lock
#define gptps_cond_wait        chaos_real_cond_wait
#define gptps_cond_timedwait   chaos_real_cond_timedwait
#define gptps_cond_signal      chaos_real_cond_signal
#define gptps_hal_monotonic_ms chaos_real_monotonic_ms
#define gptps_thread_start     chaos_real_thread_start
#include "../src/hal_posix.c"     /* first: its feature macros precede every system header */
#undef gptps_thread_start
#undef gptps_mutex_lock
#undef gptps_cond_wait
#undef gptps_cond_timedwait
#undef gptps_cond_signal
#undef gptps_hal_monotonic_ms

#include <sched.h>

/* gptps_hal.h was read with the names above renamed, so declare the public ones. */
void     gptps_mutex_lock(gptps_mutex *m);
void     gptps_cond_wait(gptps_cond *c, gptps_mutex *m);
void     gptps_cond_timedwait(gptps_cond *c, gptps_mutex *m, uint64_t ms);
void     gptps_cond_signal(gptps_cond *c);
uint64_t gptps_hal_monotonic_ms(void);
gptps_thread *gptps_thread_start(gptps_thread_fn fn, void *arg);

#define CHAOS_STEP_MS 16u

static uint64_t chaos_seed, chaos_rate;
static uint64_t chaos_calls;
static pthread_once_t chaos_once = PTHREAD_ONCE_INIT;

static void chaos_init(void)
{
    const char *s = getenv("GPTPS_CHAOS_SEED"), *r = getenv("GPTPS_CHAOS_RATE");
    chaos_seed = s ? strtoull(s, NULL, 10) : 0x9E3779B97F4A7C15ull;
    chaos_rate = r ? strtoull(r, NULL, 10) : 4u;
    if (chaos_rate == 0) chaos_rate = 4u;
}

/* Take the freedom this time? A per-call counter through a 64-bit mixer. */
static int chaos(void)
{
    uint64_t x;
    pthread_once(&chaos_once, chaos_init);
    x = __atomic_add_fetch(&chaos_calls, 1, __ATOMIC_RELAXED) + chaos_seed;
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27; x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x % chaos_rate == 0;
}

/* A spurious wakeup, as the contract allows one: the mutex is let go and taken
 * back, and the caller returns with no signal and no timeout behind it. */
static void spurious(gptps_mutex *m)
{
    pthread_mutex_unlock(&m->m);
    sched_yield();
    pthread_mutex_lock(&m->m);
}

void gptps_mutex_lock(gptps_mutex *m)
{
    if (chaos()) sched_yield();
    chaos_real_mutex_lock(m);
}

void gptps_cond_wait(gptps_cond *c, gptps_mutex *m)
{
    if (chaos()) { spurious(m); return; }
    chaos_real_cond_wait(c, m);
}

void gptps_cond_timedwait(gptps_cond *c, gptps_mutex *m, uint64_t ms)
{
    if (chaos()) { spurious(m); return; }
    if (ms <= UINT64_MAX - CHAOS_STEP_MS)              /* late by up to a step, like a coarse timer */
        ms = (ms + CHAOS_STEP_MS - 1) / CHAOS_STEP_MS * CHAOS_STEP_MS;
    chaos_real_cond_timedwait(c, m, ms);
}

void gptps_cond_signal(gptps_cond *c)
{
    if (chaos()) pthread_cond_broadcast(&c->c);         /* more woken than asked for */
    else chaos_real_cond_signal(c);
}

uint64_t gptps_hal_monotonic_ms(void)
{
    return chaos_real_monotonic_ms() / CHAOS_STEP_MS * CHAOS_STEP_MS;
}

/* A thread that starts late: it yields and naps before it runs its function, so
 * the core cannot count on a thread it started being under way when the call
 * that started it returns. */
typedef struct { gptps_thread_fn fn; void *arg; } chaos_start;

static void *chaos_late(void *p)
{
    chaos_start cs = *(chaos_start *)p;
    struct timespec ms1 = { 0, 1000000L };
    free(p);
    sched_yield();
    nanosleep(&ms1, NULL);
    return cs.fn(cs.arg);
}

gptps_thread *gptps_thread_start(gptps_thread_fn fn, void *arg)
{
    chaos_start *cs;
    gptps_thread *t;
    if (!chaos() || !(cs = (chaos_start *)malloc(sizeof *cs))) return chaos_real_thread_start(fn, arg);
    cs->fn = fn; cs->arg = arg;
    if (!(t = chaos_real_thread_start(chaos_late, cs))) free(cs);
    return t;
}
