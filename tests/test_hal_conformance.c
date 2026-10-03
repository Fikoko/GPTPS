/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_hal_conformance.c - does a HAL keep the contract in include/gptps_hal.h?
 *
 * One program for every HAL: the POSIX and Win32 backends (CTest runs it on each
 * platform), a port built with -DGPTPS_HAL_SOURCE=<file>, and the freestanding
 * stub (freestanding/build.sh runs it with --freestanding). It calls nothing but
 * the HAL and hosted C99, so it builds wherever a HAL does. Each check names the
 * clause it holds the HAL to; docs/HAL.md lists the clauses and why the core
 * needs each one.
 *
 * What it cannot show is said at the check, not hidden:
 *   - Memory ordering cannot go wrong on x86, whatever the HAL does. The
 *     acquire/release check is a real test only on a weak-memory CPU (the arm64
 *     macOS runner) and under ThreadSanitizer, which CI runs this file under.
 *   - A spurious wakeup cannot be provoked, only tolerated. The core's side of
 *     that - that it needs nothing the contract does not promise - is tested by
 *     running the whole suite on tests/hal_chaos.c, the weakest legal HAL.
 *   - A wall-clock step cannot be made without privileges, so timed waits that
 *     drift with the wall clock are a review item, not a check.
 *   - thread_start's NULL-on-failure needs resource exhaustion to provoke.
 *
 * usage: test_hal_conformance [--freestanding] [--dl-only] [module-path]
 *   --freestanding  the HAL declares no real-time clock, no fork, no filesystem
 *                   and no dynamic loading; those checks are skipped, by name.
 *   --dl-only       only the dynamic-loading checks (CTest registers them apart,
 *                   so a job that cannot load modules - s390x under QEMU - can
 *                   leave them out and still run everything else).
 *   module-path     a loadable add-on, for the dynamic-loading checks.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L   /* fork, waitpid under -std=c99 */
#endif
#include "gptps.h"
#include "gptps_hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__unix__) || defined(__APPLE__)
#  include <unistd.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  define HAVE_FORK 1
#endif

/* --- reporting ------------------------------------------------------------ */

static int n_pass, n_fail, n_skip;
static char why[256];

static void pass(const char *what)                  { printf("  PASS  %s\n", what); ++n_pass; }
static void fail(const char *what, const char *msg) { printf("  FAIL  %s\n        %s\n", what, msg); ++n_fail; }
static void skip(const char *what, const char *msg) { printf("  SKIP  %s\n        %s\n", what, msg); ++n_skip; }
static void expect(int ok, const char *what)        { if (ok) pass(what); else fail(what, why); }

static unsigned long long ull(uint64_t v) { return (unsigned long long)v; }
static uint64_t now(void) { return gptps_hal_monotonic_ms(); }

/* A pause through the HAL itself: its timed wait on a private pair. On a HAL whose
 * timed wait returns at once (the freestanding stub) this is a busy pause, which
 * is all such a HAL offers. */
static void nap(uint64_t ms)
{
    gptps_mutex *m = gptps_mutex_create();
    gptps_cond  *c = gptps_cond_create();
    uint64_t end = now() + ms;
    if (!m || !c) { gptps_mutex_destroy(m); gptps_cond_destroy(c); return; }
    gptps_mutex_lock(m);
    while (now() < end) gptps_cond_timedwait(c, m, end - now());
    gptps_mutex_unlock(m);
    gptps_cond_destroy(c);
    gptps_mutex_destroy(m);
}

/* --- the watchdog ------------------------------------------------------------
 * A lost wakeup or a broken broadcast does not fail, it hangs. The watchdog turns
 * a hang into a FAIL that names the check, measured by time() so a stuck HAL
 * clock cannot hide it. It needs threads, so it runs for the threaded checks. */

#define CHECK_LIMIT_S 30   /* per check; generous for TSan and QEMU */

static gptps_mutex  *wd_m;
static gptps_cond   *wd_c;
static char          wd_check[160];   /* a copy: some names are built on the caller's stack */
static time_t        wd_deadline;
static int           wd_stop;

static void *watchdog(void *arg)
{
    (void)arg;
    gptps_mutex_lock(wd_m);
    while (!wd_stop) {
        if (wd_check[0] && time(NULL) > wd_deadline) {
            printf("  FAIL  %s\n        no progress for %d s: a lost wakeup, a broadcast that"
                   " woke too few, or a deadlock\n", wd_check, CHECK_LIMIT_S);
            printf("hal conformance: FAILED (hung)\n");
            fflush(stdout);
            exit(1);
        }
        gptps_cond_timedwait(wd_c, wd_m, 200);
    }
    gptps_mutex_unlock(wd_m);
    return NULL;
}

