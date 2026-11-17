/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_engine_fixes.c - the core's fixes before 1.10.0, each checked where it shows:
 *
 *  1) The starvation guard does not reserve for a top item that cannot fit beside
 *     what running SERVICES hold - they never drain, so that reservation stopped all
 *     admission for as long as they lived, silently. Now other work goes on, one
 *     WARN says why, and the top item starts once the service stops. A second large
 *     item behind it is protected as well: every item a backfill passes is charged.
 *  2) MANUAL mode: gptps_is_cancelled() turns true at the deadline (no watchdog runs
 *     beside an inline body), and a body that returns after its deadline is
 *     GPTPS_E_TIMEOUT, as in THREADED mode - not FINISHED.
 *  3) GPTPS_SUBMIT_TIMEOUT_MS reaches PROGRAM jobs: threaded to the slice, MANUAL to
 *     the second rounded up. It used to be dropped without a word. A cancel issued
 *     before that deadline stays a cancel, however long its graceful stop takes.
 *  4) A dead letter's stderr tail is its last failed attempt's own: an attempt that
 *     wrote nothing leaves none, not an earlier attempt's.
 *  5) Every per-task key goes from the config file into the def (a walk over all of
 *     them, so a key the file accepts cannot do nothing), stop_grace_ms and
 *     stderr_tail_bytes among them, and a file's stderr_tail_bytes takes effect.
 *  6) GPTPS_TASK_* flags are distinct bits: DIE_WITH_HOST no longer reads back as
 *     RETIRE_ON_OK.
 *  7) gptps_clone_task keeps values set past an older registrant's struct_size.
 *  8) The host table has task_flags and event_measure (ABI 2.8), for binary plug-ins.
 *  9) An add-on that does not load says why: the OS's reason, or which check.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "gptps.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#  include <windows.h>
#  include <direct.h>
#  define make_dir(p)   _mkdir(p)
#  define remove_dir(p) _rmdir(p)
#else
#  include <sys/stat.h>
#  include <unistd.h>
#  define make_dir(p)   mkdir((p), 0700)
#  define remove_dir(p) rmdir(p)
#endif

#ifndef HELPER_PATH
#define HELPER_PATH "./prog_helper"
#endif
#ifndef ADDON_HOSTAPI_PATH
#define ADDON_HOSTAPI_PATH "./addon_hostapi.so"
#endif
#ifndef ADDON_BAD_PATH
#define ADDON_BAD_PATH "./addon_bad.so"
#endif

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int  inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int  get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static void put(int *p, int v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }

static void sleep_ms(unsigned ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u); ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) != 0) { }
#endif
}

static int wait_for(int *p, int want, unsigned ms)
{
    unsigned waited = 0;
    while (get(p) < want && waited < ms) { sleep_ms(5); waited += 5; }
    return get(p) >= want;
}

/* --- the log: every WARN and ERROR line, kept --------------------------------- */
#define NLOG 128
static char g_log[NLOG][600];
static int  g_nlog;
static void on_log(gptps_log_level lvl, const char *msg, void *ud)
{
    int i;
    (void)ud;
    if (lvl < GPTPS_LOG_WARN || !msg) return;
    i = inc(&g_nlog) - 1;
    if (i < NLOG) snprintf(g_log[i], sizeof g_log[i], "%s", msg);
}
/* How many lines have all of `a` and (if given) `b` in them. */
static int logged(const char *a, const char *b)
{
    int i, n = get(&g_nlog), c = 0;
    if (n > NLOG) n = NLOG;
    for (i = 0; i < n; ++i)
        if (strstr(g_log[i], a) && (!b || strstr(g_log[i], b))) ++c;
    return c;
}
static void log_reset(void) { put(&g_nlog, 0); }
static void log_dump(void)
{
    int i, n = get(&g_nlog);
    if (n > NLOG) n = NLOG;
    for (i = 0; i < n; ++i) printf("  log: %s\n", g_log[i]);
}

static void base_def(gptps_task_def *d, const char *name, gptps_exec_kind exec)
{
    memset(d, 0, sizeof *d);
    d->struct_size = sizeof *d; d->name = name; d->exec = exec;
    d->default_cost.struct_size = sizeof d->default_cost;
    d->default_policy.struct_size = sizeof d->default_policy;
}

