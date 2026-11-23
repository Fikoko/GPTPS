/* SPDX-License-Identifier: MIT */
/* Copyright (c) nightops00dev */
/* Adapted from nightops00dev's night-window harnesses,
 * https://github.com/nightops00dev/gptps-night-window-harness, MIT; see Fikoko/GPTPS#12 */
/*
 * pilot_pass2_retry.c - pilot pass two (measured against v1.10.0, re-verified 1.13.0).
 *
 * Exercises the failure path the night window lives on, all through the
 * gptps_retry add-on and the 1.10 PROGRAM measurements:
 *
 *   item "good"     exits 0                 -> finishes
 *   item "bad"      exits 2 (bad input)     -> dead-lettered AT ONCE, GPTPS_E_DENIED,
 *                                              1 attempt run, retries NOT spent
 *   item "licence"  exits 3 (licence)       -> retried once after >= 300 ms (the
 *                                              "retry much later" rule, scaled from
 *                                              1 h for the bench), then dead-lettered,
 *                                              2 attempts
 *   item "runtime"  exits 1 (runtime/lock)  -> retried per policy (2 retries,
 *                                              3 attempts), then dead-lettered
 *
 * Every PROGRAM job's stderr is captured (stderr_tail_bytes = 512) and the
 * morning report reads the tail off the dead letter: the LAST bytes, carrying
 * TAIL-EVIDENCE, not the first chatter.
 *
 * MANUAL mode. The timing check is a lower bound only (the retry waited at least
 * its 300 ms), which a loaded machine cannot break.
 *
 * Changes from the pilot's harness: the stderr tail is searched within its length
 * (it is not NUL-terminated); the dead letter's own tail must carry the evidence
 * (the harness marked any non-empty tail as evidence); the gap is read off the
 * events' monotonic ts_ms; attempts_run (ABI 2.11) is checked against the attempts
 * that ran; and the helper's path comes from the build (ERP_JOB_PATH).
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L   /* nanosleep */
#endif
#include "gptps.h"

/* Sanitizer builds declare no memory cost. A PROGRAM job's declared memory becomes
 * RLIMIT_AS in the forked child before exec, and a sanitized child still runs its
 * runtime there: one allocation it needs then (TSan's sync allocator, say) fails and
 * it exits 66 ("ThreadSanitizer: out of memory") instead of running the job - now and
 * then, by what the runtime held at the fork (seen once on CI's TSan leg). The cap is
 * production behaviour, not what these tests are about. */
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#  define PILOT_SANITIZED 1
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#    define PILOT_SANITIZED 1
#  endif
#endif
#if defined(PILOT_SANITIZED)
#  define PILOT_JOB_MEM 0u
#else
#  define PILOT_JOB_MEM (64u << 20)
#endif
#include "gptps_retry.h"
#include <stdio.h>
#include <string.h>

#ifndef ERP_JOB_PATH
#  error "ERP_JOB_PATH must name the erp_job helper (CMake sets it)"
#endif

#ifdef _WIN32
#include <windows.h>
static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
#include <time.h>

static void sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u); ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) != 0) { }
}
#endif

/* `needle` within the first `len` bytes of `hay` - a stderr tail is not
 * NUL-terminated, so strstr() on it reads past its end. */
static int mem_has(const char *hay, size_t len, const char *needle)
{
    size_t n = strlen(needle), i;
    if (!hay || n == 0 || len < n) return 0;
    for (i = 0; i + n <= len; ++i)
        if (memcmp(hay + i, needle, n) == 0) return 1;
    return 0;
}

typedef struct {
    const char *name;
    const char *payload;   /* "ms code marker" */
    int  max_retries;
    int  failed_events;
    int  terminal;         /* finished or dead-lettered */
    gptps_status terminal_status;
    uint64_t last_failed_ts;
    uint64_t prev_failed_ts;
    int tail_seen;         /* a FAILED event carried a tail */
    int tail_has_evidence; /* ... with TAIL-EVIDENCE in it */
    int dl_seen;           /* drained as a dead letter */
    int dl_evidence;       /* the dead letter's own tail carries TAIL-EVIDENCE */
    unsigned dl_attempts, dl_attempts_run;
} item;