static void watch(const char *check)
{
    gptps_mutex_lock(wd_m);
    strncpy(wd_check, check, sizeof wd_check - 1);
    wd_deadline = time(NULL) + CHECK_LIMIT_S;
    gptps_mutex_unlock(wd_m);
}

/* --- hardware detection ------------------------------------------------------ */

static void check_detect(void)
{
    gptps_hwinfo hw;
    gptps_status st;
    memset(&hw, 0, sizeof hw);
    st = gptps_hal_detect(&hw);
    snprintf(why, sizeof why, "returned %d, cpu_count %u", (int)st, hw.cpu_count);
    expect(st == GPTPS_OK && hw.cpu_count >= 1, "detect: GPTPS_OK and cpu_count >= 1");
    snprintf(why, sizeof why, "returned %d", (int)gptps_hal_detect(NULL));
    expect(gptps_hal_detect(NULL) == GPTPS_E_INVAL, "detect: NULL is GPTPS_E_INVAL");
}

/* --- the clock --------------------------------------------------------------- */

static uint64_t g_step;   /* the clock's step, measured below; used for tolerances */

static void check_clock(int freestanding)
{
    uint64_t prev = now(), start = prev, back = 0, t;
    long i;
    time_t w0;

    for (i = 0; i < 200000; ++i) {
        t = now();
        if (t < prev) ++back;
        prev = t;
    }
    snprintf(why, sizeof why, "went backwards %llu times in 200000 reads", ull(back));
    expect(back == 0, "clock: never decreases");

    /* Its step: the smallest increase seen. Any step is allowed - Win32's is about
     * 16 ms - but a clock that never moves would leave every deadline and backoff
     * unfired. Bounded by time() so a stuck clock cannot stall this loop. */
    g_step = 0; prev = now(); start = prev; w0 = time(NULL);
    while (now() - start < 300 && time(NULL) - w0 < 3) {
        t = now();
        if (t > prev) { if (!g_step || t - prev < g_step) g_step = t - prev; prev = t; }
    }
    snprintf(why, sizeof why, "the clock did not move in 3 s of time()");
    expect(g_step > 0, "clock: advances");
    if (g_step > 0) printf("        (step %llu ms)\n", ull(g_step));

    /* Its rate, against the only clock hosted C99 guarantees: time(), whole seconds.
     * Two whole seconds of time() must read as 2000 ms, within 25% for scheduling
     * and boundary error. This is the check that catches the classic port bug: an
     * RTOS tick count passed off as milliseconds. */
    if (freestanding) {
        skip("clock: runs at the rate of real time",
             "--freestanding: the stub's clock counts reads, not milliseconds, so the"
             " core's deadlines and backoff are not real time on it");
        return;
    }
    {
        time_t a0 = time(NULL), a1;
        uint64_t m0, m1;
        while ((a1 = time(NULL)) == a0) nap(5);
        m0 = now();
        while (time(NULL) - a1 < 2) nap(5);
        m1 = now();
        snprintf(why, sizeof why, "two seconds of time() read as %llu ms", ull(m1 - m0));
        expect(m1 - m0 >= 1500 && m1 - m0 <= 2500, "clock: runs at the rate of real time");
    }
}

/* --- cancel flag, and the acquire/release pair (single thread) ---------------- */

static void check_flag_single(void)
{
    gptps_flag *f = gptps_flag_create(false), *g = gptps_flag_create(true);
    uint32_t word = 0;
    snprintf(why, sizeof why, "gptps_flag_create returned NULL");
    expect(f && g, "flag: create");
    if (!f || !g) { gptps_flag_destroy(f); gptps_flag_destroy(g); return; }
    snprintf(why, sizeof why, "create(false) read %d, create(true) read %d", (int)gptps_flag_get(f), (int)gptps_flag_get(g));
    expect(!gptps_flag_get(f) && gptps_flag_get(g), "flag: starts at its initial value");
    gptps_flag_set(f, true);
    snprintf(why, sizeof why, "read the wrong value back");
    {
        int ok = gptps_flag_get(f);
        gptps_flag_set(f, false);
        ok = ok && !gptps_flag_get(f);
        expect(ok, "flag: get returns the last value set");
    }
    gptps_flag_destroy(f);
    gptps_flag_destroy(g);

    gptps_hal_store_release_u32(&word, 0xA5A5A5A5u);
    snprintf(why, sizeof why, "stored 0xA5A5A5A5, loaded 0x%08lX", (unsigned long)gptps_hal_load_acquire_u32(&word));
    expect(gptps_hal_load_acquire_u32(&word) == 0xA5A5A5A5u, "acquire/release: a load returns the stored value");

    snprintf(why, sizeof why, "two calls on one thread returned different ids");
    expect(gptps_hal_thread_id() == gptps_hal_thread_id(), "thread_id: stable on one thread");
}

