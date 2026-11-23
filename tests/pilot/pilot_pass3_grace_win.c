/* SPDX-License-Identifier: MIT */
/* Copyright (c) nightops00dev */
/* Adapted from nightops00dev's night-window harnesses,
 * https://github.com/nightops00dev/gptps-night-window-harness, MIT; see Fikoko/GPTPS#12 */
/*
 * pilot_pass3_grace_win.c - pilot pass three (measured against v1.13.0). WINDOWS
 * ONLY: the stop is a Ctrl+Break, and the cancel comes from a CreateThread thread.
 *
 * Two scenarios that complete the 1.10 story the night window cares about:
 *
 *   A. stop_grace_ms rollback: a long-running PROGRAM job is cancelled
 *      mid-run; the child gets Ctrl+Break, rolls back (ROLLBACK-EVIDENCE in
 *      the stderr tail), and the engine reports GPTPS_E_CANCELLED. We measure
 *      the wall time from gptps_cancel to the terminal event and check it
 *      stays inside the grace window (2000 ms).
 *
 *   B. dead-letter re-drive: a licence job exits 3 and dead-letters at once
 *      (0 retries). Morning: the operator "renews the licence" (drops a
 *      marker file), the dead letter is drained and re-driven unchanged, and
 *      the second run finishes GPTPS_OK. Night-to-morning cycle, closed.
 *
 * MANUAL mode, concurrency 1. Dead letters drained BEFORE gptps_shutdown.
 *
 * Changes from the pilot's harness: the stderr tail is searched within its length
 * (it is not NUL-terminated); the helpers' paths come from the build
 * (ERP_STOP_JOB_PATH, ERP_REDRIVE_JOB_PATH) and the licence marker lives in a
 * directory of this test's own (PILOT_WORK_DIR, the redrive type's work_dir), not
 * in whatever directory the test was started from; every wait is bounded (30 s),
 * so a lost event fails the test instead of hanging it; the re-drive submits the
 * dead letter's own payload and options.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>
#include <direct.h>     /* _mkdir */
#include <windows.h>

#if !defined(ERP_STOP_JOB_PATH) || !defined(ERP_REDRIVE_JOB_PATH) || !defined(PILOT_WORK_DIR)
#  error "ERP_STOP_JOB_PATH, ERP_REDRIVE_JOB_PATH and PILOT_WORK_DIR come from CMake"
#endif
#define MARKER PILOT_WORK_DIR "/licence.renewed"

static unsigned long long wall_ms(void) { return (unsigned long long)gptps_now_ms(NULL); }

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

/* ---- shared observation state ------------------------------------------ */

typedef struct {
    const char *name;
    int started, finished, failed, dead;
    gptps_status terminal_status;
    unsigned long long terminal_wall;
    int tail_has_rollback;
} obs;

static obs g_long = { "long", 0, 0, 0, 0, GPTPS_OK, 0, 0 };
static obs g_redrive = { "redrive", 0, 0, 0, 0, GPTPS_OK, 0, 0 };

static gptps_handle h_long, h_redrive;

/* MANUAL runs a task to completion on the pumping thread, so the cancel has
 * to come from a second host thread (gptps_cancel is safe from any thread). */
static volatile unsigned long long t_cancel;
static volatile gptps_status cancel_st = GPTPS_E_INVAL;
static DWORD WINAPI cancel_after_delay(void *arg)
{
    gptps *e = (gptps *)arg;
    Sleep(600);                          /* let the job work a few ticks */
    t_cancel = wall_ms();
    cancel_st = gptps_cancel(e, h_long);
    return 0;
}

static obs *obs_by_handle(gptps_handle h)
{
    if (h == h_long) return &g_long;
    if (h == h_redrive) return &g_redrive;
    return NULL;
}

