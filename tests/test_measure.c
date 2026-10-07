/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_measure.c - what a process job actually used (docs/MEASUREMENTS.md).
 *
 * Jobs of known size run through the PROGRAM executor (prog_helper "mem", "spin",
 * "exit") and, on POSIX, the OOP executor, on a MANUAL engine so every event arrives
 * on this thread, in order. Each attempt's FINISHED or FAILED must carry:
 *   - mem.peak at least what the job took, in bytes, kind peak, by the method this
 *     platform offers: the job's cgroup in cgroup mode, wait4's largest process on
 *     Linux and macOS (for an OOP job, its own host_fork method), the job object's
 *     committed memory on Windows. On Linux without a cgroup a program's peak may be
 *     absent, but only where the host is about as big as the job (docs/
 *     MEASUREMENTS.md: the copy of the host it was forked as) - and a host far
 *     bigger than its job must never pass its own size off as the job's;
 *   - cpu.user_ms and cpu.sys_ms, kind total - and a job that spins for 400 ms shows
 *     at least half of it;
 *   - io.* where the platform reports bytes (Linux with task I/O accounting, a cgroup
 *     with the io controller, Windows), and nowhere else;
 *   - mem.cap_hit only in cgroup mode, where a job over its cap reports 1;
 *   - at most one entry per name, and nothing on an in-process task's events.
 * With measure.sample_ms set, a running job emits SAMPLE events between its STARTED
 * and its FINISHED, each one mem.current; with it at 0 (the default), none. And
 * gptps_stats folds all of it into rows keyed by name and method, merges them, and
 * takes measurements folded by hand - unknown names included.
 *
 * Cgroup mode is on when GPTPS_CGROUP_PARENT names a delegated cgroup; CI's cgroup
 * step sets it, and GPTPS_TEST_EXPECT_CGROUP=1 to make the cgroup methods required
 * rather than accepted.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#  define _DARWIN_C_SOURCE
#endif
#include "gptps.h"
#include "gptps_stats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#  include <unistd.h>
#endif

#ifndef HELPER_PATH
#  error "HELPER_PATH must point at prog_helper"
#endif

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

#define MiB (1024ull * 1024ull)

/* ---- what the platform should report ------------------------------------ */
static int g_cgroup;          /* GPTPS_CGROUP_PARENT is set: jobs may run in cgroups */
static int g_cgroup_strict;   /* ...and CI says they must */
#if !defined(_WIN32)
static int g_fork;            /* the attempt running is an OOP job: a fork of this host */
#endif

#if defined(__linux__)
static int task_io_accounting(void) { return access("/proc/self/io", R_OK) == 0; }

/* This process's resident size now, in bytes. */
static uint64_t host_rss(void)
{
    unsigned long size = 0, res = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    if (fscanf(f, "%lu %lu", &size, &res) != 2) res = 0;
    fclose(f);
    return (uint64_t)res * (uint64_t)sysconf(_SC_PAGESIZE);
}
#else
static uint64_t host_rss(void) { return 0; }
#endif

static int one_of(const char *s, const char *a, const char *b)
{
    return s && ((a && strcmp(s, a) == 0) || (b && strcmp(s, b) == 0));
}

/* mem.peak's method, as this platform measures a process job (an OOP one: g_fork). */
static int peak_method_ok(const char *m)
{
#if defined(_WIN32)
    return one_of(m, "jobobject.tree.committed", NULL);
#else
    const char *rusage = g_fork ? "rusage.host_fork.resident" : "rusage.largest_process.resident";
#  if defined(__linux__)
    if (g_cgroup_strict) return one_of(m, "cgroup.job.resident", NULL);
    if (g_cgroup) return one_of(m, "cgroup.job.resident", rusage);
#  endif
    return one_of(m, rusage, NULL);
#endif
}

static int cpu_method_ok(const char *m)
{
#if defined(_WIN32)
    return one_of(m, "jobobject.tree", NULL);
#elif defined(__linux__)
    if (g_cgroup_strict) return one_of(m, "cgroup.job", NULL);
    if (g_cgroup) return one_of(m, "cgroup.job", "rusage.process");
    return one_of(m, "rusage.process", NULL);
#else
    return one_of(m, "rusage.process", NULL);
#endif
}

