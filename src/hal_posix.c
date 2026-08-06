/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * hal_posix.c - POSIX implementation of the GPTPS HAL (T3, first slice).
 *
 * Implemented here: hardware detection, monotonic clock, cancel flag.
 * Pending (later T3 increments): threads/pool, dlopen loader, OS memory cap.
 *
 * The cancel flag uses compiler atomic builtins (GCC/Clang, available in C99),
 * else C11 <stdatomic.h>, else a plain store with a documented weak guarantee.
 */

/* Feature-test macros must precede ANY system header so strict -std=c99 still
 * exposes the POSIX / clock / sysctl APIs the HAL uses. Defining them here lets
 * the file (and the single-file amalgamation) build with a plain `cc -std=c99`. */
#if !defined(_WIN32) /* POSIX backend; compiles to nothing on Windows (hal_win.c is used) */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#  define _DARWIN_C_SOURCE
#endif

#include "gptps_hal.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>
#include <signal.h>   /* sig_atomic_t: the pthread_atfork child flag */
#if defined(__linux__)
#  include <sys/sysinfo.h>
#  include <sched.h>   /* sched_getaffinity / CPU_COUNT (container CPU bound) */
#elif defined(__APPLE__)
#  include <sys/sysctl.h>
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

#if defined(__linux__)
/* Clamp detected CPU/RAM to this process's cgroup v2 limits + CPU affinity, so
 * auto-tune inside a container sizes to the container, not the host. Best-effort:
 * any unreadable/absent control file or "max" value leaves the host figure. */
static void cgroup_v2_clamp(gptps_hwinfo *out)
{
    FILE *f;
    /* CPU quota: cpu.max = "<quota> <period>" (or "max <period>" for unlimited). */
    f = fopen("/sys/fs/cgroup/cpu.max", "r");
    if (f) {
        char q[32]; unsigned long long period = 0;
        if (fscanf(f, "%31s %llu", q, &period) == 2 && period > 0 && strcmp(q, "max") != 0) {
            unsigned long long quota = strtoull(q, NULL, 10);
            if (quota > 0) {
                unsigned eff = (unsigned)((quota + period - 1) / period); /* ceil */
                if (eff < 1u) eff = 1u;
                if (eff < out->cpu_count) out->cpu_count = eff;
            }
        }
        fclose(f);
    }
    /* CPU affinity mask: a tighter bound than online CPUs (e.g. cpuset pinning). */
    {
        cpu_set_t set;
        if (sched_getaffinity(0, sizeof set, &set) == 0) {
            int c = CPU_COUNT(&set);
            if (c > 0 && (unsigned)c < out->cpu_count) out->cpu_count = (unsigned)c;
        }
    }
    /* Memory limit: memory.max = "<bytes>" (or "max" for unlimited). */
    f = fopen("/sys/fs/cgroup/memory.max", "r");
    if (f) {
        char m[64];
        if (fscanf(f, "%63s", m) == 1 && strcmp(m, "max") != 0) {
            uint64_t lim = strtoull(m, NULL, 10);
            if (lim > 0 && (out->ram_bytes == 0 || lim < out->ram_bytes)) out->ram_bytes = lim;
        }
        fclose(f);
    }
}
#endif

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
#elif defined(__APPLE__)
    {
        int mib[2]; uint64_t mem = 0; size_t len = sizeof mem;
        mib[0] = CTL_HW; mib[1] = HW_MEMSIZE;
        if (sysctl(mib, 2, &mem, &len, NULL, 0) == 0) out->ram_bytes = mem;
    }
#endif
#if defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
    if (out->ram_bytes == 0) {
        long pages = sysconf(_SC_PHYS_PAGES);
        long psize = sysconf(_SC_PAGESIZE);
        if (pages > 0 && psize > 0)
            out->ram_bytes = (uint64_t)pages * (uint64_t)psize;
    }
#endif

#if defined(__linux__)
    cgroup_v2_clamp(out);   /* size to the container, not the host */
#endif

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
    if (!m) return NULL;
