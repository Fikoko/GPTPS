/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_tui_budget.c - the dashboard's MEMORY and BUDGET (1.13; gptps_tui.h, at
 * gptps_tui_install), with the add-on compiled in and EVERY allocation it makes
 * interposed: each block carries its size, so the bytes it holds are known exactly.
 *
 *  1) The FOOTPRINT formula equals what is allocated, byte for byte, at every tier -
 *     installed there and moved there - with and without the TASKS pane, at other journal
 *     sizes and windows, with add_task entries and payloads, and what a resize or a free
 *     gives back; the BUFFERS (view, pane cache, snapshot's frame) are runtime_bytes, and
 *     footprint + buffers is everything held; after close nothing is. The struct sizes the
 *     header documents are the real ones (x86-64 Linux).
 *  2) 0 = no budget: nothing refused, nothing stopped (the 1.12 behaviour).
 *  3) install over the budget returns NULL with one WARN naming the exact figure, and holds
 *     nothing; at the figure (rounded up to a KiB) it installs.
 *  4) Every change that grows the request past the budget is refused - GPTPS_E_INVAL, one WARN
 *     naming the figure, counted, nothing changed: set_kpi, set_recent, set_latency_window,
 *     add_task (a new entry, a bigger payload), the 'm' key, the tui.kpi / tui.recent /
 *     tui.activity / tui.latency_window writes, and a budget below the footprint
 *     (set_budget_kb, tui.budget_kb); a change that does not grow it never is.
 *  5) The BUFFERS stop at the budget, counted, a WARN once a minute: the view (the frame
 *     says so), snapshot's first 16 KiB (no frame), a frame's growth (cut, truncated), the
 *     tasks pane's cache (it says so); never counted as an allocation failure.
 *  5b) A pane pass counts one stop however many entries it leaves out (tasks and settings
 *     panes), and a real allocation failure in the same pass is still counted.
 *  5c) Settings files (gptps.h, THE FILE'S ORDER): the file's order does not matter. At
 *     install (the tui keys defined under gptps_settings_hold) and at a reload, a file that
 *     raises or lowers both the footprint and tui.budget_kb applies whole in either order -
 *     no refusal counted, no budget WARN, no ERROR; a file that cannot fit is refused once:
 *     one WARN, one count, one ERROR, GPTPS_E_CONFIG, the same state in either order.
 *  6) With ADAPT: REPORT shows BUDGET pressure; DEGRADE sheds for it (-activity first),
 *     a WARN each, until the buffers fit; the request is unchanged, so a budget below it is
 *     still refused and 'u' restores it; the event path allocates nothing.
 *  7) health: a struct_size from before BUDGET leaves its fields alone; tui.budget_kb reads
 *     back.
 */
#if !defined(_WIN32)
#  define _XOPEN_SOURCE 700         /* posix_openpt & co. (and what the add-on asks for: its headers come first here) */
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "gptps.h"

/* ---- the interposed allocator: a header before each block holds its size ---- */
typedef union { size_t n; long double ld; void *p; uint64_t u; } tb_hdr;
static size_t tb_live;              /* bytes the add-on holds now */
static long   tb_calls;             /* allocations made (the event path must make none) */
static size_t tb_fail_n;            /* !0: the next allocation of exactly this many bytes fails (once) */
static void *tb_malloc(size_t n)
{
    tb_hdr *h;
    tb_calls++;
    if (tb_fail_n && n == tb_fail_n) { tb_fail_n = 0; return NULL; }
    h = (tb_hdr *)malloc(sizeof *h + n);
    if (!h) return NULL;
    h->n = n; tb_live += n;
    return h + 1;
}
static void *tb_calloc(size_t k, size_t s)
{
    void *p;
    if (s && k > (size_t)-1 / s) return NULL;
    p = tb_malloc(k * s);
    if (p) memset(p, 0, k * s);
    return p;
}
static void tb_free(void *p)
{
    tb_hdr *h;
    if (!p) return;
    h = (tb_hdr *)p - 1;
    tb_live -= h->n;
    free(h);
}
static void *tb_realloc(void *p, size_t n)
{
    tb_hdr *h, *nh;
    if (!p) return tb_malloc(n);
    h = (tb_hdr *)p - 1;
    tb_calls++;
    if (tb_fail_n && n == tb_fail_n) { tb_fail_n = 0; return NULL; }
    nh = (tb_hdr *)realloc(h, sizeof *nh + n);
    if (!nh) return NULL;
    tb_live -= nh->n; nh->n = n; tb_live += n;
    return nh + 1;
}
/* ADAPT's windows on a clock the test sets (0: the real one) */
static uint64_t tui_fake_ms;
#define TUI_NOW_MS() (tui_fake_ms ? tui_fake_ms : gptps_now_ms(NULL))
#define TUI_FRAME_US(us) (tui_fake_ms ? 0u : (us))
#define malloc(n) tb_malloc((n))
#define calloc(n, s) tb_calloc((n), (s))
#define realloc(p, n) tb_realloc((p), (n))
#define free(p) tb_free((p))
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
    if (getenv("TUI_BUDGET_LOG")) fprintf(stderr, "[log] %s\n", msg);   /* to read the WARNs themselves */
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
static gptps_status ok_task(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }
static void deftask(gptps *e, const char *name)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = ok_task; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
}
static void base_cfg(gptps_tui_config *c, int kpi, int budget_kb)
{
    memset(c, 0, sizeof *c);
    c->struct_size = sizeof *c; c->color = 0; c->interactive = 0; c->unicode = 0;
    c->kpi = kpi; c->budget_kb = budget_kb; c->clock = GPTPS_TUI_CLOCK_OFF;
}
static gptps_tui_health_info health(gptps_tui *t)
{
    gptps_tui_health_info h;
    memset(&h, 0, sizeof h); h.struct_size = sizeof h;
    CHECK(gptps_tui_health(t, &h) == GPTPS_OK);
    return h;
}
static void emit(gptps_tui *t, int kind, const char *task, gptps_handle h, uint64_t ts)
{
    gptps_event ev;
    memset(&ev, 0, sizeof ev);
    ev.struct_size = sizeof ev; ev.kind = (gptps_event_kind)kind; ev.task_name = task; ev.handle = h;
    ev.status = kind == GPTPS_EV_FAILED ? GPTPS_E_TASK : GPTPS_OK; ev.ts_ms = ts;
    tui_on_event(&ev, t);
}

/* ---- the formula, as gptps_tui.h (MEMORY) writes it ---- */
static uint64_t np2(uint64_t n) { uint64_t p = 1; while (p < n) p <<= 1; return p; }
static uint64_t doc_formula(int tier, int max_tasks, int show_tasks, int show_recent, int recent, int act,
                            int act_shed, int win, int ui_cap, uint64_t payloads)
{
    const uint64_t D = sizeof(gptps_tui), U = sizeof(tui_ui), R = sizeof(tui_task), E = sizeof(tui_event),
                   A = sizeof(tui_act), S = sizeof(tui_slot);
    uint64_t f = D + U * (uint64_t)ui_cap + payloads, idx = np2(2u * (uint64_t)max_tasks);
    int tasks = tier >= TUI_T_TASKS && show_tasks;
    if (idx < 8) idx = 8;
    if (tasks) f += R * ((uint64_t)max_tasks + 1) + 4 * idx + 2048;
    if (tier >= TUI_T_PROBLEMS && show_recent && recent > 0) f += E * (uint64_t)recent;
    if (tier >= TUI_T_ACTIVITY && show_recent && act > 0 && !act_shed) f += (A + (tasks ? 0u : 64u)) * (uint64_t)act;
    if (tier >= TUI_T_LATENCY) f += S * np2(((uint64_t)win + 64u) * 4u / 3u + 1u);
    if (tier >= TUI_T_FULL && tasks) f += 256u * ((uint64_t)max_tasks + 1);
    return f;
}
/* the view a dashboard frame of R rows holds (gptps_tui.h, MEMORY 2) */
static uint64_t doc_view(int R) { return sizeof(tui_view) + (uint64_t)R * (sizeof(tui_vrow) + sizeof(tui_event)); }

