/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_cgroup.c - accurate memory enforcement via cgroup v2 (Linux).
 *
 * Prepares a memory-delegated parent cgroup, points GPTPS at it via
 * GPTPS_CGROUP_PARENT, then runs out-of-process tasks: one that blows past its
 * memory cap (must be OOM-killed and reported GPTPS_E_NOMEM) and one that stays
 * under it (must finish). SKIPS (exit 0) where cgroup v2 delegation is absent
 * (macOS, a runner with no delegated cgroup) - there the executor falls back to
 * RLIMIT_AS, covered by test_oop. A GPTPS_CGROUP_PARENT already set names a
 * delegated parent to use as it is: CI's cgroup step makes one by hand, and runs
 * this test (and test_measure) inside it.
 *
 * Also: a job's cgroup is removed when the job ends - all of it when GPTPS stopped
 * the job (a process that left its process group too), and, when the job ended on
 * its own and left a process running, once that process has gone, with one WARN.
 */
#define _POSIX_C_SOURCE 200809L
#include "gptps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static int c_hog_nomem, c_small_ok, c_hog_cap_hit, c_small_measured;
static int c_spin_ok, c_spin_throttled, c_spin_unthrottled;
static int c_io_ok, c_io_capped;

/* `needle` in the first `n` bytes of `hay` (no memmem in C99) */
static int memmem_s(const char *hay, size_t n, const char *needle)
{
    size_t k = strlen(needle), i;
    for (i = 0; i + k <= n; ++i) if (memcmp(hay + i, needle, k) == 0) return 1;
    return 0;
}

static void on_ev(const gptps_event *ev, void *ud)
{
    const gptps_measure *m;
    (void)ud;
    if (ev->kind == GPTPS_EV_FAILED && ev->status == GPTPS_E_NOMEM &&
        strcmp(ev->task_name, "hog") == 0) {
        inc(&c_hog_nomem);
        /* the job's cgroup recorded reaching its cap (docs/MEASUREMENTS.md) */
        m = gptps_event_measure(ev, GPTPS_M_MEM_CAP_HIT);
        if (m && m->value == 1 && strcmp(m->method, "cgroup.job") == 0) inc(&c_hog_cap_hit);
    }
    /* the CPU cap (ABI 2.6): the capped job is held back, and says so */
    if (ev->kind == GPTPS_EV_FINISHED && strcmp(ev->task_name, "spin") == 0) {
        inc(&c_spin_ok);
        m = gptps_event_measure(ev, GPTPS_M_CPU_THROTTLED_MS);
        if (m && m->value > 0 && strcmp(m->method, "cgroup.job") == 0) inc(&c_spin_throttled);
    }
    if (ev->kind == GPTPS_EV_FINISHED && strcmp(ev->task_name, "small") == 0 &&
        gptps_event_measure(ev, GPTPS_M_CPU_THROTTLED_MS) == NULL) inc(&c_spin_unthrottled);
    /* the I/O cap (ABI 2.6): the job's own cgroup holds io.max for its devices */
    if (ev->kind == GPTPS_EV_FINISHED && strcmp(ev->task_name, "ioread") == 0) {
        inc(&c_io_ok);
        if (ev->result && ev->result_len &&
            memmem_s((const char *)ev->result, ev->result_len, "wbps=1048576") &&
            memmem_s((const char *)ev->result, ev->result_len, "rbps=2097152")) inc(&c_io_capped);
    }
    if (ev->kind == GPTPS_EV_FINISHED && strcmp(ev->task_name, "small") == 0) {
        inc(&c_small_ok);
        m = gptps_event_measure(ev, GPTPS_M_MEM_CAP_HIT);
        if (m && m->value == 0 && gptps_event_measure(ev, GPTPS_M_MEM_PEAK)) inc(&c_small_measured);
    }
}

