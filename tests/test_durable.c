/*
 * test_durable.c - durable-queue add-on.
 *   A) happy path: dq_submit'd tasks run, and the journal marks them complete
 *      (pending -> 0) once they finish.
 *   B) crash recovery: a forked child dq_submits work and _exit()s WITHOUT a
 *      clean shutdown (a real crash); the parent reopens the journal and
 *      recovers every persisted-but-uncompleted task, which then runs.
 *
 * Phase A fully shuts down (joining all engine threads) before the fork, so the
 * fork happens from a single-threaded process.
 */
#define _POSIX_C_SOURCE 200809L
#include "gptps.h"
#include "durable_queue.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static int g_work_runs;
static int g_block;     /* child blocks tasks so none complete before the "crash" */
static int g_release;

static gptps_status task_work(gptps_ctx *ctx, void *ud)
{
    uint64_t start = gptps_now_ms(ctx);
    (void)ud;
    inc(&g_work_runs);
    while (get(&g_block) && !get(&g_release) && !gptps_is_cancelled(ctx))
        if (gptps_now_ms(ctx) - start > 5000) break;  /* safety */
    return GPTPS_OK;
}

static void reg_work(gptps *e)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "work"; d.run = task_work; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = 1024;
    d.default_policy.struct_size = sizeof d.default_policy;
    gptps_register_task(e, &d);
}

static gptps *open_engine(unsigned conc)
{
    gptps_config cfg; gptps *e = NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = conc; cfg.limits.max_memory_bytes = 64u << 20;
    gptps_open_ex(&cfg, &e);
    return e;
}

#define JOURNAL_A "dq_test_a.journal"
#define JOURNAL_B "dq_test_b.journal"
#define NREC 5

static void test_happy(void)
{
    gptps *e;
    gptps_dq *dq;
    gptps_handle h;
    int i;

    remove(JOURNAL_A);
    __atomic_store_n(&g_work_runs, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_block, 0, __ATOMIC_SEQ_CST);

    e = open_engine(2); CHECK(e != NULL);
    if (!e) return;
    reg_work(e);
    dq = gptps_dq_open(e, JOURNAL_A); CHECK(dq != NULL);
    if (!dq) { gptps_shutdown(e); return; }

    CHECK(gptps_dq_pending(dq) == 0);
    for (i = 0; i < NREC; ++i) {
        unsigned char b = (unsigned char)i;
        CHECK(gptps_dq_submit(dq, "work", &b, 1, &h) == GPTPS_OK);
    }

    gptps_shutdown(e);                       /* drains; observer marks each done */
    CHECK(get(&g_work_runs) == NREC);
    CHECK(gptps_dq_pending(dq) == 0);         /* all completions journaled */
    gptps_dq_close(dq);                       /* after shutdown */
    remove(JOURNAL_A);
}

static void test_recovery(void)
{
    pid_t pid;
    int wst = 0;

    remove(JOURNAL_B);

    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        /* ---- CHILD: persist work, then "crash" (no shutdown) ---- */
        gptps *e; gptps_dq *dq; gptps_handle h; int i;
        __atomic_store_n(&g_block, 1, __ATOMIC_SEQ_CST);   /* block so nothing completes */
        __atomic_store_n(&g_release, 0, __ATOMIC_SEQ_CST);
        e = open_engine(1);                                /* one worker: rest queue */
        if (!e) _exit(2);
        reg_work(e);
        dq = gptps_dq_open(e, JOURNAL_B);
        if (!dq) _exit(3);
        for (i = 0; i < NREC; ++i) { unsigned char b = (unsigned char)i; gptps_dq_submit(dq, "work", &b, 1, &h); }
        _exit(0);   /* abrupt: PENDING records are fsync'd, no DONE written */
    }

    /* ---- PARENT: reopen the journal and recover ---- */
    waitpid(pid, &wst, 0);
    CHECK(WIFEXITED(wst) && WEXITSTATUS(wst) == 0);

    /* Simulate a write torn by the crash: a record magic followed by a truncated
     * header. Replay must ignore this tail and keep the NREC valid records. */
    {
        FILE *j = fopen(JOURNAL_B, "ab");
        if (j) {
            unsigned char torn[8] = { 0x31, 0x52, 0x51, 0x44, 0xAA, 0xBB, 0xCC, 0xDD };
            fwrite(torn, 1, sizeof torn, j);
            fclose(j);
        }
    }
    {
        gptps *e; gptps_dq *dq; size_t recovered;
        __atomic_store_n(&g_block, 0, __ATOMIC_SEQ_CST);   /* parent: run to completion */
        __atomic_store_n(&g_work_runs, 0, __ATOMIC_SEQ_CST);
        e = open_engine(2); CHECK(e != NULL);
        if (!e) return;
        reg_work(e);
        dq = gptps_dq_open(e, JOURNAL_B); CHECK(dq != NULL);
        if (!dq) { gptps_shutdown(e); return; }

        CHECK(gptps_dq_pending(dq) == NREC);   /* the child's survivors */
        recovered = gptps_dq_recover(dq);
        CHECK(recovered == NREC);

        gptps_shutdown(e);
        CHECK(get(&g_work_runs) == NREC);      /* every survivor ran */
        CHECK(gptps_dq_pending(dq) == 0);
        gptps_dq_close(dq);
    }
    remove(JOURNAL_B);
}

int main(void)
{
    test_happy();      /* shuts down + joins threads (single-threaded before fork) */
    test_recovery();
    if (fails) { printf("%d durable-queue check(s) FAILED\n", fails); return 1; }
    printf("all durable-queue checks passed\n");
    return 0;
}