static gptps *open_engine(gptps_run_mode mode, uint32_t conc, uint64_t mem, const char *cfg_path)
{
    gptps_config cfg; gptps *e = NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = conc;
    cfg.limits.max_memory_bytes = mem;
    cfg.mode = mode;
    cfg.config_path = cfg_path;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    return e;
}

/* --- the events, by task name --------------------------------------------------- */
typedef struct { const char *name; int started, finished, failed, first_status, last_status, last_err_len; } tally;
static tally g_t[8];
static int   g_nt;
static tally *tally_of(const char *name)
{
    int i;
    for (i = 0; i < g_nt; ++i) if (strcmp(g_t[i].name, name) == 0) return &g_t[i];
    return NULL;
}
static void tally_reset(void) { memset(g_t, 0, sizeof g_t); g_nt = 0; }
static void track(const char *name) { g_t[g_nt++].name = name; }

static void (*g_on_first_fail)(void);   /* 4): what changes between attempt 1 and 2 */
static void on_ev(const gptps_event *ev, void *ud)
{
    tally *t = tally_of(ev->task_name);
    (void)ud;
    if (!t) return;
    switch (ev->kind) {
        case GPTPS_EV_STARTED:  inc(&t->started); break;
        case GPTPS_EV_FINISHED: inc(&t->finished); break;
        case GPTPS_EV_FAILED:
            if (get(&t->failed) == 0) put(&t->first_status, (int)ev->status);
            put(&t->last_status, (int)ev->status);
            put(&t->last_err_len, (int)ev->stderr_tail_len);
            if (g_on_first_fail && ev->attempt == 1) g_on_first_fail();
            inc(&t->failed);
            break;
        default: break;
    }
}

/* ============================================================== 1) the guard */
static gptps_status run_service(gptps_ctx *c, void *ud)
{
    (void)ud;
    while (!gptps_is_cancelled(c)) sleep_ms(2);
    return GPTPS_E_CANCELLED;
}
static gptps_status run_short(gptps_ctx *c, void *ud) { (void)c; (void)ud; sleep_ms(5); return GPTPS_OK; }

