/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * edge_admission.c - several AI jobs on one board's shared memory, started only
 * when they fit. examples/edge_ai/README.md tells the story; this is the host.
 *
 * On a Jetson the CPU and the GPU share one pool of DRAM. Start a detector, a
 * segmenter, a small LLM and a few classifiers at once and their peaks add up to
 * more than the board has, and the kernel's OOM killer ends one of them - not
 * necessarily the one at fault. Here each job declares its peak (mem_mb) and its
 * share of a resource named "gpu". GPTPS starts a job only when both fit what is
 * left of the budget, and queues the rest. A job that fails is retried and then
 * dead-lettered; one that runs past its timeout is killed; one that outgrows its
 * declaration is capped - alone - and reported.
 *
 *   edge_admission --budget-mb 768 --gpu-slots 4 jobs.txt   GPTPS admits the jobs
 *   edge_admission --naive jobs.txt                         all at once, no GPTPS
 *
 * The jobs file has one job per line; `#` starts a comment:
 *
 *   name  mem_mb  gpu_units  timeout_s  retries  command [args...]
 *
 * --naive forks every job at once and waits: no admission, no cap, no timeout, no
 * retry. It is the contrast, and it is meant to run out of memory, so run it only
 * in a memory-limited container (run_demo.sh does that), never on a machine you
 * care about.
 *
 * Exit status: 0 if every job finished, 1 if any failed for good, 2 on a usage or
 * setup error.
 */
/* feature-test macros before any header (match hal_posix.c): fork, waitpid,
 * clock_gettime and strtok_r under -std=c99. On macOS _POSIX_C_SOURCE alone would
 * hide clock_gettime, so it is not used. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#  define _DARWIN_C_SOURCE
#endif
#include "gptps.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_JOBS  64
#define MAX_ARGS  32

enum { WAITING, FINISHED, DEAD, REFUSED };

typedef struct {
    char         *line;               /* the job's own copy of its line; the strings below point into it */
    char         *name;
    char         *argv[MAX_ARGS + 1];
    unsigned long mem_mb, gpu, timeout_s, retries;
    /* what happened to it */
    int           state;
    gptps_status  status;             /* why it is a dead letter, or was refused */
    long          start_ms, end_ms;   /* from the start of the run; -1 = never */
    unsigned      attempts;
    uint64_t      peak;               /* the highest mem.peak an attempt measured (bytes) */
    char          peak_method[40];    /* how it was measured; "" = never measured */
    pid_t         pid;                /* --naive */
    int           wstatus;            /* --naive: how the process ended */
} job;

typedef struct {
    job             jobs[MAX_JOBS];
    int             n;
    unsigned long   budget_mb, gpu_slots;  /* gpu_slots 0: the gpu column is not enforced */
    pthread_mutex_t m;
    pthread_cond_t  cv;
    int             ended;                 /* jobs that reached a terminal event */
    unsigned long   mem_now, gpu_now, mem_peak, gpu_peak;   /* declared by running attempts */
    uint64_t        t0;
} run;

static const char *status_name(gptps_status s)
{
    static const char *names[] = {
        "GPTPS_OK", "GPTPS_E_NOMEM", "GPTPS_E_INVAL", "GPTPS_E_NOTFOUND", "GPTPS_E_DUP",
        "GPTPS_E_BUDGET", "GPTPS_E_FULL", "GPTPS_E_TIMEOUT", "GPTPS_E_CANCELLED",
        "GPTPS_E_ABI", "GPTPS_E_CONFIG", "GPTPS_E_IO", "GPTPS_E_TASK", "GPTPS_E_SHUTDOWN",
        "GPTPS_E_DENIED", "GPTPS_E_BUSY"
    };
    return ((unsigned)s < sizeof names / sizeof names[0]) ? names[s] : "GPTPS_E_?";
}

/* ---- the jobs file ------------------------------------------------------- */

static int parse_ul(const char *s, unsigned long *out)
{
    char *end;
    if (!s || !*s || *s == '-') return 0;
    errno = 0;
    *out = strtoul(s, &end, 10);
    return errno == 0 && *end == '\0';
}

/* 1 if `prog` can be run: a path as it is, a bare name through PATH, as execvp
 * looks it up. Checked up front, so that a missing tool is an error that names it
 * rather than a job that exits 127 and ends as a dead letter with GPTPS_E_TASK. */
