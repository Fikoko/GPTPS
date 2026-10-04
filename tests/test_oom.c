/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_oom.c - any allocation the engine makes may fail, and nothing breaks.
 *
 * The core routes every allocation through gptps_set_allocator (src/alloc.c), and
 * gptps.h promises that a hook returning NULL "surfaces as GPTPS_E_NOMEM rather
 * than crashing". Nothing else in the suite fails one. This test does, one at a
 * time: its allocator fails the Nth allocation counted from the start of a
 * scenario, and each scenario runs for N = 1, 2, 3, ... until a run completes with
 * no failure injected. For every N:
 *   - no crash and no leak: the allocator's live block count is back to 0 after
 *     gptps_shutdown (and ASan/LSan, in the sanitizer build, stays quiet);
 *   - no hang: a watchdog thread aborts a run that stops making progress;
 *   - every call returns what it returns with memory to spare, or GPTPS_E_NOMEM
 *     from the call the failure was injected into - and then the same call, made
 *     again, returns what it would have;
 *   - the engine is as usable after a refusal as before: everything the scenario
 *     observes - each event, result, dead letter, setting and saved file - is what
 *     the run with no failure observed.
 * The engines run in MANUAL mode, so the order of allocations is the same each run.
 * The scenarios: the work path; a config file, checked, reloaded and saved; the
 * settings API; a bounded engine's seal; the durable queue add-on; a THREADED
 * engine, whose order is not deterministic - there one allocation in 50 fails at
 * random, from fixed seeds, and only crashes, leaks, hangs and lost events count;
 * and two threads, one defining or registering while the other does too, where each
 * call must publish all it makes or none of it (scenario 7).
 *
 * In the environment, OOM_VERBOSE=1 prints every failure rather than the first at
 * each line, and each call that absorbed an injected failure; OOM_DUMP=n prints what
 * run N=n observed beside the reference.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "gptps.h"
#include "gptps_hal.h"
#if defined(OOM_WITH_DQ)
#  include "gptps_durable_queue.h"
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
static int verbose = 0;
static long g_dump_n;               /* OOM_DUMP=n: print run n's record beside the reference */
static const char *g_scn = "";      /* the scenario running */
static long g_n;                    /* the allocation it fails; 0 for none */
static const char *g_at;            /* which submit, when a failure is in the helper's */

/* A failure is printed once per source line, with the N it first happened at, and
 * counted after that: one bug would otherwise print a line for every N. */
#define MAX_SITES 128
static struct { int line; long count, first_n; const char *scn; } g_sites[MAX_SITES];
static int g_nsites;
static void report(int line, const char *fmt, ...)
{
    int i, first = 1;
    va_list ap;
    ++fails;
    for (i = 0; i < g_nsites; ++i)
        if (g_sites[i].line == line && g_sites[i].scn == g_scn) { ++g_sites[i].count; first = 0; break; }
    if (first && g_nsites < MAX_SITES) {
        g_sites[g_nsites].line = line; g_sites[g_nsites].count = 1;
        g_sites[g_nsites].first_n = g_n; g_sites[g_nsites].scn = g_scn;
        ++g_nsites;
    }
    if (!first && !verbose) return;
    printf("FAIL %s:%d [%s, N=%ld%s%s]: ", __FILE__, line, g_scn, g_n, g_at ? ", submit " : "", g_at ? g_at : "");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}
#define CHECK(c) do { if (!(c)) report(__LINE__, "%s", #c); } while (0)

/* ---- the failing allocator ---------------------------------------------------
 * Counts every malloc_fn and realloc_fn call once armed, and fails the a_fail_at-th
 * - or, for the THREADED engine, one call in OOM_RANDOM_DEN chosen by a seeded hash
 * of its number. Live blocks are counted, so a leak is a count above zero after
 * shutdown. The lock comes from the HAL, which allocates through libc, not here. */
#define OOM_RANDOM_DEN 50u
static gptps_mutex *a_m;
static long a_live, a_count, a_fail_at, a_injected;
static int  a_armed, a_random, a_paused;
static uint64_t a_seed;

