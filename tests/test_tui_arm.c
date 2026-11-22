/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_tui_arm.c - the dashboard's ON-VIEW ARMING (1.13; gptps_tui.h, ARM at
 * gptps_tui_install) and its install HINT, with the add-on compiled in, EVERY allocation it
 * makes interposed (each block carries its size) and the dashboard's clock faked. MANUAL
 * engine; events are synthetic (the observer called directly).
 *
 *  1) ALWAYS (the default) is 1.12: armed from install, no "recording since" line, nothing
 *     armed or disarmed by views or health.
 *  2) ON_VIEW: installed DISARMED - only the totals and gauges run (health.kpi GAUGE, the
 *     request FULL), the footprint is the formula at GAUGE, events are counted in the totals and
 *     nowhere else, and the event path allocates nothing.
 *  3) the first view arms: render, snapshot or a key - the request is built (footprint = the
 *     formula at FULL), the frame says "recording since first view (... ago)"; the event path
 *     still allocates nothing once armed.
 *  4) a disarm happens only at health, disarm_ms after the last view: not one ms before, never
 *     at a frame (a frame is a view); it frees exactly what it armed (the allocator agrees);
 *     PROBLEMS records no frame showed are counted as not shown; the next view re-arms.
 *  5) a live loop is a view while it runs (no disarm, however long), and disarm_ms counts from
 *     when it returns (POSIX, a pty; not on the simulation HAL: the loop waits in poll()).
 *  6) with DEGRADE: a shed survives a disarm and an arm (the arm builds the request less the
 *     shed), no tier step is shed while disarmed, and 'u' restores the request.
 *  7) with BUDGET: install and every check are against the request, so an arm is never refused
 *     and never passes the budget; a budget below the request is refused while disarmed too.
 *  8) OOM on arm: counted (arm_failures, alloc_failures), shown ("no memory for:"), WARNed; the
 *     next arm builds it.
 *  9) setters, settings (tui.arm, tui.disarm_ms: readback, choices, range), install refusals,
 *     set_arm(always) arms at once, set_arm(on_view) counts as a view.
 * 10) the header line follows the clock's unit (and says a fixed point with clock OFF).
 * 11) HINT: FULL with no terminal logs one WARN at install (not at a lower tier, not with
 *     on_view); gptps_tui_run with interactive 0 says so - once - and counts every call; with
 *     no terminal it says so at most once a minute, and counts every call.
 * Throughout 3) and 4): an arm (render, snapshot, key) and a disarm (health) allocate nothing
 * while the dashboard's lock is held (TUI_LOCK_HOOK follows it).
 */
#if !defined(_WIN32)
#  define _XOPEN_SOURCE 700         /* posix_openpt & co. */
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "gptps.h"

/* ---- the interposed allocator: a header before each block holds its size ---- */
typedef union { size_t n; long double ld; void *p; uint64_t u; } ta_hdr;
static size_t ta_live;              /* bytes the add-on holds now */
static size_t ta_peak;              /* the most it held (set ta_peak = ta_live to start) */
static long   ta_calls;             /* allocations made (the event path must make none) */
static size_t ta_fail_n;            /* !0: the next allocation of exactly this many bytes fails (once) */
/* the dashboard's lock `mu` (tl_watch: &t->mu; its other locks, smu and rmu, may be held
 * while allocating), followed through TUI_LOCK_HOOK: how deep this thread holds it, and the
 * allocations made while it did (arming builds unlocked and swaps under the lock: none) */
#if defined(__GNUC__) || defined(__clang__)
static __thread int tl_held;
#else
static int tl_held;
#endif
static long ta_locked;
static const void *tl_watch;
#define TUI_LOCK_HOOK(m, d) ((void)((const void *)(m) == tl_watch ? (tl_held += (d)) : 0))
static void *ta_malloc(size_t n)
{
    ta_hdr *h;
    ta_calls++;
    if (tl_held > 0) ta_locked++;
    if (ta_fail_n && n == ta_fail_n) { ta_fail_n = 0; return NULL; }
    h = (ta_hdr *)malloc(sizeof *h + n);
    if (!h) return NULL;
    h->n = n; ta_live += n; if (ta_live > ta_peak) ta_peak = ta_live;
    return h + 1;
}
static void *ta_calloc(size_t k, size_t s)
{
    void *p;
    if (s && k > (size_t)-1 / s) return NULL;
    p = ta_malloc(k * s);
    if (p) memset(p, 0, k * s);
    return p;
}
static void ta_free(void *p)
{
    ta_hdr *h;
    if (!p) return;
    h = (ta_hdr *)p - 1;
    ta_live -= h->n;
    free(h);
}
static void *ta_realloc(void *p, size_t n)
{
    ta_hdr *h, *nh;
    if (!p) return ta_malloc(n);
    h = (ta_hdr *)p - 1;
    ta_calls++;
    if (tl_held > 0) ta_locked++;
    if (ta_fail_n && n == ta_fail_n) { ta_fail_n = 0; return NULL; }
    nh = (ta_hdr *)realloc(h, sizeof *nh + n);
    if (!nh) return NULL;
    ta_live -= nh->n; nh->n = n; ta_live += n; if (ta_live > ta_peak) ta_peak = ta_live;
    return nh + 1;
}
/* the dashboard's clock: the test sets it (never 0 here, so never the real one) */
static uint64_t tui_fake_ms;
#if defined(__GNUC__) || defined(__clang__)
/* read and moved atomically: the live-loop test (5) moves it while gptps_tui_run reads it */
static uint64_t fake_now(void) { return __atomic_load_n(&tui_fake_ms, __ATOMIC_SEQ_CST); }
static void fake_add(uint64_t d) { (void)__atomic_add_fetch(&tui_fake_ms, d, __ATOMIC_SEQ_CST); }
#else
static uint64_t fake_now(void) { return tui_fake_ms; }
static void fake_add(uint64_t d) { tui_fake_ms += d; }
#endif
#define TUI_NOW_MS() (fake_now() ? fake_now() : gptps_now_ms(NULL))
#define TUI_FRAME_US(us) (tui_fake_ms ? 0u : (us))
#define malloc(n) ta_malloc((n))
#define calloc(n, s) ta_calloc((n), (s))
#define realloc(p, n) ta_realloc((p), (n))
#define free(p) ta_free((p))
#include "../addons/gptps_tui.c"
#undef malloc
#undef calloc
#undef realloc
#undef free

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static char logbuf[32768];
static size_t loglen;
static void cap_sink(gptps_log_level lvl, const char *msg, void *u)
{
    size_t n = strlen(msg);
    (void)u;
    if (lvl < GPTPS_LOG_WARN) return;
    if (getenv("TUI_ARM_LOG")) fprintf(stderr, "[log] %s\n", msg);
    if (loglen + n + 2 < sizeof logbuf) { memcpy(logbuf + loglen, msg, n); loglen += n; logbuf[loglen++] = '\n'; logbuf[loglen] = 0; }
}
static void log_begin(void) { loglen = 0; logbuf[0] = 0; gptps_set_log_sink(cap_sink, NULL); }
static void log_end(void)   { gptps_set_log_sink(NULL, NULL); }
static int log_count(const char *needle)
{
    int c = 0;
    const char *p = logbuf;
    while ((p = strstr(p, needle)) != NULL) { ++c; p += strlen(needle); }
    return c;
}