static int runnable(const char *prog)
{
    const char *p = getenv("PATH");
    char buf[4096];
    if (strchr(prog, '/')) return access(prog, X_OK) == 0;
    for (p = p ? p : "/usr/bin:/bin"; ; ++p) {
        size_t n = strcspn(p, ":");
        int w = n ? snprintf(buf, sizeof buf, "%.*s/%s", (int)n, p, prog)
                  : snprintf(buf, sizeof buf, "%s", prog);     /* an empty entry is "." */
        if (w > 0 && (size_t)w < sizeof buf && access(buf, X_OK) == 0) return 1;
        p += n;
        if (!*p) return 0;
    }
}

static int load_jobs(run *r, const char *path)
{
    char buf[4096];
    int lineno = 0, ok = 0;
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "edge_admission: cannot open %s\n", path); return 0; }
    while (fgets(buf, sizeof buf, f)) {
        char *tok[MAX_ARGS + 5], *p, *save = NULL;
        size_t len = 0;
        int nt = 0, k;
        job *j = &r->jobs[r->n];
        ++lineno;
        if (!strchr(buf, '\n') && !feof(f)) { fprintf(stderr, "%s:%d: line too long\n", path, lineno); goto out; }
        buf[strcspn(buf, "#")] = '\0';
        for (p = strtok_r(buf, " \t\r\n", &save); p; p = strtok_r(NULL, " \t\r\n", &save)) {
            if (nt == MAX_ARGS + 5) { fprintf(stderr, "%s:%d: more than %d arguments\n", path, lineno, MAX_ARGS); goto out; }
            tok[nt++] = p;
        }
        if (nt == 0) continue;
        if (nt < 6) {
            fprintf(stderr, "%s:%d: want: name mem_mb gpu_units timeout_s retries command [args...]\n", path, lineno);
            goto out;
        }
        if (r->n == MAX_JOBS) { fprintf(stderr, "%s: more than %d jobs\n", path, MAX_JOBS); goto out; }
        if (!parse_ul(tok[1], &j->mem_mb) || !parse_ul(tok[2], &j->gpu) || !parse_ul(tok[3], &j->timeout_s) ||
            !parse_ul(tok[4], &j->retries) || j->mem_mb > 1048576ul || j->timeout_s > 86400ul || j->retries > 100ul) {
            fprintf(stderr, "%s:%d: mem_mb (up to 1048576), gpu_units, timeout_s (up to 86400) and "
                            "retries (up to 100) are whole numbers\n", path, lineno);
            goto out;
        }
        if (!runnable(tok[5])) {
            fprintf(stderr, "%s:%d: %s: %s\n", path, lineno, tok[5],
                    strchr(tok[5], '/') ? "no such program" : "not found on PATH");
            goto out;
        }
        for (k = 0; k < nt; ++k) len += strlen(tok[k]) + 1;
        if (!(p = j->line = (char *)malloc(len))) goto out;
        for (k = 0; k < nt; ++k) {      /* name, then argv; the numbers are parsed already */
            size_t l = strlen(tok[k]) + 1;
            memcpy(p, tok[k], l);
            if (k == 0) j->name = p;
            else if (k >= 5) j->argv[k - 5] = p;
            p += l;
        }
        j->argv[nt - 5] = NULL;
        j->state = WAITING;
        j->start_ms = j->end_ms = -1;
        r->n++;
    }
    if (r->n == 0) fprintf(stderr, "%s: no jobs\n", path);
    else ok = 1;
out:
    fclose(f);
    return ok;
}

/* ---- GPTPS mode ---------------------------------------------------------- */

static job *find(run *r, const char *name)
{
    int i;
    for (i = 0; name && i < r->n; ++i)
        if (!strcmp(r->jobs[i].name, name)) return &r->jobs[i];
    return NULL;
}

static void log_line(run *r, uint64_t ts, const char *what, const job *j, const char *more)
{
    printf("%8.3f  %-7s %-16s %4lu/%lu MB", (double)(ts - r->t0) / 1000.0, what, j->name,
           r->mem_now, r->budget_mb);
    if (r->gpu_slots) printf("  gpu %lu/%lu", r->gpu_now, r->gpu_slots);
    printf("%s%s\n", more ? "   " : "", more ? more : "");
}