/* OOP task bodies (run in the forked child) */
static gptps_status task_hog(gptps_ctx *ctx, void *ud)
{
    int i; (void)ctx; (void)ud;
    for (i = 0; i < 256; ++i) {            /* leak + touch ~256 MiB; cap is 32 MiB */
        char *b = (char *)malloc(1u << 20);
        if (!b) return GPTPS_E_NOMEM;
        memset(b, 1, 1u << 20);
    }
    return GPTPS_OK;                        /* unreachable: OOM-killed first */
}
static gptps_status task_small(gptps_ctx *ctx, void *ud)
{
    char *b = (char *)malloc(4u << 20); (void)ctx; (void)ud; /* 4 MiB, under the cap */
    if (!b) return GPTPS_E_NOMEM;
    memset(b, 1, 4u << 20); free(b);
    return GPTPS_OK;
}

/* Burn 100 ms of CPU. Under a 50-millicpu cap that is two seconds of wall time, nearly
 * all of it throttled; uncapped it is 100 ms and never throttled. */
static gptps_status task_spin(gptps_ctx *ctx, void *ud)
{
    volatile unsigned long x = 0;
    clock_t start = clock();
    (void)ctx; (void)ud;
    while ((double)(clock() - start) / CLOCKS_PER_SEC < 0.1) x += 1;
    return GPTPS_OK;
}

/* In the job: read the io.max of the cgroup this process is in, and return it. That
 * is the kernel's own record of what holds the job - the proof, from inside, that the
 * cap is the job's and not just written somewhere. */
static gptps_status task_ioread(gptps_ctx *ctx, void *ud)
{
    char line[1024], path[1200], buf[4096];
    size_t n = 0;
    FILE *f = fopen("/proc/self/cgroup", "r");
    (void)ud;
    if (!f) return GPTPS_E_IO;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "0::", 3) == 0) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            snprintf(path, sizeof path, "/sys/fs/cgroup%s/io.max", line + 3);
            break;
        }
    }
    fclose(f);
    f = fopen(path, "r");
    if (!f) return GPTPS_E_IO;
    n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return gptps_result_set(ctx, buf, n);
}

static int cg_write(const char *dir, const char *file, const char *val)
{
    char path[2048]; FILE *f;
    /* refuse, never truncate: a cut path could name another cgroup's file */
    if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, file) >= sizeof path) return -1;
    f = fopen(path, "w");
    if (!f) return -1;
    fputs(val, f);
    return fclose(f) == 0 ? 0 : -1;
}

/* Find the systemd user-delegation boundary and prepare a memory-enabled parent
 * cgroup under it. Returns 1 (sets GPTPS_CGROUP_PARENT + fills `parent`) or 0. */
static int g_owned;   /* the parent is this test's own, to remove at the end */

static int probe_parent(const char *parent)
{
    char probe[1088];
    if ((size_t)snprintf(probe, sizeof probe, "%s/_probe", parent) >= sizeof probe) return 0;
    if (mkdir(probe, 0700) != 0) return 0;
    if (cg_write(probe, "memory.max", "33554432") != 0) { rmdir(probe); return 0; }
    rmdir(probe);
    return 1;
}

static int setup_parent(char *parent, size_t pn)
{
    char line[1024], base[1024], probe[1088];
    char *nl, *at, *svc;
    const char *given = getenv("GPTPS_CGROUP_PARENT");
    FILE *f;
    if (given && *given) {                       /* delegated already: use it as it is */
        if ((size_t)snprintf(parent, pn, "%s", given) >= pn) return 0;
        return probe_parent(parent);
    }
    f = fopen("/proc/self/cgroup", "r");
    if (!f) return 0;
    if (!fgets(line, sizeof line, f)) { fclose(f); return 0; }
    fclose(f);
    if ((nl = strchr(line, '\n')) != NULL) *nl = 0;
    /* format: "0::/user.slice/.../user@<uid>.service/.../<scope>" */
    if (strncmp(line, "0::", 3) != 0) return 0;
    at = strstr(line + 3, "/user@");
    if (!at) return 0;
    svc = strstr(at, ".service");
    if (!svc) return 0;
    svc[8] = 0;                                  /* cut just after ".service" */
    if ((size_t)snprintf(base, sizeof base, "/sys/fs/cgroup%s", line + 3) >= sizeof base)
        return 0;                                /* path too long -> skip, never truncate */

    if ((size_t)snprintf(parent, pn, "%s/gptps_test_%ld", base, (long)getpid()) >= pn)
        return 0;
    if (mkdir(parent, 0700) != 0) return 0;      /* no delegation here -> skip */
    if (cg_write(parent, "cgroup.subtree_control", "+memory") != 0) { rmdir(parent); return 0; }
    (void)cg_write(parent, "cgroup.subtree_control", "+cpu");   /* best effort: see cpu_delegated */
    (void)cg_write(parent, "cgroup.subtree_control", "+io");    /* best effort: see io_delegated */
    /* verify a child can actually take a memory cap */
    if ((size_t)snprintf(probe, sizeof probe, "%s/_probe", parent) >= sizeof probe) { rmdir(parent); return 0; }
    if (mkdir(probe, 0700) != 0) { rmdir(parent); return 0; }
    if (cg_write(probe, "memory.max", "33554432") != 0) { rmdir(probe); rmdir(parent); return 0; }
    rmdir(probe);

    setenv("GPTPS_CGROUP_PARENT", parent, 1);
    g_owned = 1;
    return 1;
}