static gptps *open_manual(void)
{
    gptps_config cfg; gptps *e = NULL;
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1; cfg.limits.max_memory_bytes = 1u << 20;
    cfg.mode = GPTPS_RUN_MANUAL;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    return e;
}
static void base_cfg(gptps_tui_config *c, int kpi, int arm)
{
    memset(c, 0, sizeof *c);
    c->struct_size = sizeof *c; c->color = 0; c->interactive = 0; c->unicode = 0;
    c->kpi = kpi; c->arm = arm; c->clock = GPTPS_TUI_CLOCK_MINUTES;
}
static gptps_tui_health_info health(gptps_tui *t)
{
    gptps_tui_health_info h;
    memset(&h, 0, sizeof h); h.struct_size = sizeof h;
    CHECK(gptps_tui_health(t, &h) == GPTPS_OK);
    return h;
}
static void emit(gptps_tui *t, int kind, const char *task, gptps_handle h)
{
    gptps_event ev;
    memset(&ev, 0, sizeof ev);
    ev.struct_size = sizeof ev; ev.kind = (gptps_event_kind)kind; ev.task_name = task; ev.handle = h;
    ev.status = kind == GPTPS_EV_FAILED ? GPTPS_E_TASK : GPTPS_OK; ev.ts_ms = tui_fake_ms;
    tui_on_event(&ev, t);
}
/* a lifecycle of `n` items and one failure each, from handle h0 */
static void burst(gptps_tui *t, gptps_handle h0, int n)
{
    int k;
    for (k = 0; k < n; ++k) {
        gptps_handle h = h0 + (gptps_handle)k;
        emit(t, GPTPS_EV_QUEUED, "a", h); emit(t, GPTPS_EV_STARTED, "a", h);
        if (k % 2) emit(t, GPTPS_EV_FAILED, "a", h); else emit(t, GPTPS_EV_FINISHED, "a", h);
    }
}

/* ---- the formula, as gptps_tui.h (MEMORY) writes it, at the defaults' sizes ---- */
static uint64_t np2(uint64_t n) { uint64_t p = 1; while (p < n) p <<= 1; return p; }
static uint64_t formula(int tier, int act_shed)
{
    const uint64_t D = sizeof(gptps_tui), R = sizeof(tui_task), E = sizeof(tui_event), A = sizeof(tui_act),
                   S = sizeof(tui_slot);
    uint64_t f = D;
    if (tier >= TUI_T_TASKS) f += R * 65u + 4u * np2(128u) + 2048u;
    if (tier >= TUI_T_PROBLEMS) f += E * 8u;
    if (tier >= TUI_T_ACTIVITY && !act_shed) f += A * 8u;
    if (tier >= TUI_T_LATENCY) f += S * np2((1024u + 64u) * 4u / 3u + 1u);
    if (tier >= TUI_T_FULL) f += 256u * 65u;
    return f;
}
static uint64_t view_bytes(int R) { return sizeof(tui_view) + (uint64_t)R * (sizeof(tui_vrow) + sizeof(tui_event)); }

static char frame[65536];

