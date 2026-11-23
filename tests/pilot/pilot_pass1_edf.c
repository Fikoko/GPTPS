/* SPDX-License-Identifier: MIT */
/* Copyright (c) nightops00dev */
/* Adapted from nightops00dev's night-window harnesses,
 * https://github.com/nightops00dev/gptps-night-window-harness, MIT; see Fikoko/GPTPS#12 */
/*
 * pilot_pass1_edf.c - the night-batch pilot, pass one (measured against v1.9.0).
 *
 * The issue #12 scenario at bench scale: the MRP consolidation job is big and
 * slow, the three morning reports are small and due hours earlier. Under a
 * first-come ordering the MRP job submitted first would hold the single
 * executor slot for its whole run and every report would wait behind it.
 * This run installs gptps_sched_edf (earliest deadline first) and
 * gptps_sched_shed_late (refuse work already past its due time) and checks:
 *
 *   1. start order vs deadline order (EDF must invert the FIFO order),
 *   2. the shed-late constraint refusing a job whose deadline already passed
 *      (one dead letter, GPTPS_E_DENIED, never started),
 *   3. clean completion of every on-time job, in deadline order.
 *
 * MANUAL mode, single concurrency slot, so admission order is observable.
 *
 * Changes from the pilot's harness: the simulated work and the wait for the stale
 * job's deadline spin on the engine's own monotonic clock (gptps_now_ms) instead of
 * clock(). That is the clock the deadline is kept on (clock() is process CPU time on
 * POSIX and wall time on Windows), and it is a HAL call, so the spin also runs on
 * tests/hal_sim.c, whose virtual clock it advances, instead of holding its baton.
 * The ledger now checks what it prints: every on-time job FINISHED, the refusal and
 * the dead letter are counted, and the dead letter is drained before shutdown.
 */
#include "gptps.h"
#include "gptps_sched.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    uint64_t    deadline_ms;   /* from its submit until due */
    uint64_t    run_ms;        /* simulated work */
    uint64_t    declared_mem;
    int         start_seq;     /* fill at task start */
    int         finished;      /* GPTPS_EV_FINISHED, GPTPS_OK */
    int         dead;          /* GPTPS_EV_DEAD_LETTERED */
    gptps_status dead_status;
    gptps_handle handle;
} job;

static job jobs[] = {
    /* submitted in THIS order: MRP first, so FIFO would run it first */
    { "mrp",       3600000u, 600, 512u << 20, -1, 0, 0, GPTPS_OK, 0 },
    { "rpt_fin",    600000u, 100,  64u << 20, -1, 0, 0, GPTPS_OK, 0 },
    { "rpt_sales", 1200000u, 100,  64u << 20, -1, 0, 0, GPTPS_OK, 0 },
    { "rpt_inv",   1800000u, 100,  64u << 20, -1, 0, 0, GPTPS_OK, 0 },
    { "rpt_stale",       1u, 100,  64u << 20, -1, 0, 0, GPTPS_OK, 0 }, /* due 1 ms after submit */
};
#define NJOBS (sizeof jobs / sizeof jobs[0])
#define STALE 4u           /* index of rpt_stale */

static int      g_seq;
static unsigned g_dead_letters;   /* DEAD_LETTERED events */

/* Busy work for `ms` on the engine's monotonic clock. Every iteration is a HAL
 * clock read, so a simulation HAL sees it (and moves its virtual clock). */
static void spin_ms(uint64_t ms)
{
    uint64_t t0 = gptps_now_ms(NULL);
    while (gptps_now_ms(NULL) - t0 < ms) { }
}

static gptps_status run_job(gptps_ctx *ctx, void *ud)
{
    size_t n, i;
    const char *p = (const char *)gptps_payload(ctx, &n);
    job *j = (job *)ud;
    unsigned long sum = 0;
    j->start_seq = g_seq++;
    for (i = 0; i < n; ++i) sum += (unsigned char)p[i];
    spin_ms(j->run_ms);
    return gptps_result_set(ctx, &sum, sizeof sum);
}

static job *job_by_handle(gptps_handle h)
{
    size_t i;
    for (i = 0; i < NJOBS; ++i) if (jobs[i].handle == h) return &jobs[i];
    return NULL;
}

static void observe(const gptps_event *ev, void *ud)
{
    job *j = job_by_handle(ev->handle);
    (void)ud;
    if (ev->kind == GPTPS_EV_DEAD_LETTERED) {
        ++g_dead_letters;
        if (j) { j->dead = 1; j->dead_status = ev->status; }
    } else if (ev->kind == GPTPS_EV_FINISHED && j && ev->status == GPTPS_OK) {
        j->finished = 1;
    }
}

static int g_drained_stale;
static void inspect_dead_letter(const gptps_dead_letter *dl, void *ud)
{
    (void)ud;
    if (dl->handle == jobs[STALE].handle && dl->status == GPTPS_E_DENIED) ++g_drained_stale;
}

static int pump(gptps *e)
{
    int step;
    size_t ran = 1;
    for (step = 0; step < 1000 && ran; ++step)
        if (gptps_step(e, &ran) != GPTPS_OK) return 0;
    return ran == 0;
}

#define REQUIRE(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); return 1; \
} } while (0)

