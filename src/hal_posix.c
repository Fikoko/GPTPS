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
#include <sys/stat.h> /* the replaced file's permission bits, kept by atomic_replace */
#include <fcntl.h>    /* atomic_replace: open, F_FULLFSYNC */
#include <errno.h>
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
/* cancel flag - the core no longer calls these (see gptps_hal.h); kept so     */
/* anything built against the older contract still links                      */
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

/* Same fallback caveat as the flag: without the builtins, a plain volatile access
 * (C11 atomics would need an _Atomic object, which the core does not have). */
uint32_t gptps_hal_load_acquire_u32(const uint32_t *p)
{
#if defined(__GNUC__)
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
#else
    return *(const volatile uint32_t *)p;
#endif
}

void gptps_hal_store_release_u32(uint32_t *p, uint32_t v)
{
#if defined(__GNUC__)
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
#else
    *(volatile uint32_t *)p = v;
#endif
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
/* Clamp detected CPU/RAM to the limits of the cgroup THIS process is in, so auto-tune
 * sizes to the container or service, not the host. The cgroup is found the way the
 * kernel describes it: /proc/self/cgroup names it, /proc/self/mountinfo says where its
 * hierarchy is mounted (and which part of it - a container without its own cgroup
 * namespace sees the host's tree), and the limits are read from it AND every cgroup
 * above it up to the mount, the tightest winning - a systemd service's MemoryMax=
 * sits on its own cgroup, a slice's on the parent, and neither on the root that is
 * /sys/fs/cgroup outside a container. cgroup v2 (memory.max, cpu.max) and v1
 * (memory.limit_in_bytes, cpu.cfs_quota_us / cpu.cfs_period_us) both. Best-effort:
 * a file that is absent, unreadable or says "max" leaves the figure as it was.
 *
 * gptps__cgroup_clamp_at is the whole of it with the three inputs named - the files
 * and a prefix for every mount point - so tests/test_hal_posix.c can hand it a tree of
 * its own. Internal: not in any header but its test's. */
void gptps__cgroup_clamp_at(const char *self_cgroup, const char *mountinfo, const char *prefix,
                            gptps_hwinfo *out);

#define GPTPS_CG_PATH 1024
typedef struct { char root[GPTPS_CG_PATH], point[GPTPS_CG_PATH], path[GPTPS_CG_PATH]; int have_mnt, have_path; } cg_hier;

/* "a,b,c" holds `tok` as one of its comma-separated words */
static int cg_has_word(const char *list, const char *tok)
{
    size_t n = strlen(tok);
    const char *p = list;
    while (p && *p) {
        const char *e = strchr(p, ',');
        size_t L = e ? (size_t)(e - p) : strlen(p);
        if (L == n && strncmp(p, tok, n) == 0) return 1;
        p = e ? e + 1 : NULL;
    }
    return 0;
}

/* mountinfo escapes space, tab, newline and backslash as \ooo */
static void cg_unescape(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    while (*src && n + 1 < cap) {
        if (src[0] == '\\' && src[1] >= '0' && src[1] <= '7' && src[2] >= '0' && src[2] <= '7' &&
            src[3] >= '0' && src[3] <= '7') {
            dst[n++] = (char)(((src[1] - '0') << 6) | ((src[2] - '0') << 3) | (src[3] - '0'));
            src += 4;
        } else {
            dst[n++] = *src++;
        }
    }
    dst[n] = 0;
}

/* Read a whole line into buf; a longer one is cut and its rest skipped. 0 at EOF. */
static int cg_line(FILE *f, char *buf, size_t cap)
{
    size_t n;
    if (!fgets(buf, (int)cap, f)) return 0;
    n = strlen(buf);
    if (n && buf[n - 1] == '\n') buf[--n] = 0;
    else { int c; while ((c = fgetc(f)) != EOF && c != '\n') { } }
    return 1;
}

/* The first line of a small file, or 0. */
static int cg_read1(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "r");
    int ok;
    if (!f) return 0;
    ok = cg_line(f, buf, cap);
    fclose(f);
    return ok;
}

/* The directory of this process's cgroup in `h`, into dir (prefix + mount point +
 * the path below the mount's root). 0 when it cannot be placed. */