static uint64_t mix64(uint64_t x)
{
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27; x *= 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static int fail_now(void)                       /* a_m held */
{
    if (!a_armed || a_paused) return 0;
    ++a_count;
    if (a_random ? mix64(a_seed + (uint64_t)a_count) % OOM_RANDOM_DEN == 0 : a_count == a_fail_at) {
        ++a_injected;
        return 1;
    }
    return 0;
}

/* One thread stopped mid-call (scenario 7): the thread p_tid stops at its p_at-th
 * allocation counted from its mark, until the test lets it go, and then that
 * allocation fails if p_fail says so. */
#define P_ARMED    1
#define P_PAUSED   2
#define P_RELEASED 3
#define P_DONE     4
static gptps_cond *p_c;                 /* with a_m */
static uint64_t p_tid;
static long p_at, p_count;
static int  p_fail, p_state;
static int pause_here(void)             /* a_m held; 1: fail this allocation */
{
    if (!p_tid || gptps_hal_thread_id() != p_tid || ++p_count != p_at) return 0;
    p_state = P_PAUSED;
    gptps_cond_broadcast(p_c);
    while (p_state != P_RELEASED) gptps_cond_wait(p_c, a_m);
    return p_fail;
}
/* The other thread of scenario 7: its q_at-th allocation, counted from its mark, fails. */
static uint64_t q_tid;
static long q_at, q_count;
static int  q_hit;
static int fail_other(void)             /* a_m held */
{
    if (!q_tid || gptps_hal_thread_id() != q_tid || ++q_count != q_at) return 0;
    q_hit = 1;
    return 1;
}

static void *f_malloc(size_t n, void *ud)
{
    void *p = NULL;
    (void)ud;
    gptps_mutex_lock(a_m);
    if (!pause_here() && !fail_other() && !fail_now() && (p = malloc(n ? n : 1)) != NULL) ++a_live;
    gptps_mutex_unlock(a_m);
    return p;
}
static void *f_realloc(void *p, size_t n, void *ud)
{
    void *q = NULL;
    (void)ud;
    gptps_mutex_lock(a_m);
    if (!pause_here() && !fail_other() && !fail_now()) q = realloc(p, n ? n : 1);
    gptps_mutex_unlock(a_m);
    return q;
}
static void f_free(void *p, void *ud)
{
    (void)ud;
    gptps_mutex_lock(a_m);
    free(p);
    --a_live;
    gptps_mutex_unlock(a_m);
}

static void arm(long fail_at, int random, uint64_t seed)
{
    gptps_mutex_lock(a_m);
    a_count = 0; a_injected = 0; a_fail_at = fail_at;
    a_random = random; a_seed = seed; a_armed = 1;
    gptps_mutex_unlock(a_m);
}
static void disarm(void) { gptps_mutex_lock(a_m); a_armed = 0; gptps_mutex_unlock(a_m); }
#if defined(OOM_WITH_DQ)
/* Neither counted nor failed meanwhile: set-up another scenario sweeps already. */
static void pause_count(int on) { gptps_mutex_lock(a_m); a_paused = on; gptps_mutex_unlock(a_m); }
#endif
static long injected(void) { long n; gptps_mutex_lock(a_m); n = a_injected; gptps_mutex_unlock(a_m); return n; }
static long live(void)     { long n; gptps_mutex_lock(a_m); n = a_live; gptps_mutex_unlock(a_m); return n; }
static long counted(void)  { long n; gptps_mutex_lock(a_m); n = a_count; gptps_mutex_unlock(a_m); return n; }

static const char *st_name(gptps_status s)
{
    static const char *const N[] = { "OK", "E_NOMEM", "E_INVAL", "E_NOTFOUND", "E_DUP", "E_BUDGET",
        "E_FULL", "E_TIMEOUT", "E_CANCELLED", "E_ABI", "E_CONFIG", "E_IO", "E_TASK", "E_SHUTDOWN",
        "E_DENIED", "E_BUSY" };
    return (unsigned)s < sizeof N / sizeof N[0] ? N[s] : "?";
}

/* One call that must end with `want`. GPTPS_E_NOMEM is accepted only from the call
 * the failure was injected into, and then the same call, made again with memory to
 * spare, must end with `want`. A call that absorbed a failure and still returned
 * `want` is counted (OOM_VERBOSE lists them): what it should have done shows in
 * what the scenario observes afterwards. */
static long g_refusals, g_absorbed;
static void absorbed(int line, const char *call)
{
    ++g_absorbed;
    if (verbose) printf("  [%s, N=%ld] line %d absorbed the failure: %s\n", g_scn, g_n, line, call);
}
#define TRY(want, call) do {                                                        \
        long inj0_ = injected();                                                    \
        gptps_status st_ = (call);                                                  \
        if (st_ == GPTPS_E_NOMEM && injected() != inj0_) { ++g_refusals; st_ = (call); } \
        else if (st_ == (want) && injected() != inj0_) absorbed(__LINE__, #call);   \
        if (st_ != (want))                                                          \
            report(__LINE__, "%s gave %s, want %s%s", #call, st_name(st_), st_name(want), \
                   injected() != inj0_ ? "" : " (no failure was injected into it)"); \
    } while (0)

/* ---- the watchdog --------------------------------------------------------------
 * A run that stops making progress for OOM_HANG_MS is a hang: say where, and abort. */
#define OOM_HANG_MS 30000u
static gptps_mutex *w_m;
static gptps_cond  *w_c;
static int w_stop;
static uint64_t w_last;
static void beat(void) { gptps_mutex_lock(w_m); w_last = gptps_hal_monotonic_ms(); gptps_mutex_unlock(w_m); }
static void *watchdog(void *arg)
{
    (void)arg;
    gptps_mutex_lock(w_m);
    while (!w_stop) {
        gptps_cond_timedwait(w_c, w_m, 500);
        if (!w_stop && gptps_hal_monotonic_ms() - w_last > OOM_HANG_MS) {
            printf("FAIL hang: [%s, N=%ld] made no progress for %u ms\n", g_scn, g_n, OOM_HANG_MS);
            fflush(stdout);
            abort();
        }
    }
    gptps_mutex_unlock(w_m);
    return NULL;
}


/* ---- what a scenario observes ------------------------------------------------
 * One line per event, result, dead letter, setting, task and saved file, in the
 * order they happen. Compared with the line-for-line record of the run with no
 * failure. Held in libc memory: the test's own bookkeeping is not the engine's. */
static char  *g_fp;
static size_t g_fplen, g_fpcap;
static void fp(const char *fmt, ...)
{
    char line[600];
    int k;
    va_list ap;
    va_start(ap, fmt);
    k = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (k < 0) return;
    if ((size_t)k >= sizeof line) k = (int)sizeof line - 1;
    if (g_fplen + (size_t)k + 2 > g_fpcap) {
        size_t nc = g_fpcap ? g_fpcap * 2 : 65536;
        char *grown;
        while (nc < g_fplen + (size_t)k + 2) nc *= 2;
        grown = (char *)realloc(g_fp, nc);
        if (!grown) { printf("test out of memory\n"); exit(2); }
        g_fp = grown; g_fpcap = nc;
    }
    memcpy(g_fp + g_fplen, line, (size_t)k);
    g_fplen += (size_t)k;
    g_fp[g_fplen++] = '\n';
    g_fp[g_fplen] = 0;
}

/* Handles by name: a submit names the handle its QUEUED announces. */
static struct { gptps_handle h; char label[24]; } g_lab[128];
static int g_nlab;
static const char *g_next_label;
static const char *label_of(gptps_handle h)
{
    int i;
    for (i = 0; i < g_nlab; ++i) if (g_lab[i].h == h) return g_lab[i].label;
    return "?";
}
static void name_handle(gptps_handle h)
{
    if (!g_next_label || g_nlab == (int)(sizeof g_lab / sizeof g_lab[0])) return;
    g_lab[g_nlab].h = h;
    snprintf(g_lab[g_nlab].label, sizeof g_lab[g_nlab].label, "%s", g_next_label);
    ++g_nlab;
}

static void render(char *out, size_t cap, const void *b, size_t n)
{
    size_t i, k = 0;
    const unsigned char *p = (const unsigned char *)b;
    for (i = 0; p && i < n && k + 2 < cap; ++i) out[k++] = (p[i] >= 32 && p[i] < 127) ? (char)p[i] : '.';
    out[k] = 0;
}

static const char *const KIND[] = { "QUEUED", "STARTED", "FINISHED", "FAILED", "RETRIED",
                                    "DEAD_LETTERED", "DROPPED" };
static long g_observed;
static void on_event(const gptps_event *ev, void *ud)
{
    char res[64];
    (void)ud;
    if (ev->kind == GPTPS_EV_QUEUED) name_handle(ev->handle);
    render(res, sizeof res, ev->result, ev->result_len);
    fp("ev %s %s %s st=%s att=%u fl=%u res=%s", (unsigned)ev->kind < 7 ? KIND[ev->kind] : "?",
       label_of(ev->handle), ev->task_name ? ev->task_name : "-", st_name(ev->status),
       (unsigned)ev->attempt, (unsigned)ev->flags, res);
}
static void on_observe(const gptps_event *ev, void *ud) { (void)ev; (void)ud; ++g_observed; }

/* The state as a host can read it: every setting, task and named resource. */
static void dump(gptps *e)
{
    size_t i, n = gptps_settings_count(e);
    static const char *const RES[] = { "gpu", "disk", "nstest.slots", 0 };
    for (i = 0; i < n; ++i) {
        gptps_setting_info si;
        memset(&si, 0, sizeof si);
        si.struct_size = sizeof si;
        if (gptps_settings_get_info(e, i, &si) != GPTPS_OK) { fp("setting %u unreadable", (unsigned)i); continue; }
        /* tests/addon_demo.c ignores what registering its setting returns, so after a
         * failure there that setting is missing by the add-on's own choice */
        if (strncmp(si.key, "demo.", 5) == 0) continue;
        fp("set %s = %s (default %s, %s)", si.key, si.value, si.defval, si.desc);
    }
    for (i = 0; ; ++i) {
        gptps_task_info ti;
        memset(&ti, 0, sizeof ti);
        ti.struct_size = sizeof ti;
        if (gptps_task_get_info(e, i, &ti) != GPTPS_OK) break;
        fp("task %s prio=%d on=%d removed=%d q=%u run=%u dead=%u retries=%u mem=%llu", ti.name,
           (int)ti.priority, ti.enabled, ti.removed, (unsigned)ti.queued, (unsigned)ti.running,
           (unsigned)ti.dead, (unsigned)ti.default_policy.max_retries,
           (unsigned long long)ti.default_cost.mem_bytes);
    }
    for (i = 0; RES[i]; ++i) {
        uint64_t r = 0, b = 0;
        if (gptps_resource_usage(e, RES[i], &r, &b) == GPTPS_OK)
            fp("resource %s %llu/%llu", RES[i], (unsigned long long)r, (unsigned long long)b);
    }
    fp("dead letters %u, observed %ld", (unsigned)gptps_dead_letter_count(e), g_observed);
}

/* Step until a step runs nothing. */
static int g_step_injected;         /* a failure landed inside gptps_step */
static void drain(gptps *e)
{
    int k;
    for (k = 0; k < 100; ++k) {
        size_t ran = 0;
        long inj0 = injected();
        TRY(GPTPS_OK, gptps_step(e, &ran));
        if (injected() != inj0) g_step_injected = 1;
        beat();
        if (!ran) return;
    }
    CHECK(!"the engine never went idle");
}

/* A record with each handle's events gathered under it, and each run of dead letters
 * sorted. An admission that cannot allocate its named-resource snapshot leaves that
 * item queued for the next pass ("fail closed", engine_pass step 4), so the items
 * admitted together - and the order events of DIFFERENT handles arrive in - can
 * change; what each handle goes through, and the end state, must not. */
typedef struct { const char *p; size_t n; size_t at; } rec_line;
static int line_by_label(const void *a, const void *b)
{
    const rec_line *x = (const rec_line *)a, *y = (const rec_line *)b;
    const char *lx = x->p + 3, *ly = y->p + 3;     /* past "ev " */
    size_t kx, ky;
    int c;
    lx += strcspn(lx, " "); ly += strcspn(ly, " ");   /* past the kind: the label */
    kx = strcspn(lx + 1, " "); ky = strcspn(ly + 1, " ");
    c = memcmp(lx + 1, ly + 1, kx < ky ? kx : ky);
    if (c) return c;
    if (kx != ky) return kx < ky ? -1 : 1;
    return x->at < y->at ? -1 : x->at > y->at;
}
static int line_by_text(const void *a, const void *b)
{
    const rec_line *x = (const rec_line *)a, *y = (const rec_line *)b;
    int c = memcmp(x->p, y->p, x->n < y->n ? x->n : y->n);
    return c ? c : (x->n < y->n ? -1 : x->n > y->n);
}
static char *canon(const char *rec)
{
    size_t n = 0, i, j, nev = 0, nother = 0, len = strlen(rec), k = 0;
    const char *p;
    rec_line *ev, *other;
    char *out;
    for (p = rec; *p; ++p) if (*p == '\n') ++n;
    ev = (rec_line *)malloc((n + 1) * sizeof *ev);
    other = (rec_line *)malloc((n + 1) * sizeof *other);
    out = (char *)malloc(len + 2);
    if (!ev || !other || !out) { printf("test out of memory\n"); exit(2); }
    for (p = rec, i = 0; *p; ++i) {
        rec_line L;
        L.p = p; L.n = strcspn(p, "\n"); L.at = i;
        if (strncmp(p, "ev ", 3) == 0) ev[nev++] = L; else other[nother++] = L;
        p += L.n + (p[L.n] ? 1 : 0);
    }
    for (i = 0; i < nother; i = j) {                    /* each run of dead letters, sorted */
        for (j = i; j < nother && strncmp(other[j].p, "dead ", 5) == 0; ++j) { }
        if (j > i + 1) qsort(other + i, j - i, sizeof *other, line_by_text);
        if (j == i) ++j;
    }
    qsort(ev, nev, sizeof *ev, line_by_label);
    for (i = 0; i < nother; ++i) { memcpy(out + k, other[i].p, other[i].n); k += other[i].n; out[k++] = '\n'; }
    for (i = 0; i < nev; ++i)    { memcpy(out + k, ev[i].p, ev[i].n);       k += ev[i].n;    out[k++] = '\n'; }
    out[k] = 0;
    free(ev); free(other);
    return out;
}

/* ---- tasks -------------------------------------------------------------------- */
static void manual_cfg(gptps_config *c, uint32_t conc)
{
    memset(c, 0, sizeof *c);
    c->struct_size = sizeof *c;
    c->limits.struct_size = sizeof c->limits;
    c->limits.max_concurrent_tasks = conc;
    c->limits.max_memory_bytes = 1u << 20;
    c->mode = GPTPS_RUN_MANUAL;
}

static gptps_status reg_task(gptps *e, const char *name, gptps_run_fn run, uint32_t retries,
                             uint32_t backoff, gptps_on_failure of, uint64_t mem)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = run; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_cost.mem_bytes = mem;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = retries;
    d.default_policy.retry_backoff_seconds = backoff;
    d.default_policy.on_failure = of;
    return gptps_register_task(e, &d);
}

/* The payload back as the result - a copy, so the call allocates. One refused for
 * memory is made again: with memory to spare it must take. */