/* 1) */
static void test_always(void)
{
    gptps *e = open_manual();
    gptps_tui *t;
    gptps_tui_config c;
    gptps_tui_health_info h;
    if (!e) return;
    tui_fake_ms = 1000000;
    base_cfg(&c, GPTPS_TUI_KPI_FULL, 0);                  /* 0 = ALWAYS */
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    h = health(t);
    CHECK(h.arm == GPTPS_TUI_ARM_ALWAYS && h.armed == 1 && h.kpi == GPTPS_TUI_KPI_FULL && h.disarm_ms == 60000);
    CHECK(h.footprint_bytes == formula(TUI_T_FULL, 0) && h.arms == 0 && h.disarms == 0);
    fake_add(5000);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "recording since") == NULL);      /* the record is the session */
    tui_fake_ms += 10u * 60000u;                          /* long unseen: nothing disarms ALWAYS */
    h = health(t);
    CHECK(h.armed == 1 && h.kpi == GPTPS_TUI_KPI_FULL && h.disarms == 0 && h.record_ms == 10u * 60000u + 5000u);
    CHECK(h.views == 1 && h.view_age_ms == 10u * 60000u);
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 2) 3) 4) */
static void test_on_view(void)
{
    gptps *e = open_manual();
    gptps_tui *t;
    gptps_tui_config c;
    gptps_tui_health_info h;
    long calls;
    size_t live, armed_live;
    FILE *snap;
    if (!e) return;
    tui_fake_ms = 2000000;
    base_cfg(&c, GPTPS_TUI_KPI_FULL, GPTPS_TUI_ARM_ON_VIEW);
    c.disarm_ms = 30000;
    snap = tmpfile();
    c.out = snap;
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    t->cols = 120;                                        /* the ARM line whole (it is fitted to the width) */
    tl_watch = &t->mu;
    /* 2) disarmed: totals and gauges only */
    h = health(t);
    CHECK(h.arm == GPTPS_TUI_ARM_ON_VIEW && h.armed == 0 && h.kpi == GPTPS_TUI_KPI_GAUGE &&
          h.kpi_requested == GPTPS_TUI_KPI_FULL && h.record_ms == 0 && h.views == 0 && h.disarm_ms == 30000);
    CHECK(h.footprint_bytes == formula(TUI_T_GAUGE, 0) && h.footprint_request_bytes == formula(TUI_T_FULL, 0));
    CHECK(ta_live == h.footprint_bytes);                  /* the allocator agrees: nothing else is held */
    {   /* the setting reads back the request, unchanged */
        char v[64];
        CHECK(gptps_settings_get(e, "tui.kpi", v, sizeof v) == GPTPS_OK && strcmp(v, "full") == 0);
    }
    calls = ta_calls;
    burst(t, 1, 50);
    CHECK(ta_calls == calls);                             /* the event path allocates nothing */
    h = health(t);
    CHECK(h.problems_held == 0 && h.activity_held == 0 && h.task_rows == 0 && h.handles_tracked == 0);
    CHECK(t->fin == 25 && t->fail == 25 && t->q == 50);   /* the totals always run */
    CHECK(h.gauges == GPTPS_TUI_GAUGES_APPROX);
    /* 3) the first view arms - building the request with the lock let go */
    tui_fake_ms += 5000;
    ta_locked = 0;
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(ta_locked == 0);                                /* nothing allocated under the lock */
    h = health(t);
    CHECK(h.armed == 1 && h.kpi == GPTPS_TUI_KPI_FULL && h.arms == 1 && h.views == 1 && h.record_ms == 0);
    CHECK(h.footprint_bytes == formula(TUI_T_FULL, 0));
    CHECK(ta_live == h.footprint_bytes + h.runtime_bytes && h.runtime_bytes == view_bytes(24));
    CHECK(strstr(frame, "recording since first view (<1m ago) - tui.arm=on_view, off 30.0 s after the last view\n") != NULL);
    CHECK(strstr(frame, "kpi:full") != NULL);
    armed_live = ta_live;
    calls = ta_calls;
    burst(t, 100, 10);
    CHECK(ta_calls == calls);                             /* armed: still nothing on the event path */
    h = health(t);
    CHECK(h.problems_held == 5 && h.task_rows == 1 && t->fin == 30);
    /* 4) not one ms early; never at a frame */
    tui_fake_ms += 29999;
    CHECK(health(t).armed == 1);
    tui_fake_ms += 100000;                                /* long past disarm_ms ... */
    gptps_tui_render(t, frame, sizeof frame);             /* ... but a frame is a view: still armed */
    h = health(t);
    CHECK(h.armed == 1 && h.disarms == 0 && h.views == 2 && h.view_age_ms == 0);
    burst(t, 200, 4);                                     /* 2 failures no frame shows */
    tui_fake_ms += 30000;
    live = ta_live;
    ta_locked = 0;
    h = health(t);                                        /* disarm_ms since the last view: disarmed */
    CHECK(ta_locked == 0);
    CHECK(h.armed == 0 && h.disarms == 1 && h.kpi == GPTPS_TUI_KPI_GAUGE && h.record_ms == 0);
    CHECK(h.footprint_bytes == formula(TUI_T_GAUGE, 0));
    CHECK(live - ta_live == formula(TUI_T_FULL, 0) - formula(TUI_T_GAUGE, 0));   /* frees exactly what it armed */
    CHECK(ta_live == h.footprint_bytes + h.runtime_bytes);
    CHECK(h.problems_not_shown == 2);                     /* never silent: counted */
    CHECK(health(t).disarms == 1);                        /* once */
    calls = ta_calls;
    burst(t, 300, 6);
    CHECK(ta_calls == calls && health(t).problems_held == 0);
    /* re-armed by the next view: a snapshot this time */
    tui_fake_ms += 60000;
    ta_locked = 0;
    gptps_tui_snapshot(t);
    CHECK(ta_locked == 0);                                /* the snapshot's arm: nothing under the lock */
    h = health(t);
    CHECK(h.armed == 1 && h.arms == 2 && h.footprint_bytes == formula(TUI_T_FULL, 0) && ta_live == armed_live);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "tui.arm=on_view, off 30.0 s after the last view, 1 disarm so far\n") != NULL);
    tui_fake_ms += 30000;
    CHECK(health(t).armed == 0 && health(t).disarms == 2);
    ta_locked = 0;
    CHECK(gptps_tui_press(t, 'j') == 2);                  /* and a key */
    CHECK(ta_locked == 0);
    CHECK(health(t).armed == 1 && health(t).arms == 3);
    tl_watch = NULL;
    gptps_shutdown(e);
    gptps_tui_close(t);
    if (snap) fclose(snap);
    CHECK(ta_live == 0);
}

