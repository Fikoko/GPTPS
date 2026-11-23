/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/* In the style of nightops00dev's night-window harnesses,
 * https://github.com/nightops00dev/gptps-night-window-harness, MIT; see Fikoko/GPTPS#12 */
/*
 * pilot_pass4_shutdown.c - pilot pass four (1.14, ABI 2.11): the night ends in a
 * shutdown with work still queued, and the morning re-drives what it cut off.
 *
 * NIGHT. A MANUAL engine with one slot gets five PROGRAM jobs, in this order:
 *
 *   0  gl_close   erp_redrive_job  the licence is missing: exits 3, dead-lettered
 *   1  rpt_fin    erp_job          exits 0, finishes
 *   2  rpt_sales  erp_job          \
 *   3  mrp        erp_redrive_job   > still queued when the window closes
 *   4  rpt_inv    erp_job          /
 *
 * Only two attempts are stepped; then the window closes: gptps_shutdown_ex with a
 * dead_letter callback that copies each dead letter (task, payload, submit options,
 * status, attempts, attempts_run, stderr tail). It must hand over, before the
 * engine is freed:
 *   - the night's own failure (never drained) first: status GPTPS_E_TASK,
 *     attempts 1, attempts_run 1, its stderr tail saying the licence is missing;
 *   - every job the shutdown cut off: status GPTPS_E_SHUTDOWN, attempts_run 0, no
 *     stderr tail, payload and submit options intact - each once, and their
 *     DEAD_LETTERED events flagged GPTPS_EV_FLAG_SHUTDOWN;
 *   - and nothing for the job that finished.
 *
 * MORNING. The operator renews the licence (a marker file in this test's own
 * directory, PILOT_WORK_DIR, which is the licence jobs' work_dir), opens a new
 * engine, and re-submits every copied dead letter with gptps_submit_ex(...,
 * &copy.options, ...). Every one must FINISH GPTPS_OK, and the morning's shutdown
 * hands over nothing.
 *
 * Portable (POSIX and Windows). The waits are bounded (30 s) and there is no timing
 * assertion.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L   /* nanosleep, mkdir */
#endif
#include "gptps.h"
#include <stdio.h>
#include <string.h>

#if !defined(ERP_JOB_PATH) || !defined(ERP_REDRIVE_JOB_PATH) || !defined(PILOT_WORK_DIR)
#  error "ERP_JOB_PATH, ERP_REDRIVE_JOB_PATH and PILOT_WORK_DIR come from CMake"
#endif
#define MARKER PILOT_WORK_DIR "/licence.renewed"

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
static void sleep_ms(unsigned ms) { Sleep(ms); }
static void make_dir(const char *p) { (void)_mkdir(p); }
#else
#include <sys/stat.h>
#include <time.h>
static void sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u); ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) != 0) { }
}
static void make_dir(const char *p) { (void)mkdir(p, 0700); }
#endif

/* `needle` within the first `len` bytes of `hay` (a stderr tail is not NUL-terminated) */
static int mem_has(const char *hay, size_t len, const char *needle)
{
    size_t n = strlen(needle), i;
    if (!hay || n == 0 || len < n) return 0;
    for (i = 0; i + n <= len; ++i)
        if (memcmp(hay + i, needle, n) == 0) return 1;
    return 0;
}

/* ---- the night's jobs --------------------------------------------------- */

typedef struct {
    const char *name;        /* for the report */
    const char *task;
    const char *payload;
    unsigned    flags;       /* GPTPS_SUBMIT_* it is submitted with */
    gptps_handle night_h, morning_h;
    /* night events */
    int night_finished, night_failed, night_dead, night_dead_shutdown_flag;
    gptps_status night_dead_status;
    /* morning events */
    int morning_finished, morning_failed;
    gptps_status morning_status;
} job;