static gptps_status t_echo(gptps_ctx *ctx, void *ud)
{
    size_t n;
    const void *p = gptps_payload(ctx, &n);
    (void)ud;
    TRY(GPTPS_OK, gptps_result_set(ctx, p ? p : "-", p ? n : 1));
    return GPTPS_OK;
}
static gptps_status t_fail(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_E_TASK; }
static int g_flaky_left;
static gptps_status t_flaky(gptps_ctx *ctx, void *ud)
{
    (void)ud;
    if (g_flaky_left > 0) { --g_flaky_left; return GPTPS_E_TASK; }
    return t_echo(ctx, NULL);
}

/* Denies a payload that says ":deny" - its item is dead-lettered with GPTPS_E_DENIED. */
static gptps_admit_decision c_deny(const gptps_constraint_input *in, uint32_t *retry_after, void *ud)
{
    const char *p = (const char *)in->payload;
    size_t i;
    (void)retry_after; (void)ud;
    for (i = 0; p && i + 5 <= in->payload_len; ++i)
        if (memcmp(p + i, ":deny", 5) == 0) return GPTPS_DENY;
    return GPTPS_ADMIT;
}

static gptps_handle submit(gptps *e, const char *task, const char *payload, const char *label,
                           gptps_status want)
{
    gptps_handle h = 0;
    g_next_label = g_at = label;
    TRY(want, gptps_submit(e, task, payload, payload ? strlen(payload) : 0, &h));
    g_next_label = g_at = NULL;
    return h;
}

static void begin_run(void)
{
    g_fplen = 0;
    if (g_fp) g_fp[0] = 0;
    g_nlab = 0;
    g_next_label = NULL;
    g_observed = 0;
    g_flaky_left = 1;
    g_step_injected = 0;
}

/* A dead letter: what it was - its task's name among it. */
static void on_dead(const gptps_dead_letter *dl, void *ud)
{
    gptps *e = (gptps *)ud;
    char pay[64];
    render(pay, sizeof pay, dl->payload, dl->payload_len);
    fp("dead %s %s st=%s att=%u payload=%s", label_of(dl->handle), dl->task_name, st_name(dl->status),
       (unsigned)dl->attempts, pay);
    if (strcmp(pay, "doomed:d") == 0)       /* resubmitted, from inside the drain */
        submit(e, "echo", "echo:again", "again", GPTPS_OK);
}

/* The dead-letter drain. It copies each one's name before it hands any over (the
 * callback may unregister the type); a copy that cannot be made stops it there, the
 * rest left retained - so a drain short of the count must have been refused memory,
 * and the next one, with memory to spare, takes the rest. */
static size_t drain_dead(gptps *e)
{
    size_t want = gptps_dead_letter_count(e), n;
    long inj0 = injected();
    n = gptps_dead_letter_drain(e, on_dead, e);
    if (n < want && injected() != inj0) {
        ++g_refusals;
        CHECK(gptps_dead_letter_count(e) == want - n);
        n += gptps_dead_letter_drain(e, on_dead, e);
    }
    if (n != want) report(__LINE__, "the drain took %u of %u dead letters", (unsigned)n, (unsigned)want);
    return n;
}

/* ==================================================================== scenarios */

/* 1. The work path: open, register, define resources and costs, submit with payloads,
 * run to completion with results, retries, dead letters and drops, a constraint's
 * denial, cancels from the queue and from backoff, a removal that leaves a dead letter,
 * the dead-letter drain, a removal with work queued, an add-on, shutdown. */
static void sc_work(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    gptps_handle h_cx, h_slow, h_again, h_alpha, hx = 0;
    gptps_submit_options o;
    size_t n;

    begin_run();
    manual_cfg(&cfg, 2);
    TRY(GPTPS_OK, gptps_open_ex(&cfg, &e));
    if (!e) return;
    TRY(GPTPS_OK, gptps_set_event_cb(e, on_event, NULL));
    TRY(GPTPS_OK, gptps_register_observer(e, on_observe, NULL));
    TRY(GPTPS_OK, gptps_register_constraint(e, c_deny, NULL));
    TRY(GPTPS_OK, gptps_define_resource(e, "gpu", 2));
    TRY(GPTPS_OK, reg_task(e, "echo", t_echo, 0, 0, GPTPS_ON_FAILURE_DEAD_LETTER, 1000));
    TRY(GPTPS_OK, gptps_define_resource(e, "disk", 4));        /* grows echo's cost vector */
    TRY(GPTPS_OK, gptps_set_task_resource_cost(e, "echo", "gpu", 1));
    TRY(GPTPS_OK, gptps_set_task_resource_cost(e, "echo", "disk", 3));
    TRY(GPTPS_OK, reg_task(e, "flaky", t_flaky, 2, 0, GPTPS_ON_FAILURE_DEAD_LETTER, 0));
    TRY(GPTPS_OK, reg_task(e, "doomed", t_fail, 1, 0, GPTPS_ON_FAILURE_DEAD_LETTER, 0));
    TRY(GPTPS_OK, reg_task(e, "dropper", t_fail, 0, 0, GPTPS_ON_FAILURE_DROP, 0));
    TRY(GPTPS_OK, reg_task(e, "slow", t_fail, 3, 3600, GPTPS_ON_FAILURE_DEAD_LETTER, 0));
    TRY(GPTPS_OK, reg_task(e, "again", t_fail, 0, 3600, GPTPS_ON_FAILURE_REQUEUE, 0));
    TRY(GPTPS_OK, reg_task(e, "gone", t_fail, 0, 0, GPTPS_ON_FAILURE_DEAD_LETTER, 0));
    TRY(GPTPS_OK, gptps_set_task_priority(e, "flaky", 2));
    TRY(GPTPS_OK, gptps_clone_task(e, "echo", "echo2"));
    TRY(GPTPS_E_DUP, gptps_clone_task(e, "echo", "echo2"));
    TRY(GPTPS_E_DUP, reg_task(e, "echo", t_echo, 0, 0, GPTPS_ON_FAILURE_DEAD_LETTER, 0));
    {   /* a program task's argv is copied - at registration, and again by a clone */
        static const char *const ARGV[] = { "oom-never-run", "--flag", NULL };
        gptps_task_def d;
        memset(&d, 0, sizeof d);
        d.struct_size = sizeof d; d.name = "prog"; d.exec = GPTPS_EXEC_PROGRAM; d.argv = ARGV;
        d.default_cost.struct_size = sizeof d.default_cost;
        d.default_policy.struct_size = sizeof d.default_policy;
        TRY(GPTPS_OK, gptps_register_task(e, &d));
        TRY(GPTPS_OK, gptps_clone_task(e, "prog", "prog2"));
    }
#if defined(ADDON_DEMO_PATH)
    TRY(GPTPS_OK, gptps_load_addon(e, ADDON_DEMO_PATH));
#endif

    h_alpha = submit(e, "echo", "echo:alpha", "alpha", GPTPS_OK);
    submit(e, "echo2", "echo2:beta", "beta", GPTPS_OK);
    submit(e, "flaky", "flaky:f", "f", GPTPS_OK);
    submit(e, "doomed", "doomed:d", "d", GPTPS_OK);
    submit(e, "dropper", "dropper:x", "x", GPTPS_OK);
    submit(e, "echo", "echo:deny", "denied", GPTPS_OK);
    h_slow = submit(e, "slow", "slow:s", "s", GPTPS_OK);
    h_again = submit(e, "again", "again:r", "r", GPTPS_OK);
    h_cx = submit(e, "echo", "echo:cancelme", "cx", GPTPS_OK);
    submit(e, "gone", "gone:g", "g", GPTPS_OK);
    submit(e, "echo", NULL, "empty", GPTPS_OK);
    submit(e, "nosuch", "nosuch:z", "z", GPTPS_E_NOTFOUND);
#if defined(ADDON_DEMO_PATH)
    submit(e, "plugintask", "plugintask:p", "p", GPTPS_OK);
#endif
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.flags = GPTPS_SUBMIT_PRIORITY | GPTPS_SUBMIT_POLICY;
    o.priority = 9;
    o.policy.struct_size = sizeof o.policy;
    o.policy.max_retries = 1;
    o.policy.on_failure = GPTPS_ON_FAILURE_DROP;
    g_next_label = "ex";
    TRY(GPTPS_OK, gptps_submit_ex(e, "doomed", "doomed:ex", 9, &o, &hx));
    g_next_label = NULL;
    TRY(GPTPS_OK, gptps_cancel(e, h_cx));                   /* still queued */
    drain(e);
    TRY(GPTPS_OK, gptps_cancel(e, h_slow));                 /* waiting out its backoff */
    TRY(GPTPS_OK, gptps_cancel(e, h_again));                /* waiting to start over */
    TRY(GPTPS_E_NOTFOUND, gptps_cancel(e, h_alpha));        /* long finished */
    /* its dead letter outlives it, and still names it */
    TRY(GPTPS_OK, gptps_unregister_task(e, "gone", GPTPS_REMOVE_DRAIN));
    fp("dead letters %u", (unsigned)gptps_dead_letter_count(e));
    n = drain_dead(e);
    fp("drained %u", (unsigned)n);
    drain(e);

    submit(e, "echo2", "echo2:late", "late", GPTPS_OK);     /* queued when its type goes */
    TRY(GPTPS_OK, gptps_unregister_task(e, "echo2", GPTPS_REMOVE_CANCEL));
    TRY(GPTPS_OK, gptps_set_task_enabled(e, "dropper", 0));
    submit(e, "dropper", "dropper:off", "off", GPTPS_E_NOTFOUND);
    submit(e, "echo", "echo:left", "left", GPTPS_OK);       /* still queued at shutdown */
    dump(e);
    TRY(GPTPS_OK, gptps_shutdown(e));
}

