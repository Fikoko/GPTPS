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