static item items[] = {
    { "good",    "50 0 good1", 0, 0, 0, GPTPS_OK, 0, 0, 0, 0, 0, 0, 0, 0 },
    { "bad",     "50 2 bad1",  2, 0, 0, GPTPS_OK, 0, 0, 0, 0, 0, 0, 0, 0 },
    { "licence", "50 3 lic1",  1, 0, 0, GPTPS_OK, 0, 0, 0, 0, 0, 0, 0, 0 },
    { "runtime", "50 1 run1",  2, 0, 0, GPTPS_OK, 0, 0, 0, 0, 0, 0, 0, 0 },
};
#define NITEMS (sizeof items / sizeof items[0])

static gptps_handle g_handles[NITEMS];

static item *item_by_handle(gptps_handle h)
{
    size_t i;
    for (i = 0; i < NITEMS; ++i)
        if (g_handles[i] == h) return &items[i];
    return NULL;
}

static void note_tail(const char *tail, size_t len, item *it)
{
    if (!tail || !len) return;
    it->tail_seen = 1;
    if (mem_has(tail, len, "TAIL-EVIDENCE")) it->tail_has_evidence = 1;
}

static void observe(const gptps_event *ev, void *ud)
{
    item *it = item_by_handle(ev->handle);
    (void)ud;
    if (!it) return;
    if (ev->kind == GPTPS_EV_FAILED) {
        ++it->failed_events;
        it->prev_failed_ts = it->last_failed_ts;
        it->last_failed_ts = ev->ts_ms;
        if (ev->struct_size >= sizeof(gptps_event))
            note_tail(ev->stderr_tail, ev->stderr_tail_len, it);
    } else if (ev->kind == GPTPS_EV_FINISHED || ev->kind == GPTPS_EV_DEAD_LETTERED) {
        it->terminal = 1;
        it->terminal_status = ev->status;
        if (ev->struct_size >= sizeof(gptps_event))
            note_tail(ev->stderr_tail, ev->stderr_tail_len, it);
    }
}

static void inspect_dead_letter(const gptps_dead_letter *dl, void *ud)
{
    item *it = item_by_handle(dl->handle);
    int *count = (int *)ud;
    int has;
    ++*count;
    if (!it || dl->struct_size < sizeof(gptps_dead_letter)) return;
    has = mem_has(dl->stderr_tail, dl->stderr_tail_len, "TAIL-EVIDENCE");
    printf("dead letter: %-8s attempts=%u attempts_run=%u status=%d tail=%lu bytes evidence=%s\n",
           it->name, (unsigned)dl->attempts, (unsigned)dl->attempts_run, (int)dl->status,
           (unsigned long)dl->stderr_tail_len, has ? "yes" : "no");
    it->dl_seen = 1;
    it->dl_evidence = has;
    it->dl_attempts = dl->attempts;
    it->dl_attempts_run = dl->attempts_run;
}

#define REQUIRE(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); return 1; \
} } while (0)