/* --- locks without a second thread ---------------------------------------------
 * What MANUAL mode needs of them: they exist, and a lock, an unlock, a signal and a
 * broadcast with nobody waiting all return. */

static void check_locks_single(void)
{
    gptps_mutex *m = gptps_mutex_create();
    gptps_cond  *c = gptps_cond_create();
    snprintf(why, sizeof why, "mutex %s, cond %s", m ? "ok" : "NULL", c ? "ok" : "NULL");
    expect(m && c, "mutex/cond: create");
    if (!m || !c) { gptps_mutex_destroy(m); gptps_cond_destroy(c); return; }
    gptps_mutex_lock(m);
    gptps_cond_signal(c);
    gptps_cond_broadcast(c);
    gptps_mutex_unlock(m);
    gptps_mutex_lock(m);         /* lock again after unlock: not a recursive lock */
    gptps_mutex_unlock(m);
    pass("mutex/cond: lock, unlock, and signal or broadcast with no waiter return");
    gptps_cond_destroy(c);
    gptps_mutex_destroy(m);
}

/* --- fork generation ------------------------------------------------------------ */

static void check_fork(int freestanding)
{
    uint64_t g0;
    gptps_hal_fork_guard_install();
    gptps_hal_fork_guard_install();                  /* idempotent */
    g0 = gptps_hal_fork_generation();
    snprintf(why, sizeof why, "changed between two reads with no fork");
    expect(gptps_hal_fork_generation() == g0, "fork: the generation holds still without a fork");
#if defined(HAVE_FORK)
    if (freestanding) {
        skip("fork: the generation changes in a forked child", "--freestanding: no fork on the target");
        return;
    }
    {
        int status = 0;
        pid_t pid;
        fflush(stdout);
        pid = fork();
        if (pid == 0) _exit(gptps_hal_fork_generation() != g0 ? 0 : 3);
        if (pid < 0) { skip("fork: the generation changes in a forked child", "fork() failed here"); return; }
        waitpid(pid, &status, 0);
        snprintf(why, sizeof why, "the child read the parent's generation: an engine created before the fork"
                     " would be used in the child instead of refused");
        expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, "fork: the generation changes in a forked child");
        snprintf(why, sizeof why, "the parent's generation changed");
        expect(gptps_hal_fork_generation() == g0, "fork: the parent's generation does not change");
    }
#else
    (void)freestanding;
    skip("fork: the generation changes in a forked child", "no fork() on this platform");
#endif
}

/* --- dynamic loading ------------------------------------------------------------- */

static void check_dl(int freestanding, const char *module)
{
    gptps_dl *h;
    if (freestanding) { skip("dl: open, sym, close", "--freestanding: no dynamic loading on the target"); return; }
    snprintf(why, sizeof why, "opening a path that does not exist returned a handle");
    h = gptps_dl_open("gptps-hal-conformance-no-such-module.so");
    expect(h == NULL, "dl: open of a missing file returns NULL");
    if (h) gptps_dl_close(h);
    if (!module) { skip("dl: open, sym, close", "no module path given"); return; }
    h = gptps_dl_open(module);
    snprintf(why, sizeof why, "could not open %s", module);
    expect(h != NULL, "dl: open of a real module");
    if (!h) return;
    snprintf(why, sizeof why, "gptps_addon_init not found");
    expect(gptps_dl_sym(h, "gptps_addon_init") != NULL, "dl: sym finds an exported symbol");
    snprintf(why, sizeof why, "a symbol that does not exist was found");
    expect(gptps_dl_sym(h, "gptps_hal_conformance_no_such_symbol") == NULL, "dl: sym of a missing symbol is NULL");
    gptps_dl_close(h);
    h = gptps_dl_open(module);                       /* release: the wrapper goes, the mapping stays */
    if (h) gptps_dl_release(h);
    pass("dl: close, and release, return (LeakSanitizer checks the wrapper is freed)");
}