/* 5) */
#if !defined(_WIN32) && !defined(GPTPS_TEST_HAL_SIM)
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/select.h>
static gptps_tui *run_t;
static int run_done;
static void *run_main(void *p) { (void)p; gptps_tui_run(run_t); __atomic_store_n(&run_done, 1, __ATOMIC_SEQ_CST); return NULL; }
static void test_run_is_a_view(void)
{
    gptps *e;
    gptps_tui_config c;
    pthread_t th;
    struct winsize ws;
    int m = posix_openpt(O_RDWR | O_NOCTTY), s, in0;
    FILE *out;
    char b[4096];
    if (m < 0 || grantpt(m) != 0 || unlockpt(m) != 0 || (s = open(ptsname(m), O_RDWR | O_NOCTTY)) < 0) {
        printf("note: no pty here; the run() view check skipped\n");
        if (m >= 0) close(m);
        return;
    }
    memset(&ws, 0, sizeof ws); ws.ws_row = 24; ws.ws_col = 80;
    ioctl(s, TIOCSWINSZ, &ws);
    fcntl(m, F_SETFL, fcntl(m, F_GETFL) | O_NONBLOCK);
    in0 = dup(0);
    out = fdopen(s, "w");
    if (!out || in0 < 0 || dup2(s, 0) < 0) { printf("note: cannot set up; skipped\n"); close(m); return; }
    if ((e = open_manual()) == NULL) return;
    tui_fake_ms = 3000000;
    log_begin();                                          /* HINT: a terminal both ways - quiet */
    base_cfg(&c, GPTPS_TUI_KPI_FULL, GPTPS_TUI_ARM_ALWAYS);
    c.out = out;
    run_t = gptps_tui_install(e, &c);
    CHECK(run_t != NULL && log_count("no terminal to run") == 0);
    log_end();
    gptps_shutdown(e);
    gptps_tui_close(run_t);
    if ((e = open_manual()) == NULL) return;
    /* BUDGET: run makes its two 16 KiB buffers before the frame that arms - checked against the
     * request, so with room for the request + 8 KiB they are stopped (as armed) and the peak
     * never passes the budget */
    base_cfg(&c, GPTPS_TUI_KPI_FULL, GPTPS_TUI_ARM_ON_VIEW);
    c.interactive = 1; c.out = out; c.budget_kb = (int)((formula(TUI_T_FULL, 0) + 1023u) / 1024u) + 8;
    run_t = gptps_tui_install(e, &c);
    CHECK(run_t != NULL);
    if (run_t) {
        int k;
        log_begin();
        ta_peak = ta_live;
        run_done = 0;
        if (pthread_create(&th, NULL, run_main, NULL) == 0) {
            for (k = 0; k < 2000 && !__atomic_load_n(&run_done, __ATOMIC_SEQ_CST); ++k) {   /* <= 10 s */
                struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 5000;
                select(0, NULL, NULL, NULL, &tv);
                while (read(m, b, sizeof b) > 0) { }
            }
            CHECK(__atomic_load_n(&run_done, __ATOMIC_SEQ_CST));   /* it returned: no buffer */
            tui_quit(run_t);                              /* (else: it ran - stop it) */
            pthread_join(th, NULL);
            while (read(m, b, sizeof b) > 0) { }
        }
        log_end();
        CHECK(ta_peak <= health(run_t).budget_bytes && health(run_t).budget_stops >= 1 && health(run_t).armed == 0);
        gptps_shutdown(e);
        gptps_tui_close(run_t);
    }
    if ((e = open_manual()) == NULL) return;
    base_cfg(&c, GPTPS_TUI_KPI_FULL, GPTPS_TUI_ARM_ON_VIEW);
    c.interactive = 1; c.out = out; c.mode = GPTPS_TUI_PAUSED;   /* idle: no frame after the first */
    run_t = gptps_tui_install(e, &c);
    CHECK(run_t != NULL);
    if (!run_t) { gptps_shutdown(e); return; }
    CHECK(health(run_t).armed == 0);
    if (pthread_create(&th, NULL, run_main, NULL) == 0) {
        alarm(60);
        while (health(run_t).frames_painted == 0) {
            struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 5000;
            select(0, NULL, NULL, NULL, &tv);
            while (read(m, b, sizeof b) > 0) { }
        }
        CHECK(health(run_t).armed == 1 && health(run_t).arms == 1);
        fake_add(10u * 60000u);                      /* ten disarm_ms, the loop idle in PAUSED */
        CHECK(health(run_t).armed == 1 && health(run_t).disarms == 0);   /* it is a view while it runs */
        tui_quit(run_t);                                  /* as 'q' does, but no key: a key is a view itself */
        pthread_join(th, NULL);
        alarm(0);
        while (read(m, b, sizeof b) > 0) { }
        fake_add(59999);                             /* disarm_ms counts from its return */
        CHECK(health(run_t).armed == 1);
        fake_add(1);
        CHECK(health(run_t).armed == 0 && health(run_t).disarms == 1);
    }
    gptps_shutdown(e);
    gptps_tui_close(run_t);
    dup2(in0, 0); close(in0);
    fclose(out);
    close(m);
}
#endif

