/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_stress_api.c - a randomized stress of the public API, checked against the
 * engine's own invariants.
 *
 * The engine's worst bugs have sat where two features meet on two threads - an
 * add-on loading while a reload runs, a setting's write accessor submitting work, a
 * resource defined while a task is unregistered - and each was pinned by a scenario
 * written after it was found. This test searches that space instead. N threads draw
 * operations at random, from one logged seed, out of everything the THREADING
 * contract (gptps.h) lets run at once, while the engine runs THREADED or MANUAL (a
 * thread of ours pumping gptps_step):
 *   - submit, submit_ex and cancel;
 *   - settings get, set and set_ex, on core, per-task and host keys;
 *   - pause, clone, unregister with every policy, and register or re-register, both
 *     in-process and PROGRAM types (POSIX);
 *   - a resource re-budgeted, a task's cost of one changed, its priority changed;
 *   - reload and save of a config file other threads keep rewriting, config_check;
 *   - the dead-letter drain;
 *   - an add-on load whose setup registers types and then fails;
 * and the callbacks re-enter as the contract invites: observers cancel, submit and
 * remove types, a settings watcher reads, a setting's write accessor submits, the
 * drain callback resubmits, task bodies submit and cancel. Some rounds run a bounded
 * engine (docs/BOUNDED.md), whose items come from a fixed pool, and some install a
 * scheduler hook.
 *
 * What must hold, during the run and once it is over:
 *   - every handle a submit returned gets exactly one terminal event, the same one
 *     for the event callback as for the observers, and nothing else gets any (the
 *     terminal events are the ones tests/test_reconcile.c names);
 *   - every live handle can still be cancelled to its end, and then nothing is
 *     queued, running or holding a resource;
 *   - no setting outlives its task, and every live task has its own;
 *   - a saved config file always reopens with gptps_open;
 *   - gptps_task_get_info never reports a count the engine cannot have;
 *   - a settings watcher hears every successful live set, once;
 *   - shutdown finishes within its grace;
 *   - and, since the suite runs under ASan and TSan, no sanitizer report.
 * A hang is a failure too: a watchdog names the operation each thread is stuck in.
 *
 * Only what the contract allows is done. Registration is setup-time in general, but
 * the control plane is live (Readme, "Manage tasks at runtime"); what stays out of
 * bounds is registering a name while its removal runs, so each name's register,
 * clone-into and unregister are serialised here by a flag of the test's own.
 *
 * REPRODUCING. Every round prints its seed. `test_stress_api --seed S --rounds 1`
 * runs that round again; add `--replay` to run its operations on one thread, in the
 * same order, so a failure that comes from the sequence rather than the timing
 * repeats exactly. `--mode`, `--hot` and `--bounded` pin a round's shape. The budget is
 * about 10 s by default, for CI; `--ms` (or GPTPS_STRESS_MS) and `--seed` (or
 * GPTPS_STRESS_SEED) drive a long local run, and `--keep-going` runs on past a failing
 * round. GPTPS_STRESS_DEBUG prints the engine's state and where the time went.
 */
#include "gptps.h"
#include "gptps_hal.h"     /* threads, a mutex and condvar to nap on, the atomic file replace */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  define HAVE_PROGRAM 0   /* the PROGRAM types run /bin/sh */
/* Windows refuses to replace a file another handle has open, so a save - or this
 * test's rewrite - that lands while a reload or a reopen is reading the file fails
 * with GPTPS_E_IO there. The other way round, a reload or a reopen that lands while
 * the file is being replaced cannot open it: "cannot open the file (Permission
 * denied)", and the call fails with GPTPS_E_CONFIG (RACED_A_REPLACE). Either way the
 * reader never sees half a file, which is what matters. */
#  define REPLACE_MAY_FAIL 1
#else
#  define HAVE_PROGRAM 1
#  define REPLACE_MAY_FAIL 0
#endif

/* ------------------------------------------------------------------ checks */

static int fails = 0;
static int g_reported = 0;           /* failures printed: past 40 they are only counted */
static void dump_state(void);

static void failed(const char *file, int line, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (__atomic_add_fetch(&g_reported, 1, __ATOMIC_SEQ_CST) <= 40) {
        printf("FAIL %s:%d: %s\n", file, line, msg);
        fflush(stdout);
    }
    __atomic_add_fetch(&fails, 1, __ATOMIC_SEQ_CST);
}
#define CHECK(c) do { if (!(c)) failed(__FILE__, __LINE__, "%s", #c); } while (0)
/* A check whose failure needs its numbers to be understood. */
#define CHECKF(c, ...) do { if (!(c)) failed(__FILE__, __LINE__, __VA_ARGS__); } while (0)

