/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_tui_source.c - gptps_tui_source (1.12): the dashboard SHOWING a host's numbers, with
 * the add-on compiled in (its clock and its observer in reach). Headless, MANUAL engine.
 *
 *  1) set_source refuses a NULL dashboard, a short struct and a source with no read; a
 *     newer, larger struct is fine; NULL clears.
 *  2) A frame shows the source's totals, gauges (EXACT: no mark, LOWER_BOUND '>=', APPROX
 *     '~', SUM: MINIMAL's one sum; anything else '~'), done/s (at kpi off too) and peak,
 *     its TASKS rows in its order with the add_task label and hotkey for a name, avg / max
 *     ms only for a row that has latency, "+N more" past what fits, a name with no NUL cut
 *     at its last byte; "source:<name>" on the kpi line, cleaned.
 *  3) Read only at a dashboard frame's copy-out: 30k events, health, the tasks pane, set_kpi
 *     and 'm' read nothing; render and snapshot read once each; the event path still
 *     allocates nothing with a source set.
 *  4) A failed read is counted (health), shown ("read failed ... own counts shown", the
 *     dashboard's own counts), WARNed at most once a minute; the next good read clears the
 *     line and keeps the count.
 *  5) Once set_source(NULL) / another source / close has returned the old one is never
 *     called again (POSIX: while another thread renders in a loop, the ctx is freed right
 *     after set_source returns); the dashboard never writes to the ctx (no reset, no close).
 *  6) done/s and peak start again when the totals change hands (own -> source -> own) and
 *     when the source's count goes back (its host reset it): no wrapped, huge rate.
 *  7) While a source is set the frame is due again every refresh_ms (the live loop polls).
 *  8) ABI: the health fields are appended after render_max_us, and a struct_size that ends
 *     there gets none of them; the source structs start with struct_size.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include <stdlib.h>
static int src_allocs;                 /* every allocation the add-on makes (3: none on the event path) */
static void *src_test_calloc(size_t n, size_t s) { __atomic_add_fetch(&src_allocs, 1, __ATOMIC_SEQ_CST); return calloc(n, s); }
static void *src_test_malloc(size_t n) { __atomic_add_fetch(&src_allocs, 1, __ATOMIC_SEQ_CST); return malloc(n); }
static void *src_test_realloc(void *p, size_t n) { __atomic_add_fetch(&src_allocs, 1, __ATOMIC_SEQ_CST); return realloc(p, n); }
#include "gptps.h"
static uint64_t tui_fake_ms;           /* the dashboard's clock (0: the real one) */
#define TUI_NOW_MS() (tui_fake_ms ? tui_fake_ms : gptps_now_ms(NULL))
#define TUI_FRAME_US(us) (tui_fake_ms ? 0u : (us))
#define calloc(n, s) src_test_calloc((n), (s))
#define malloc(n) src_test_malloc((n))
#define realloc(p, n) src_test_realloc((p), (n))
#include "../addons/gptps_tui.c"
#undef calloc
#undef malloc
#undef realloc
#include <stdio.h>
#if !defined(_WIN32)
#  include <pthread.h>
#endif

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

/* ---- the log (WARN and up) ---- */
static char logbuf[16384];
static size_t loglen;
static void cap_sink(gptps_log_level lvl, const char *msg, void *u)
{
    size_t n = strlen(msg);
    (void)u;
    if (lvl < GPTPS_LOG_WARN) return;
    if (loglen + n + 2 < sizeof logbuf) { memcpy(logbuf + loglen, msg, n); loglen += n; logbuf[loglen++] = '\n'; logbuf[loglen] = 0; }
}
static void log_begin(void) { loglen = 0; logbuf[0] = 0; gptps_set_log_sink(cap_sink, NULL); }
static void log_end(void) { gptps_set_log_sink(NULL, NULL); }
static int log_count(const char *needle)
{
    int c = 0;
    const char *p = logbuf;
    while ((p = strstr(p, needle)) != NULL) { ++c; p += strlen(needle); }
    return c;
}

/* ---- a scripted source ---- */
typedef struct {
    gptps_tui_source_totals tot;
    gptps_tui_source_row    rows[64];
    size_t                  nrows;       /* rows it says it has (may exceed what it holds) */
    gptps_status            ret;
    int                     reads;       /* an int: read from another thread in 5) */
    size_t                  last_cap, last_row_size, last_tot_size;
    int                     freed;       /* 5): set once the host has "freed" it */
    int                     delay_ms;    /* 5): each read takes this long (real time) */
    int                     hold;        /* 5): a read waits, inside, until `release` */
    int                     in_read, release;
    int                     bogus_size;  /* 2): each read writes nonsense into totals->struct_size */
    unsigned char           guard[32];   /* 5): never written by the dashboard */
} script;