/* 6) */
static void test_degrade(void)
{
    gptps *e = open_manual();
    gptps_tui *t;
    gptps_tui_config c;
    gptps_tui_health_info h;
    char what[400];
    int chosen = -1;
    if (!e) return;
    tui_fake_ms = 4000000;
    base_cfg(&c, GPTPS_TUI_KPI_FULL, GPTPS_TUI_ARM_ON_VIEW);
    c.adapt = GPTPS_TUI_ADAPT_DEGRADE;
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    /* disarmed: no tier step would change what runs, so DEGRADE has none to take */
    mu_lock(&t->mu);
    CHECK(shed_next(t, GPTPS_TUI_P_COST | GPTPS_TUI_P_RENDER | GPTPS_TUI_P_NOMEM | GPTPS_TUI_P_UNTRACKED, &chosen) == 0);
    mu_unlock(&t->mu);
    tui_fake_ms += 15000;                                 /* past a rate window (10 s) since install */
    gptps_tui_render(t, frame, sizeof frame);             /* armed - and the window starts again with it */
    CHECK(health(t).windows == 0 && t->ad_ms == tui_fake_ms);
    mu_lock(&t->mu);                                      /* the step tui_adapt takes: -activity -percentiles */
    CHECK(shed_next(t, GPTPS_TUI_P_ACTIVITY_LOST, &chosen) == GPTPS_TUI_SHED_ACTIVITY);
    t->shed |= GPTPS_TUI_SHED_ACTIVITY | GPTPS_TUI_SHED_PERCENTILES; t->shed_why = t->shed_whys = GPTPS_TUI_P_RENDER;
    mu_unlock(&t->mu);
    CHECK(tui_reconfig(t, -1, 0, -1, -1, 0, what, sizeof what) == GPTPS_OK);
    h = health(t);
    CHECK(h.kpi == GPTPS_TUI_KPI_LATENCY && h.footprint_bytes == formula(TUI_T_LATENCY, 1));
    tui_fake_ms += 60000;
    h = health(t);                                        /* disarmed: the shed is kept */
    CHECK(h.armed == 0 && h.kpi == GPTPS_TUI_KPI_GAUGE &&
          h.shed == (GPTPS_TUI_SHED_ACTIVITY | GPTPS_TUI_SHED_PERCENTILES));
    tui_fake_ms += 1000;
    gptps_tui_render(t, frame, sizeof frame);             /* re-armed: the request LESS the shed */
    h = health(t);
    CHECK(h.armed == 1 && h.kpi == GPTPS_TUI_KPI_LATENCY && h.shed == (GPTPS_TUI_SHED_ACTIVITY | GPTPS_TUI_SHED_PERCENTILES));
    CHECK(h.footprint_bytes == formula(TUI_T_LATENCY, 1) && h.activity_held == 0 && t->act == NULL && t->hist == NULL);
    CHECK(strstr(frame, "DEGRADED: -activity -percentiles") != NULL && strstr(frame, "recording since first view") != NULL);
    CHECK(gptps_tui_press(t, 'u') == 3);                  /* only the operator restores */
    h = health(t);
    CHECK(h.shed == 0 && h.kpi == GPTPS_TUI_KPI_FULL && h.armed == 1 && h.footprint_bytes == formula(TUI_T_FULL, 0));
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 7) */
static void test_budget(void)
{
    gptps *e = open_manual();
    gptps_tui *t;
    gptps_tui_config c;
    gptps_tui_health_info h;
    int kb = (int)((formula(TUI_T_FULL, 0) + view_bytes(24) + 1023u) / 1024u);
    if (!e) return;
    tui_fake_ms = 5000000;
    base_cfg(&c, GPTPS_TUI_KPI_FULL, GPTPS_TUI_ARM_ON_VIEW);
    c.budget_kb = (int)((formula(TUI_T_FULL, 0) + 1023u) / 1024u) - 1;   /* under the request: refused */
    log_begin();
    CHECK(gptps_tui_install(e, &c) == NULL);              /* checked against the request, not GAUGE */
    CHECK(log_count("the dashboard was not installed") == 1);
    log_end();
    c.budget_kb = kb;                                     /* the request and a 24-row view fit */
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    h = health(t);
    CHECK(h.footprint_bytes == formula(TUI_T_GAUGE, 0) && h.footprint_request_bytes == formula(TUI_T_FULL, 0));
    CHECK(gptps_tui_set_budget_kb(t, (int)(formula(TUI_T_GAUGE, 0) / 1024u) + 1) == GPTPS_E_INVAL);  /* disarmed too */
    CHECK(gptps_tui_set_latency_window(t, 4096) == GPTPS_E_INVAL);   /* the request would grow past it */
    CHECK(health(t).budget_refusals == 2);
    tui_fake_ms += 1000;
    gptps_tui_render(t, frame, sizeof frame);             /* the arm: never refused, within the budget */
    h = health(t);
    CHECK(h.armed == 1 && h.budget_refusals == 2 && h.budget_stops == 0 && h.kpi == GPTPS_TUI_KPI_FULL);
    CHECK(h.footprint_bytes + h.runtime_bytes <= h.budget_bytes && ta_live == h.footprint_bytes + h.runtime_bytes);
    gptps_shutdown(e);
    gptps_tui_close(t);
    {   /* a snapshot makes its 16 KiB buffer BEFORE the frame that arms: disarmed, the buffer is
         * checked against the request (what the arm builds), so the peak never passes the budget */
        FILE *o = tmpfile();
        if (!o) { printf("note: no tmpfile here; the snapshot budget check skipped\n"); return; }
        if ((e = open_manual()) == NULL) { fclose(o); return; }
        base_cfg(&c, GPTPS_TUI_KPI_FULL, GPTPS_TUI_ARM_ON_VIEW); c.out = o;
        c.budget_kb = (int)((formula(TUI_T_FULL, 0) + 1023u) / 1024u) + 8;   /* the request + 8 KiB < + 16 */
        t = gptps_tui_install(e, &c);
        CHECK(t != NULL);
        if (!t) { gptps_shutdown(e); fclose(o); return; }
        h = health(t);
        CHECK(h.armed == 0 && ta_live == h.footprint_bytes);
        log_begin();
        ta_peak = ta_live;
        tui_fake_ms += 1000;
        gptps_tui_snapshot(t);                            /* stopped, as it is armed: no frame */
        h = health(t);
        CHECK(ta_peak <= h.budget_bytes && h.budget_stops == 1 && h.armed == 0);
        CHECK(log_count("gptps_tui_snapshot's frame buffer") == 1);
        log_end();
        gptps_tui_render(t, frame, sizeof frame);        /* armed: the same snapshot is stopped the same */
        {
            uint64_t st0 = health(t).budget_stops;
            ta_peak = ta_live;
            gptps_tui_snapshot(t);
            h = health(t);
            CHECK(h.armed == 1 && h.budget_stops == st0 + 1 && ta_peak <= h.budget_bytes);
        }
        /* room for the request, the buffer and the view: it arms and makes its frame */
        CHECK(gptps_tui_set_budget_kb(t, (int)((formula(TUI_T_FULL, 0) + TUI_FRAME_MIN + view_bytes(24) + 1023u) /
                                               1024u) + 2) == GPTPS_OK);
        tui_fake_ms += 61000;
        CHECK(health(t).armed == 0);
        {
            uint64_t st0 = health(t).budget_stops;
            long at = ftell(o);
            ta_peak = ta_live;
            gptps_tui_snapshot(t);
            h = health(t);
            CHECK(h.armed == 1 && h.budget_stops == st0 && ta_peak <= h.budget_bytes &&
                  ta_peak > formula(TUI_T_FULL, 0) + TUI_FRAME_MIN && ftell(o) > at);
        }
        gptps_shutdown(e);
        gptps_tui_close(t);
        fclose(o);
    }
}