/* --- atomic file replace (settings save) --------------------------------------------- */

static int write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fputs(text, f);
    return fclose(f) == 0;
}

static int file_is(const char *path, const char *text)
{
    char buf[64];
    size_t n;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strcmp(buf, text) == 0;
}

static void check_replace(int freestanding)
{
    const char *fin = "hal_conformance_final.tmp", *tmp = "hal_conformance_next.tmp";
    FILE *gone;
    if (freestanding) { skip("atomic_replace", "--freestanding: no filesystem; settings save is unavailable there"); return; }
    remove(fin); remove(tmp);
    if (!write_file(tmp, "first")) { skip("atomic_replace", "cannot write in the working directory"); return; }
    snprintf(why, sizeof why, "the replace failed, or the target does not hold the new contents");
    expect(gptps_hal_atomic_replace(tmp, fin) == GPTPS_OK && file_is(fin, "first"),
           "atomic_replace: onto a target that does not exist");
    write_file(tmp, "second");
    snprintf(why, sizeof why, "the replace failed, or the target still holds the old contents");
    expect(gptps_hal_atomic_replace(tmp, fin) == GPTPS_OK && file_is(fin, "second"),
           "atomic_replace: over an existing target");
    gone = fopen(tmp, "rb");
    snprintf(why, sizeof why, "the source still exists after the replace");
    expect(gone == NULL, "atomic_replace: the source is gone");
    if (gone) fclose(gone);
    snprintf(why, sizeof why, "replacing from a missing source returned %d", (int)gptps_hal_atomic_replace(tmp, fin));
    expect(gptps_hal_atomic_replace(tmp, fin) == GPTPS_E_IO, "atomic_replace: a missing source is GPTPS_E_IO");
    remove(fin); remove(tmp);
}

/* ===== THREADED-mode checks ===================================================== */

static void *noop(void *arg) { (void)arg; return NULL; }

/* start/join: every thread runs once, with its argument, and its writes are seen
 * after the join. */
static void *store_one(void *arg) { *(volatile int *)arg = 1; return NULL; }

static void check_start_join(void)
{
    enum { N = 16 };
    gptps_thread *th[N];
    int done[N], i, ok = 1;
    watch("thread: start runs fn(arg); join waits for it and frees it");
    for (i = 0; i < N; ++i) done[i] = 0;
    for (i = 0; i < N; ++i) if (!(th[i] = gptps_thread_start(store_one, &done[i]))) ok = 0;
    for (i = 0; i < N; ++i) if (th[i]) gptps_thread_join(th[i]);
    for (i = 0; i < N; ++i) if (done[i] != 1) ok = 0;
    snprintf(why, sizeof why, "a thread failed to start, or its write was not seen after its join");
    expect(ok, "thread: start runs fn(arg); join waits for it and frees it");
}

/* thread_id: distinct among threads that are alive at the same time. */
typedef struct { gptps_mutex *m; gptps_cond *c; int arrived, open; uint64_t id[8]; int stable[8]; } id_gate;
typedef struct { id_gate *g; int slot; } id_arg;

static void *record_id(void *arg)
{
    id_arg *a = (id_arg *)arg;
    id_gate *g = a->g;
    uint64_t id = gptps_hal_thread_id();
    gptps_mutex_lock(g->m);
    g->id[a->slot] = id;
    g->stable[a->slot] = (gptps_hal_thread_id() == id);
    ++g->arrived;
    gptps_cond_broadcast(g->c);
    while (!g->open) gptps_cond_wait(g->c, g->m);   /* stay alive until every id is in */
    gptps_mutex_unlock(g->m);
    return NULL;
}