/* Every job is its own task type, so an event names its job: there is no handle to
 * map, and so no race with a job that ends before gptps_submit has returned its
 * handle. Events come from worker and dispatcher threads; the lock keeps the
 * in-flight sums and the log lines together. The STARTED of an attempt always
 * comes before its FINISHED or FAILED, so the sums never go below zero. */
static void on_event(const gptps_event *ev, void *ud)
{
    run *r = (run *)ud;
    job *j = find(r, ev->task_name);
    char more[64];
    const gptps_measure *pk;
    if (!j) return;
    /* The kinds this host acts on; any other - QUEUED, RETRIED, a SAMPLE, or one a
     * later engine adds - is ignored, never mistaken for an ending. */
    if (ev->kind != GPTPS_EV_STARTED && ev->kind != GPTPS_EV_FINISHED && ev->kind != GPTPS_EV_FAILED &&
        ev->kind != GPTPS_EV_DEAD_LETTERED && ev->kind != GPTPS_EV_DROPPED) return;
    pthread_mutex_lock(&r->m);
    /* What the attempt actually used, measured by GPTPS (docs/MEASUREMENTS.md), next
     * to what the job declared. */
    if ((pk = gptps_event_measure(ev, GPTPS_M_MEM_PEAK)) != NULL && pk->value >= j->peak) {
        j->peak = pk->value;
        snprintf(j->peak_method, sizeof j->peak_method, "%s", pk->method);
    }
    switch (ev->kind) {
    case GPTPS_EV_STARTED:
        if (j->start_ms < 0) j->start_ms = (long)(ev->ts_ms - r->t0);
        j->attempts = ev->attempt;
        r->mem_now += j->mem_mb; r->gpu_now += j->gpu;
        if (r->mem_now > r->mem_peak) r->mem_peak = r->mem_now;
        if (r->gpu_now > r->gpu_peak) r->gpu_peak = r->gpu_now;
        snprintf(more, sizeof more, "attempt %u", (unsigned)ev->attempt);
        log_line(r, ev->ts_ms, "start", j, ev->attempt > 1 ? more : NULL);
        break;
    case GPTPS_EV_FINISHED:
        r->mem_now -= j->mem_mb; r->gpu_now -= j->gpu;
        j->state = FINISHED; j->end_ms = (long)(ev->ts_ms - r->t0);
        r->ended++;
        log_line(r, ev->ts_ms, "done", j, NULL);
        break;
    case GPTPS_EV_FAILED:               /* one attempt: a retry or a dead letter follows */
        r->mem_now -= j->mem_mb; r->gpu_now -= j->gpu;
        snprintf(more, sizeof more, "attempt %u: %s", (unsigned)ev->attempt, status_name(ev->status));
        log_line(r, ev->ts_ms, "failed", j, more);
        break;
    default:                            /* DEAD_LETTERED, or DROPPED (which no job here uses) */
        j->state = DEAD; j->status = ev->status; j->end_ms = (long)(ev->ts_ms - r->t0);
        r->ended++;
        log_line(r, ev->ts_ms, "dead", j, status_name(ev->status));
        break;
    }
    if (r->ended == r->n) pthread_cond_signal(&r->cv);
    pthread_mutex_unlock(&r->m);
}

static int fail(gptps *e, const char *what, gptps_status st)
{
    fprintf(stderr, "edge_admission: %s: %s\n", what, gptps_strerror(st));
    if (e) gptps_shutdown(e);
    return 2;
}

