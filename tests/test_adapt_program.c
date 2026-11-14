/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_adapt_program.c - addons/gptps_adapt with EXTERNAL PROGRAM jobs, so on every
 * platform: Windows has no GPTPS_EXEC_OOP, and test_adapt is POSIX-only for that.
 *
 *  1) Real jobs: prog_helper takes 64 MiB, resident and committed. After min_samples
 *     attempts the suggestion is the measured peak plus the headroom, a whole number
 *     of MiB, and gptps_adapt_apply writes it to tasks.<t>.mem_bytes. The peak's
 *     method is the platform's own (a job object's on Windows, wait4 on POSIX).
 *  2) Synthetic events, through the add-on's own observer (this file includes
 *     gptps_adapt.c, so `obs` is in reach): a peak measured another way is counted in
 *     other_method and never folded in (MEASUREMENTS.md: never mix methods), and a
 *     mem.cap_hit grows the suggestion from the declaration that was hit, at once.
 *
 * The jobs declare 8 MiB, below the executors' 16 MiB cap floor, so no cap is set
 * that a sanitizer runtime's reservations could hit (as in test_measure).
 */
#include "gptps_adapt.c"     /* the add-on itself: its observer is static */
#include <stdio.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

#define MiB (1024ull * 1024ull)

static uint64_t setting(gptps *e, const char *key)
{
    char buf[64];
    if (gptps_settings_get(e, key, buf, sizeof buf) != GPTPS_OK) return 0;
    return strtoull(buf, NULL, 10);
}

/* A FINISHED for `task` declaring `declared`, carrying the given measurements. */
static void feed(gptps_adapt *a, const char *task, uint64_t declared, const gptps_measure *m, size_t n)
{
    gptps_event ev;
    memset(&ev, 0, sizeof ev);
    ev.struct_size = sizeof ev;
    ev.kind = GPTPS_EV_FINISHED; ev.task_name = task; ev.status = GPTPS_OK;
    ev.mem_bytes = declared; ev.measures = m; ev.n_measures = n;
    obs(&ev, a);
}

int main(void)
{
    static const char *const av[] = { HELPER_PATH, "mem", "64", "50", NULL };
    gptps_config cfg; gptps *e = NULL; gptps_task_def d;
    gptps_adapt_config c; gptps_adapt *a; gptps_adapt_report r;
    size_t ran; int i;

    /* 1) real program jobs */
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1;
    cfg.limits.max_memory_bytes = 64ull * 1024 * MiB;
    cfg.mode = GPTPS_RUN_MANUAL;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    if (!e) return 1;
    memset(&c, 0, sizeof c);
    c.struct_size = sizeof c; c.min_samples = 3;
    a = gptps_adapt_open(e, &c);
    CHECK(a != NULL);
    if (!a) return 1;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = "mem64"; d.exec = GPTPS_EXEC_PROGRAM; d.argv = av;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = 8 * MiB;
    d.default_policy.struct_size = sizeof d.default_policy; d.default_policy.timeout_seconds = 60;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
    for (i = 0; i < 3; ++i) CHECK(gptps_submit(e, "mem64", NULL, 0, NULL) == GPTPS_OK);
    while (gptps_step(e, &ran) == GPTPS_OK && ran) { }

    memset(&r, 0, sizeof r); r.struct_size = sizeof r;
    CHECK(gptps_adapt_get(a, "mem64", &r) == GPTPS_OK);
    printf("mem64: %llu samples, peak %llu bytes (%s), suggested %llu\n",
           (unsigned long long)r.samples, (unsigned long long)r.peak, r.method, (unsigned long long)r.suggested);
    CHECK(r.samples == 3);
    CHECK(r.peak >= 64 * MiB);
    CHECK(r.method[0] != 0);
    CHECK(r.suggested >= r.peak + r.peak / 4 && r.suggested < r.peak + r.peak / 4 + MiB);
    CHECK(r.suggested % MiB == 0);
    CHECK(gptps_adapt_apply(a, "mem64") == GPTPS_OK);
    CHECK(setting(e, "tasks.mem64.mem_bytes") == r.suggested);

    /* 2a) a peak measured another way: counted aside, never folded in */
    {
        gptps_measure m;
        uint64_t before = r.peak;
        memset(&m, 0, sizeof m);
        m.name = GPTPS_M_MEM_PEAK; m.value = 900 * MiB; m.unit = GPTPS_UNIT_BYTES;
        m.kind = GPTPS_MEASURE_PEAK; m.method = "some.other.method";
        feed(a, "mem64", 8 * MiB, &m, 1);
        CHECK(gptps_adapt_get(a, "mem64", &r) == GPTPS_OK);
        CHECK(r.other_method == 1);
        CHECK(r.peak == before);
        CHECK(r.samples == 3);
    }
    /* 2b) a cap hit, in a fresh task: grown from what was hit, without waiting */
    {
        gptps_measure m[2];
        memset(m, 0, sizeof m);
        m[0].name = GPTPS_M_MEM_PEAK;    m[0].value = 32 * MiB; m[0].unit = GPTPS_UNIT_BYTES;
        m[0].kind = GPTPS_MEASURE_PEAK;  m[0].method = "cgroup.job.resident";
        m[1].name = GPTPS_M_MEM_CAP_HIT; m[1].value = 1;        m[1].unit = GPTPS_UNIT_FLAG;
        m[1].kind = GPTPS_MEASURE_FLAG;  m[1].method = "cgroup.job";
        feed(a, "hit", 32 * MiB, m, 2);
        CHECK(gptps_adapt_get(a, "hit", &r) == GPTPS_OK);
        CHECK(r.cap_hits == 1 && r.hit_declared == 32 * MiB);
        CHECK(r.suggested == 48 * MiB);                  /* 32 MiB + 50%, at once */
    }

    CHECK(gptps_shutdown(e) == GPTPS_OK);
    gptps_adapt_close(a);
    if (fails) { printf("%d adapt (program) check(s) FAILED\n", fails); return 1; }
    printf("adapt (program): all checks passed\n");
    return 0;
}