#define NJOBS 5
static job jobs[NJOBS] = {
    { "gl_close",  "erp_licence", "gl-close",     GPTPS_SUBMIT_POLICY,
      0, 0, 0, 0, 0, 0, GPTPS_OK, 0, 0, GPTPS_OK },
    { "rpt_fin",   "erp_job",     "10 0 rpt_fin", GPTPS_SUBMIT_TIMEOUT_MS,
      0, 0, 0, 0, 0, 0, GPTPS_OK, 0, 0, GPTPS_OK },
    { "rpt_sales", "erp_job",     "10 0 rpt_sales", GPTPS_SUBMIT_TIMEOUT_MS | GPTPS_SUBMIT_DEADLINE,
      0, 0, 0, 0, 0, 0, GPTPS_OK, 0, 0, GPTPS_OK },
    { "mrp",       "erp_licence", "mrp",          GPTPS_SUBMIT_POLICY | GPTPS_SUBMIT_TIMEOUT_MS,
      0, 0, 0, 0, 0, 0, GPTPS_OK, 0, 0, GPTPS_OK },
    { "rpt_inv",   "erp_job",     "10 0 rpt_inv", 0,
      0, 0, 0, 0, 0, 0, GPTPS_OK, 0, 0, GPTPS_OK },
};
#define TIMEOUT_MS 30000u
#define DUE_MS     3600000u

static int g_morning;   /* which engine's events the observer is seeing */

static job *job_by_handle(gptps_handle h)
{
    int i;
    for (i = 0; i < NJOBS; ++i)
        if ((g_morning ? jobs[i].morning_h : jobs[i].night_h) == h) return &jobs[i];
    return NULL;
}

static void observe(const gptps_event *ev, void *ud)
{
    job *j = job_by_handle(ev->handle);
    (void)ud;
    if (!j) return;
    if (!g_morning) {
        if (ev->kind == GPTPS_EV_FINISHED) j->night_finished = 1;
        else if (ev->kind == GPTPS_EV_FAILED) ++j->night_failed;
        else if (ev->kind == GPTPS_EV_DEAD_LETTERED) {
            ++j->night_dead;
            j->night_dead_status = ev->status;
            if (ev->flags & GPTPS_EV_FLAG_SHUTDOWN) j->night_dead_shutdown_flag = 1;
        }
    } else {
        if (ev->kind == GPTPS_EV_FINISHED) { j->morning_finished = 1; j->morning_status = ev->status; }
        else if (ev->kind == GPTPS_EV_FAILED) { ++j->morning_failed; j->morning_status = ev->status; }
    }
}

/* ---- what the shutdown callback keeps ------------------------------------ */

typedef struct {
    gptps_handle h;
    char         task[32];
    char         payload[64];
    size_t       payload_len;
    gptps_submit_options options;
    gptps_status status;
    uint32_t     attempts, attempts_run;
    char         tail[512];
    size_t       tail_len;
} kept_dl;

static kept_dl kept[16];
static int     nkept, nseen;
static int     too_old;     /* a dead letter shorter than the 2.11 struct */

static void keep_dead_letter(const gptps_dead_letter *dl, void *ud)
{
    kept_dl *k;
    (void)ud;
    ++nseen;
    if (dl->struct_size < sizeof(gptps_dead_letter)) { too_old = 1; return; }
    if (nkept >= (int)(sizeof kept / sizeof kept[0])) return;
    k = &kept[nkept++];
    memset(k, 0, sizeof *k);
    k->h = dl->handle;
    snprintf(k->task, sizeof k->task, "%s", dl->task_name ? dl->task_name : "?");
    k->payload_len = dl->payload_len < sizeof k->payload ? dl->payload_len : sizeof k->payload;
    if (k->payload_len && dl->payload) memcpy(k->payload, dl->payload, k->payload_len);
    k->options = dl->options;
    k->status = dl->status;
    k->attempts = dl->attempts;
    k->attempts_run = dl->attempts_run;
    k->tail_len = dl->stderr_tail_len < sizeof k->tail ? dl->stderr_tail_len : sizeof k->tail;
    if (k->tail_len && dl->stderr_tail) memcpy(k->tail, dl->stderr_tail, k->tail_len);
}

static const kept_dl *kept_for(const job *j)
{
    int i;
    for (i = 0; i < nkept; ++i) if (kept[i].h == j->night_h) return &kept[i];
    return NULL;
}
static int kept_count_for(const job *j)
{
    int i, n = 0;
    for (i = 0; i < nkept; ++i) if (kept[i].h == j->night_h) ++n;
    return n;
}

/* ---- the engine --------------------------------------------------------- */

static const char *argv_job[2]     = { ERP_JOB_PATH, NULL };
static const char *argv_licence[2] = { ERP_REDRIVE_JOB_PATH, NULL };

