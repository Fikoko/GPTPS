/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * hal_sim.c - a deterministic simulation HAL: the same seed, the same interleaving.
 *
 * tests/hal_chaos.c shakes the core's assumptions at random, and a failure it finds
 * may never come back. This HAL makes the interleaving of threads a function of a
 * seed, so a failure is a number that replays, and a search over seeds is a search
 * over schedules:
 *
 *   - Every engine thread is a real OS thread, but only one runs at a time. The
 *     running thread holds a baton - a real mutex, plus a condition variable per
 *     thread that it waits on until the baton is handed to it.
 *   - Every HAL call is a scheduling point: mutex lock and unlock, a condition wait,
 *     signal, broadcast and timed wait, the acquire load and the release store,
 *     thread start and join, the clock. There a PRNG seeded from GPTPS_SIM_SEED
 *     picks the thread that runs next, from those that can.
 *   - Time is virtual. When every thread waits, the clock jumps to the earliest
 *     timed wait's deadline. When the threads that can run only poll - the clock,
 *     an acquire load - it creeps, a ms per SIM_SPIN polls. And work costs a ms per
 *     SIM_TICK scheduling points, so a thread that waits for time while it locks and
 *     unlocks still sees it pass. A 30 s shutdown grace takes no real time, and a
 *     timeout fires at the same point of the schedule on every run.
 *   - Every thread waiting, and none with a timeout, is a deadlock. It is reported
 *     with each thread's state and last call, and the process aborts instead of
 *     hanging. Virtual time running on for GPTPS_SIM_HORIZON_S seconds with no
 *     thread waking another is reported too, once, as a possible hang - a test that
 *     naps until time() says so looks the same - and the run goes on.
 *   - At exit it prints the seed and a hash of every decision it made. The same
 *     seed prints the same hash: that is the determinism check. A run in which
 *     something outside the simulation may have decided - a child process, real
 *     time while paced, a thread or a lock it did not see - says so on that line.
 *
 * Build the suite on it, then run a test with a seed:
 *
 *   cmake -S . -B build-sim -DGPTPS_HAL_SOURCE=$PWD/tests/hal_sim.c
 *   cmake --build build-sim -j
 *   GPTPS_SIM_SEED=5 GPTPS_SIM_CPUS=4 build-sim/test_orch
 *
 * Not every test runs on it as it is: some need real time for their child processes,
 * block in a read() the simulation cannot see, or only time the clock. CI's hal_sim
 * job (.github/workflows/ci.yml) runs the rest on three seeds, and the tests whose
 * children need real time paced to it.
 * docs/HAL.md has the job's two commands, in a loop over seeds, and says which tests
 * each one leaves out, and why.
 *
 * Beyond the HAL. The add-ons and some tests synchronize with pthreads of their own
 * (addons/addon_compat.h), start threads with pthread_create, sleep with nanosleep
 * and read clock_gettime. A thread that blocked in one of those while it held the
 * baton would stall every other thread, so this file defines those functions too,
 * and the linker binds the program's calls to them instead of libc's. They run
 * through the same scheduler: a pthread mutex is still taken for real - with
 * trylock, once the scheduler finds it free - so its type and its memory ordering
 * stay libc's; a pthread condition variable is simulated outright; CLOCK_MONOTONIC
 * reads the virtual clock and CLOCK_REALTIME a fixed date plus it, so a deadline a
 * caller builds from either still replays. libc's own functions are reached through
 * dlsym(RTLD_NEXT). time() is left alone: it is the real-time reference the
 * conformance test checks the clock against.
 *
 * fork. A forked child has one thread and its own process. The simulation steps
 * aside there: every call goes to the POSIX HAL and to libc, as without this file,
 * and the HAL clock goes on from the virtual time of the fork at the rate of real
 * time. The parent's clock keeps to real time from its first fork on, as
 * GPTPS_SIM_PACE makes it do from the start: the child runs on real time, and so
 * must a deadline the parent holds it to, or a grace it waits out for it.
 *
 * What it cannot see. A thread that blocks or spins outside everything above - a
 * read() on a socket another thread of the process has yet to write, a loop on a
 * plain variable with no call in it - keeps the baton, and nothing else runs. A
 * watchdog prints the threads after GPTPS_SIM_STALL_S seconds without a scheduling
 * point. A deadline that must keep to real time before the first fork - the
 * conformance test's check of the clock's rate - needs GPTPS_SIM_PACE.
 *
 * Environment:
 *   GPTPS_SIM_SEED=<n>       the schedule (default 1).
 *   GPTPS_SIM_CPUS=<n>       the CPU count gptps_hal_detect reports; default the
 *                            machine's, as the POSIX HAL detects it. The worker pool
 *                            is sized from it, so a seed replays only on the same count.
 *   GPTPS_SIM_PACE=1         keep the clock to real time from the start: a jump
 *                            waits for it, every scheduling point catches the clock
 *                            up with it, and a thread that held the baton for
 *                            SIM_SLICE ms of it lets another go first. A run is paced
 *                            from its first fork on in any case.
 *   GPTPS_SIM_SWITCH=<n>     switch threads at one scheduling point in n (default
 *                            1, 4, 16 or 64, chosen by the seed).
 *   GPTPS_SIM_SPURIOUS=<n>   take two freedoms of the contract one time in n: a wait
 *                            that returns with no signal, a signal that wakes every
 *                            waiter. 0 is never; the default (0, 16 or 64) is chosen
 *                            by the seed.
 *   GPTPS_SIM_HORIZON_S=<n>  virtual seconds with no thread waking another before a
 *                            possible hang is reported (default 600; 0 = never).
 *   GPTPS_SIM_STALL_S=<n>    real seconds without a scheduling point before the
 *                            watchdog prints the threads (default 10; 0 = off).
 *   GPTPS_SIM_VERBOSE=1      log every thread switch and time jump to stderr.
 *
 * POSIX with glibc or musl, GCC or Clang: it relies on symbol interposition, __thread
 * and RTLD_NEXT. It is a test tool, like hal_chaos.c; the core is the same C on
 * every platform, so one platform is enough to search its schedules.
 */
#define gptps_mutex_lock            sim_posix_mutex_lock
#define gptps_mutex_unlock          sim_posix_mutex_unlock
#define gptps_mutex_destroy         sim_posix_mutex_destroy
#define gptps_cond_destroy          sim_posix_cond_destroy
#define gptps_cond_wait             sim_posix_cond_wait
#define gptps_cond_timedwait        sim_posix_cond_timedwait
#define gptps_cond_signal           sim_posix_cond_signal
#define gptps_cond_broadcast        sim_posix_cond_broadcast
#define gptps_thread_start          sim_posix_thread_start
#define gptps_thread_join           sim_posix_thread_join
#define gptps_hal_thread_id         sim_posix_thread_id
#define gptps_hal_monotonic_ms      sim_posix_monotonic_ms
#define gptps_hal_load_acquire_u32  sim_posix_load_acquire_u32
#define gptps_hal_store_release_u32 sim_posix_store_release_u32
#define gptps_hal_detect            sim_posix_detect
#include "../src/hal_posix.c"     /* first: its feature macros precede every system header */
#undef gptps_mutex_lock
#undef gptps_mutex_unlock
#undef gptps_mutex_destroy
#undef gptps_cond_destroy
#undef gptps_cond_wait
#undef gptps_cond_timedwait
#undef gptps_cond_signal
#undef gptps_cond_broadcast
#undef gptps_thread_start
#undef gptps_thread_join
#undef gptps_hal_thread_id
#undef gptps_hal_monotonic_ms
#undef gptps_hal_load_acquire_u32
#undef gptps_hal_store_release_u32
#undef gptps_hal_detect