/* 2. A config file - the strict loader (src/config_toml.c) and the engine's cfg_*
 * path, a key waiting for the task and the settings that claim it later, an add-on
 * loaded from the file - then gptps_config_check, gptps_settings_save in place and to
 * a new path, and gptps_settings_reload, of the file and of one that defines a
 * resource more, which allocates as the file is applied. The files are part of what
 * the scenario observes. */
#define CFG     "oom_cfg.toml"
#define CFG_NEW "oom_cfg_new.toml"
#define CFG_RES "oom_cfg_res.toml"   /* the same, and a resource more */
static void put_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) { CHECK(!"cannot write a file"); return; }
    fputs(text, f);
    fclose(f);
}
static void fp_file(const char *path)
{
    char line[512];
    FILE *f = fopen(path, "rb");
    if (!f) { fp("file %s: none", path); return; }
    fp("file %s:", path);
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        fp("| %s", line);
    }
    fclose(f);
}
static void write_cfg(const char *path, int more)
{
    char text[3072], addons[1024] = "";
#if defined(ADDON_NS_PATH)
    {   /* a path as a TOML string: a backslash (a Windows path's) is written \\ */
        const char *p = ADDON_NS_PATH;
        size_t k = (size_t)snprintf(addons, sizeof addons, "addons = [\"");
        for (; *p && k + 4 < sizeof addons; ++p) {
            if (*p == '\\') addons[k++] = '\\';
            addons[k++] = *p;
        }
        snprintf(addons + k, sizeof addons - k, "\"]\n");
    }
#endif
    snprintf(text, sizeof text,
             "# what the OOM sweep opens\n"
             "%s"
             "\n"
             "[limits]\n"
             "max_concurrent_tasks = 2\n"
             "max_memory_bytes = 1048576   # one MiB\n"
             "max_intake_depth = 40\n"
             "shutdown_grace_ms = 5000\n"
             "\n"
             "[scheduler]\n"
             "reserve_after_skips = 4\n"
             "\n"
             "[resources]\n"
             "gpu = 2\n"
             "%s"
             "\n"
             "[task_defaults]\n"
             "max_retries = 1\n"
             "\n"
             "[tasks.echo]\n"
             "priority = 3   # kept by a save\n"
             "quality = 7\n"
             "\n"
             "[tasks.echo.resources]\n"
             "gpu = 1\n"
             "\n"
             "[tasks.late]\n"
             "max_retries = 2\n"
             "\n"
             "[host]\n"
             "color = \"blue\"\n"
             "%s", addons, more ? "disk = 3\n" : "", more ? "size = 3\n" : "");
    put_file(path, text);
}

/* A host's setting whose write accessor allocates - through this test's allocator, so
 * the sweep fails it too - and says GPTPS_E_NOMEM when it cannot. */
static unsigned long long g_size;
static size_t size_rd(void *t, char *b, size_t c) { (void)t; return (size_t)snprintf(b, c, "%llu", g_size); }
static gptps_status size_wr(void *t, const char *v)
{
    void *p = f_malloc(32, NULL);
    (void)t;
    if (!p) return GPTPS_E_NOMEM;
    g_size = strtoull(v, NULL, 10);
    f_free(p, NULL);
    return GPTPS_OK;
}

static void sc_config(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    gptps_setting_def d;
    char why[256], v[GPTPS_SETTINGS_VALUE_MAX];

    begin_run();
    g_size = 0;
    write_cfg(CFG, 0);
    write_cfg(CFG_RES, 1);
    remove(CFG_NEW);
    manual_cfg(&cfg, 0);
    cfg.limits.max_memory_bytes = 0;            /* the file's */
    cfg.config_path = CFG;
    TRY(GPTPS_OK, gptps_open_ex(&cfg, &e));
    if (!e) return;
    TRY(GPTPS_OK, gptps_set_event_cb(e, on_event, NULL));
    TRY(GPTPS_OK, reg_task(e, "echo", t_echo, 0, 0, GPTPS_ON_FAILURE_DEAD_LETTER, 100));
    TRY(GPTPS_OK, reg_task(e, "late", t_echo, 0, 0, GPTPS_ON_FAILURE_DEAD_LETTER, 100));
    TRY(GPTPS_OK, gptps_define_task_setting(e, "quality", GPTPS_SETTING_INT, "1", "0..10", 0));
    TRY(GPTPS_OK, gptps_define_global(e, "host.color", GPTPS_SETTING_STRING, "red", NULL, 0));
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.key = "host.size"; d.type = GPTPS_SETTING_UINT; d.hot = 1;
    d.desc = "a host's own, whose write allocates"; d.read = size_rd; d.write = size_wr;
    TRY(GPTPS_OK, gptps_register_setting(e, &d));
    TRY(GPTPS_OK, gptps_config_check(e));
    TRY(GPTPS_OK, gptps_settings_set(e, "limits.max_intake_depth", "50"));
    TRY(GPTPS_OK, gptps_settings_set_ex(e, "tasks.echo.priority", "4", why, sizeof why));
    TRY(GPTPS_OK, gptps_settings_set(e, "host.color", "green"));
    TRY(GPTPS_OK, gptps_settings_set(e, "tasks.late.quality", "9"));
    TRY(GPTPS_E_CONFIG, gptps_settings_set_ex(e, "tasks.echo.quality", "11", why, sizeof why));
    fp("why: %s", why);
    TRY(GPTPS_OK, gptps_settings_save(e, NULL));            /* in place */
    TRY(GPTPS_OK, gptps_settings_save(e, CFG_NEW));         /* a copy of the loaded file, edited */
    TRY(GPTPS_OK, gptps_settings_reload(e, NULL));
    TRY(GPTPS_OK, gptps_settings_get(e, "tasks.echo.priority", v, sizeof v));
    fp("echo priority after the reload: %s", v);
    /* a reload that defines a resource, and sets the host's setting: memory can run out
     * applying the file, in the engine and in the host's write accessor */
    TRY(GPTPS_OK, gptps_settings_reload(e, CFG_RES));
    submit(e, "echo", "echo:1", "one", GPTPS_OK);
    submit(e, "late", "late:2", "two", GPTPS_OK);
    drain(e);
    dump(e);
    TRY(GPTPS_OK, gptps_shutdown(e));
    fp_file(CFG);
    fp_file(CFG_NEW);
}

/* 3. The settings API: globals of each kind, per-task settings on tasks registered
 * before and after, a host's own setting, a watcher, set and set_ex, reads from a
 * task body, and a removal. */
static char g_knob[32] = "off", g_t3color[32] = "green";
static size_t knob_rd(void *t, char *b, size_t c) { (void)t; return (size_t)snprintf(b, c, "%s", g_knob); }
static gptps_status knob_wr(void *t, const char *v) { (void)t; snprintf(g_knob, sizeof g_knob, "%s", v); return GPTPS_OK; }
static size_t t3c_rd(void *t, char *b, size_t c) { (void)t; return (size_t)snprintf(b, c, "%s", g_t3color); }
static gptps_status t3c_wr(void *t, const char *v) { (void)t; snprintf(g_t3color, sizeof g_t3color, "%s", v); return GPTPS_OK; }
static void on_watch(const char *key, const char *value, void *ud) { (void)ud; fp("watch %s = %s", key, value); }
static gptps_status t_readset(gptps_ctx *ctx, void *ud)
{
    long w = -1;
    char c[GPTPS_SETTINGS_VALUE_MAX];
    (void)ud;
    TRY(GPTPS_OK, gptps_task_setting_str(ctx, "color", c, sizeof c));
    TRY(GPTPS_E_INVAL, gptps_task_setting_int(ctx, "weight", &w));    /* a double */
    TRY(GPTPS_OK, gptps_task_setting_str(ctx, "weight", c + 32, sizeof c - 32));
    fp("body color=%.31s weight=%s", c, c + 32);
    return GPTPS_OK;
}
static void sc_settings(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    gptps_setting_def d;
    char why[256], v[GPTPS_SETTINGS_VALUE_MAX];

    begin_run();
    snprintf(g_knob, sizeof g_knob, "off");
    snprintf(g_t3color, sizeof g_t3color, "green");
    manual_cfg(&cfg, 1);
    TRY(GPTPS_OK, gptps_open_ex(&cfg, &e));
    if (!e) return;
    TRY(GPTPS_OK, gptps_set_event_cb(e, on_event, NULL));
    TRY(GPTPS_OK, reg_task(e, "t1", t_readset, 0, 0, GPTPS_ON_FAILURE_DEAD_LETTER, 0));
    TRY(GPTPS_OK, gptps_define_global(e, "app.level", GPTPS_SETTING_INT, "3", "0..9", 0));
    TRY(GPTPS_OK, gptps_define_global(e, "app.mode", GPTPS_SETTING_ENUM, "fast", "fast|slow|auto", 0));
    TRY(GPTPS_OK, gptps_define_global(e, "app.name", GPTPS_SETTING_STRING, "x", NULL, GPTPS_SETTING_RESTART));
    TRY(GPTPS_OK, gptps_define_global(e, "app.ratio", GPTPS_SETTING_DOUBLE, "0.5", "0..1", 0));
    TRY(GPTPS_OK, gptps_define_global(e, "app.on", GPTPS_SETTING_BOOL, "true", NULL, 0));
    TRY(GPTPS_E_DUP, gptps_define_global(e, "app.level", GPTPS_SETTING_INT, "1", NULL, 0));
    TRY(GPTPS_E_CONFIG, gptps_define_global(e, "app.bad", GPTPS_SETTING_ENUM, "zzz", "a|b", 0));
    TRY(GPTPS_E_CONFIG, gptps_define_global(e, "app.bad", GPTPS_SETTING_ENUM, "a", NULL, 0));   /* no choices: */
    TRY(GPTPS_E_CONFIG, gptps_define_global(e, "app.bad", GPTPS_SETTING_ENUM, "a", "", 0));     /* not memory */
    TRY(GPTPS_E_CONFIG, gptps_define_task_setting(e, "badenum", GPTPS_SETTING_ENUM, "a", NULL, 0));
    TRY(GPTPS_E_CONFIG, gptps_define_task_setting(e, "badenum", GPTPS_SETTING_ENUM, "a", "", 0));
    TRY(GPTPS_OK, gptps_define_task_setting(e, "weight", GPTPS_SETTING_DOUBLE, "1.5", "0..10", 0));
    TRY(GPTPS_OK, gptps_define_task_setting(e, "color", GPTPS_SETTING_ENUM, "red", "red|blue", 0));
    TRY(GPTPS_E_DUP, gptps_define_task_setting(e, "weight", GPTPS_SETTING_INT, "1", NULL, 0));
    TRY(GPTPS_OK, reg_task(e, "t2", t_readset, 0, 0, GPTPS_ON_FAILURE_DEAD_LETTER, 0));
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.key = "host.knob"; d.type = GPTPS_SETTING_STRING;
    d.desc = "a host's own"; d.hot = 1; d.read = knob_rd; d.write = knob_wr;
    TRY(GPTPS_OK, gptps_register_setting(e, &d));
    TRY(GPTPS_E_DUP, gptps_register_setting(e, &d));
    /* a host's own key under a task's name, before the task: it stays the host's */
    d.key = "tasks.t3.color"; d.desc = "a host's own, where a task's would go";
    d.read = t3c_rd; d.write = t3c_wr;
    TRY(GPTPS_OK, gptps_register_setting(e, &d));
    TRY(GPTPS_OK, reg_task(e, "t3", t_readset, 0, 0, GPTPS_ON_FAILURE_DEAD_LETTER, 0));
    TRY(GPTPS_OK, gptps_settings_get(e, "tasks.t3.color", v, sizeof v));
    fp("t3 color %s", v);
    TRY(GPTPS_OK, gptps_settings_watch(e, on_watch, NULL));
    TRY(GPTPS_OK, gptps_settings_set(e, "app.level", "5"));
    TRY(GPTPS_E_CONFIG, gptps_settings_set_ex(e, "app.level", "10", why, sizeof why));
    fp("why: %s", why);
    TRY(GPTPS_E_NOTFOUND, gptps_settings_set_ex(e, "app.levl", "1", why, sizeof why));
    fp("why: %s", why);
    TRY(GPTPS_OK, gptps_settings_set(e, "app.mode", "slow"));
    TRY(GPTPS_OK, gptps_settings_set(e, "tasks.t2.weight", "2.5"));
    TRY(GPTPS_OK, gptps_settings_set(e, "tasks.t1.color", "blue"));
    TRY(GPTPS_OK, gptps_settings_set(e, "host.knob", "on"));
    TRY(GPTPS_OK, gptps_settings_set(e, "tasks.t2.max_retries", "4"));
    TRY(GPTPS_OK, gptps_settings_get(e, "app.mode", v, sizeof v));
    fp("app.mode %s", v);
    submit(e, "t1", NULL, "r1", GPTPS_OK);
    submit(e, "t2", NULL, "r2", GPTPS_OK);
    drain(e);
    TRY(GPTPS_OK, gptps_unregister_task(e, "t1", GPTPS_REMOVE_REJECT_IF_BUSY));
    TRY(GPTPS_E_NOTFOUND, gptps_settings_get(e, "tasks.t1.color", v, sizeof v));
    dump(e);
    TRY(GPTPS_OK, gptps_shutdown(e));
}