/* everything held is footprint + buffers, and the footprint is the formula for what runs */
static int holds(gptps_tui *t, uint64_t want_foot, uint64_t want_rt, int line)
{
    gptps_tui_health_info h = health(t);
    int ok = h.footprint_bytes == want_foot && h.runtime_bytes == want_rt &&
             tb_live == h.footprint_bytes + h.runtime_bytes;
    if (!ok) printf("line %d: live %lu, footprint %lu (formula %lu), buffers %lu (expected %lu)\n", line,
                    (unsigned long)tb_live, (unsigned long)h.footprint_bytes, (unsigned long)want_foot,
                    (unsigned long)h.runtime_bytes, (unsigned long)want_rt);
    return ok;
}
#define HOLDS(t, f, r) CHECK(holds((t), (f), (r), __LINE__))

static const int TIER_KPI[8] = { GPTPS_TUI_KPI_OFF, GPTPS_TUI_KPI_MINIMAL, GPTPS_TUI_KPI_GAUGE, GPTPS_TUI_KPI_PROBLEMS,
                                 GPTPS_TUI_KPI_TASKS, GPTPS_TUI_KPI_NORMAL, GPTPS_TUI_KPI_LATENCY, GPTPS_TUI_KPI_FULL };

/* 1) */
static void test_formula(void)
{
    static char frame[65536];
    gptps *e;
    gptps_tui *t;
    gptps_tui_config c;
    int r, st, sr;
#if defined(__linux__) && defined(__x86_64__)
    /* the sizes gptps_tui.h states (a change there must change the header too) */
    CHECK(sizeof(gptps_tui) == 7080 && sizeof(tui_ui) == 280 && sizeof(tui_task) == 264 && sizeof(tui_event) == 304);
    CHECK(sizeof(tui_slot) == 32 && sizeof(tui_act) == 24 && TUI_HIST_B * sizeof(uint64_t) == 256);
    CHECK(sizeof(tui_view) == 13560 && sizeof(tui_vrow) + sizeof(tui_event) == 560 &&
          sizeof(gptps_tui_source_row) == 208);
    CHECK(doc_formula(TUI_T_FULL, 64, 1, 1, 8, 8, 0, 1024, 0, 0) == 111600);   /* the defaults: 109 KiB */
#endif
    /* installed at each tier, each pane shown or hidden */
    for (st = 0; st < 2; ++st)
        for (sr = 0; sr < 2; ++sr)
            for (r = 0; r <= TUI_T_FULL; ++r) {
                size_t before = tb_live;
                if ((e = open_manual()) == NULL) return;
                base_cfg(&c, TIER_KPI[r], 0);
                c.show_tasks = st ? 0 : -1; c.show_recent = sr ? 0 : -1;
                c.max_tasks = 10; c.recent_lines = 5; c.activity_lines = 7; c.latency_window = 100;
                t = gptps_tui_install(e, &c);
                CHECK(t != NULL);
                if (!t) { gptps_shutdown(e); continue; }
                HOLDS(t, doc_formula(r, 10, st, sr, 5, 7, 0, 100, 0, 0), 0);
                CHECK(tb_live - before == doc_formula(r, 10, st, sr, 5, 7, 0, 100, 0, 0));
                CHECK(health(t).footprint_request_bytes == health(t).footprint_bytes);
                gptps_shutdown(e);
                gptps_tui_close(t);
                CHECK(tb_live == before);          /* nothing left behind: the formula is all of it */
            }
    e = open_manual();
    if (!e) return;
    deftask(e, "a"); deftask(e, "b");
    /* moved there: from FULL down the ladder and up again, then sizes and add_task */
    base_cfg(&c, GPTPS_TUI_KPI_FULL, 0);
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    HOLDS(t, doc_formula(TUI_T_FULL, 64, 1, 1, 8, 8, 0, 1024, 0, 0), 0);
    for (r = TUI_T_FULL; r >= 0; --r) {
        CHECK(gptps_tui_set_kpi(t, (gptps_tui_kpi)TIER_KPI[r]) == GPTPS_OK);
        HOLDS(t, doc_formula(r, 64, 1, 1, 8, 8, 0, 1024, 0, 0), 0);
    }
    for (r = 0; r <= TUI_T_FULL; ++r) {
        CHECK(gptps_tui_set_kpi(t, (gptps_tui_kpi)TIER_KPI[r]) == GPTPS_OK);
        HOLDS(t, doc_formula(r, 64, 1, 1, 8, 8, 0, 1024, 0, 0), 0);
    }
    CHECK(gptps_tui_set_recent(t, 100, 0) == GPTPS_OK);
    HOLDS(t, doc_formula(TUI_T_FULL, 64, 1, 1, 100, 0, 0, 1024, 0, 0), 0);
    CHECK(gptps_tui_set_recent(t, -1, 300) == GPTPS_OK && gptps_tui_set_latency_window(t, 5000) == GPTPS_OK);
    HOLDS(t, doc_formula(TUI_T_FULL, 64, 1, 1, 100, 300, 0, 5000, 0, 0), 0);
    CHECK(gptps_tui_add_task(t, "a", "A", 'a', "12345", 5) == GPTPS_OK);   /* the array: 8 entries */
    HOLDS(t, doc_formula(TUI_T_FULL, 64, 1, 1, 100, 300, 0, 5000, 8, 5), 0);
    CHECK(gptps_tui_add_task(t, "a", "A", 'a', "123", 3) == GPTPS_OK);     /* replaced: 2 bytes back */
    HOLDS(t, doc_formula(TUI_T_FULL, 64, 1, 1, 100, 300, 0, 5000, 8, 3), 0);
    for (r = 0; r < 8; ++r) {                                              /* the 9th doubles it */
        char nm[16];
        snprintf(nm, sizeof nm, "x%d", r);
        CHECK(gptps_tui_add_task(t, nm, NULL, 0, NULL, 0) == GPTPS_OK);
    }
    HOLDS(t, doc_formula(TUI_T_FULL, 64, 1, 1, 100, 300, 0, 5000, 16, 3), 0);
    /* the BUFFERS: a dashboard frame's view (24 rows headless), the tasks pane's cache, and
     * snapshot's frame buffer only during the call */
    gptps_tui_render(t, frame, sizeof frame);
    HOLDS(t, doc_formula(TUI_T_FULL, 64, 1, 1, 100, 300, 0, 5000, 16, 3), doc_view(24));
    t->pane = TUI_PANE_TASKS;
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(t->pc.tasks != NULL);
    HOLDS(t, doc_formula(TUI_T_FULL, 64, 1, 1, 100, 300, 0, 5000, 16, 3), doc_view(24) + pc_bytes(&t->pc));
    CHECK(health(t).bytes_panes == pc_bytes(&t->pc) && pc_bytes(&t->pc) == 16 * sizeof(tui_ctask));
    t->pane = TUI_PANE_SETTINGS;                            /* a pool, too */
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(t->pc.sets != NULL && t->pc.pool != NULL);
    HOLDS(t, doc_formula(TUI_T_FULL, 64, 1, 1, 100, 300, 0, 5000, 16, 3), doc_view(24) + pc_bytes(&t->pc));
    t->pane = TUI_PANE_TASKS;                               /* the settings entries freed, the pool kept */
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(t->pc.sets == NULL && t->pc.tasks != NULL);
    HOLDS(t, doc_formula(TUI_T_FULL, 64, 1, 1, 100, 300, 0, 5000, 16, 3), doc_view(24) + pc_bytes(&t->pc));
    t->pane = TUI_PANE_DASH;
    gptps_tui_render(t, frame, sizeof frame);               /* the cache freed */
    HOLDS(t, doc_formula(TUI_T_FULL, 64, 1, 1, 100, 300, 0, 5000, 16, 3), doc_view(24));
    {
        FILE *f = tmpfile();
        if (f) {
            t->cfg.out = f;
            gptps_tui_snapshot(t);                          /* its 16 KiB, freed when it returns */
            HOLDS(t, doc_formula(TUI_T_FULL, 64, 1, 1, 100, 300, 0, 5000, 16, 3), doc_view(24));
            t->cfg.out = stdout;
            fclose(f);
        }
    }
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 2) */
static void test_no_budget(void)
{
    static char frame[65536];
    gptps *e = open_manual();
    gptps_tui *t;
    gptps_tui_config c;
    gptps_tui_health_info h;
    if (!e) return;
    base_cfg(&c, GPTPS_TUI_KPI_FULL, 0);
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    log_begin();
    CHECK(gptps_tui_set_latency_window(t, 1 << 20) == GPTPS_OK && gptps_tui_set_recent(t, 4096, 4096) == GPTPS_OK);
    gptps_tui_render(t, frame, sizeof frame);
    h = health(t);
    CHECK(h.budget_bytes == 0 && h.budget_refusals == 0 && h.budget_stops == 0 && h.budget_need_bytes == 0);
    CHECK(h.footprint_bytes > (uint64_t)sizeof(tui_slot) * (1u << 21) && strstr(frame, "budget") == NULL);
    CHECK(log_count("budget") == 0);
    log_end();
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 3) */
static void test_install_refused(void)
{
    gptps *e = open_manual();
    gptps_tui *t;
    gptps_tui_config c;
    char want[160];
    uint64_t f = doc_formula(TUI_T_FULL, 64, 1, 1, 8, 8, 0, 1024, 0, 0);
    size_t before;
    if (!e) return;
    before = tb_live;
    base_cfg(&c, GPTPS_TUI_KPI_FULL, (int)(f / 1024u));     /* a KiB short of the defaults */
    log_begin();
    t = gptps_tui_install(e, &c);
    CHECK(t == NULL && tb_live == before);
    snprintf(want, sizeof want, "needs %lu KiB (%lu bytes: the dashboard %lu, TASKS", (unsigned long)((f + 1023) / 1024),
             (unsigned long)f, (unsigned long)sizeof(gptps_tui));
    CHECK(log_count("gptps_tui_install: the configuration (kpi full") == 1 && log_count(want) == 1 &&
          log_count("not installed") == 1);
    log_end();
    base_cfg(&c, GPTPS_TUI_KPI_FULL, (int)((f + 1023) / 1024u));   /* at the figure: installed */
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    if (t) {
        gptps_tui_health_info h = health(t);
        CHECK(h.budget_bytes == (f + 1023) / 1024u * 1024u && h.footprint_bytes == f && h.budget_refusals == 0);
        gptps_tui_close(t);
    }
    base_cfg(&c, GPTPS_TUI_KPI_OFF, 1);                      /* even the dashboard itself must fit */
    CHECK(gptps_tui_install(e, &c) == NULL);
    base_cfg(&c, GPTPS_TUI_KPI_OFF, -1);                     /* out of range */
    CHECK(gptps_tui_install(e, &c) == NULL);
    gptps_shutdown(e);
}