static gptps_status script_read(void *ctx, gptps_tui_source_totals *totals, gptps_tui_source_row *rows,
                                size_t row_size, size_t cap, size_t *nrows)
{
    script *sc = (script *)ctx;
    size_t i, n;
    CHECK(!__atomic_load_n(&sc->freed, __ATOMIC_SEQ_CST));   /* never called once set_source returned */
    __atomic_add_fetch(&sc->reads, 1, __ATOMIC_SEQ_CST);
    sc->last_cap = cap; sc->last_row_size = row_size; sc->last_tot_size = totals->struct_size;
    if (__atomic_load_n(&sc->hold, __ATOMIC_SEQ_CST)) {   /* held inside, until the test lets go */
        __atomic_store_n(&sc->in_read, 1, __ATOMIC_SEQ_CST);
        while (!__atomic_load_n(&sc->release, __ATOMIC_SEQ_CST)) (void)gptps_now_ms(NULL);
        CHECK(!__atomic_load_n(&sc->freed, __ATOMIC_SEQ_CST));
    }
    if (sc->delay_ms) {                  /* a slow read: set_source must wait for it to end */
        uint64_t end = gptps_now_ms(NULL) + (uint64_t)sc->delay_ms;
        while (gptps_now_ms(NULL) < end) { }
        CHECK(!__atomic_load_n(&sc->freed, __ATOMIC_SEQ_CST));   /* still the host's at its end */
    }
    if (sc->ret != GPTPS_OK) return sc->ret;
    memcpy((char *)totals + sizeof(size_t), (char *)&sc->tot + sizeof(size_t), sizeof sc->tot - sizeof(size_t));
    n = sc->nrows < cap ? sc->nrows : cap;
    if (n > 64) n = 64;
    for (i = 0; i < n; ++i) memcpy((char *)rows + i * row_size, &sc->rows[i], sizeof sc->rows[i]);
    if (sc->bogus_size) totals->struct_size = 3;   /* a source that writes the field it should not */
    *nrows = sc->nrows;
    return GPTPS_OK;
}
static int nreads(script *sc) { return __atomic_load_n(&sc->reads, __ATOMIC_SEQ_CST); }

static void script_row(script *sc, size_t i, const char *name, uint64_t ok, uint64_t fail, int lat)
{
    gptps_tui_source_row *r = &sc->rows[i];
    memset(r, 0, sizeof *r);
    r->struct_size = sizeof *r;
    snprintf(r->name, sizeof r->name, "%s", name);
    r->started = ok + fail; r->finished = ok; r->failed = fail;
    r->lat_n = ok; r->lat_sum_ms = ok * 12; r->lat_max_ms = 77;   /* shown only with the flag */
    if (lat) r->flags = GPTPS_TUI_SRC_ROW_LATENCY;
}