/* 4. A bounded engine: the first submit allocates its whole working set (the seal),
 * all or nothing. A seal that cannot allocate leaves the engine as it was - open
 * for setup - and the submit, made again, seals it. After the seal nothing on the
 * work path allocates, so the sweep ends there. */
static void on_probe(const gptps_event *ev, void *ud) { (void)ev; (void)ud; }
static void sc_bounded(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    gptps_handle h = 0;
    gptps_status st;
    long inj0;
    int i;

    begin_run();
    manual_cfg(&cfg, 2);
    cfg.max_items = 6;
    cfg.max_payload_bytes = 16;
    cfg.max_result_bytes = 16;
    TRY(GPTPS_OK, gptps_open_ex(&cfg, &e));
    if (!e) return;
    TRY(GPTPS_OK, gptps_set_event_cb(e, on_event, NULL));
    TRY(GPTPS_OK, gptps_register_observer(e, on_observe, NULL));
    TRY(GPTPS_OK, gptps_register_constraint(e, c_deny, NULL));
    TRY(GPTPS_OK, reg_task(e, "echo", t_echo, 0, 0, GPTPS_ON_FAILURE_DEAD_LETTER, 100));
    TRY(GPTPS_OK, reg_task(e, "doomed", t_fail, 1, 0, GPTPS_ON_FAILURE_DEAD_LETTER, 0));
    TRY(GPTPS_OK, gptps_define_resource(e, "gpu", 2));
    TRY(GPTPS_OK, gptps_set_task_resource_cost(e, "echo", "gpu", 1));
    submit(e, "nosuch", "nosuch:x", "x", GPTPS_E_NOTFOUND);     /* seals nothing */

    inj0 = injected();
    g_next_label = "first";
    st = gptps_submit(e, "echo", "echo:first", 10, &h);
    if (st == GPTPS_E_NOMEM && injected() != inj0) {
        /* The seal failed: nothing is sealed, so setup is still allowed. */
        CHECK(gptps_register_observer(e, on_probe, NULL) == GPTPS_OK);
        CHECK(gptps_unregister_observer(e, on_probe, NULL) == GPTPS_OK);
        st = gptps_submit(e, "echo", "echo:first", 10, &h);
    }
    g_next_label = NULL;
    CHECK(st == GPTPS_OK);
    fp("first submit %s", st_name(st));
    TRY(GPTPS_E_BUSY, reg_task(e, "late", t_echo, 0, 0, GPTPS_ON_FAILURE_DEAD_LETTER, 0));
    TRY(GPTPS_E_BUSY, gptps_define_global(e, "app.x", GPTPS_SETTING_INT, "1", NULL, 0));
    submit(e, "echo", "echo:2", "e2", GPTPS_OK);
    submit(e, "doomed", "doomed:3", "d3", GPTPS_OK);
    submit(e, "echo", "echo:deny", "denied", GPTPS_OK);
    submit(e, "echo", "echo:this-is-too-long", "long", GPTPS_E_INVAL);
    drain(e);
    fp("drained %u", (unsigned)drain_dead(e));
    for (i = 0; i < 6; ++i) submit(e, "echo", "echo:fill", "fill", GPTPS_OK);
    submit(e, "echo", "echo:full", "full", GPTPS_E_FULL);
    drain(e);
    dump(e);
    TRY(GPTPS_OK, gptps_shutdown(e));
}

#if defined(OOM_WITH_DQ)
/* 5. The durable queue add-on. Its own memory is libc's (addons/gptps_durable_queue.c
 * calls malloc directly), so the failures here are the engine's, under its calls:
 * gptps_dq_open registers an observer, gptps_dq_submit and gptps_dq_recover submit.
 * A submit the engine refuses is closed in the journal and refused to the caller;
 * one the recovery could not hand over stays pending for the next call. The engines
 * are set up uncounted - the work scenario sweeps that - which keeps the runs, and
 * the journal's fsyncs in each, few. */
#define JOURNAL "oom_dq.journal"
static int g_work_runs[4];
static gptps_status t_work(gptps_ctx *ctx, void *ud)
{
    size_t n;
    const char *p = (const char *)gptps_payload(ctx, &n);
    (void)ud;
    if (p && n == 1 && *p >= '1' && *p <= '3') ++g_work_runs[*p - '0'];
    return GPTPS_OK;
}
static gptps *dq_engine(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    manual_cfg(&cfg, 1);
    pause_count(1);
    TRY(GPTPS_OK, gptps_open_ex(&cfg, &e));
    if (e) TRY(GPTPS_OK, reg_task(e, "work", t_work, 0, 0, GPTPS_ON_FAILURE_DEAD_LETTER, 0));
    pause_count(0);
    return e;
}
static gptps_dq *dq_open(gptps *e)
{
    long inj0 = injected();
    gptps_dq *dq = gptps_dq_open(e, JOURNAL);
    if (!dq && injected() != inj0) { ++g_refusals; dq = gptps_dq_open(e, JOURNAL); }
    CHECK(dq != NULL);
    return dq;
}
static void sc_durable(void)
{
    gptps *e;
    gptps_dq *dq;
    gptps_handle h;
    size_t ran = 0, got;
    long inj0;
    int i;

    begin_run();
    memset(g_work_runs, 0, sizeof g_work_runs);
    remove(JOURNAL);
    /* the first run: three durable submits, one of them run */
    if (!(e = dq_engine())) return;
    if (!(dq = dq_open(e))) { gptps_shutdown(e); return; }
    for (i = 1; i <= 3; ++i) {
        char p = (char)('0' + i);
        TRY(GPTPS_OK, gptps_dq_submit(dq, "work", &p, 1, &h));
    }
    TRY(GPTPS_OK, gptps_step(e, &ran));
    fp("ran %u, pending %u", (unsigned)ran, (unsigned)gptps_dq_pending(dq));
    TRY(GPTPS_OK, gptps_shutdown(e));
    gptps_dq_close(dq);

    /* the next: the two left are recovered, and run */
    if (!(e = dq_engine())) return;
    if (!(dq = dq_open(e))) { gptps_shutdown(e); return; }
    fp("pending at open %u", (unsigned)gptps_dq_pending(dq));
    inj0 = injected();
    got = gptps_dq_recover(dq);
    if (got < 2 && injected() != inj0) { ++g_refusals; got += gptps_dq_recover(dq); }
    fp("recovered %u", (unsigned)got);
    drain(e);
    fp("pending %u, quarantined %u", (unsigned)gptps_dq_pending(dq), (unsigned)gptps_dq_quarantined(dq));
    TRY(GPTPS_OK, gptps_shutdown(e));
    gptps_dq_close(dq);

    /* and the one after: nothing left */
    if (!(e = dq_engine())) return;
    if (!(dq = dq_open(e))) { gptps_shutdown(e); return; }
    fp("pending at the last open %u", (unsigned)gptps_dq_pending(dq));
    TRY(GPTPS_OK, gptps_shutdown(e));
    gptps_dq_close(dq);
    fp("runs %d %d %d", g_work_runs[1], g_work_runs[2], g_work_runs[3]);
    remove(JOURNAL);
}
#endif