static int cg_dir(const cg_hier *h, const char *prefix, char *dir, size_t cap)
{
    const char *rel = h->path;
    size_t rl = strlen(h->root);
    int n;
    if (!h->have_mnt || !h->have_path) return 0;
    if (strcmp(h->root, "/") != 0) {
        /* the mount shows part of the tree: the path must be below that part */
        if (strncmp(h->path, h->root, rl) == 0 && (h->path[rl] == '/' || h->path[rl] == 0)) rel = h->path + rl;
        else rel = "";                             /* not ours to place: the mount's own */
    }
    if (strcmp(rel, "/") == 0) rel = "";
    n = snprintf(dir, cap, "%s%s%s", prefix, strcmp(h->point, "/") == 0 ? "" : h->point, rel);
    return n > 0 && (size_t)n < cap;
}

/* The first line of `dir`/`file`, or 0 - also for a path too long to build. */
static int cg_read_at(const char *dir, const char *file, char *buf, size_t cap)
{
    char path[3 * GPTPS_CG_PATH + 64];
    int n = snprintf(path, sizeof path, "%s/%s", dir, file);
    if (n < 0 || (size_t)n >= sizeof path) return 0;
    return cg_read1(path, buf, cap);
}

/* Walk from `dir` up to (and including) the mount point at `stop` chars, calling the
 * reader at each level. */
typedef void (*cg_level_fn)(const char *dir, gptps_hwinfo *out);
static void cg_walk(char *dir, size_t stop, cg_level_fn fn, gptps_hwinfo *out)
{
    for (;;) {
        char *slash;
        fn(dir, out);
        if (strlen(dir) <= stop) return;
        slash = strrchr(dir, '/');
        if (!slash || (size_t)(slash - dir) < stop) return;
        *slash = 0;
        if (strlen(dir) < stop) return;
    }
}

static void cg_mem_limit(gptps_hwinfo *out, const char *val)
{
    char *end;
    uint64_t lim;
    if (strncmp(val, "max", 3) == 0) return;
    lim = (uint64_t)strtoull(val, &end, 10);
    if (end == val || lim == 0) return;
    if (out->ram_bytes == 0 || lim < out->ram_bytes) out->ram_bytes = lim;   /* v1 "no limit" is huge */
}

static void cg_cpu_limit(gptps_hwinfo *out, long long quota, long long period)
{
    unsigned eff;
    if (quota <= 0 || period <= 0) return;
    eff = (unsigned)((quota + period - 1) / period);   /* ceil */
    if (eff < 1u) eff = 1u;
    if (eff < out->cpu_count) out->cpu_count = eff;
}

static void cg_v2_level(const char *dir, gptps_hwinfo *out)
{
    char buf[128];
    if (cg_read_at(dir, "memory.max", buf, sizeof buf)) cg_mem_limit(out, buf);
    if (cg_read_at(dir, "cpu.max", buf, sizeof buf)) {
        char q[32];
        long long period = 0;
        if (sscanf(buf, "%31s %lld", q, &period) == 2 && strcmp(q, "max") != 0)
            cg_cpu_limit(out, strtoll(q, NULL, 10), period);
    }
}

static void cg_v1_mem_level(const char *dir, gptps_hwinfo *out)
{
    char buf[64];
    if (cg_read_at(dir, "memory.limit_in_bytes", buf, sizeof buf)) cg_mem_limit(out, buf);
}

static void cg_v1_cpu_level(const char *dir, gptps_hwinfo *out)
{
    char q[64], p[64];
    if (!cg_read_at(dir, "cpu.cfs_quota_us", q, sizeof q)) return;
    if (!cg_read_at(dir, "cpu.cfs_period_us", p, sizeof p)) return;
    cg_cpu_limit(out, strtoll(q, NULL, 10), strtoll(p, NULL, 10));   /* -1 = no quota */
}

static void cg_apply(const cg_hier *h, const char *prefix, cg_level_fn fn, gptps_hwinfo *out)
{
    char dir[3 * GPTPS_CG_PATH];
    size_t stop;
    if (!cg_dir(h, prefix, dir, sizeof dir)) return;
    stop = strlen(prefix) + (strcmp(h->point, "/") == 0 ? 0 : strlen(h->point));
    cg_walk(dir, stop, fn, out);
}