static void check_thread_ids(void)
{
    id_gate g;
    id_arg a[8];
    gptps_thread *th[8];
    int i, j, started = 0, ok = 1;
    uint64_t me = gptps_hal_thread_id();
    watch("thread_id: distinct among live threads");
    memset(&g, 0, sizeof g);
    g.m = gptps_mutex_create(); g.c = gptps_cond_create();
    for (i = 0; i < 8; ++i) {
        a[i].g = &g; a[i].slot = i;
        if ((th[i] = gptps_thread_start(record_id, &a[i])) != NULL) ++started; else ok = 0;
    }
    gptps_mutex_lock(g.m);
    while (g.arrived < started) gptps_cond_wait(g.c, g.m);
    for (i = 0; i < 8 && ok; ++i) {
        if (!g.stable[i] || g.id[i] == me) ok = 0;
        for (j = i + 1; j < 8; ++j) if (g.id[i] == g.id[j]) ok = 0;
    }
    g.open = 1;
    gptps_cond_broadcast(g.c);
    gptps_mutex_unlock(g.m);
    for (i = 0; i < 8; ++i) if (th[i]) gptps_thread_join(th[i]);
    gptps_cond_destroy(g.c); gptps_mutex_destroy(g.m);
    snprintf(why, sizeof why, "a thread failed to start, two live threads shared an id, a thread's id"
                              " changed, or one matched the main thread's");
    expect(ok, "thread_id: distinct among live threads, stable within each");
}

/* mutex: mutual exclusion, and unlock-to-lock ordering (ThreadSanitizer sees the
 * second; the exact count shows the first). */
static gptps_mutex *x_m;
static long         x_count;
enum { X_THREADS = 8, X_ITERS = 20000 };

static void *bump(void *arg)
{
    int i;
    (void)arg;
    for (i = 0; i < X_ITERS; ++i) { gptps_mutex_lock(x_m); ++x_count; gptps_mutex_unlock(x_m); }
    return NULL;
}

static void check_exclusion(void)
{
    gptps_thread *th[X_THREADS];
    int i, started = 0;
    watch("mutex: mutual exclusion");
    x_m = gptps_mutex_create(); x_count = 0;
    for (i = 0; i < X_THREADS; ++i) if ((th[i] = gptps_thread_start(bump, NULL)) != NULL) ++started;
    for (i = 0; i < X_THREADS; ++i) if (th[i]) gptps_thread_join(th[i]);
    gptps_mutex_destroy(x_m);
    snprintf(why, sizeof why, "%d of %d threads started; %d x %d increments under the mutex counted %ld",
             started, X_THREADS, started, X_ITERS, x_count);
    expect(started == X_THREADS && x_count == (long)X_THREADS * X_ITERS, "mutex: mutual exclusion");
}

/* cond: a hand-off in both directions, 20000 times, with signal - the worker pool's
 * pattern. A signal that can be lost while the other side is about to wait shows
 * up as a hang, which the watchdog reports. */
typedef struct { gptps_mutex *m; gptps_cond *to_a, *to_b; int turn; long rounds; } pingpong;

static void *pong(void *arg)
{
    pingpong *p = (pingpong *)arg;
    long r;
    gptps_mutex_lock(p->m);
    for (r = 0; r < p->rounds; ++r) {
        while (p->turn != 1) gptps_cond_wait(p->to_b, p->m);
        p->turn = 0;
        gptps_cond_signal(p->to_a);
    }
    gptps_mutex_unlock(p->m);
    return NULL;
}

static void check_handoff(void)
{
    pingpong p;
    gptps_thread *t;
    long r;
    watch("cond: wait/signal hand-off loses no wakeup");
    p.m = gptps_mutex_create(); p.to_a = gptps_cond_create(); p.to_b = gptps_cond_create();
    p.turn = 0; p.rounds = 20000;
    t = gptps_thread_start(pong, &p);
    gptps_mutex_lock(p.m);
    for (r = 0; r < p.rounds && t; ++r) {
        p.turn = 1;
        gptps_cond_signal(p.to_b);
        while (p.turn != 0) gptps_cond_wait(p.to_a, p.m);
    }
    gptps_mutex_unlock(p.m);
    if (t) gptps_thread_join(t);
    gptps_cond_destroy(p.to_a); gptps_cond_destroy(p.to_b); gptps_mutex_destroy(p.m);
    snprintf(why, sizeof why, "the peer thread did not start");
    expect(t != NULL, "cond: wait/signal hand-off loses no wakeup (20000 round trips)");
}

/* cond: one broadcast wakes every waiter. Each waiter holds the mutex from
 * announcing itself until its wait releases it, so all are waiting when the
 * broadcast is sent; one that is not woken hangs the check. */
typedef struct { gptps_mutex *m; gptps_cond *c, *back; int ready, go, woke; } crowd;

static void *crowd_wait(void *arg)
{
    crowd *k = (crowd *)arg;
    gptps_mutex_lock(k->m);
    ++k->ready;
    gptps_cond_signal(k->back);
    while (!k->go) gptps_cond_wait(k->c, k->m);
    ++k->woke;
    gptps_cond_signal(k->back);
    gptps_mutex_unlock(k->m);
    return NULL;
}