/* ---- the sweep ------------------------------------------------------------------ */
static void sweep(const char *name, void (*scenario)(void))
{
    char *ref;
    long total, n, mismatches = 0, reordered = 0;
    g_scn = name;
    g_n = 0;
    g_refusals = g_absorbed = 0;
    beat();
    arm(0, 0, 0);                       /* counted, never failed: the reference */
    scenario();
    disarm();
    total = counted();
    CHECK(live() == 0);
    ref = (char *)malloc(g_fplen + 1);
    if (!ref) { printf("test out of memory\n"); exit(2); }
    memcpy(ref, g_fp ? g_fp : "", g_fplen + 1);
    if (verbose) printf("%s: reference run, %ld allocations:\n%s", name, total, ref);

    for (n = 1; ; ++n) {
        long leaked;
        g_n = n;
        beat();
        arm(n, 0, 0);
        scenario();
        disarm();
        if ((leaked = live()) != 0) {
            report(__LINE__, "%ld block(s) still allocated after shutdown", leaked);
            gptps_mutex_lock(a_m); a_live = 0; gptps_mutex_unlock(a_m);
        }
        if (!injected()) break;         /* fewer than n allocations: the sweep is done */
        if (n == g_dump_n) printf("%s: reference run:\n%s\n%s: run N=%ld:\n%s", name, ref, name, n, g_fp ? g_fp : "");
        if (strcmp(g_fp ? g_fp : "", ref) != 0) {
            char *want = canon(ref), *got = canon(g_fp ? g_fp : "");
            if (g_step_injected && strcmp(want, got) == 0) {
                ++reordered;                /* a delayed admission: same per handle, same end */
            } else {
                const char *a = want, *b = got;
                int line = 1;
                while (*a && *a == *b) { if (*a == '\n') ++line; ++a; ++b; }
                while (a > want && a[-1] != '\n') { --a; --b; }
                ++mismatches;
                report(__LINE__, "what the scenario observed differs from the run with no failure "
                       "(each handle's events gathered), at line %d:\n    want: %.*s\n    got:  %.*s", line,
                       (int)strcspn(a, "\n"), a, (int)strcspn(b, "\n"), b);
            }
            free(want); free(got);
        }
    }
    printf("%s: %ld allocations, each failed in turn; %ld refused with GPTPS_E_NOMEM, "
           "%ld absorbed, %ld runs differed, %ld reordered\n", name, total, g_refusals, g_absorbed,
           mismatches, reordered);
    fflush(stdout);
    free(ref);
    g_n = 0;
}

/* ==================================================== 6. a THREADED engine, at random */
/* Every handle a submit returned must reach exactly one terminal event. */
#define T_MAX_H 1024
static gptps_mutex *t_m;
static gptps_cond  *t_c;
static unsigned char t_term[T_MAX_H];
static long t_terms, t_badterm, t_accepted;
static long t_all_handles, t_all_injected;   /* over every seed, for the summary */
static int is_terminal(const gptps_event *ev)
{
    return ev->kind == GPTPS_EV_FINISHED || ev->kind == GPTPS_EV_DEAD_LETTERED ||
           ev->kind == GPTPS_EV_DROPPED ||
           (ev->kind == GPTPS_EV_FAILED && ev->status == GPTPS_E_CANCELLED);
}
static void t_on_event(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (!is_terminal(ev)) return;
    gptps_mutex_lock(t_m);
    if (ev->handle < T_MAX_H) {
        if (t_term[ev->handle]++) ++t_badterm;
        ++t_terms;
        gptps_cond_broadcast(t_c);
    } else {
        ++t_badterm;
    }
    gptps_mutex_unlock(t_m);
}
/* A result that cannot be copied is set again, a few times, before the attempt
 * gives up and fails: it is retried by its policy like any failure. */
static gptps_status tt_echo(gptps_ctx *ctx, void *ud)
{
    size_t n;
    int k;
    const void *p = gptps_payload(ctx, &n);
    (void)ud;
    for (k = 0; k < 8; ++k)
        if (gptps_result_set(ctx, p ? p : "-", p ? n : 1) == GPTPS_OK) return GPTPS_OK;
    return GPTPS_E_NOMEM;
}
static gptps_status tt_flaky(gptps_ctx *ctx, void *ud)
{
    size_t n;
    const unsigned char *p = (const unsigned char *)gptps_payload(ctx, &n);
    (void)ud;
    return (p && n && (p[0] & 1)) ? GPTPS_E_TASK : tt_echo(ctx, NULL);
}

/* A call made until it is not refused for memory. */
#define RETRY(st, call) do { int k_; for (k_ = 0; k_ < 64; ++k_) { (st) = (call); if ((st) != GPTPS_E_NOMEM) break; } } while (0)

typedef struct { gptps *e; int from, count; gptps_handle hs[64]; int nh; } t_producer;
static void produce(t_producer *p)
{
    static const char *const TASKS[] = { "echo", "flaky", "doomed", "dropper" };
    int i;
    for (i = p->from; i < p->from + p->count; ++i) {
        unsigned char pay[8];
        gptps_handle h = 0;
        gptps_status st;
        pay[0] = (unsigned char)i; memcpy(pay + 1, "payload", 7);
        RETRY(st, gptps_submit(p->e, TASKS[i % 4], pay, sizeof pay, &h));
        if (st == GPTPS_OK) {
            gptps_mutex_lock(t_m); ++t_accepted; gptps_mutex_unlock(t_m);
            if (p->nh < 64) p->hs[p->nh++] = h;
        }
    }
}
static void *producer_main(void *arg) { produce((t_producer *)arg); return NULL; }

static void sc_threaded(uint64_t seed)
{
    gptps *e = NULL;
    gptps_config cfg;
    gptps_status st;
    gptps_thread *th;
    t_producer a, b;
    uint64_t until;
    long leaked;
    int i;

    memset(t_term, 0, sizeof t_term);
    t_terms = t_badterm = t_accepted = 0;
    arm(0, 1, seed);
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 3;
    cfg.limits.max_memory_bytes = 1u << 20;
    RETRY(st, gptps_open_ex(&cfg, &e));
    CHECK(st == GPTPS_OK && e != NULL);
    if (!e) { disarm(); return; }
    RETRY(st, gptps_set_event_cb(e, t_on_event, NULL));
    RETRY(st, gptps_register_observer(e, on_probe, NULL));
    RETRY(st, gptps_register_constraint(e, c_deny, NULL));
    RETRY(st, gptps_define_resource(e, "gpu", 2));
    RETRY(st, reg_task(e, "echo", tt_echo, 2, 0, GPTPS_ON_FAILURE_DEAD_LETTER, 1000));
    RETRY(st, gptps_set_task_resource_cost(e, "echo", "gpu", 1));
    RETRY(st, reg_task(e, "flaky", tt_flaky, 1, 0, GPTPS_ON_FAILURE_DEAD_LETTER, 0));
    RETRY(st, reg_task(e, "doomed", t_fail, 1, 0, GPTPS_ON_FAILURE_DEAD_LETTER, 0));
    RETRY(st, reg_task(e, "dropper", t_fail, 0, 0, GPTPS_ON_FAILURE_DROP, 0));

    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.e = b.e = e; a.from = 0; a.count = 60; b.from = 60; b.count = 60;
    th = gptps_thread_start(producer_main, &b);     /* a second host thread submits too */
    produce(&a);
    if (th) gptps_thread_join(th); else produce(&b);
    for (i = 0; i < a.nh; i += 7) (void)gptps_cancel(e, a.hs[i]);
    RETRY(st, gptps_settings_set(e, "limits.max_intake_depth", "500"));
    RETRY(st, gptps_define_resource(e, "gpu", 3));  /* a re-budget, live */

    /* every accepted handle reaches its terminal event */
    until = gptps_hal_monotonic_ms() + 20000;
    gptps_mutex_lock(t_m);
    while (t_terms < t_accepted && gptps_hal_monotonic_ms() < until) {
        gptps_cond_timedwait(t_c, t_m, 100);
        gptps_mutex_unlock(t_m);
        beat();
        gptps_mutex_lock(t_m);
    }
    if (t_terms < t_accepted)               /* stalled: shutdown would end them, and hide it */
        report(__LINE__, "seed %llu: %ld of %ld handles still open after 20 s",
               (unsigned long long)seed, t_accepted - t_terms, t_accepted);
    gptps_mutex_unlock(t_m);
    (void)gptps_dead_letter_drain(e, NULL, NULL);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    disarm();
    t_all_injected += injected();
    gptps_mutex_lock(t_m);
    t_all_handles += t_accepted;
    if (t_terms != t_accepted || t_badterm)
        report(__LINE__, "seed %llu: %ld handles accepted, %ld terminal events, %ld duplicate",
               (unsigned long long)seed, t_accepted, t_terms, t_badterm);
    gptps_mutex_unlock(t_m);
    if ((leaked = live()) != 0) {
        report(__LINE__, "seed %llu: %ld block(s) still allocated after shutdown", (unsigned long long)seed, leaked);
        gptps_mutex_lock(a_m); a_live = 0; gptps_mutex_unlock(a_m);
    }
}

