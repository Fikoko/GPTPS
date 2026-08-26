/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_fork.c - an engine inherited across fork() must REFUSE, not hang or crash.
 *
 * include/gptps.h and docs/SECURITY.md both state the rule flatly: an engine created
 * before a fork() cannot be used in the child, and every entry point returns
 * GPTPS_E_SHUTDOWN there. The reason it must be every entry point, not just the
 * interesting ones, is that the refusal has to happen BEFORE the mutex is taken -
 * e->m may be held by a thread that did not survive the fork, so merely locking it
 * is the hang. gptps_shutdown was the worst of them: it went on to join dispatcher
 * and worker pthread_t's that do not exist in the child (SIGSEGV, observed).
 *
 * The child exercises a spread of entry points - mutating, read-only, settings, and
 * teardown - and reports any that let it through. POSIX only: no fork() on Windows.
 */
#include "gptps.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static gptps_status noop(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }
static void         obs(const gptps_event *ev, void *u) { (void)ev; (void)u; }

int main(void)
{
    gptps_config cfg;
    gptps *e = NULL;
    gptps_task_def d;
    pid_t pid;
    int st = 0;

    memset(&cfg, 0, sizeof cfg); cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 2;          /* THREADED: real threads to not-survive */
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (!e) { printf("test_fork: FAILED (no engine)\n"); return 1; }

    memset(&d, 0, sizeof d); d.struct_size = sizeof d;
    d.name = "t"; d.run = noop; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);

    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        char b[64];
        int bad = 0;
#define REFUSES(expr, want, nm) \
    do { if ((long)(expr) != (long)(want)) { printf("FAIL %s did not refuse in the child\n", nm); ++bad; } } while (0)
        REFUSES(gptps_submit(e, "t", NULL, 0, NULL),      GPTPS_E_SHUTDOWN, "gptps_submit");
        REFUSES(gptps_register_task(e, &d),               GPTPS_E_SHUTDOWN, "gptps_register_task");
        REFUSES(gptps_unregister_task(e, "t", 0),         GPTPS_E_SHUTDOWN, "gptps_unregister_task");
        REFUSES(gptps_set_event_cb(e, NULL, NULL),        GPTPS_E_SHUTDOWN, "gptps_set_event_cb");
        REFUSES(gptps_register_observer(e, obs, NULL),    GPTPS_E_SHUTDOWN, "gptps_register_observer");
        REFUSES(gptps_task_count(e),                      0,                "gptps_task_count");
        REFUSES(gptps_task_exists(e, "t"),                0,                "gptps_task_exists");
        REFUSES(gptps_dead_letter_count(e),               0,                "gptps_dead_letter_count");
        REFUSES(gptps_settings_get(e, "limits.max_intake_depth", b, sizeof b),
                                                          GPTPS_E_SHUTDOWN, "gptps_settings_get");
        REFUSES(gptps_settings_set(e, "limits.max_intake_depth", "4"),
                                                          GPTPS_E_SHUTDOWN, "gptps_settings_set");
        /* last, and the one that used to SIGSEGV: it joins threads that are not here */
        REFUSES(gptps_shutdown(e),                        GPTPS_E_SHUTDOWN, "gptps_shutdown");
        fflush(stdout);
        _exit(bad ? 1 : 0);
    }

    CHECK(waitpid(pid, &st, 0) == pid);
    if (WIFSIGNALED(st)) {
        printf("FAIL child died from signal %d (an entry point crashed instead of refusing)\n",
               WTERMSIG(st));
        ++fails;
    } else {
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }

    /* the PARENT's engine is untouched by any of this and still shuts down cleanly */
    CHECK(gptps_shutdown(e) == GPTPS_OK);
    printf("test_fork: %s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