#include <errno.h>
#include <limits.h>
#include <sched.h>

/* gptps_hal.h was read with the names above renamed, so declare the public ones. */
void          gptps_mutex_lock(gptps_mutex *m);
void          gptps_mutex_unlock(gptps_mutex *m);
void          gptps_mutex_destroy(gptps_mutex *m);
void          gptps_cond_destroy(gptps_cond *c);
void          gptps_cond_wait(gptps_cond *c, gptps_mutex *m);
void          gptps_cond_timedwait(gptps_cond *c, gptps_mutex *m, uint64_t ms);
void          gptps_cond_signal(gptps_cond *c);
void          gptps_cond_broadcast(gptps_cond *c);
gptps_thread *gptps_thread_start(gptps_thread_fn fn, void *arg);
void          gptps_thread_join(gptps_thread *t);
uint64_t      gptps_hal_thread_id(void);
uint64_t      gptps_hal_monotonic_ms(void);
uint32_t      gptps_hal_load_acquire_u32(const uint32_t *p);
void          gptps_hal_store_release_u32(uint32_t *p, uint32_t v);
gptps_status  gptps_hal_detect(gptps_hwinfo *out);

#define SIM_NEVER    UINT64_MAX
#define SIM_T0       1000000u              /* the clock's first reading: never 0, which the core reads as "none" */
#define SIM_EPOCH_MS 1700000000000ull      /* CLOCK_REALTIME reads this plus the virtual clock */
#define SIM_SPIN     32u                   /* polls in a row, with nothing else, that make a thread a spinner */
#define SIM_TICK     1000u                 /* scheduling points to a ms of work: a HAL call costs about a us */
#define SIM_SLICE    10u                   /* paced: real ms a thread may hold the baton before others go first */

/* What a thread is doing, as the scheduler sees it. */
enum { S_RUN, S_MUTEX, S_COND, S_SLEEP, S_JOIN, S_DONE };

/* Scheduling points, as the trace hash records them. */
enum { OP_LOCK = 1, OP_UNLOCK, OP_WAIT, OP_SIGNAL, OP_BROADCAST, OP_CLOCK, OP_LOAD, OP_STORE,
       OP_START, OP_JOIN, OP_EXIT, OP_SLEEP, OP_YIELD, OP_TRYLOCK };

enum { SIM_OFF, SIM_ON, SIM_PASS };       /* SIM_PASS: a forked child, where everything is real */

typedef struct sim_thread sim_thread;
struct sim_thread {
    unsigned        id;          /* 0 is the first thread that called in: the program's main */
    int             state;
    const void     *on;          /* S_MUTEX: the mutex; S_COND: the cond; S_JOIN: the thread */
    const void     *relock;      /* S_COND: the mutex the wait takes back */
    int             raw;         /* that mutex is a pthread_mutex_t, which is also taken for real */
    int             timedout;    /* how the last cond wait ended */
    uint64_t        wake_at;     /* S_COND / S_SLEEP: when time ends the wait; SIM_NEVER if never */
    unsigned        polls;       /* clock reads and acquire loads in a row, with nothing else */
    const char     *call;        /* the last call it made, and where from: the deadlock report */
    void           *where;
    int             err;         /* errno, kept across a call */
    pthread_cond_t  go;          /* it waits here until it is the running thread */
    pthread_t       pt;
    void         *(*fn)(void *);
    void           *arg, *ret;
    int             detached;
    sim_thread     *next;        /* the live threads in id order; the exited ones not yet joined */
};

typedef struct { const void *m; sim_thread *owner; unsigned depth; } sim_hold;

static struct {
    pthread_mutex_t g;           /* guards everything below; the baton holder takes it in every call */
    int             mode;
    sim_thread     *live, *live_tail, *dead, *running;
    unsigned        nlive, next_id;
    sim_thread    **cand;        /* scratch for the scheduler: the threads that can run */
    unsigned        ncand;
    sim_hold       *holds;       /* every mutex held, simulated or pthread, and by whom */
    size_t          nholds, capholds;
    uint64_t        now;         /* the virtual clock, ms */
    uint64_t        progress_at; /* virtual time when a thread last woke or freed another */
    uint64_t        told_at;     /* the progress_at a HANG? report was last made for */
    uint64_t        seed, rng, trace, steps, switches;
    unsigned        ticks;       /* scheduling points since the clock last moved for work */
    unsigned        switch_one_in, spurious_one_in, cpus, stall_s, stray, outside;
    unsigned        forks;       /* child processes forked: they run on real time */
    int             realtime;    /* real time has moved the paced clock */
    uint64_t        horizon;
    int             pace, pacing, verbose;
    uint64_t        pace_v, pace_r;  /* paced: virtual time pace_v was real time pace_r */
    uint64_t        fork_v, fork_r;
    pid_t           pid;
} sim;                           /* sim.g is initialised by sim_init, before any use */

static pthread_once_t        sim_once = PTHREAD_ONCE_INIT;
static __thread sim_thread  *sim_me;      /* this thread's record; NULL until it first calls in */
static sim_thread            sim_gone;    /* the record of a thread that has finished: nothing it calls is simulated */