/* ================================ 7. a definition, and a second thread meanwhile */
/* gptps_define_task_setting and gptps_define_resource make a setting for each task
 * there is, and gptps_register_task makes each setting a new task has; each makes all
 * of them before it publishes any - or, out of memory, publishes nothing and returns
 * GPTPS_E_NOMEM. A second thread meanwhile must find all of it or none: a task it
 * registers has the setting if the definition succeeds, no task keeps one if it fails,
 * a task registered meanwhile is there whole or not at all, and a definition of its
 * own is not lost to the other's. Thread A stops at its k-th allocation, for every k,
 * and that allocation fails or not; thread B registers a task or makes a definition,
 * with up to 20 ms to go as far as it can; then A is let go. Where A is the
 * registration, B's j-th allocation is also failed instead of A's, for every j. How
 * far B got is timing; what is checked holds whatever it was. */
enum { W_RES_REG, W_LEAF_REG, W_RES_RES, W_RES_SAME, W_LEAF_LEAF, W_REG_RES, W_REG_LEAF, W_KINDS };
static const char *const W_NAME[W_KINDS] = {
    "a resource, a task registered meanwhile", "a task setting, a task registered meanwhile",
    "a resource, another defined meanwhile", "a resource, the same one defined meanwhile",
    "a task setting, the same one defined meanwhile",
    "a task registered, a resource defined meanwhile", "a task registered, a task setting defined meanwhile" };
typedef struct { gptps *e; int what; gptps_status st; } w_call;
static int w_b_done;                    /* with a_m and p_c */

static int w_seen[3];                   /* t1, t2, late: a run of the task read leafx */
static gptps_status t_leaf(gptps_ctx *ctx, void *ud)
{
    char v[GPTPS_SETTINGS_VALUE_MAX];
    *(int *)ud = gptps_task_setting_str(ctx, "leafx", v, sizeof v) == GPTPS_OK;
    return GPTPS_OK;
}
static gptps_status w_reg(gptps *e, const char *name, int *seen)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = t_leaf; d.user_data = seen;
    d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    return gptps_register_task(e, &d);
}
static gptps_status w_leaf(gptps *e)
{ return gptps_define_task_setting(e, "leafx", GPTPS_SETTING_INT, "1", "0..9", 0); }
static gptps_status w_late(gptps *e) { return w_reg(e, "late", &w_seen[2]); }
static gptps_status w_gpu2(gptps *e) { return gptps_define_resource(e, "gpu2", 4); }

static gptps_status w_a_call(gptps *e, int what)    /* thread A's call */
{
    switch (what) {
    case W_LEAF_REG: case W_LEAF_LEAF:  return w_leaf(e);
    case W_REG_RES:  case W_REG_LEAF:   return w_late(e);
    default:                            return w_gpu2(e);
    }
}
static void *w_a(void *arg)             /* thread A: the call that stops */
{
    w_call *c = (w_call *)arg;
    gptps_mutex_lock(a_m);
    p_tid = gptps_hal_thread_id(); p_count = 0;                 /* the mark */
    gptps_mutex_unlock(a_m);
    c->st = w_a_call(c->e, c->what);
    gptps_mutex_lock(a_m);
    p_tid = 0;
    if (p_state == P_ARMED) p_state = P_DONE;                   /* k was past its allocations */
    gptps_cond_broadcast(p_c);
    gptps_mutex_unlock(a_m);
    return NULL;
}
static void *w_b(void *arg)             /* thread B: what happens meanwhile */
{
    w_call *c = (w_call *)arg;
    gptps_mutex_lock(a_m);
    q_tid = gptps_hal_thread_id(); q_count = 0;                 /* its mark */
    gptps_mutex_unlock(a_m);
    switch (c->what) {
    case W_RES_RES:   c->st = gptps_define_resource(c->e, "gpu3", 2); break;
    case W_RES_SAME:  c->st = w_gpu2(c->e); break;
    case W_LEAF_LEAF: case W_REG_LEAF: c->st = w_leaf(c->e); break;
    case W_REG_RES:   c->st = w_gpu2(c->e); break;
    default:          c->st = w_late(c->e); break;
    }
    gptps_mutex_lock(a_m);
    q_tid = 0;
    w_b_done = 1;
    gptps_cond_broadcast(p_c);
    gptps_mutex_unlock(a_m);
    return NULL;
}

static int has_setting(gptps *e, const char *key)
{
    char v[GPTPS_SETTINGS_VALUE_MAX];
    return gptps_settings_get(e, key, v, sizeof v) == GPTPS_OK;
}
/* Settings whose key starts with `head` and ends with `tail`. */
static int keys_like(gptps *e, const char *head, const char *tail)
{
    size_t i, n = gptps_settings_count(e), hl = strlen(head), tl = strlen(tail);
    int c = 0;
    for (i = 0; i < n; ++i) {
        gptps_setting_info si;
        size_t kl;
        memset(&si, 0, sizeof si);
        si.struct_size = sizeof si;
        if (gptps_settings_get_info(e, i, &si) != GPTPS_OK) continue;
        kl = strlen(si.key);
        if (kl >= hl + tl && strncmp(si.key, head, hl) == 0 && strcmp(si.key + kl - tl, tail) == 0) ++c;
    }
    return c;
}

/* The resource `name` with its settings - the budget's and a cost for each task - or
 * no trace of it. */
static void w_check_res(gptps *e, const char *ctx, const char *name, int want,
                        const char *const *tasks, int nt)
{
    char key[96], tail[64];
    uint64_t b = 0;
    int i, n, found = gptps_resource_usage(e, name, NULL, &b) == GPTPS_OK;
    if (found != want || (found && b != (strcmp(name, "gpu2") ? 2u : 4u)))
        report(__LINE__, "%s: the resource %s is %s", ctx, name, found ? "there" : "missing");
    snprintf(key, sizeof key, "resources.%s", name);
    if (has_setting(e, key) != want) report(__LINE__, "%s: %s %s", ctx, key, want ? "missing" : "left behind");
    for (i = 0; i < nt; ++i) {
        snprintf(key, sizeof key, "tasks.%s.resources.%s", tasks[i], name);
        if (has_setting(e, key) != want) report(__LINE__, "%s: %s %s", ctx, key, want ? "missing" : "left behind");
    }
    snprintf(tail, sizeof tail, ".resources.%s", name);
    if ((n = keys_like(e, "tasks.", tail)) != (want ? nt : 0))
        report(__LINE__, "%s: %d tasks.*%s keys for %d tasks", ctx, n, tail, want ? nt : 0);
}

/* The task setting leafx on each task, read by a run of it - or nowhere. */
static void w_check_leaf(gptps *e, const char *ctx, int want, const char *const *tasks, int nt)
{
    char key[96];
    size_t ran;
    int i, n, k;
    for (i = 0; i < nt; ++i) {
        snprintf(key, sizeof key, "tasks.%s.leafx", tasks[i]);
        if (has_setting(e, key) != want) report(__LINE__, "%s: %s %s", ctx, key, want ? "missing" : "left behind");
    }
    if ((n = keys_like(e, "tasks.", ".leafx")) != (want ? nt : 0))
        report(__LINE__, "%s: %d tasks.*.leafx keys for %d tasks", ctx, n, want ? nt : 0);
    memset(w_seen, 0, sizeof w_seen);
    for (i = 0; i < nt; ++i) CHECK(gptps_submit(e, tasks[i], NULL, 0, NULL) == GPTPS_OK);
    for (k = 0; k < 10 && gptps_step(e, &ran) == GPTPS_OK && ran; ++k) { }
    for (i = 0; i < nt; ++i)
        if (w_seen[i] != want) report(__LINE__, "%s: a run of %s %s leafx", ctx, tasks[i], want ? "could not read" : "read");
}

/* The task "late" whole - every setting t1 has, under its own name - or no trace of it. */
static void w_check_late(gptps *e, const char *ctx, int want)
{
    int n = keys_like(e, "tasks.late.", ""), t1 = keys_like(e, "tasks.t1.", "");
    if (gptps_task_exists(e, "late") != want)
        report(__LINE__, "%s: the task late is %s", ctx, want ? "missing" : "there");
    if (n != (want ? t1 : 0))
        report(__LINE__, "%s: late has %d settings, t1 %d", ctx, n, t1);
}

/* Everything that can be there, against what must be. */
static void w_check(gptps *e, const char *ctx, int what, int late, int gpu2, int gpu3, int leaf)
{
    static const char *const TASKS[] = { "t1", "t2", "late" };
    int nt = late ? 3 : 2;
    w_check_late(e, ctx, late);
    w_check_res(e, ctx, "gpu", 1, TASKS, nt);
    w_check_res(e, ctx, "gpu2", gpu2, TASKS, nt);
    if (what == W_RES_RES) w_check_res(e, ctx, "gpu3", gpu3, TASKS, nt);
    if (what == W_LEAF_REG || what == W_LEAF_LEAF || what == W_REG_LEAF) w_check_leaf(e, ctx, leaf, TASKS, nt);
}

/* One run: A stops at its k-th allocation (failing it if `fail`), B runs meanwhile, and
 * B's j-th allocation fails (none for j = 0). Returns whether A stopped, i.e. whether k
 * was within A's call; *b_failed, whether B's j-th allocation came. */