static gptps_tui_source src_of(script *sc, const char *name)
{
    gptps_tui_source s;
    memset(&s, 0, sizeof s);
    s.struct_size = sizeof s; s.ctx = sc; s.read = script_read; s.name = name;
    return s;
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
static gptps_tui *install_kpi(gptps *e, int kpi, int cols)
{
    gptps_tui_config c;
    gptps_tui *t;
    memset(&c, 0, sizeof c);
    c.struct_size = sizeof c; c.color = 0; c.interactive = 0; c.unicode = 0; c.kpi = kpi;
    c.clock = GPTPS_TUI_CLOCK_OFF; c.rate_window_s = 10; c.refresh_ms = 200;
    t = gptps_tui_install(e, &c);
    if (t && cols) t->cols = cols;
    return t;
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
    ev.status = GPTPS_OK; ev.ts_ms = TUI_NOW_MS();
    tui_on_event(&ev, t);
}
static void lifecycles(gptps_tui *t, int n, gptps_handle *h)
{
    int i;
    for (i = 0; i < n; ++i, ++*h) {
        emit(t, GPTPS_EV_QUEUED, "own", *h); emit(t, GPTPS_EV_STARTED, "own", *h); emit(t, GPTPS_EV_FINISHED, "own", *h);
    }
}
static void totals(script *sc, uint64_t q, uint64_t fin, uint64_t pend, uint64_t run, int gauges)
{
    memset(&sc->tot, 0, sizeof sc->tot);
    sc->tot.struct_size = sizeof sc->tot;
    sc->tot.queued = q; sc->tot.started = fin + run; sc->tot.finished = fin; sc->tot.failed = 4; sc->tot.retried = 2;
    sc->tot.dead = 1; sc->tot.dropped = 3; sc->tot.cancelled = 1; sc->tot.pending = pend; sc->tot.in_flight = run;
    sc->tot.gauges = gauges;
}

/* 1) */
static void test_validation(void)
{
    gptps *e = open_manual(); gptps_tui *t; script sc; gptps_tui_source s;
    struct { gptps_tui_source s; char more[16]; } big;
    if (!e) return;
    t = install_kpi(e, GPTPS_TUI_KPI_OFF, 0);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    memset(&sc, 0, sizeof sc);
    s = src_of(&sc, "x");
    CHECK(gptps_tui_set_source(NULL, &s) == GPTPS_E_INVAL);
    s.struct_size = sizeof s - 1;
    CHECK(gptps_tui_set_source(t, &s) == GPTPS_E_INVAL && t->src.read == NULL);
    s = src_of(&sc, "x"); s.read = NULL;
    CHECK(gptps_tui_set_source(t, &s) == GPTPS_E_INVAL && t->src.read == NULL);
    memset(&big, 0x5a, sizeof big);
    big.s = src_of(&sc, NULL); big.s.struct_size = sizeof big;   /* a later, larger struct */
    CHECK(gptps_tui_set_source(t, &big.s) == GPTPS_OK && t->src.read == script_read && strcmp(t->src_name, "host") == 0);
    CHECK(health(t).source == 1);
    CHECK(gptps_tui_set_source(t, NULL) == GPTPS_OK && t->src.read == NULL && health(t).source == 0);
    gptps_shutdown(e); gptps_tui_close(t);
}

/* 2) */
static void test_frame(void)
{
    gptps *e = open_manual(); gptps_tui *t; script sc; gptps_tui_source s;
    static char frame[32768];
    gptps_tui_health_info h;
    char *p;
    if (!e) return;
    tui_fake_ms = 5000000u;
    t = install_kpi(e, GPTPS_TUI_KPI_OFF, 100);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); tui_fake_ms = 0; return; }
    CHECK(gptps_tui_add_task(t, "resize", "Resize images", 'r', NULL, 0) == GPTPS_OK);
    memset(&sc, 0, sizeof sc);
    totals(&sc, 1234, 1000, 7, 3, GPTPS_TUI_GAUGES_EXACT);
    script_row(&sc, 0, "resize", 900, 2, 1);
    script_row(&sc, 1, "upload", 100, 2, 0);
    sc.nrows = 2;
    s = src_of(&sc, "stats\x1b[31m");   /* a host's name is shown cleaned */
    CHECK(gptps_tui_set_source(t, &s) == GPTPS_OK);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(sc.reads == 1 && sc.last_row_size == sizeof(gptps_tui_source_row) &&
          sc.last_tot_size == sizeof(gptps_tui_source_totals) && sc.last_cap >= 20);
    CHECK(strstr(frame, "queued 1234  started 1003  pending 7  in-flight 3  [") != NULL);   /* EXACT: no mark */
    CHECK(strstr(frame, "finished 1000  failed 4 (1 cancelled)  retried 2  dead 1  dropped 3") != NULL);
    CHECK(strstr(frame, "kpi:off mode:realtime refresh:200ms source:stats [31m") != NULL && strchr(frame, '\x1b') == NULL);
    CHECK(strstr(frame, "peak 3") != NULL);
    CHECK(strstr(frame, "n/a done/s") == NULL && strstr(frame, "- done/s") != NULL);   /* at OFF: a rate to come, not n/a */
    p = strstr(frame, "Resize images");
    CHECK(p != NULL && strstr(frame, "[r]") != NULL);
    /* avg / max ms only for the row that has latency, max ms because 100 columns have room */
    CHECK(strstr(frame, "   max ms") != NULL);
    CHECK(p && strstr(p, "     12.0       77") != NULL);
    p = strstr(frame, "  upload");
    CHECK(p != NULL && p && strstr(p, "      --       --       --") != NULL);
    CHECK(strstr(frame, "TASKS") != NULL && strstr(frame, "no memory for") == NULL);
    h = health(t);
    CHECK(h.source == 1 && h.source_reads == 1 && h.source_errors == 0 && h.source_rows == 2 && h.source_status == GPTPS_OK);
    CHECK(sc.reads == 1);                /* health reads nothing from the source */
    /* done/s from the source's count, 2 s later */
    tui_fake_ms += 2000u; totals(&sc, 1234, 1100, 7, 3, GPTPS_TUI_GAUGES_EXACT);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "50.0 done/s") != NULL);
    /* the marks */
    totals(&sc, 1234, 1100, 7, 3, GPTPS_TUI_GAUGES_LOWER_BOUND); gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "pending >=7  in-flight >=3") != NULL);
    totals(&sc, 1234, 1100, 7, 3, GPTPS_TUI_GAUGES_APPROX); gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "pending ~7  in-flight ~3") != NULL);
    totals(&sc, 1234, 1100, 7, 3, 77); gptps_tui_render(t, frame, sizeof frame);   /* nonsense: approximate */
    CHECK(strstr(frame, "pending ~7  in-flight ~3") != NULL);
    totals(&sc, 1234, 1100, 10, 0, GPTPS_TUI_GAUGES_SUM); gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "pending+in-flight ~10") != NULL && strstr(frame, "] peak") == NULL);
    /* a name with no NUL: cut at its last byte; more rows than fit: "+N more" */
    totals(&sc, 1, 1, 0, 0, GPTPS_TUI_GAUGES_EXACT);
    memset(sc.rows[1].name, 'z', sizeof sc.rows[1].name);
    sc.nrows = 500;
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "zzzzzzzzzzzzzzz") != NULL);
    p = strstr(frame, "+");
    CHECK(p != NULL && strstr(frame, " more (a taller terminal shows them)") != NULL);
    {   /* the frame still fits 24 rows */
        int lines = 0; const char *c;
        for (c = frame; *c; ++c) lines += *c == '\n';
        CHECK(lines <= 24);
    }
    CHECK(health(t).source_rows == 500);
    /* the unterminated name is cut at exactly its last byte: 127 'z's, which is a task added
     * with a label, so the TASKS row shows the label - the bytes after the name (padding,
     * started) are not 0 either, so an uncut name runs on past them, matches nothing and is
     * shown raw */
    {
        char zn[GPTPS_TASK_NAME_MAX + 1];
        size_t off = offsetof(gptps_tui_source_row, name);
        memset(zn, 'z', GPTPS_TASK_NAME_MAX); zn[GPTPS_TASK_NAME_MAX] = 0;
        CHECK(gptps_tui_add_task(t, zn, "Zed label", 'z', NULL, 0) == GPTPS_OK);
        memset((char *)&sc.rows[1] + off, 'z', offsetof(gptps_tui_source_row, finished) - off);
        sc.nrows = 2;
        gptps_tui_render(t, frame, sizeof frame);
        CHECK(strstr(frame, "Zed label") != NULL && strstr(frame, "[z]") != NULL);
        CHECK(strstr(frame, "zzzzzzzzzzzzzzz") == NULL);   /* the row is the label, not the raw name */
    }
    /* a source that writes totals->struct_size: the next read is handed the right size again,
     * and its totals are still shown */
    script_row(&sc, 1, "upload", 100, 2, 0);
    totals(&sc, 4321, 4000, 0, 0, GPTPS_TUI_GAUGES_EXACT);
    sc.bogus_size = 1;
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "queued 4321 ") != NULL);
    sc.last_tot_size = 0;
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(sc.last_tot_size == sizeof(gptps_tui_source_totals) && strstr(frame, "queued 4321 ") != NULL);
    sc.bogus_size = 0;
    gptps_shutdown(e); gptps_tui_close(t);
    tui_fake_ms = 0;
}