static gptps *open_engine(void)
{
    gptps_config cfg;
    gptps_task_def d;
    gptps *e = NULL;

    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.mode = GPTPS_RUN_MANUAL;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;
    cfg.limits.max_memory_bytes = 4096ull << 20;
    if (gptps_open_ex(&cfg, &e) != GPTPS_OK) return NULL;
    if (gptps_register_observer(e, observe, NULL) != GPTPS_OK) goto fail;

    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.exec = GPTPS_EXEC_PROGRAM;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_cost.mem_bytes = 64u << 20;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = 0;
    d.default_policy.on_failure = GPTPS_ON_FAILURE_DEAD_LETTER;
    d.stderr_tail_bytes = 512;

    d.name = "erp_job";
    d.argv = argv_job;
    if (gptps_register_task(e, &d) != GPTPS_OK) goto fail;

    d.name = "erp_licence";
    d.argv = argv_licence;
    d.work_dir = PILOT_WORK_DIR;            /* where the licence marker is looked for */
    if (gptps_register_task(e, &d) != GPTPS_OK) goto fail;
    return e;
fail:
    (void)gptps_shutdown(e);
    return NULL;
}

#define REQUIRE(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); remove(MARKER); return 1; \
} } while (0)

int main(void)
{
    gptps *e;
    gptps_shutdown_options so;
    int i, ran_total = 0, t, all;
    unsigned long long t0;

    make_dir(PILOT_WORK_DIR);          /* CMake made it; a no-op then */
    remove(MARKER);                    /* the night: no licence */

    /* ================= NIGHT ================= */
    e = open_engine();
    REQUIRE(e != NULL);
    for (i = 0; i < NJOBS; ++i) {
        gptps_submit_options o;
        memset(&o, 0, sizeof o);
        o.struct_size = sizeof o;
        o.flags = jobs[i].flags;
        o.policy.struct_size = sizeof o.policy;
        o.policy.max_retries = 0;
        o.policy.on_failure = GPTPS_ON_FAILURE_DEAD_LETTER;
        o.timeout_ms = TIMEOUT_MS;
        o.deadline_in_ms = DUE_MS;
        REQUIRE(gptps_submit_ex(e, jobs[i].task, jobs[i].payload, strlen(jobs[i].payload),
                                &o, &jobs[i].night_h) == GPTPS_OK);
    }
    /* step exactly two attempts: gl_close (fails, exit 3) and rpt_fin (finishes) */
    t0 = (unsigned long long)gptps_now_ms(NULL);
    while (ran_total < 2 && (unsigned long long)gptps_now_ms(NULL) - t0 < 30000u) {
        size_t ran = 0;
        REQUIRE(gptps_step(e, &ran) == GPTPS_OK);
        ran_total += (int)ran;
        if (!ran) sleep_ms(2);
    }
    REQUIRE(ran_total == 2);

    /* the window closes with three jobs still queued */
    memset(&so, 0, sizeof so);
    so.struct_size = sizeof so;
    so.dead_letter = keep_dead_letter;
    so.dead_letter_user_data = NULL;
    REQUIRE(gptps_shutdown_ex(e, &so) == GPTPS_OK);
    e = NULL;

    printf("GPTPS pilot pass four - the night ends in gptps_shutdown_ex, the morning re-drives\n");
    for (i = 0; i < NJOBS; ++i)
        printf("night: %-10s finished=%d failed=%d dead_lettered=%d status=%d shutdown_flag=%d\n",
               jobs[i].name, jobs[i].night_finished, jobs[i].night_failed, jobs[i].night_dead,
               (int)jobs[i].night_dead_status, jobs[i].night_dead_shutdown_flag);
    for (i = 0; i < nkept; ++i)
        printf("dead letter %d: task=%-11s payload=%.*s status=%d attempts=%u attempts_run=%u "
               "flags=0x%x tail=%lu bytes\n",
               i, kept[i].task, (int)kept[i].payload_len, kept[i].payload, (int)kept[i].status,
               (unsigned)kept[i].attempts, (unsigned)kept[i].attempts_run,
               kept[i].options.flags, (unsigned long)kept[i].tail_len);

    t = !too_old && nseen == 4 && nkept == 4;
    printf("shutdown handed over 4 dead letters (1 night failure + 3 cut off): %s\n",
           t ? "HOLDS" : "FAILS");
    REQUIRE(t);

    {   /* the night's own failure, never drained, comes first */
        const job *j = &jobs[0];
        const kept_dl *k = kept_for(j);
        t = k == &kept[0] && kept_count_for(j) == 1 && k->status == GPTPS_E_TASK &&
            k->attempts == 1 && k->attempts_run == 1 && j->night_failed == 1 &&
            j->night_dead == 1 && !j->night_dead_shutdown_flag &&
            strcmp(k->task, "erp_licence") == 0 &&
            mem_has(k->tail, k->tail_len, "TAIL-EVIDENCE exit=3 marker=licence-missing job=gl-close") &&
            k->payload_len == strlen(j->payload) && memcmp(k->payload, j->payload, k->payload_len) == 0 &&
            k->options.flags == j->flags && k->options.policy.max_retries == 0;
        printf("the night's licence failure arrived first: E_TASK, attempts_run 1, stderr tail: %s\n",
               t ? "HOLDS" : "FAILS");
        REQUIRE(t);
    }

    t = jobs[1].night_finished && !jobs[1].night_dead && kept_for(&jobs[1]) == NULL;
    printf("the job that finished is not among them: %s\n", t ? "HOLDS" : "FAILS");
    REQUIRE(t);

    t = 1;
    for (i = 2; i < NJOBS; ++i) {
        const job *j = &jobs[i];
        const kept_dl *k = kept_for(j);
        if (!k || kept_count_for(j) != 1 || k->status != GPTPS_E_SHUTDOWN ||
            k->attempts_run != 0 || k->tail_len != 0 ||
            strcmp(k->task, j->task) != 0 ||
            k->payload_len != strlen(j->payload) || memcmp(k->payload, j->payload, k->payload_len) != 0 ||
            k->options.flags != j->flags ||
            ((j->flags & GPTPS_SUBMIT_TIMEOUT_MS) && k->options.timeout_ms != TIMEOUT_MS) ||
            ((j->flags & GPTPS_SUBMIT_DEADLINE) &&
             (k->options.deadline_in_ms == 0 || k->options.deadline_in_ms > DUE_MS)) ||
            j->night_failed != 0 || j->night_dead != 1 ||
            j->night_dead_status != GPTPS_E_SHUTDOWN || !j->night_dead_shutdown_flag) {
            printf("  cut-off job %s: wrong dead letter or events\n", j->name);
            t = 0;
        }
    }
    printf("every cut-off job: E_SHUTDOWN, attempts_run 0, payload + options intact, once: %s\n",
           t ? "HOLDS" : "FAILS");
    REQUIRE(t);

    /* ================= MORNING ================= */
    {
        FILE *f = fopen(MARKER, "wb");
        REQUIRE(f != NULL);
        fputs("renewed by the morning operator\n", f);
        fclose(f);
    }
    g_morning = 1;
    e = open_engine();
    REQUIRE(e != NULL);
    for (i = 0; i < nkept; ++i) {
        int k;
        job *j = NULL;
        for (k = 0; k < NJOBS; ++k) if (jobs[k].night_h == kept[i].h) j = &jobs[k];
        REQUIRE(j != NULL);
        REQUIRE(gptps_submit_ex(e, kept[i].task, kept[i].payload, kept[i].payload_len,
                                &kept[i].options, &j->morning_h) == GPTPS_OK);
    }
    t0 = (unsigned long long)gptps_now_ms(NULL);
    all = 0;
    while (!all && (unsigned long long)gptps_now_ms(NULL) - t0 < 30000u) {
        size_t ran = 0;
        REQUIRE(gptps_step(e, &ran) == GPTPS_OK);
        all = 1;
        for (i = 0; i < NJOBS; ++i)
            if (i != 1 && !jobs[i].morning_finished && !jobs[i].morning_failed) all = 0;
        if (!all && !ran) sleep_ms(2);
    }
    REQUIRE(all);
    nseen = nkept = 0;
    REQUIRE(gptps_shutdown_ex(e, &so) == GPTPS_OK);
    e = NULL;
    remove(MARKER);

    for (i = 0; i < NJOBS; ++i)
        if (i != 1)
            printf("morning: %-10s finished=%d failed=%d status=%d\n", jobs[i].name,
                   jobs[i].morning_finished, jobs[i].morning_failed, (int)jobs[i].morning_status);
    t = 1;
    for (i = 0; i < NJOBS; ++i)
        if (i != 1 && (!jobs[i].morning_finished || jobs[i].morning_failed ||
                       jobs[i].morning_status != GPTPS_OK)) t = 0;
    t = t && nseen == 0;
    printf("after the licence renewal every re-driven dead letter finished GPTPS_OK: %s\n",
           t ? "HOLDS" : "FAILS");
    REQUIRE(t);

    printf("PASS FOUR COMPLETE\n");
    return 0;
}