static int sample_method_ok(const char *m)
{
#if defined(_WIN32)
    return one_of(m, "jobobject.tree.committed", NULL);
#elif defined(__linux__)
    const char *procfs = g_fork ? "procfs.host_fork.resident" : "procfs.process.resident";
    if (g_cgroup_strict) return one_of(m, "cgroup.job.resident", NULL);
    if (g_cgroup) return one_of(m, "cgroup.job.resident", procfs);
    return one_of(m, procfs, NULL);
#elif defined(__APPLE__)
    return one_of(m, g_fork ? "libproc.host_fork.resident" : "libproc.process.resident", NULL);
#else
    (void)m; return 0;
#endif
}

/* ---- the events of one attempt ------------------------------------------- */
#define MAXM 16
typedef struct {
    int           started, ended, samples, order_ok, sample_shape_ok;
    int           end_kind;
    gptps_status  end_status;
    size_t        n;                     /* the end's measurements, copied */
    gptps_measure m[MAXM];
    char          names[MAXM][64], methods[MAXM][64];
    uint64_t      sample_max;
    char          sample_method[64];
} attempt;

static attempt  g_at;
static int      g_measures_on_other_kinds;   /* a measurement where none belongs */
static uint64_t g_host;                      /* host_rss() as the attempt started */

static void reset_attempt(void)
{
    memset(&g_at, 0, sizeof g_at);
    g_at.order_ok = 1; g_at.sample_shape_ok = 1;
}

static void on_ev(const gptps_event *ev, void *ud)
{
    size_t i;
    (void)ud;
    CHECK(ev->struct_size == sizeof *ev);
    CHECK((ev->measures == NULL) == (ev->n_measures == 0));
    switch (ev->kind) {
    case GPTPS_EV_STARTED:
        if (g_at.started || g_at.ended) g_at.order_ok = 0;
        g_at.started++;
        break;
    case GPTPS_EV_SAMPLE:
        if (!g_at.started || g_at.ended) g_at.order_ok = 0;   /* inside the attempt only */
        g_at.samples++;
        if (ev->n_measures != 1 || strcmp(ev->measures[0].name, GPTPS_M_MEM_CURRENT) != 0 ||
            ev->measures[0].unit != GPTPS_UNIT_BYTES || ev->measures[0].kind != GPTPS_MEASURE_CURRENT ||
            !sample_method_ok(ev->measures[0].method) || ev->measures[0].flags != 0)
            g_at.sample_shape_ok = 0;
        else {
            if (ev->measures[0].value > g_at.sample_max) g_at.sample_max = ev->measures[0].value;
            snprintf(g_at.sample_method, sizeof g_at.sample_method, "%s", ev->measures[0].method);
        }
        break;
    case GPTPS_EV_FINISHED:
    case GPTPS_EV_FAILED:
        if (!g_at.started || g_at.ended) g_at.order_ok = 0;
        g_at.ended++;
        g_at.end_kind = ev->kind; g_at.end_status = ev->status;
        g_at.n = ev->n_measures < MAXM ? ev->n_measures : MAXM;
        for (i = 0; i < g_at.n; ++i) {
            g_at.m[i] = ev->measures[i];
            snprintf(g_at.names[i], sizeof g_at.names[i], "%s", ev->measures[i].name);
            snprintf(g_at.methods[i], sizeof g_at.methods[i], "%s", ev->measures[i].method);
            g_at.m[i].name = g_at.names[i]; g_at.m[i].method = g_at.methods[i];
        }
        /* the lookup agrees with the array, and knows nothing it does not have */
        for (i = 0; i < ev->n_measures; ++i)
            CHECK(gptps_event_measure(ev, ev->measures[i].name) == &ev->measures[i]);
        CHECK(gptps_event_measure(ev, "no.such.measure") == NULL);
        CHECK(gptps_event_measure(ev, NULL) == NULL);
        break;
    default:
        if (ev->n_measures) g_measures_on_other_kinds++;
        break;
    }
}

static const gptps_measure *find(const char *name)
{
    size_t i;
    for (i = 0; i < g_at.n; ++i) if (strcmp(g_at.m[i].name, name) == 0) return &g_at.m[i];
    return NULL;
}