/* 8) */
static void test_arm_oom(void)
{
    gptps *e = open_manual();
    gptps_tui *t;
    gptps_tui_config c;
    gptps_tui_health_info h;
    size_t tab = trk_slots(1024) * sizeof(tui_slot);
    if (!e) return;
    tui_fake_ms = 6000000;
    base_cfg(&c, GPTPS_TUI_KPI_FULL, GPTPS_TUI_ARM_ON_VIEW);
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    log_begin();
    ta_fail_n = tab;                                      /* the handle table's allocation fails */
    tui_fake_ms += 1000;
    gptps_tui_render(t, frame, sizeof frame);
    h = health(t);
    CHECK(h.armed == 1 && h.arm_failures == 1 && h.alloc_failures == 1 && h.kpi == GPTPS_TUI_KPI_FULL);
    CHECK(h.bytes_handles == 0 && h.footprint_bytes == formula(TUI_T_FULL, 0) - tab);
    CHECK(strstr(frame, "no memory for: latency") != NULL);
    CHECK(log_count("tui.arm=on_view: arming on a view: no memory for the latency table") == 1);
    log_end();
    tui_fake_ms += 60000;
    CHECK(health(t).armed == 0);
    tui_fake_ms += 1000;
    gptps_tui_render(t, frame, sizeof frame);             /* the next arm builds it */
    h = health(t);
    CHECK(h.armed == 1 && h.arm_failures == 1 && h.bytes_handles == tab && h.footprint_bytes == formula(TUI_T_FULL, 0));
    CHECK(strstr(frame, "no memory for:") == NULL);
    /* arms come by themselves: an arm that keeps failing WARNs at most once a minute, and
     * every failure is counted */
    CHECK(gptps_tui_set_disarm_ms(t, 100) == GPTPS_OK);
    tui_fake_ms += 1000;
    CHECK(health(t).armed == 0);
    log_begin();
    {
        int k;
        for (k = 0; k < 20; ++k) {                        /* 2 s of arm / disarm, the table failing each time */
            ta_fail_n = tab;
            gptps_tui_render(t, frame, sizeof frame);
            tui_fake_ms += 100;
            CHECK(health(t).armed == 0);
        }
    }
    h = health(t);
    CHECK(h.arm_failures == 21 && h.alloc_failures == 21 && h.arms == 22);
    CHECK(log_count("arming on a view: no memory for") == 1);
    tui_fake_ms += 60000;                                 /* a minute on: said again */
    ta_fail_n = tab;
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(log_count("arming on a view: no memory for") == 2 && health(t).arm_failures == 22);
    log_end();
    ta_fail_n = 0;
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 9) */
static void test_settings(void)
{
    gptps *e = open_manual();
    gptps_tui *t;
    gptps_tui_config c;
    char v[64];
    if (!e) return;
    tui_fake_ms = 7000000;
    base_cfg(&c, GPTPS_TUI_KPI_FULL, 3);                  /* out of range: refused */
    CHECK(gptps_tui_install(e, &c) == NULL);
    base_cfg(&c, GPTPS_TUI_KPI_FULL, -1);
    CHECK(gptps_tui_install(e, &c) == NULL);
    base_cfg(&c, GPTPS_TUI_KPI_FULL, GPTPS_TUI_ARM_ON_VIEW); c.disarm_ms = 99;
    CHECK(gptps_tui_install(e, &c) == NULL);
    c.disarm_ms = 86400001;
    CHECK(gptps_tui_install(e, &c) == NULL);
    c.disarm_ms = -5;
    CHECK(gptps_tui_install(e, &c) == NULL);
    base_cfg(&c, GPTPS_TUI_KPI_FULL, 0);
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    CHECK(gptps_settings_get(e, "tui.arm", v, sizeof v) == GPTPS_OK && strcmp(v, "always") == 0);
    CHECK(gptps_settings_get(e, "tui.disarm_ms", v, sizeof v) == GPTPS_OK && strcmp(v, "60000") == 0);
    CHECK(gptps_settings_set(e, "tui.arm", "sometimes") != GPTPS_OK);
    CHECK(gptps_settings_set(e, "tui.disarm_ms", "50") != GPTPS_OK);
    CHECK(gptps_settings_set(e, "tui.disarm_ms", "86400001") != GPTPS_OK);
    CHECK(gptps_settings_set(e, "tui.disarm_ms", "2500") == GPTPS_OK && health(t).disarm_ms == 2500);
    CHECK(gptps_settings_get(e, "tui.disarm_ms", v, sizeof v) == GPTPS_OK && strcmp(v, "2500") == 0);
    {   /* the registry declares both (docs/CONFIG.md and a settings file are checked by them) */
        size_t i, n = gptps_settings_count(e);
        int seen = 0;
        for (i = 0; i < n; ++i) {
            gptps_setting_info in;
            memset(&in, 0, sizeof in); in.struct_size = sizeof in;
            if (gptps_settings_get_info(e, i, &in) != GPTPS_OK) continue;
            if (strcmp(in.key_copy, "tui.disarm_ms") == 0) {
                CHECK(in.type == GPTPS_SETTING_UINT && in.has_range && in.min == 100 && in.max == 86400000);
                seen |= 1;
            } else if (strcmp(in.key_copy, "tui.arm") == 0) {
                CHECK(in.type == GPTPS_SETTING_ENUM && in.choices && strcmp(in.choices[0], "always") == 0 &&
                      strcmp(in.choices[1], "on_view") == 0 && in.choices[2] == NULL);
                seen |= 2;
            } else if (strcmp(in.key_copy, "tui.budget_kb") == 0) {
                CHECK(seen == 3);                         /* budget_kb stays the last tui key registered */
                seen |= 4;
            }
        }
        CHECK(seen == 7);
    }
    CHECK(gptps_tui_set_disarm_ms(t, 99) == GPTPS_E_INVAL && gptps_tui_set_disarm_ms(t, 86400001) == GPTPS_E_INVAL);
    CHECK(gptps_tui_set_disarm_ms(t, 0) == GPTPS_OK && health(t).disarm_ms == 60000);
    CHECK(gptps_tui_set_arm(t, (gptps_tui_arm)3) == GPTPS_E_INVAL && gptps_tui_set_arm(NULL, GPTPS_TUI_ARM_ALWAYS) == GPTPS_E_INVAL);
    /* to on_view: counts as a view - armed now, disarmed disarm_ms later */
    tui_fake_ms += 1000;
    CHECK(gptps_settings_set(e, "tui.arm", "on_view") == GPTPS_OK);
    CHECK(gptps_settings_get(e, "tui.arm", v, sizeof v) == GPTPS_OK && strcmp(v, "on_view") == 0);
    CHECK(health(t).armed == 1 && health(t).arm == GPTPS_TUI_ARM_ON_VIEW);
    tui_fake_ms += 59999;
    CHECK(health(t).armed == 1);
    tui_fake_ms += 1;
    CHECK(health(t).armed == 0 && health(t).kpi == GPTPS_TUI_KPI_GAUGE);
    /* to always: arms at once, with no view */
    CHECK(gptps_tui_set_arm(t, GPTPS_TUI_ARM_DEFAULT) == GPTPS_OK);
    CHECK(health(t).armed == 1 && health(t).arm == GPTPS_TUI_ARM_ALWAYS && health(t).kpi == GPTPS_TUI_KPI_FULL &&
          health(t).arms == 1);
    tui_fake_ms += 10u * 60000u;
    CHECK(health(t).armed == 1);                          /* ALWAYS never disarms */
    /* the request moves freely while disarmed; the next view builds it */
    CHECK(gptps_tui_set_arm(t, GPTPS_TUI_ARM_ON_VIEW) == GPTPS_OK);
    tui_fake_ms += 60000;
    CHECK(health(t).armed == 0);
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_TASKS) == GPTPS_OK && health(t).kpi == GPTPS_TUI_KPI_GAUGE &&
          health(t).kpi_requested == GPTPS_TUI_KPI_TASKS && health(t).footprint_bytes == formula(TUI_T_GAUGE, 0));
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(health(t).kpi == GPTPS_TUI_KPI_TASKS && health(t).footprint_bytes == formula(TUI_T_TASKS, 0));
    CHECK(gptps_tui_press(t, 'm') == 3 && health(t).kpi_requested == GPTPS_TUI_KPI_NORMAL);   /* from what runs, armed */
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 10) */
static void test_line(void)
{
    gptps *e = open_manual();
    gptps_tui *t;
    gptps_tui_config c;
    if (!e) return;
    tui_fake_ms = 8000000;
    base_cfg(&c, GPTPS_TUI_KPI_FULL, GPTPS_TUI_ARM_ON_VIEW);
    c.disarm_ms = 600000;
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    tui_fake_ms += 59000;                                 /* armed 59 s after install */
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "recording since first view (<1m ago) - ") != NULL);
    tui_fake_ms += 1500;                                  /* the uptime ticks next at 2m, the age at 1m */
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(t->view->next_change == 8000000u + 59000u + 60000u);   /* an idle frame is due when the age changes */
    tui_fake_ms = 8000000u + 59000u + 125000u;
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "recording since first view (2m ago) - ") != NULL);
    CHECK(gptps_tui_set_clock(t, GPTPS_TUI_CLOCK_SECONDS) == GPTPS_OK);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "recording since first view (2m05s ago) - ") != NULL);
    CHECK(gptps_tui_set_clock(t, GPTPS_TUI_CLOCK_OFF) == GPTPS_OK);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "recording since first view (59 s after install) - ") != NULL);
    CHECK(t->view->next_change == 0);                     /* with no clock it never changes by itself */
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 10) also: the line takes a row of the frame's height - a full frame is as tall armed on a view
 * as with ALWAYS (the TASKS rows give it up: "+N more") */