static void observe(const gptps_event *ev, void *ud)
{
    obs *o = obs_by_handle(ev->handle);
    (void)ud;
    if (!o) return;
    switch (ev->kind) {
    case GPTPS_EV_STARTED: o->started = 1; break;
    case GPTPS_EV_FINISHED:
        o->finished = 1; o->terminal_status = ev->status;
        o->terminal_wall = wall_ms(); break;
    case GPTPS_EV_FAILED:
        ++o->failed; o->terminal_status = ev->status;
        o->terminal_wall = wall_ms();
        if (ev->struct_size >= sizeof(gptps_event) &&
            mem_has(ev->stderr_tail, ev->stderr_tail_len, "ROLLBACK-EVIDENCE"))
            o->tail_has_rollback = 1;
        break;
    case GPTPS_EV_DEAD_LETTERED: o->dead = 1; break;
    default: break;
    }
}

/* the morning drain keeps what a re-drive needs */
static int drained = 0;
static char dl_payload[64];
static size_t dl_payload_len;
static gptps_submit_options dl_opts;
static int dl_licence_evidence;
static void count_dl(const gptps_dead_letter *dl, void *ud)
{
    (void)ud;
    ++drained;
    printf("dead letter: task=%s attempts=%u status=%d tail=%lu bytes\n",
           dl->task_name, (unsigned)dl->attempts, (int)dl->status,
           (unsigned long)dl->stderr_tail_len);
    if (dl->handle == h_redrive && dl->struct_size >= sizeof(gptps_dead_letter)) {
        dl_payload_len = dl->payload_len < sizeof dl_payload ? dl->payload_len : sizeof dl_payload;
        if (dl_payload_len) memcpy(dl_payload, dl->payload, dl_payload_len);
        dl_opts = dl->options;
        dl_licence_evidence = mem_has(dl->stderr_tail, dl->stderr_tail_len, "marker=licence-missing");
    }
}

/* ---- helpers ------------------------------------------------------------ */

#define REQUIRE(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); remove(MARKER); return 1; \
} } while (0)

/* pump until *flag is set or 30 s pass; 1 when it was set */
static int pump_until(gptps *e, const int *flag)
{
    unsigned long long t0 = wall_ms();
    while (!*flag) {
        size_t ran = 0;
        if (gptps_step(e, &ran) != GPTPS_OK) return 0;
        if (*flag) break;
        if (wall_ms() - t0 > 30000) return 0;
        if (!ran) Sleep(2);
    }
    return 1;
}

static gptps_status submit_prog(gptps *e, const char *task, const void *payload, size_t len,
                                const gptps_submit_options *opts, gptps_handle *out)
{
    gptps_submit_options o;
    if (opts) return gptps_submit_ex(e, task, payload, len, opts, out);
    memset(&o, 0, sizeof o);
    o.struct_size = sizeof o;
    o.flags = GPTPS_SUBMIT_POLICY;
    o.policy.struct_size = sizeof o.policy;
    o.policy.max_retries = 0;
    o.policy.on_failure = GPTPS_ON_FAILURE_DEAD_LETTER;
    return gptps_submit_ex(e, task, payload, len, &o, out);
}