/* 3) */
static void test_read_only_at_render(void)
{
    gptps *e = open_manual(); gptps_tui *t; script sc; gptps_tui_source s;
    static char frame[32768];
    gptps_handle hd = 1;
    int a0;
    FILE *out;
    if (!e) return;
    t = install_kpi(e, GPTPS_TUI_KPI_FULL, 0);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    out = tmpfile();
    if (out) t->cfg.out = out;
    memset(&sc, 0, sizeof sc);
    totals(&sc, 10, 9, 0, 1, GPTPS_TUI_GAUGES_EXACT); sc.nrows = 0;
    s = src_of(&sc, "s");
    CHECK(gptps_tui_set_source(t, &s) == GPTPS_OK);
    gptps_tui_render(t, frame, sizeof frame);          /* builds the view and its rows first */
    CHECK(nreads(&sc) == 1);
    a0 = __atomic_load_n(&src_allocs, __ATOMIC_SEQ_CST);
    lifecycles(t, 10000, &hd);                          /* 30k events at FULL with a source */
    CHECK(__atomic_load_n(&src_allocs, __ATOMIC_SEQ_CST) == a0);   /* the event path allocates nothing */
    CHECK(nreads(&sc) == 1);                            /* and reads nothing */
    (void)health(t);
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_LATENCY) == GPTPS_OK);
    CHECK(gptps_tui_press(t, 'm') == 3);
    CHECK(gptps_tui_set_adapt(t, GPTPS_TUI_ADAPT_DEGRADE) == GPTPS_OK);
    CHECK(nreads(&sc) == 1);
    CHECK(gptps_tui_press(t, 't') == 4);               /* the tasks pane reads the engine, not the source */
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(nreads(&sc) == 1 && strstr(frame, "source:") == NULL);
    t->pane = TUI_PANE_DASH;                            /* back to the dashboard */
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(nreads(&sc) == 2 && strstr(frame, "source:s") != NULL);
    if (out) { gptps_tui_snapshot(t); CHECK(nreads(&sc) == 3); }
    CHECK(health(t).source_reads == (uint64_t)nreads(&sc));
    gptps_shutdown(e); gptps_tui_close(t);
    CHECK(nreads(&sc) == (out ? 3 : 2));               /* close reads nothing */
    if (out) fclose(out);
}