static void test_guard_beside_services(void)
{
    gptps *e = open_engine(GPTPS_RUN_THREADED, 4, 1000, NULL);
    gptps_task_def d;
    gptps_handle svc = 0;
    int i;
    if (!e) return;
    tally_reset(); track("svc"); track("big"); track("small"); log_reset();
    CHECK(gptps_set_event_cb(e, on_ev, NULL) == GPTPS_OK);
    base_def(&d, "svc", GPTPS_EXEC_INPROC); d.run = run_service; d.flags = GPTPS_TASK_SERVICE;
    d.default_cost.mem_bytes = 600;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    base_def(&d, "big", GPTPS_EXEC_INPROC); d.run = run_short; d.default_cost.mem_bytes = 500;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    base_def(&d, "small", GPTPS_EXEC_INPROC); d.run = run_short; d.default_cost.mem_bytes = 10;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(gptps_set_task_priority(e, "big", 10) == GPTPS_OK);

    CHECK(gptps_submit(e, "svc", NULL, 0, &svc) == GPTPS_OK);
    CHECK(wait_for(&tally_of("svc")->started, 1, 5000));
    CHECK(gptps_submit(e, "big", NULL, 0, NULL) == GPTPS_OK);
    for (i = 0; i < 30; ++i) CHECK(gptps_submit(e, "small", NULL, 0, NULL) == GPTPS_OK);

    /* All 30 run past `big`: it needs 500, and 600 of the 1000 are the service's for
     * as long as it is up. Before, the guard reserved for `big` after 8 and the other
     * 22 never started, with nothing logged. */
    CHECK(wait_for(&tally_of("small")->finished, 30, 10000));
    CHECK(get(&tally_of("big")->started) == 0);
    CHECK(logged("task 'big'", "services") == 1);     /* said, once - not once per pass */
    /* the service stops: now `big` fits, and starts */
    CHECK(gptps_cancel(e, svc) == GPTPS_OK);
    CHECK(wait_for(&tally_of("big")->finished, 1, 5000));
    if (fails) log_dump();
    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

/* 1b) A second large item behind an unreservable top is protected too. `big` cannot
 * fit beside the service; `mid` (300) can once the running non-service work drains,
 * but a steady stream of `chunk` (110 each, so 3 at a time, and 600 + 110 + 300 > 1000)
 * keeps it from fitting on its own: one chunk running is enough to block it, and the
 * chunks' staggered lengths keep them from all ending between two passes. Skips used to be charged to the head of the queue
 * only, so `mid` was never reserved for: it waited for the whole stream to drain,
 * with no line naming it. Now every item a backfill passes is charged, and the guard
 * reserves for the highest-ranked one it can - `mid`, after its 8 skips. */
static int g_chunks_at_mid = -1;
static gptps_status run_mid(gptps_ctx *c, void *ud)
{
    (void)c; (void)ud;
    put(&g_chunks_at_mid, get(&tally_of("chunk")->started));
    return GPTPS_OK;
}
static int g_chunk_n;
static gptps_status run_chunk(gptps_ctx *c, void *ud)
{
    int n = inc(&g_chunk_n);
    (void)c; (void)ud;
    sleep_ms((unsigned)(4 + (n * 7) % 13));
    return GPTPS_OK;
}
static void test_guard_second_item(void)
{
    gptps *e = open_engine(GPTPS_RUN_THREADED, 8, 1000, NULL);
    gptps_task_def d;
    gptps_handle svc = 0;
    int i;
    if (!e) return;
    tally_reset(); track("svc"); track("big"); track("mid"); track("chunk"); log_reset();
    put(&g_chunks_at_mid, -1);
    CHECK(gptps_set_event_cb(e, on_ev, NULL) == GPTPS_OK);
    base_def(&d, "svc", GPTPS_EXEC_INPROC); d.run = run_service; d.flags = GPTPS_TASK_SERVICE;
    d.default_cost.mem_bytes = 600;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    base_def(&d, "big", GPTPS_EXEC_INPROC); d.run = run_short; d.default_cost.mem_bytes = 500;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    base_def(&d, "mid", GPTPS_EXEC_INPROC); d.run = run_mid; d.default_cost.mem_bytes = 300;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    base_def(&d, "chunk", GPTPS_EXEC_INPROC); d.run = run_chunk; d.default_cost.mem_bytes = 110;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(gptps_set_task_priority(e, "big", 10) == GPTPS_OK);
    CHECK(gptps_set_task_priority(e, "mid", 5) == GPTPS_OK);

    CHECK(gptps_submit(e, "svc", NULL, 0, &svc) == GPTPS_OK);
    CHECK(wait_for(&tally_of("svc")->started, 1, 5000));
    /* the stream first, so chunks are running when `mid` arrives and it does not fit */
    for (i = 0; i < 60; ++i) CHECK(gptps_submit(e, "chunk", NULL, 0, NULL) == GPTPS_OK);
    CHECK(wait_for(&tally_of("chunk")->started, 1, 5000));
    CHECK(gptps_submit(e, "big", NULL, 0, NULL) == GPTPS_OK);
    CHECK(gptps_submit(e, "mid", NULL, 0, NULL) == GPTPS_OK);

    CHECK(wait_for(&tally_of("chunk")->finished, 60, 15000));
    CHECK(get(&tally_of("mid")->finished) == 1);
    /* `mid` ran once it had been passed 8 times and the 3 running chunks drained -
     * not after all 60 (a generous bound: well short of the whole stream) */
    CHECK(get(&g_chunks_at_mid) >= 0 && get(&g_chunks_at_mid) <= 30);
    CHECK(get(&tally_of("big")->started) == 0);
    CHECK(logged("task 'big'", "services") == 1);
    CHECK(gptps_cancel(e, svc) == GPTPS_OK);
    CHECK(wait_for(&tally_of("big")->finished, 1, 5000));
    if (fails) { printf("  chunks started when mid ran: %d\n", get(&g_chunks_at_mid)); log_dump(); }
    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

/* ============================================================ 2) MANUAL deadline */
static int g_polled_out;
static gptps_status run_poll(gptps_ctx *c, void *ud)
{
    uint64_t t0 = gptps_now_ms(c);
    (void)ud;
    while (!gptps_is_cancelled(c)) {
        if (gptps_now_ms(c) - t0 > 3000) return GPTPS_OK;   /* the safety exit: never saw it */
        sleep_ms(1);
    }
    put(&g_polled_out, 1);
    return GPTPS_OK;                    /* "done" - but late: the attempt timed out */
}
static gptps_status run_late(gptps_ctx *c, void *ud) { (void)c; (void)ud; sleep_ms(60); return GPTPS_OK; }

static void test_manual_deadline(void)
{
    gptps *e = open_engine(GPTPS_RUN_MANUAL, 1, 1u << 20, NULL);
    gptps_task_def d;
    gptps_submit_options o;
    size_t ran = 0;
    int guard = 0;
    if (!e) return;
    tally_reset(); track("poll"); track("late");
    CHECK(gptps_set_event_cb(e, on_ev, NULL) == GPTPS_OK);
    base_def(&d, "poll", GPTPS_EXEC_INPROC); d.run = run_poll;
    d.default_policy.on_failure = GPTPS_ON_FAILURE_DROP;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    base_def(&d, "late", GPTPS_EXEC_INPROC); d.run = run_late;
    d.default_policy.on_failure = GPTPS_ON_FAILURE_DROP;
    d.default_policy.timeout_seconds = 0;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);

    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o; o.flags = GPTPS_SUBMIT_TIMEOUT_MS; o.timeout_ms = 20;
    CHECK(gptps_submit_ex(e, "poll", NULL, 0, &o, NULL) == GPTPS_OK);
    o.timeout_ms = 10;
    CHECK(gptps_submit_ex(e, "late", NULL, 0, &o, NULL) == GPTPS_OK);
    while (gptps_step(e, &ran) == GPTPS_OK && ran && ++guard < 100) { }
    /* the polling body saw its deadline, and both late returns are timeouts */
    CHECK(get(&g_polled_out) == 1);
    CHECK(get(&tally_of("poll")->failed) == 1 && get(&tally_of("poll")->finished) == 0);
    CHECK(get(&tally_of("poll")->last_status) == (int)GPTPS_E_TIMEOUT);
    CHECK(get(&tally_of("late")->failed) == 1 && get(&tally_of("late")->finished) == 0);
    CHECK(get(&tally_of("late")->last_status) == (int)GPTPS_E_TIMEOUT);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

/* ========================================================= 3) TIMEOUT_MS, PROGRAM */
static const char *const HANG_ARGV[] = { HELPER_PATH, "hang", 0 };

static void test_program_timeout_ms(gptps_run_mode mode)
{
    gptps *e = open_engine(mode, 1, 1u << 30, NULL);
    gptps_task_def d;
    gptps_submit_options o;
    double secs;
    if (!e) return;
    tally_reset(); track("hang");
    CHECK(gptps_set_event_cb(e, on_ev, NULL) == GPTPS_OK);
    base_def(&d, "hang", GPTPS_EXEC_PROGRAM); d.argv = HANG_ARGV;   /* no timeout_seconds */
    d.default_policy.on_failure = GPTPS_ON_FAILURE_DROP;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o; o.flags = GPTPS_SUBMIT_TIMEOUT_MS; o.timeout_ms = 300;
    {
        time_t t0 = time(NULL);
        CHECK(gptps_submit_ex(e, "hang", NULL, 0, &o, NULL) == GPTPS_OK);
        if (mode == GPTPS_RUN_MANUAL) {
            size_t ran = 0;
            CHECK(gptps_step(e, &ran) == GPTPS_OK && ran == 1);   /* returns: the job was stopped */
            CHECK(gptps_step(e, &ran) == GPTPS_OK);
        } else {
            CHECK(wait_for(&tally_of("hang")->failed, 1, 10000));
        }
        secs = difftime(time(NULL), t0);
    }
    CHECK(get(&tally_of("hang")->failed) == 1);
    CHECK(get(&tally_of("hang")->last_status) == (int)GPTPS_E_TIMEOUT);
    CHECK(secs <= 5.0);                  /* 300 ms - or 1 s in MANUAL - not "never" */
    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

#if !defined(_WIN32)
/* 3b) A cancel issued before the per-submit deadline stays a cancel even when the
 * job's graceful stop (stop_grace_ms) ends after that deadline. execute() used to
 * decide by the clock when the executor returned, and called it GPTPS_E_TIMEOUT -
 * which gptps.h says a cancel never is. POSIX: SIGTERM is the graceful stop. */
#define TERM_FILE "gptps_fix_term.out"
static const char *const TERM_ARGV[] = { HELPER_PATH, "term", TERM_FILE, "2500", 0 };
static void test_cancel_before_deadline(void)
{
    gptps *e = open_engine(GPTPS_RUN_THREADED, 1, 1u << 30, NULL);
    gptps_task_def d;
    gptps_submit_options o;
    gptps_handle h = 0;
    uint64_t t_sub, t_cancel;
    if (!e) return;
    (void)remove(TERM_FILE);
    tally_reset(); track("slowstop");
    CHECK(gptps_set_event_cb(e, on_ev, NULL) == GPTPS_OK);
    base_def(&d, "slowstop", GPTPS_EXEC_PROGRAM); d.argv = TERM_ARGV;
    d.default_policy.on_failure = GPTPS_ON_FAILURE_DROP;
    d.stop_grace_ms = 5000;              /* it cleans up in 2.5 s: well inside */
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o; o.flags = GPTPS_SUBMIT_TIMEOUT_MS; o.timeout_ms = 2000;
    t_sub = gptps_now_ms(NULL);
    CHECK(gptps_submit_ex(e, "slowstop", NULL, 0, &o, &h) == GPTPS_OK);
    CHECK(wait_for(&tally_of("slowstop")->started, 1, 5000));
    sleep_ms(200);                       /* the job has its SIGTERM handler up */
    t_cancel = gptps_now_ms(NULL);       /* the deadline is ~1.8 s away */
    CHECK(gptps_cancel(e, h) == GPTPS_OK);
    CHECK(wait_for(&tally_of("slowstop")->failed, 1, 10000));
    /* it ends ~2.5 s in, past its 2 s deadline - but it was cancelled first. Its one
     * FAILED says so: it used to say GPTPS_E_TIMEOUT, and a second FAILED with
     * GPTPS_E_CANCELLED then closed the handle the cancel had asked to end.
     * The precondition is checked, not assumed: a cancel that lands AFTER the
     * deadline (a host too loaded to send it within 2 s) rightly gets TIMEOUT for
     * the attempt and then a terminal CANCELLED - the documented contract, not this
     * bug - so it is reported as a test that could not run, never as a pass. */
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    e = NULL;
    if (t_cancel - t_sub >= 2000) {
        printf("FAIL cancel_before_deadline could not run: the cancel was sent %llu ms "
               "after the submit, past the 2000 ms deadline\n",
               (unsigned long long)(t_cancel - t_sub));
        ++fails;
    } else {
        CHECK(get(&tally_of("slowstop")->first_status) == (int)GPTPS_E_CANCELLED);
        CHECK(get(&tally_of("slowstop")->failed) == 1);
    }
    {
        FILE *f = fopen(TERM_FILE, "r");
        CHECK(f != NULL);                /* the graceful stop did run */
        if (f) fclose(f);
    }
    (void)remove(TERM_FILE);
}
#endif

/* ======================================================= 4) the dead letter's tail */
#define WD "gptps_fix_wd"
static const char *const ERR_ARGV[] = { HELPER_PATH, "errout", "0", "licence server unreachable", "3", 0 };
static int    g_dl_n, g_dl_tail_len, g_dl_start, g_dl_code, g_dl_first_tail;
#if !defined(_WIN32)
/* POSIX: the job's argv[0] is "./job" in its work_dir - first the helper, which
 * writes the line to stderr and fails, then a script of our own that fails writing
 * nothing. Not /bin/false: macOS has it only in /usr/bin, and a multi-call coreutils
 * (uutils, Ubuntu 25.10+) run through a symlink named "job" says on stderr that it
 * has no such utility. */
#define JOB WD "/job"
static const char *const JOB_ARGV[] = { "./job", "errout", "0", "licence server unreachable", "3", 0 };
static void swap_job(void)
{
    FILE *f;
    (void)unlink(JOB);
    f = fopen(JOB, "w");
    CHECK(f != NULL);
    if (!f) return;
    fputs("#!/bin/sh\nexit 1\n", f);
    CHECK(fclose(f) == 0);
    CHECK(chmod(JOB, 0755) == 0);
}
#else
/* Windows: the work_dir goes, so attempt 2 cannot start - start.error, no stderr. */
static void swap_job(void) { (void)remove_dir(WD); }
#endif
static void on_dl(const gptps_dead_letter *dl, void *ud)
{
    size_t i;
    (void)ud;
    ++g_dl_n;
    g_dl_tail_len = (int)dl->stderr_tail_len;
    if (dl->stderr_tail_len) CHECK(dl->stderr_tail != NULL);
    if (dl->stderr_tail_len && dl->stderr_tail) {
        char t[256];
        size_t n = dl->stderr_tail_len < sizeof t - 1 ? dl->stderr_tail_len : sizeof t - 1;
        memcpy(t, dl->stderr_tail, n); t[n] = 0;
        if (strstr(t, "licence server unreachable")) g_dl_first_tail = 1;
    }
    for (i = 0; i < dl->n_measures; ++i) {
        if (strcmp(dl->measures[i].name, "start.error") == 0) g_dl_start = 1;
        if (strcmp(dl->measures[i].name, "exit.code") == 0)   g_dl_code = 1;
    }
}

static void test_dead_letter_tail(void)
{
    gptps *e = open_engine(GPTPS_RUN_THREADED, 1, 1u << 30, NULL);
    gptps_task_def d;
    if (!e) return;
    tally_reset(); track("erp");
    (void)make_dir(WD);
#if !defined(_WIN32)
    (void)unlink(JOB);
    CHECK(symlink(HELPER_PATH, JOB) == 0);
#endif
    g_on_first_fail = swap_job;
    CHECK(gptps_set_event_cb(e, on_ev, NULL) == GPTPS_OK);
#if !defined(_WIN32)
    base_def(&d, "erp", GPTPS_EXEC_PROGRAM); d.argv = JOB_ARGV; d.work_dir = WD;
#else
    base_def(&d, "erp", GPTPS_EXEC_PROGRAM); d.argv = ERR_ARGV; d.work_dir = WD;
#endif
    d.stderr_tail_bytes = 4096;
    d.default_policy.max_retries = 1;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(gptps_submit(e, "erp", NULL, 0, NULL) == GPTPS_OK);
    CHECK(wait_for(&tally_of("erp")->failed, 2, 10000));
    sleep_ms(100);                      /* the dead-lettering follows the second FAILED */
    g_dl_n = 0; g_dl_tail_len = -1; g_dl_start = g_dl_code = g_dl_first_tail = 0;
    CHECK(gptps_dead_letter_drain(e, on_dl, NULL) == 1);
    CHECK(g_dl_n == 1);
#if !defined(_WIN32)
    CHECK(g_dl_code == 1 && g_dl_start == 0);   /* attempt 2's measurements (the script's exit) ... */
    CHECK(g_dl_tail_len == 0);                  /* ... and its stderr: none, not attempt 1's */
#else
    CHECK(g_dl_start == 1 && g_dl_code == 0);
#endif
    CHECK(g_dl_first_tail == 0);                /* never attempt 1's line beside attempt 2's status */
    g_on_first_fail = NULL;
#if !defined(_WIN32)
    (void)unlink(JOB);
#endif
    (void)remove_dir(WD);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

/* ===================================================== 5) every key, from the file */
#define CFG_FILE "gptps_fix_keys.toml"
typedef struct { char leaf[64]; char value[64]; int quoted; } kv;

static void test_keys_from_file(void)
{
    kv keys[32];
    size_t n = 0, i, cnt;
    char buf[GPTPS_SETTINGS_VALUE_MAX];
    gptps_task_def d;
    gptps *e = open_engine(GPTPS_RUN_THREADED, 1, 1u << 30, NULL);
    FILE *f;
    int pass;
    if (!e) return;
    base_def(&d, "probe", GPTPS_EXEC_INPROC); d.run = run_short;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    /* Walk every built-in per-task key and pick a value that is not its default. */
    cnt = gptps_settings_count(e);
    for (i = 0; i < cnt && n < 32; ++i) {
        gptps_setting_info si;
        const char *leaf;
        memset(&si, 0, sizeof si); si.struct_size = sizeof si;
        if (gptps_settings_get_info(e, i, &si) != GPTPS_OK || strncmp(si.key, "tasks.probe.", 12) != 0) continue;
        leaf = si.key + 12;
        if (strchr(leaf, '.')) continue;          /* a resource cost: not a table key */
        snprintf(keys[n].leaf, sizeof keys[n].leaf, "%s", leaf);
        keys[n].quoted = 0;
        if (si.type == GPTPS_SETTING_ENUM) {
            size_t k = 0;
            while (si.choices && si.choices[k]) ++k;
            CHECK(k >= 2);
            snprintf(keys[n].value, sizeof keys[n].value, "%s", k ? si.choices[k - 1] : "");
            keys[n].quoted = 1;
        } else if (si.type == GPTPS_SETTING_INT) {
            snprintf(keys[n].value, sizeof keys[n].value, "-3");
        } else {
            snprintf(keys[n].value, sizeof keys[n].value, "7");
        }
        CHECK(strcmp(keys[n].value, si.defval) != 0);
        ++n;
    }
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    CHECK(n >= 11);                     /* the six, the three caps, and the two PROGRAM numbers */

    /* [tasks.probe] first, then [task_defaults]: both must reach the def. */
    for (pass = 0; pass < 2; ++pass) {
        f = fopen(CFG_FILE, "w");
        CHECK(f != NULL);
        if (!f) return;
        fprintf(f, pass == 0 ? "[tasks.probe]\n" : "[task_defaults]\n");
        for (i = 0; i < n; ++i)
            fprintf(f, keys[i].quoted ? "%s = \"%s\"\n" : "%s = %s\n", keys[i].leaf, keys[i].value);
        fclose(f);
        e = open_engine(GPTPS_RUN_THREADED, 1, 1u << 30, CFG_FILE);
        if (!e) { remove(CFG_FILE); return; }
        base_def(&d, "probe", GPTPS_EXEC_INPROC); d.run = run_short;
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        for (i = 0; i < n; ++i) {
            char key[128];
            snprintf(key, sizeof key, "tasks.probe.%.63s", keys[i].leaf);
            CHECK(gptps_settings_get(e, key, buf, sizeof buf) == GPTPS_OK);
            if (strcmp(buf, keys[i].value) != 0) {
                printf("FAIL %s: the file said %s, the task has %s (%s)\n", key, keys[i].value, buf,
                       pass == 0 ? "[tasks.probe]" : "[task_defaults]");
                ++fails;
            }
        }
        CHECK(gptps_shutdown(e) == GPTPS_OK);
    }

    /* and a file's stderr_tail_bytes is what a PROGRAM job runs with */
    f = fopen(CFG_FILE, "w");
    CHECK(f != NULL);
    if (!f) return;
    fprintf(f, "[tasks.tailcfg]\nstderr_tail_bytes = 64\nstop_grace_ms = 250\n");
    fclose(f);
    e = open_engine(GPTPS_RUN_THREADED, 1, 1u << 30, CFG_FILE);
    if (e) {
        tally_reset(); track("tailcfg");
        CHECK(gptps_set_event_cb(e, on_ev, NULL) == GPTPS_OK);
        base_def(&d, "tailcfg", GPTPS_EXEC_PROGRAM); d.argv = ERR_ARGV;   /* stderr_tail_bytes 0 */
        d.default_policy.on_failure = GPTPS_ON_FAILURE_DROP;
        CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        CHECK(gptps_settings_get(e, "tasks.tailcfg.stop_grace_ms", buf, sizeof buf) == GPTPS_OK && strcmp(buf, "250") == 0);
        CHECK(gptps_submit(e, "tailcfg", NULL, 0, NULL) == GPTPS_OK);
        CHECK(wait_for(&tally_of("tailcfg")->failed, 1, 10000));
        CHECK(get(&tally_of("tailcfg")->last_err_len) > 0);   /* captured: the file's 64 */
        CHECK(get(&tally_of("tailcfg")->last_err_len) <= 64);
        CHECK(gptps_shutdown(e) == GPTPS_OK);
    }
    remove(CFG_FILE);
}

/* ===================================================================== 6) flags */
static void test_flag_bits(void)
{
    const uint64_t all[3] = { GPTPS_TASK_SERVICE, GPTPS_TASK_RETIRE_ON_OK, GPTPS_TASK_DIE_WITH_HOST };
    size_t i, j;
    gptps *e = open_engine(GPTPS_RUN_THREADED, 1, 1u << 30, NULL);
    gptps_task_def d;
    uint64_t fl = 0;
    for (i = 0; i < 3; ++i) {
        CHECK(all[i] != 0 && (all[i] & (all[i] - 1)) == 0);     /* one bit each */
        for (j = i + 1; j < 3; ++j) CHECK((all[i] & all[j]) == 0);
    }
    if (!e) return;
    base_def(&d, "dies", GPTPS_EXEC_PROGRAM); d.argv = HANG_ARGV; d.flags = GPTPS_TASK_DIE_WITH_HOST;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(gptps_task_flags(e, "dies", &fl) == GPTPS_OK);
    CHECK(fl == GPTPS_TASK_DIE_WITH_HOST);
    CHECK((fl & GPTPS_TASK_RETIRE_ON_OK) == 0);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

/* ===================================================================== 7) clone */
static void test_clone_old_struct(void)
{
    gptps *e = open_engine(GPTPS_RUN_THREADED, 1, 1u << 30, NULL);
    gptps_task_def d;
    char buf[GPTPS_SETTINGS_VALUE_MAX];
    if (!e) return;
    base_def(&d, "old", GPTPS_EXEC_PROGRAM); d.argv = HANG_ARGV;
    d.struct_size = offsetof(gptps_task_def, flags) + sizeof(uint64_t);   /* a pre-2.6 caller */
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(gptps_settings_set(e, "tasks.old.cpu_max_millicpus", "500") == GPTPS_OK);
    CHECK(gptps_settings_set(e, "tasks.old.io_max_write_bps", "1048576") == GPTPS_OK);
    CHECK(gptps_settings_set(e, "tasks.old.stop_grace_ms", "1500") == GPTPS_OK);
    CHECK(gptps_clone_task(e, "old", "old2") == GPTPS_OK);
    CHECK(gptps_settings_get(e, "tasks.old2.cpu_max_millicpus", buf, sizeof buf) == GPTPS_OK && strcmp(buf, "500") == 0);
    CHECK(gptps_settings_get(e, "tasks.old2.io_max_write_bps", buf, sizeof buf) == GPTPS_OK && strcmp(buf, "1048576") == 0);
    CHECK(gptps_settings_get(e, "tasks.old2.stop_grace_ms", buf, sizeof buf) == GPTPS_OK && strcmp(buf, "1500") == 0);
    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

/* ======================================================== 8) + 9) the add-on loader */
static void test_addon_loader(void)
{
    gptps *e = open_engine(GPTPS_RUN_THREADED, 1, 1u << 30, NULL);
    gptps_task_def d;
    gptps_status st;
    if (!e) return;
    log_reset();
    base_def(&d, "hostapi_svc", GPTPS_EXEC_INPROC); d.run = run_service; d.flags = GPTPS_TASK_SERVICE;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    base_def(&d, "hostapi_one", GPTPS_EXEC_INPROC); d.run = run_short;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    CHECK(gptps_load_addon(e, ADDON_HOSTAPI_PATH) == GPTPS_OK);
    CHECK(logged("hostapi: svc 0 0x1 one 0 0x0 measure 7 none 1", NULL) == 1);

    /* a library the system cannot open: its reason is in the line, not just "I/O error" */
    st = gptps_load_addon(e, "gptps-no-such-addon-anywhere.so");
    CHECK(st == GPTPS_E_IO);
    CHECK(logged("add-on gptps-no-such-addon-anywhere.so did not load: the system could not open it: ", NULL) == 1);
    CHECK(logged("gptps-no-such-addon-anywhere.so", "no reason given") == 0);
    /* a descriptor refused by the ABI gate says which check */
    st = gptps_load_addon(e, ADDON_BAD_PATH);
    CHECK(st == GPTPS_E_ABI);
    CHECK(logged("did not load: its descriptor's magic is wrong", NULL) == 1);
    if (fails) log_dump();
    CHECK(gptps_shutdown(e) == GPTPS_OK);
}

int main(void)
{
    gptps_set_log_sink(on_log, NULL);
    test_guard_beside_services();
    test_guard_second_item();
    test_manual_deadline();
    test_program_timeout_ms(GPTPS_RUN_THREADED);
    test_program_timeout_ms(GPTPS_RUN_MANUAL);
#if !defined(_WIN32)
    test_cancel_before_deadline();
#endif
    test_dead_letter_tail();
    test_keys_from_file();
    test_flag_bits();
    test_clone_old_struct();
    test_addon_loader();
    gptps_set_log_sink(NULL, NULL);
    if (fails) { printf("%d engine_fixes check(s) FAILED\n", fails); return 1; }
    printf("engine_fixes: all checks passed\n");
    return 0;
}