int main(void)
{
    gptps *e = NULL;
    gptps_config cfg;
    gptps_task_def d;
    gptps_sched *edf = NULL, *shed = NULL;
    size_t i, drained;
    uint64_t shed_count;
    int t;

    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.mode = GPTPS_RUN_MANUAL;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;
    cfg.limits.max_memory_bytes = 4096ull << 20;
    REQUIRE(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    REQUIRE(gptps_register_observer(e, observe, NULL) == GPTPS_OK);

    /* admission discipline: EDF owns the scheduler seam, shed_late is a
     * constraint that composes with it */
    REQUIRE(gptps_sched_edf(e, &edf) == GPTPS_OK);
    /* grace 0: the stale job is 20 ms past its 1 ms deadline by first
     * admission, well outside any grace, so it must be refused */
    REQUIRE(gptps_sched_shed_late(e, 0, &shed) == GPTPS_OK);

    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    d.default_policy.max_retries = 0;
    d.default_policy.on_failure = GPTPS_ON_FAILURE_DEAD_LETTER;
    for (i = 0; i < NJOBS; ++i) {
        d.name = jobs[i].name;
        d.run = run_job;
        d.user_data = &jobs[i];
        d.default_cost.mem_bytes = jobs[i].declared_mem;
        REQUIRE(gptps_register_task(e, &d) == GPTPS_OK);
    }

    /* NIGHT: submit in FIFO-adverse order - the big MRP job first */
    for (i = 0; i < NJOBS; ++i) {
        gptps_submit_options o;
        memset(&o, 0, sizeof o);
        o.struct_size = sizeof o;
        o.flags = GPTPS_SUBMIT_DEADLINE;
        o.deadline_in_ms = jobs[i].deadline_ms;
        REQUIRE(gptps_submit_ex(e, jobs[i].name, jobs[i].name,
                                strlen(jobs[i].name), &o, &jobs[i].handle) == GPTPS_OK);
    }
    /* let the stale job's 1 ms deadline elapse so shed_late sees it past due */
    spin_ms(20);
    REQUIRE(pump(e));
    /* drain BEFORE gptps_shutdown: shutdown frees the engine */
    drained = gptps_dead_letter_drain(e, inspect_dead_letter, NULL);
    REQUIRE(gptps_shutdown(e) == GPTPS_OK);
    e = NULL;
    shed_count = gptps_sched_shed_count(shed);   /* the add-on outlives the engine */

    /* MORNING: the ledger */
    printf("GPTPS pilot pass one - EDF + shed_late, single slot\n");
    printf("%-10s %-14s %-9s %-10s %s\n",
           "job", "deadline(ms)", "started", "start_seq", "result");
    for (i = 0; i < NJOBS; ++i) {
        const char *res = jobs[i].finished ? "finished"
                        : jobs[i].dead ? "dead-lettered" : "not finished";
        printf("%-10s %-14llu %-9s %-10d %s\n",
               jobs[i].name,
               (unsigned long long)jobs[i].deadline_ms,
               jobs[i].start_seq >= 0 ? "yes" : "no",
               jobs[i].start_seq,
               res);
    }
    printf("shed_late refused: %llu (expect 1: rpt_stale)\n", (unsigned long long)shed_count);
    printf("dead letters:      %u events, %lu drained (expect 1)\n",
           g_dead_letters, (unsigned long)drained);

    /* the assertion that makes this a pilot, not a smoke test:
     * among the jobs that started, start order must follow strictly
     * increasing deadline order (EDF), and MRP must be LAST even though it
     * was submitted FIRST. That inversion is EDF doing its job. */
    {
        int inv_ok = 1;
        uint64_t last_dl = 0;
        int k;
        for (k = 0; k < (int)NJOBS - 1; ++k) {
            job *j = NULL;
            size_t s;
            for (s = 0; s < NJOBS; ++s)
                if (jobs[s].start_seq == k) { j = &jobs[s]; break; }
            if (!j) { inv_ok = 0; break; }
            if (j->deadline_ms <= last_dl) { inv_ok = 0; break; }
            last_dl = j->deadline_ms;
        }
        if (jobs[0].start_seq != (int)NJOBS - 2) inv_ok = 0;   /* MRP last */
        printf("EDF order (deadline order, MRP last): %s\n", inv_ok ? "HOLDS" : "FAILS");
        REQUIRE(inv_ok);
    }

    t = jobs[STALE].start_seq < 0 && jobs[STALE].dead &&
        jobs[STALE].dead_status == GPTPS_E_DENIED && shed_count == 1 &&
        g_dead_letters == 1 && drained == 1 && g_drained_stale == 1;
    printf("the stale job was refused at admission, never ran, one dead letter (E_DENIED): %s\n",
           t ? "HOLDS" : "FAILS");
    REQUIRE(t);

    t = 1;
    for (i = 0; i < NJOBS; ++i)
        if (i != STALE && (!jobs[i].finished || jobs[i].dead)) t = 0;
    printf("every on-time job finished: %s\n", t ? "HOLDS" : "FAILS");
    REQUIRE(t);

    gptps_sched_close(edf);     /* AFTER gptps_shutdown, as the add-on requires */
    gptps_sched_close(shed);
    printf("PASS ONE COMPLETE\n");
    return 0;
}