/* 4) */
static void test_errors(void)
{
    gptps *e = open_manual(); gptps_tui *t; script sc; gptps_tui_source s;
    static char frame[32768];
    gptps_tui_health_info h;
    gptps_handle hd = 1;
    if (!e) return;
    tui_fake_ms = 7000000u;
    t = install_kpi(e, GPTPS_TUI_KPI_MINIMAL, 0);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); tui_fake_ms = 0; return; }
    lifecycles(t, 5, &hd);                              /* the dashboard's own: finished 5 */
    memset(&sc, 0, sizeof sc);
    totals(&sc, 900, 800, 0, 0, GPTPS_TUI_GAUGES_EXACT);
    sc.ret = GPTPS_E_IO;
    s = src_of(&sc, "stats");
    CHECK(gptps_tui_set_source(t, &s) == GPTPS_OK);
    log_begin();
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "source stats: read failed (") != NULL && strstr(frame, "1 errors - own counts shown") != NULL);
    CHECK(strstr(frame, "finished 5 ") != NULL && strstr(frame, "finished 800") == NULL);   /* its own counts */
    CHECK(strstr(frame, "source:stats") != NULL);
    h = health(t);
    CHECK(h.source_errors == 1 && h.source_reads == 1 && h.source_status == GPTPS_E_IO && h.source_rows == 0);
    CHECK(log_count("source 'stats' failed to read") == 1);
    tui_fake_ms += 30000u;                              /* 30 s on: counted, not WARNed again */
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(health(t).source_errors == 2 && log_count("source 'stats' failed to read") == 1);
    CHECK(strstr(frame, "2 errors - own counts shown") != NULL);
    tui_fake_ms += 30000u;                              /* 60 s after the first: one more */
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(health(t).source_errors == 3 && log_count("source 'stats' failed to read") == 2);
    log_end();
    sc.ret = GPTPS_OK;                                  /* it recovers: its counts, no line */
    gptps_tui_render(t, frame, sizeof frame);
    h = health(t);
    CHECK(strstr(frame, "read failed") == NULL && strstr(frame, "finished 800") != NULL);
    CHECK(h.source_errors == 3 && h.source_reads == 4 && h.source_status == GPTPS_OK);
    /* the WARN shows a host's name cleaned, as the frame does: no escape reaches the log sink */
    s = src_of(&sc, "a\x1b[2J");
    CHECK(gptps_tui_set_source(t, &s) == GPTPS_OK);
    sc.ret = GPTPS_E_IO;
    tui_fake_ms += 61000u;
    log_begin();
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(log_count("failed to read") == 1 && log_count("source 'a [2J' failed to read") == 1);
    CHECK(strchr(logbuf, '\x1b') == NULL);
    log_end();
    gptps_shutdown(e); gptps_tui_close(t);
    tui_fake_ms = 0;
}

#if !defined(_WIN32)
typedef struct { gptps_tui *t; int stop; int frames; } render_loop;
typedef struct { gptps_tui *t; int returned; } clear_call;
static void *clear_thread(void *p)
{
    clear_call *cc = (clear_call *)p;
    CHECK(gptps_tui_set_source(cc->t, NULL) == GPTPS_OK);
    __atomic_store_n(&cc->returned, 1, __ATOMIC_SEQ_CST);
    return NULL;
}
static void *render_thread(void *p)
{
    render_loop *rl = (render_loop *)p;
    static char fr[32768];
    do {                                 /* at least one frame */
        uint64_t ms;
        gptps_tui_render(rl->t, fr, sizeof fr);
        __atomic_add_fetch(&rl->frames, 1, __ATOMIC_SEQ_CST);
        /* a gap between frames, as a live loop has: a mutex is not fair, and set_source would
         * wait behind back-to-back frames (the HAL call lets the simulation HAL switch) */
        ms = gptps_now_ms(NULL);
        while (gptps_now_ms(NULL) == ms) { }
    } while (!__atomic_load_n(&rl->stop, __ATOMIC_SEQ_CST));
    return NULL;
}
#endif