static int run_gptps(run *r)
{
    gptps_config cfg;
    gptps *e = NULL;
    gptps_status st;
    int i;

    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg;
    cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_memory_bytes = (uint64_t)r->budget_mb << 20;
    /* A worker only waits on its job's process, so give every job one: memory and
     * the gpu slots are what limit this run, not the number of CPUs. */
    cfg.limits.max_concurrent_tasks = (uint32_t)r->n;
    if ((st = gptps_open_ex(&cfg, &e)) != GPTPS_OK) return fail(NULL, "gptps_open_ex", st);
    if (r->gpu_slots && (st = gptps_define_resource(e, "gpu", r->gpu_slots)) != GPTPS_OK)
        return fail(e, "gptps_define_resource", st);
    if ((st = gptps_register_observer(e, on_event, r)) != GPTPS_OK)
        return fail(e, "gptps_register_observer", st);

    for (i = 0; i < r->n; ++i) {        /* one PROGRAM task per job */
        job *j = &r->jobs[i];
        gptps_task_def d;
        memset(&d, 0, sizeof d);
        d.struct_size = sizeof d;
        d.name = j->name;
        d.exec = GPTPS_EXEC_PROGRAM;                            /* fork + exec the command */
        d.argv = (const char *const *)j->argv;
        d.default_cost.struct_size = sizeof d.default_cost;
        d.default_cost.mem_bytes = (uint64_t)j->mem_mb << 20;   /* admitted against the budget,
                                                                 * and the job's own cap */
        d.default_policy.struct_size = sizeof d.default_policy;
        d.default_policy.timeout_seconds = (uint32_t)j->timeout_s;
        d.default_policy.max_retries = (uint32_t)j->retries;
        d.default_policy.on_failure = GPTPS_ON_FAILURE_DEAD_LETTER;
        if ((st = gptps_register_task(e, &d)) != GPTPS_OK)
            return fail(e, st == GPTPS_E_DUP ? "two jobs have one name" : j->name, st);
        if (r->gpu_slots && (st = gptps_set_task_resource_cost(e, j->name, "gpu", j->gpu)) != GPTPS_OK)
            return fail(e, j->name, st);
    }

    printf("    time  event   job              in flight (declared)\n");
    r->t0 = gptps_now_ms(NULL);
    for (i = 0; i < r->n; ++i) {
        job *j = &r->jobs[i];
        gptps_handle h;
        if ((st = gptps_submit(e, j->name, NULL, 0, &h)) != GPTPS_OK) {
            /* GPTPS_E_BUDGET: it can never fit, so it is refused now rather than queued forever */
            pthread_mutex_lock(&r->m);
            j->state = REFUSED; j->status = st;
            r->ended++;
            printf("%8.3f  %-7s %-16s %s\n", (double)(gptps_now_ms(NULL) - r->t0) / 1000.0,
                   "refused", j->name, status_name(st));
            pthread_mutex_unlock(&r->m);
        }
    }
    pthread_mutex_lock(&r->m);
    while (r->ended < r->n) pthread_cond_wait(&r->cv, &r->m);
    pthread_mutex_unlock(&r->m);
    gptps_shutdown(e);                  /* nothing is left to run: this only frees */
    return 0;
}

/* ---- naive mode: the same jobs, all at once, without GPTPS ---------------- */

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

static long read_oom_kill(const char *path)
{
    char key[64];
    long n;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    while (fscanf(f, "%63s %ld", key, &n) == 2)
        if (!strcmp(key, "oom_kill")) { fclose(f); return n; }
    fclose(f);
    return -1;
}

/* The kernel's count of OOM kills in this process's memory cgroup, or -1: cgroup
 * v2's memory.events, else v1's memory.oom_control. The process's own cgroup
 * first; in a container, where that path is not mounted, the mount's root, which
 * is the container. Read before and after, it tells the OOM killer's SIGKILLs from
 * any other. */
static long oom_kills(void)
{
    char line[512], path[600];
    long n = -1;
    FILE *f = fopen("/proc/self/cgroup", "r");
    while (n < 0 && f && fgets(line, sizeof line, f)) {   /* "0::/path" or "4:memory:/path" */
        char *c = strchr(line, ':');
        line[strcspn(line, "\n")] = '\0';
        if (c && !strncmp(c, "::", 2)) {
            snprintf(path, sizeof path, "/sys/fs/cgroup%s/memory.events", c + 2);
            n = read_oom_kill(path);
        } else if (c && !strncmp(c, ":memory:", 8)) {
            snprintf(path, sizeof path, "/sys/fs/cgroup/memory%s/memory.oom_control", c + 8);
            n = read_oom_kill(path);
        }
    }
    if (f) fclose(f);
    if (n < 0) n = read_oom_kill("/sys/fs/cgroup/memory.events");
    if (n < 0) n = read_oom_kill("/sys/fs/cgroup/memory/memory.oom_control");
    return n;
}

static const char *signal_name(int sig)
{
    switch (sig) {
    case SIGKILL: return "SIGKILL";
    case SIGABRT: return "SIGABRT";
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS:  return "SIGBUS";
    case SIGTERM: return "SIGTERM";
    case SIGINT:  return "SIGINT";
    default:      return "a signal";
    }
}