static void check_broadcast(void)
{
    enum { N = 8 };
    crowd k;
    gptps_thread *th[N];
    int i, started = 0;
    watch("cond: broadcast wakes every waiter");
    memset(&k, 0, sizeof k);
    k.m = gptps_mutex_create(); k.c = gptps_cond_create(); k.back = gptps_cond_create();
    for (i = 0; i < N; ++i) if ((th[i] = gptps_thread_start(crowd_wait, &k)) != NULL) ++started;
    gptps_mutex_lock(k.m);
    while (k.ready < started) gptps_cond_wait(k.back, k.m);
    k.go = 1;
    gptps_cond_broadcast(k.c);                       /* once */
    while (k.woke < started) gptps_cond_wait(k.back, k.m);
    gptps_mutex_unlock(k.m);
    for (i = 0; i < N; ++i) if (th[i]) gptps_thread_join(th[i]);
    gptps_cond_destroy(k.c); gptps_cond_destroy(k.back); gptps_mutex_destroy(k.m);
    snprintf(why, sizeof why, "only %d of %d waiter threads started", started, N);
    expect(started == N, "cond: one broadcast wakes every waiter (8)");
}

/* cond_timedwait with nobody signalling: it times out, near its time. Spurious
 * returns are allowed, so it is used as the core uses it - in a loop until the
 * clock says the time is up - and held to two things: one call never oversleeps
 * by seconds, and the loop does not spin (a wait that returns at once nearly
 * every time would make the dispatcher spin). */
static void check_timeout(uint64_t ms)
{
    gptps_mutex *m = gptps_mutex_create();
    gptps_cond  *c = gptps_cond_create();
    uint64_t t0, first, end;
    long calls = 0;
    char what[96];
    snprintf(what, sizeof what, "cond_timedwait(%llu ms): times out, without oversleeping or spinning", ull(ms));
    watch(what);
    gptps_mutex_lock(m);
    t0 = now();
    gptps_cond_timedwait(c, m, ms); ++calls;
    first = now() - t0;
    while (now() - t0 < ms) { gptps_cond_timedwait(c, m, ms - (now() - t0)); ++calls; }
    end = now() - t0;
    gptps_mutex_unlock(m);
    gptps_cond_destroy(c); gptps_mutex_destroy(m);
    snprintf(why, sizeof why, "first call took %llu ms, the loop %llu ms in %ld calls (clock step %llu ms)",
            ull(first), ull(end), calls, ull(g_step));
    expect(first <= ms + 2000 && calls <= 64, what);
}

/* cond_timedwait wakes on a signal long before its timeout - for every timeout the
 * core can pass. Its only timed wait sleeps until the next deadline or backoff, and
 * a task's deadline is timeout_seconds * 1000 ms away: up to 4294967295000 ms. A HAL
 * that adds that to the time of day without clamping can overflow its time_t and
 * return at once, every time - and the dispatcher spins. */
typedef struct { gptps_mutex *m; gptps_cond *c; int flag; uint64_t timeout; long calls; uint64_t woke_at; } sleeper;

static void *sleep_until_flag(void *arg)
{
    sleeper *s = (sleeper *)arg;
    gptps_mutex_lock(s->m);
    while (!s->flag) { gptps_cond_timedwait(s->c, s->m, s->timeout); ++s->calls; }
    s->woke_at = now();
    gptps_mutex_unlock(s->m);
    return NULL;
}

static void check_wake(uint64_t timeout, const char *what)
{
    sleeper s;
    gptps_thread *t;
    uint64_t sent;
    watch(what);
    memset(&s, 0, sizeof s);
    s.m = gptps_mutex_create(); s.c = gptps_cond_create(); s.timeout = timeout;
    t = gptps_thread_start(sleep_until_flag, &s);
    nap(200);
    gptps_mutex_lock(s.m);
    s.flag = 1;
    sent = now();
    gptps_cond_signal(s.c);
    gptps_mutex_unlock(s.m);
    if (t) gptps_thread_join(t);
    gptps_cond_destroy(s.c); gptps_mutex_destroy(s.m);
    snprintf(why, sizeof why, "woke %lld ms after the signal; returned %ld times in the 200 ms before it",
            (long long)(s.woke_at - sent), s.calls);
    expect(t && s.woke_at - sent <= 5000 && s.calls <= 64, what);
}