/* 5) */
static void test_ownership(void)
{
    gptps *e = open_manual(); gptps_tui *t; script *a, *b; gptps_tui_source s;
    static char frame[32768];
    unsigned char zero[32];
    int ra;
    if (!e) return;
    t = install_kpi(e, GPTPS_TUI_KPI_GAUGE, 0);
    CHECK(t != NULL);
    a = (script *)calloc(1, sizeof *a); b = (script *)calloc(1, sizeof *b);
    if (!t || !a || !b) { gptps_shutdown(e); if (t) gptps_tui_close(t); free(a); free(b); return; }
    totals(a, 3, 2, 0, 1, GPTPS_TUI_GAUGES_EXACT); totals(b, 30, 20, 0, 1, GPTPS_TUI_GAUGES_EXACT);
    s = src_of(a, "a");
    CHECK(gptps_tui_set_source(t, &s) == GPTPS_OK);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(nreads(a) == 1 && strstr(frame, "source:a") != NULL);
    s = src_of(b, "b");                                 /* another: a is never called again */
    CHECK(gptps_tui_set_source(t, &s) == GPTPS_OK);
    __atomic_store_n(&a->freed, 1, __ATOMIC_SEQ_CST);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(nreads(a) == 1 && nreads(b) == 1 && strstr(frame, "source:b") != NULL && strstr(frame, "finished 20") != NULL);
#if !defined(_WIN32)
    {   /* set_source waits for a read in progress: it has not returned while one is inside */
        render_loop rl;
        clear_call cc;
        pthread_t th, th2;
        script *c = (script *)calloc(1, sizeof *c);
        if (c) {
            uint64_t until;
            totals(c, 1, 1, 0, 0, GPTPS_TUI_GAUGES_EXACT);
            __atomic_store_n(&c->hold, 1, __ATOMIC_SEQ_CST);
            s = src_of(c, "c");
            CHECK(gptps_tui_set_source(t, &s) == GPTPS_OK);
            memset(&rl, 0, sizeof rl); rl.t = t;
            memset(&cc, 0, sizeof cc); cc.t = t;
            __atomic_store_n(&rl.stop, 1, __ATOMIC_SEQ_CST);     /* one frame: the held one */
            if (pthread_create(&th, NULL, render_thread, &rl) == 0) {
                while (!__atomic_load_n(&c->in_read, __ATOMIC_SEQ_CST)) (void)gptps_now_ms(NULL);
                if (pthread_create(&th2, NULL, clear_thread, &cc) == 0) {
                    until = gptps_now_ms(NULL) + 30;
                    while (gptps_now_ms(NULL) < until) { }
                    CHECK(!__atomic_load_n(&cc.returned, __ATOMIC_SEQ_CST));   /* still waiting for the read */
                    __atomic_store_n(&c->release, 1, __ATOMIC_SEQ_CST);
                    pthread_join(th2, NULL);
                    CHECK(__atomic_load_n(&cc.returned, __ATOMIC_SEQ_CST));
                    __atomic_store_n(&c->freed, 1, __ATOMIC_SEQ_CST);   /* the host may free it now */
                } else __atomic_store_n(&c->release, 1, __ATOMIC_SEQ_CST);
                pthread_join(th, NULL);
            }
            CHECK(gptps_tui_set_source(t, NULL) == GPTPS_OK);
            free(c);
        }
    }
    {   /* while another thread renders: once set_source has returned, the ctx may go */
        render_loop rl;
        pthread_t th;
        int round;
        for (round = 0; round < 20; ++round) {
            script *c = (script *)calloc(1, sizeof *c);
            if (!c) break;
            totals(c, 1, 1, 0, 0, GPTPS_TUI_GAUGES_EXACT);
            c->delay_ms = 1;
            s = src_of(c, "c");
            CHECK(gptps_tui_set_source(t, &s) == GPTPS_OK);
            memset(&rl, 0, sizeof rl); rl.t = t;
            if (pthread_create(&th, NULL, render_thread, &rl) != 0) { free(c); break; }
            /* spinning with a HAL call in the loop: the simulation HAL switches only at one */
            while (__atomic_load_n(&rl.frames, __ATOMIC_SEQ_CST) < 3) (void)gptps_now_ms(NULL);
            CHECK(gptps_tui_set_source(t, round & 1 ? NULL : &s) == GPTPS_OK);
            if (round & 1) {
                ra = nreads(c);
                __atomic_store_n(&c->freed, 1, __ATOMIC_SEQ_CST);   /* the host frees it now */
                while (__atomic_load_n(&rl.frames, __ATOMIC_SEQ_CST) < 6) (void)gptps_now_ms(NULL);
                CHECK(nreads(c) == ra);
            }
            __atomic_store_n(&rl.stop, 1, __ATOMIC_SEQ_CST);
            pthread_join(th, NULL);
            CHECK(gptps_tui_set_source(t, NULL) == GPTPS_OK);
            free(c);
        }
    }
#endif
    s = src_of(b, "b");
    CHECK(gptps_tui_set_source(t, &s) == GPTPS_OK);
    gptps_tui_render(t, frame, sizeof frame);
    ra = nreads(b);
    memset(zero, 0, sizeof zero);
    CHECK(memcmp(b->guard, zero, sizeof zero) == 0);    /* the dashboard never wrote to the host's object */
    gptps_shutdown(e);
    gptps_tui_close(t);                                 /* reads nothing, closes nothing of the host's */
    CHECK(nreads(b) == ra && b->tot.finished == 20 && memcmp(b->guard, zero, sizeof zero) == 0);
    free(a); free(b);
}

