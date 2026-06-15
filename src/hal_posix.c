/*
 * hal_posix.c - POSIX implementation of the GPTPS HAL (T3, first slice).
 *
 * Implemented here: hardware detection, monotonic clock, cancel flag.
 * Pending (later T3 increments): threads/pool, dlopen loader, OS memory cap.
 *
 * The cancel flag uses compiler atomic builtins (GCC/Clang, available in C99),
 * else C11 <stdatomic.h>, else a plain store with a documented weak guarantee.
 */
#include "gptps_hal.h"

#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#if defined(__linux__)
#  include <sys/sysinfo.h>
#endif

#if !defined(__GNUC__) && (__STDC_VERSION__ >= 201112L) && !defined(__STDC_NO_ATOMICS__)
#  include <stdatomic.h>
#endif

/* ------------------------------------------------------------------------- */
/* cancel flag                                                               */
/* ------------------------------------------------------------------------- */

struct gptps_flag {
#if defined(__GNUC__)
    int v;                 /* accessed via __atomic_* builtins (C99-compatible) */
#elif (__STDC_VERSION__ >= 201112L) && !defined(__STDC_NO_ATOMICS__)
    _Atomic int v;
#else
    volatile int v;        /* fallback: no true cross-thread barrier; weak-memory
                            * CPUs need a real barrier here. Documented limitation. */
#endif
};

void gptps_flag_set(gptps_flag *f, bool value)
{
    int x = value ? 1 : 0;
#if defined(__GNUC__)
    __atomic_store_n(&f->v, x, __ATOMIC_SEQ_CST);
#elif (__STDC_VERSION__ >= 201112L) && !defined(__STDC_NO_ATOMICS__)
    atomic_store(&f->v, x);
#else
    f->v = x;
#endif
}

bool gptps_flag_get(const gptps_flag *f)
{
    struct gptps_flag *m = (struct gptps_flag *)f; /* builtins want a non-const lvalue */
#if defined(__GNUC__)
    return __atomic_load_n(&m->v, __ATOMIC_SEQ_CST) != 0;
#elif (__STDC_VERSION__ >= 201112L) && !defined(__STDC_NO_ATOMICS__)
    return atomic_load(&m->v) != 0;
#else
    return m->v != 0;
#endif
}

gptps_flag *gptps_flag_create(bool initial)
{
    struct gptps_flag *f = (struct gptps_flag *)malloc(sizeof *f);
    if (!f) return NULL;
    gptps_flag_set(f, initial);
    return f;
}

void gptps_flag_destroy(gptps_flag *f)
{
    free(f);
}

/* ------------------------------------------------------------------------- */
/* monotonic clock                                                           */
/* ------------------------------------------------------------------------- */

uint64_t gptps_hal_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* ------------------------------------------------------------------------- */
/* hardware detection                                                        */
/* ------------------------------------------------------------------------- */

gptps_status gptps_hal_detect(gptps_hwinfo *out)
{
    long n;

    if (!out) return GPTPS_E_INVAL;

    n = sysconf(_SC_NPROCESSORS_ONLN);
    out->cpu_count = (n > 0) ? (unsigned)n : 1u;

    out->ram_bytes = 0;
#if defined(__linux__)
    {
        struct sysinfo si;
        if (sysinfo(&si) == 0)
            out->ram_bytes = (uint64_t)si.totalram * (uint64_t)si.mem_unit;
    }
#endif
    if (out->ram_bytes == 0) {
        long pages = sysconf(_SC_PHYS_PAGES);
        long psize = sysconf(_SC_PAGESIZE);
        if (pages > 0 && psize > 0)
            out->ram_bytes = (uint64_t)pages * (uint64_t)psize;
    }

    out->has_gpu = false; /* unknown without a vendor GPU add-on */
    return GPTPS_OK;
}

/* ------------------------------------------------------------------------- */
/* threads / mutex / condvar (pthreads)                                      */
/* ------------------------------------------------------------------------- */

struct gptps_mutex  { pthread_mutex_t m; };
struct gptps_cond   { pthread_cond_t  c; };
struct gptps_thread { pthread_t t; gptps_thread_fn fn; void *arg; };

static void *gptps__thread_trampoline(void *p)
{
    struct gptps_thread *th = (struct gptps_thread *)p;
    return th->fn(th->arg);
}

gptps_mutex *gptps_mutex_create(void)
{
    struct gptps_mutex *m = (struct gptps_mutex *)malloc(sizeof *m);
    if (m && pthread_mutex_init(&m->m, NULL) != 0) { free(m); m = NULL; }
    return m;
}
void gptps_mutex_destroy(gptps_mutex *m) { if (m) { pthread_mutex_destroy(&m->m); free(m); } }
void gptps_mutex_lock(gptps_mutex *m)    { pthread_mutex_lock(&m->m); }
void gptps_mutex_unlock(gptps_mutex *m)  { pthread_mutex_unlock(&m->m); }

gptps_cond *gptps_cond_create(void)
{
    struct gptps_cond *c = (struct gptps_cond *)malloc(sizeof *c);
    pthread_condattr_t a;
    if (!c) return NULL;
    pthread_condattr_init(&a);
#if defined(__linux__)
    /* match the engine's CLOCK_MONOTONIC timing so timed waits are immune to
     * wall-clock (CLOCK_REALTIME) steps; gptps_cond_timedwait uses MONOTONIC too. */
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
#endif
    if (pthread_cond_init(&c->c, &a) != 0) { pthread_condattr_destroy(&a); free(c); return NULL; }
    pthread_condattr_destroy(&a);
    return c;
}
void gptps_cond_destroy(gptps_cond *c)              { if (c) { pthread_cond_destroy(&c->c); free(c); } }
void gptps_cond_wait(gptps_cond *c, gptps_mutex *m) { pthread_cond_wait(&c->c, &m->m); }
void gptps_cond_signal(gptps_cond *c)               { pthread_cond_signal(&c->c); }
void gptps_cond_broadcast(gptps_cond *c)            { pthread_cond_broadcast(&c->c); }

void gptps_cond_timedwait(gptps_cond *c, gptps_mutex *m, uint64_t ms)
{
    struct timespec ts;
#if defined(__linux__)
    clock_gettime(CLOCK_MONOTONIC, &ts); /* cond created with CLOCK_MONOTONIC */
#else
    clock_gettime(CLOCK_REALTIME, &ts);  /* fallback: setclock unavailable */
#endif
    ts.tv_sec  += (time_t)(ms / 1000u);
    ts.tv_nsec += (long)((ms % 1000u) * 1000000u);
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(&c->c, &m->m, &ts);
}

gptps_thread *gptps_thread_start(gptps_thread_fn fn, void *arg)
{
    struct gptps_thread *th = (struct gptps_thread *)malloc(sizeof *th);
    if (!th) return NULL;
    th->fn = fn; th->arg = arg;
    if (pthread_create(&th->t, NULL, gptps__thread_trampoline, th) != 0) { free(th); return NULL; }
    return th;
}
void gptps_thread_join(gptps_thread *t)
{
    if (!t) return;
    pthread_join(t->t, NULL);
    free(t);
}