#if defined(GPTPS_HAL_FAST) && defined(__GLIBC__)
    /* Fast HAL (opt-in build knob, glibc): an ADAPTIVE mutex spins briefly before
     * blocking, cutting the syscall/wakeup cost of the engine's short, contended
     * critical sections under high submit/dispatch load. Same lock semantics as a
     * plain mutex (non-recursive, no correctness change) - purely a latency knob.
     * Gated on __GLIBC__ because PTHREAD_MUTEX_ADAPTIVE_NP is a glibc ENUM constant
     * (not a macro, so it can't be #if defined-tested); non-glibc toolchains fall
     * through to the portable default below. Requires _GNU_SOURCE (set at the top). */
    {
        pthread_mutexattr_t a; int rc;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ADAPTIVE_NP);
        rc = pthread_mutex_init(&m->m, &a);
        pthread_mutexattr_destroy(&a);
        if (rc != 0) { free(m); return NULL; }
    }
#else
    if (pthread_mutex_init(&m->m, NULL) != 0) { free(m); return NULL; }
#endif
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

uint64_t gptps_hal_thread_id(void)
{
    /* pthread_t is opaque and NOT required to be an integer, so it cannot be cast.
     * Copying its bytes is the portable way to get a comparable value: it is an
     * unsigned long on glibc/musl and a pointer on macOS/BSD, so 8 bytes capture it
     * exactly on every target this HAL builds for. Wider (or struct) pthread_t
     * implementations keep only the leading bytes, which is still sound for the one
     * thing the core does with this - comparing it against a thread's own id. */
    pthread_t self = pthread_self();
    uint64_t id = 0;
    memcpy(&id, &self, (sizeof self < sizeof id) ? sizeof self : sizeof id);
    return id;
}

/* --- fork safety ---------------------------------------------------------
 * A host that fork()s while GPTPS worker threads are live gets a child in which
 * only the calling thread exists, but the engine mutex may have been LOCKED by a
 * thread that did not survive - so the child's first gptps_submit() blocks
 * forever on a lock nobody will ever release. POSIX says a child of a
 * multithreaded parent may only call async-signal-safe functions before exec(),
 * so the honest contract is: a forked child must exec() or _exit(), never keep
 * using the engine. These handlers make violating it FAIL rather than hang -
 * gptps_hal_forked_child() reports true in the child, and the engine turns every
 * subsequent entry point into GPTPS_E_SHUTDOWN.
 * (The engine's own OOP executor is unaffected: its child execs or _exit()s and
 * never re-enters the engine - see gptps_run_capture.) */
static volatile sig_atomic_t g_fork_gen = 0;
static void gptps__atfork_child(void) { g_fork_gen = (sig_atomic_t)(g_fork_gen + 1); }
static void gptps__install_atfork(void) { pthread_atfork(NULL, NULL, gptps__atfork_child); }

uint64_t gptps_hal_fork_generation(void) { return (uint64_t)g_fork_gen; }

void gptps_hal_fork_guard_install(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, gptps__install_atfork);
}

/* ------------------------------------------------------------------------- */
/* dynamic loading (add-on loader)                                           */
/* ------------------------------------------------------------------------- */

struct gptps_dl { void *handle; };

gptps_dl *gptps_dl_open(const char *path)
{
    struct gptps_dl *d;
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL); /* LOCAL: add-on can't capture core symbols */
    if (!h) return NULL;
    d = (struct gptps_dl *)malloc(sizeof *d);
    if (!d) { dlclose(h); return NULL; }
    d->handle = h;
    return d;
}
void *gptps_dl_sym(gptps_dl *h, const char *symbol) { return h ? dlsym(h->handle, symbol) : NULL; }
void  gptps_dl_close(gptps_dl *h) { if (h) { dlclose(h->handle); free(h); } }

gptps_status gptps_hal_atomic_replace(const char *tmp_path, const char *final_path)
{ return rename(tmp_path, final_path) == 0 ? GPTPS_OK : GPTPS_E_IO; }

#endif /* !_WIN32 */