/* cond_wait blocks until it is signalled. It may wake spuriously, but not as a
 * rule: a wait that returns at once every time turns the worker pool's idle wait
 * into a spin. */
static void *wait_until_flag(void *arg)
{
    sleeper *s = (sleeper *)arg;
    gptps_mutex_lock(s->m);
    while (!s->flag) { gptps_cond_wait(s->c, s->m); ++s->calls; }
    s->woke_at = now();
    gptps_mutex_unlock(s->m);
    return NULL;
}

static void check_wait_blocks(void)
{
    sleeper s;
    gptps_thread *t;
    uint64_t sent;
    const char *what = "cond_wait: blocks until a signal, and does not spin";
    watch(what);
    memset(&s, 0, sizeof s);
    s.m = gptps_mutex_create(); s.c = gptps_cond_create();
    t = gptps_thread_start(wait_until_flag, &s);
    nap(200);
    gptps_mutex_lock(s.m);
    s.flag = 1;
    sent = now();
    gptps_cond_signal(s.c);
    gptps_mutex_unlock(s.m);
    if (t) gptps_thread_join(t);
    gptps_cond_destroy(s.c); gptps_mutex_destroy(s.m);
    snprintf(why, sizeof why, "woke %lld ms after the signal; returned %ld times in the 200 ms before it",
             (long long)(s.woke_at - sent), s.calls);
    expect(t && s.woke_at - sent <= 5000 && s.calls <= 64, what);
}

/* acquire/release: message passing. The writer fills a slot, then publishes its
 * index with a release store; the reader loads the index with acquire and must
 * then see every slot up to it. On x86 this cannot fail whatever the HAL does -
 * the hardware orders the plain accesses itself - so it counts on the arm64 runner
 * and under ThreadSanitizer, which reports a HAL that uses plain accesses. */
enum { MP_N = 100000 };
static uint32_t mp_data[MP_N];
static uint32_t mp_published;

static uint32_t mp_value(uint32_t i) { return i * 2654435761u + 1u; }

static void *mp_writer(void *arg)
{
    uint32_t i;
    (void)arg;
    for (i = 0; i < MP_N; ++i) {
        mp_data[i] = mp_value(i);
        gptps_hal_store_release_u32(&mp_published, i + 1);
    }
    return NULL;
}

static void check_message_passing(void)
{
    gptps_thread *t;
    uint32_t seen = 0, got, i, bad = 0;
    watch("acquire/release: a released write is seen after the acquiring load");
    mp_published = 0;
    t = gptps_thread_start(mp_writer, NULL);
    while (seen < MP_N && t) {
        got = gptps_hal_load_acquire_u32(&mp_published);
        for (i = seen; i < got; ++i) if (mp_data[i] != mp_value(i)) ++bad;
        seen = got;
    }
    if (t) gptps_thread_join(t);
    snprintf(why, sizeof why, "%lu of %d slots read stale after their index was published", (unsigned long)bad, MP_N);
    expect(t && bad == 0, "acquire/release: a released write is seen after the acquiring load");
}

/* cancel flag across threads: the watchdog sets it, the task polls it. */
typedef struct { gptps_flag *f; uint64_t seen_at; } poller;

static void *poll_flag(void *arg)
{
    poller *p = (poller *)arg;
    while (!gptps_flag_get(p->f)) nap(1);
    p->seen_at = now();
    return NULL;
}

static void check_flag_threads(void)
{
    poller p;
    gptps_thread *t;
    uint64_t set_at;
    watch("flag: a set on one thread is seen by a poll on another");
    p.f = gptps_flag_create(false); p.seen_at = 0;
    t = gptps_thread_start(poll_flag, &p);
    nap(50);
    set_at = now();
    gptps_flag_set(p.f, true);
    if (t) gptps_thread_join(t);
    gptps_flag_destroy(p.f);
    snprintf(why, sizeof why, "seen %lld ms after it was set", (long long)(p.seen_at - set_at));
    expect(t && p.seen_at - set_at <= 5000, "flag: a set on one thread is seen by a poll on another");
}

/* The clock across threads: a reading taken after another thread's (ordered by the
 * mutex) is never smaller - per-CPU clocks that disagree would break this. */
typedef struct { gptps_mutex *m; gptps_cond *c; int turn; uint64_t last; long back; long rounds; } relay;