/* Every name at most once, every entry well-formed. */
static void check_shape(void)
{
    size_t i, j;
    for (i = 0; i < g_at.n; ++i) {
        CHECK(g_at.m[i].flags == 0);
        CHECK(g_at.m[i].unit >= GPTPS_UNIT_BYTES && g_at.m[i].unit <= GPTPS_UNIT_FLAG);
        CHECK(g_at.m[i].kind >= GPTPS_MEASURE_PEAK && g_at.m[i].kind <= GPTPS_MEASURE_CURRENT);
        for (j = i + 1; j < g_at.n; ++j) CHECK(strcmp(g_at.m[i].name, g_at.m[j].name) != 0);
    }
}

/* What every measured process attempt reports, whatever it did. `host` is this
 * process's resident size when the job started (Linux; else 0). */
static void check_process_end(uint64_t at_least_mem, int in_cgroup, uint64_t host)
{
    const gptps_measure *pk = find(GPTPS_M_MEM_PEAK), *u = find(GPTPS_M_CPU_USER_MS),
                        *sy = find(GPTPS_M_CPU_SYS_MS), *r = find(GPTPS_M_IO_READ_BYTES),
                        *w = find(GPTPS_M_IO_WRITE_BYTES), *cap = find(GPTPS_M_MEM_CAP_HIT);
    int may_lack_peak = 0;
#if defined(__linux__)
    /* A program's own peak is told apart from the copy of the host it was forked as
     * only above that copy's size: one that took less than the host holds may have
     * no peak, and one that took clearly more must have one. */
    may_lack_peak = !in_cgroup && !g_fork && host + 4 * MiB >= at_least_mem;
#else
    (void)host;
#endif
    check_shape();
    CHECK(pk != NULL || may_lack_peak);
    if (pk) {
        CHECK(pk->unit == GPTPS_UNIT_BYTES && pk->kind == GPTPS_MEASURE_PEAK);
        CHECK(peak_method_ok(pk->method));
        CHECK(pk->value >= at_least_mem);
        CHECK(pk->value < (uint64_t)64 * 1024 * MiB);      /* a sanity bound, not a measure */
        if (!peak_method_ok(pk->method) || pk->value < at_least_mem)
            printf("  mem.peak %llu via %s (wanted at least %llu)\n", (unsigned long long)pk->value,
                   pk->method, (unsigned long long)at_least_mem);
    }
    CHECK(u != NULL && sy != NULL);
    if (u && sy) {
        CHECK(u->unit == GPTPS_UNIT_MS && u->kind == GPTPS_MEASURE_TOTAL && cpu_method_ok(u->method));
        CHECK(sy->unit == GPTPS_UNIT_MS && sy->kind == GPTPS_MEASURE_TOTAL && cpu_method_ok(sy->method));
        CHECK(strcmp(u->method, sy->method) == 0);
    }
    /* io: bytes where the platform counts bytes, absent where it does not */
#if defined(_WIN32)
    CHECK(r != NULL && w != NULL);
    if (r && w) CHECK(one_of(r->method, "jobobject.tree.all", NULL) && strcmp(r->method, w->method) == 0);
#elif defined(__linux__)
    if (in_cgroup || task_io_accounting()) {
        CHECK(r != NULL && w != NULL);
        if (r && w) CHECK(one_of(r->method, "cgroup.job.block", "rusage.process.block") &&
                          strcmp(r->method, w->method) == 0);
    } else {
        CHECK(r == NULL && w == NULL);
    }
#else
    CHECK(r == NULL && w == NULL);           /* macOS counts operations, not bytes */
#endif
    if (r) CHECK(r->unit == GPTPS_UNIT_BYTES && r->kind == GPTPS_MEASURE_TOTAL);
    if (w) CHECK(w->unit == GPTPS_UNIT_BYTES && w->kind == GPTPS_MEASURE_TOTAL);
    /* the cap: reported only by a cgroup, which records reaching it */
    if (in_cgroup) {
        CHECK(cap != NULL);
        if (cap) CHECK(cap->unit == GPTPS_UNIT_FLAG && cap->kind == GPTPS_MEASURE_FLAG &&
                       one_of(cap->method, "cgroup.job", NULL) && cap->value <= 1);
    } else {
        CHECK(cap == NULL);
    }
}