/* Can a job's cgroup under the parent take a CPU cap? The CPU part of this test needs
 * it and skips without it; the memory part does not. */
static int cpu_delegated(const char *parent)
{
    char probe[1088];
    int ok;
    if ((size_t)snprintf(probe, sizeof probe, "%s/_cpuprobe", parent) >= sizeof probe) return 0;
    if (mkdir(probe, 0700) != 0) return 0;
    ok = cg_write(probe, "cpu.max", "5000 100000") == 0;
    rmdir(probe);
    return ok;
}

/* Can a job's cgroup under the parent take an I/O cap on some block device? The I/O
 * part of this test needs it and skips without it. */
static int io_delegated(const char *parent)
{
    char probe[1088], dev[64], line[160], p[300];
    FILE *ls = popen("ls /sys/block 2>/dev/null", "r");
    int ok = 0;
    if (!ls) return 0;
    if ((size_t)snprintf(probe, sizeof probe, "%s/_ioprobe", parent) >= sizeof probe || mkdir(probe, 0700) != 0) { pclose(ls); return 0; }
    while (!ok && fgets(line, sizeof line, ls)) {
        FILE *f;
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        snprintf(p, sizeof p, "/sys/block/%s/dev", line);
        if (!(f = fopen(p, "r"))) continue;
        if (fgets(dev, sizeof dev, f)) {
            char val[128];
            if ((nl = strchr(dev, '\n')) != NULL) *nl = 0;
            snprintf(val, sizeof val, "%s wbps=1048576", dev);
            ok = cg_write(probe, "io.max", val) == 0;
        }
        fclose(f);
    }
    pclose(ls);
    rmdir(probe);
    return ok;
}