/* one refusal: GPTPS_E_INVAL (or the registry's refusal), one WARN with `need`'s figure, counted,
 * and nothing changed - the footprint, the tier, the sizes */
static void refused(gptps_tui *t, gptps_status st, const char *who, uint64_t need, int line)
{
    gptps_tui_health_info h = health(t);
    char want[96];
    snprintf(want, sizeof want, "needs %lu KiB (%lu bytes", (unsigned long)((need + 1023) / 1024), (unsigned long)need);
    if (!(st != GPTPS_OK && log_count(who) == 1 && log_count(want) == 1 && log_count("refused, nothing changed") == 1 &&
          h.budget_need_bytes == need))
        printf("line %d: %s (status %d, need %lu, last need %lu):\n%s", line, who, (int)st, (unsigned long)need,
               (unsigned long)h.budget_need_bytes, logbuf), ++fails;
    loglen = 0; logbuf[0] = 0;
}
#define REFUSED(t, call, who, need) refused((t), (call), (who), (need), __LINE__)

/* 4) */
static void test_refusals(void)
{
    gptps *e = open_manual();
    gptps_tui *t;
    gptps_tui_config c;
    gptps_tui_health_info h0, h;
    uint64_t f0, bud;
    size_t live;
    char v[64];
    if (!e) return;
    /* TASKS, 8 + 8 lines: room for a little more */
    f0 = doc_formula(TUI_T_TASKS, 64, 1, 1, 8, 8, 0, 1024, 0, 0);
    bud = (f0 + 1023) / 1024 * 1024 + 4096;
    base_cfg(&c, GPTPS_TUI_KPI_TASKS, (int)(bud / 1024));
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    h0 = health(t); live = tb_live;
    log_begin();
    REFUSED(t, gptps_tui_set_kpi(t, GPTPS_TUI_KPI_FULL), "gptps_tui_set_kpi(full): tui.kpi=full needs",
            doc_formula(TUI_T_FULL, 64, 1, 1, 8, 8, 0, 1024, 0, 0));
    REFUSED(t, gptps_settings_set(e, "tui.kpi", "latency"), "tui.kpi=latency needs",
            doc_formula(TUI_T_LATENCY, 64, 1, 1, 8, 8, 0, 1024, 0, 0));
    REFUSED(t, gptps_tui_set_recent(t, 100, -1), "gptps_tui_set_recent: tui.recent=100 needs",
            doc_formula(TUI_T_TASKS, 64, 1, 1, 100, 8, 0, 1024, 0, 0));
    REFUSED(t, gptps_settings_set(e, "tui.recent", "50"), "tui.recent=50 needs",
            doc_formula(TUI_T_TASKS, 64, 1, 1, 50, 8, 0, 1024, 0, 0));
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_PROBLEMS) == GPTPS_OK);   /* lower: never refused */
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_TASKS) == GPTPS_OK);      /* back: it fits */
    CHECK(tb_live == live);
    /* NORMAL fits; then a bigger ACTIVITY journal does not */
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_NORMAL) == GPTPS_OK);
    live = tb_live;
    REFUSED(t, gptps_tui_set_recent(t, -1, 400), "gptps_tui_set_recent: tui.activity=400 needs",
            doc_formula(TUI_T_ACTIVITY, 64, 1, 1, 8, 400, 0, 1024, 0, 0));
    REFUSED(t, gptps_settings_set(e, "tui.activity", "300"), "tui.activity=300 needs",
            doc_formula(TUI_T_ACTIVITY, 64, 1, 1, 8, 300, 0, 1024, 0, 0));
    /* a window: allowed when the tier holds no table (the request's footprint does not grow) */
    CHECK(gptps_tui_set_latency_window(t, 1 << 20) == GPTPS_OK);
    CHECK(gptps_tui_set_latency_window(t, 1024) == GPTPS_OK);
    /* add_task: the first entry makes the array of 8 (8 x 280 B): with less than that left it is refused */
    {
        uint64_t fn = doc_formula(TUI_T_ACTIVITY, 64, 1, 1, 8, 8, 0, 1024, 0, 0);
        CHECK(gptps_tui_set_budget_kb(t, (int)((fn + 1023) / 1024)) == GPTPS_OK);
        REFUSED(t, gptps_tui_add_task(t, "a", NULL, 'a', NULL, 0), "gptps_tui_add_task(a): the entry needs",
                fn + 8 * sizeof(tui_ui));
        CHECK(t->nui == 0 && t->ui_cap == 0);
        CHECK(gptps_tui_set_budget_kb(t, (int)(bud / 1024)) == GPTPS_OK);
    }
    /* ... it fits the budget installed; a payload past what is left does not */
    CHECK(gptps_tui_add_task(t, "a", NULL, 'a', NULL, 0) == GPTPS_OK);
    {
        static char big[8192];
        uint64_t fa = doc_formula(TUI_T_ACTIVITY, 64, 1, 1, 8, 8, 0, 1024, 8, 0);
        live = tb_live;
        REFUSED(t, gptps_tui_add_task(t, "a", NULL, 'a', big, sizeof big), "gptps_tui_add_task(a): the entry needs",
                fa + sizeof big);
        REFUSED(t, gptps_tui_add_task(t, "new", NULL, 'n', big, sizeof big), "gptps_tui_add_task(new): the entry needs",
                fa + sizeof big);
        CHECK(tb_live == live && t->nui == 1 && t->ui[0].plen == 0);
    }
    /* the 'm' key steps to LATENCY: refused, the toast says why */
    REFUSED(t, gptps_tui_press(t, 'm') == 3 && strstr(t->status, "over tui.budget_kb") ? GPTPS_E_INVAL : GPTPS_OK,
            "gptps_tui_set_kpi(latency): tui.kpi=latency needs",
            doc_formula(TUI_T_LATENCY, 64, 1, 1, 8, 8, 0, 1024, 8, 0));
    /* at LATENCY, a bigger window is refused */
    CHECK(gptps_tui_set_budget_kb(t, 4096) == GPTPS_OK);
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_LATENCY) == GPTPS_OK);
    REFUSED(t, gptps_tui_set_latency_window(t, 1 << 20), "gptps_tui_set_latency_window: tui.latency_window=1048576 needs",
            doc_formula(TUI_T_LATENCY, 64, 1, 1, 8, 8, 0, 1 << 20, 8, 0));
    REFUSED(t, gptps_settings_set(e, "tui.latency_window", "500000"), "tui.latency_window=500000 needs",
            doc_formula(TUI_T_LATENCY, 64, 1, 1, 8, 8, 0, 500000, 8, 0));
    CHECK(t->lat_cap == 1024 && t->kpi == GPTPS_TUI_KPI_LATENCY);
    /* the budget itself: not below the footprint */
    {
        uint64_t fl = doc_formula(TUI_T_LATENCY, 64, 1, 1, 8, 8, 0, 1024, 8, 0);
        REFUSED(t, gptps_tui_set_budget_kb(t, 64), "gptps_tui_set_budget_kb: tui.budget_kb=64 is below", fl);
        REFUSED(t, gptps_settings_set(e, "tui.budget_kb", "10"), "tui.budget_kb=10 is below", fl);
        CHECK(gptps_settings_get(e, "tui.budget_kb", v, sizeof v) == GPTPS_OK && strcmp(v, "4096") == 0);
        CHECK(gptps_settings_set(e, "tui.budget_kb", "0") == GPTPS_OK && health(t).budget_bytes == 0);
        CHECK(gptps_settings_get(e, "tui.budget_kb", v, sizeof v) == GPTPS_OK && strcmp(v, "0") == 0);
        CHECK(gptps_tui_set_budget_kb(t, -1) == GPTPS_E_INVAL);
    }
    h = health(t);
    CHECK(h.budget_refusals == h0.budget_refusals + 14);
    CHECK(log_count("WARN") == 0);
    log_end();
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* a host source with no rows (SOURCE): the view holds room for its rows too */
static gptps_status src_read0(void *ctx, gptps_tui_source_totals *tot, gptps_tui_source_row *rows, size_t row_size,
                              size_t cap, size_t *nrows)
{
    (void)ctx; (void)rows; (void)row_size; (void)cap;
    tot->finished = 7; *nrows = 0;
    return GPTPS_OK;
}
static gptps_tui_source src0(void)
{
    gptps_tui_source s;
    memset(&s, 0, sizeof s);
    s.struct_size = sizeof s; s.read = src_read0; s.name = "s";
    return s;
}