/* libc's own functions, which this file's definitions stand in front of. */
static int  (*real_mutex_lock)(pthread_mutex_t *);
static int  (*real_mutex_trylock)(pthread_mutex_t *);
static int  (*real_mutex_unlock)(pthread_mutex_t *);
static int  (*real_cond_wait)(pthread_cond_t *, pthread_mutex_t *);
static int  (*real_cond_timedwait)(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
static int  (*real_cond_signal)(pthread_cond_t *);
static int  (*real_cond_broadcast)(pthread_cond_t *);
static int  (*real_create)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
static int  (*real_join)(pthread_t, void **);
static int  (*real_detach)(pthread_t);
static int  (*real_clock_gettime)(clockid_t, struct timespec *);
static int  (*real_clock_nanosleep)(clockid_t, int, const struct timespec *, struct timespec *);
static int  (*real_nanosleep)(const struct timespec *, struct timespec *);
static int  (*real_sched_yield)(void);

static void *sim_sym(const char *name)
{
    void *p = dlsym(RTLD_NEXT, name);
    if (!p) { fprintf(stderr, "hal_sim: cannot find libc's %s\n", name); abort(); }
    return p;
}
/* Through memcpy: ISO C has no conversion from an object pointer to a function pointer. */
#define SIM_REAL(var, name) do { void *p_ = sim_sym(name); memcpy(&(var), &p_, sizeof p_); } while (0)

static uint64_t sim_real_ms(void)
{
    struct timespec ts;
    real_clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t sim_add(uint64_t a, uint64_t b) { return b > SIM_NEVER - a ? SIM_NEVER : a + b; }

/* splitmix64: one stream for every choice, drawn in program order - which is the
 * same order on every run, because only one thread runs at a time. */
static uint64_t sim_next(void)
{
    uint64_t z = (sim.rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static unsigned sim_below(unsigned n) { return n > 1 ? (unsigned)(sim_next() % n) : 0u; }
static int      sim_one_in(unsigned n) { return n && sim_below(n) == 0; }

/* The trace hash: FNV-1a over every decision. */
static void sim_mix(uint64_t v)
{
    int i;
    for (i = 0; i < 8; ++i) { sim.trace ^= (v >> (i * 8)) & 0xFFu; sim.trace *= 0x100000001B3ull; }
}

/* --- the held-mutex table -------------------------------------------------- */

static sim_hold *hold_find(const void *m)
{
    size_t i;
    for (i = 0; i < sim.nholds; ++i) if (sim.holds[i].m == m) return &sim.holds[i];
    return NULL;
}
static sim_thread *owner_of(const void *m) { sim_hold *h = hold_find(m); return h ? h->owner : NULL; }

static void hold_take(const void *m, sim_thread *t)
{
    sim_hold *h = hold_find(m);
    if (h) { ++h->depth; return; }                       /* a recursive pthread mutex, again */
    if (sim.nholds == sim.capholds) {
        size_t cap = sim.capholds ? sim.capholds * 2 : 64;
        sim_hold *n = (sim_hold *)realloc(sim.holds, cap * sizeof *n);
        if (!n) { fprintf(stderr, "hal_sim: out of memory\n"); abort(); }
        sim.holds = n; sim.capholds = cap;
    }
    sim.holds[sim.nholds].m = m; sim.holds[sim.nholds].owner = t; sim.holds[sim.nholds].depth = 1;
    ++sim.nholds;
}

/* Let go of one level of `m`, or all of them; the depth left. */
static unsigned hold_drop(const void *m, int all)
{
    sim_hold *h = hold_find(m);
    if (!h) return 0;
    if (!all && --h->depth > 0) return h->depth;
    *h = sim.holds[--sim.nholds];
    return 0;
}

static int sim_lockers(const void *m)        /* is any thread waiting to take `m`? */
{
    const sim_thread *t;
    for (t = sim.live; t; t = t->next) if (t->state == S_MUTEX && t->on == m) return 1;
    return 0;
}

/* --- reports ----------------------------------------------------------------- */

static void sim_where(char *buf, size_t cap, void *addr)
{
    Dl_info di;
    const char *base;
    if (!addr || !dladdr(addr, &di) || !di.dli_fname) { snprintf(buf, cap, "%p", addr); return; }
    base = strrchr(di.dli_fname, '/');
    base = base ? base + 1 : di.dli_fname;
    /* An offset into the file, which addr2line -e <file> turns into a line. `addr` is
     * a return address; one byte back is inside the call, so the line is the call's
     * rather than the one after it. */
    snprintf(buf, cap, "%s+0x%lx", base, (unsigned long)((char *)addr - 1 - (char *)di.dli_fbase));
}

static void sim_dump1(const sim_thread *t)
{
    char what[192], at[160];
    const sim_thread *o;
    switch (t->state) {
    case S_RUN:   snprintf(what, sizeof what, "%s", t->polls >= SIM_SPIN ? "runs, polling" : "can run"); break;
    case S_MUTEX: o = owner_of(t->on);
                  if (o) snprintf(what, sizeof what, "waits for mutex %p, held by thread %u", t->on, o->id);
                  else   snprintf(what, sizeof what, "waits for mutex %p, which is free", t->on);
                  break;
    case S_COND:  if (t->wake_at == SIM_NEVER) snprintf(what, sizeof what, "waits on cond %p (mutex %p), no timeout", t->on, t->relock);
                  else snprintf(what, sizeof what, "waits on cond %p (mutex %p) until %llu ms", t->on, t->relock,
                                (unsigned long long)t->wake_at);
                  break;
    case S_SLEEP: snprintf(what, sizeof what, "sleeps until %llu ms", (unsigned long long)t->wake_at); break;
    case S_JOIN:  snprintf(what, sizeof what, "joins thread %u", ((const sim_thread *)t->on)->id); break;
    default:      snprintf(what, sizeof what, "has exited, not yet joined"); break;
    }
    sim_where(at, sizeof at, t->where);
    fprintf(stderr, "  thread %u%s: %s; last call %s from %s\n", t->id, t == sim.running ? " (running)" : "",
            what, t->call ? t->call : "-", at);
}

static void sim_dump(void)
{
    const sim_thread *t;
    for (t = sim.live; t; t = t->next) sim_dump1(t);
    for (t = sim.dead; t; t = t->next) sim_dump1(t);
}

static void sim_report(const char *what, const char *why)
{
    fflush(stdout);                      /* the test's own lines first: no other thread is inside stdio */
    fprintf(stderr, "hal_sim: %s - %s\n"
                    "hal_sim: seed %llu, step %llu, virtual time %llu ms, trace %016llx\n",
            what, why, (unsigned long long)sim.seed, (unsigned long long)sim.steps,
            (unsigned long long)sim.now, (unsigned long long)sim.trace);
    sim_dump();
    fflush(stderr);
}

static void sim_stuck(const char *what, const char *why)
{
    sim_report(what, why);
    abort();
}

static void sim_fatal(const sim_thread *me, const char *msg)
{
    char why[256];
    snprintf(why, sizeof why, "thread %u %s", me->id, msg);
    sim_stuck("CONTRACT", why);
}

/* --- threads ------------------------------------------------------------------ */

static sim_thread *sim_new_thread(void)       /* sim.g held */
{
    sim_thread *t = (sim_thread *)calloc(1, sizeof *t);
    if (!t || pthread_cond_init(&t->go, NULL) != 0) { fprintf(stderr, "hal_sim: out of memory\n"); abort(); }
    t->id = sim.next_id++;
    t->state = S_RUN; t->wake_at = SIM_NEVER; t->call = "(start)";
    if (sim.live_tail) sim.live_tail->next = t; else sim.live = t;
    sim.live_tail = t;
    if (++sim.nlive > sim.ncand) {
        sim_thread **c = (sim_thread **)realloc(sim.cand, sim.nlive * 2 * sizeof *c);
        if (!c) { fprintf(stderr, "hal_sim: out of memory\n"); abort(); }
        sim.cand = c; sim.ncand = sim.nlive * 2;
    }
    return t;
}

static void sim_unlink_live(sim_thread *t)
{
    sim_thread **pp, *prev = NULL;
    for (pp = &sim.live; *pp; prev = *pp, pp = &(*pp)->next)
        if (*pp == t) { *pp = t->next; break; }
    if (sim.live_tail == t) sim.live_tail = prev;
    t->next = NULL;
    --sim.nlive;
}

static void sim_unlink_dead(sim_thread *t)
{
    sim_thread **pp;
    for (pp = &sim.dead; *pp; pp = &(*pp)->next)
        if (*pp == t) { *pp = t->next; break; }
}

static sim_thread *sim_find(pthread_t pt)
{
    sim_thread *t;
    for (t = sim.live; t; t = t->next) if (pthread_equal(t->pt, pt)) return t;
    for (t = sim.dead; t; t = t->next) if (pthread_equal(t->pt, pt)) return t;
    return NULL;
}

/* --- the scheduler ------------------------------------------------------------- */

static int sim_ready(const sim_thread *t)
{
    switch (t->state) {
    case S_RUN:   return 1;
    case S_MUTEX: return owner_of(t->on) == NULL;
    case S_JOIN:  return ((const sim_thread *)t->on)->state == S_DONE;
    default:      return 0;          /* a cond wait or a sleep, until it is woken or times out */
    }
}

/* Move the clock to `target` and end every wait that is due. A poll streak ends
 * when time moves for it (`polls`): a spinner gets SIM_SPIN polls to see the new
 * time before the clock creeps again. */
static void sim_advance(uint64_t target, int polls)
{
    sim_thread *t;
    /* Timers that only re-arm - a livelock - look exactly like a test that naps
     * until time() says enough (the conformance test's rate check does): the
     * difference is a call to time() the simulation cannot see. So this is said
     * once, and the run goes on; a real hang ends at the CTest timeout. */
    if (sim.horizon && target - sim.progress_at > sim.horizon && sim.told_at != sim.progress_at) {
        sim.told_at = sim.progress_at;
        sim_report("HANG?", "no thread has woken or freed another within the horizon; time only passes");
    }
    if (sim.pace) {
        uint64_t real = sim_real_ms() - sim.pace_r, want = target - sim.pace_v;
        if (want > real) {
            struct timespec ts;
            ts.tv_sec  = (time_t)((want - real) / 1000u);
            ts.tv_nsec = (long)((want - real) % 1000u) * 1000000L;
            __atomic_store_n(&sim.pacing, 1, __ATOMIC_RELAXED);   /* not a stall: the watchdog reads it */
            real_nanosleep(&ts, NULL);
            __atomic_store_n(&sim.pacing, 0, __ATOMIC_RELAXED);
        }
    }
    if (sim.verbose)
        fprintf(stderr, "hal_sim: [%llu] time %llu -> %llu ms\n", (unsigned long long)sim.steps,
                (unsigned long long)sim.now, (unsigned long long)target);
    sim.now = target;
    sim_mix(0x5EC0DE00ull ^ target);
    for (t = sim.live; t; t = t->next) {
        if (polls) t->polls = 0;
        if (t->wake_at > sim.now) continue;
        if (t->state == S_COND) { t->state = S_MUTEX; t->on = t->relock; t->timedout = 1; t->wake_at = SIM_NEVER; }
        else if (t->state == S_SLEEP) { t->state = S_RUN; t->wake_at = SIM_NEVER; }
    }
}

/* Choose the thread that runs next. `me` has reached a scheduling point, and may be
 * blocked. Time moves here and only here: a little for work, and as far as needed
 * when no thread can go on without it. */
static sim_thread *sim_pick(sim_thread *me, int yield)
{
    sim_thread *t, **busy = sim.cand, **spin;
    unsigned nbusy, nspin, n, k;
    /* Work costs time too: a ms per SIM_TICK scheduling points. Without it a thread
     * that waits for time while it locks and unlocks - an observer that polls the
     * engine until shutdown, a dispatcher pass that is due again at once - would
     * hold the clock still for every thread forever. No deadline is passed over:
     * every wait due by now has ended already.
     *   Paced, work costs the real time it took instead: at every scheduling point
     * the clock catches up with real time. A lag must not build up. Real time spent
     * in a call the simulation does not see (a poll() on a child's pipe) or in slow
     * work - every point a switch is slow - would otherwise be made up by the next
     * waits, all at once and in no real time, in the middle of somebody's
     * measurement: a 700 ms shutdown grace read as 6 s. And a thread that held the
     * baton for SIM_SLICE ms of real time lets another go first, as the end of a
     * time slice would: an executor that polls a child in real 200 ms slices would
     * otherwise keep a thread whose deadline has come waiting through seconds of
     * them, on a seed that seldom switches. */
    if (sim.pace) {
        uint64_t real = sim.pace_v + (sim_real_ms() - sim.pace_r);
        if (real > sim.now) {
            if (real - sim.now >= SIM_SLICE) yield = 1;
            sim.realtime = 1;
            sim_advance(real, 0);
        }
    } else if (++sim.ticks >= SIM_TICK) {
        sim.ticks = 0;
        sim_advance(sim.now + 1, 0);
    }
    for (;;) {
        uint64_t soonest = SIM_NEVER, target;
        nbusy = nspin = 0;
        spin = sim.cand + sim.nlive;     /* spinners fill from the far end, the rest from the front */
        for (t = sim.live; t; t = t->next) {
            if (sim_ready(t)) {
                if (t->polls < SIM_SPIN) busy[nbusy++] = t;
                else { *--spin = t; ++nspin; }
            } else if ((t->state == S_COND || t->state == S_SLEEP) && t->wake_at < soonest) {
                soonest = t->wake_at;
            }
        }
        if (nbusy) break;
        /* Every thread waits, or only polls: nothing changes until time passes. */
        if (!nspin && soonest == SIM_NEVER)
            sim_stuck("DEADLOCK", "every thread waits, and none has a timeout");
        target = soonest;
        /* Pollers: the clock creeps, a ms per SIM_SPIN polls. Paced, that ms waits for
         * real time, which the clock follows anyway. */
        if (nspin && sim.now + 1 < target) target = sim.now + 1;
        sim_advance(target, 1);
    }
    n = nbusy + nspin;
    if (nspin) memmove(busy + nbusy, spin, nspin * sizeof *busy);
    /* Stay on this thread, unless the seed says switch, or it only polls while others work. */
    if (!yield && sim_ready(me) && me->polls < SIM_SPIN && !sim_one_in(sim.switch_one_in)) return me;
    /* Otherwise one at random - most often one that is not just polling. */
    k = (nspin && !sim_one_in(8)) ? sim_below(nbusy) : sim_below(n);
    if (yield && busy[k] == me && n > 1) k = (k + 1) % n;
    return busy[k];
}

/* Make `next` the running thread: every switch, logged and counted, goes through
 * here. `why` is the call `me` switches at. */
static void sim_handoff(const sim_thread *me, sim_thread *next, const char *why)
{
    if (sim.verbose)
        fprintf(stderr, "hal_sim: [%llu] %llu ms: thread %u -> %u (%s)\n", (unsigned long long)sim.steps,
                (unsigned long long)sim.now, me->id, next->id, why);
    sim.running = next;
    ++sim.switches;
    real_cond_signal(&next->go);
}

/* Hand the baton to `next`, and wait until it comes back. */
static void sim_go(sim_thread *me, sim_thread *next)
{
    sim_mix(next->id);
    if (next == me) return;
    sim_handoff(me, next, me->call);
    while (sim.running != me) real_cond_wait(&me->go, &sim.g);
}

static void sim_step(sim_thread *me, int op)
{
    __atomic_store_n(&sim.steps, sim.steps + 1, __ATOMIC_RELAXED);   /* the watchdog reads it */
    sim_mix(((uint64_t)op << 32) | me->id);
}

/* A wait that returns with no signal, as the contract allows - to a waiter chosen by
 * the seed. Only from a thread that can run: a spurious wakeup must never be what
 * ends a deadlock, or a lost wakeup would go unreported. */
static void sim_spurious(void)
{
    sim_thread *t;
    unsigned n = 0, k;
    for (t = sim.live; t; t = t->next) if (t->state == S_COND) ++n;
    if (!n) return;
    k = sim_below(n);
    for (t = sim.live; t; t = t->next) {
        if (t->state != S_COND) continue;
        if (k--) continue;
        t->state = S_MUTEX; t->on = t->relock; t->timedout = 0; t->wake_at = SIM_NEVER;
        return;
    }
}

/* A scheduling point: `me` can run on; another thread may run first. A poll is a
 * call that only looks - a clock read, an acquire load, a yield. */
static void sim_point(sim_thread *me, int op, int poll)
{
    sim_step(me, op);
    if (!poll) me->polls = 0;
    else if (me->polls < UINT_MAX) ++me->polls;
    if (sim.spurious_one_in && sim_one_in(sim.spurious_one_in)) sim_spurious();
    sim_go(me, sim_pick(me, op == OP_YIELD));
}

/* `me` cannot run: let another thread run until it can. */
static void sim_block(sim_thread *me)
{
    me->polls = 0;
    sim_go(me, sim_pick(me, 0));
}

/* --- entering and leaving -------------------------------------------------------- */

static void sim_prepare(void) { if (sim.mode == SIM_ON) real_mutex_lock(&sim.g); }

/* The child runs on real time, and the parent may hold it to a deadline or wait out
 * a grace for it: from here on the parent's clock keeps to real time too, as with
 * GPTPS_SIM_PACE. Unpaced, a wait would jump the clock over seconds while the child
 * had yet to be scheduled: a 30 s shutdown grace ran out on a child that was about
 * to exit, and a task's 5 s timeout on one that had not started. */
static void sim_parent(void)
{
    if (sim.mode != SIM_ON) return;
    ++sim.forks;                         /* the summary says the run may not replay */
    if (!sim.pace) { sim.pace = 1; sim.pace_v = sim.now; sim.pace_r = sim_real_ms(); }
    real_mutex_unlock(&sim.g);
}

static void sim_child(void)
{
    if (sim.mode != SIM_ON) return;
    sim.fork_v = sim.now;
    sim.fork_r = sim_real_ms();
    sim.mode = SIM_PASS;
    real_mutex_unlock(&sim.g);           /* this thread took it in sim_prepare */
}

/* The seed and the trace, and a warning when something outside the simulation may
 * have decided, so that the seed may not replay. A child process runs on real time: a
 * parent that polls it, or holds it to a deadline, does not replay, and one that only
 * waits for it to exit does, but the two look the same from here. Real time moved
 * the clock, paced. Or a thread or a lock the simulation did not see, reported as it
 * was met. */
static void sim_summary(void)
{
    char why[192];
    size_t n = 0;
    if (sim.mode != SIM_ON || getpid() != sim.pid) return;
    why[0] = '\0';
    if (sim.forks)
        n += (size_t)snprintf(why + n, sizeof why - n, "%s%u child process%s ran on real time", n ? "; " : "",
                              sim.forks, sim.forks == 1 ? "" : "es");
    if (sim.realtime && n < sizeof why)
        n += (size_t)snprintf(why + n, sizeof why - n, "%sreal time moved the clock", n ? "; " : "");
    if ((sim.stray || sim.outside) && n < sizeof why)
        snprintf(why + n, sizeof why - n, "%ssee above", n ? "; " : "");
    fprintf(stderr, "hal_sim: seed %llu: %llu steps, %llu switches, %llu ms of virtual time, trace %016llx%s%s%s\n",
            (unsigned long long)sim.seed, (unsigned long long)sim.steps, (unsigned long long)sim.switches,
            (unsigned long long)(sim.now - SIM_T0), (unsigned long long)sim.trace,
            why[0] ? " (may not replay: " : "", why, why[0] ? ")" : "");
}

/* The watchdog: a real thread outside the simulation. A thread that runs for seconds
 * without a scheduling point is computing, blocked in a call the simulation does not
 * see, or spinning on memory; say which thread, then let the CTest timeout decide. */
static void *sim_watch(void *arg)
{
    uint64_t last = SIM_NEVER;
    unsigned quiet = 0;
    (void)arg;
    for (;;) {
        struct timespec ts = { 1, 0 };
        uint64_t steps;
        real_nanosleep(&ts, NULL);
        if (sim.mode == SIM_PASS) return NULL;
        steps = __atomic_load_n(&sim.steps, __ATOMIC_RELAXED);
        if (steps != last || __atomic_load_n(&sim.pacing, __ATOMIC_RELAXED)) { last = steps; quiet = 0; continue; }
        if (++quiet != sim.stall_s) continue;
        if (real_mutex_trylock(&sim.g) != 0) continue;
        fprintf(stderr, "hal_sim: STALL - no scheduling point for %u s: the running thread computes, blocks in a"
                        " call the simulation does not see, or spins with no call in its loop (seed %llu, step %llu)\n",
                sim.stall_s, (unsigned long long)sim.seed, (unsigned long long)sim.steps);
        sim_dump();
        real_mutex_unlock(&sim.g);
    }
}

static uint64_t sim_env(const char *name, uint64_t dflt)
{
    const char *s = getenv(name);
    return (s && *s) ? strtoull(s, NULL, 10) : dflt;
}

static void sim_init(void)
{
    uint64_t mix;
    const char *sp = getenv("GPTPS_SIM_SPURIOUS"), *sw = getenv("GPTPS_SIM_SWITCH");
    SIM_REAL(real_mutex_lock,      "pthread_mutex_lock");
    SIM_REAL(real_mutex_trylock,   "pthread_mutex_trylock");
    SIM_REAL(real_mutex_unlock,    "pthread_mutex_unlock");
    SIM_REAL(real_cond_wait,       "pthread_cond_wait");
    SIM_REAL(real_cond_timedwait,  "pthread_cond_timedwait");
    SIM_REAL(real_cond_signal,     "pthread_cond_signal");
    SIM_REAL(real_cond_broadcast,  "pthread_cond_broadcast");
    SIM_REAL(real_create,          "pthread_create");
    SIM_REAL(real_join,            "pthread_join");
    SIM_REAL(real_detach,          "pthread_detach");
    SIM_REAL(real_clock_gettime,   "clock_gettime");
    SIM_REAL(real_clock_nanosleep, "clock_nanosleep");
    SIM_REAL(real_nanosleep,       "nanosleep");
    SIM_REAL(real_sched_yield,     "sched_yield");
    pthread_mutex_init(&sim.g, NULL);

    sim.seed    = sim_env("GPTPS_SIM_SEED", 1);
    sim.cpus    = (unsigned)sim_env("GPTPS_SIM_CPUS", 0);
    sim.pace    = sim_env("GPTPS_SIM_PACE", 0) != 0;
    sim.verbose = sim_env("GPTPS_SIM_VERBOSE", 0) != 0;
    sim.horizon = sim_env("GPTPS_SIM_HORIZON_S", 600) * 1000u;
    sim.stall_s = (unsigned)sim_env("GPTPS_SIM_STALL_S", 10);
    /* The seed picks the style of the schedule too, so a search over seeds also tries
     * long runs of one thread and constant switching, with and without the freedoms. */
    sim.rng = sim.seed;
    mix = sim_next();
    sim.switch_one_in   = (sw && *sw) ? (unsigned)strtoul(sw, NULL, 10) : 1u << ((mix & 3u) * 2u);
    if (!sim.switch_one_in) sim.switch_one_in = 1;
    sim.spurious_one_in = (sp && *sp) ? (unsigned)strtoul(sp, NULL, 10)
                        : ((mix >> 2) & 3u) == 1 ? 64u : ((mix >> 2) & 3u) == 2 ? 16u : 0u;
    sim.now = sim.progress_at = SIM_T0;
    sim.trace = 0xCBF29CE484222325ull;
    sim.pid = getpid();
    sim.pace_v = SIM_T0;
    sim.pace_r = sim_real_ms();
    pthread_atfork(sim_prepare, sim_parent, sim_child);
    atexit(sim_summary);
    {
        char fr[32], cpus[32];
        if (sim.spurious_one_in) snprintf(fr, sizeof fr, "1 in %u", sim.spurious_one_in);
        else                     snprintf(fr, sizeof fr, "off");
        if (sim.cpus) snprintf(cpus, sizeof cpus, ", %u cpus", sim.cpus);
        else          cpus[0] = '\0';
        fprintf(stderr, "hal_sim: seed %llu (switch 1 in %u, freedoms %s%s%s)\n", (unsigned long long)sim.seed,
                sim.switch_one_in, fr, cpus, sim.pace ? ", paced to real time" : "");
    }
    if (sim.stall_s) {
        pthread_t w;
        if (real_create(&w, NULL, sim_watch, NULL) == 0) real_detach(w);
    }
    sim.mode = SIM_ON;
}

/* A thread the simulation did not start (none should: pthread_create is defined
 * here too). It cannot have been under the baton until now, so its place in the
 * schedule depends on real timing; say so once. */
static sim_thread *sim_adopt(void)          /* sim.g held */
{
    sim_thread *me = sim_new_thread();
    me->pt = pthread_self();
    sim_me = me;
    if (!sim.running) { sim.running = me; return me; }   /* the first: it holds the baton */
    if (!sim.stray++)
        fprintf(stderr, "hal_sim: thread %u called in without being started through the simulation;"
                        " this run is not reproducible\n", me->id);
    while (sim.running != me) real_cond_wait(&me->go, &sim.g);
    return me;
}

/* The caller's record, with sim.g held - or NULL when this call is not simulated: in
 * a forked child, or on a thread that has finished. `where` is the caller's caller,
 * for the deadlock report. */
static sim_thread *sim_enter(const char *call, void *where)
{
    sim_thread *me;
    int err = errno;
    pthread_once(&sim_once, sim_init);
    me = sim_me;
    if (sim.mode != SIM_ON || me == &sim_gone) return NULL;
    real_mutex_lock(&sim.g);
    if (!me) me = sim_adopt();
    me->err = err; me->call = call; me->where = where;
    return me;
}

static void sim_leave(sim_thread *me)
{
    errno = me->err;
    real_mutex_unlock(&sim.g);
}

#define SIM_CALLER __builtin_return_address(0)

/* --- mutexes and condition variables, simulated and pthread alike ---------------- */

/* A pthread mutex that is free in the table but busy for real: a thread outside the
 * simulation holds it. Wait for it for real, keeping the baton. */
static int sim_outside_lock(sim_thread *me, pthread_mutex_t *m)
{
    int rc;
    if (!sim.outside++)
        fprintf(stderr, "hal_sim: thread %u found pthread mutex %p held outside the simulation;"
                        " this run is not reproducible\n", me->id, (void *)m);
    real_mutex_unlock(&sim.g);
    rc = real_mutex_lock(m);
    real_mutex_lock(&sim.g);
    return rc;
}

static int sim_take(sim_thread *me, const void *m, int raw)
{
    sim_thread *o;
    int rc;
    while ((o = owner_of(m)) != NULL && o != me) {
        me->state = S_MUTEX; me->on = m; me->raw = raw;
        sim_block(me);
    }
    me->state = S_RUN;
    if (o == me && !raw) sim_fatal(me, "locks a gptps_mutex it already holds; the core never does (gptps_hal.h)");
    if (raw) {
        rc = real_mutex_trylock((pthread_mutex_t *)m);
        if (rc == EBUSY && o == me) sim_fatal(me, "locks a pthread mutex it already holds: a real run would hang here");
        if (rc == EBUSY) rc = sim_outside_lock(me, (pthread_mutex_t *)m);
        if (rc != 0) return rc;
    }
    hold_take(m, me);
    return 0;
}

static void sim_release(sim_thread *me, const void *m, int raw, int all)
{
    (void)me;
    if (raw) real_mutex_unlock((pthread_mutex_t *)m);
    if (hold_drop(m, all) == 0 && sim_lockers(m)) sim.progress_at = sim.now;
}

/* A cond wait: let go of `m`, wait for a signal or `wake_at`, take `m` back.
 * Whether it timed out. */
static int sim_wait(sim_thread *me, int op, const void *c, const void *m, int raw, uint64_t wake_at)
{
    int timedout;
    sim_step(me, op);
    if (owner_of(m) != me) sim_fatal(me, "waits on a condition without holding its mutex");
    sim_release(me, m, raw, 1);
    me->relock = m; me->raw = raw; me->timedout = 0;
    if (wake_at <= sim.now) {            /* already due: it times out, but others may run first */
        me->state = S_MUTEX; me->on = m; me->timedout = 1; me->wake_at = SIM_NEVER;
    } else {
        me->state = S_COND; me->on = c; me->wake_at = wake_at;
    }
    sim_block(me);
    timedout = me->timedout;
    me->wake_at = SIM_NEVER;
    sim_take(me, m, raw);
    return timedout;
}

/* signal wakes one waiter, chosen by the seed - or, taking the contract's freedom,
 * every one; broadcast wakes every one. */
static void sim_wake(const void *c, int all)
{
    sim_thread *t;
    unsigned n = 0, k;
    for (t = sim.live; t; t = t->next) if (t->state == S_COND && t->on == c) ++n;
    if (!n) return;
    if (!all && sim.spurious_one_in && sim_one_in(sim.spurious_one_in)) all = 1;
    k = all ? 0 : sim_below(n);
    for (t = sim.live; t; t = t->next) {
        if (t->state != S_COND || t->on != c) continue;
        if (!all && k--) continue;
        t->state = S_MUTEX; t->on = t->relock; t->timedout = 0; t->wake_at = SIM_NEVER;
        if (!all) break;
    }
    sim.progress_at = sim.now;
}

void gptps_mutex_lock(gptps_mutex *m)
{
    sim_thread *me = sim_enter("gptps_mutex_lock", SIM_CALLER);
    if (!me) { sim_posix_mutex_lock(m); return; }
    sim_point(me, OP_LOCK, 0);
    sim_take(me, m, 0);
    sim_leave(me);
}

void gptps_mutex_unlock(gptps_mutex *m)
{
    sim_thread *me = sim_enter("gptps_mutex_unlock", SIM_CALLER);
    if (!me) { sim_posix_mutex_unlock(m); return; }
    sim_point(me, OP_UNLOCK, 0);
    if (owner_of(m) != me) sim_fatal(me, "unlocks a gptps_mutex it does not hold");
    sim_release(me, m, 0, 1);
    sim_leave(me);
}

/* Destroying a mutex someone holds or waits for, or a cond someone waits on, is
 * undefined in POSIX and a use-after-free in the core: report it where it happens. */
void gptps_mutex_destroy(gptps_mutex *m)
{
    sim_thread *me = m ? sim_enter("gptps_mutex_destroy", SIM_CALLER) : NULL;
    if (me) {
        if (owner_of(m) || sim_lockers(m)) sim_fatal(me, "destroys a gptps_mutex that is held or waited for");
        sim_leave(me);
    }
    sim_posix_mutex_destroy(m);
}

void gptps_cond_destroy(gptps_cond *c)
{
    sim_thread *me = c ? sim_enter("gptps_cond_destroy", SIM_CALLER) : NULL, *t;
    if (me) {
        for (t = sim.live; t; t = t->next)
            if (t->state == S_COND && t->on == c) sim_fatal(me, "destroys a gptps_cond that a thread waits on");
        sim_leave(me);
    }
    sim_posix_cond_destroy(c);
}

void gptps_cond_wait(gptps_cond *c, gptps_mutex *m)
{
    sim_thread *me = sim_enter("gptps_cond_wait", SIM_CALLER);
    if (!me) { sim_posix_cond_wait(c, m); return; }
    sim_wait(me, OP_WAIT, c, m, 0, SIM_NEVER);
    sim_leave(me);
}

/* Any ms up to UINT64_MAX, exactly: nothing needs clamping on a virtual clock. */
void gptps_cond_timedwait(gptps_cond *c, gptps_mutex *m, uint64_t ms)
{
    sim_thread *me = sim_enter("gptps_cond_timedwait", SIM_CALLER);
    if (!me) { sim_posix_cond_timedwait(c, m, ms); return; }
    sim_wait(me, OP_WAIT, c, m, 0, sim_add(sim.now, ms));
    sim_leave(me);
}

void gptps_cond_signal(gptps_cond *c)
{
    sim_thread *me = sim_enter("gptps_cond_signal", SIM_CALLER);
    if (!me) { sim_posix_cond_signal(c); return; }
    sim_point(me, OP_SIGNAL, 0);
    sim_wake(c, 0);
    sim_leave(me);
}

void gptps_cond_broadcast(gptps_cond *c)
{
    sim_thread *me = sim_enter("gptps_cond_broadcast", SIM_CALLER);
    if (!me) { sim_posix_cond_broadcast(c); return; }
    sim_point(me, OP_BROADCAST, 0);
    sim_wake(c, 1);
    sim_leave(me);
}

/* --- threads -------------------------------------------------------------------- */

/* A thread leaves the simulation: joiners may go on, and the baton passes on - this
 * thread does not wait for it back. */
static void sim_exit(sim_thread *me)
{
    sim_thread *next;
    sim_step(me, OP_EXIT);
    me->state = S_DONE; me->call = "(exited)"; me->where = NULL;
    sim_unlink_live(me);
    if (!me->detached) { me->next = sim.dead; sim.dead = me; }
    sim.progress_at = sim.now;
    next = sim_pick(me, 0);
    sim_mix(next->id);
    sim_handoff(me, next, "exit");
    sim_me = &sim_gone;
    if (me->detached) { pthread_cond_destroy(&me->go); free(me); }
}

static void *sim_trampoline(void *p)
{
    sim_thread *me = (sim_thread *)p;
    void *ret;
    sim_me = me;
    real_mutex_lock(&sim.g);
    while (sim.running != me) real_cond_wait(&me->go, &sim.g);
    real_mutex_unlock(&sim.g);
    ret = me->fn(me->arg);
    real_mutex_lock(&sim.g);
    me->ret = ret;
    sim_exit(me);
    real_mutex_unlock(&sim.g);
    return ret;
}

/* A new thread, runnable at once; it starts on its function when the scheduler
 * first picks it. sim.g held. */
static sim_thread *sim_spawn(const pthread_attr_t *attr, void *(*fn)(void *), void *arg, int *rc)
{
    sim_thread *t = sim_new_thread();
    int ds;
    t->fn = fn; t->arg = arg;
    if (attr && pthread_attr_getdetachstate(attr, &ds) == 0 && ds == PTHREAD_CREATE_DETACHED) t->detached = 1;
    *rc = real_create(&t->pt, attr, sim_trampoline, t);
    if (*rc != 0) {
        sim_unlink_live(t);
        --sim.next_id;
        pthread_cond_destroy(&t->go);
        free(t);
        return NULL;
    }
    sim.progress_at = sim.now;
    return t;
}

/* Wait for `t` to finish, then reap it for real: it has left the simulation, so
 * the real join waits only for its last instructions. */
static void sim_reap(sim_thread *me, sim_thread *t, void **ret)
{
    while (t->state != S_DONE) { me->state = S_JOIN; me->on = t; sim_block(me); }
    me->state = S_RUN;
    real_join(t->pt, NULL);
    if (ret) *ret = t->ret;
    sim_unlink_dead(t);
    pthread_cond_destroy(&t->go);
    free(t);
}

gptps_thread *gptps_thread_start(gptps_thread_fn fn, void *arg)
{
    sim_thread *me = sim_enter("gptps_thread_start", SIM_CALLER), *t;
    struct gptps_thread *th;
    int rc;
    if (!me) return sim_posix_thread_start(fn, arg);
    th = (struct gptps_thread *)malloc(sizeof *th);
    t = th ? sim_spawn(NULL, fn, arg, &rc) : NULL;
    if (!t) { free(th); sim_leave(me); return NULL; }
    th->t = t->pt; th->fn = fn; th->arg = arg;
    sim_point(me, OP_START, 0);
    sim_leave(me);
    return th;
}

void gptps_thread_join(gptps_thread *th)
{
    sim_thread *me, *t;
    if (!th) return;
    me = sim_enter("gptps_thread_join", SIM_CALLER);
    if (!me) { sim_posix_thread_join(th); return; }
    sim_point(me, OP_JOIN, 0);
    t = sim_find(th->t);
    if (t) sim_reap(me, t, NULL);
    sim_leave(me);
    if (!t) real_join(th->t, NULL);
    free(th);
}

/* Never 0, which the core reads as "no thread"; never reused. */
uint64_t gptps_hal_thread_id(void)
{
    sim_thread *me = sim_enter("gptps_hal_thread_id", SIM_CALLER);
    uint64_t id;
    if (!me) return sim_posix_thread_id();
    id = (uint64_t)me->id + 1u;
    sim_leave(me);
    return id;
}

/* --- the clock and the acquire/release pair ---------------------------------------- */

/* The clock where this call is not simulated: a forked child goes on from the
 * virtual time of the fork at the rate of real time; a finished thread reads it as
 * it stands. */
static uint64_t sim_unsimulated_ms(void)
{
    if (sim.mode == SIM_PASS) return sim.fork_v + (sim_real_ms() - sim.fork_r);
    return __atomic_load_n(&sim.now, __ATOMIC_RELAXED);
}

uint64_t gptps_hal_monotonic_ms(void)
{
    sim_thread *me = sim_enter("gptps_hal_monotonic_ms", SIM_CALLER);
    uint64_t now;
    if (!me) return sim_unsimulated_ms();
    sim_point(me, OP_CLOCK, 1);
    now = sim.now;
    sim_leave(me);
    return now;
}

/* The real atomics underneath: one thread runs at a time, so they cannot matter to
 * the result here, but they keep the file correct if that is ever relaxed. */
uint32_t gptps_hal_load_acquire_u32(const uint32_t *p)
{
    sim_thread *me = sim_enter("gptps_hal_load_acquire_u32", SIM_CALLER);
    uint32_t v;
    if (!me) return sim_posix_load_acquire_u32(p);
    sim_point(me, OP_LOAD, 1);
    v = sim_posix_load_acquire_u32(p);
    sim_leave(me);
    return v;
}

void gptps_hal_store_release_u32(uint32_t *p, uint32_t v)
{
    sim_thread *me = sim_enter("gptps_hal_store_release_u32", SIM_CALLER);
    if (!me) { sim_posix_store_release_u32(p, v); return; }
    sim_point(me, OP_STORE, 0);
    sim_posix_store_release_u32(p, v);
    sim.progress_at = sim.now;           /* a poller may be waiting for exactly this */
    sim_leave(me);
}

gptps_status gptps_hal_detect(gptps_hwinfo *out)
{
    gptps_status st = sim_posix_detect(out);
    pthread_once(&sim_once, sim_init);
    if (st == GPTPS_OK && sim.cpus) out->cpu_count = sim.cpus;
    return st;
}

/* --- the pthread, sleep and clock calls of the add-ons and the tests ---------------- */

int pthread_mutex_lock(pthread_mutex_t *m)
{
    sim_thread *me = sim_enter("pthread_mutex_lock", SIM_CALLER);
    int rc;
    if (!me) return real_mutex_lock(m);
    sim_point(me, OP_LOCK, 0);
    rc = sim_take(me, m, 1);
    sim_leave(me);
    return rc;
}

int pthread_mutex_trylock(pthread_mutex_t *m)
{
    sim_thread *me = sim_enter("pthread_mutex_trylock", SIM_CALLER), *o;
    int rc;
    if (!me) return real_mutex_trylock(m);
    sim_point(me, OP_TRYLOCK, 0);
    o = owner_of(m);
    if (o && o != me) rc = EBUSY;
    else if ((rc = real_mutex_trylock(m)) == 0) hold_take(m, me);
    sim_leave(me);
    return rc;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    sim_thread *me = sim_enter("pthread_mutex_unlock", SIM_CALLER);
    int rc;
    if (!me) return real_mutex_unlock(m);
    sim_point(me, OP_UNLOCK, 0);
    if (owner_of(m) == me) { sim_release(me, m, 1, 0); rc = 0; }
    else rc = real_mutex_unlock(m);      /* not taken through the simulation: as libc says */
    sim_leave(me);
    return rc;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    sim_thread *me = sim_enter("pthread_cond_wait", SIM_CALLER);
    if (!me) return real_cond_wait(c, m);
    sim_wait(me, OP_WAIT, c, m, 1, SIM_NEVER);
    sim_leave(me);
    return 0;
}

/* An absolute time on CLOCK_REALTIME or CLOCK_MONOTONIC - the cond's attribute says
 * which, and only libc can read that. Both are virtual here and their ranges are
 * decades apart, so the value says which. Rounded up: never early. */
static uint64_t sim_abs_ms(const struct timespec *ts)
{
    uint64_t ms;
    if (ts->tv_sec < 0) return 0;
    ms = (uint64_t)ts->tv_sec * 1000u + ((uint64_t)ts->tv_nsec + 999999u) / 1000000u;
    return ms >= SIM_EPOCH_MS ? ms - SIM_EPOCH_MS : ms;
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime)
{
    sim_thread *me = sim_enter("pthread_cond_timedwait", SIM_CALLER);
    int rc;
    if (!me) return real_cond_timedwait(c, m, abstime);
    if (abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000L) { sim_leave(me); return EINVAL; }
    rc = sim_wait(me, OP_WAIT, c, m, 1, sim_abs_ms(abstime)) ? ETIMEDOUT : 0;
    sim_leave(me);
    return rc;
}

int pthread_cond_signal(pthread_cond_t *c)
{
    sim_thread *me = sim_enter("pthread_cond_signal", SIM_CALLER);
    if (!me) return real_cond_signal(c);
    sim_point(me, OP_SIGNAL, 0);
    sim_wake(c, 0);
    sim_leave(me);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c)
{
    sim_thread *me = sim_enter("pthread_cond_broadcast", SIM_CALLER);
    if (!me) return real_cond_broadcast(c);
    sim_point(me, OP_BROADCAST, 0);
    sim_wake(c, 1);
    sim_leave(me);
    return 0;
}

int pthread_create(pthread_t *thr, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
    sim_thread *me = sim_enter("pthread_create", SIM_CALLER), *t;
    int rc;
    if (!me) return real_create(thr, attr, fn, arg);
    if ((t = sim_spawn(attr, fn, arg, &rc)) != NULL) {
        *thr = t->pt;
        sim_point(me, OP_START, 0);
    }
    sim_leave(me);
    return rc;
}

int pthread_join(pthread_t thr, void **ret)
{
    sim_thread *me = sim_enter("pthread_join", SIM_CALLER), *t;
    if (!me) return real_join(thr, ret);
    sim_point(me, OP_JOIN, 0);
    t = sim_find(thr);
    if (t == me) { sim_leave(me); return EDEADLK; }
    if (t) sim_reap(me, t, ret);
    sim_leave(me);
    return t ? 0 : real_join(thr, ret);
}

int pthread_detach(pthread_t thr)
{
    sim_thread *me = sim_enter("pthread_detach", SIM_CALLER), *t;
    int rc;
    if (!me) return real_detach(thr);
    t = sim_find(thr);
    rc = real_detach(thr);
    if (t && rc == 0) {
        if (t->state != S_DONE) t->detached = 1;
        else { sim_unlink_dead(t); pthread_cond_destroy(&t->go); free(t); }
    }
    sim_leave(me);
    return rc;
}

static int sim_clock(clockid_t clk, int *realtime)
{
    *realtime = 0;
    switch (clk) {
    case CLOCK_REALTIME:
#ifdef CLOCK_REALTIME_COARSE
    case CLOCK_REALTIME_COARSE:
#endif
        *realtime = 1;
        return 1;
    case CLOCK_MONOTONIC:
#ifdef CLOCK_MONOTONIC_RAW
    case CLOCK_MONOTONIC_RAW:
#endif
#ifdef CLOCK_MONOTONIC_COARSE
    case CLOCK_MONOTONIC_COARSE:
#endif
#ifdef CLOCK_BOOTTIME
    case CLOCK_BOOTTIME:
#endif
        return 1;
    default:
        return 0;                        /* CPU-time clocks stay real */
    }
}

int clock_gettime(clockid_t clk, struct timespec *ts)
{
    sim_thread *me;
    uint64_t ms;
    int realtime;
    pthread_once(&sim_once, sim_init);
    if (!sim_clock(clk, &realtime)) return real_clock_gettime(clk, ts);
    if (!(me = sim_enter("clock_gettime", SIM_CALLER))) return real_clock_gettime(clk, ts);
    sim_point(me, OP_CLOCK, 1);
    ms = sim.now + (realtime ? SIM_EPOCH_MS : 0u);
    sim_leave(me);
    ts->tv_sec  = (time_t)(ms / 1000u);
    ts->tv_nsec = (long)(ms % 1000u) * 1000000L;
    return 0;
}

/* A sleep is a timed wait on nothing: the clock jumps over it when everything else
 * waits too. A zero sleep is a yield. */
static void sim_sleep(sim_thread *me, uint64_t ms)
{
    if (!ms) { sim_point(me, OP_YIELD, 1); return; }
    sim_step(me, OP_SLEEP);
    me->state = S_SLEEP; me->wake_at = sim_add(sim.now, ms);
    sim_block(me);
    me->state = S_RUN; me->wake_at = SIM_NEVER;
}

static uint64_t sim_ts_ms(const struct timespec *ts)     /* a duration, rounded up */
{
    return (uint64_t)ts->tv_sec * 1000u + ((uint64_t)ts->tv_nsec + 999999u) / 1000000u;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    sim_thread *me = sim_enter("nanosleep", SIM_CALLER);
    if (!me) return real_nanosleep(req, rem);
    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
        sim_leave(me);
        errno = EINVAL;
        return -1;
    }
    sim_sleep(me, sim_ts_ms(req));
    sim_leave(me);
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}

int clock_nanosleep(clockid_t clk, int flags, const struct timespec *req, struct timespec *rem)
{
    sim_thread *me;
    uint64_t ms;
    int realtime;
    pthread_once(&sim_once, sim_init);
    if (!sim_clock(clk, &realtime)) return real_clock_nanosleep(clk, flags, req, rem);
    if (!(me = sim_enter("clock_nanosleep", SIM_CALLER))) return real_clock_nanosleep(clk, flags, req, rem);
    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) { sim_leave(me); return EINVAL; }
    if (flags & TIMER_ABSTIME) {
        uint64_t at = sim_abs_ms(req);
        ms = at > sim.now ? at - sim.now : 0;
    } else {
        ms = sim_ts_ms(req);
    }
    sim_sleep(me, ms);
    sim_leave(me);
    if (rem && !(flags & TIMER_ABSTIME)) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}

int usleep(useconds_t us)
{
    sim_thread *me = sim_enter("usleep", SIM_CALLER);
    struct timespec ts;
    if (!me) {
        ts.tv_sec = (time_t)(us / 1000000u); ts.tv_nsec = (long)(us % 1000000u) * 1000L;
        return real_nanosleep(&ts, NULL);
    }
    sim_sleep(me, ((uint64_t)us + 999u) / 1000u);
    sim_leave(me);
    return 0;
}

unsigned int sleep(unsigned int s)
{
    sim_thread *me = sim_enter("sleep", SIM_CALLER);
    struct timespec ts, rem;
    if (!me) {
        ts.tv_sec = (time_t)s; ts.tv_nsec = 0;
        return real_nanosleep(&ts, &rem) == 0 ? 0u : (unsigned)rem.tv_sec;
    }
    sim_sleep(me, (uint64_t)s * 1000u);
    sim_leave(me);
    return 0;
}

int sched_yield(void)
{
    sim_thread *me = sim_enter("sched_yield", SIM_CALLER);
    if (!me) return real_sched_yield();
    sim_point(me, OP_YIELD, 1);
    sim_leave(me);
    return 0;
}