static void describe_exit(int ws, char *buf, size_t cap)
{
    if (WIFSIGNALED(ws)) snprintf(buf, cap, "killed by %s", signal_name(WTERMSIG(ws)));
    else                 snprintf(buf, cap, "exit %d", WEXITSTATUS(ws));
}

static int run_naive(run *r)
{
    uint64_t t0;
    int i, left = 0;
    long oom_before = oom_kills();

    printf("    time  event   job\n");
    t0 = mono_ms();
    for (i = 0; i < r->n; ++i) {
        job *j = &r->jobs[i];
        j->pid = fork();
        if (j->pid == 0) {
            /* As GPTPS runs it: nothing on stdin, stdout not shown, stderr shared. */
            int null = open("/dev/null", O_RDWR);
            if (null >= 0) { dup2(null, 0); dup2(null, 1); if (null > 1) close(null); }
            execvp(j->argv[0], j->argv);
            fprintf(stderr, "edge_admission: cannot run %s\n", j->argv[0]);
            _exit(127);
        }
        if (j->pid < 0) { j->state = DEAD; continue; }
        j->start_ms = (long)(mono_ms() - t0);
        j->attempts = 1;
        printf("%8.3f  %-7s %s\n", (double)j->start_ms / 1000.0, "start", j->name);
        ++left;
    }
    while (left > 0) {
        char how[48];
        int ws;
        pid_t pid = waitpid(-1, &ws, 0);
        if (pid < 0) { if (errno == EINTR) continue; break; }
        for (i = 0; i < r->n; ++i) {
            job *j = &r->jobs[i];
            if (j->state != WAITING || j->pid != pid) continue;
            j->wstatus = ws;
            j->end_ms = (long)(mono_ms() - t0);
            j->state = (WIFEXITED(ws) && WEXITSTATUS(ws) == 0) ? FINISHED : DEAD;
            describe_exit(ws, how, sizeof how);
            if (j->state == FINISHED)
                printf("%8.3f  %-7s %s\n", (double)j->end_ms / 1000.0, "done", j->name);
            else
                printf("%8.3f  %-7s %-16s   %s\n", (double)j->end_ms / 1000.0, "ended", j->name, how);
            --left;
            break;
        }
    }
    if (oom_before >= 0) {
        long d = oom_kills() - oom_before;
        if (d >= 0) printf("the kernel OOM-killed %ld process%s in this memory cgroup during the run\n",
                           d, d == 1 ? "" : "es");
    }
    return 0;
}

/* ---- the report ---------------------------------------------------------- */

static int report(const run *r, int naive)
{
    unsigned long all_mem = 0, all_gpu = 0;
    int i, finished = 0;
    char start[24], end[24], result[64], peak[24];
    const char *method = NULL;
    int methods = 0;

    /* GPTPS mode adds what each job measured at its peak (docs/MEASUREMENTS.md) next
     * to what it declared; naive mode has no GPTPS to measure anything. */
    if (naive) printf("\njob                mem MB   gpu  start ms    end ms  tries  result\n");
    else       printf("\njob                mem MB  peak MB   gpu  start ms    end ms  tries  result\n");
    for (i = 0; i < r->n; ++i) {
        const job *j = &r->jobs[i];
        all_mem += j->mem_mb;
        all_gpu += j->gpu;
        if (j->start_ms >= 0) snprintf(start, sizeof start, "%ld", j->start_ms); else strcpy(start, "-");
        if (j->end_ms >= 0) snprintf(end, sizeof end, "%ld", j->end_ms); else strcpy(end, "-");
        if (j->state == FINISHED) {
            ++finished;
            snprintf(result, sizeof result, "finished%s", j->attempts > 1 ? " after a retry" : "");
        } else if (naive) {
            if (j->start_ms < 0) strcpy(result, "could not fork");
            else describe_exit(j->wstatus, result, sizeof result);
        } else if (j->state == REFUSED) {
            snprintf(result, sizeof result, "refused: %s%s", status_name(j->status),
                     j->status == GPTPS_E_BUDGET ? " (can never fit)" : "");
        } else {
            snprintf(result, sizeof result, "dead letter: %s", status_name(j->status));
        }
        if (naive) {
            printf("%-16s %8lu %5lu %9s %9s %6u  %s\n", j->name, j->mem_mb, j->gpu, start, end,
                   j->attempts, result);
            continue;
        }
        if (j->peak_method[0]) {
            snprintf(peak, sizeof peak, "%.1f", (double)j->peak / 1048576.0);
            if (!method || strcmp(method, j->peak_method) != 0) { method = j->peak_method; ++methods; }
        } else {
            strcpy(peak, "-");
        }
        printf("%-16s %8lu %8s %5lu %9s %9s %6u  %s\n", j->name, j->mem_mb, peak, j->gpu, start, end,
               j->attempts, result);
    }
    printf("\n");
    if (!naive && method)
        printf("peak MB: measured by GPTPS, the highest of a job's attempts (%s)\n",
               methods == 1 ? method : "methods differ by job: see docs/MEASUREMENTS.md");
    if (naive) {
        printf("all at once they declared %lu MB", all_mem);
        if (r->budget_mb) printf(" (%.1fx a %lu MB budget)", (double)all_mem / (double)r->budget_mb, r->budget_mb);
        printf(" and %lu gpu units\n", all_gpu);
    } else {
        printf("peak declared in flight: %lu MB of a %lu MB budget", r->mem_peak, r->budget_mb);
        if (r->gpu_slots) printf(", gpu %lu of %lu", r->gpu_peak, r->gpu_slots);
        printf("\nall at once they would declare %lu MB (%.1fx the budget)\n",
               all_mem, (double)all_mem / (double)r->budget_mb);
    }
    printf("%d of %d jobs finished\n", finished, r->n);
    return finished == r->n ? 0 : 1;
}