/* 5) */
static void test_buffer_stops(void)
{
    static char frame[300000];
    gptps *e = open_manual();
    gptps_tui *t;
    gptps_tui_config c;
    gptps_tui_health_info h;
    uint64_t f;
    FILE *out;
    if (!e) return;
    deftask(e, "a");
    f = doc_formula(TUI_T_COUNT, 64, 1, 1, 8, 8, 0, 1024, 0, 0);
    /* no room for the view */
    base_cfg(&c, GPTPS_TUI_KPI_MINIMAL, (int)((f + 1023) / 1024 + 4));
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    log_begin();
    tui_fake_ms = 1000000;
    gptps_tui_render(t, frame, sizeof frame);
    h = health(t);
    CHECK(strstr(frame, "no room in tui.budget_kb for the frame") != NULL && t->view == NULL);
    CHECK(h.budget_stops == 1 && h.alloc_failures == 0 && h.runtime_bytes == 0);
    {
        char want[160];
        snprintf(want, sizeof want, "tui.budget_kb %lu: the dashboard frame's view (no dashboard frame) not grown to %lu "
                 "bytes", (unsigned long)((f + 1023) / 1024 + 4), (unsigned long)sizeof(tui_view));
        CHECK(log_count(want) == 1);
        snprintf(want, sizeof want, "would need %lu KiB", (unsigned long)((f + sizeof(tui_view) + 1023) / 1024));
        CHECK(log_count(want) == 1);
    }
    gptps_tui_render(t, frame, sizeof frame);           /* counted again, WARNed once a minute */
    CHECK(health(t).budget_stops == 2 && log_count("not grown") == 1);
    tui_fake_ms += TUI_WARN_GAP_MS;
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(health(t).budget_stops == 3 && log_count("not grown") == 2);
    /* room for the view, not for snapshot's 16 KiB: no frame */
    CHECK(gptps_tui_set_budget_kb(t, (int)((f + doc_view(24) + 1023) / 1024 + 1)) == GPTPS_OK);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(t->view != NULL && strstr(frame, "tasks") != NULL && health(t).budget_stops == 3);
    out = tmpfile();
    if (out) {
        long n;
        t->cfg.out = out;
        tui_fake_ms += TUI_WARN_GAP_MS;
        gptps_tui_snapshot(t);
        n = ftell(out);
        CHECK(n == 0 && health(t).budget_stops == 4 && log_count("gptps_tui_snapshot's frame buffer (no frame)") == 1);
        t->cfg.out = stdout;
        fclose(out);
    }
    /* the tasks pane's cache: "no room", not "no memory" */
    h = health(t);
    CHECK(gptps_tui_set_budget_kb(t, (int)((h.footprint_bytes + h.runtime_bytes + 1023) / 1024)) == GPTPS_OK);
    t->pane = TUI_PANE_TASKS;
    tui_fake_ms += TUI_WARN_GAP_MS;
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "no room in tui.budget_kb to list 1 task types") != NULL && health(t).alloc_failures == 0);
    CHECK(log_count("the tasks pane's cache (fewer task types listed) not grown") == 1);
    t->pane = TUI_PANE_DASH;
    gptps_tui_render(t, frame, sizeof frame);
    /* room for snapshot's 16 KiB, not for a frame that needs more: cut where the buffer ends,
     * counted as truncated and as a stop - not as an allocation failure */
    {
        int k;
        uint64_t tr0, st0;
        for (k = 0; k < 450; ++k) { char nm[32]; snprintf(nm, sizeof nm, "a_task_type_with_a_name_%03d", k); deftask(e, nm); }
        CHECK(gptps_tui_set_budget_kb(t, 0) == GPTPS_OK);
        t->rows = 1000;
        t->pane = TUI_PANE_TASKS;
        gptps_tui_render(t, frame, sizeof frame);       /* the cache, read with no budget */
        CHECK(t->pc.n == 451 && strlen(frame) > TUI_FRAME_MIN);
        h = health(t);
        CHECK(gptps_tui_set_budget_kb(t, (int)((h.footprint_bytes + h.runtime_bytes + TUI_FRAME_MIN + 4096) / 1024))
              == GPTPS_OK);
        tr0 = h.frames_truncated; st0 = h.budget_stops;
        out = tmpfile();
        if (out) {
            t->cfg.out = out;
            tui_fake_ms += TUI_WARN_GAP_MS;
            gptps_tui_snapshot(t);
            h = health(t);
            CHECK(h.budget_stops == st0 + 1 && h.frames_truncated == tr0 + 1 && h.alloc_failures == 0);
            CHECK(ftell(out) > 0 && ftell(out) <= (long)TUI_FRAME_MIN);
            CHECK(log_count("the frame buffer (the frame is cut where it ends) not grown to 32768 bytes") == 1);
            t->cfg.out = stdout;
            fclose(out);
        }
        t->pane = TUI_PANE_DASH; t->rows = 24;
    }
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(holds(t, health(t).footprint_bytes, health(t).runtime_bytes, __LINE__));
    /* a source's rows: no room for them - stopped, counted; the frame still shows its totals */
    {
        gptps_tui_source s = src0();
        uint64_t st0;
        h = health(t);
        CHECK(gptps_tui_set_budget_kb(t, (int)((h.footprint_bytes + h.runtime_bytes + 1023) / 1024)) == GPTPS_OK);
        st0 = h.budget_stops;
        CHECK(gptps_tui_set_source(t, &s) == GPTPS_OK);
        tui_fake_ms += TUI_WARN_GAP_MS;
        gptps_tui_render(t, frame, sizeof frame);
        h = health(t);
        CHECK(h.budget_stops == st0 + 1 && h.alloc_failures == 0 && t->view->srows == NULL &&
              strstr(frame, "source:s") != NULL);
        {
            char want[96];
            snprintf(want, sizeof want, "the source's rows (fewer shown) not grown to %lu bytes",
                     (unsigned long)(24 * sizeof(gptps_tui_source_row)));
            CHECK(log_count(want) == 1);
        }
        CHECK(gptps_tui_set_budget_kb(t, 0) == GPTPS_OK);
        gptps_tui_render(t, frame, sizeof frame);           /* room again: 24 rows of 208 B */
        CHECK(holds(t, health(t).footprint_bytes, doc_view(24) + 24 * sizeof(gptps_tui_source_row), __LINE__));
        CHECK(gptps_tui_set_source(t, NULL) == GPTPS_OK);
    }
    tui_fake_ms = 0;
    log_end();
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 6) */
static void test_adapt(void)
{
    static char frame[65536];
    gptps *e = open_manual();
    gptps_tui *t;
    gptps_tui_config c;
    gptps_tui_health_info h;
    uint64_t f, req;
    long calls;
    int k, w;
    if (!e) return;
    f = doc_formula(TUI_T_FULL, 64, 1, 1, 8, 8, 0, 1024, 0, 0);
    /* REPORT: a stop is a pressure condition, shown and WARNed; nothing shed */
    base_cfg(&c, GPTPS_TUI_KPI_FULL, (int)((f + 1023) / 1024 + 8));
    c.rate_window_s = 1; c.cost_budget_pct = 100;
    tui_fake_ms = 5000000;
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    log_begin();
    gptps_tui_render(t, frame, sizeof frame);           /* a stop (the view) */
    tui_fake_ms += 1000;
    h = health(t);                                      /* the window: BUDGET */
    CHECK(h.pressure == GPTPS_TUI_P_BUDGET && h.shed == 0);
    {
        char want[128];
        snprintf(want, sizeof want, "1 buffer growths stopped at tui.budget_kb %lu: raise it, or a lower tui.kpi",
                 (unsigned long)((f + 1023) / 1024 + 8));
        CHECK(strstr(t->ad_line, want) != NULL);
    }
    CHECK(log_count("gptps_tui: pressure (budget, tui.adapt=report)") == 1);
    gptps_shutdown(e);
    gptps_tui_close(t);
    /* DEGRADE: shed for it until the view fits */
    if ((e = open_manual()) == NULL) return;
    c.adapt = GPTPS_TUI_ADAPT_DEGRADE;
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); log_end(); return; }
    loglen = 0; logbuf[0] = 0;
    req = health(t).footprint_request_bytes;
    CHECK(req == f);
    for (w = 0; w < 40; ++w) {                          /* until a frame stops nothing */
        uint64_t s0 = health(t).budget_stops;
        gptps_tui_render(t, frame, sizeof frame);
        tui_fake_ms += 1000;
        if (t->view && t->view->rows_cap >= 24 && t->view->recs_cap >= 24 && health(t).budget_stops == s0) break;
    }
    CHECK(w < 40);
    h = health(t);
    CHECK(t->view != NULL && h.shed != 0 && (h.shed & GPTPS_TUI_SHED_ACTIVITY));
    CHECK(h.shed_pressure == GPTPS_TUI_P_BUDGET && h.shed_pressure_all == GPTPS_TUI_P_BUDGET);
    CHECK(log_count("tui.adapt=degrade: pressure (budget) for 3 windows in a row; shedding -activity") == 1);
    CHECK(h.footprint_bytes < f && h.footprint_request_bytes == req);
    CHECK(h.footprint_bytes + h.runtime_bytes <= h.budget_bytes && tb_live == h.footprint_bytes + h.runtime_bytes);
    /* the request is what a budget is checked against: below it, refused, though what runs fits */
    loglen = 0; logbuf[0] = 0;
    CHECK(gptps_tui_set_budget_kb(t, (int)((h.footprint_bytes + 1023) / 1024)) == GPTPS_E_INVAL);
    CHECK(log_count("tui.budget_kb=") == 1);
    /* no stop once it fits: nothing more is shed */
    {
        uint64_t s0 = h.shed, st0 = h.budget_stops;
        for (k = 0; k < 5; ++k) { gptps_tui_render(t, frame, sizeof frame); tui_fake_ms += 1000; }
        h = health(t);
        CHECK(h.shed == s0 && h.budget_stops == st0);
    }
    /* the event path: nothing allocated, a budget or not */
    calls = tb_calls;
    for (k = 0; k < 1000; ++k) {
        emit(t, GPTPS_EV_QUEUED, "a", (gptps_handle)(k + 1), 1);
        emit(t, GPTPS_EV_STARTED, "a", (gptps_handle)(k + 1), 1);
        emit(t, GPTPS_EV_FINISHED, "a", (gptps_handle)(k + 1), 2);
    }
    CHECK(tb_calls == calls);
    /* 'u': the request again - it fits the budget, so it is not refused */
    CHECK(gptps_tui_press(t, 'u') == 3 && strstr(t->status, "restored kpi full") != NULL);
    h = health(t);
    CHECK(h.shed == 0 && h.kpi == GPTPS_TUI_KPI_FULL && h.footprint_bytes == f && t->act != NULL);
    tui_fake_ms = 0;
    log_end();
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 5) also: gptps_tui_run on a terminal, with no room for its two 16 KiB frame buffers, makes no
 * frame and returns at once - counted as a stop, not as an allocation failure */
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/select.h>
static void test_run_stopped(void)
{
    gptps *e;
    gptps_tui *t;
    gptps_tui_config c;
    int m = posix_openpt(O_RDWR | O_NOCTTY), s, in0;
    FILE *out;
    uint64_t f;
    if (m < 0 || grantpt(m) != 0 || unlockpt(m) != 0 || (s = open(ptsname(m), O_RDWR | O_NOCTTY)) < 0) {
        printf("note: no pty here; the run() budget check skipped\n");
        if (m >= 0) close(m);
        return;
    }
    in0 = dup(0);
    out = fdopen(s, "w");
    if (!out || in0 < 0 || dup2(s, 0) < 0) { printf("note: cannot set up; skipped\n"); close(m); return; }
    if ((e = open_manual()) == NULL) return;
    f = doc_formula(TUI_T_COUNT, 64, 1, 1, 8, 8, 0, 1024, 0, 0);
    base_cfg(&c, GPTPS_TUI_KPI_MINIMAL, (int)((f + 1023) / 1024 + 20));   /* one 16 KiB buffer fits, two do not */
    c.interactive = 1; c.out = out;
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    if (t) {
        log_begin();
        alarm(10);                                      /* a loop that ran would not return */
        gptps_tui_run(t);
        alarm(0);
        CHECK(health(t).budget_stops == 1 && health(t).alloc_failures == 0 && health(t).frames_painted == 0);
        CHECK(log_count("gptps_tui_run's second frame buffer (the live dashboard does not run) not grown to 16384") == 1);
        CHECK(health(t).runtime_bytes == 0 && tb_live == health(t).footprint_bytes);
        log_end();
    }
    gptps_shutdown(e);
    gptps_tui_close(t);
    dup2(in0, 0); close(in0);
    fclose(out);
    close(m);
}