/* ---- the job's cgroup is removed (PROGRAM jobs, through /bin/sh) ---- */
static int g_cg_warns, g_left_ok, g_left_done, g_stopped_st;
static void on_cg_log(gptps_log_level lvl, const char *msg, void *ud)
{
    (void)ud;
    if (lvl == GPTPS_LOG_WARN && msg && strstr(msg, "cgroup") && strstr(msg, "cannot be removed")) inc(&g_cg_warns);
}
static void on_cg_ev(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind != GPTPS_EV_FINISHED && ev->kind != GPTPS_EV_FAILED) return;
    if (ev->kind == GPTPS_EV_FINISHED) inc(&g_left_ok);
    else __atomic_store_n(&g_stopped_st, (int)ev->status, __ATOMIC_SEQ_CST);
    inc(&g_left_done);
}
static int job_cgroups(const char *parent)       /* gptps.* under the parent */
{
    DIR *d = opendir(parent);
    struct dirent *de;
    int n = 0;
    if (!d) return -1;
    while ((de = readdir(d)) != NULL) if (strncmp(de->d_name, "gptps.", 6) == 0) ++n;
    closedir(d);
    return n;
}
/* there and not a zombie: an init that does not reap (a container's) keeps those */
static int running(long pid)
{
    char path[64], st[512], *rp;
    FILE *f;
    size_t n;
    if (kill((pid_t)pid, 0) != 0) return 0;
    snprintf(path, sizeof path, "/proc/%ld/stat", pid);
    if (!(f = fopen(path, "r"))) return 0;
    n = fread(st, 1, sizeof st - 1, f);
    fclose(f);
    st[n] = 0;
    rp = strrchr(st, ')');
    return !(rp && rp[1] == ' ' && (rp[2] == 'Z' || rp[2] == 'X'));
}
static void def_sh(gptps_task_def *d, const char *name, const char *const *argv, uint32_t timeout_s)
{
    memset(d, 0, sizeof *d);
    d->struct_size = sizeof *d; d->name = name; d->exec = GPTPS_EXEC_PROGRAM; d->argv = argv;
    d->default_cost.struct_size = sizeof d->default_cost;
    d->default_cost.mem_bytes = 64u << 20;       /* a cap, so the job has a cgroup */
    d->default_policy.struct_size = sizeof d->default_policy;
    d->default_policy.timeout_seconds = timeout_s;
}
static void sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u); ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) != 0) { }
}
static void cgroup_removal(const char *parent, const gptps_config *cfg)
{
    static const char *const a_left[] = { "/bin/sh", "-c", "sleep 2 </dev/null >/dev/null 2>&1 & echo ok", NULL };
    static const char *const a_quick[] = { "/bin/sh", "-c", "echo ok", NULL };
    static const char *const a_esc[] = { "/bin/sh", "-c",
        "setsid sh -c 'echo $$ > gptps_cg_escapee.txt; exec sleep 30' </dev/null >/dev/null 2>&1 & sleep 30", NULL };
    gptps *e = NULL;
    gptps_task_def d;
    int base = job_cgroups(parent), t;
    long esc = 0;
    FILE *f;
    gptps_set_log_sink(on_cg_log, NULL);
    CHECK(gptps_open_ex(cfg, &e) == GPTPS_OK);
    if (!e) return;
    gptps_set_event_cb(e, on_cg_ev, NULL);
    def_sh(&d, "left", a_left, 0);  CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    def_sh(&d, "quick", a_quick, 0); CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    def_sh(&d, "esc", a_esc, 1);     CHECK(gptps_register_task(e, &d) == GPTPS_OK);

    /* 1) ended on its own, left `sleep 2` running: the cgroup stays while it runs,
     * and that is said */
    CHECK(gptps_submit(e, "left", NULL, 0, NULL) == GPTPS_OK);
    for (t = 0; t < 1000 && __atomic_load_n(&g_left_done, __ATOMIC_SEQ_CST) < 1; ++t) sleep_ms(10);
    CHECK(get(&g_left_ok) == 1);
    CHECK(job_cgroups(parent) == base + 1);
    CHECK(get(&g_cg_warns) == 1);
    /* ... and is removed once it has gone, when a later job ends */
    sleep_ms(2500);
    CHECK(gptps_submit(e, "quick", NULL, 0, NULL) == GPTPS_OK);
    for (t = 0; t < 1000 && __atomic_load_n(&g_left_done, __ATOMIC_SEQ_CST) < 2; ++t) sleep_ms(10);
    CHECK(job_cgroups(parent) == base);
    CHECK(get(&g_cg_warns) == 1);                   /* said once */

    /* 2) stopped by its deadline: everything in its cgroup goes, a process that left
     * the job's process group too, and so does the cgroup */
    remove("gptps_cg_escapee.txt");
    CHECK(gptps_submit(e, "esc", NULL, 0, NULL) == GPTPS_OK);
    for (t = 0; t < 1000 && __atomic_load_n(&g_left_done, __ATOMIC_SEQ_CST) < 3; ++t) sleep_ms(10);
    CHECK(__atomic_load_n(&g_stopped_st, __ATOMIC_SEQ_CST) == (int)GPTPS_E_TIMEOUT);
    if ((f = fopen("gptps_cg_escapee.txt", "r")) != NULL) {
        if (fscanf(f, "%ld", &esc) != 1) esc = 0;
        fclose(f);
    }
    CHECK(esc > 1);
    if (esc > 1) {
        for (t = 0; t < 100 && running(esc); ++t) sleep_ms(10);
        CHECK(!running(esc));
        if (running(esc)) kill((pid_t)esc, SIGKILL);
    }
    CHECK(job_cgroups(parent) == base);
    gptps_shutdown(e);
    gptps_set_log_sink(NULL, NULL);
    remove("gptps_cg_escapee.txt");
}