int main(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    gptps_task_def d;
    gptps_retry *r = NULL;
    const char *argv[2];
    size_t i;
    int drained = 0, all_terminal = 0, step;

    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.mode = GPTPS_RUN_MANUAL;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;
    cfg.limits.max_memory_bytes = 4096ull << 20;
    REQUIRE(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    REQUIRE(gptps_register_observer(e, observe, NULL) == GPTPS_OK);
    REQUIRE(gptps_retry_install(e, &r) == GPTPS_OK);
    /* the vendor's convention from issue #12, as rules */
    REQUIRE(gptps_retry_on_exit(r, NULL, 2, GPTPS_RETRY_NEVER) == GPTPS_OK);
    REQUIRE(gptps_retry_on_exit(r, NULL, 3, 300) == GPTPS_OK);

    argv[0] = ERP_JOB_PATH; argv[1] = NULL;

    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.name = "erp_job";
    d.exec = GPTPS_EXEC_PROGRAM;
    d.argv = argv;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_cost.mem_bytes = PILOT_JOB_MEM;   /* 64 MB; none when sanitized */
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = 0;
    d.default_policy.on_failure = GPTPS_ON_FAILURE_DEAD_LETTER;
    d.stderr_tail_bytes = 512;
    REQUIRE(gptps_register_task(e, &d) == GPTPS_OK);

    for (i = 0; i < NITEMS; ++i) {
        gptps_submit_options o;
        memset(&o, 0, sizeof o);
        o.struct_size = sizeof o;
        o.flags = GPTPS_SUBMIT_POLICY;
        o.policy.struct_size = sizeof o.policy;
        o.policy.max_retries = (uint32_t)items[i].max_retries;
        o.policy.on_failure = GPTPS_ON_FAILURE_DEAD_LETTER;
        REQUIRE(gptps_submit_ex(e, "erp_job", items[i].payload,
                                strlen(items[i].payload), &o, &g_handles[i]) == GPTPS_OK);
    }

    /* NIGHT: pump until every item is terminal (a retry waits in backoff) */
    for (step = 0; step < 20000 && !all_terminal; ++step) {
        size_t ran = 0;
        REQUIRE(gptps_step(e, &ran) == GPTPS_OK);
        all_terminal = 1;
        for (i = 0; i < NITEMS; ++i) if (!items[i].terminal) all_terminal = 0;
        if (!ran && !all_terminal) sleep_ms(2);
    }
    REQUIRE(all_terminal);
    /* dead letters must be drained BEFORE gptps_shutdown: it frees the engine */
    REQUIRE(gptps_dead_letter_drain(e, inspect_dead_letter, &drained) == 3);
    REQUIRE(gptps_shutdown(e) == GPTPS_OK);
    e = NULL;

    /* MORNING: the report */
    printf("GPTPS pilot pass two - gptps_retry + stderr tails, PROGRAM jobs\n");
    for (i = 0; i < NITEMS; ++i)
        printf("%-8s failed_events=%d terminal=%d status=%d tail=%d evidence=%d\n",
               items[i].name, items[i].failed_events, items[i].terminal,
               (int)items[i].terminal_status, items[i].tail_seen,
               items[i].tail_has_evidence);

    /* findings */
    {
        item *good = &items[0], *bad = &items[1], *lic = &items[2], *run = &items[3];
        unsigned long long lic_gap = lic->failed_events == 2 && lic->last_failed_ts >= lic->prev_failed_ts
                                   ? (unsigned long long)(lic->last_failed_ts - lic->prev_failed_ts) : 0ull;
        int t;
        printf("licence attempt gap: %llu ms (rule: >= 300)\n", lic_gap);

        t = bad->terminal && bad->terminal_status == GPTPS_E_DENIED && bad->failed_events == 1;
        printf("exit 2 dead-lettered at once, E_DENIED, 1 attempt: %s\n", t ? "HOLDS" : "FAILS");
        REQUIRE(t);

        t = lic->failed_events == 2 && lic_gap >= 300;
        printf("exit 3 waited its extra %d ms before the retry: %s\n", 300, t ? "HOLDS" : "FAILS");
        REQUIRE(t);

        t = run->failed_events == 3;
        printf("exit 1 retried per policy (3 attempts total): %s\n", t ? "HOLDS" : "FAILS");
        REQUIRE(t);

        t = bad->tail_has_evidence && lic->tail_has_evidence && run->tail_has_evidence &&
            bad->dl_evidence && lic->dl_evidence && run->dl_evidence;
        printf("every failure and every dead letter carries its stderr TAIL-EVIDENCE: %s\n",
               t ? "HOLDS" : "FAILS");
        REQUIRE(t);

        /* ABI 2.11: attempts_run = the FAILED events; the refused retry of "bad"
         * is counted in attempts but never ran */
        t = bad->dl_seen && bad->dl_attempts == 2 && bad->dl_attempts_run == 1 &&
            lic->dl_seen && lic->dl_attempts == 2 && lic->dl_attempts_run == 2 &&
            run->dl_seen && run->dl_attempts == 3 && run->dl_attempts_run == 3;
        printf("dead letters count the attempts that ran (bad 2/1, licence 2/2, runtime 3/3): %s\n",
               t ? "HOLDS" : "FAILS");
        REQUIRE(t);

        t = good->terminal && good->terminal_status == GPTPS_OK && good->failed_events == 0 &&
            !good->dl_seen;
        printf("the good job finished untouched: %s\n", t ? "HOLDS" : "FAILS");
        REQUIRE(t);
    }

    gptps_retry_close(r);      /* AFTER gptps_shutdown, as the add-on requires */
    printf("drained dead letters: %d (expect 3)\n", drained);
    printf("PASS TWO COMPLETE\n");
    return 0;
}