static int nlines(const char *f) { int n = 0; for (; *f; ++f) n += *f == '\n'; return n; }
static void test_line_height(void)
{
    int arm, k, n[2] = { 0, 0 };
    for (arm = 0; arm < 2; ++arm) {
        gptps *e = open_manual();
        gptps_tui *t;
        gptps_tui_config c;
        if (!e) return;
        tui_fake_ms = 8500000;
        base_cfg(&c, GPTPS_TUI_KPI_FULL, arm ? GPTPS_TUI_ARM_ON_VIEW : GPTPS_TUI_ARM_ALWAYS);
        t = gptps_tui_install(e, &c);
        CHECK(t != NULL);
        if (!t) { gptps_shutdown(e); return; }
        tui_fake_ms += 1000;
        gptps_tui_render(t, frame, sizeof frame);         /* armed (on_view) */
        for (k = 0; k < 40; ++k) {                        /* more rows than 24 lines hold */
            char nm[16];
            snprintf(nm, sizeof nm, "type%02d", k);
            emit(t, GPTPS_EV_QUEUED, nm, (gptps_handle)(k + 1));
        }
        gptps_tui_render(t, frame, sizeof frame);
        n[arm] = nlines(frame);
        CHECK((strstr(frame, "recording since first view") != NULL) == arm && strstr(frame, " more") != NULL);
        gptps_shutdown(e);
        gptps_tui_close(t);
    }
    CHECK(n[0] == n[1] && n[0] <= 24);
}