static int w_run(int what, long k, int fail, long j, int *b_failed)
{
    gptps *e = NULL;
    gptps_config cfg;
    gptps_thread *ta, *tb = NULL;
    w_call A, B;
    char ctx[192];
    long leaked;
    int paused, a_ok, b_ok, late, gpu2, gpu3, leaf;
    uint64_t until;

    g_n = k;
    *b_failed = 0;
    snprintf(ctx, sizeof ctx, "%s, k=%ld%s%s", W_NAME[what], k, fail ? ", failing" : "", "");
    if (j) snprintf(ctx + strlen(ctx), sizeof ctx - strlen(ctx), ", the other failing at %ld", j);
    manual_cfg(&cfg, 2);
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (!e) return 0;
    CHECK(gptps_define_resource(e, "gpu", 2) == GPTPS_OK);     /* a new one grows the cost vectors */
    /* and a task setting before them: a registration that finds another defined
     * meanwhile makes it, and stops at the ones it has made already */
    CHECK(gptps_define_task_setting(e, "pre", GPTPS_SETTING_UINT, "5", NULL, 0) == GPTPS_OK);
    CHECK(w_reg(e, "t1", &w_seen[0]) == GPTPS_OK);
    CHECK(w_reg(e, "t2", &w_seen[1]) == GPTPS_OK);
    memset(&A, 0, sizeof A);
    A.e = e; A.what = what; B = A;
    gptps_mutex_lock(a_m);
    p_at = k; p_fail = fail; p_state = P_ARMED;
    q_at = j; q_hit = 0; w_b_done = 0;
    gptps_mutex_unlock(a_m);
    ta = gptps_thread_start(w_a, &A);
    CHECK(ta != NULL);
    if (!ta) { gptps_shutdown(e); return 0; }
    gptps_mutex_lock(a_m);
    while (p_state == P_ARMED) gptps_cond_wait(p_c, a_m);
    paused = (p_state == P_PAUSED);
    gptps_mutex_unlock(a_m);
    if (paused) {
        tb = gptps_thread_start(w_b, &B);
        CHECK(tb != NULL);
        until = gptps_hal_monotonic_ms() + 20;      /* B goes as far as it can */
        gptps_mutex_lock(a_m);
        while (!w_b_done && gptps_hal_monotonic_ms() < until) gptps_cond_timedwait(p_c, a_m, 5);
        p_state = P_RELEASED;
        gptps_cond_broadcast(p_c);
        gptps_mutex_unlock(a_m);
    }
    gptps_thread_join(ta);
    if (tb) gptps_thread_join(tb);
    gptps_mutex_lock(a_m); p_state = 0; *b_failed = q_hit; gptps_mutex_unlock(a_m);

    /* Each call finished, or was refused the memory it was refused, or - two threads
     * defining one leaf - was beaten to it. */
    if (!(A.st == GPTPS_OK || (fail && A.st == GPTPS_E_NOMEM) || (what == W_LEAF_LEAF && tb && A.st == GPTPS_E_DUP)))
        report(__LINE__, "%s: A's call gave %s", ctx, st_name(A.st));
    if (tb && !(B.st == GPTPS_OK || (*b_failed && B.st == GPTPS_E_NOMEM) || (what == W_LEAF_LEAF && B.st == GPTPS_E_DUP)))
        report(__LINE__, "%s: the other thread's call gave %s", ctx, st_name(B.st));
    if (what == W_LEAF_LEAF && tb && (A.st == GPTPS_OK) + (B.st == GPTPS_OK) != 1)
        report(__LINE__, "%s: the definition gave %s, the other %s: one of them must define it",
               ctx, st_name(A.st), st_name(B.st));

    a_ok = A.st == GPTPS_OK;
    b_ok = tb && B.st == GPTPS_OK;
    late = (what == W_REG_RES || what == W_REG_LEAF) ? a_ok : (what == W_RES_REG || what == W_LEAF_REG) && b_ok;
    gpu2 = (what == W_REG_RES) ? b_ok : (what == W_RES_REG || what == W_RES_RES || what == W_RES_SAME) && (a_ok || (what == W_RES_SAME && b_ok));
    gpu3 = what == W_RES_RES && b_ok;
    leaf = (what == W_REG_LEAF) ? b_ok : (what == W_LEAF_REG || what == W_LEAF_LEAF) && (a_ok || (what == W_LEAF_LEAF && b_ok));
    w_check(e, ctx, what, late, gpu2, gpu3, leaf);

    /* Each call refused for memory, made again with memory to spare. */
    if (A.st == GPTPS_E_NOMEM) {
        gptps_status again = w_a_call(e, what);
        gptps_status want = (what == W_LEAF_LEAF && b_ok) ? GPTPS_E_DUP : GPTPS_OK;
        if (again != want)
            report(__LINE__, "%s: A's call made again gave %s, want %s", ctx, st_name(again), st_name(want));
    }
    if (tb && B.st == GPTPS_E_NOMEM) {
        gptps_status again = (what == W_REG_RES) ? w_gpu2(e) : (what == W_REG_LEAF) ? w_leaf(e) : GPTPS_E_INVAL;
        if (again != GPTPS_OK)
            report(__LINE__, "%s: the other thread's call made again gave %s", ctx, st_name(again));
    }
    if (A.st == GPTPS_E_NOMEM || (tb && B.st == GPTPS_E_NOMEM)) {
        late = late || what == W_REG_RES || what == W_REG_LEAF || (tb && (what == W_RES_REG || what == W_LEAF_REG));
        gpu2 = gpu2 || what == W_RES_REG || what == W_RES_RES || what == W_RES_SAME || (tb && what == W_REG_RES);
        leaf = leaf || what == W_LEAF_REG || what == W_LEAF_LEAF || (tb && what == W_REG_LEAF);
        w_check(e, ctx, what, late, gpu2, gpu3, leaf);
    }
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    if ((leaked = live()) != 0) {
        report(__LINE__, "%s: %ld block(s) still allocated after shutdown", ctx, leaked);
        gptps_mutex_lock(a_m); a_live = 0; gptps_mutex_unlock(a_m);
    }
    return paused;
}

/* Every k, failing A's allocation and not; where A is the registration, also every j
 * for B with A's not failing. */
static long w_runs;
static void w_sweep(int what)
{
    char bad[256];
    size_t nb = 0;
    long k, j, maxj = 0;
    int fail, b_failed, both = (what == W_REG_RES || what == W_REG_LEAF);
    bad[0] = 0;
    for (k = 1; ; ++k) {
        int paused = 1;
        for (fail = 0; fail <= 1 && paused; ++fail) {
            for (j = 0; ; ++j) {
                int before = fails;
                beat();
                ++w_runs;
                paused = w_run(what, k, fail, j, &b_failed);
                if (fails != before && nb + 32 < sizeof bad) {
                    nb += (size_t)snprintf(bad + nb, sizeof bad - nb, " %ld%s", k, fail ? "f" : "");
                    if (j) nb += (size_t)snprintf(bad + nb, sizeof bad - nb, "/j%ld", j);
                }
                if (j > maxj && b_failed) maxj = j;
                if (!paused || !both || fail || (j && !b_failed)) break;
            }
        }
        if (!paused) break;                     /* past A's allocations */
    }
    printf("race: %s: stopped at each of %ld allocations, failing it and not%s:%s%s\n",
           W_NAME[what], k - 1, both ? "; the other's failed in turn" : "", nb ? " FAILED at k =" : " held", bad);
    if (both && maxj == 0) report(__LINE__, "%s: the other thread's allocations were never failed", W_NAME[what]);
    fflush(stdout);
}

static void quiet(gptps_log_level lvl, const char *msg, void *ud) { (void)lvl; (void)msg; (void)ud; }

int main(void)
{
    gptps_allocator al;
    gptps_thread *dog;
    const char *vb = getenv("OOM_VERBOSE");
    uint64_t seed;
    int i;

    verbose = vb && *vb && strcmp(vb, "0") != 0;
    if (getenv("OOM_DUMP")) g_dump_n = atol(getenv("OOM_DUMP"));
    a_m = gptps_mutex_create(); w_m = gptps_mutex_create(); w_c = gptps_cond_create();
    t_m = gptps_mutex_create(); t_c = gptps_cond_create(); p_c = gptps_cond_create();
    if (!a_m || !w_m || !w_c || !t_m || !t_c || !p_c) { printf("no HAL primitives\n"); return 1; }
    memset(&al, 0, sizeof al);
    al.struct_size = sizeof al;
    al.malloc_fn = f_malloc; al.realloc_fn = f_realloc; al.free_fn = f_free;
    CHECK(gptps_set_allocator(&al) == GPTPS_OK);
    gptps_set_log_sink(quiet, NULL);       /* refusals log; the log is not what is checked */
    beat();
    dog = gptps_thread_start(watchdog, NULL);

    sweep("work", sc_work);
    sweep("config", sc_config);
    sweep("settings", sc_settings);
    sweep("bounded", sc_bounded);
#if defined(OOM_WITH_DQ)
    sweep("durable_queue", sc_durable);
#endif
    g_scn = "threaded";
    for (seed = 1; seed <= 8; ++seed) { beat(); sc_threaded(seed); }
    printf("threaded: 8 seeds, one allocation in %u failed at random: %ld failures over %ld handles\n",
           OOM_RANDOM_DEN, t_all_injected, t_all_handles);
    fflush(stdout);
    g_scn = "definition race";
    for (i = 0; i < W_KINDS; ++i) w_sweep(i);
    printf("race: %ld runs\n", w_runs);
    g_n = 0;

    gptps_mutex_lock(w_m); w_stop = 1; gptps_cond_signal(w_c); gptps_mutex_unlock(w_m);
    if (dog) gptps_thread_join(dog);
    gptps_set_allocator(NULL);
    gptps_set_log_sink(NULL, NULL);
    remove(CFG); remove(CFG_NEW); remove(CFG_RES);
    free(g_fp);
    for (i = 0; i < g_nsites; ++i)
        if (g_sites[i].count > 1)
            printf("  line %d [%s] failed %ld times, first at N=%ld\n", g_sites[i].line,
                   g_sites[i].scn, g_sites[i].count, g_sites[i].first_n);
    if (fails) { printf("%d OOM check(s) FAILED\n", fails); return 1; }
    printf("all OOM checks passed\n");
    return 0;
}