/* 6) */
static void test_rate_hands(void)
{
    gptps *e = open_manual(); gptps_tui *t; script sc; gptps_tui_source s;
    static char frame[32768];
    gptps_handle hd = 1;
    if (!e) return;
    tui_fake_ms = 9000000u;
    t = install_kpi(e, GPTPS_TUI_KPI_GAUGE, 0);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); tui_fake_ms = 0; return; }
    lifecycles(t, 1000, &hd);                           /* own: finished 1000 */
    gptps_tui_render(t, frame, sizeof frame);
    tui_fake_ms += 1000u; gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "0.0 done/s") != NULL);
    memset(&sc, 0, sizeof sc);
    totals(&sc, 10, 5, 0, 0, GPTPS_TUI_GAUGES_EXACT);
    s = src_of(&sc, "s");
    CHECK(gptps_tui_set_source(t, &s) == GPTPS_OK);
    tui_fake_ms += 1000u; gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "- done/s") != NULL);          /* starts again: nothing to measure against yet */
    tui_fake_ms += 1000u; totals(&sc, 20, 15, 0, 0, GPTPS_TUI_GAUGES_EXACT); gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "10.0 done/s") != NULL);       /* (15 - 5) / 1 s - never 15 - 1000 */
    tui_fake_ms += 1000u; totals(&sc, 3, 2, 0, 0, GPTPS_TUI_GAUGES_EXACT); gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "- done/s") != NULL);          /* the host reset its totals: from here */
    tui_fake_ms += 1000u; totals(&sc, 9, 6, 0, 0, GPTPS_TUI_GAUGES_EXACT); gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "4.0 done/s") != NULL);
    /* peak: the source's in-flight, in buckets of its own */
    totals(&sc, 9, 6, 0, 40, GPTPS_TUI_GAUGES_EXACT); gptps_tui_render(t, frame, sizeof frame);
    totals(&sc, 9, 6, 0, 2, GPTPS_TUI_GAUGES_EXACT); gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "peak 40") != NULL);
    CHECK(gptps_tui_set_source(t, NULL) == GPTPS_OK);   /* back to its own: starts again, too */
    tui_fake_ms += 1000u; gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "- done/s") != NULL && strstr(frame, "finished 1000") != NULL && strstr(frame, "peak 40") == NULL);
    lifecycles(t, 30, &hd);
    tui_fake_ms += 1000u; gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "30.0 done/s") != NULL);
    gptps_shutdown(e); gptps_tui_close(t);
    tui_fake_ms = 0;
}

/* 7) */
static void test_polled(void)
{
    gptps *e = open_manual(); gptps_tui *t; script sc; gptps_tui_source s;
    static char frame[32768];
    if (!e) return;
    tui_fake_ms = 11000000u;
    t = install_kpi(e, GPTPS_TUI_KPI_OFF, 0);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); tui_fake_ms = 0; return; }
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(t->view && t->view->next_change == 0);         /* idle, no source: no frame is due */
    memset(&sc, 0, sizeof sc);
    totals(&sc, 1, 1, 0, 0, GPTPS_TUI_GAUGES_EXACT);
    s = src_of(&sc, "s");
    CHECK(gptps_tui_set_source(t, &s) == GPTPS_OK);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(t->view && t->view->next_change && t->view->next_change <= tui_fake_ms + 200u);   /* refresh_ms */
    tui_fake_ms += 1000u;                                /* done/s settled, nothing else changes with time */
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "0.0 done/s") != NULL);
    CHECK(t->view && t->view->next_change == tui_fake_ms + 200u);   /* only the poll: due in refresh_ms */
    CHECK(gptps_tui_set_source(t, NULL) == GPTPS_OK);
    tui_fake_ms += 1000u; gptps_tui_render(t, frame, sizeof frame);
    CHECK(t->view && t->view->next_change == 0);         /* no source: no poll */
    gptps_shutdown(e); gptps_tui_close(t);
    tui_fake_ms = 0;
}