/* 5) also: a live loop whose output buffer has no room still writes its frame - whole, in
 * pieces - counted as a stop. Not on the simulation HAL: the loop waits in poll(), a real block
 * the simulation does not see (docs/HAL.md). */
#if !defined(GPTPS_TEST_HAL_SIM)
static gptps_tui *ob_t;
static void *ob_run(void *p) { (void)p; gptps_tui_run(ob_t); return NULL; }
static void test_run_output_stopped(void)
{
    static char frame[65536];
    gptps *e;
    gptps_tui_config c;
    pthread_t th;
    struct winsize ws;
    int m = posix_openpt(O_RDWR | O_NOCTTY), s, in0, k;
    FILE *out;
    gptps_tui_health_info h;
    uint64_t sum, pad;
    if (m < 0 || grantpt(m) != 0 || unlockpt(m) != 0 || (s = open(ptsname(m), O_RDWR | O_NOCTTY)) < 0) {
        printf("note: no pty here; the run() output-buffer check skipped\n");
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
    base_cfg(&c, GPTPS_TUI_KPI_MINIMAL, 0);
    c.interactive = 1; c.out = out; c.mode = GPTPS_TUI_ON_DEMAND;
    ob_t = gptps_tui_install(e, &c);
    CHECK(ob_t != NULL);
    if (!ob_t) { gptps_shutdown(e); return; }
    gptps_tui_render(ob_t, frame, sizeof frame);        /* the view, 24 rows */
    /* a payload that makes footprint + view + the two 16 KiB frame buffers a whole KiB: the
     * output buffer then has no room at all */
    sum = health(ob_t).footprint_bytes + 8 * sizeof(tui_ui) + health(ob_t).runtime_bytes + 2 * TUI_FRAME_MIN;
    pad = (1024 - sum % 1024) % 1024;
    {
        static char pl[1024];
        CHECK(gptps_tui_add_task(ob_t, "x", NULL, 0, pad ? pl : NULL, (size_t)pad) == GPTPS_OK);
    }
    CHECK((health(ob_t).footprint_bytes + health(ob_t).runtime_bytes + 2 * TUI_FRAME_MIN) % 1024 == 0);
    CHECK(gptps_tui_set_budget_kb(ob_t, (int)((health(ob_t).footprint_bytes + health(ob_t).runtime_bytes +
                                               2 * TUI_FRAME_MIN) / 1024)) == GPTPS_OK);
    log_begin();
    if (pthread_create(&th, NULL, ob_run, NULL) == 0) {
        char b[4096];
        alarm(60);                                      /* the only bound: no wall-clock limit to miss */
        for (k = 0; health(ob_t).frames_painted == 0; ++k) {
            struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 5000;
            select(0, NULL, NULL, NULL, &tv);
            while (read(m, b, sizeof b) > 0) { }
        }
        alarm(0);
        h = health(ob_t);
        CHECK(h.frames_painted >= 1 && h.budget_stops >= 1 && h.budget_stops <= h.frames_painted &&   /* one a frame */
              h.alloc_failures == 0 && h.bytes_written > 0);
        CHECK(log_count("gptps_tui_run's output buffer (the frame is written whole, in pieces) not grown") == 1);
        gptps_tui_press(ob_t, 'q');
        pthread_join(th, NULL);
        while (read(m, b, sizeof b) > 0) { }
        h = health(ob_t);
        CHECK(h.runtime_bytes == doc_view(24) && tb_live == h.footprint_bytes + h.runtime_bytes);
    }
    log_end();
    gptps_shutdown(e);
    gptps_tui_close(ob_t);
    dup2(in0, 0); close(in0);
    fclose(out);
    close(m);
}
#endif
#endif

/* 5b) A pane pass counts ONE budget stop, however many entries it leaves out (no growth is
 * tried after it), and a real allocation failure in the same pass is still counted. */
static void test_pane_one_stop(void)
{
    static char frame[300000];
    gptps *e = open_manual();
    gptps_tui *t;
    gptps_tui_config c;
    gptps_tui_health_info h;
    uint64_t st0, nm0, b;
    int k;
    if (!e) return;
    for (k = 0; k < 200; ++k) { char nm[32]; snprintf(nm, sizeof nm, "t%03d", k); deftask(e, nm); }
    base_cfg(&c, GPTPS_TUI_KPI_MINIMAL, 0);
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    log_begin();
    tui_fake_ms = 1000000;
    gptps_tui_render(t, frame, sizeof frame);           /* the view */
    /* 200 task types, no room for even 16 cache entries: one stop, 200 not read */
    h = health(t);
    CHECK(gptps_tui_set_budget_kb(t, (int)((h.footprint_bytes + h.runtime_bytes + 1023) / 1024)) == GPTPS_OK);
    st0 = h.budget_stops; nm0 = h.alloc_failures;
    t->pane = TUI_PANE_TASKS;
    gptps_tui_render(t, frame, sizeof frame);
    h = health(t);
    CHECK(h.budget_stops == st0 + 1 && h.alloc_failures == nm0 && h.pane_reads == 1 && t->pc.short_by == 200);
    CHECK(strstr(frame, "no room in tui.budget_kb to list 200 task types") != NULL);
    /* room for 16 entries, not 32; the first 16-entry allocation fails for real: the pane
     * reads 15 (the next entry's try makes the 16), one budget stop, one allocation failure */
    t->pane = TUI_PANE_DASH;
    gptps_tui_render(t, frame, sizeof frame);           /* the cache freed */
    h = health(t);
    CHECK(16 * sizeof(tui_ctask) >= 1024);
    b = (h.footprint_bytes + h.runtime_bytes + 16 * sizeof(tui_ctask) + 1023) / 1024;
    CHECK(b * 1024 < h.footprint_bytes + h.runtime_bytes + 32 * sizeof(tui_ctask));
    CHECK(gptps_tui_set_budget_kb(t, (int)b) == GPTPS_OK);
    st0 = h.budget_stops; nm0 = h.alloc_failures;
    tb_fail_n = 16 * sizeof(tui_ctask);
    t->pane = TUI_PANE_TASKS;
    tui_fake_ms += TUI_WARN_GAP_MS;
    gptps_tui_render(t, frame, sizeof frame);
    tb_fail_n = 0;
    h = health(t);
    CHECK(h.budget_stops == st0 + 1 && h.alloc_failures == nm0 + 1);
    CHECK(t->pc.n == 16 && t->pc.short_by == 184 && t->pc.cap == 16);
    CHECK(strstr(frame, "184 not read: no memory, and no room in tui.budget_kb") != NULL);
    CHECK(log_count("the tasks pane's cache (fewer task types listed) not grown") == 2);
    /* the settings pane: the pool's stop ends the pass's growth too - one stop */
    t->pane = TUI_PANE_DASH;
    gptps_tui_render(t, frame, sizeof frame);
    h = health(t);
    CHECK(gptps_tui_set_budget_kb(t, (int)((h.footprint_bytes + h.runtime_bytes + 1023) / 1024)) == GPTPS_OK);
    st0 = h.budget_stops; nm0 = h.alloc_failures;
    t->pane = TUI_PANE_SETTINGS;
    gptps_tui_render(t, frame, sizeof frame);
    h = health(t);
    CHECK(h.budget_stops == st0 + 1 && h.alloc_failures == nm0 && t->pc.short_by > 16 && t->pc.n < 16);
    CHECK(strstr(frame, "not read: no room in tui.budget_kb") != NULL);
    t->pane = TUI_PANE_DASH;
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(holds(t, health(t).footprint_bytes, health(t).runtime_bytes, __LINE__));
    tui_fake_ms = 0;
    log_end();
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 5c) A settings file, in either order (gptps.h, THE FILE'S ORDER). Before 1.13 the keys
 * applied in the file's order: a reload raising tui.kpi and tui.budget_kb, kpi listed first,
 * had kpi refused (counted, a WARN and an ERROR, GPTPS_E_CONFIG, the old tier with the new
 * budget); one lowering both failed in the other order; at install, a file's kpi was checked
 * against the budget the install had, not the file's. Now each key the budget refuses is
 * tried again after the others, provisionally - not counted, not logged - and only what is
 * still refused at the end is reported, once. */
static const char *RL_PATH = "test_tui_budget_reload.toml";
static void rl_write(const char *text)
{
    FILE *f = fopen(RL_PATH, "w");
    CHECK(f != NULL);
    if (f) { fputs(text, f); fclose(f); }
}
static int rl_is(gptps *e, const char *kpi, const char *budget)
{
    char v[64], w[64];
    if (gptps_settings_get(e, "tui.kpi", v, sizeof v) != GPTPS_OK) return 0;
    if (gptps_settings_get(e, "tui.budget_kb", w, sizeof w) != GPTPS_OK) return 0;
    return !strcmp(v, kpi) && !strcmp(w, budget);
}
/* nothing about the budget in the log: no refusal WARN, no ERROR */
static int rl_quiet(void)
{
    return log_count("needs") == 0 && log_count("refused") == 0 && log_count("is below the footprint") == 0;
}
/* A reload of `text`: the result, and the log it made from here */
static gptps_status rl_reload(gptps *e, const char *text)
{
    loglen = 0; logbuf[0] = 0;
    rl_write(text);
    return gptps_settings_reload(e, RL_PATH);
}
static gptps *rl_open(const char *text)
{
    gptps_config cfg;
    gptps *e = NULL;
    rl_write(text);
    memset(&cfg, 0, sizeof cfg);
    cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
    cfg.limits.max_concurrent_tasks = 1; cfg.limits.max_memory_bytes = 1u << 20;
    cfg.mode = GPTPS_RUN_MANUAL; cfg.config_path = RL_PATH;
    CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
    return e;
}
static void test_reload(void)
{
    static const char *const RAISE[2]  = { "[tui]\nbudget_kb = 200\nkpi = \"full\"\n",
                                           "[tui]\nkpi = \"full\"\nbudget_kb = 200\n" };
    static const char *const LOWER[2]  = { "[tui]\nkpi = \"minimal\"\nbudget_kb = 16\n",
                                           "[tui]\nbudget_kb = 16\nkpi = \"minimal\"\n" };
    /* full cannot fit 16 KiB: the one that comes second is refused, whichever it is */
    static const char *const NOFIT[2]  = { "[tui]\nkpi = \"full\"\nbudget_kb = 16\n",
                                           "[tui]\nbudget_kb = 16\nkpi = \"full\"\n" };
    gptps *e;
    gptps_tui *t;
    gptps_tui_config c;
    int o;
    for (o = 0; o < 2; ++o) {
        /* at install: the install's budget (16 KiB) is below FULL, the file's (200) is not -
         * kpi is checked against the file's budget, in either order */
        e = rl_open(RAISE[o]);
        if (!e) return;
        base_cfg(&c, GPTPS_TUI_KPI_MINIMAL, 16);
        log_begin();
        t = gptps_tui_install(e, &c);
        CHECK(t != NULL);
        if (!t) { log_end(); gptps_shutdown(e); remove(RL_PATH); return; }
        CHECK(rl_is(e, "full", "200") && health(t).budget_refusals == 0 && rl_quiet());
        CHECK(gptps_config_check(e) == GPTPS_OK);
        /* lower both, either order */
        CHECK(rl_reload(e, LOWER[o]) == GPTPS_OK);
        CHECK(rl_is(e, "minimal", "16") && health(t).budget_refusals == 0 && rl_quiet());
        CHECK(rl_reload(e, LOWER[1 - o]) == GPTPS_OK);
        CHECK(rl_is(e, "minimal", "16") && health(t).budget_refusals == 0 && rl_quiet());
        /* raise both, either order */
        CHECK(rl_reload(e, RAISE[o]) == GPTPS_OK);
        CHECK(rl_is(e, "full", "200") && health(t).budget_refusals == 0 && rl_quiet());
        CHECK(rl_reload(e, LOWER[o]) == GPTPS_OK);
        CHECK(rl_reload(e, RAISE[1 - o]) == GPTPS_OK);
        CHECK(rl_is(e, "full", "200") && health(t).budget_refusals == 0 && rl_quiet());
        CHECK(gptps_config_check(e) == GPTPS_OK);
        /* a file that cannot fit: from full/200, budget_kb = 16 is refused (full holds more) -
         * once: one WARN, one count, one ERROR, GPTPS_E_CONFIG; kpi = full is applied (no
         * change) in either order */
        CHECK(rl_reload(e, NOFIT[o]) == GPTPS_E_CONFIG);
        CHECK(rl_is(e, "full", "200") && health(t).budget_refusals == 1);
        CHECK(log_count("tui.budget_kb=16 is below the footprint") == 1 &&
              log_count("tui.budget_kb: 16 was refused") == 1 && log_count("needs") == 1);
        CHECK(gptps_config_check(e) == GPTPS_E_CONFIG);
        /* from minimal/16, the same file: kpi = full is refused once, the budget stays 16 */
        CHECK(rl_reload(e, LOWER[o]) == GPTPS_OK && rl_is(e, "minimal", "16"));
        CHECK(rl_reload(e, NOFIT[1 - o]) == GPTPS_E_CONFIG);
        CHECK(rl_is(e, "minimal", "16") && health(t).budget_refusals == 2);
        CHECK(log_count("tui.kpi=full needs") == 1 && log_count("tui.kpi: full was refused") == 1 &&
              log_count("needs") == 1 && log_count("refused (") == 1);
        log_end();
        gptps_shutdown(e);
        gptps_tui_close(t);

        /* at install, a file that cannot fit: kpi = full against 16 KiB - refused once, in
         * either order. (From an install budget the file lowers, which key the file loses
         * would depend on the order: either can be applied, not both - gptps.h.) */
        e = rl_open(NOFIT[o]);
        if (!e) { remove(RL_PATH); return; }
        base_cfg(&c, GPTPS_TUI_KPI_MINIMAL, 16);
        log_begin();
        t = gptps_tui_install(e, &c);
        CHECK(t != NULL);
        if (!t) { log_end(); gptps_shutdown(e); remove(RL_PATH); return; }
        CHECK(rl_is(e, "minimal", "16") && health(t).budget_refusals == 1);
        CHECK(log_count("tui.kpi=full needs") == 1 && log_count("tui.kpi: full was refused") == 1 &&
              log_count("needs") == 1);
        CHECK(gptps_config_check(e) == GPTPS_E_CONFIG);
        log_end();
        gptps_shutdown(e);
        gptps_tui_close(t);
    }
    remove(RL_PATH);
}

/* 5d) The same for the other footprint keys: tui.recent, tui.activity and
 * tui.latency_window raised or lowered together with tui.budget_kb, in either order - the
 * one listed first, refused provisionally, counts and logs nothing. And a latency window
 * above the cap (clamped, with a WARN) that is refused provisionally and then taken logs
 * that WARN exactly once. */
static void test_reload_sizes(void)
{
    static const struct { const char *key; int small, big; } K[] = {
        { "recent", 8, 4096 }, { "activity", 8, 4096 }, { "latency_window", 1024, 65536 },
    };
    gptps *e;
    gptps_tui *t;
    gptps_tui_config c;
    char text[256];
    size_t k;
    int o;
    e = rl_open("");
    if (!e) return;
    base_cfg(&c, GPTPS_TUI_KPI_FULL, 0);
    log_begin();
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    if (!t) { log_end(); gptps_shutdown(e); remove(RL_PATH); return; }
    for (k = 0; k < sizeof K / sizeof K[0]; ++k) {
        char v[64];
        int lo_kb, hi_kb, cap_kb = 0;
        uint64_t f_small, f_big, r0;
        gptps_tui_health_info h;
        /* calibrate: the footprint at each size, with no budget */
        CHECK(gptps_settings_set(e, "tui.budget_kb", "0") == GPTPS_OK);
        snprintf(v, sizeof v, "%d", K[k].big);
        CHECK(gptps_settings_set(e, k == 0 ? "tui.recent" : k == 1 ? "tui.activity" : "tui.latency_window", v) == GPTPS_OK);
        h = health(t); f_big = h.footprint_bytes + h.runtime_bytes;
        if (k == 2) {                    /* and at the cap */
            CHECK(gptps_tui_set_latency_window(t, TUI_LAT_CAP) == GPTPS_OK);
            h = health(t); cap_kb = (int)((h.footprint_bytes + h.runtime_bytes) / 1024) + 2;
        }
        snprintf(v, sizeof v, "%d", K[k].small);
        CHECK(gptps_settings_set(e, k == 0 ? "tui.recent" : k == 1 ? "tui.activity" : "tui.latency_window", v) == GPTPS_OK);
        h = health(t); f_small = h.footprint_bytes + h.runtime_bytes;
        lo_kb = (int)(f_small / 1024) + 2; hi_kb = (int)(f_big / 1024) + 2;
        CHECK((uint64_t)lo_kb * 1024u < f_big);   /* the big size does not fit the small budget */
        snprintf(v, sizeof v, "%d", lo_kb);
        CHECK(gptps_settings_set(e, "tui.budget_kb", v) == GPTPS_OK);
        r0 = health(t).budget_refusals;
        for (o = 0; o < 2; ++o) {
            char want[64], got[64], bk[64], key[64];
            snprintf(key, sizeof key, "tui.%s", K[k].key);
            /* raise both: the size listed first is refused provisionally, then taken */
            if (o == 0) snprintf(text, sizeof text, "[tui]\nkpi = \"full\"\n%s = %d\nbudget_kb = %d\n", K[k].key, K[k].big, hi_kb);
            else        snprintf(text, sizeof text, "[tui]\nkpi = \"full\"\nbudget_kb = %d\n%s = %d\n", hi_kb, K[k].key, K[k].big);
            CHECK(rl_reload(e, text) == GPTPS_OK);
            snprintf(want, sizeof want, "%d", K[k].big);
            CHECK(gptps_settings_get(e, key, got, sizeof got) == GPTPS_OK && !strcmp(got, want));
            snprintf(want, sizeof want, "%d", hi_kb);
            CHECK(gptps_settings_get(e, "tui.budget_kb", bk, sizeof bk) == GPTPS_OK && !strcmp(bk, want));
            CHECK(health(t).budget_refusals == r0 && rl_quiet());
            /* lower both: the budget listed first is refused provisionally, then taken */
            if (o == 0) snprintf(text, sizeof text, "[tui]\nkpi = \"full\"\nbudget_kb = %d\n%s = %d\n", lo_kb, K[k].key, K[k].small);
            else        snprintf(text, sizeof text, "[tui]\nkpi = \"full\"\n%s = %d\nbudget_kb = %d\n", K[k].key, K[k].small, lo_kb);
            CHECK(rl_reload(e, text) == GPTPS_OK);
            snprintf(want, sizeof want, "%d", K[k].small);
            CHECK(gptps_settings_get(e, key, got, sizeof got) == GPTPS_OK && !strcmp(got, want));
            snprintf(want, sizeof want, "%d", lo_kb);
            CHECK(gptps_settings_get(e, "tui.budget_kb", bk, sizeof bk) == GPTPS_OK && !strcmp(bk, want));
            CHECK(health(t).budget_refusals == r0 && rl_quiet());
        }
        if (k == 2) {
            /* above the cap, listed first: refused provisionally (no WARN, no count), taken
             * after the budget - clamped, one cap WARN; listed last: one WARN too */
            for (o = 0; o < 2; ++o) {
                char got[64], want[64];
                if (o == 0) snprintf(text, sizeof text, "[tui]\nkpi = \"full\"\nlatency_window = %d\nbudget_kb = %d\n", TUI_LAT_CAP + 1, cap_kb);
                else        snprintf(text, sizeof text, "[tui]\nkpi = \"full\"\nbudget_kb = %d\nlatency_window = %d\n", cap_kb, TUI_LAT_CAP + 1);
                CHECK(rl_reload(e, text) == GPTPS_OK);
                snprintf(want, sizeof want, "%d", TUI_LAT_CAP);
                CHECK(gptps_settings_get(e, "tui.latency_window", got, sizeof got) == GPTPS_OK && !strcmp(got, want));
                CHECK(log_count("is above the cap of") == 1 && log_count("needs") == 0 && log_count("refused") == 0);
                CHECK(health(t).budget_refusals == r0);
                /* a provisional try that is TAKEN (it fits the budget the file already has,
                 * and another key follows it): the clamp's WARN, once */
                snprintf(text, sizeof text, "[tui]\nlatency_window = %d\n", K[k].small);
                CHECK(rl_reload(e, text) == GPTPS_OK);
                snprintf(text, sizeof text, "[tui]\nlatency_window = %d\nkpi = \"full\"\n", TUI_LAT_CAP + 1);
                CHECK(rl_reload(e, text) == GPTPS_OK);
                CHECK(gptps_settings_get(e, "tui.latency_window", got, sizeof got) == GPTPS_OK && !strcmp(got, want));
                CHECK(log_count("is above the cap of") == 1 && log_count("needs") == 0 && log_count("refused") == 0);
                CHECK(health(t).budget_refusals == r0);
                snprintf(text, sizeof text, "[tui]\nkpi = \"full\"\nlatency_window = %d\nbudget_kb = %d\n", K[k].small, lo_kb);
                CHECK(rl_reload(e, text) == GPTPS_OK && health(t).budget_refusals == r0 && rl_quiet());
            }
        }
    }
    log_end();
    gptps_shutdown(e);
    gptps_tui_close(t);
    remove(RL_PATH);
}

/* 7) */
static void test_health_layout(void)
{
    gptps *e = open_manual();
    gptps_tui *t;
    gptps_tui_config c;
    union { gptps_tui_health_info h; unsigned char b[sizeof(gptps_tui_health_info)]; } u;
    size_t k, end = offsetof(gptps_tui_health_info, shed_pressure_all) + sizeof(uint64_t);
    if (!e) return;
    base_cfg(&c, GPTPS_TUI_KPI_FULL, 1024);
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    memset(u.b, 0xee, sizeof u.b);
    u.h.struct_size = end;                              /* the layout before BUDGET */
    CHECK(gptps_tui_health(t, &u.h) == GPTPS_OK && u.h.struct_size == end);
    for (k = end; k < sizeof u.b; ++k) if (u.b[k] != 0xee) break;
    CHECK(k == sizeof u.b && end < sizeof u.b);
    u.h.struct_size = sizeof u.h;
    CHECK(gptps_tui_health(t, &u.h) == GPTPS_OK && u.h.budget_bytes == 1024u * 1024u &&
          u.h.footprint_bytes == u.h.footprint_request_bytes && u.h.footprint_bytes > 100000u);
    gptps_shutdown(e);
    gptps_tui_close(t);
}

int main(void)
{
    test_formula();
    test_no_budget();
    test_install_refused();
    test_refusals();
    test_buffer_stops();
    test_pane_one_stop();
    test_reload();
    test_reload_sizes();
#if !defined(_WIN32)
    test_run_stopped();
#  if !defined(GPTPS_TEST_HAL_SIM)
    test_run_output_stopped();
#  endif
#endif
    test_adapt();
    test_health_layout();
    if (fails) { printf("test_tui_budget: %d FAILED\n", fails); return 1; }
    printf("test_tui_budget: OK\n");
    return 0;
}