void gptps__cgroup_clamp_at(const char *self_cgroup, const char *mountinfo, const char *prefix,
                            gptps_hwinfo *out)
{
    cg_hier v2, mem1, cpu1;
    char line[4 * GPTPS_CG_PATH];
    FILE *f;
    memset(&v2, 0, sizeof v2); memset(&mem1, 0, sizeof mem1); memset(&cpu1, 0, sizeof cpu1);
    /* which cgroup: "0::<path>" (v2), "<id>:<controllers>:<path>" (v1) */
    if ((f = fopen(self_cgroup, "r")) != NULL) {
        while (cg_line(f, line, sizeof line)) {
            char *c1 = strchr(line, ':'), *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
            if (!c2) continue;
            *c1 = 0; *c2 = 0;
            if (strcmp(line, "0") == 0 && c1[1] == 0) {
                snprintf(v2.path, sizeof v2.path, "%s", c2 + 1); v2.have_path = 1;
            } else {
                if (cg_has_word(c1 + 1, "memory")) { snprintf(mem1.path, sizeof mem1.path, "%s", c2 + 1); mem1.have_path = 1; }
                if (cg_has_word(c1 + 1, "cpu"))    { snprintf(cpu1.path, sizeof cpu1.path, "%s", c2 + 1); cpu1.have_path = 1; }
            }
        }
        fclose(f);
    }
    /* where: "<id> <parent> <maj:min> <root> <point> <opts> [optional...] - <fstype> <src> <super>" */
    if ((f = fopen(mountinfo, "r")) != NULL) {
        while (cg_line(f, line, sizeof line)) {
            char *fld[6], *p = line, *dash, *fstype, *super = NULL, *tok;
            int i;
            dash = strstr(line, " - ");
            if (!dash) continue;
            *dash = 0;
            fstype = dash + 3;
            tok = strchr(fstype, ' ');
            if (tok) { *tok = 0; tok = strchr(tok + 1, ' '); if (tok) super = tok + 1; }
            for (i = 0; i < 6; ++i) {
                while (*p == ' ') ++p;
                fld[i] = p;
                p = strchr(p, ' ');
                if (!p) { if (i < 4) goto next; break; }
                *p++ = 0;
            }
            if (strcmp(fstype, "cgroup2") == 0 && !v2.have_mnt) {
                cg_unescape(v2.root, sizeof v2.root, fld[3]); cg_unescape(v2.point, sizeof v2.point, fld[4]);
                v2.have_mnt = 1;
            } else if (strcmp(fstype, "cgroup") == 0 && super) {
                if (!mem1.have_mnt && cg_has_word(super, "memory")) {
                    cg_unescape(mem1.root, sizeof mem1.root, fld[3]); cg_unescape(mem1.point, sizeof mem1.point, fld[4]);
                    mem1.have_mnt = 1;
                }
                if (!cpu1.have_mnt && cg_has_word(super, "cpu")) {
                    cg_unescape(cpu1.root, sizeof cpu1.root, fld[3]); cg_unescape(cpu1.point, sizeof cpu1.point, fld[4]);
                    cpu1.have_mnt = 1;
                }
            }
        next:;
        }
        fclose(f);
    }
    if (v2.have_mnt && v2.have_path) {
        cg_apply(&v2, prefix, cg_v2_level, out);
    } else if (!v2.have_mnt && !mem1.have_mnt && !cpu1.have_mnt) {
        /* no mountinfo to go by: the conventional place, as before */
        char dir[GPTPS_CG_PATH + 32];
        int n = snprintf(dir, sizeof dir, "%s/sys/fs/cgroup", prefix);
        if (n > 0 && (size_t)n < sizeof dir) cg_v2_level(dir, out);
    }
    if (mem1.have_mnt && mem1.have_path) cg_apply(&mem1, prefix, cg_v1_mem_level, out);
    if (cpu1.have_mnt && cpu1.have_path) cg_apply(&cpu1, prefix, cg_v1_cpu_level, out);
}