/* ---- tasks --------------------------------------------------------------- */
static gptps_status inproc_ok(gptps_ctx *ctx, void *ud) { (void)ctx; (void)ud; return GPTPS_OK; }

#if !defined(_WIN32)
static gptps_status oop_take(gptps_ctx *ctx, void *ud)
{
    size_t n = (size_t)24 << 20, i;
    volatile char *p = (volatile char *)malloc(n);
    (void)ctx; (void)ud;
    if (!p) return GPTPS_E_NOMEM;
    for (i = 0; i < n; i += 4096) p[i] = 1;
    return GPTPS_OK;                        /* the child exits; nothing to free */
}
#endif

static void reg_program(gptps *e, const char *name, const char *const *argv, uint64_t mem)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.exec = GPTPS_EXEC_PROGRAM; d.argv = argv;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = mem;
    d.default_policy.struct_size = sizeof d.default_policy; d.default_policy.timeout_seconds = 30;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}

/* Run one item of `task` to its end on the MANUAL engine. */
static void run_one(gptps *e, const char *task)
{
    gptps_handle h;
    size_t ran = 1;
    int guard = 0;
    reset_attempt();
    g_host = host_rss();
    CHECK(gptps_submit(e, task, NULL, 0, &h) == GPTPS_OK);
    while (gptps_step(e, &ran) == GPTPS_OK && ran && ++guard < 8) { }
    CHECK(g_at.started == 1 && g_at.ended == 1 && g_at.order_ok);
}

/* ---- gptps_stats, fed by hand -------------------------------------------- */
static void check_stats_by_hand(void)
{
    gptps_stats *s = gptps_stats_open();
    gptps_stats_measure a, b, m;
    gptps_stats_counters c;
    gptps_measure x[3];
    char nm[GPTPS_STATS_NAME_MAX + 8], name[GPTPS_STATS_NAME_MAX], method[GPTPS_STATS_NAME_MAX];
    CHECK(s != NULL);
    if (!s) return;
    /* an add-on's own name, unknown to the core, folds like any other */
    memset(x, 0, sizeof x);
    x[0].name = "acme.widgets"; x[0].value = 7; x[0].unit = GPTPS_UNIT_COUNT;
    x[0].kind = GPTPS_MEASURE_TOTAL; x[0].method = "acme.counter";
    x[1] = x[0]; x[1].value = 3;
    CHECK(gptps_stats_measure_fold(s, "t", &x[0], 1, 10) == GPTPS_OK);
    CHECK(gptps_stats_measure_fold(s, "t", &x[1], 1, 20) == GPTPS_OK);
    m.struct_size = sizeof m;
    CHECK(gptps_stats_measure_get(s, "t", "acme.widgets", "acme.counter", &m) == GPTPS_OK);
    CHECK(m.count == 2 && m.sum == 10 && m.min == 3 && m.max == 7 && m.last == 3 && m.last_ms == 20);
    CHECK(m.unit == GPTPS_UNIT_COUNT && m.kind == GPTPS_MEASURE_TOTAL);
    CHECK(gptps_stats_measure_get(s, NULL, "acme.widgets", "acme.counter", &m) == GPTPS_OK && m.count == 2);
    /* the same name by another method is another row, never mixed in */
    x[2] = x[0]; x[2].method = "acme.other"; x[2].value = 100;
    CHECK(gptps_stats_measure_fold(s, "t", &x[2], 1, 30) == GPTPS_OK);
    CHECK(gptps_stats_measure_count(s, "t") == 2);
    CHECK(gptps_stats_measure_get(s, "t", "acme.widgets", "acme.counter", &m) == GPTPS_OK && m.max == 7);
    CHECK(gptps_stats_measure_at(s, "t", 1, name, sizeof name, method, sizeof method, &m) == GPTPS_OK);
    CHECK(strcmp(name, "acme.widgets") == 0 && strcmp(method, "acme.other") == 0 && m.max == 100);
    CHECK(gptps_stats_measure_at(s, "t", 2, NULL, 0, NULL, 0, NULL) == GPTPS_E_NOTFOUND);
    CHECK(gptps_stats_measure_get(s, "nope", "acme.widgets", "acme.counter", &m) == GPTPS_E_NOTFOUND);
    /* what cannot be folded is counted, never mixed or lost silently: a different
     * unit under a known name and method, and a name too long to key */
    x[1].unit = GPTPS_UNIT_BYTES;
    CHECK(gptps_stats_measure_fold(s, "t", &x[1], 1, 40) == GPTPS_OK);
    memset(nm, 'n', sizeof nm - 1); nm[sizeof nm - 1] = 0;
    x[1] = x[0]; x[1].name = nm;
    CHECK(gptps_stats_measure_fold(s, "t", &x[1], 1, 50) == GPTPS_OK);
    c.struct_size = sizeof c;
    CHECK(gptps_stats_task(s, "t", &c) == GPTPS_OK && c.measures_dropped == 2);
    CHECK(gptps_stats_total(s, &c) == GPTPS_OK && c.measures_dropped == 2);
    CHECK(gptps_stats_measure_get(s, "t", "acme.widgets", "acme.counter", &m) == GPTPS_OK && m.count == 2);
    CHECK(gptps_stats_measure_fold(NULL, "t", x, 1, 0) == GPTPS_E_INVAL);
    CHECK(gptps_stats_measure_fold(s, NULL, x, 1, 0) == GPTPS_E_INVAL);
    CHECK(gptps_stats_measure_fold(s, "t", NULL, 1, 0) == GPTPS_E_INVAL);
    /* merge: counts and sums add, extremes win, the later value is the latest */
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.struct_size = b.struct_size = sizeof a;
    a.unit = b.unit = GPTPS_UNIT_BYTES; a.kind = b.kind = GPTPS_MEASURE_PEAK;
    a.count = 2; a.sum = 30; a.min = 10; a.max = 20; a.last = 20; a.last_ms = 100;
    b.count = 1; b.sum = 5;  b.min = 5;  b.max = 5;  b.last = 5;  b.last_ms = 200;
    gptps_stats_measure_merge(&a, &b);
    CHECK(a.count == 3 && a.sum == 35 && a.min == 5 && a.max == 20 && a.last == 5 && a.last_ms == 200);
    memset(&b, 0, sizeof b);
    gptps_stats_measure_merge(&a, &b);                  /* an empty row changes nothing */
    CHECK(a.count == 3 && a.sum == 35);
    memset(&b, 0, sizeof b);
    gptps_stats_measure_merge(&b, &a);                  /* into an empty row: a copy */
    CHECK(b.count == 3 && b.min == 5 && b.max == 20 && b.unit == GPTPS_UNIT_BYTES);
    gptps_stats_reset(s);
    CHECK(gptps_stats_measure_count(s, "t") == 0 && gptps_stats_measure_count(s, NULL) == 0);
    gptps_stats_close(s);
}