static void def_oop(gptps_task_def *d, const char *name, gptps_run_fn run)
{
    memset(d, 0, sizeof *d);
    d->struct_size = sizeof *d; d->name = name; d->run = run; d->exec = GPTPS_EXEC_OOP;
    d->default_cost.struct_size = sizeof d->default_cost;
    d->default_cost.mem_bytes = 32u << 20;       /* 32 MiB cap */
    d->default_policy.struct_size = sizeof d->default_policy; /* no timeout: only the cap kills */
}

int main(void)
{
    char parent[1024];
    gptps_config cfg;
    gptps *e = NULL;
    gptps_task_def d;
    gptps_handle h;

    if (!setup_parent(parent, sizeof parent)) {
        printf("cgroup test skipped (no cgroup v2 memory delegation)\n");
        return 0;
    }

    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 2; cfg.limits.max_memory_bytes = 256u << 20;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (e) {
        gptps_set_event_cb(e, on_ev, NULL);
        def_oop(&d, "hog",   task_hog);   CHECK(gptps_register_task(e, &d) == GPTPS_OK);
        def_oop(&d, "small", task_small); CHECK(gptps_register_task(e, &d) == GPTPS_OK);

        CHECK(gptps_submit(e, "small", NULL, 0, &h) == GPTPS_OK);
        CHECK(gptps_submit(e, "hog",   NULL, 0, &h) == GPTPS_OK);
        gptps_shutdown(e);

        CHECK(get(&c_small_ok) == 1);   /* under-cap task finished */
        CHECK(get(&c_hog_nomem) == 1);  /* over-cap task OOM-killed -> GPTPS_E_NOMEM */
        CHECK(get(&c_hog_cap_hit) == 1);     /* ...and its measurements say it hit the cap */
        CHECK(get(&c_small_measured) == 1);  /* the other did not, and has its peak */
        CHECK(get(&c_spin_unthrottled) == 1); /* no CPU cap: no throttling figure at all */
    }

    if (!cpu_delegated(parent)) {
        printf("cgroup CPU-cap check skipped (cpu controller not delegated)\n");
    } else {
        e = NULL;
        CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
        if (e) {
            gptps_set_event_cb(e, on_ev, NULL);
            def_oop(&d, "spin", task_spin);
            d.default_cost.mem_bytes = 0;            /* a CPU cap alone still makes a cgroup */
            d.cpu_max_millicpus = 50;                /* 5% of one CPU */
            CHECK(gptps_register_task(e, &d) == GPTPS_OK);
            CHECK(gptps_submit(e, "spin", NULL, 0, &h) == GPTPS_OK);
            gptps_shutdown(e);
            CHECK(get(&c_spin_ok) == 1);
            CHECK(get(&c_spin_throttled) == 1);      /* held back by cpu.max, and measured */
        }
    }

    if (!io_delegated(parent)) {
        printf("cgroup I/O-cap check skipped (io controller not delegated)\n");
    } else {
        e = NULL;
        CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
        if (e) {
            gptps_set_event_cb(e, on_ev, NULL);
            def_oop(&d, "ioread", task_ioread);
            d.default_cost.mem_bytes = 0;            /* an I/O cap alone still makes a cgroup */
            d.io_max_read_bps  = 2u << 20;           /* 2 MiB/s */
            d.io_max_write_bps = 1u << 20;           /* 1 MiB/s */
            CHECK(gptps_register_task(e, &d) == GPTPS_OK);
            CHECK(gptps_submit(e, "ioread", NULL, 0, &h) == GPTPS_OK);
            gptps_shutdown(e);
            CHECK(get(&c_io_ok) == 1);
            CHECK(get(&c_io_capped) == 1);           /* the job's own cgroup holds both limits */
        }
    }

    cgroup_removal(parent, &cfg);

    if (g_owned) rmdir(parent); /* GPTPS removed its per-task children already */
    if (fails) { printf("%d cgroup check(s) FAILED\n", fails); return 1; }
    printf("all cgroup checks passed\n");
    return 0;
}