static int  inc(int *p)        { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int  dec(int *p)        { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int  get(int *p)        { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static void put(int *p, int v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static int  claim(int *p)      { int idle = 0; return __atomic_compare_exchange_n(p, &idle, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }

/* ------------------------------------------------------------- randomness */

/* splitmix64: each thread has its own stream, so its operations depend on the seed
 * and its index alone. A callback has no stream - it runs on whichever thread the
 * engine picks - so it draws from a hash of what it is called for instead. */
static uint64_t mix64(uint64_t z)
{
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static uint64_t rnext(uint64_t *s) { *s += 0x9E3779B97F4A7C15ull; return mix64(*s); }
static unsigned rint_(uint64_t *s, unsigned n) { return n ? (unsigned)(rnext(s) % n) : 0u; }
static unsigned hcoin(uint64_t a, uint64_t b, unsigned n)
{
    uint64_t s = a * 0x100000001B3ull ^ (b + 0x632BE59BD9B4E019ull);
    return rint_(&s, n);
}
static unsigned pick(uint64_t *s, const unsigned *w, unsigned n)
{
    unsigned i, total = 0, x;
    for (i = 0; i < n; ++i) total += w[i];
    x = rint_(s, total);
    for (i = 0; i < n; ++i) { if (x < w[i]) return i; x -= w[i]; }
    return n - 1;
}

/* ------------------------------------------------------------ the handles */

/* Every event, counted per handle. The engine numbers handles from 1 in each engine,
 * so a flat table indexed by handle needs no lock and no lookup - and an event that
 * arrives before its submit has returned (STARTED can, see gptps.h EVENTS) simply
 * lands first. */
typedef struct {
    int submitted;      /* times a submit returned this handle: ends at 1, or 0 and no events */
    int queued, started, finished, failed_cancel, failed_other, retried, dead, dropped;
    int terminal;       /* the events that close it, as the observers count them */
    int cb_terminal;    /* the same, as the host's event callback counts them */
    int defers;         /* times the constraint deferred it */
} hrec;
#define H_CAP (1 << 18)
static hrec *g_h;

/* Per-round counters, all touched with atomics. */
static struct {
    int attempts;       /* submits tried: bounds the handles the engine can issue */
    int submit_ok, live, svc_live, last_handle;
    int untracked;      /* an event for a handle outside the table */
    int sets_ok, watched, go_writes, drained, resubmits, flaky;
    int reload_ok, reload_busy, reload_raced, save_ok, save_busy, reopened;
    int reg_ok, unreg_ok, unreg_busy, unreg_cb_tried, unreg_cb, clone_ok, throttled, throttled_live, loads;
    int quiesce;        /* the round is winding down: callbacks start nothing new */
    int sealed;         /* a bounded engine took its first submit */
} g_n;
#define LIVE_MAX 1500   /* backlog past which a thread stops submitting for a while */
#define SVC_MAX  4      /* service instances: each one keeps a worker half busy */

static hrec *hrec_of(gptps_handle h)
{
    if (h == 0 || h >= H_CAP) { inc(&g_n.untracked); return NULL; }
    return &g_h[h];
}

/* ------------------------------------------------------------- the slots */

/* The task names, each with a fixed FAMILY: what it runs and how its handles close.
 * A clone only ever copies within a family, so the family of a handle can be told
 * from its task's name - which is all an event carries. */
enum { F_INPROC, F_PROGRAM, F_SERVICE, F_RETIRE };
typedef struct {
    const char *name;
    int         family;
    int         at_setup;   /* registered before the operations start */
    int         registered; /* the test's view: changed only by the holder of `busy` */
    int         busy;       /* a register, clone-into or unregister of this name is running */
    int         gen;        /* registrations so far: varies the definition */
} slot;

static slot g_slots[] = {
    { "t0",   F_INPROC,  1, 0, 0, 0 },
    { "t1",   F_INPROC,  1, 0, 0, 0 },
    { "t2",   F_INPROC,  1, 0, 0, 0 },
    { "t3",   F_INPROC,  0, 0, 0, 0 },
    { "t0.x", F_INPROC,  1, 0, 0, 0 },   /* shares t0's "tasks.t0." settings prefix */
    { "c0",   F_INPROC,  0, 0, 0, 0 },
    { "c1",   F_INPROC,  0, 0, 0, 0 },
    { "p0",   F_PROGRAM, 1, 0, 0, 0 },
    { "p1",   F_PROGRAM, 0, 0, 0, 0 },
    { "s0",   F_SERVICE, 1, 0, 0, 0 },
    { "s1",   F_SERVICE, 0, 0, 0, 0 },
    { "r0",   F_RETIRE,  1, 0, 0, 0 },
};
#define NSLOTS ((int)(sizeof g_slots / sizeof g_slots[0]))

static int slot_index(const char *name)
{
    int i;
    for (i = 0; i < NSLOTS; ++i) if (strcmp(g_slots[i].name, name) == 0) return i;
    return -1;
}
static int family_of(const char *name)
{
    int i = name ? slot_index(name) : -1;
    return i < 0 ? F_INPROC : g_slots[i].family;
}
static int is_service(int family) { return family == F_SERVICE || family == F_RETIRE; }

/* Which task a "tasks.<name>.<leaf>" key belongs to: the slot whose name is the
 * longest prefix followed by a dot - "tasks.t0.x.priority" is t0.x's, not t0's. */
static int owner_slot(const char *rest)
{
    int i, best = -1;
    size_t bl = 0;
    for (i = 0; i < NSLOTS; ++i) {
        size_t n = strlen(g_slots[i].name);
        if (n > bl && strncmp(rest, g_slots[i].name, n) == 0 && rest[n] == '.') { best = i; bl = n; }
    }
    return best;
}

/* -------------------------------------------------------- shared helpers */

static gptps *g_e;                  /* the round's engine: callbacks reach it here */
static int    g_manual;             /* the round runs MANUAL */
static int    g_bounded;            /* ... on a bounded engine, sealed before the operations start */
#define BOUNDED_PAYLOAD 48          /* its payload slots: a longer payload is refused */
static const unsigned g_conc = 3;   /* max_concurrent_tasks it is opened with */
static char   g_cfg[96], g_bad[2][96];

/* A nap is a timed wait on a condvar nobody signals. A body that waits must still be
 * stoppable, so busy_for naps a millisecond at a time and looks at its cancel flag in
 * between, as a cooperative body must. */
static gptps_mutex *g_napm;
static gptps_cond  *g_napc;
static void nap(unsigned ms)
{
    gptps_mutex_lock(g_napm);
    gptps_cond_timedwait(g_napc, g_napm, ms);
    gptps_mutex_unlock(g_napm);
}
static int busy_for(gptps_ctx *c, unsigned ms)
{
    uint64_t t0 = gptps_now_ms(c);
    while (gptps_now_ms(c) - t0 < ms) {
        if (gptps_is_cancelled(c)) return 1;
        nap(1);
    }
    return gptps_is_cancelled(c);
}

/* The log sink: the last lines, for a failure to show what the engine said. */
static gptps_mutex *g_logm;
static char g_logring[16][320];
static int  g_logn;
/* Windows cannot open a file while another handle replaces it, so there a reload
 * or a reopen that meets one of the writers' replaces fails to open the file. The
 * sink counts those failures per thread: an open counts as having raced a replace
 * only if its own thread logged "cannot open the file" during the call. */
static struct { uint64_t tid; unsigned n; } g_ofail[64];
static int g_nofail;
static unsigned *ofail_slot(uint64_t tid)          /* caller holds g_logm */
{
    int i;
    for (i = 0; i < g_nofail; ++i) if (g_ofail[i].tid == tid) return &g_ofail[i].n;
    if (g_nofail == (int)(sizeof g_ofail / sizeof g_ofail[0])) return NULL;
    g_ofail[g_nofail].tid = tid;
    g_ofail[g_nofail].n = 0;
    return &g_ofail[g_nofail++].n;
}
static void sink(gptps_log_level lvl, const char *msg, void *ud)
{
    (void)lvl; (void)ud;
    gptps_mutex_lock(g_logm);
    snprintf(g_logring[g_logn % 16], sizeof g_logring[0], "%s", msg);
    ++g_logn;
    if (strstr(msg, "cannot open the file")) {
        unsigned *n = ofail_slot(gptps_hal_thread_id());
        if (n) ++*n;
    }
    gptps_mutex_unlock(g_logm);
}
/* How many times this thread has failed to open a config file so far. */
static unsigned open_failures_here(void)
{
    unsigned n, *slot;
    gptps_mutex_lock(g_logm);
    slot = ofail_slot(gptps_hal_thread_id());
    n = slot ? *slot : 0u;
    gptps_mutex_unlock(g_logm);
    return n;
}
/* A failed open is allowed only where a replace can block it, and only when this
 * thread's open of the file is what failed. */
#define RACED_A_REPLACE(st, before) \
    (REPLACE_MAY_FAIL && (st) == GPTPS_E_CONFIG && open_failures_here() != (before))
static void dump_log(void)
{
    int i, from;
    gptps_mutex_lock(g_logm);
    from = g_logn > 16 ? g_logn - 16 : 0;
    printf("  the engine's last log lines:\n");
    for (i = from; i < g_logn; ++i) printf("    %s\n", g_logring[i % 16]);
    gptps_mutex_unlock(g_logm);
}

static void print_file(const char *path)
{
    char buf[4096];
    size_t n;
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  (%s cannot be opened)\n", path); return; }
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    printf("  --- %s ---\n%s\n  ---\n", path, buf);
}

static int submit_status_ok(gptps_status st)
{
    return st == GPTPS_OK || st == GPTPS_E_NOTFOUND || st == GPTPS_E_BUDGET ||
           st == GPTPS_E_FULL || st == GPTPS_E_SHUTDOWN;
}

/* Submit, and record the handle. Every submit in this test - an operation's, a
 * callback's, a task body's - goes through here, so a handle with events that no
 * submit returned is the engine's doing. */
static gptps_status submit_tracked(const char *name, const void *pay, size_t len,
                                   const gptps_submit_options *o)
{
    gptps_handle h = 0;
    gptps_status st;
    if (inc(&g_n.attempts) >= H_CAP - 64) return GPTPS_E_FULL;   /* the table is full: no more this round */
    st = o ? gptps_submit_ex(g_e, name, pay, len, o, &h) : gptps_submit(g_e, name, pay, len, &h);
    if (st != GPTPS_OK) return st;
    if (h == 0 || h >= H_CAP) {
        failed(__FILE__, __LINE__, "submit returned handle %llu, outside 1..%d", (unsigned long long)h, H_CAP - 1);
        return st;
    }
    inc(&g_h[h].submitted);
    inc(&g_n.submit_ok);
    inc(&g_n.live);
    if (is_service(family_of(name))) inc(&g_n.svc_live);
    put(&g_n.last_handle, (int)h);
    return st;
}

/* ------------------------------------------------------------ task bodies */

/* A structured payload: magic, behaviour, a parameter, constraint flags, filler. */
#define PAY_MAGIC 0xA5
enum { B_OK, B_FAIL, B_WORK, B_SPIN, B_SELFCANCEL, B_RESULT, B_NOCOPY, B_SUBMIT, B_CANCEL,
       B_SETTING, B_FLAKY, B_N };
static const unsigned B_WEIGHT[B_N] = { 30, 14, 14, 2, 3, 8, 5, 4, 4, 5, 8 };
#define C_DENY  0x1u
#define C_DEFER 0x2u

static void nested_submit(const char *name, unsigned char k)
{
    unsigned char pay[4];
    gptps_status st;
    if (get(&g_n.quiesce) || get(&g_n.live) > LIVE_MAX) return;
    if (is_service(family_of(name)) && get(&g_n.svc_live) >= SVC_MAX) return;
    pay[0] = PAY_MAGIC; pay[1] = (unsigned char)((k % 4u) ? B_OK : B_FAIL); pay[2] = k; pay[3] = 0;
    st = submit_tracked(name, pay, sizeof pay, NULL);
    CHECKF(submit_status_ok(st), "a nested submit to %s returned %d", name, (int)st);
}

static void cancel_status(gptps_status st, const char *where)
{
    CHECKF(st == GPTPS_OK || st == GPTPS_E_NOTFOUND || st == GPTPS_E_SHUTDOWN,
           "gptps_cancel from %s returned %d", where, (int)st);
}

/* From inside any callback or task body, gptps_shutdown and gptps_step are refused
 * (gptps.h THREADING): either would free, or recurse into, the engine the caller is
 * standing in. Now and then, every kind of callback asks. */
static void check_refused(uint64_t a, uint64_t b, const char *where)
{
    gptps_status st;
    if (hcoin(a, b, 64) != 0) return;
    st = gptps_shutdown(g_e);
    CHECKF(st == GPTPS_E_BUSY, "gptps_shutdown from %s = %d", where, (int)st);
    if (g_manual) {
        st = gptps_step(g_e, NULL);
        CHECKF(st == GPTPS_E_BUSY, "gptps_step from %s = %d", where, (int)st);
    }
}

static gptps_status run_inproc(gptps_ctx *c, void *ud)
{
    size_t n;
    const unsigned char *p = (const unsigned char *)gptps_payload(c, &n);
    (void)ud;
    if (!p || n < 4 || p[0] != PAY_MAGIC) return GPTPS_OK;
    check_refused(p[2], n, "a task body");
    switch (p[1]) {
        case B_FAIL:       return GPTPS_E_TASK;
        case B_WORK:       busy_for(c, p[2] % 16u); return GPTPS_OK;
        case B_SPIN:       busy_for(c, 150); return GPTPS_OK;
        case B_SELFCANCEL: return GPTPS_E_CANCELLED;
        case B_RESULT:     return gptps_result_set(c, p, n);
        case B_NOCOPY: {
            char *b = (char *)malloc(n);
            if (!b) return GPTPS_E_NOMEM;
            memcpy(b, p, n);
            (void)gptps_result_set(c, "first", 5);       /* replaced next: the core frees its copy */
            return gptps_result_set_nocopy(c, b, n, free);
        }
        case B_SUBMIT:     nested_submit("t1", p[2]); return GPTPS_OK;
        case B_CANCEL: {            /* one of the latest handles: 0 is no handle at all */
            int h = get(&g_n.last_handle) - (int)(p[2] % 8u) + 1;
            if (h > 0) cancel_status(gptps_cancel(g_e, (gptps_handle)h), "a task body");
            return GPTPS_OK;
        }
        case B_SETTING: {           /* defined at setup, so every type has it from its first item on */
            long v = -1;
            gptps_status st = gptps_task_setting_int(c, "weight", &v);
            CHECKF(st == GPTPS_OK, "a task read its per-task setting weight: %d", (int)st);
            if (st == GPTPS_OK) CHECKF(v >= 0 && v <= 10, "a task read weight %ld, outside its 0..10", v);
            return GPTPS_OK;
        }
        case B_FLAKY:      return (inc(&g_n.flaky) & 1) ? GPTPS_E_TASK : GPTPS_OK;
        default:           return GPTPS_OK;
    }
}

/* A service run: works a while, then ends - cleanly, by failing, or by returning
 * GPTPS_E_CANCELLED itself, which ends the instance for good. */
static gptps_status run_service(gptps_ctx *c, void *ud)
{
    size_t n;
    const unsigned char *p = (const unsigned char *)gptps_payload(c, &n);
    unsigned b = (p && n >= 4 && p[0] == PAY_MAGIC) ? p[1] : (unsigned)B_OK;
    (void)ud;
    if (b == B_SELFCANCEL) return GPTPS_E_CANCELLED;
    if (busy_for(c, b == B_SPIN ? 150u : (p && n >= 4) ? p[2] % 40u : 5u)) return GPTPS_E_CANCELLED;
    return (b == B_FAIL || (b == B_FLAKY && (inc(&g_n.flaky) & 1))) ? GPTPS_E_TASK : GPTPS_OK;
}

#if HAVE_PROGRAM
/* One line of payload picks what the program does: echo it back, fail, or sleep - a
 * child that has to be killed to be stopped. */
static const char *const PROGRAM_ARGV[] = {
    "/bin/sh", "-c", "read a; case \"$a\" in fail*) exit 3;; nap*) sleep 1;; esac; echo \"$a\"", 0
};
#endif

/* -------------------------------------------------------- the callbacks */

/* Which events close a handle (test_reconcile.c): FINISHED - except an always-up
 * service's, which ends one run - FAILED carrying GPTPS_E_CANCELLED, DEAD_LETTERED
 * and DROPPED. */
static int closes(const gptps_event *ev)
{
    switch (ev->kind) {
        case GPTPS_EV_FINISHED:      return family_of(ev->task_name) != F_SERVICE;
        case GPTPS_EV_FAILED:        return ev->status == GPTPS_E_CANCELLED;
        case GPTPS_EV_DEAD_LETTERED:
        case GPTPS_EV_DROPPED:       return 1;
        default:                     return 0;
    }
}

/* What an event may carry, as gptps.h describes each field. */
static void check_event_shape(const gptps_event *ev)
{
    int shutdown = (ev->flags & GPTPS_EV_FLAG_SHUTDOWN) != 0;
    int self     = (ev->flags & GPTPS_EV_FLAG_SELF_CANCELLED) != 0;
    CHECKF(ev->struct_size == sizeof *ev, "an event's struct_size is %u", (unsigned)ev->struct_size);
    CHECK(ev->task_name != NULL);
    CHECKF(ev->kind <= GPTPS_EV_DROPPED, "event kind %d", (int)ev->kind);
    if (ev->kind == GPTPS_EV_FINISHED) CHECKF(ev->status == GPTPS_OK, "FINISHED with status %d", (int)ev->status);
    if (ev->kind == GPTPS_EV_FAILED || ev->kind == GPTPS_EV_DEAD_LETTERED)
        CHECKF(ev->status != GPTPS_OK, "a %d event with status OK", (int)ev->kind);
    if (ev->kind != GPTPS_EV_FINISHED) CHECKF(ev->result == NULL && ev->result_len == 0, "a result on event kind %d", (int)ev->kind);
    /* Measurements (ABI 2.5) come only with an attempt's end, and the array is there
     * exactly when it has entries. */
    CHECKF((ev->measures == NULL) == (ev->n_measures == 0), "measures %p with n_measures %u",
           (const void *)ev->measures, (unsigned)ev->n_measures);
    if (ev->kind != GPTPS_EV_FINISHED && ev->kind != GPTPS_EV_FAILED)
        CHECKF(ev->n_measures == 0, "measurements on event kind %d", (int)ev->kind);
    if (shutdown) CHECKF(ev->kind == GPTPS_EV_DEAD_LETTERED || ev->kind == GPTPS_EV_DROPPED,
                         "GPTPS_EV_FLAG_SHUTDOWN on event kind %d", (int)ev->kind);
    if (self) CHECKF(ev->kind == GPTPS_EV_FAILED && ev->status == GPTPS_E_CANCELLED,
                     "GPTPS_EV_FLAG_SELF_CANCELLED on event kind %d status %d", (int)ev->kind, (int)ev->status);
}

/* Remove a type from inside a STARTED callback - a worker, or the stepper inside
 * gptps_step. Only there: a QUEUED or a cancel's FAILED may run on a thread inside a
 * write accessor, which holds the registry's lock that a removal takes. */
static void remove_from_callback(unsigned coin)
{
    slot *s = &g_slots[coin % (unsigned)NSLOTS];
    unsigned flags = (coin / (unsigned)NSLOTS) % 3u;
    gptps_status st;
    int was;
    if (!claim(&s->busy)) return;
    inc(&g_n.unreg_cb_tried);
    was = get(&s->registered);
    st = gptps_unregister_task(g_e, s->name, flags);
    if (g_bounded) CHECKF(st == GPTPS_E_BUSY, "unregister(%s, %u) from a callback, sealed, = %d", s->name, flags, (int)st);
    else if (was)  CHECKF(st == GPTPS_OK || st == GPTPS_E_BUSY, "unregister(%s, %u) from a callback = %d", s->name, flags, (int)st);
    else           CHECKF(st == GPTPS_E_NOTFOUND, "unregister(%s, %u) of a removed type, from a callback = %d", s->name, flags, (int)st);
    if (st == GPTPS_OK) { put(&s->registered, 0); inc(&g_n.unreg_cb); }
    put(&s->busy, 0);
}

static void observer(const gptps_event *ev, void *ud)
{
    hrec *r = hrec_of(ev->handle);
    unsigned coin;
    (void)ud;
    check_event_shape(ev);
    if (!r) return;
    switch (ev->kind) {
        case GPTPS_EV_QUEUED:        inc(&r->queued); break;
        case GPTPS_EV_STARTED:       inc(&r->started); break;
        case GPTPS_EV_FINISHED:      inc(&r->finished); break;
        case GPTPS_EV_FAILED:        inc(ev->status == GPTPS_E_CANCELLED ? &r->failed_cancel : &r->failed_other); break;
        case GPTPS_EV_RETRIED:       inc(&r->retried); break;
        case GPTPS_EV_DEAD_LETTERED: inc(&r->dead); break;
        case GPTPS_EV_DROPPED:       inc(&r->dropped); break;
        case GPTPS_EV_SAMPLE:        break;   /* sampling is off here: never emitted */
    }
    if (closes(ev)) {
        inc(&r->terminal);
        dec(&g_n.live);
        if (is_service(family_of(ev->task_name))) dec(&g_n.svc_live);
    }

    /* Re-enter, as the contract invites (callbacks run with the engine lock released).
     * The coin is the handle's, so the same sequence makes the same choices. */
    check_refused(ev->handle, (uint64_t)ev->kind + 17u, "an observer");
    coin = hcoin(ev->handle, (uint64_t)ev->kind * 131u + ev->attempt, 1000);
    switch (ev->kind) {
        case GPTPS_EV_QUEUED:
            if (coin < 4) cancel_status(gptps_cancel(g_e, ev->handle), "its own QUEUED");
            break;
        case GPTPS_EV_RETRIED:            /* lands before the retry: gptps.h EVENTS */
            if (coin < 60) cancel_status(gptps_cancel(g_e, ev->handle), "its own RETRIED");
            break;
        case GPTPS_EV_STARTED:
            if (coin < 15 && !get(&g_n.quiesce)) remove_from_callback(hcoin(ev->handle, 99, 1000000));
            break;
        case GPTPS_EV_FINISHED:
        case GPTPS_EV_FAILED:
            if (coin < 8)       cancel_status(gptps_cancel(g_e, ev->handle), "its own FINISHED/FAILED");
            else if (coin < 16) cancel_status(gptps_cancel(g_e, ev->handle - 1), "a FINISHED/FAILED");
            else if (coin < 20) nested_submit(ev->task_name, (unsigned char)coin);
            break;
        default: break;
    }
}

/* The host's single event callback: the same events, counted on their own, so an
 * event that reached one channel and not the other shows. Two of them, swapped at
 * random while events flow: the engine takes the callback and its user data together,
 * so each must only ever see its own. */
static int g_cb_tag[2];
static void on_event(const gptps_event *ev, void *ud)
{
    hrec *r = hrec_of(ev->handle);
    CHECKF(ud == &g_cb_tag[0], "the first event callback got the second's user data");
    if (r && closes(ev)) inc(&r->cb_terminal);
}
static void on_event2(const gptps_event *ev, void *ud)
{
    hrec *r = hrec_of(ev->handle);
    CHECKF(ud == &g_cb_tag[1], "the second event callback got the first's user data");
    if (r && closes(ev)) inc(&r->cb_terminal);
}

static void watcher(const char *key, const char *value, void *ud)
{
    int n = inc(&g_n.watched);
    (void)ud;
    CHECK(key != NULL && value != NULL);
    check_refused((uint64_t)n, 3, "a settings watcher");
    if (key && hcoin((uint64_t)n, strlen(key), 8) == 0) {   /* a watcher may read (gptps.h) */
        char b[GPTPS_SETTINGS_VALUE_MAX];
        gptps_status st = gptps_settings_get(g_e, key, b, sizeof b);
        CHECKF(st == GPTPS_OK || st == GPTPS_E_NOTFOUND, "gptps_settings_get(%s) from a watcher = %d", key, (int)st);
    }
}

static gptps_admit_decision gate(const gptps_constraint_input *in, uint32_t *retry_after_ms, void *ud)
{
    const unsigned char *p = (const unsigned char *)in->payload;
    hrec *r;
    (void)ud;
    if (!p || in->payload_len < 4 || p[0] != PAY_MAGIC) return GPTPS_ADMIT;
    if (p[3] & C_DENY) return GPTPS_DENY;
    /* at most twice per item: a constraint that defers forever makes a DRAIN wait
     * forever, which gptps.h documents */
    if ((p[3] & C_DEFER) && (r = hrec_of(in->handle)) != NULL && inc(&r->defers) <= 2) {
        *retry_after_ms = 1u + p[2] % 20u;
        return GPTPS_DEFER;
    }
    return GPTPS_ADMIT;
}

/* An ordering hook for some rounds: priority, then age - a key that only rises. */
static int64_t sched_age(const gptps_sched_input *in, void *ud)
{
    (void)ud;
    return (int64_t)in->priority * 1000000 + (int64_t)(gptps_now_ms(NULL) - in->enqueue_ms);
}

static void on_dead(const gptps_dead_letter *dl, void *ud)
{
    ++*(int *)ud;                    /* the drain's own count: it calls back on its caller's thread */
    inc(&g_n.drained);
    check_refused(dl->handle, 5, "the dead-letter drain");
    CHECK(dl->task_name != NULL);
    CHECKF(dl->status != GPTPS_OK, "a dead letter with status OK (handle %llu)", (unsigned long long)dl->handle);
    /* One the engine could have issued. Its submit may not have returned yet - an item
     * can be denied, dead-lettered and drained first - so check_handles, at the end,
     * is what holds it to exactly one. */
    CHECKF(dl->handle > 0 && dl->handle < H_CAP && dl->handle <= (gptps_handle)get(&g_n.attempts) &&
           get(&g_h[dl->handle].submitted) <= 1,
           "a dead letter for handle %llu, which no submit returned", (unsigned long long)dl->handle);
    /* The drain callback may resubmit (gptps.h DEAD LETTER) */
    if (dl->task_name && !get(&g_n.quiesce) && get(&g_n.live) <= LIVE_MAX && hcoin(dl->handle, 7, 4) == 0 &&
        !is_service(family_of(dl->task_name))) {
        gptps_status st = submit_tracked(dl->task_name, dl->payload, dl->payload_len, NULL);
        CHECKF(submit_status_ok(st), "a resubmit from the drain returned %d", (int)st);
        if (st == GPTPS_OK) inc(&g_n.resubmits);
    }
}

/* Host settings. Their accessors run holding the registry's lock, which is all the
 * locking these cells need. host.go submits, which a write accessor may do. */
static char g_go[32] = "0";
static char g_note[2][GPTPS_SETTINGS_VALUE_MAX];
static size_t go_read(void *t, char *b, size_t c) { (void)t; return (size_t)snprintf(b, c, "%s", g_go); }
static gptps_status go_write(void *t, const char *v)
{
    unsigned k, i;
    (void)t;
    snprintf(g_go, sizeof g_go, "%s", v);
    check_refused((uint64_t)get(&g_n.go_writes), 9, "a write accessor");
    k = (unsigned)strtoul(v, NULL, 10) % 3u;
    for (i = 0; i < k; ++i) nested_submit("t0", (unsigned char)(i + 1u));
    inc(&g_n.go_writes);
    return GPTPS_OK;
}
static size_t note_read(void *t, char *b, size_t c) { return (size_t)snprintf(b, c, "%s", (const char *)t); }
static gptps_status note_write(void *t, const char *v)
{
    snprintf((char *)t, GPTPS_SETTINGS_VALUE_MAX, "%s", v);
    return GPTPS_OK;
}
static void host_setting(const char *key, gptps_setting_type type,
                         size_t (*rd)(void *, char *, size_t), gptps_status (*wr)(void *, const char *),
                         void *target)
{
    gptps_setting_def d;
    gptps_status st;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.key = key; d.type = type; d.desc = "stress_api"; d.hot = 1;
    d.read = rd; d.write = wr; d.target = target;
    if (type == GPTPS_SETTING_UINT) { d.has_range = 1; d.min = 0; d.max = 9; }
    st = gptps_register_setting(g_e, &d);
    CHECKF(st == GPTPS_OK, "gptps_register_setting(%s) = %d", key, (int)st);
}

/* --------------------------------------------------------- registration */

static gptps_status register_slot(slot *s)
{
    gptps_task_def d;
    unsigned g = (unsigned)inc(&s->gen);
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.name = s->name;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_cost.mem_bytes = (g % 4u) * 100u;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = g % 3u;
    d.default_policy.retry_backoff_seconds = (g % 5u == 0) ? 1u : 0u;
    d.default_policy.on_failure = (gptps_on_failure)(g % 3u);
    switch (s->family) {
        case F_PROGRAM:
#if HAVE_PROGRAM
            d.exec = GPTPS_EXEC_PROGRAM;
            d.argv = PROGRAM_ARGV;
            break;
#else
            return GPTPS_E_INVAL;
#endif
        case F_SERVICE:
        case F_RETIRE:
            d.exec = GPTPS_EXEC_INPROC;
            d.run = run_service;
            d.flags = GPTPS_TASK_SERVICE | (s->family == F_RETIRE ? GPTPS_TASK_RETIRE_ON_OK : 0u);
            break;
        default:
            d.exec = GPTPS_EXEC_INPROC;
            d.run = run_inproc;
            break;
    }
    return gptps_register_task(g_e, &d);
}

/* What a register of this slot may return, given the test's view of it. */
static int register_expected(const slot *s, int was, gptps_status st)
{
    /* Where PROGRAM types cannot run, register_slot refuses them itself, before the
     * engine is asked, so nothing the engine would say applies to them. */
    if (s->family == F_PROGRAM && !HAVE_PROGRAM) return st == GPTPS_E_INVAL;
    if (g_bounded && get(&g_n.sealed)) return st == GPTPS_E_BUSY;       /* sealed: setup is over */
    if (was) return st == GPTPS_E_DUP;
    if (is_service(s->family) && g_manual) return st == GPTPS_E_INVAL;   /* services are THREADED only */
    if (s->family == F_PROGRAM && g_bounded) return st == GPTPS_E_INVAL;   /* bounded: in-process only */
    return st == GPTPS_OK;
}

/* --------------------------------------------------------- config files */

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    CHECKF(f != NULL, "cannot write %s", path);
    if (!f) return;
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

/* A valid config file, written whole beside the real one and renamed over it - so
 * whoever opens it, a reload or a reopen, sees one file or the other, never half.
 * Every key either names an engine setting with a value it takes, or is a host's,
 * which a fresh engine leaves waiting: so every version reopens. */
static void write_config(const char *path, uint64_t seed, int writer)
{
    static const char *const ONFAIL[] = { "dead_letter", "requeue", "drop" };
    char text[4096], tmp[128];
    size_t k = 0;
    int i, used[NSLOTS];
    uint64_t s = seed;
    memset(used, 0, sizeof used);
#define ADD(...) do { if (k < sizeof text) k += (size_t)snprintf(text + k, sizeof text - k, __VA_ARGS__); } while (0)
    ADD("# stress_api, written by writer %d\n[limits]\n", writer);
    ADD("shutdown_grace_ms = %u\n", 500u + rint_(&s, 3000));
    ADD("max_dead_letters = %u\n", rint_(&s, 3) ? 1024u : rint_(&s, 20));
    if (rint_(&s, 4) == 0) ADD("max_intake_depth = %u\n", rint_(&s, 2) ? 0u : 200u + rint_(&s, 2000));
    if (rint_(&s, 6) == 0) ADD("max_memory_bytes = %u\n", rint_(&s, 2) ? 0u : 1000u + rint_(&s, 100000));
    ADD("[scheduler]\nreserve_after_skips = %u\n", rint_(&s, 10));
    ADD("[resources]\ngpu = %u\nio = %u\n", 1u + rint_(&s, 3), 1u + rint_(&s, 6));
    if (rint_(&s, 4) == 0) ADD("lic = %u\n", 1u + rint_(&s, 3));   /* defined at runtime by a reload */
    ADD("[task_defaults]\nmax_retries = %u\n", rint_(&s, 3));
    for (i = 0; i < 3; ++i) {
        int j = (int)rint_(&s, (unsigned)NSLOTS);
        if (used[j]) continue;                                     /* a table twice does not parse */
        used[j] = 1;
        ADD("[tasks.%s]\npriority = %d\n", g_slots[j].name, (int)rint_(&s, 11) - 5);
        if (rint_(&s, 2)) ADD("on_failure = \"%s\"\n", ONFAIL[rint_(&s, 3)]);
        if (rint_(&s, 2)) ADD("weight = %u\n", rint_(&s, 11));
        if (rint_(&s, 3) == 0) ADD("[tasks.%s.resources]\ngpu = %u\n", g_slots[j].name, rint_(&s, 3));
    }
    ADD("[host]\nknob = %u\nmode = \"%c\"\n", rint_(&s, 101), 'a' + (int)rint_(&s, 3));
    if (rint_(&s, 3) == 0) ADD("go = %u\n", rint_(&s, 3));
#undef ADD
    snprintf(tmp, sizeof tmp, "%s.w%d", path, writer);
    write_file(tmp, text);
    if (gptps_hal_atomic_replace(tmp, path) != GPTPS_OK) {
        CHECKF(REPLACE_MAY_FAIL, "renaming %s over %s", tmp, path);
        remove(tmp);
    }
}

/* A saved file must open again: in MANUAL mode, with no threads, just to read it. */
static void check_reopens(const char *path, const char *what)
{
    gptps *x = NULL;
    gptps_config c;
    gptps_status st;
    unsigned of0;
    memset(&c, 0, sizeof c);
    c.struct_size = sizeof c;
    c.limits.struct_size = sizeof c.limits;
    c.limits.max_concurrent_tasks = 1;
    c.mode = GPTPS_RUN_MANUAL;
    c.config_path = path;
    of0 = open_failures_here();
    st = gptps_open_ex(&c, &x);
    if (st != GPTPS_OK && RACED_A_REPLACE(st, of0)) { inc(&g_n.reload_raced); return; }
    CHECKF(st == GPTPS_OK, "%s: %s does not reopen (%d)", what, path, (int)st);
    if (st != GPTPS_OK) { print_file(path); dump_log(); }
    else inc(&g_n.reopened);
    if (x) gptps_shutdown(x);
}

/* ------------------------------------------------------------- operations */

enum { OP_SUBMIT, OP_SUBMIT_EX, OP_CANCEL, OP_GET, OP_SET, OP_ENABLE, OP_CLONE, OP_UNREGISTER,
       OP_REGISTER, OP_REBUDGET, OP_RESCOST, OP_PRIORITY, OP_RELOAD, OP_SAVE, OP_REWRITE, OP_CHECK,
       OP_DRAIN, OP_SCAN_TASKS, OP_SCAN_SETTINGS, OP_USAGE, OP_LOAD, OP_EVENT_CB, OP_N };
static const char *const OP_NAME[OP_N] = {
    "submit", "submit_ex", "cancel", "get", "set", "enable", "clone", "unregister",
    "register", "rebudget", "rescost", "priority", "reload", "save", "rewrite", "config_check",
    "drain", "scan_tasks", "scan_settings", "usage", "load_addon", "event_cb"
};
static const unsigned OP_WEIGHT[OP_N] = { 30, 10, 12, 6, 8, 3, 2, 2, 3, 2, 2, 1, 2, 1, 2, 1, 2, 2, 1, 1, 1, 1 };

#define RING 32
typedef struct { unsigned char op, slot; short st; } oprec;
typedef struct {
    int      idx;
    uint64_t rng;
    int      cur;            /* the operation in progress, -1 between them (atomic) */
    int      nops;
    int      done;           /* the thread has finished (atomic) */
    oprec    ring[RING];     /* the last operations, for a failure or a hang to show */
    unsigned nring;
} tstate;

#define MAX_THREADS 16
static tstate g_ts[MAX_THREADS];
static int    g_nthreads;

static const char *const TASK_LEAF[] = {
    "timeout_seconds", "max_retries", "retry_backoff_seconds", "mem_bytes", "priority",
    "on_failure", "weight", "note", "resources.gpu", "resources.io", "resources.lic"
};
#define N_TASK_LEAF (sizeof TASK_LEAF / sizeof TASK_LEAF[0])
static const char *const CORE_KEY[] = {
    "limits.max_memory_bytes", "limits.max_concurrent_tasks", "limits.max_intake_depth",
    "limits.shutdown_grace_ms", "limits.max_dead_letters", "stats.dead_letters_evicted",
    "scheduler.reserve_after_skips", "host.knob", "host.mode", "host.label", "host.go",
    "resources.gpu", "resources.io", "resources.lic"
};
#define N_CORE_KEY (sizeof CORE_KEY / sizeof CORE_KEY[0])
static const char *const ODD_KEY[] = { "limits.nope", "tasks.zz.priority", "host.knobb", "", "tasks.t0.", "tasks" };
#define N_ODD_KEY (sizeof ODD_KEY / sizeof ODD_KEY[0])

/* A key: a core or host one, a task's, or one that does not exist. */
static void random_key(uint64_t r1, uint64_t r2, char *key, size_t cap)
{
    unsigned k = (unsigned)(r1 % 100u);
    if (k < 40)      snprintf(key, cap, "%s", CORE_KEY[r2 % N_CORE_KEY]);
    else if (k < 92) snprintf(key, cap, "tasks.%s.%s", g_slots[r2 % (unsigned)NSLOTS].name,
                              TASK_LEAF[(r2 / (unsigned)NSLOTS) % N_TASK_LEAF]);
    else             snprintf(key, cap, "%s", ODD_KEY[r2 % N_ODD_KEY]);
}

/* A value for that key - one it takes, mostly, and some it must refuse. */
static void random_value(const char *key, uint64_t r, char *val, size_t cap)
{
    static const char *const BAD[] = { "-1", "x", "99999999999999999999999", "", "1e3", "0x10", " 3 ", "true" };
    static const char *const ONFAIL[] = { "dead_letter", "requeue", "drop" };
    static const char TEXT[] = "ab \"#=[]\\\t\nz";
    const char *leaf = strrchr(key, '.');
    unsigned v = (unsigned)(r >> 8);
    leaf = leaf ? leaf + 1 : key;
    if (r % 10u == 0) { snprintf(val, cap, "%s", BAD[v % (sizeof BAD / sizeof BAD[0])]); return; }
    if (!strcmp(leaf, "on_failure"))                snprintf(val, cap, "%s", ONFAIL[v % 3u]);
    else if (!strcmp(leaf, "priority"))             snprintf(val, cap, "%d", (int)(v % 11u) - 5);
    else if (!strcmp(leaf, "mode"))                 snprintf(val, cap, "%c", 'a' + (int)(v % 4u));   /* 'd' is refused */
    else if (!strcmp(leaf, "label") || !strcmp(leaf, "note")) {
        size_t i, n = v % 12u;                      /* text a save has to quote and escape */
        for (i = 0; i < n && i + 1 < cap; ++i) val[i] = TEXT[(v >> (i % 16u)) % (sizeof TEXT - 1)];
        val[i < cap ? i : cap - 1] = 0;
    }
    else if (!strcmp(leaf, "knob"))                 snprintf(val, cap, "%u", v % 120u);              /* past 100 is refused */
    else if (!strcmp(leaf, "go"))                   snprintf(val, cap, "%u", v % 3u);
    else if (!strcmp(leaf, "weight"))               snprintf(val, cap, "%u", v % 12u);
    else if (!strcmp(leaf, "max_memory_bytes"))     snprintf(val, cap, "%u", (v % 3u) ? 0u : 500u + v % 100000u);
    else if (!strcmp(leaf, "max_concurrent_tasks")) snprintf(val, cap, "%u", v % 9u);
    else if (!strcmp(leaf, "max_intake_depth"))     snprintf(val, cap, "%u", (v % 3u) ? 0u : 100u + v % 3000u);
    else if (!strcmp(leaf, "shutdown_grace_ms"))    snprintf(val, cap, "%u", 500u + v % 3000u);
    else if (!strcmp(leaf, "max_dead_letters"))     snprintf(val, cap, "%u", (v % 2u) ? 1024u : v % 30u);
    else if (!strcmp(leaf, "dead_letters_evicted")) snprintf(val, cap, "0");
    else if (!strcmp(leaf, "reserve_after_skips"))  snprintf(val, cap, "%u", v % 10u);
    else if (!strcmp(leaf, "mem_bytes"))            snprintf(val, cap, "%u", (v % 4u) * 128u);
    else if (!strcmp(leaf, "timeout_seconds") || !strcmp(leaf, "retry_backoff_seconds"))
                                                    snprintf(val, cap, "%u", (v % 4u) == 0 ? 1u : 0u);
    else                                            snprintf(val, cap, "%u", v % 4u);   /* retries, costs, budgets */
}

static size_t make_payload(uint64_t seed, int family, unsigned char *buf, size_t cap)
{
    uint64_t s = seed;
    size_t n, i;
    if (family == F_PROGRAM) {
        unsigned k = rint_(&s, 40);
        const char *t = k == 0 ? "nap\n" : k < 5 ? "fail\n" : "ok\n";
        n = strlen(t);
        memcpy(buf, t, n);
        return n;
    }
    if (rint_(&s, 20) == 0) return 0;                       /* no payload at all */
    n = 4 + rint_(&s, (unsigned)(cap - 4));
    buf[0] = PAY_MAGIC;
    buf[1] = (unsigned char)pick(&s, B_WEIGHT, B_N);
    buf[2] = (unsigned char)rint_(&s, 256);
    buf[3] = (unsigned char)((rint_(&s, 33) == 0 ? C_DENY : 0u) | (rint_(&s, 12) == 0 ? C_DEFER : 0u));
    for (i = 4; i < n; ++i) buf[i] = (unsigned char)rint_(&s, 256);
    return n;
}

/* The counts a gptps_task_get_info may report, while anything runs. The name is
 * borrowed and another thread may be unregistering that very type, so it is not read. */
static void scan_tasks(void)
{
    size_t i, n = gptps_task_count(g_e);
    int k;
    for (i = 0; i <= n; ++i) {
        gptps_task_info ti;
        gptps_status st;
        memset(&ti, 0, sizeof ti);
        ti.struct_size = sizeof ti;
        st = gptps_task_get_info(g_e, i, &ti);
        CHECKF(st == GPTPS_OK || st == GPTPS_E_NOTFOUND, "gptps_task_get_info(%u) = %d", (unsigned)i, (int)st);
        if (st != GPTPS_OK) break;                         /* the registry shrank meanwhile */
        CHECKF(ti.running <= g_conc, "a type with %u items admitted, on %u slots", ti.running, g_conc);
        CHECKF(ti.queued <= (uint32_t)get(&g_n.attempts) && ti.dead <= (uint32_t)get(&g_n.attempts),
               "a type with %u queued and %u dead, of %d submits", ti.queued, ti.dead, get(&g_n.attempts));
        CHECKF((ti.enabled == 0 || ti.enabled == 1) && (ti.removed == 0 || ti.removed == 1),
               "enabled %d, removed %d", ti.enabled, ti.removed);
        CHECKF((unsigned)ti.exec <= (unsigned)GPTPS_EXEC_PROGRAM, "exec kind %d", (int)ti.exec);
    }
    CHECKF(gptps_dead_letter_count(g_e) <= (size_t)get(&g_n.attempts), "%u dead letters", (unsigned)gptps_dead_letter_count(g_e));
    for (k = 0; k < NSLOTS; ++k) {          /* a type's flags, by name: safe while it is removed */
        uint64_t fl = 0;
        gptps_status st = gptps_task_flags(g_e, g_slots[k].name, &fl);
        int ex = gptps_task_exists(g_e, g_slots[k].name);
        CHECKF(st == GPTPS_OK || st == GPTPS_E_NOTFOUND, "gptps_task_flags(%s) = %d", g_slots[k].name, (int)st);
        CHECK(ex == 0 || ex == 1);
        if (st == GPTPS_OK)
            CHECKF(((fl & GPTPS_TASK_SERVICE) != 0) == is_service(g_slots[k].family) &&
                   ((fl & GPTPS_TASK_RETIRE_ON_OK) != 0) == (g_slots[k].family == F_RETIRE),
                   "%s has flags 0x%llx", g_slots[k].name, (unsigned long long)fl);
    }
}

/* Time spent in each kind of operation, for GPTPS_STRESS_DEBUG: a mix in which most
 * threads sit inside one blocking call tests little else. */
static int g_op_ms[OP_N], g_op_cnt[OP_N];
static void do_op_inner(tstate *ts);
static void do_op(tstate *ts)
{
    uint64_t t = gptps_now_ms(NULL);
    unsigned before = ts->nring;
    do_op_inner(ts);
    if (ts->nring != before) {
        unsigned op = ts->ring[before % RING].op;
        __atomic_add_fetch(&g_op_ms[op], (int)(gptps_now_ms(NULL) - t), __ATOMIC_SEQ_CST);
        __atomic_add_fetch(&g_op_cnt[op], 1, __ATOMIC_SEQ_CST);
    }
}
static void do_op_inner(tstate *ts)
{
    uint64_t r[6];
    unsigned op, i;
    int st = 0;
    slot *s;
    char key[160], val[GPTPS_SETTINGS_VALUE_MAX], why[256];

    /* A fixed number of draws per operation, whatever the engine answers, so the
     * sequence depends on the seed alone - which is what makes --replay replay it. */
    op = pick(&ts->rng, OP_WEIGHT, OP_N);
    for (i = 0; i < 6; ++i) r[i] = rnext(&ts->rng);
    s = &g_slots[r[0] % (unsigned)NSLOTS];
    put(&ts->cur, (int)op);

    switch (op) {
    case OP_SUBMIT:
    case OP_SUBMIT_EX: {
        unsigned char pay[64];
        size_t len;
        gptps_submit_options o;
        if (get(&g_n.live) > LIVE_MAX || (is_service(s->family) && get(&g_n.svc_live) >= SVC_MAX)) {
            inc(&g_n.throttled);
            if (get(&g_n.live) > LIVE_MAX) inc(&g_n.throttled_live);
            st = -1;
            break;
        }
        len = make_payload(r[1], s->family, pay, sizeof pay);
        memset(&o, 0, sizeof o);
        o.struct_size = sizeof o;
        o.flags = (unsigned)(r[2] % 8u);
        o.priority = (int32_t)(r[3] % 11u) - 5;
        o.policy.struct_size = sizeof o.policy;
        o.policy.max_retries = (uint32_t)(r[3] >> 8) % 3u;
        o.policy.retry_backoff_seconds = ((r[3] >> 16) % 6u == 0) ? 1u : 0u;
        o.policy.on_failure = (gptps_on_failure)((r[3] >> 24) % 3u);
        o.timeout_ms = 5u + (uint32_t)(r[4] % 60u);
        st = submit_tracked(s->name, len ? pay : NULL, len, op == OP_SUBMIT_EX ? &o : NULL);
        CHECKF((submit_status_ok(st) && st != GPTPS_E_SHUTDOWN) ||
               (g_bounded && st == GPTPS_E_INVAL && len > BOUNDED_PAYLOAD), "submit(%s) = %d", s->name, st);
        break;
    }
    case OP_CANCEL: {
        int last = get(&g_n.last_handle);
        gptps_handle h;
        switch (r[1] % 4u) {
            case 0:  h = (gptps_handle)last; break;
            case 1:  h = (gptps_handle)(last - (int)(r[2] % 20u)); break;
            case 2:  h = (gptps_handle)(1u + r[2] % (uint64_t)(last + 1)); break;
            default: h = (gptps_handle)(H_CAP + r[2] % 1000u); break;    /* never issued */
        }
        if ((int64_t)h <= 0) h = 1;
        st = gptps_cancel(g_e, h);
        CHECKF(st == GPTPS_OK || st == GPTPS_E_NOTFOUND, "cancel(%llu) = %d", (unsigned long long)h, st);
        if (h >= H_CAP) CHECKF(st == GPTPS_E_NOTFOUND, "cancel(%llu), a handle never issued, = %d", (unsigned long long)h, st);
        break;
    }
    case OP_GET: {
        size_t cap = (r[3] % 8u == 0) ? 4u : sizeof val;    /* a short buffer truncates, still OK */
        random_key(r[1], r[2], key, sizeof key);
        st = gptps_settings_get(g_e, key, val, cap);
        CHECKF(st == GPTPS_OK || st == GPTPS_E_NOTFOUND, "get(%s) = %d", key, st);
        if (st == GPTPS_OK) CHECKF(strlen(val) < cap, "get(%s) overran a %u-byte buffer", key, (unsigned)cap);
        break;
    }
    case OP_SET: {
        random_key(r[1], r[2], key, sizeof key);
        random_value(key, r[3], val, sizeof val);
        if (r[4] & 1u) {
            st = gptps_settings_set(g_e, key, val);
        } else {
            st = gptps_settings_set_ex(g_e, key, val, why, sizeof why);
            CHECKF(st == GPTPS_OK ? why[0] == 0 : why[0] != 0, "set_ex(%s, %s) = %d, why \"%s\"", key, val, st, why);
        }
        CHECKF(st == GPTPS_OK || st == GPTPS_E_NOTFOUND || st == GPTPS_E_CONFIG, "set(%s, %s) = %d", key, val, st);
        if (st == GPTPS_OK) inc(&g_n.sets_ok);
        break;
    }
    case OP_ENABLE:
        st = gptps_set_task_enabled(g_e, s->name, (r[1] % 4u) != 0);
        CHECKF(st == GPTPS_OK || st == GPTPS_E_NOTFOUND, "set_task_enabled(%s) = %d", s->name, st);
        break;
    case OP_CLONE: {
        slot *src = &g_slots[r[1] % (unsigned)NSLOTS];
        int was;
        if (src == s || src->family != s->family) { st = -1; break; }   /* within a family only */
        if (!claim(&s->busy)) { st = -2; break; }
        was = get(&s->registered);
        st = gptps_clone_task(g_e, src->name, s->name);
        if (was)            CHECKF(st == GPTPS_E_DUP || st == GPTPS_E_NOTFOUND, "clone(%s -> %s), a live name, = %d", src->name, s->name, st);
        else if (g_bounded) CHECKF(st == GPTPS_E_BUSY || st == GPTPS_E_NOTFOUND, "clone(%s -> %s), sealed, = %d", src->name, s->name, st);
        else                CHECKF(st == GPTPS_OK || st == GPTPS_E_NOTFOUND || (st == GPTPS_E_INVAL && is_service(s->family) && g_manual),
                                   "clone(%s -> %s) = %d", src->name, s->name, st);
        if (st == GPTPS_OK) { put(&s->registered, 1); inc(&g_n.clone_ok); }
        put(&s->busy, 0);
        break;
    }
    case OP_UNREGISTER: {
        unsigned flags = (unsigned)(r[1] % 3u);
        int was;
        if (!claim(&s->busy)) { st = -2; break; }
        was = get(&s->registered);
        st = gptps_unregister_task(g_e, s->name, flags);
        if (g_bounded)  CHECKF(st == GPTPS_E_BUSY, "unregister(%s, %u), sealed, = %d", s->name, flags, st);
        else if (was)   CHECKF(st == GPTPS_OK || st == GPTPS_E_BUSY, "unregister(%s, %u) = %d", s->name, flags, st);
        else            CHECKF(st == GPTPS_E_NOTFOUND, "unregister(%s, %u) of a removed type = %d", s->name, flags, st);
        if (st == GPTPS_OK) { put(&s->registered, 0); inc(&g_n.unreg_ok); }
        if (st == GPTPS_E_BUSY) inc(&g_n.unreg_busy);
        put(&s->busy, 0);
        break;
    }
    case OP_REGISTER: {
        int was;
        if (!claim(&s->busy)) { st = -2; break; }
        was = get(&s->registered);
        st = register_slot(s);
        CHECKF(register_expected(s, was, (gptps_status)st), "register(%s) = %d, registered before: %d", s->name, st, was);
        if (st == GPTPS_OK) { put(&s->registered, 1); inc(&g_n.reg_ok); }
        put(&s->busy, 0);
        break;
    }
    case OP_REBUDGET:
        st = gptps_define_resource(g_e, (r[1] & 1u) ? "gpu" : "io", r[2] % 5u);   /* a re-budget: live */
        CHECKF(st == GPTPS_OK, "re-budget = %d", st);
        break;
    case OP_RESCOST: {
        static const char *const RES[] = { "gpu", "io", "lic" };
        st = gptps_set_task_resource_cost(g_e, s->name, RES[r[1] % 3u], r[2] % 4u);
        CHECKF(st == GPTPS_OK || st == GPTPS_E_NOTFOUND, "set_task_resource_cost(%s) = %d", s->name, st);
        break;
    }
    case OP_PRIORITY:
        st = gptps_set_task_priority(g_e, s->name, (int)(r[1] % 11u) - 5);
        CHECKF(st == GPTPS_OK || st == GPTPS_E_NOTFOUND, "set_task_priority(%s) = %d", s->name, st);
        break;
    case OP_RELOAD:
        if (r[1] % 8u == 0) {
            st = gptps_settings_reload(g_e, g_bad[r[2] & 1u]);      /* refused, in part or whole */
            CHECKF(st == GPTPS_E_CONFIG || st == GPTPS_E_BUSY, "reload(%s) = %d", g_bad[r[2] & 1u], st);
        } else {
            unsigned of0 = open_failures_here();
            st = gptps_settings_reload(g_e, NULL);
            if (st != GPTPS_OK && st != GPTPS_E_BUSY && !RACED_A_REPLACE(st, of0)) {
                failed(__FILE__, __LINE__, "reload of a valid file = %d", st);
                dump_log();
            }
            inc(st == GPTPS_OK ? &g_n.reload_ok : st == GPTPS_E_BUSY ? &g_n.reload_busy : &g_n.reload_raced);
        }
        break;
    case OP_SAVE: {
        char path[128];
        if (r[1] % 4u == 0) {               /* to a new file: a copy of the loaded one */
            snprintf(path, sizeof path, "%s.new%d", g_cfg, ts->idx);
            remove(path);
            st = gptps_settings_save(g_e, path);
        } else {
            snprintf(path, sizeof path, "%s", g_cfg);
            st = gptps_settings_save(g_e, NULL);
        }
        if (st != GPTPS_OK && st != GPTPS_E_BUSY && !(REPLACE_MAY_FAIL && st == GPTPS_E_IO)) {
            failed(__FILE__, __LINE__, "save(%s) = %d", path, st);
            dump_log();
        }
        if (st == GPTPS_OK) {
            inc(&g_n.save_ok);
            if (r[2] % 3u == 0 || strcmp(path, g_cfg) != 0) check_reopens(path, "a saved file");
        } else {
            inc(&g_n.save_busy);
        }
        if (strcmp(path, g_cfg) != 0) remove(path);
        break;
    }
    case OP_REWRITE:
        write_config(g_cfg, r[1], ts->idx);
        break;
    case OP_CHECK:
        st = gptps_config_check(g_e);
        CHECKF(st == GPTPS_OK || st == GPTPS_E_CONFIG, "config_check = %d", st);
        break;
    case OP_DRAIN: {
        /* Whatever other threads drain or dead-letter meanwhile, the callback runs
         * once for each item this drain took, and returns before it does. */
        int heard = 0;
        size_t n = gptps_dead_letter_drain(g_e, (r[1] & 1u) ? on_dead : NULL, &heard);
        if (r[1] & 1u) CHECKF((size_t)heard == n, "a drain took %u dead letters and called back for %d",
                              (unsigned)n, heard);
        st = (int)n;
        break;
    }
    case OP_SCAN_TASKS:
        scan_tasks();
        break;
    case OP_SCAN_SETTINGS: {
        size_t n = gptps_settings_count(g_e), j, from = n ? (size_t)(r[1] % n) : 0;
        for (j = from; j < from + 8; ++j) {     /* key and desc are borrowed: only the inline values are read */
            gptps_setting_info in;
            gptps_status s2;
            memset(&in, 0, sizeof in);
            in.struct_size = sizeof in;
            s2 = gptps_settings_get_info(g_e, j, &in);
            CHECKF(s2 == GPTPS_OK || s2 == GPTPS_E_NOTFOUND, "settings_get_info(%u) = %d", (unsigned)j, (int)s2);
            if (s2 != GPTPS_OK) break;
            CHECK(memchr(in.value, 0, sizeof in.value) != NULL && memchr(in.defval, 0, sizeof in.defval) != NULL);
        }
        break;
    }
    case OP_USAGE: {
        static const char *const RES[] = { "gpu", "io", "lic" };
        uint64_t reserved = 0, budget = 0;
        st = gptps_resource_usage(g_e, RES[r[1] % 3u], &reserved, &budget);
        CHECKF(st == GPTPS_OK || (st == GPTPS_E_NOTFOUND && r[1] % 3u == 2), "resource_usage(%s) = %d", RES[r[1] % 3u], st);
        /* a release counted twice would wrap the unsigned sum far past anything admitted */
        CHECKF(reserved <= 3u * (uint64_t)g_conc, "%s: %llu reserved, by at most %u items costing at most 3",
               RES[r[1] % 3u], (unsigned long long)reserved, g_conc);
        break;
    }
    case OP_LOAD:
#if defined(ADDON_UNWIND_PATH)
        /* An add-on whose setup registers twenty types and then fails: the unwind must
         * take those and nothing the other threads register or remove meanwhile. One
         * load at a time; the others are told GPTPS_E_BUSY. Seldom: while a setup
         * runs, a reload is refused too. */
        if (r[1] % 16u == 0) {
            st = gptps_load_addon(g_e, ADDON_UNWIND_PATH);
            CHECKF(st == GPTPS_E_BUSY || (st == GPTPS_E_TASK && !g_bounded), "loading addon_unwind = %d", st);
            if (st == GPTPS_E_TASK) inc(&g_n.loads);
            break;
        }
#endif
        {
            size_t n = gptps_addon_count(g_e);
            gptps_addon_info ai;
            memset(&ai, 0, sizeof ai);
            ai.struct_size = sizeof ai;
            CHECKF(n == 0, "%u add-ons loaded, and every load fails", (unsigned)n);
            st = gptps_addon_get_info(g_e, 0, &ai);
            CHECKF(st == GPTPS_E_NOTFOUND, "addon_get_info(0) = %d, with nothing loaded", st);
        }
        break;
    case OP_EVENT_CB:
        st = (r[1] & 1u) ? gptps_set_event_cb(g_e, on_event2, &g_cb_tag[1]) : gptps_set_event_cb(g_e, on_event, &g_cb_tag[0]);
        CHECKF(st == GPTPS_OK, "gptps_set_event_cb = %d", st);
        break;
    default: break;
    }

    ts->ring[ts->nring % RING].op = (unsigned char)op;
    ts->ring[ts->nring % RING].slot = (unsigned char)(s - g_slots);
    ts->ring[ts->nring % RING].st = (short)st;
    ++ts->nring;
    ++ts->nops;
    put(&ts->cur, -1);
}

static void dump_state(void)
{
    int i;
    unsigned j;
    for (i = 0; i < g_nthreads; ++i) {
        tstate *ts = &g_ts[i];
        int cur = get(&ts->cur);
        printf("  thread %d: %d ops, now %s; last:", i, ts->nops, cur >= 0 && cur < OP_N ? OP_NAME[cur] : "-");
        for (j = ts->nring > 8 ? ts->nring - 8 : 0; j < ts->nring; ++j)
            printf(" %s(%s)=%d", OP_NAME[ts->ring[j % RING].op], g_slots[ts->ring[j % RING].slot].name, ts->ring[j % RING].st);
        printf("\n");
    }
    fflush(stdout);
}

/* ----------------------------------------------------------- the watchdog */

/* A hang is the failure this test must not wait out: CTest's timeout would say only
 * that it took too long. The watchdog says what each thread was doing, and aborts.
 * So it must speak before CTest stops the run, at 120 s (CMakeLists.txt): no deadline
 * is armed past the run's own end, its planned length plus RUN_SLACK_MS - 99 s for
 * the default run, whatever round it hangs in. */
#define RUN_SLACK_MS 90000u
static gptps_mutex *g_wdm;
static gptps_cond  *g_wdc;
static uint64_t     g_wd_deadline;   /* under g_wdm; 0 = unarmed */
static uint64_t     g_wd_end;        /* the run's own end; set before the first round */
static const char  *g_wd_what;
static int          g_wd_quit;
static void arm(unsigned ms, const char *what)
{
    uint64_t d;
    gptps_mutex_lock(g_wdm);
    d = gptps_now_ms(NULL) + ms;
    g_wd_deadline = d < g_wd_end ? d : g_wd_end;
    g_wd_what = what;
    gptps_mutex_unlock(g_wdm);
}
static void *watchdog_main(void *a)
{
    (void)a;
    gptps_mutex_lock(g_wdm);
    while (!g_wd_quit) {
        if (g_wd_deadline && gptps_now_ms(NULL) > g_wd_deadline) {
            printf("FAIL: HANG - %s did not finish in time\n", g_wd_what);
            dump_state();
            dump_log();
            fflush(stdout);
            abort();
        }
        gptps_cond_timedwait(g_wdc, g_wdm, 100);
    }
    gptps_mutex_unlock(g_wdm);
    return NULL;
}

/* -------------------------------------------------------------- one round */

static int g_ops_on;                 /* the operation threads keep going while 1 */
static int g_stepping;               /* the MANUAL stepper keeps going while 1 */

static void *op_thread(void *a)
{
    tstate *ts = (tstate *)a;
    while (get(&g_ops_on)) do_op(ts);
    put(&ts->done, 1);
    return NULL;
}

static void *stepper(void *a)
{
    (void)a;
    while (get(&g_stepping)) {
        size_t ran = 0;
        gptps_status st = gptps_step(g_e, &ran);
        CHECKF(st == GPTPS_OK, "gptps_step = %d", (int)st);
        if (ran == 0) nap(1);
    }
    return NULL;
}

/* Every handle a submit returned must close; cancel the open ones until they have.
 * Callbacks start nothing new now (g_n.quiesce), so the set only shrinks. */
static void cancel_until_closed(void)
{
    uint64_t t0 = gptps_now_ms(NULL), swept = 0;
    for (;;) {
        int h, top = get(&g_n.attempts) + 1, open = 0, sweep;
        uint64_t now = gptps_now_ms(NULL);
        if (top > H_CAP) top = H_CAP;
        sweep = (now - swept >= 100);
        if (sweep) swept = now;
        for (h = 1; h < top; ++h) {
            if (get(&g_h[h].submitted) != 1 || get(&g_h[h].terminal) != 0) continue;
            ++open;
            if (sweep) cancel_status(gptps_cancel(g_e, (gptps_handle)h), "the closing sweep");
        }
        if (!open) return;
        if (now - t0 > 20000) {
            int shown = 0;
            failed(__FILE__, __LINE__, "%d handles still open after 20 s of cancelling them", open);
            for (h = 1; h < top && shown < 8; ++h) {
                hrec *x = &g_h[h];
                if (get(&x->submitted) != 1 || get(&x->terminal) != 0) continue;
                printf("  handle %d: queued %d started %d finished %d failed %d+%d retried %d dead %d dropped %d\n",
                       h, get(&x->queued), get(&x->started), get(&x->finished), get(&x->failed_cancel),
                       get(&x->failed_other), get(&x->retried), get(&x->dead), get(&x->dropped));
                ++shown;
            }
            return;
        }
        nap(2);
    }
}

/* Once every handle is closed, nothing may be queued, running or holding a budget. */
static int engine_idle(int report)
{
    static const char *const RES[] = { "gpu", "io", "lic" };
    size_t i, n = gptps_task_count(g_e);
    int idle = 1;
    for (i = 0; i < n; ++i) {
        gptps_task_info ti;
        memset(&ti, 0, sizeof ti);
        ti.struct_size = sizeof ti;
        if (gptps_task_get_info(g_e, i, &ti) != GPTPS_OK) continue;
        if (ti.queued || ti.running) {
            idle = 0;
            if (report) failed(__FILE__, __LINE__, "%s: %u queued, %u running, after every handle closed", ti.name, ti.queued, ti.running);
        }
    }
    for (i = 0; i < 3; ++i) {
        uint64_t reserved = 0;
        if (gptps_resource_usage(g_e, RES[i], &reserved, NULL) == GPTPS_OK && reserved) {
            idle = 0;
            if (report) failed(__FILE__, __LINE__, "%s: %llu still reserved, with nothing running", RES[i], (unsigned long long)reserved);
        }
    }
    return idle;
}

/* No setting outlives its task, and every live task has all of its own - bound to it:
 * a value set through its priority setting is the one the task reports. */
static void check_settings_match_tasks(void)
{
    static const char *const LEAF[] = { "timeout_seconds", "max_retries", "retry_backoff_seconds", "mem_bytes",
                                        "priority", "on_failure", "weight", "resources.gpu", "resources.io", 0 };
    char live[NSLOTS];
    size_t i, n;
    int k, lic = gptps_resource_usage(g_e, "lic", NULL, NULL) == GPTPS_OK;
    memset(live, 0, sizeof live);
    n = gptps_task_count(g_e);
    for (i = 0; i < n; ++i) {
        gptps_task_info ti;
        memset(&ti, 0, sizeof ti);
        ti.struct_size = sizeof ti;
        if (gptps_task_get_info(g_e, i, &ti) != GPTPS_OK) { CHECK(0); continue; }
        CHECKF(!ti.removed, "%s is still being removed after every handle closed", ti.name);
        k = slot_index(ti.name);
        CHECKF(k >= 0, "a task the test never registered: %s", ti.name);
        if (k < 0) continue;
        CHECKF(!live[k], "%s is registered twice", ti.name);
        live[k] = 1;
    }
    for (k = 0; k < NSLOTS; ++k)
        CHECKF(live[k] == get(&g_slots[k].registered), "%s: the engine says %sregistered, its last register/unregister said %s",
               g_slots[k].name, live[k] ? "" : "not ", get(&g_slots[k].registered) ? "registered" : "removed");

    n = gptps_settings_count(g_e);
    for (i = 0; i < n; ++i) {
        gptps_setting_info in;
        memset(&in, 0, sizeof in);
        in.struct_size = sizeof in;
        if (gptps_settings_get_info(g_e, i, &in) != GPTPS_OK) { CHECK(0); continue; }
        if (strncmp(in.key, "tasks.", 6) != 0) continue;
        k = owner_slot(in.key + 6);
        CHECKF(k >= 0 && live[k], "the setting %s outlived its task", in.key);
    }

    for (k = 0; k < NSLOTS; ++k) {
        char key[160], v[GPTPS_SETTINGS_VALUE_MAX];
        int j;
        if (!live[k]) continue;
        for (j = 0; LEAF[j]; ++j) {
            snprintf(key, sizeof key, "tasks.%s.%s", g_slots[k].name, LEAF[j]);
            CHECKF(gptps_settings_get(g_e, key, v, sizeof v) == GPTPS_OK, "%s is registered without its setting %s", g_slots[k].name, key);
        }
        if (lic) {
            snprintf(key, sizeof key, "tasks.%s.resources.lic", g_slots[k].name);
            CHECKF(gptps_settings_get(g_e, key, v, sizeof v) == GPTPS_OK, "%s is registered without its setting %s", g_slots[k].name, key);
        }
        snprintf(key, sizeof key, "tasks.%s.priority", g_slots[k].name);
        snprintf(v, sizeof v, "%d", 40 + k);
        CHECKF(gptps_settings_set(g_e, key, v) == GPTPS_OK, "set(%s) on a live task", key);
        n = gptps_task_count(g_e);
        for (i = 0; i < n; ++i) {
            gptps_task_info ti;
            memset(&ti, 0, sizeof ti);
            ti.struct_size = sizeof ti;
            if (gptps_task_get_info(g_e, i, &ti) == GPTPS_OK && strcmp(ti.name, g_slots[k].name) == 0)
                CHECKF(ti.priority == 40 + k, "%s reports priority %d after its setting was set to %d", ti.name, ti.priority, 40 + k);
        }
    }
}

/* Every handle a submit returned has exactly one terminal event, for the observers and
 * the event callback alike; no other handle has any event; and each attempt's
 * FINISHED or FAILED had its STARTED. */
static void check_handles(const char *when)
{
    int h, bad = 0, sum = 0;
    for (h = 1; h < H_CAP; ++h) {
        hrec *x = &g_h[h];
        int sub = get(&x->submitted), term = get(&x->terminal);
        int any = get(&x->queued) + get(&x->started) + get(&x->finished) + get(&x->failed_cancel) +
                  get(&x->failed_other) + get(&x->retried) + get(&x->dead) + get(&x->dropped);
        if (!sub && !any) continue;
        sum += sub;
        if (bad >= 8) continue;
        if (sub > 1) { ++bad; failed(__FILE__, __LINE__, "%s: handle %d was returned by %d submits", when, h, sub); }
        if (!sub)    { ++bad; failed(__FILE__, __LINE__, "%s: handle %d has events, but no submit returned it", when, h); }
        if (!sub) continue;
        if (term != 1 || get(&x->cb_terminal) != term || get(&x->queued) != 1 ||
            get(&x->finished) + get(&x->failed_other) > get(&x->started)) {
            ++bad;
            failed(__FILE__, __LINE__, "%s: handle %d: %d terminal events (event callback: %d); queued %d started %d "
                   "finished %d failed %d+%d (cancel+other) retried %d dead %d dropped %d", when, h, term,
                   get(&x->cb_terminal), get(&x->queued), get(&x->started), get(&x->finished),
                   get(&x->failed_cancel), get(&x->failed_other), get(&x->retried), get(&x->dead), get(&x->dropped));
        }
    }
    CHECKF(sum == get(&g_n.submit_ok), "%s: %d handles returned, %d submits succeeded", when, sum, get(&g_n.submit_ok));
    CHECKF(get(&g_n.untracked) == 0, "%s: %d events for handles outside the table", when, get(&g_n.untracked));
}

typedef struct {
    uint64_t seed;
    int      manual, hot, sched, bounded, replay;
    unsigned ms;
} round_opts;

static void open_engine(const round_opts *o)
{
    gptps_config cfg;
    int k;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.config_path = g_cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = g_conc;
    cfg.mode = o->manual ? GPTPS_RUN_MANUAL : GPTPS_RUN_THREADED;
    if (o->bounded) {   /* docs/BOUNDED.md: few enough items that the pool runs dry */
        cfg.max_items = 64u + (o->seed >> 8) % 448u;
        cfg.max_payload_bytes = BOUNDED_PAYLOAD;
        cfg.max_result_bytes = 64;
    }
    g_e = NULL;
    CHECKF(gptps_open_ex(&cfg, &g_e) == GPTPS_OK, "gptps_open_ex(%s)", g_cfg);
    if (!g_e) { print_file(g_cfg); dump_log(); return; }

    CHECK(gptps_register_observer(g_e, observer, NULL) == GPTPS_OK);
    CHECK(gptps_set_event_cb(g_e, on_event, &g_cb_tag[0]) == GPTPS_OK);
    CHECK(gptps_settings_watch(g_e, watcher, NULL) == GPTPS_OK);
    CHECK(gptps_register_constraint(g_e, gate, NULL) == GPTPS_OK);
    if (o->sched) CHECK(gptps_set_scheduler(g_e, sched_age, NULL) == GPTPS_OK);
    CHECK(gptps_define_resource(g_e, "gpu", 2) == GPTPS_OK);
    CHECK(gptps_define_resource(g_e, "io", 4) == GPTPS_OK);
    if (o->bounded) CHECK(gptps_define_resource(g_e, "lic", 2) == GPTPS_OK);   /* a reload may only re-budget it */
    CHECK(gptps_define_global(g_e, "host.knob", GPTPS_SETTING_UINT, "5", "0..100", 0) == GPTPS_OK);
    CHECK(gptps_define_global(g_e, "host.mode", GPTPS_SETTING_ENUM, "a", "a|b|c", 0) == GPTPS_OK);
    CHECK(gptps_define_global(g_e, "host.label", GPTPS_SETTING_STRING, "x", NULL, 0) == GPTPS_OK);
    CHECK(gptps_define_task_setting(g_e, "weight", GPTPS_SETTING_UINT, "1", "0..10", 0) == GPTPS_OK);
    snprintf(g_note[0], sizeof g_note[0], "t0");
    snprintf(g_note[1], sizeof g_note[1], "t0.x");
    host_setting("host.go", GPTPS_SETTING_UINT, go_read, go_write, NULL);
    /* host keys under task prefixes: t0's goes with t0, and t0.x's must survive t0 */
    host_setting("tasks.t0.note", GPTPS_SETTING_STRING, note_read, note_write, g_note[0]);
    host_setting("tasks.t0.x.note", GPTPS_SETTING_STRING, note_read, note_write, g_note[1]);
    for (k = 0; k < NSLOTS; ++k) {
        slot *s = &g_slots[k];
        s->registered = 0; s->busy = 0; s->gen = 0;
        if (s->at_setup) {
            gptps_status st = register_slot(s);
            CHECKF(register_expected(s, 0, st), "register(%s) at setup = %d", s->name, (int)st);
            if (st == GPTPS_OK) s->registered = 1;
        }
    }
    (void)gptps_set_task_resource_cost(g_e, "t1", "gpu", 1);
    (void)gptps_set_task_resource_cost(g_e, "t2", "io", 2);
    (void)gptps_set_task_resource_cost(g_e, "p0", "io", 1);
    if (o->bounded) {
        /* Seal it now. Its lifecycle is setup, then the first submit, then work
         * (docs/BOUNDED.md): a setup call still running when another thread's first
         * submit seals the engine is outside it. Sealed, every setup call is refused. */
        static const unsigned char pay[4] = { PAY_MAGIC, B_OK, 0, 0 };
        CHECK(submit_tracked("t0", pay, sizeof pay, NULL) == GPTPS_OK);
        put(&g_n.sealed, 1);
    }
}

static void run_round(const round_opts *o, int round)
{
    gptps_thread *th[MAX_THREADS], *stp = NULL, *wd;
    uint64_t t0, dt, grace = 1000;
    int i, nops;

    memset(&g_n, 0, sizeof g_n);
    memset(g_h, 0, H_CAP * sizeof *g_h);
    gptps_mutex_lock(g_logm);
    g_nofail = 0;                       /* this round's threads are new ones */
    gptps_mutex_unlock(g_logm);
    g_manual = o->manual;
    g_bounded = o->bounded;
    snprintf(g_cfg, sizeof g_cfg, "stress_api_%08x.toml", (unsigned)(o->seed & 0xffffffffu));
    snprintf(g_bad[0], sizeof g_bad[0], "stress_api_%08x_bad0.toml", (unsigned)(o->seed & 0xffffffffu));
    snprintf(g_bad[1], sizeof g_bad[1], "stress_api_%08x_bad1.toml", (unsigned)(o->seed & 0xffffffffu));
    write_config(g_cfg, o->seed, 99);
    /* one that parses with a value the engine refuses - but that a fresh open leaves
     * waiting, so a copy of it still reopens - and one that does not parse */
    write_file(g_bad[0], "[host]\nknob = 999\n[scheduler]\nreserve_after_skips = 4\n");
    write_file(g_bad[1], "[scheduler\nreserve_after_skips = 6\n");

    printf("round %d: seed 0x%016llx, %s%s, %s shutdown%s%s, %u ms\n", round, (unsigned long long)o->seed,
           o->manual ? "MANUAL" : "THREADED", o->bounded ? " bounded" : "", o->hot ? "hot" : "quiet",
           o->sched ? ", scheduler hook" : "", o->replay ? ", replayed on one thread" : "", o->ms);
    fflush(stdout);

    g_wd_quit = 0; g_wd_deadline = 0;
    wd = gptps_thread_start(watchdog_main, NULL);
    CHECK(wd != NULL);
    arm(60000, "opening the engine");
    open_engine(o);
    if (!g_e) { gptps_mutex_lock(g_wdm); g_wd_quit = 1; gptps_mutex_unlock(g_wdm); if (wd) gptps_thread_join(wd); return; }

    for (i = 0; i < g_nthreads; ++i) {
        memset(&g_ts[i], 0, sizeof g_ts[i]);
        g_ts[i].idx = i;
        g_ts[i].rng = mix64(o->seed + 0x1000u * (uint64_t)(i + 1));
        g_ts[i].cur = -1;
    }
    arm(o->ms + 60000, "the operations");
    put(&g_ops_on, 1);
    t0 = gptps_now_ms(NULL);
    if (o->replay) {
        /* the same streams, one operation from each in turn, on this thread */
        while (gptps_now_ms(NULL) - t0 < o->ms) {
            for (i = 0; i < g_nthreads; ++i) do_op(&g_ts[i]);
            if (o->manual) { size_t ran; CHECK(gptps_step(g_e, &ran) == GPTPS_OK); }
        }
    } else {
        if (o->manual) {
            put(&g_stepping, 1);
            stp = gptps_thread_start(stepper, NULL);
            CHECK(stp != NULL);
        }
        for (i = 0; i < g_nthreads; ++i) { th[i] = gptps_thread_start(op_thread, &g_ts[i]); CHECK(th[i] != NULL); }
        while (gptps_now_ms(NULL) - t0 < o->ms) nap(20);
        put(&g_ops_on, 0);
        for (i = 0; i < g_nthreads; ++i) if (th[i]) gptps_thread_join(th[i]);
    }
    put(&g_ops_on, 0);
    put(&g_n.quiesce, 1);
    if (getenv("GPTPS_STRESS_DEBUG")) {
        size_t n = gptps_task_count(g_e), j;
        unsigned q = 0, ru = 0, dd = 0;
        for (j = 0; j < n; ++j) {
            gptps_task_info ti;
            memset(&ti, 0, sizeof ti); ti.struct_size = sizeof ti;
            if (gptps_task_get_info(g_e, j, &ti) == GPTPS_OK) { q += ti.queued; ru += ti.running; dd += ti.dead; }
        }
        printf("  after the operations: live %d, svc %d, queued %u, running %u, dead %u, dead letters %u, throttled for the backlog %d\n",
               get(&g_n.live), get(&g_n.svc_live), q, ru, dd, (unsigned)gptps_dead_letter_count(g_e), get(&g_n.throttled_live));
        for (j = 0; j < OP_N; ++j) { printf("    %-14s %6d ops %6d ms\n", OP_NAME[j], g_op_cnt[j], g_op_ms[j]); g_op_cnt[j] = g_op_ms[j] = 0; }
    }

    if (!o->hot) {
        /* The quiet ending: close every handle by cancelling it, then hold the engine
         * to what an idle engine must look like. */
        uint64_t w;
        arm(60000, "closing every handle");
        if (o->replay && o->manual) { put(&g_stepping, 1); stp = gptps_thread_start(stepper, NULL); }
        /* sweep again while waiting: a callback that was past its quiesce check when it
         * was set may still have submitted one more */
        for (w = gptps_now_ms(NULL); ; nap(5)) {
            cancel_until_closed();
            if (engine_idle(0) || gptps_now_ms(NULL) - w > 10000) break;
        }
        (void)engine_idle(1);
        CHECKF(get(&g_n.watched) == get(&g_n.sets_ok), "the watcher heard %d sets, %d succeeded",
               get(&g_n.watched), get(&g_n.sets_ok));
        check_handles("before shutdown");
        check_settings_match_tasks();
    }

    if (stp) { put(&g_stepping, 0); gptps_thread_join(stp); stp = NULL; }
    CHECK(gptps_settings_set(g_e, "limits.shutdown_grace_ms", "1000") == GPTPS_OK);
    arm((unsigned)grace + 60000, "gptps_shutdown");
    t0 = gptps_now_ms(NULL);
    CHECK(gptps_shutdown(g_e) == GPTPS_OK);
    dt = gptps_now_ms(NULL) - t0;
    g_e = NULL;
    /* the grace, plus what it takes to stop: a body polls each millisecond, a child is
     * killed within ~200 ms - and a sanitizer slows every step of it */
    CHECKF(dt <= grace + 4000, "gptps_shutdown took %llu ms, with a %llu ms grace", (unsigned long long)dt, (unsigned long long)grace);
    check_handles("after shutdown");
    arm(60000, "reopening the config file");
    check_reopens(g_cfg, "the config file the round left");

    for (i = 0, nops = 0; i < g_nthreads; ++i) nops += g_ts[i].nops;
    printf("  ops %d, submits %d (%d throttled), reloads %d (+%d busy, +%d raced a replace), saves %d (+%d busy), reopened %d, "
           "registers %d, clones %d, unregisters %d (+%d busy; from callbacks %d of %d), drained %d, resubmits %d, "
           "accessor writes %d, failed loads unwound %d, shutdown %llu ms\n",
           nops, get(&g_n.submit_ok), get(&g_n.throttled), get(&g_n.reload_ok), get(&g_n.reload_busy), get(&g_n.reload_raced),
           get(&g_n.save_ok), get(&g_n.save_busy), get(&g_n.reopened), get(&g_n.reg_ok), get(&g_n.clone_ok),
           get(&g_n.unreg_ok), get(&g_n.unreg_busy), get(&g_n.unreg_cb), get(&g_n.unreg_cb_tried), get(&g_n.drained),
           get(&g_n.resubmits), get(&g_n.go_writes), get(&g_n.loads), (unsigned long long)dt);
    fflush(stdout);

    gptps_mutex_lock(g_wdm); g_wd_quit = 1; gptps_cond_signal(g_wdc); gptps_mutex_unlock(g_wdm);
    if (wd) gptps_thread_join(wd);
    {
        char p[128];
        remove(g_cfg); remove(g_bad[0]); remove(g_bad[1]);
        snprintf(p, sizeof p, "%s.tmp", g_cfg); remove(p);
        for (i = 0; i < MAX_THREADS; ++i) {
            snprintf(p, sizeof p, "%s.w%d", g_cfg, i); remove(p);
            snprintf(p, sizeof p, "%s.new%d", g_cfg, i); remove(p);
        }
        snprintf(p, sizeof p, "%s.w99", g_cfg); remove(p);
    }
}

/* ------------------------------------------------------------------- main */

static uint64_t parse_u64(const char *s) { return (uint64_t)strtoull(s, NULL, 0); }

int main(int argc, char **argv)
{
    uint64_t seed = 0;
    int have_seed = 0, rounds = -1, force_mode = -1, force_hot = -1, force_bounded = -1, replay = 0, keep_going = 0, r, i;
    unsigned total_ms = 10000, round_ms = 1500;
    const char *env;

    if ((env = getenv("GPTPS_STRESS_SEED")) != NULL && *env) { seed = parse_u64(env); have_seed = 1; }
    if ((env = getenv("GPTPS_STRESS_MS")) != NULL && *env) total_ms = (unsigned)strtoul(env, NULL, 10);
    g_nthreads = 8;
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc)          { seed = parse_u64(argv[++i]); have_seed = 1; }
        else if (!strcmp(argv[i], "--ms") && i + 1 < argc)       total_ms = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--round-ms") && i + 1 < argc) round_ms = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--rounds") && i + 1 < argc)   rounds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc)  g_nthreads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc)     { ++i; force_mode = !strcmp(argv[i], "manual"); }
        else if (!strcmp(argv[i], "--hot") && i + 1 < argc)      force_hot = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bounded") && i + 1 < argc)  force_bounded = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--replay"))                   replay = 1;
        else if (!strcmp(argv[i], "--keep-going"))               keep_going = 1;
        else { printf("usage: %s [--seed S] [--ms TOTAL] [--round-ms MS] [--rounds N] [--threads N] "
                      "[--mode threaded|manual] [--hot 0|1] [--bounded 0|1] [--replay] [--keep-going]\n", argv[0]); return 2; }
    }
    if (g_nthreads < 1) g_nthreads = 1;
    if (g_nthreads > MAX_THREADS) g_nthreads = MAX_THREADS;
    if (!have_seed) seed = mix64((uint64_t)time(NULL) ^ (gptps_now_ms(NULL) << 20) ^ (uint64_t)(size_t)&seed);
    if (rounds < 0) rounds = (int)(total_ms / (round_ms ? round_ms : 1));
    if (rounds < 1) rounds = 1;

    g_logm = gptps_mutex_create(); g_napm = gptps_mutex_create(); g_napc = gptps_cond_create();
    g_wdm = gptps_mutex_create();  g_wdc = gptps_cond_create();
    g_h = (hrec *)calloc(H_CAP, sizeof *g_h);
    if (!g_logm || !g_napm || !g_napc || !g_wdm || !g_wdc || !g_h) { printf("FAIL: setup\n"); return 1; }
    gptps_set_log_sink(sink, NULL);

    g_wd_end = gptps_now_ms(NULL) + (uint64_t)rounds * round_ms + RUN_SLACK_MS;
    printf("stress_api: seed 0x%016llx%s, %d rounds of %u ms, %d threads\n", (unsigned long long)seed,
           have_seed ? "" : " (from the clock; GPTPS_STRESS_SEED or --seed pins it)", rounds, round_ms, g_nthreads);
    for (r = 0; r < rounds; ++r) {
        round_opts o;
        uint64_t k;
        int before;
        o.seed = r == 0 ? seed : mix64(seed + (uint64_t)r);
        k = mix64(o.seed ^ 0x5bd1e995u);
        o.manual = force_mode >= 0 ? force_mode : (int)(k & 1u);
        o.hot = force_hot >= 0 ? force_hot : (int)((k >> 1) & 1u);
        o.sched = (int)((k >> 2) & 3u) == 0;
        o.bounded = force_bounded >= 0 ? force_bounded : (int)((k >> 4) & 3u) == 0;
        o.replay = replay;
        o.ms = round_ms;
        before = get(&fails);
        run_round(&o, r);
        if (get(&fails) != before) {
            printf("reproduce: test_stress_api --seed 0x%016llx --rounds 1 --round-ms %u --threads %d --mode %s --hot %d --bounded %d"
                   " (add --replay for one thread)\n", (unsigned long long)o.seed, round_ms, g_nthreads,
                   o.manual ? "manual" : "threaded", o.hot, o.bounded);
            if (!keep_going) break;
        }
    }

    gptps_set_log_sink(NULL, NULL);
    free(g_h);
    gptps_cond_destroy(g_wdc); gptps_mutex_destroy(g_wdm);
    gptps_cond_destroy(g_napc); gptps_mutex_destroy(g_napm); gptps_mutex_destroy(g_logm);
    if (get(&fails)) { printf("%d stress_api check(s) FAILED\n", get(&fails)); return 1; }
    printf("all stress_api checks passed\n");
    return 0;
}