static void *relay_peer(void *arg)
{
    relay *r = (relay *)arg;
    long i;
    gptps_mutex_lock(r->m);
    for (i = 0; i < r->rounds; ++i) {
        uint64_t t;
        while (r->turn != 1) gptps_cond_wait(r->c, r->m);
        t = now();
        if (t < r->last) ++r->back;
        r->last = t;
        r->turn = 0;
        gptps_cond_broadcast(r->c);
    }
    gptps_mutex_unlock(r->m);
    return NULL;
}

static void check_clock_threads(void)
{
    relay r;
    gptps_thread *t;
    long i;
    watch("clock: never decreases across threads");
    memset(&r, 0, sizeof r);
    r.m = gptps_mutex_create(); r.c = gptps_cond_create(); r.rounds = 2000;
    t = gptps_thread_start(relay_peer, &r);
    gptps_mutex_lock(r.m);
    for (i = 0; i < r.rounds && t; ++i) {
        uint64_t x = now();
        if (x < r.last) ++r.back;
        r.last = x;
        r.turn = 1;
        gptps_cond_broadcast(r.c);
        while (r.turn != 0) gptps_cond_wait(r.c, r.m);
    }
    gptps_mutex_unlock(r.m);
    if (t) gptps_thread_join(t);
    gptps_cond_destroy(r.c); gptps_mutex_destroy(r.m);
    snprintf(why, sizeof why, "a reading was smaller than one taken before it on the other thread, %ld times", r.back);
    expect(t && r.back == 0, "clock: never decreases across threads (2000 hand-offs)");
}

int main(int argc, char **argv)
{
    int freestanding = 0, dl_only = 0, i;
    const char *module = NULL;
    gptps_thread *probe, *wd;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--freestanding") == 0) freestanding = 1;
        else if (strcmp(argv[i], "--dl-only") == 0) dl_only = 1;
        else module = argv[i];
    }

    printf("HAL conformance (include/gptps_hal.h; clauses in docs/HAL.md)\n");
    if (dl_only) {
        check_dl(freestanding, module);
        printf("hal conformance (dynamic loading): %d passed, %d failed, %d skipped - %s\n",
               n_pass, n_fail, n_skip, n_fail ? "FAILED" : "OK");
        return n_fail ? 1 : 0;
    }
    check_detect();
    check_clock(freestanding);
    check_flag_single();
    check_locks_single();
    check_fork(freestanding);
    if (module) check_dl(freestanding, module);
    else skip("dl: open, sym, close", "run with --dl-only <module> (CTest: hal_conformance_dl)");
    check_replace(freestanding);

    /* Can it run THREADED mode at all? A HAL that cannot start a thread runs the
     * engine in MANUAL mode only - legitimately, as the freestanding stub does. */
    probe = gptps_thread_start(noop, NULL);
    if (!probe) {
        skip("THREADED mode", "gptps_thread_start returned NULL: this HAL runs the engine in"
             " MANUAL mode only, which needs none of the checks below");
    } else {
        gptps_thread_join(probe);
        wd_m = gptps_mutex_create(); wd_c = gptps_cond_create();
        wd = gptps_thread_start(watchdog, NULL);
        if (!wd_m || !wd_c || !wd) {
            fail("THREADED mode", "could not start the watchdog thread");
        } else {
            /* Each check uses only primitives an earlier one verified, so the first
             * FAIL names what is broken: threads, then the mutex, then signal and
             * wait, then broadcast - which the later checks use. */
            check_start_join();
            check_exclusion();
            check_handoff();
            check_broadcast();
            check_clock_threads();                   /* the checks after it compare readings across threads */
            check_wait_blocks();
            check_timeout(0);
            check_timeout(1);
            check_timeout(25);
            check_timeout(150);
            check_wake(60000, "cond_timedwait(60 s): a signal wakes it");
            check_wake(4294967295000ull, "cond_timedwait(4294967295000 ms, the core's longest): a signal wakes it, and it does not spin");
            check_wake(UINT64_MAX, "cond_timedwait(UINT64_MAX): a signal wakes it, and it does not spin");
            check_thread_ids();
            check_message_passing();
            check_flag_threads();
            gptps_mutex_lock(wd_m); wd_stop = 1; gptps_cond_signal(wd_c); gptps_mutex_unlock(wd_m);
            gptps_thread_join(wd);
        }
        gptps_cond_destroy(wd_c); gptps_mutex_destroy(wd_m);
    }

    printf("hal conformance: %d passed, %d failed, %d skipped - %s\n",
           n_pass, n_fail, n_skip, n_fail ? "FAILED" : "OK");
    return n_fail ? 1 : 0;
}