static void cgroup_clamp(gptps_hwinfo *out)
{
    gptps__cgroup_clamp_at("/proc/self/cgroup", "/proc/self/mountinfo", "", out);
    /* CPU affinity mask: a tighter bound than online CPUs (e.g. cpuset pinning). */
    {
        cpu_set_t set;
        if (sched_getaffinity(0, sizeof set, &set) == 0) {
            int c = CPU_COUNT(&set);
            if (c > 0 && (unsigned)c < out->cpu_count) out->cpu_count = (unsigned)c;
        }
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
    cgroup_clamp(out);   /* size to the container or service, not the host */
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

/* The core may ask for any wait up to UINT64_MAX ms: its one timed wait sleeps until
 * the next deadline, and a task's deadline is timeout_seconds * 1000 ms away. Added to
 * the clock unclamped, that overflows a 32-bit time_t, and pthread_cond_timedwait then
 * returns at once, every time - the dispatcher spins. A day is far enough: the core
 * works out its wait again each time it wakes. */
#define GPTPS_HAL_MAX_WAIT_MS ((uint64_t)24u * 60u * 60u * 1000u)

void gptps_cond_timedwait(gptps_cond *c, gptps_mutex *m, uint64_t ms)
{
    struct timespec ts;
    if (ms > GPTPS_HAL_MAX_WAIT_MS) ms = GPTPS_HAL_MAX_WAIT_MS;
#if defined(__APPLE__)
    /* No pthread_condattr_setclock here, so an absolute deadline would be on the wall
     * clock, which a time change moves. A relative wait is measured on a clock that
     * nothing moves. */
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    pthread_cond_timedwait_relative_np(&c->c, &m->m, &ts);
#else
#  if defined(__linux__)
    clock_gettime(CLOCK_MONOTONIC, &ts); /* cond created with CLOCK_MONOTONIC */
#  else
    clock_gettime(CLOCK_REALTIME, &ts);  /* fallback: setclock not wired up here */
#  endif
    ts.tv_sec  += (time_t)(ms / 1000u);
    ts.tv_nsec += (long)((ms % 1000u) * 1000000u);
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(&c->c, &m->m, &ts);
#endif
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
/* Drop the wrapper, keep the mapping (see gptps_hal.h). */
void  gptps_dl_release(gptps_dl *h) { free(h); }
/* dlerror(): the reason of the last failed dlopen / dlsym, on this thread - which
 * library or symbol was missing, a wrong architecture, ... */
size_t gptps_dl_error(char *buf, size_t cap)
{
    const char *m = dlerror();
    size_t n;
    if (!buf || !cap) return 0;
    buf[0] = 0;
    if (!m) return 0;
    /* A plain bounded copy, cut to cap - 1 like snprintf("%s") was, without the format
     * call gcc -Wformat-truncation=2 flags for an unbounded %s into a run-time cap. */
    n = strlen(m);
    if (n > cap - 1) n = cap - 1;
    memcpy(buf, m, n);
    buf[n] = 0;
    return n;
}

/* ---- atomic, durable file replace (the settings save) -----------------------
 * Atomic: rename(). Durable: the new file's data is synced before the rename, and
 * the directory after it - without both, a power cut soon after a save can leave the
 * operator's config empty or zero-filled on a file system that does not order data
 * before a rename (XFS and others; ext4 mostly hides it). macOS gets F_FULLFSYNC,
 * which also flushes the drive's cache; a file system that refuses a sync (some
 * network and FUSE mounts) gets the best it offers, as in the durable queue.
 *
 * Permissions: the replaced file's carry over to its replacement. A save edits the
 * operator's config file: one they kept to themselves must not come back readable by
 * others, nor one kept read-only to the group come back writable by it - the file can
 * name add-ons to load, so write access to it is code execution. Setting them on the
 * temp file at the end is not enough when the temp was created more open than the
 * target (umask): a descriptor opened on it while it was written keeps that access,
 * and after the rename that inode IS the config. So then the content goes into a new
 * inode, created private (mkstemp) and given the target's owner and mode, and the
 * temp file is unlinked: a descriptor anyone holds on it AFTER the replace reaches
 * nothing that is read. That is not enough on its own: what someone wrote into the
 * temp file BEFORE the replace is copied with it. So the temp file must be private
 * from its creation - gptps_hal_open_private, which settings save uses: O_EXCL, mode
 * 0600, whatever the umask, so nobody but its owner can open it at all. The new-inode
 * copy then only matters when the owner differs (root saving a user's file), and for
 * a temp file a caller made some other way. */
static int gptps__cannot_sync(int err)
{
    switch (err) {
    case EINVAL: case EBADF: case EISDIR: case EROFS: case EACCES: case EPERM:
#if defined(ENOTSUP)
    case ENOTSUP:
#endif
#if defined(EOPNOTSUPP) && (!defined(ENOTSUP) || EOPNOTSUPP != ENOTSUP)
    case EOPNOTSUPP:
#endif
        return 1;
    default:
        return 0;
    }
}

/* 0 once the descriptor's data is on disk, or the file system cannot be asked */
static int gptps__fsync_fd(int fd)
{
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    if (fcntl(fd, F_FULLFSYNC) == 0) return 0;
#endif
    while (fsync(fd) != 0) {
        if (errno == EINTR) continue;
        return gptps__cannot_sync(errno) ? 0 : -1;
    }
    return 0;
}

static int gptps__fsync_path(const char *path, int dir)
{
    int fd, rc;
#if defined(O_DIRECTORY)
    fd = open(path, O_RDONLY | (dir ? O_DIRECTORY : 0) | O_CLOEXEC);
#else
    (void)dir;
    fd = open(path, O_RDONLY | O_CLOEXEC);
#endif
    if (fd < 0) return gptps__cannot_sync(errno) ? 0 : -1;
    rc = gptps__fsync_fd(fd);
    close(fd);
    return rc;
}

/* The directory holding `path`, synced so the rename itself is on disk. Best-effort:
 * the replace has happened by now and cannot be taken back. */
static void gptps__fsync_parent(const char *path)
{
    const char *slash = strrchr(path, '/');
    char *dir;
    size_t n;
    if (!slash) { (void)gptps__fsync_path(".", 1); return; }
    n = (slash == path) ? 1u : (size_t)(slash - path);
    dir = (char *)malloc(n + 1);
    if (!dir) return;
    memcpy(dir, path, n);
    dir[n] = 0;
    (void)gptps__fsync_path(dir, 1);
    free(dir);
}

/* Copy tmp into a new private file beside final, with final's owner and mode, and
 * sync it. Returns the new file's malloc'd name, or NULL. */
static char *gptps__fresh_copy(const char *tmp_path, const char *final_path, const struct stat *st)
{
    size_t n = strlen(final_path) + 16;
    char *name = (char *)malloc(n), buf[8192];
    int in = -1, out = -1, ok = 0;
    if (!name) return NULL;
    snprintf(name, n, "%s.gptps-XXXXXX", final_path);
    out = mkstemp(name);                           /* O_EXCL, mode 0600 */
    if (out < 0) { free(name); return NULL; }
    (void)fcntl(out, F_SETFD, FD_CLOEXEC);         /* another thread may fork and exec */
    in = open(tmp_path, O_RDONLY | O_CLOEXEC);
    if (in >= 0) {
        ok = 1;
        for (;;) {
            ssize_t r = read(in, buf, sizeof buf), off = 0;
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) { if (r < 0) ok = 0; break; }
            while (off < r) {
                ssize_t w = write(out, buf + off, (size_t)(r - off));
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) { ok = 0; break; }
                off += w;
            }
            if (!ok) break;
        }
        close(in);
    }
    /* owner first: chown clears set-id bits, which the mode then restores. Another
     * owner takes privilege, so an unprivileged save keeps its own - as rename did. */
    if (ok) {
        if (fchown(out, st->st_uid, st->st_gid) != 0) { /* not privileged: it stays ours */ }
        ok = fchmod(out, st->st_mode & 07777) == 0;
    }
    if (ok) ok = gptps__fsync_fd(out) == 0;
    if (close(out) != 0) ok = 0;
    if (!ok) { unlink(name); free(name); return NULL; }
    return name;
}

FILE *gptps_hal_open_private(const char *path)
{
    int fd;
    FILE *f;
    if (unlink(path) != 0 && errno != ENOENT) return NULL;
    /* O_EXCL: a file - or a symlink - someone put there in between is refused, never
     * written through; 0600 whatever the umask; close-on-exec, as another thread may
     * fork and exec while the save writes it */
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return NULL;
    f = fdopen(fd, "wb");
    if (!f) { close(fd); (void)unlink(path); }
    return f;
}

gptps_status gptps_hal_atomic_replace(const char *tmp_path, const char *final_path)
{
    struct stat st, ts;
    if (stat(tmp_path, &ts) != 0) return GPTPS_E_IO;
    if (stat(final_path, &st) == 0 &&
        ((ts.st_mode & 0777 & ~(st.st_mode & 0777)) != 0 || ts.st_uid != st.st_uid || ts.st_gid != st.st_gid)) {
        /* the temp file was open to someone the target is not: a new inode */
        char *fresh = gptps__fresh_copy(tmp_path, final_path, &st);
        if (!fresh) return GPTPS_E_IO;
        if (rename(fresh, final_path) != 0) { unlink(fresh); free(fresh); return GPTPS_E_IO; }
        free(fresh);
        (void)unlink(tmp_path);
        gptps__fsync_parent(final_path);
        return GPTPS_OK;
    }
    /* nobody could have opened the temp file with more than the target allows */
    if (stat(final_path, &st) == 0) (void)chmod(tmp_path, st.st_mode & 07777);
    if (gptps__fsync_path(tmp_path, 0) != 0) return GPTPS_E_IO;
    if (rename(tmp_path, final_path) != 0) return GPTPS_E_IO;
    gptps__fsync_parent(final_path);
    return GPTPS_OK;
}

#else
/* The POSIX backend compiles to nothing on Windows (hal_win.c is used). ISO C forbids an empty
 * translation unit (-pedantic), so one file-scope typedef keeps this one non-empty: a
 * typedef has no linkage and emits no code or symbol. */
typedef int gptps_hal_posix_not_this_platform;
#endif /* !_WIN32 */