int main(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    gptps_task_def d;
    const char *argv_long[2], *argv_redrive[2];
    unsigned long long stop_elapsed;
    FILE *f;
    int t, done;

    (void)_mkdir(PILOT_WORK_DIR);        /* CMake made it; a no-op then */
    remove(MARKER);

    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.mode = GPTPS_RUN_MANUAL;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;
    cfg.limits.max_memory_bytes = 4096ull << 20;
    REQUIRE(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    REQUIRE(gptps_register_observer(e, observe, NULL) == GPTPS_OK);

    argv_long[0] = ERP_STOP_JOB_PATH; argv_long[1] = NULL;
    argv_redrive[0] = ERP_REDRIVE_JOB_PATH; argv_redrive[1] = NULL;

    /* scenario B first (fast), then A */
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.name = "erp_redrive";
    d.exec = GPTPS_EXEC_PROGRAM;
    d.argv = argv_redrive;
    d.work_dir = PILOT_WORK_DIR;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_cost.mem_bytes = 64u << 20;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = 0;
    d.default_policy.on_failure = GPTPS_ON_FAILURE_DEAD_LETTER;
    d.stderr_tail_bytes = 512;
    REQUIRE(gptps_register_task(e, &d) == GPTPS_OK);

    d.name = "erp_long";
    d.argv = argv_long;
    d.work_dir = NULL;
    d.stop_grace_ms = 2000;
    REQUIRE(gptps_register_task(e, &d) == GPTPS_OK);

    /* --- B: night - licence missing, dead-lettered at once --- */
    REQUIRE(submit_prog(e, "erp_redrive", "nightly-gl", 10, NULL, &h_redrive) == GPTPS_OK);
    REQUIRE(pump_until(e, &g_redrive.dead));
    printf("B night: redrive failed=%d dead=%d status=%d\n",
           g_redrive.failed, g_redrive.dead, (int)g_redrive.terminal_status);
    REQUIRE(g_redrive.failed == 1 && g_redrive.dead);

    /* --- B: morning - licence renewed, drain + re-drive unchanged --- */
    f = fopen(MARKER, "wb");
    REQUIRE(f != NULL);
    fputs("renewed by the morning operator\n", f);
    fclose(f);
    REQUIRE(gptps_dead_letter_drain(e, count_dl, NULL) == 1);
    REQUIRE(dl_licence_evidence);

    g_redrive.finished = 0; g_redrive.terminal_status = GPTPS_OK;
    REQUIRE(submit_prog(e, "erp_redrive", dl_payload, dl_payload_len, &dl_opts, &h_redrive) == GPTPS_OK);
    REQUIRE(pump_until(e, &g_redrive.finished));
    printf("B morning: re-drive finished=%d status=%d\n",
           g_redrive.finished, (int)g_redrive.terminal_status);
    t = g_redrive.finished && g_redrive.terminal_status == GPTPS_OK;
    printf("dead-letter re-drive after licence renewal: %s\n", t ? "HOLDS" : "FAILS");
    REQUIRE(t);
    remove(MARKER);

    /* --- A: cancel a long job mid-run, Ctrl+Break rollback --- */
    {
        HANDLE th;
        REQUIRE(submit_prog(e, "erp_long", NULL, 0, NULL, &h_long) == GPTPS_OK);
        th = CreateThread(NULL, 0, cancel_after_delay, e, 0, NULL);
        REQUIRE(th != NULL);
        /* this pump blocks inside gptps_step while the child runs; the
         * cancel thread interrupts it with Ctrl+Break after 600 ms */
        {
            unsigned long long t0 = wall_ms();
            done = 0;
            while (!done && wall_ms() - t0 < 30000) {
                size_t ran = 0;
                if (gptps_step(e, &ran) != GPTPS_OK) break;
                done = g_long.failed || g_long.finished;
                if (!done && !ran) Sleep(2);
            }
        }
        WaitForSingleObject(th, INFINITE);
        CloseHandle(th);
        REQUIRE(done);
        REQUIRE(cancel_st == GPTPS_OK);
    }
    stop_elapsed = g_long.terminal_wall >= t_cancel ? g_long.terminal_wall - t_cancel : 0;
    printf("A: cancel -> terminal in %llu ms (grace 2000), status=%d (CANCELLED=%d)\n",
           stop_elapsed, (int)g_long.terminal_status, (int)GPTPS_E_CANCELLED);

    t = g_long.terminal_status == GPTPS_E_CANCELLED;
    printf("cancelled job reported GPTPS_E_CANCELLED: %s\n", t ? "HOLDS" : "FAILS");
    REQUIRE(t);

    t = g_long.tail_has_rollback;
    printf("stderr tail carries ROLLBACK-EVIDENCE: %s\n", t ? "HOLDS" : "FAILS");
    REQUIRE(t);

    t = stop_elapsed >= 50 && stop_elapsed <= 2500;
    printf("stop took %llu ms - rollback happened, inside grace: %s\n",
           stop_elapsed, t ? "HOLDS" : "FAILS");
    REQUIRE(t);

    /* drain (before shutdown!) and close: a cancelled item does not dead-letter */
    REQUIRE(gptps_dead_letter_drain(e, count_dl, NULL) == 0);
    printf("total drained dead letters: %d\n", drained);
    REQUIRE(gptps_shutdown(e) == GPTPS_OK);
    printf("PASS THREE COMPLETE\n");
    return 0;
}