int main(void)
{
    static const char *const av_big[]   = { HELPER_PATH, "mem", "64", "300", NULL };
    static const char *const av_spin[]  = { HELPER_PATH, "spin", "400", NULL };
    static const char *const av_fail[]  = { HELPER_PATH, "mem", "8", "0", "3", NULL };
    static const char *const av_slow[]  = { HELPER_PATH, "mem", "48", "900", NULL };
    static const char *const av_over[]  = { HELPER_PATH, "mem", "96", "200", NULL };
    static const char *const av_small[] = { HELPER_PATH, "mem", "8", "0", NULL };
    const char *cg = getenv("GPTPS_CGROUP_PARENT"), *strict = getenv("GPTPS_TEST_EXPECT_CGROUP");
    uint64_t declare;
    gptps_config cfg;
    gptps *e = NULL;
    gptps_stats *st;
    gptps_stats_measure sm;
    gptps_task_def d;
    char v[32];
    int prog_attempts = 0;

    g_cgroup = (cg && *cg) ? 1 : 0;
#if defined(__linux__)
    g_cgroup_strict = g_cgroup && strict && strcmp(strict, "1") == 0;
#else
    (void)strict;
    g_cgroup = 0;                            /* cgroup mode is Linux only */
#endif
    /* A job gets a cgroup only with a cap of 16 MiB or more. In cgroup mode the jobs
     * declare one big enough never to bind; elsewhere they declare none, so no
     * RLIMIT_AS meets a sanitizer runtime's reservations in the helper. */
    declare = g_cgroup ? (uint64_t)1024 * MiB : 0;

    check_stats_by_hand();

    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.mode = GPTPS_RUN_MANUAL;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (!e) return 1;
    CHECK(gptps_set_event_cb(e, on_ev, NULL) == GPTPS_OK);
    st = gptps_stats_install(e);
    CHECK(st != NULL);

    /* measure.sample_ms: off by default, live, range-checked */
    CHECK(gptps_settings_get(e, "measure.sample_ms", v, sizeof v) == GPTPS_OK && strcmp(v, "0") == 0);
    CHECK(gptps_settings_set(e, "measure.sample_ms", "3600001") != GPTPS_OK);

    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "inproc"; d.run = inproc_ok;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    reg_program(e, "big",  av_big,  declare);
    reg_program(e, "spin", av_spin, declare);
    reg_program(e, "fail", av_fail, declare);
    reg_program(e, "slow", av_slow, declare);
    reg_program(e, "small", av_small, declare);
#if !defined(_WIN32)
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "oop"; d.run = oop_take; d.exec = GPTPS_EXEC_OOP;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = declare;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
#endif

    /* 1) an in-process task is never measured */
    run_one(e, "inproc");
    CHECK(g_at.end_kind == GPTPS_EV_FINISHED && g_at.n == 0);

    /* 2) a program that takes 64 MiB: its peak, CPU and I/O, by this platform's method */
    run_one(e, "big"); ++prog_attempts;
    CHECK(g_at.end_kind == GPTPS_EV_FINISHED);
    check_process_end(64 * MiB, g_cgroup_strict, g_host);
    if (g_cgroup_strict) { const gptps_measure *c = find(GPTPS_M_MEM_CAP_HIT); CHECK(c && c->value == 0); }

    /* 3) CPU time: a 400 ms spin shows at least half of it */
    run_one(e, "spin"); ++prog_attempts;
    CHECK(g_at.end_kind == GPTPS_EV_FINISHED);
    check_process_end(0, g_cgroup_strict, g_host);
    {
        const gptps_measure *u = find(GPTPS_M_CPU_USER_MS), *sy = find(GPTPS_M_CPU_SYS_MS);
        CHECK(u && sy && u->value + sy->value >= 200);
        if (u && sy && u->value + sy->value < 200)
            printf("  spin: user %llu ms + sys %llu ms\n", (unsigned long long)u->value, (unsigned long long)sy->value);
    }

    /* 4) a failed attempt reports what it used, on its FAILED */
    run_one(e, "fail"); ++prog_attempts;
    CHECK(g_at.end_kind == GPTPS_EV_FAILED && g_at.end_status == GPTPS_E_TASK);
    check_process_end(8 * MiB, g_cgroup_strict, g_host);

#if !defined(_WIN32)
    /* 5) the OOP executor measures its forked child the same way, under the method
     * that says the process is a fork of the host */
    g_fork = 1;
    run_one(e, "oop");
    CHECK(g_at.end_kind == GPTPS_EV_FINISHED);
    check_process_end(24 * MiB, g_cgroup_strict, g_host);
    g_fork = 0;

    /* 5b) a host far bigger than its program: whatever is reported is the program's
     * own. Linux starts a fork's resident high-water mark at the host's resident size
     * and carries it across exec into ru_maxrss, so there the peak must be absent
     * (or the cgroup's); a peak of the host's size would be the host's. */
    {
        size_t n = (size_t)(192 * MiB), i;
        volatile char *fat = (volatile char *)malloc(n);
        const gptps_measure *pk;
        CHECK(fat != NULL);
        if (fat) {
            for (i = 0; i < n; i += 4096) fat[i] = 1;
            run_one(e, "small");
            CHECK(g_at.end_kind == GPTPS_EV_FINISHED);
            check_process_end(8 * MiB, g_cgroup_strict, g_host);
            pk = find(GPTPS_M_MEM_PEAK);
            CHECK(pk == NULL || pk->value < 96 * MiB);
#  if defined(__linux__)
            if (!g_cgroup) CHECK(pk == NULL);
#  endif
            if (pk && pk->value >= 96 * MiB)
                printf("  small program under a 192 MiB host: mem.peak %llu via %s\n",
                       (unsigned long long)pk->value, pk->method);
            free((void *)fat);
        }
    }
#endif

    /* 6) sampling: off, no SAMPLE; on, samples inside the attempt, each mem.current */
    run_one(e, "slow"); ++prog_attempts;
    CHECK(g_at.samples == 0);
    CHECK(gptps_settings_set(e, "measure.sample_ms", "50") == GPTPS_OK);
    run_one(e, "slow"); ++prog_attempts;
    CHECK(g_at.end_kind == GPTPS_EV_FINISHED);
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    CHECK(g_at.samples >= 3);
    CHECK(g_at.sample_shape_ok);
    CHECK(g_at.sample_max >= 48 * MiB);
    if (g_at.samples < 3 || !g_at.sample_shape_ok || g_at.sample_max < 48 * MiB)
        printf("  %d samples, max %llu via %s\n", g_at.samples, (unsigned long long)g_at.sample_max,
               g_at.sample_method);
#endif
    CHECK(gptps_settings_set(e, "measure.sample_ms", "0") == GPTPS_OK);
    run_one(e, "slow"); ++prog_attempts;
    CHECK(g_at.samples == 0);

    /* 7) cgroup mode: a job over its cap is killed by it, and says it hit it */
    if (g_cgroup_strict) {
        reg_program(e, "over", av_over, 32 * MiB);
        run_one(e, "over");
        CHECK(g_at.end_kind == GPTPS_EV_FAILED && g_at.end_status == GPTPS_E_NOMEM);
        check_process_end(0, 1, g_host);
        { const gptps_measure *c = find(GPTPS_M_MEM_CAP_HIT); CHECK(c && c->value == 1); }
        /* at the cap, give or take: the kernel lets a task it is killing charge a little
         * past memory.max on its way out */
        { const gptps_measure *p = find(GPTPS_M_MEM_PEAK); CHECK(p && p->value >= 24 * MiB && p->value <= 40 * MiB); }
    }

    CHECK(g_measures_on_other_kinds == 0);

    /* 8) gptps_stats folded every attempt, by name and method */
    sm.struct_size = sizeof sm;
    {
        char name[GPTPS_STATS_NAME_MAX], method[GPTPS_STATS_NAME_MAX];
        size_t n = gptps_stats_measure_count(st, "big"), k;
        int found = 0;
        CHECK(n >= 3);                                        /* mem.peak, cpu.user_ms, cpu.sys_ms, ... */
        for (k = 0; k < n; ++k) {
            CHECK(gptps_stats_measure_at(st, "big", k, name, sizeof name, method, sizeof method, &sm) == GPTPS_OK);
            CHECK(sm.count == 1);                             /* one attempt of "big" */
            if (strcmp(name, GPTPS_M_MEM_PEAK) == 0) { found = 1; CHECK(sm.max >= 64 * MiB && peak_method_ok(method)); }
        }
        CHECK(found);
        CHECK(gptps_stats_measure_count(st, "inproc") == 0);
        /* engine-wide: every program attempt reported its CPU */
        n = gptps_stats_measure_count(st, NULL);
        found = 0;
        for (k = 0; k < n; ++k) {
            CHECK(gptps_stats_measure_at(st, NULL, k, name, sizeof name, method, sizeof method, &sm) == GPTPS_OK);
            if (strcmp(name, GPTPS_M_CPU_USER_MS) == 0) found += (int)sm.count;
            if (strcmp(name, GPTPS_M_MEM_CURRENT) == 0) CHECK(sm.kind == GPTPS_MEASURE_CURRENT && sm.count >= 3);
        }
#if !defined(_WIN32)
        CHECK(found >= prog_attempts + 1);                    /* + the OOP attempt */
#else
        CHECK(found >= prog_attempts);
#endif
    }
    {
        gptps_stats_counters c;
        c.struct_size = sizeof c;
        CHECK(gptps_stats_total(st, &c) == GPTPS_OK && c.measures_dropped == 0);
    }

    gptps_shutdown(e);
    gptps_stats_close(st);
    if (fails) { printf("%d measure check(s) FAILED\n", fails); return 1; }
    printf("all measure checks passed (%s)\n", g_cgroup_strict ? "cgroup mode" : g_cgroup ? "cgroup mode, lenient" : "no cgroup");
    return 0;
}