/* 11) */
static void test_hint(void)
{
    gptps *e = open_manual();
    gptps_tui *t;
    gptps_tui_config c;
    FILE *o = tmpfile();                                  /* not a terminal */
    if (!e) { if (o) fclose(o); return; }
    if (!o) { printf("note: no tmpfile here; the hint check skipped\n"); gptps_shutdown(e); return; }
    tui_fake_ms = 9000000;
    log_begin();
    base_cfg(&c, GPTPS_TUI_KPI_FULL, 0); c.out = o;
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    CHECK(log_count("gptps_tui_install: no terminal to run the live dashboard on") == 1);
    CHECK(log_count("kpi full collects everything from now on - about 38 ns and one lock an event, 110 KiB held") == 1 ||
          sizeof(gptps_tui) != 7184);                     /* the figure: the formula at the defaults */
    CHECK(log_count("tui.arm=on_view") == 1);
    loglen = 0; logbuf[0] = 0;
    gptps_tui_run(t);                                     /* interactive 0: said, once */
    gptps_tui_run(t);
    CHECK(log_count("gptps_tui_run: cfg.interactive is 0") == 1 && health(t).run_noops == 2);
    gptps_shutdown(e);
    gptps_tui_close(t);
    e = open_manual();
    if (!e) { log_end(); fclose(o); return; }
    loglen = 0; logbuf[0] = 0;
    base_cfg(&c, GPTPS_TUI_KPI_GAUGE, 0); c.out = o; c.interactive = 1;   /* interactive, no terminal */
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    gptps_tui_run(t);                                     /* returns at once: said, and counted */
    CHECK(log_count("gptps_tui_run: stdin or the output is not a terminal") == 1 && health(t).run_noops == 1);
    gptps_tui_run(t);                                     /* again within a minute: counted, not said */
    tui_fake_ms += 59999;
    gptps_tui_run(t);
    CHECK(log_count("gptps_tui_run: stdin or the output is not a terminal") == 1 && health(t).run_noops == 3);
    tui_fake_ms += 1;                                     /* a minute after the last: said again */
    gptps_tui_run(t);
    CHECK(log_count("gptps_tui_run: stdin or the output is not a terminal") == 2 && health(t).run_noops == 4);
    gptps_shutdown(e);
    gptps_tui_close(t);
    {   /* tui.arm = on_view from a settings file (applied as install defines the tui keys): it
         * starts disarmed, as cfg.arm does - no hint, no record from install */
        const char *path = "test_tui_arm_settings.toml";
        FILE *f = fopen(path, "w");
        gptps_config ec;
        gptps_tui_health_info h;
        if (!f) { printf("note: cannot write a settings file; that check skipped\n"); }
        else {
            fputs("[tui]\narm = \"on_view\"\n", f); fclose(f);
            memset(&ec, 0, sizeof ec);
            ec.struct_size = sizeof ec; ec.limits.struct_size = sizeof ec.limits;
            ec.limits.max_concurrent_tasks = 1; ec.limits.max_memory_bytes = 1u << 20;
            ec.mode = GPTPS_RUN_MANUAL; ec.config_path = path;
            e = NULL;
            CHECK(gptps_open_ex(&ec, &e) == GPTPS_OK);
            if (e) {
                loglen = 0; logbuf[0] = 0;
                base_cfg(&c, GPTPS_TUI_KPI_FULL, 0); c.out = o;
                t = gptps_tui_install(e, &c);
                CHECK(t != NULL);
                h = health(t);
                CHECK(h.arm == GPTPS_TUI_ARM_ON_VIEW && h.armed == 0 && h.kpi == GPTPS_TUI_KPI_GAUGE &&
                      h.footprint_bytes == formula(TUI_T_GAUGE, 0) && h.arms == 0 && h.disarms == 0);
                CHECK(log_count("no terminal to run") == 0);
                tui_fake_ms += 1000;
                gptps_tui_render(t, frame, sizeof frame);  /* the first view arms, and says so */
                h = health(t);
                CHECK(h.armed == 1 && h.arms == 1 && strstr(frame, "recording since first view") != NULL);
                gptps_shutdown(e);
                gptps_tui_close(t);
            }
            remove(path);
        }
    }
    e = open_manual();
    if (!e) { log_end(); fclose(o); return; }
    loglen = 0; logbuf[0] = 0;
    base_cfg(&c, GPTPS_TUI_KPI_LATENCY, 0); c.out = o;    /* not FULL: no hint */
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL && log_count("no terminal to run") == 0);
    gptps_shutdown(e);
    gptps_tui_close(t);
    e = open_manual();
    if (!e) { log_end(); fclose(o); return; }
    base_cfg(&c, GPTPS_TUI_KPI_FULL, GPTPS_TUI_ARM_ON_VIEW); c.out = o;   /* on_view: no hint */
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL && log_count("no terminal to run") == 0);
    log_end();
    gptps_shutdown(e);
    gptps_tui_close(t);
    fclose(o);
}

int main(void)
{
    test_always();
    test_on_view();
#if !defined(_WIN32) && !defined(GPTPS_TEST_HAL_SIM)
    test_run_is_a_view();
#endif
    test_degrade();
    test_budget();
    test_arm_oom();
    test_settings();
    test_line();
    test_line_height();
    test_hint();
    if (fails) { printf("test_tui_arm: %d FAILED\n", fails); return 1; }
    printf("test_tui_arm: OK\n");
    return 0;
}