static int usage(void)
{
    fprintf(stderr,
        "usage: edge_admission --budget-mb N [--gpu-slots N] JOBS_FILE\n"
        "       edge_admission --naive [--budget-mb N] JOBS_FILE\n"
        "  --budget-mb N   memory the board can spare for these jobs (limits.max_memory_bytes)\n"
        "  --gpu-slots N   budget of the resource \"gpu\" (left out: gpu_units are not enforced)\n"
        "  --naive         fork every job at once: no admission, and no GPTPS\n"
        "jobs file, one per line:  name mem_mb gpu_units timeout_s retries command [args...]\n");
    return 2;
}

int main(int argc, char **argv)
{
    static run r;                       /* static: 64 jobs is a lot for a small stack */
    const char *path = NULL;
    int naive = 0, a, rc;

    for (a = 1; a < argc; ++a) {
        if (!strcmp(argv[a], "--naive")) naive = 1;
        else if (!strcmp(argv[a], "--budget-mb") && a + 1 < argc && parse_ul(argv[a + 1], &r.budget_mb)) ++a;
        else if (!strcmp(argv[a], "--gpu-slots") && a + 1 < argc && parse_ul(argv[a + 1], &r.gpu_slots)) ++a;
        else if (argv[a][0] != '-' && !path) path = argv[a];
        else return usage();
    }
    if (!path || (!naive && (r.budget_mb == 0 || r.budget_mb > 1048576ul))) return usage();
    if (!load_jobs(&r, path)) rc = 2;
    else {
        setvbuf(stdout, NULL, _IOLBF, 0);    /* each line out at once, in order with the jobs' stderr,
                                              * and none left in a buffer a forked child copies */
        pthread_mutex_init(&r.m, NULL);
        pthread_cond_init(&r.cv, NULL);
        if (naive) {
            printf("edge_admission --naive: %d jobs, all at once, no admission\n", r.n);
            rc = run_naive(&r);
        } else {
            const char *cg = getenv("GPTPS_CGROUP_PARENT");
            printf("edge_admission: %d jobs, budget %lu MB, ", r.n, r.budget_mb);
            if (r.gpu_slots) printf("gpu %lu slots", r.gpu_slots); else printf("gpu not limited");
            if (cg && *cg) printf(", cap: cgroup v2 under %s\n", cg);
            else           printf(", cap: RLIMIT_AS\n");
            rc = run_gptps(&r);
        }
        if (rc == 0) rc = report(&r, naive);
        pthread_cond_destroy(&r.cv);
        pthread_mutex_destroy(&r.m);
    }
    for (a = 0; a < r.n; ++a) free(r.jobs[a].line);
    return rc;
}
