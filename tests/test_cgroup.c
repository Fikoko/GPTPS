/*
 * test_cgroup.c - accurate memory enforcement via cgroup v2 (Linux).
 *
 * Prepares a memory-delegated parent cgroup, points GPTPS at it via
 * GPTPS_CGROUP_PARENT, then runs out-of-process tasks: one that blows past its
 * memory cap (must be OOM-killed and reported GPTPS_E_NOMEM) and one that stays
 * under it (must finish). SKIPS (exit 0) where cgroup v2 delegation is absent
 * (CI runners, macOS) - there the executor falls back to RLIMIT_AS, covered by
 * test_oop.
 */
#define _POSIX_C_SOURCE 200809L
#include "gptps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int inc(int *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static int c_hog_nomem, c_small_ok;

static void on_ev(const gptps_event *ev, void *ud)
{
    (void)ud;
    if (ev->kind == GPTPS_EV_FAILED && ev->status == GPTPS_E_NOMEM &&
        strcmp(ev->task_name, "hog") == 0) inc(&c_hog_nomem);
    if (ev->kind == GPTPS_EV_FINISHED && strcmp(ev->task_name, "small") == 0) inc(&c_small_ok);
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

static int cg_write(const char *dir, const char *file, const char *val)
{
    char path[1024]; FILE *f;
    snprintf(path, sizeof path, "%s/%s", dir, file);
    f = fopen(path, "w");
    if (!f) return -1;
    fputs(val, f);
    return fclose(f) == 0 ? 0 : -1;
}

/* Find the systemd user-delegation boundary and prepare a memory-enabled parent
 * cgroup under it. Returns 1 (sets GPTPS_CGROUP_PARENT + fills `parent`) or 0. */
static int setup_parent(char *parent, size_t pn)
{
    char line[1024], base[1024], probe[1088];
    char *nl, *at, *svc;
    FILE *f = fopen("/proc/self/cgroup", "r");
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
    snprintf(base, sizeof base, "/sys/fs/cgroup%s", line + 3);

    snprintf(parent, pn, "%s/gptps_test_%ld", base, (long)getpid());
    if (mkdir(parent, 0700) != 0) return 0;      /* no delegation here -> skip */
    if (cg_write(parent, "cgroup.subtree_control", "+memory") != 0) { rmdir(parent); return 0; }
    /* verify a child can actually take a memory cap */
    snprintf(probe, sizeof probe, "%s/_probe", parent);
    if (mkdir(probe, 0700) != 0) { rmdir(parent); return 0; }
    if (cg_write(probe, "memory.max", "33554432") != 0) { rmdir(probe); rmdir(parent); return 0; }
    rmdir(probe);

    setenv("GPTPS_CGROUP_PARENT", parent, 1);
    return 1;
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
    }

    rmdir(parent); /* GPTPS removed its per-task children already */
    if (fails) { printf("%d cgroup check(s) FAILED\n", fails); return 1; }
    printf("all cgroup checks passed\n");
    return 0;
}