/* 8) */
#define SRC_ASSERT(name, c) typedef char src_assert_##name[(c) ? 1 : -1]
SRC_ASSERT(h_source, offsetof(gptps_tui_health_info, source) == offsetof(gptps_tui_health_info, render_max_us) + 8);
SRC_ASSERT(h_status, offsetof(gptps_tui_health_info, source_status) == offsetof(gptps_tui_health_info, source) + sizeof(int));
SRC_ASSERT(h_reads, offsetof(gptps_tui_health_info, source_reads) == offsetof(gptps_tui_health_info, render_max_us) + 16);
/* 1.12's struct ended at source_rows; 1.13 appends after it (a 1.12 caller's struct_size reads none of it) */
SRC_ASSERT(h_end, offsetof(gptps_tui_health_info, pane_shed_ms) == offsetof(gptps_tui_health_info, source_rows) + 8);
SRC_ASSERT(h_spa, offsetof(gptps_tui_health_info, shed_pressure_all) == offsetof(gptps_tui_health_info, pane_shed_ms) + 8);
/* 1.13's end: a later release appends after shed_pressure_all and moves this */
SRC_ASSERT(h_size, sizeof(gptps_tui_health_info) == offsetof(gptps_tui_health_info, shed_pressure_all) + 8);
SRC_ASSERT(src_ss, offsetof(gptps_tui_source, struct_size) == 0 && offsetof(gptps_tui_source_totals, struct_size) == 0 &&
                   offsetof(gptps_tui_source_row, struct_size) == 0);
static void test_health_size(void)
{
    gptps *e = open_manual(); gptps_tui *t; script sc; gptps_tui_source s;
    static char frame[32768];
    union { gptps_tui_health_info h; unsigned char b[sizeof(gptps_tui_health_info)]; } u;
    size_t k, end = offsetof(gptps_tui_health_info, render_max_us) + sizeof(uint64_t);
    if (!e) return;
    t = install_kpi(e, GPTPS_TUI_KPI_OFF, 0);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    memset(&sc, 0, sizeof sc); sc.ret = GPTPS_E_IO;
    s = src_of(&sc, "s");
    CHECK(gptps_tui_set_source(t, &s) == GPTPS_OK);
    gptps_tui_render(t, frame, sizeof frame);           /* errors 1, reads 1, status E_IO */
    memset(u.b, 0xee, sizeof u.b);
    u.h.struct_size = end;                               /* the layout before SOURCE */
    CHECK(gptps_tui_health(t, &u.h) == GPTPS_OK && u.h.struct_size == end);
    for (k = end; k < sizeof u.b; ++k) if (u.b[k] != 0xee) break;
    CHECK(k == sizeof u.b);                              /* nothing past what it covers */
    end = offsetof(gptps_tui_health_info, source_rows) + sizeof(uint64_t);
    memset(u.b, 0xee, sizeof u.b);
    u.h.struct_size = end;                               /* 1.12's layout: 1.13's fields untouched */
    CHECK(gptps_tui_health(t, &u.h) == GPTPS_OK && u.h.struct_size == end && u.h.source == 1 &&
          u.h.source_errors == 1 && u.h.source_rows == 0);
    for (k = end; k < sizeof u.b; ++k) if (u.b[k] != 0xee) break;
    CHECK(k == sizeof u.b && end < sizeof u.b);          /* pane_shed_ms, shed_pressure_all not written */
    u.h.struct_size = sizeof u.h;
    CHECK(gptps_tui_health(t, &u.h) == GPTPS_OK && u.h.source == 1 && u.h.source_errors == 1 &&
          u.h.source_reads == 1 && u.h.source_status == GPTPS_E_IO);
    gptps_shutdown(e); gptps_tui_close(t);
}

int main(void)
{
    test_validation();
    test_frame();
    test_read_only_at_render();
    test_errors();
    test_ownership();
    test_rate_hands();
    test_polled();
    test_health_size();
    if (fails) { printf("test_tui_source: %d FAILED\n", fails); return 1; }
    printf("test_tui_source: OK\n");
    return 0;
}
