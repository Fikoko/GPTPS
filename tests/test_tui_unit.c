/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * test_tui_unit.c - the dashboard's text handling and its operator actions, with the
 * add-on compiled in (its static helpers and its observer are in reach). Headless.
 *
 *  1) The failure "why" line passes nothing a terminal would execute: crafted stderr
 *     bytes - a lead byte followed by ESC, encoded C1 controls (\xc2\x9b is CSI),
 *     overlongs, surrogates, bidi overrides, bytes F5..FF - come out as printable,
 *     valid UTF-8. The old filter checked only the lead byte and copied the next 1-3
 *     bytes blind, so "\xc3\x1b]0;pwned\x07" reached the terminal.
 *  2) A RECENT line fits the terminal width, so the frame is as tall as it counts.
 *  3) The line editor keeps UTF-8 (non-ASCII values can be typed) and backspace
 *     removes a whole character.
 *  4) A settings edit commits to the key it started on, even when an unregister
 *     shifts the indices meanwhile.
 *  5) A hotkey whose submit the engine refuses says so (it said "submitted").
 *  6) Dead letters: loaded into the pane with their task and status; a re-submit the
 *     engine refuses keeps them, with the reason (it dropped them for good, blaming
 *     a removed task); one at a time or all; discard.
 *  7) POSIX: gptps_tui_run with output on a terminal but stdin at end of input returns
 *     at once (it spun, repainting tens of thousands of frames a second).
 *  8) Text a host or a config file put in a setting - its value, default, description
 *     and an enum's choices - a task's name and the status line are shown cleaned on
 *     the settings, tasks and detail panes: a STRING setting holding ESC and BEL
 *     reached the terminal raw.
 *  9) The editor keeps only whole, well-formed UTF-8 characters, and says when it
 *     left bytes out: a non-UTF-8 terminal's lone E7 ('c cedilla' in Latin-1) was
 *     stored as a dangling lead byte and committed into the setting.
 * 10) Windows: keys are told apart by their virtual-key code, so U+00E0 'a grave' is
 *     a letter, not the extended-key prefix that swallowed the next key.
 * 11) POSIX: the settings and detail panes render while another thread registers
 *     and unregisters a task: they read only key_copy / desc_copy (ABI 2.8), never
 *     the borrowed key an unregister frees (an ASan build would flag that).
 * 1.10.1:
 * 12) A 64..127-byte task name is ONE row (each event made a new one, so one such task
 *     used up all 64 rows), its RECENT record keeps the whole name, and a label cut to
 *     fit ends on a whole UTF-8 character.
 * 13) No unchecked allocation: without memory for the recent log or the latency ring
 *     the dashboard installs and runs without them, WARNs, and the setters say
 *     GPTPS_E_NOMEM (install used to fail, and FULL silently became NORMAL).
 * 14) The legend with 64 printable hotkeys fits its item array (it wrote one past it)
 *     and its buffer (it was cut, losing the global keys).
 * 15) Every terminal event releases its latency entry - DEAD_LETTERED, DROPPED and
 *     FAILED/E_CANCELLED too, without a sample; a retried FAILED keeps it.
 * 16) Latency past the window: depth 200 at window 64 still gives samples, tombstones
 *     stay bounded, and the loss is counted and WARNed once (it cascaded to 0 samples).
 * 17) Counters are 64-bit (they wrapped at 2^32) and print in the table's widths.
 * 18) Clamps and drops are logged: max_recent, kpi, mode, latency_window, the 65th
 *     task type (and "+N types not shown"), a setting that cannot be registered.
 * 19) A frame that does not fit ends on a whole line, with the colour reset and a
 *     "[frame truncated]" line (it was cut mid-character / mid-escape, silently).
 * 20) The status / toast fades after 3 s on every pane, and ON_DEMAND repaints once
 *     when it expires.
 * 21) install reads cfg->struct_size bytes, zeroes the rest, and refuses a struct too
 *     short to be a config (it copied sizeof, reading past an older caller's struct).
 * 22) Latency, below the window: a terminal event that outruns its QUEUED while the
 *     head holds a long-running item still leaves a tombstone (in a free slot) and
 *     gives its sample, and a QUEUED does not evict that item while slots are free;
 *     an end with no room at all is counted, and its late QUEUED starts no clock.
 * 23) Without memory for the frame buffer, snapshot and run say so; an engine that
 *     refuses the observer makes install return NULL with an ERROR.
 * 1.11 (15, 16 and 22 now test the handle table that replaced the latency ring; 18 the
 * '(other)' row and cfg.max_tasks):
 * 24) A FAILED's why looks at the last 512 bytes of its stderr tail only; with no line
 *     break there it shows the end of the line after an ellipsis.
 * 25) The event path allocates nothing at any KPI level (every allocation is counted).
 * 26) Cancelled while queued: in-flight and pending stay exact at FULL; below FULL the
 *     approximation is marked '~' on the frame.
 * 27) gptps_tui_health fills only the struct_size it is given.
 * 28) 1000 types: each its own row, in first-seen order, through the name index; the
 *     1001st folds into '(other)'.
 * 29) POSIX: the parts are swapped in and out while four threads emit and another
 *     renders, reads health and binds tasks (the free protocol, under ASan / TSan).
 * 30) ABI: the released config offsets and enum values (static asserts); a 1.10 caller's
 *     struct_size (its sizeof, tail padding included) and 0 read none of the 1.11
 *     fields, whatever its padding and what follows it hold.
 * 31) The '>=' (lower bound) mark stays until the engine is idle and the table holds
 *     nothing live: an always-up service's restarts (FINISHED, no QUEUED), a late
 *     install and a table rebuilt while work waits no longer read EXACT.
 * 32) POSIX: gptps_tui_health races nothing while the dashboard takes dead letters.
 * 1.11, the KPI ladder and the journals:
 * 33) Each tier collects exactly its layers - nothing above it allocated or written -
 *     installed at the tier or moved there; show_tasks / show_recent < 0 (a 1.10 caller's
 *     struct) stop the collection too, not only the pane.
 * 34) The released enum values and tui.kpi strings are unchanged and the appended ones
 *     work ("count" and "activity" read back as "minimal" and "normal"); 'm' steps up
 *     the ladder and wraps to minimal, never into off.
 * 35) The journals: a failure is never pushed out by activity; what was overwritten
 *     before a frame showed it is counted exactly ("N not shown", health, the lost:
 *     line); 0 lines frees a journal; tui.recent / tui.activity have ranges.
 * 36) A file saved with 'w' at each new tier and journal size reloads with 0 config errors.
 * 37) Latency percentiles (FULL): the log2-ms buckets, p50/p95/p99 when the width has
 *     room, nothing at LATENCY.
 * 38) RECENT merges the journals by time (a problem after its activity at the same ms);
 *     with ACTIVITY off it shows problems only.
 * 39) The 1.11 config fields: out of range refused at install, with an ERROR.
 * 40) (test_narrow_lines) every line of a narrow frame is one row.
 * 1.11, the frame and the live loop:
 * 41) The line diff: 0 bytes for an identical frame, a changed line rewritten in place, a
 *     shorter frame wiped below, a full repaint past half the lines.
 * 42) The header: the clock off / minutes (the default) / seconds and tui.clock; done/s over
 *     the window ("-", then a rate that settles to 0 when the window empties, "n/a" at OFF);
 *     peak over one to two windows; "GPTPS" once when the title starts with it.
 * 43) run's and snapshot's frame grows to 256 KiB; past that the cut is visible and counted.
 * 44) The TASKS table and the tasks pane fit the terminal's height, with "+N more".
 * 45) The legend is cached, and rebuilt when a binding or the width changes.
 * 46) POSIX: the dashboard is formatted with the lock released (copy-out under it only).
 * 47) POSIX, on a pseudo-terminal: PAUSED and ON_DEMAND idle 3 s - no frame, no byte, no
 *     wake; gptps_tui_wake wakes it once (a resize repaints); a toast fades on its own in
 *     ON_DEMAND; CONTINUOUS idle makes no frame and writes nothing; one changed line costs
 *     one line's bytes; a burst of 100k events at refresh 250 ms costs frames bounded by
 *     the time and at most one wake a frame; the wake pipe exists only while run runs,
 *     and a headless run makes none.
 * 48) The tasks / settings / detail panes read the engine in one pass into a cache: none
 *     between passes, one call per entry in a pass, a slow pass stretching the interval to
 *     100x its cost, a change made from the pane or 'R' read at once, an edit landing on
 *     the chosen key after the registry moved; trk_resync held back after any look (99x
 *     its cost, a cheap one too), a frame due when the hold ends, install / a tier change /
 *     health not held; the detail pane shows all 11 of a task's settings at 24 rows and
 *     fits them in 24 rows while editing; a pass with no memory says so, not "none".
 * 49) Windows, on a pseudo console (ConPTY; the test re-runs itself as the child attached
 *     to it): PAUSED and ON_DEMAND idle 3 s - no wake, no frame, no write (health's
 *     counters); gptps_tui_wake wakes it once and writes nothing; ResizePseudoConsole
 *     repaints once at the new width (skipped, saying so, where the resize itself is not
 *     implemented - Wine 9 - or there is no ConPTY).
 * 37) also: max ms beside avg ms from LATENCY up, only when the width has room for it.
 */
#if !defined(_WIN32)
#  define _XOPEN_SOURCE 700      /* posix_openpt & co., for 7) */
#endif
/* 1.10.1 13): the add-on's calloc, made to fail on demand - after `tui_calloc_skip`
 * more calls succeed, the next `tui_calloc_fail` calls return NULL. Defined before
 * the macro, so it reaches the real calloc. */
#include <stdlib.h>
static int tui_calloc_skip, tui_calloc_fail;
/* 1.11: every allocation the add-on makes, counted (the event path must make none) */
static int tui_allocs;              /* an int: tests/test_atomic_compat.h maps it on MSVC */
static void tui_count_alloc(void) { __atomic_add_fetch(&tui_allocs, 1, __ATOMIC_SEQ_CST); }   /* threads: 29) */
static int tui_realloc_fail;        /* 48): the next `tui_realloc_fail` reallocs return NULL */
static void *tui_test_realloc(void *p, size_t n)
{
    tui_count_alloc();
    if (tui_realloc_fail > 0) { --tui_realloc_fail; return NULL; }
    return realloc(p, n);
}
static void *tui_test_calloc(size_t n, size_t s)
{
    tui_count_alloc();
    if (tui_calloc_fail > 0) {
        if (tui_calloc_skip > 0) --tui_calloc_skip;
        else { --tui_calloc_fail; return NULL; }
    }
    return calloc(n, s);
}
/* 23): the same for malloc - the next `tui_malloc_fail` calls return NULL */
static int tui_malloc_fail;
static void *tui_test_malloc(size_t n)
{
    tui_count_alloc();
    if (tui_malloc_fail > 0) { --tui_malloc_fail; return NULL; }
    return malloc(n);
}
/* 48): every read the add-on makes of the engine's registry, counted - and, on demand,
 * made slower (a busy engine's gptps_task_get_info walks every queue under its lock).
 * gptps.h first, so its prototypes are not rewritten by the macros. */
#include "gptps.h"
static int tui_reads;               /* an int: as tui_allocs */
static int tui_read_delay_us;       /* each read takes at least this long */
static gptps_status tui_test_tgi(gptps *e, size_t i, gptps_task_info *o);
static size_t tui_test_tc(gptps *e);
static gptps_status tui_test_sgi(gptps *e, size_t i, gptps_setting_info *o);
static size_t tui_test_sc(gptps *e);
#define calloc(n, s) tui_test_calloc((n), (s))
#define malloc(n) tui_test_malloc((n))
#define realloc(p, n) tui_test_realloc((p), (n))
#define gptps_task_get_info(e, i, o) tui_test_tgi((e), (i), (o))
#define gptps_task_count(e) tui_test_tc((e))
#define gptps_settings_get_info(e, i, o) tui_test_sgi((e), (i), (o))
#define gptps_settings_count(e) tui_test_sc((e))
#include "../addons/gptps_tui.c"
#undef calloc
#undef malloc
#undef realloc
#undef gptps_task_get_info
#undef gptps_task_count
#undef gptps_settings_get_info
#undef gptps_settings_count
static void tui_read_note(void)
{
    __atomic_add_fetch(&tui_reads, 1, __ATOMIC_SEQ_CST);
    if (tui_read_delay_us > 0) { uint64_t end = tui_now_us() + (uint64_t)tui_read_delay_us; while (tui_now_us() < end) { } }
}
static gptps_status tui_test_tgi(gptps *e, size_t i, gptps_task_info *o) { tui_read_note(); return gptps_task_get_info(e, i, o); }
static size_t tui_test_tc(gptps *e) { tui_read_note(); return gptps_task_count(e); }
static gptps_status tui_test_sgi(gptps *e, size_t i, gptps_setting_info *o) { tui_read_note(); return gptps_settings_get_info(e, i, o); }
static size_t tui_test_sc(gptps *e) { tui_read_note(); return gptps_settings_count(e); }
static int reads(void) { return __atomic_load_n(&tui_reads, __ATOMIC_SEQ_CST); }
#include <stdio.h>
#if !defined(_WIN32)
#  include <fcntl.h>
#  include <pthread.h>
#  include <signal.h>
#  include <sys/select.h>   /* FD_SETSIZE */
#  include <sys/resource.h>
#endif

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

/* whether s is valid UTF-8 with no C0, DEL or C1 control in it */
static int clean_utf8(const char *s)
{
    const unsigned char *u = (const unsigned char *)s;
    size_t i = 0, n = strlen(s);
    while (i < n) {
        uint32_t cp = 0;
        size_t len = u8_decode(u + i, n - i, &cp);
        if (len == 0) return 0;
        if (cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp <= 0x9f)) return 0;
        i += len;
    }
    return 1;
}

static void why_of(const char *tail, size_t len, char *out, size_t n)
{
    gptps_event ev;
    memset(&ev, 0, sizeof ev);
    ev.struct_size = sizeof ev; ev.kind = GPTPS_EV_FAILED; ev.status = GPTPS_E_TASK;
    ev.stderr_tail = tail; ev.stderr_tail_len = len;
    {   /* as the event path records it, and a frame then shows it */
        tui_event re;
        why_record(&ev, &re);
        why_join(re.why, re.tail, re.tail_len, re.tail_cut, out, n);
    }
}

static void test_sanitiser(void)
{
#define C(in, want) { in, sizeof in - 1, want }
    static const struct { const char *in; size_t len; const char *want; } cases[] = {
        C("x\xc3\x1b]0;pwned\x07", "x? ]0;pwned "),                  /* lead + ESC, BEL */
        C("\xe0\x1b[2J", "? [2J"),                                   /* lead + ESC */
        C("\xc2\x9b" "31m", "?31m"),                                 /* UTF-8 CSI (C1) */
        C("\xc2\x85" "a", "?a"),                                     /* NEL (C1) */
        C("\xc0\x9b" "a", "??a"),                                    /* overlong */
        C("\xe0\x80\x9b", "???"),                                    /* overlong 3-byte */
        C("\xed\xa0\x80", "???"),                                    /* surrogate */
        C("\xf8\x88\x80\x80\x80", "?????"),                          /* 5-byte form */
        C("\xff\xfe", "??"),
        C("\xf4\x90\x80\x80", "????"),                               /* past U+10FFFF */
        C("a\xe2\x80\xae" "evil", "a?evil"),                         /* RLO bidi override */
        C("M\xc3\xbc\xc5\x9fteri \xe2\x9c\x93", "M\xc3\xbc\xc5\x9fteri \xe2\x9c\x93"),  /* kept */
        C("tab\there\x7f", "tab here "),
        C("\xe4\xb8\xad", "\xe4\xb8\xad"),                           /* CJK, kept */
    };
#undef C
    char out[96], why[96];
    size_t k;
    for (k = 0; k < sizeof cases / sizeof cases[0]; ++k) {
        tui_clean(cases[k].in, cases[k].len, out, sizeof out, -1, NULL);
        if (strcmp(out, cases[k].want) != 0) printf("case %lu: got \"%s\"\n", (unsigned long)k, out);
        CHECK(strcmp(out, cases[k].want) == 0);
        CHECK(clean_utf8(out));
        why_of(cases[k].in, cases[k].len, why, sizeof why);
        CHECK(clean_utf8(why));
        CHECK(strchr(why, 0x1b) == NULL);
    }
    /* the finding's own input, through the why line */
    {
        static const char evil[] = "x\xc3\x1b]0;pwned\x07\xe0\x1b[2J\xc2\x9b" "31m";
        why_of(evil, sizeof evil - 1, why, sizeof why);
        CHECK(strchr(why, 0x1b) == NULL && strchr(why, 0x07) == NULL);
        CHECK(strstr(why, "\xc2\x9b") == NULL);
        CHECK(clean_utf8(why));
    }
    /* never half a character at the cut, and columns are counted */
    {
        int cols = 0;
        size_t n = tui_clean("ab\xe4\xb8\xad" "cd", 7, out, 5, -1, &cols);     /* room for "ab" + NUL... */
        CHECK(n == 2 && strcmp(out, "ab") == 0);                             /* ...not "ab\xe4\xb8" */
        tui_clean("\xe4\xb8\xad\xe4\xb8\xad" "x", 7, out, sizeof out, 3, &cols);
        CHECK(strcmp(out, "\xe4\xb8\xad") == 0 && cols == 2);                /* a wide char is 2 */
        CHECK(u8_fit("\xc3\xbc\xc3\xbc\xc3\xbc", 2) == 4);
    }
    /* a cut tail never splits a character either */
    {
        char tail[300]; size_t i;
        for (i = 0; i + 2 <= sizeof tail; i += 2) { tail[i] = (char)0xc3; tail[i + 1] = (char)0xbc; }
        why_of(tail, sizeof tail, why, sizeof why);
        CHECK(clean_utf8(why));
    }
}

static gptps_status ok_task(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_OK; }
static gptps_status fail_task(gptps_ctx *c, void *u) { (void)c; (void)u; return GPTPS_E_TASK; }

static void deftask(gptps *e, const char *name, gptps_run_fn fn)
{
    gptps_task_def d;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.run = fn; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;   /* no retries: dead-lettered */
    CHECK(gptps_register_task(e, &d) == GPTPS_OK);
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

static gptps_tui *install(gptps *e)
{
    gptps_tui_config c;
    memset(&c, 0, sizeof c);
    c.struct_size = sizeof c; c.color = 0; c.interactive = 0; c.unicode = 0; c.max_recent = 16;
    return gptps_tui_install(e, &c);
}

static void drain(gptps *e) { size_t n; while (gptps_step(e, &n) == GPTPS_OK && n) { } }

/* the widest line of a frame, in columns (no colour: no escapes to skip) */
static int widest(const char *frame)
{
    int best = 0;
    const char *p = frame;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[1024]; int cols = 0;
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len); line[len] = 0;
        tui_clean(line, len, line, sizeof line, -1, &cols);
        if (cols > best) best = cols;
        p += len + (nl ? 1 : 0);
        if (!nl) break;
    }
    return best;
}

static void test_recent_fits(void)
{
    gptps *e = open_manual(); gptps_tui *t; char frame[16384]; int i, lines = 0;
    const char *p;
    if (!e) return;
    t = install(e);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    for (i = 0; i < 8; ++i) {                      /* eight long failure lines */
        gptps_event ev;
        static const char tail[] = "ORA-12541: TNS:no listener at host erp-db-01.example.internal port 1521 "
                                   "while connecting for the nightly MRP run (attempt 3 of 3)\n";
        memset(&ev, 0, sizeof ev);
        ev.struct_size = sizeof ev; ev.kind = GPTPS_EV_FAILED; ev.status = GPTPS_E_TASK;
        ev.task_name = "erp_nightly_mrp"; ev.handle = (gptps_handle)(1000 + i); ev.ts_ms = gptps_now_ms(NULL);
        ev.stderr_tail = tail; ev.stderr_tail_len = sizeof tail - 1;
        tui_on_event(&ev, t);
    }
    t->cols = 80; t->rows = 24;
    gptps_tui_render(t, frame, sizeof frame);
    if (widest(frame) > 80) printf("%s\n", frame);
    CHECK(widest(frame) <= 80);                    /* no line wraps... */
    for (p = frame; *p; ++p) if (*p == '\n') ++lines;
    CHECK(lines <= 24);                            /* ...so the frame is as tall as counted */
    CHECK(strstr(frame, "ORA-12541") != NULL);     /* and the reason is still there */
    t->cols = 40;
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "erp_nightly_mr") != NULL);
    gptps_shutdown(e);
    gptps_tui_close(t);
}

static void type(gptps_tui *t, const char *s) { while (*s) gptps_tui_press(t, (unsigned char)*s++); }

/* select the settings-pane row whose key is `key` */
static int select_setting(gptps_tui *t, const char *key)
{
    size_t i, n = gptps_settings_count(t->e);
    for (i = 0; i < n; ++i) {
        gptps_setting_info info;
        memset(&info, 0, sizeof info); info.struct_size = sizeof info;
        if (gptps_settings_get_info(t->e, i, &info) == GPTPS_OK && strcmp(info.key, key) == 0) {
            t->sel = (int)i;
            return 1;
        }
    }
    return 0;
}

static void test_editor_and_commit(void)
{
    gptps *e = open_manual(); gptps_tui *t; char v[GPTPS_SETTINGS_VALUE_MAX]; int i;
    if (!e) return;
    deftask(e, "acme", ok_task);
    deftask(e, "zeta", ok_task);
    CHECK(gptps_define_global(e, "site.path", GPTPS_SETTING_STRING, "", NULL, 0) == GPTPS_OK);
    t = install(e);
    if (!t) { gptps_shutdown(e); return; }

    /* 3) UTF-8 in, a whole character out per backspace */
    CHECK(gptps_tui_press(t, 's') == 4);
    CHECK(select_setting(t, "site.path"));
    CHECK(gptps_tui_press(t, '\r') == 4);
    type(t, "D:\\Batch\\M\xc3\xbc\xc5\x9fteri");
    gptps_tui_press(t, 127);                       /* the i */
    gptps_tui_press(t, 127);                       /* the r */
    gptps_tui_press(t, 127);                       /* the e */
    gptps_tui_press(t, 127);                       /* the t */
    gptps_tui_press(t, 127);                       /* the whole s-cedilla, both bytes */
    type(t, "\xc5\x9f");                           /* typed back */
    CHECK(gptps_tui_press(t, '\r') == 4);
    CHECK(gptps_settings_get(e, "site.path", v, sizeof v) == GPTPS_OK);
    CHECK(strcmp(v, "D:\\Batch\\M\xc3\xbc\xc5\x9f") == 0);   /* was "D:\\Batch\\M" + bytes dropped */

    /* 4) the commit goes to the key the edit started on */
    CHECK(select_setting(t, "tasks.zeta.timeout_seconds") || select_setting(t, "tasks.zeta.max_retries"));
    {
        gptps_setting_info info; char key[160];
        memset(&info, 0, sizeof info); info.struct_size = sizeof info;
        CHECK(gptps_settings_get_info(e, (size_t)t->sel, &info) == GPTPS_OK);
        snprintf(key, sizeof key, "%s", info.key);
        CHECK(gptps_tui_press(t, '\r') == 4);      /* start editing it */
        for (i = 0; i < 8; ++i) gptps_tui_press(t, 127);
        type(t, "7");
        CHECK(gptps_unregister_task(e, "acme", GPTPS_REMOVE_REJECT_IF_BUSY) == GPTPS_OK);  /* shifts indices */
        CHECK(gptps_tui_press(t, '\r') == 4);
        CHECK(gptps_settings_get(e, key, v, sizeof v) == GPTPS_OK);
        CHECK(strcmp(v, "7") == 0);                /* not some neighbour */
        CHECK(strstr(t->status, key) != NULL);
    }
    gptps_tui_press(t, 27);
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* a frame (lines of text) with nothing a terminal would execute: valid UTF-8, and no
 * control character but the newline */
static int frame_clean(const char *s)
{
    const unsigned char *u = (const unsigned char *)s;
    size_t i = 0, n = strlen(s);
    while (i < n) {
        uint32_t cp = 0;
        size_t len = u8_decode(u + i, n - i, &cp);
        if (len == 0) return 0;
        if ((cp < 0x20 && cp != '\n') || cp == 0x7f || (cp >= 0x80 && cp <= 0x9f)) return 0;
        i += len;
    }
    return 1;
}

static const char *const EVIL_CHOICES[] = { "a\x1b[2Jb", "plain", NULL };
static char evil_mode[64] = "a\x1b[2Jb";
static size_t evil_rd(void *t, char *b, size_t c) { (void)t; return (size_t)snprintf(b, c, "%s", evil_mode); }
static gptps_status evil_wr(void *t, const char *v) { (void)t; snprintf(evil_mode, sizeof evil_mode, "%s", v); return GPTPS_OK; }

static void test_untrusted_settings(void)
{
    gptps *e = open_manual(); gptps_tui *t; char frame[16384]; size_t i, n;
    gptps_setting_def d;
    if (!e) return;
    deftask(e, "ev\x1b[2Jil", ok_task);
    CHECK(gptps_define_task_setting(e, "label", GPTPS_SETTING_STRING, "x\x1b[31my", NULL, 0) == GPTPS_OK);
    CHECK(gptps_define_global(e, "app.label", GPTPS_SETTING_STRING, "d\x1b[1mef", NULL, 0) == GPTPS_OK);
    CHECK(gptps_settings_set(e, "app.label", "x\x1b]0;pwned\x07\x1b[2Jy") == GPTPS_OK);
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.key = "app.mode"; d.type = GPTPS_SETTING_ENUM; d.hot = 1;
    d.desc = "picks \x1b]0;title\x07 the mode \xc2\x9b" "31m"; d.choices = EVIL_CHOICES;
    d.read = evil_rd; d.write = evil_wr;
    CHECK(gptps_register_setting(e, &d) == GPTPS_OK);
    t = install(e);
    if (!t) { gptps_shutdown(e); return; }

    CHECK(gptps_tui_press(t, 's') == 4);
    CHECK(select_setting(t, "app.label"));
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "]0;pwned") != NULL);      /* shown ... */
    CHECK(frame_clean(frame));                     /* ... and not executed (had a raw ESC) */
    CHECK(gptps_tui_press(t, '\r') == 4);          /* edit, commit as it is: the value */
    CHECK(gptps_tui_press(t, '\r') == 4);          /* lands in the status line */
    CHECK(strstr(t->status, "app.label") != NULL);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "set app.label") != NULL);
    CHECK(frame_clean(frame));
    CHECK(select_setting(t, "app.mode"));          /* description, choices, default */
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "the mode") != NULL);
    CHECK(frame_clean(frame));
    CHECK(select_setting(t, "tasks.ev\x1b[2Jil.label"));
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(frame_clean(frame));

    CHECK(gptps_tui_press(t, 's') == 4);           /* back to the dashboard */
    CHECK(gptps_tui_press(t, 't') == 4);           /* tasks pane: the name */
    n = gptps_task_count(e);
    for (i = 0; i < n; ++i) {
        gptps_task_info ti; memset(&ti, 0, sizeof ti); ti.struct_size = sizeof ti;
        if (gptps_task_get_info(e, i, &ti) == GPTPS_OK && strncmp(ti.name_copy, "ev", 2) == 0) t->task_sel = (int)i;
    }
    toast(t, "about ev\x1b[2Jil");                 /* a status naming a task */
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "ev [2Jil") != NULL);
    CHECK(frame_clean(frame));
    CHECK(gptps_tui_press(t, '\r') == 4);          /* its detail pane */
    CHECK(t->pane == TUI_PANE_DETAIL);
    for (i = 0; i < 32; ++i) gptps_tui_press(t, 'j');   /* down to the last: its own "label" */
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "label") != NULL);
    CHECK(frame_clean(frame));

    t->pane = TUI_PANE_DASH;                       /* the dashboard: label, legend, RECENT */
    CHECK(gptps_tui_add_task(t, "ev\x1b[2Jil", "L\x1b]0;x\x07", 'e', NULL, 0) == GPTPS_OK);
    CHECK(gptps_submit(e, "ev\x1b[2Jil", "p", 1, NULL) == GPTPS_OK);
    drain(e);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "ev [2Jil") != NULL);
    CHECK(strstr(frame, "L ]0;x") != NULL);
    CHECK(frame_clean(frame));
    gptps_shutdown(e);
    gptps_tui_close(t);
}

static void test_editor_bytes(void)
{
    gptps *e = open_manual(); gptps_tui *t; char v[GPTPS_SETTINGS_VALUE_MAX];
    if (!e) return;
    CHECK(gptps_define_global(e, "site.dir", GPTPS_SETTING_STRING, "", NULL, 0) == GPTPS_OK);
    t = install(e);
    if (!t) { gptps_shutdown(e); return; }
    CHECK(gptps_tui_press(t, 's') == 4);
    CHECK(select_setting(t, "site.dir"));
    CHECK(gptps_tui_press(t, '\r') == 4);
    t->status[0] = 0;
    type(t, "a\xe7" "b");                          /* Latin-1 c-cedilla: a lone E7 */
    CHECK(strcmp(t->editbuf, "ab") == 0);
    CHECK(strstr(t->status, "not UTF-8") != NULL); /* and it says so */
    type(t, "\x80");                               /* a stray continuation byte */
    type(t, "\xc0\xaf");                           /* never a lead byte (overlong '/') */
    type(t, "\xe0\x80\x80");                       /* overlong 3-byte NUL */
    type(t, "\xed\xa0\x80");                       /* a surrogate */
    type(t, "\xe2\x82\xac");                       /* the euro sign: kept */
    CHECK(strcmp(t->editbuf, "ab\xe2\x82\xac") == 0);
    type(t, "\xc3");                               /* a lead byte, then Enter */
    CHECK(gptps_tui_press(t, '\r') == 4);
    CHECK(gptps_settings_get(e, "site.dir", v, sizeof v) == GPTPS_OK);
    CHECK(strcmp(v, "ab\xe2\x82\xac") == 0);       /* had a dangling C3 at the end */
    CHECK(clean_utf8(v));
    gptps_shutdown(e);
    gptps_tui_close(t);
}

#if defined(_WIN32)
static void test_windows_keys(void)
{
    KEY_EVENT_RECORD k;
    memset(&k, 0, sizeof k);
    k.bKeyDown = TRUE; k.wRepeatCount = 1;
    k.wVirtualKeyCode = 'A'; k.uChar.UnicodeChar = 0x00E0;   /* a grave */
    CHECK(tui_key_of(&k) == 0xC3);
    CHECK(tui_kq_n == 2 && tui_kq[1] == 0xA0);
    {   int eof = 0, rz = 0;
        CHECK(term_key(&eof, &rz) == 0xA0);        /* the queued byte, no wait */
    }
    k.wVirtualKeyCode = VK_UP; k.uChar.UnicodeChar = 0;
    CHECK(tui_key_of(&k) == 'k');
    k.wVirtualKeyCode = VK_LEFT;
    CHECK(tui_key_of(&k) == '\t');
    k.wVirtualKeyCode = VK_SHIFT;
    CHECK(tui_key_of(&k) == -1);
    k.wVirtualKeyCode = 0; k.uChar.UnicodeChar = 0xD83D;      /* U+1F600 as a pair */
    CHECK(tui_key_of(&k) == -1);
    k.uChar.UnicodeChar = 0xDE00;
    CHECK(tui_key_of(&k) == 0xF0);
    CHECK(tui_kq_n == 4 && tui_kq[1] == 0x9F && tui_kq[2] == 0x98 && tui_kq[3] == 0x80);
    tui_kq_i = tui_kq_n;
    k.uChar.UnicodeChar = 'q'; k.bKeyDown = FALSE;           /* a release is no key */
    CHECK(tui_key_of(&k) == -1);
}
#else
static int churn_stop;                          /* atomic: __atomic_* (MSVC: test_atomic_compat.h) */
static void *churn(void *arg)
{
    gptps *e = (gptps *)arg;
    while (!__atomic_load_n(&churn_stop, __ATOMIC_SEQ_CST)) {
        gptps_task_def d;
        memset(&d, 0, sizeof d);
        d.struct_size = sizeof d; d.name = "churn"; d.run = ok_task; d.exec = GPTPS_EXEC_INPROC;
        d.default_cost.struct_size = sizeof d.default_cost;
        d.default_policy.struct_size = sizeof d.default_policy;
        if (gptps_register_task(e, &d) == GPTPS_OK)
            gptps_unregister_task(e, "churn", GPTPS_REMOVE_REJECT_IF_BUSY);
    }
    return NULL;
}

static void test_render_while_unregistering(void)
{
    gptps *e = open_manual(); gptps_tui *t; char frame[16384]; int i; pthread_t th;
    if (!e) return;
    deftask(e, "steady", ok_task);
    t = install(e);
    if (!t) { gptps_shutdown(e); return; }
    __atomic_store_n(&churn_stop, 0, __ATOMIC_SEQ_CST);
    if (pthread_create(&th, NULL, churn, e) != 0) { gptps_shutdown(e); gptps_tui_close(t); return; }
    CHECK(gptps_tui_press(t, 's') == 4);
    for (i = 0; i < 3000; ++i) {
        t->sel = i % 40;
        gptps_tui_render(t, frame, sizeof frame);
    }
    t->pane = TUI_PANE_DETAIL;
    snprintf(t->detail_task, sizeof t->detail_task, "churn");
    for (i = 0; i < 3000; ++i) {
        t->detail_sel = i % 8;
        gptps_tui_render(t, frame, sizeof frame);
        if (i % 7 == 0) { gptps_tui_press(t, '\r'); gptps_tui_press(t, 27); }   /* start and drop an edit */
    }
    __atomic_store_n(&churn_stop, 1, __ATOMIC_SEQ_CST);
    pthread_join(th, NULL);
    CHECK(frame_clean(frame));
    gptps_shutdown(e);
    gptps_tui_close(t);
}
#endif

static void test_hotkey_refused(void)
{
    gptps *e = open_manual(); gptps_tui *t; char frame[8192];
    if (!e) return;
    deftask(e, "nightly", ok_task);
    t = install(e);
    if (!t) { gptps_shutdown(e); return; }
    CHECK(gptps_tui_add_task(t, "nightly", "Nightly", 'n', NULL, 0) == GPTPS_OK);
    CHECK(gptps_tui_press(t, 'n') == 1);
    CHECK(gptps_set_task_enabled(e, "nightly", 0) == GPTPS_OK);   /* paused: submits refused */
    CHECK(gptps_tui_press(t, 'n') == 0);                           /* was 1, "submitted" */
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "NOT submitted Nightly") != NULL);
    gptps_shutdown(e);
    gptps_tui_close(t);
}

static void test_dead_letters(void)
{
    gptps *e = open_manual(); gptps_tui *t; char frame[16384]; int i;
    if (!e) return;
    deftask(e, "flaky", fail_task);
    deftask(e, "other", fail_task);
    t = install(e);
    if (!t) { gptps_shutdown(e); return; }
    for (i = 0; i < 3; ++i) CHECK(gptps_submit(e, "flaky", "p", 1, NULL) == GPTPS_OK);
    CHECK(gptps_submit(e, "other", "q", 1, NULL) == GPTPS_OK);
    drain(e);
    CHECK(gptps_dead_letter_count(e) == 4);

    CHECK(gptps_tui_press(t, 'l') == 4);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "4 retained by the engine") != NULL);
    CHECK(gptps_dead_letter_count(e) == 4);        /* opening the pane takes nothing */
    CHECK(gptps_tui_press(t, 'g') == 4);           /* load: each one, with its task */
    CHECK(gptps_dead_letter_count(e) == 0 && t->nheld == 4);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "flaky") != NULL && strstr(frame, "other") != NULL);
    CHECK(strstr(frame, "4 held here") != NULL);

    /* 41) re-submit all while "flaky" is paused: refused, and KEPT, with the reason */
    CHECK(gptps_set_task_enabled(e, "flaky", 0) == GPTPS_OK);
    CHECK(gptps_tui_press(t, 'r') == 4);
    CHECK(t->nheld == 3);                          /* "other" went; the three stayed */
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "re-submit refused") != NULL);
    CHECK(strstr(frame, "3 refused and kept here") != NULL);
    CHECK(strstr(frame, "task removed") == NULL);  /* the old, wrong reason */
    drain(e);                                      /* "other" ran, failed: dead again */
    CHECK(gptps_dead_letter_count(e) == 1);

    /* one at a time once it is resumed; discard one; discard all */
    CHECK(gptps_set_task_enabled(e, "flaky", 1) == GPTPS_OK);
    t->dl_sel = 0;
    CHECK(gptps_tui_press(t, 'e') == 4);
    CHECK(t->nheld == 2);
    CHECK(gptps_tui_press(t, 'x') == 4);
    CHECK(t->nheld == 1);
    CHECK(gptps_tui_press(t, 'D') == 4);           /* the held one + the engine's one */
    CHECK(t->nheld == 0 && gptps_dead_letter_count(e) == 0);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "discarded 2") != NULL);
    drain(e);                                      /* the one re-submitted with 'e' */
    gptps_shutdown(e);
    gptps_tui_close(t);
}

#if !defined(_WIN32)
static void test_run_without_stdin(void)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY), s;
    FILE *out;
    gptps *e; gptps_tui *t;
    gptps_tui_config c;
    if (m < 0 || grantpt(m) != 0 || unlockpt(m) != 0) { printf("note: no pty here; run() check skipped\n"); if (m >= 0) close(m); return; }
    s = open(ptsname(m), O_RDWR | O_NOCTTY);
    if (s < 0) { close(m); printf("note: no pty here; run() check skipped\n"); return; }
    out = fdopen(s, "w");
    if (!out || !freopen("/dev/null", "r", stdin)) { printf("note: cannot set up; run() check skipped\n"); close(m); return; }
    e = open_manual();
    memset(&c, 0, sizeof c);
    c.struct_size = sizeof c; c.color = 0; c.interactive = 1; c.out = out; c.refresh_ms = 50;
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    if (t) {
        alarm(10);                                 /* the spin never returned */
        gptps_tui_run(t);
        alarm(0);
    }
    gptps_shutdown(e);
    gptps_tui_close(t);
    fclose(out);
    close(m);
}
#endif

/* ---- 1.10.1 ------------------------------------------------------------------ */

/* WARN and above, as the process-wide log sink delivers them */
static char logbuf[32768];
static size_t loglen;
static void cap_sink(gptps_log_level lvl, const char *msg, void *u)
{
    size_t n = strlen(msg);
    (void)u;
    if (lvl < GPTPS_LOG_WARN) return;
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

static void emit(gptps_tui *t, int kind, const char *task, gptps_handle h, gptps_status st, uint64_t ts)
{
    gptps_event ev;
    memset(&ev, 0, sizeof ev);
    ev.struct_size = sizeof ev; ev.kind = (gptps_event_kind)kind; ev.task_name = task; ev.handle = h;
    ev.status = st; ev.ts_ms = ts;
    tui_on_event(&ev, t);
}

static gptps_tui *install_cfg(gptps *e, int kpi, int latency_window, int color)
{
    gptps_tui_config c;
    memset(&c, 0, sizeof c);
    c.struct_size = sizeof c; c.color = color; c.interactive = 0; c.unicode = 0; c.max_recent = 16;
    c.kpi = kpi; c.latency_window = latency_window;
    return gptps_tui_install(e, &c);
}

static void emit_a(gptps_tui *t, int kind, const char *task, gptps_handle h, gptps_status st, uint64_t ts,
                   uint32_t attempt)
{
    gptps_event ev;
    memset(&ev, 0, sizeof ev);
    ev.struct_size = sizeof ev; ev.kind = (gptps_event_kind)kind; ev.task_name = task; ev.handle = h;
    ev.status = st; ev.ts_ms = ts; ev.attempt = attempt;
    tui_on_event(&ev, t);
}

/* a task type's TASKS row (through the name index), NULL without one */
static tui_task *row(gptps_tui *t, const char *name)
{
    int r;
    if (!t->tasks) return NULL;
    r = row_lookup(t, name, name_hash(name));
    return r < 0 ? NULL : &t->tasks[r];
}
/* rows in use, '(other)' not counted */
static int nrows(const gptps_tui *t) { return t->tasks ? t->ntasks - 1 : 0; }
/* whether the handle table tracks h (a live entry, not a marker) */
static int live_entries(gptps_tui *t, gptps_handle h)
{
    tui_slot *s = t->tab ? trk_find(t, h) : NULL;
    return s && s->state != TS_DONE;
}
static gptps_tui_health_info health(gptps_tui *t)
{
    gptps_tui_health_info h;
    memset(&h, 0, sizeof h); h.struct_size = sizeof h;
    CHECK(gptps_tui_health(t, &h) == GPTPS_OK);
    return h;
}

/* whether s decodes as UTF-8 throughout (controls allowed) */
static int valid_utf8(const char *s)
{
    const unsigned char *u = (const unsigned char *)s;
    size_t i = 0, n = strlen(s);
    while (i < n) {
        uint32_t cp;
        size_t len = u8_decode(u + i, n - i, &cp);
        if (len == 0) return 0;
        i += len;
    }
    return 1;
}

/* every ESC starts a whole CSI sequence: ESC [ params final */
static int escapes_whole(const char *s)
{
    for (; *s; ++s) {
        if (*s != '\x1b') continue;
        if (s[1] != '[') return 0;
        s += 2;
        while (*s && (unsigned char)*s < 0x40) ++s;
        if (!*s || (unsigned char)*s > 0x7e) return 0;
    }
    return 1;
}

/* 12) */
static void test_long_names(void)
{
    gptps *e = open_manual(); gptps_tui *t; char name[160], big[200]; int i; size_t k;
    if (!e) return;
    t = install(e);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    memset(name, 'n', 100); name[100] = 0;                 /* 100 bytes: over the old 63 */
    for (i = 0; i < 5; ++i) emit(t, GPTPS_EV_QUEUED, name, (gptps_handle)(10 + i), GPTPS_OK, gptps_now_ms(NULL));
    CHECK(nrows(t) == 1);                                  /* one row, not five */
    /* the whole name: the ACTIVITY record names it by its row, the PROBLEMS record keeps a copy */
    CHECK(strcmp(t->tasks[t->act[(t->ahead + t->acap - 1) % t->acap].row].name, name) == 0);
    emit(t, GPTPS_EV_DROPPED, name, 10, GPTPS_E_TASK, gptps_now_ms(NULL));
    CHECK(strcmp(t->prob[(t->phead + t->pcap - 1) % t->pcap].name, name) == 0);
    for (k = 0; k < 63; ++k) { name[2 * k] = '\xc3'; name[2 * k + 1] = '\xa9'; }    /* 63 x U+00E9 */
    name[126] = 'x'; name[127] = 0;                        /* 127 bytes: GPTPS_TASK_NAME_MAX */
    emit(t, GPTPS_EV_QUEUED, name, 20, GPTPS_OK, gptps_now_ms(NULL));
    emit(t, GPTPS_EV_STARTED, name, 20, GPTPS_OK, gptps_now_ms(NULL));
    CHECK(nrows(t) == 2);
    CHECK(row(t, name) != NULL && row(t, name)->started == 1);
    CHECK(gptps_tui_add_task(t, name, NULL, 'z', NULL, 0) == GPTPS_OK && nrows(t) == 2);   /* the same row */
    CHECK(row(t, name) != NULL && row(t, name)->ui == 0 && t->nui == 1);
    CHECK(strlen(t->ui[0].label) == 62);                   /* cut at a character: 31 whole ones */
    CHECK(valid_utf8(t->ui[0].label));
    memset(big, 'b', sizeof big - 1); big[sizeof big - 1] = 0;
    CHECK(gptps_tui_add_task(t, big, NULL, 0, NULL, 0) == GPTPS_E_INVAL);   /* never a task's name */
    CHECK(nrows(t) == 2);
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 13) */
static void test_no_memory(void)
{
    gptps *e = open_manual(); gptps_tui *t; static char frame[16384];
    if (!e) return;
    log_begin();
    /* callocs at install: the dashboard, the TASKS rows, its fold set, the PROBLEMS and
     * ACTIVITY journals, the histograms, the latency table - the last four fail */
    tui_calloc_skip = 3; tui_calloc_fail = 4;
    t = install_cfg(e, GPTPS_TUI_KPI_FULL, 64, 0);
    tui_calloc_skip = 0; tui_calloc_fail = 0;
    CHECK(t != NULL);                                      /* it installs anyway */
    if (!t) { log_end(); gptps_shutdown(e); return; }
    CHECK(t->prob == NULL && t->act == NULL && t->hist == NULL && t->tab == NULL && t->tasks != NULL);
    CHECK(log_count("gptps_tui_install: no memory for the PROBLEMS journal") == 1);
    CHECK(log_count("and the ACTIVITY journal") == 1);
    CHECK(log_count("and the latency table") == 1);
    CHECK(log_count("and the latency histograms") == 1);
    emit(t, GPTPS_EV_QUEUED, "job", 1, GPTPS_OK, 1);       /* and runs without them */
    emit(t, GPTPS_EV_STARTED, "job", 1, GPTPS_OK, 2);
    emit(t, GPTPS_EV_FINISHED, "job", 1, GPTPS_OK, 3);
    CHECK(t->fin == 1 && row(t, "job") != NULL && row(t, "job")->finished == 1);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "no memory for: problems log, activity log, latency, percentiles") != NULL);
    CHECK(strstr(frame, "lost: 4 allocations failed") != NULL);   /* and on the frame, counted */
    CHECK(strstr(frame, "in-flight ~0") != NULL);          /* no table: the approximate gauge, marked */
    CHECK(health(t).alloc_failures == 4 && health(t).gauges == GPTPS_TUI_GAUGES_APPROX);
    tui_calloc_skip = 3; tui_calloc_fail = 1;              /* the journals and histograms go through, the table not */
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_FULL) == GPTPS_E_NOMEM);
    tui_calloc_skip = 0; tui_calloc_fail = 0;
    CHECK(t->tab == NULL && t->prob != NULL && t->act != NULL && t->hist != NULL);
    tui_calloc_fail = 1;
    CHECK(gptps_tui_set_latency_window(t, 128) == GPTPS_E_NOMEM && t->tab == NULL);
    CHECK(gptps_tui_set_latency_window(t, 128) == GPTPS_OK && t->tab != NULL);   /* retried */
    CHECK(health(t).gauges == GPTPS_TUI_GAUGES_EXACT);
    tui_calloc_fail = 1;                                   /* below LATENCY first, then up: the table fails */
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_NORMAL) == GPTPS_OK && t->tab == NULL && t->hist == NULL);
    CHECK(gptps_tui_press(t, 'm') == 3 && t->kpi == GPTPS_TUI_KPI_LATENCY && t->tab == NULL);
    CHECK(strstr(t->status, "no memory") != NULL);         /* the 'm' toast says so */
    CHECK(log_count("gptps_tui_set_kpi(full): no memory") == 1);
    CHECK(log_count("gptps_tui_set_kpi(latency): no memory") == 1);
    CHECK(log_count("gptps_tui_set_latency_window: no memory") == 1);
    /* the TASKS table: down to MINIMAL frees it, back up without memory runs without it */
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_MINIMAL) == GPTPS_OK && t->tasks == NULL && t->prob == NULL && t->act == NULL);
    tui_calloc_fail = 1;
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_NORMAL) == GPTPS_E_NOMEM && t->tasks == NULL && t->prob != NULL);
    CHECK(t->act != NULL && t->act_nm != NULL);            /* no rows to name its tasks by: it keeps copies */
    CHECK(log_count("gptps_tui_set_kpi(normal): no memory for the TASKS table") == 1);
    emit(t, GPTPS_EV_QUEUED, "job", 2, GPTPS_OK, 10);      /* counted, no row to count it in */
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "no memory for: tasks") != NULL && strstr(frame, "TASKS") == NULL);
    CHECK(strstr(frame, "queued 2") != NULL);
    CHECK(strstr(frame, "QUEUED") != NULL && strstr(frame, "job ") != NULL);   /* named from its copy */
    log_end();
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 14) */
static void test_legend_full(void)
{
    gptps *e = open_manual(); gptps_tui *t; static char frame[65536]; int i;
    if (!e) return;
    t = install(e);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    for (i = 0; i < TUI_MAX_TASKS; ++i) {                  /* 64 printable hotkeys, '!' to '`' */
        char nm[32], lb[64];
        snprintf(nm, sizeof nm, "task_%02d", i);
        snprintf(lb, sizeof lb, "a label long enough to wrap the legend %02d", i);
        CHECK(gptps_tui_add_task(t, nm, lb, 33 + i, NULL, 0) == GPTPS_OK);
    }
    t->cols = 200; t->rows = 400;
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "[!] a label long enough to wrap the legend 00") != NULL);
    CHECK(strstr(frame, "[`] a label long enough to wrap the legend 63") != NULL);
    CHECK(strstr(frame, "? help") != NULL && strstr(frame, "q quit") != NULL);   /* the global keys survive */
    CHECK(strstr(frame, "[frame truncated]") == NULL);
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 15) every terminal kind frees its handle-table slot (1.11: the table replaced the ring) */
static void test_latency_release(void)
{
    gptps *e = open_manual(); gptps_tui *t; tui_task *k;
    if (!e) return;
    t = install_cfg(e, GPTPS_TUI_KPI_FULL, 16, 0);
    CHECK(t != NULL && t->tab != NULL);
    if (!t || !t->tab) { gptps_shutdown(e); gptps_tui_close(t); return; }
    emit_a(t, GPTPS_EV_QUEUED, "rel", 1, GPTPS_OK, 100, 0);
    emit_a(t, GPTPS_EV_STARTED, "rel", 1, GPTPS_OK, 101, 1);
    emit_a(t, GPTPS_EV_FAILED, "rel", 1, GPTPS_E_TASK, 102, 1);
    CHECK(live_entries(t, 1) == 1);                        /* an attempt failed: not the end */
    emit_a(t, GPTPS_EV_DEAD_LETTERED, "rel", 1, GPTPS_E_TASK, 103, 1);
    CHECK(live_entries(t, 1) == 0 && t->tab_n == 0);       /* the end: released */
    emit_a(t, GPTPS_EV_QUEUED, "rel", 2, GPTPS_OK, 110, 0);
    emit_a(t, GPTPS_EV_FAILED, "rel", 2, GPTPS_E_CANCELLED, 111, 1);   /* cancelled while queued */
    CHECK(t->tab_n == 0);
    emit_a(t, GPTPS_EV_QUEUED, "rel", 3, GPTPS_OK, 120, 0);
    emit_a(t, GPTPS_EV_STARTED, "rel", 3, GPTPS_OK, 120, 1);
    emit_a(t, GPTPS_EV_FAILED, "rel", 3, GPTPS_E_TASK, 121, 1);
    emit_a(t, GPTPS_EV_DROPPED, "rel", 3, GPTPS_E_TASK, 121, 1);
    CHECK(t->tab_n == 0);
    emit_a(t, GPTPS_EV_QUEUED, "rel", 6, GPTPS_OK, 130, 0);
    emit_a(t, GPTPS_EV_STARTED, "rel", 6, GPTPS_OK, 131, 1);
    emit_a(t, GPTPS_EV_FAILED, "rel", 6, GPTPS_E_CANCELLED, 132, 1);   /* cancelled while running */
    CHECK(t->tab_n == 0 && t->g_run == 0 && t->g_pend == 0);
    k = row(t, "rel");
    CHECK(k != NULL && k->lat_n == 0);                     /* no sample: avg ms is finished items */
    /* a retried attempt keeps its entry: the sample spans queue to the final finish */
    emit_a(t, GPTPS_EV_QUEUED, "rel", 4, GPTPS_OK, 200, 0);
    emit_a(t, GPTPS_EV_STARTED, "rel", 4, GPTPS_OK, 201, 1);
    emit_a(t, GPTPS_EV_FAILED, "rel", 4, GPTPS_E_TASK, 202, 1);
    emit_a(t, GPTPS_EV_RETRIED, "rel", 4, GPTPS_E_TASK, 203, 2);
    CHECK(live_entries(t, 4) == 1 && t->g_pend == 1 && t->g_run == 0);   /* waiting to retry: pending */
    emit_a(t, GPTPS_EV_STARTED, "rel", 4, GPTPS_OK, 204, 2);
    emit_a(t, GPTPS_EV_FINISHED, "rel", 4, GPTPS_OK, 230, 2);
    CHECK(k != NULL && k->lat_n == 1 && k->lat_sum_ms == 30 && t->tab_n == 0);
    /* a cancel that outruns its own QUEUED: the late QUEUED clears it, no sample, no entry */
    emit_a(t, GPTPS_EV_FAILED, "rel", 5, GPTPS_E_CANCELLED, 300, 1);
    CHECK(t->tab_done == 1);                               /* a marker, waiting for it */
    emit_a(t, GPTPS_EV_QUEUED, "rel", 5, GPTPS_OK, 299, 0);
    CHECK(t->tab_n == 0 && t->tab_done == 0 && k != NULL && k->lat_n == 1);
    CHECK(t->g_pend == 0 && t->g_run == 0 && health(t).gauges == GPTPS_TUI_GAUGES_EXACT);
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 16) past the window: a QUEUED that finds every live entry taken is not tracked - no
 * tracked item is evicted - counted, WARNed once a window, and the gauges say '>=' */
static void test_latency_overflow(void)
{
    gptps *e = open_manual(); gptps_tui *t; tui_task *k; static char frame[16384];
    gptps_handle h;
    int round;
    size_t maxmark = 0;
    if (!e) return;
    log_begin();
    t = install_cfg(e, GPTPS_TUI_KPI_FULL, 64, 0);
    CHECK(t != NULL && t->tab != NULL);
    if (!t || !t->tab) { log_end(); gptps_shutdown(e); gptps_tui_close(t); return; }
    for (round = 0; round < 2; ++round) {                  /* depth 200 at window 64, twice */
        gptps_handle base = (gptps_handle)(1 + round * 1000);
        for (h = base; h < base + 200; ++h) emit(t, GPTPS_EV_QUEUED, "deep", h, GPTPS_OK, 1000 + h);
        CHECK(t->g_pend == 64 && t->trk_lb == 1);          /* the 64 tracked, and a lower bound */
        gptps_tui_render(t, frame, sizeof frame);
        CHECK(strstr(frame, "pending >=64") != NULL);
        for (h = base; h < base + 200; ++h) {
            emit(t, GPTPS_EV_FINISHED, "deep", h, GPTPS_OK, 5000 + h);
            if (t->tab_done > maxmark) maxmark = t->tab_done;
        }
        CHECK(t->trk_lb == 1);                             /* events alone never clear it... */
        CHECK(health(t).gauges == GPTPS_TUI_GAUGES_EXACT && t->trk_lb == 0);   /* ...the idle engine does */
    }
    k = row(t, "deep");
    CHECK(k != NULL && k->lat_n == 128);                   /* the 64 tracked, each round */
    CHECK(t->lat_untracked == 272 && health(t).handles_untracked == 272);   /* 136 a round, counted */
    /* the ends of the untracked: markers for a QUEUED that already passed, bounded */
    CHECK(maxmark <= TUI_ORPHANS && t->tab_done == TUI_ORPHANS && t->tab_n == TUI_ORPHANS);
    CHECK(t->lat_unmatched == 272 - TUI_ORPHANS && health(t).orphans_expired == 272 - TUI_ORPHANS);
    CHECK(log_count("tui.latency_window = 64") == 1);      /* WARNed, once for this window */
    CHECK(log_count("about 65 items were queued or running") == 1);  /* the depth when it began */
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "latency: 272 not tracked (window 64 full), 208 ends unmatched") != NULL);
    CHECK(strstr(frame, "lost: 272 items not tracked") != NULL);
    CHECK(gptps_tui_set_latency_window(t, 32) == GPTPS_OK);   /* a new window: counted afresh, WARNed again */
    for (h = 5000; h < 5040; ++h) emit(t, GPTPS_EV_QUEUED, "deep", h, GPTPS_OK, 1);
    CHECK(t->lat_untracked == 8 && log_count("tui.latency_window = 32") == 1);
    CHECK(health(t).handles_untracked == 280);             /* health: since install */
    log_end();
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 22) below the window: a terminal event that outruns its QUEUED gives its sample, and
 * the table at its limit still takes the ends (as markers), never evicting */
static void test_latency_below_window(void)
{
    gptps *e = open_manual(); gptps_tui *t; tui_task *k; static char frame[16384]; gptps_handle h;
    gptps_tui_health_info hh;
    if (!e) return;
    log_begin();
    t = install_cfg(e, GPTPS_TUI_KPI_FULL, 4, 0);
    CHECK(t != NULL && t->tab != NULL);
    if (!t || !t->tab) { log_end(); gptps_shutdown(e); gptps_tui_close(t); return; }
    emit(t, GPTPS_EV_QUEUED, "x", 1, GPTPS_OK, 100);       /* long-running: stays live */
    for (h = 2; h <= 4; ++h) { emit(t, GPTPS_EV_QUEUED, "x", h, GPTPS_OK, 200); emit(t, GPTPS_EV_FINISHED, "x", h, GPTPS_OK, 210); }
    CHECK(live_entries(t, 1) == 1);
    emit(t, GPTPS_EV_FINISHED, "x", 5, GPTPS_OK, 305);     /* outruns its QUEUED */
    emit(t, GPTPS_EV_QUEUED, "x", 5, GPTPS_OK, 300);
    k = row(t, "x");
    CHECK(k != NULL && k->lat_n == 4 && k->lat_sum_ms == 35);   /* the fast one is sampled */
    CHECK(t->tab_n == 1 && t->tab_done == 0 && live_entries(t, 1) == 1);   /* no zombie */
    for (h = 6; h <= 12; ++h) { emit(t, GPTPS_EV_QUEUED, "x", h, GPTPS_OK, 400); emit(t, GPTPS_EV_FINISHED, "x", h, GPTPS_OK, 401); }
    CHECK(live_entries(t, 1) == 1);                        /* a QUEUED does not evict it either */
    emit(t, GPTPS_EV_FINISHED, "x", 1, GPTPS_OK, 1100);
    CHECK(k != NULL && k->lat_n == 12 && k->lat_max_ms == 1000);
    CHECK(t->lat_untracked == 0 && t->lat_unmatched == 0 && t->tab_n == 0 && log_count("latency samples") == 0);
    /* at the limit: four live; an end that outran its QUEUED still gets a marker (its
     * sample is not lost), and a fifth QUEUED is not tracked - counted, WARNed */
    for (h = 20; h < 24; ++h) emit(t, GPTPS_EV_QUEUED, "x", h, GPTPS_OK, 500);
    emit(t, GPTPS_EV_FINISHED, "x", 30, GPTPS_OK, 505);
    emit(t, GPTPS_EV_QUEUED, "x", 30, GPTPS_OK, 501);
    CHECK(k != NULL && k->lat_n == 13 && t->tab_n == 4 && t->tab_done == 0);
    for (h = 20; h < 24; ++h) CHECK(live_entries(t, h) == 1);
    emit(t, GPTPS_EV_QUEUED, "x", 24, GPTPS_OK, 502);
    CHECK(t->lat_untracked == 1 && live_entries(t, 24) == 0 && t->tab_n == 4);
    for (h = 20; h < 24; ++h) CHECK(live_entries(t, h) == 1);   /* none evicted */
    CHECK(log_count("tui.latency_window = 4 entries has no room left") == 1);
    hh = health(t);
    CHECK(hh.gauges == GPTPS_TUI_GAUGES_LOWER_BOUND && hh.pending == 4 && hh.handles_tracked == 4);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "latency: 1 not tracked (window 4 full), 0 ends unmatched") != NULL);
    CHECK(strstr(frame, "pending >=4") != NULL);
    for (h = 20; h <= 24; ++h) emit(t, GPTPS_EV_FINISHED, "x", h, GPTPS_OK, 600);
    hh = health(t);                                        /* every QUEUED has had its end: exact again */
    CHECK(hh.gauges == GPTPS_TUI_GAUGES_EXACT && hh.pending == 0 && hh.in_flight == 0);
    CHECK(hh.handles_markers == 1);                        /* 24's end, waiting for the QUEUED that passed */
    log_end();
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 23) */
static void test_no_memory_frames(void)
{
    gptps *e; gptps_tui *t; gptps_tui_config c; FILE *out;
    log_begin();
    if ((e = open_manual()) != NULL) {
        out = tmpfile();
        memset(&c, 0, sizeof c);
        c.struct_size = sizeof c; c.max_recent = 4; c.out = out;
        t = out ? gptps_tui_install(e, &c) : NULL;
        CHECK(t != NULL);
        if (t) {
            tui_malloc_fail = 1;
            gptps_tui_snapshot(t);
            tui_malloc_fail = 0;
            CHECK(log_count("gptps_tui_snapshot: no memory for the 16 KiB frame buffer") == 1);
            CHECK(ftell(out) == 0);                        /* and no frame */
#if !defined(_WIN32)
            {   /* run needs a terminal both ways before it allocates: a pty on stdin and out */
                int m = posix_openpt(O_RDWR | O_NOCTTY);
                FILE *tty = NULL;
                if (m >= 0 && grantpt(m) == 0 && unlockpt(m) == 0 && freopen(ptsname(m), "r", stdin) &&
                    (tty = fopen(ptsname(m), "w")) != NULL) {
                    t->cfg.out = tty; t->cfg.interactive = 1;
                    tui_malloc_fail = 1;
                    alarm(10);
                    gptps_tui_run(t);                      /* returns: no buffer to paint into */
                    alarm(0);
                    tui_malloc_fail = 0;
                    CHECK(log_count("gptps_tui_run: no memory for the 16 KiB frame buffer") == 1);
                    t->cfg.out = out; t->cfg.interactive = 0;
                    CHECK(freopen("/dev/null", "r", stdin) != NULL);
                } else printf("note: no pty here; run() no-memory check skipped\n");
                if (tty) fclose(tty);
                if (m >= 0) close(m);
            }
#endif
        }
        gptps_shutdown(e);
        gptps_tui_close(t);
        if (out) fclose(out);
    }
    /* a bounded engine ends setup at its first submit: the observer is refused */
    {
        gptps_config cfg; gptps_handle h;
        e = NULL;
        memset(&cfg, 0, sizeof cfg);
        cfg.struct_size = sizeof cfg; cfg.limits.struct_size = sizeof cfg.limits;
        cfg.limits.max_concurrent_tasks = 1; cfg.limits.max_memory_bytes = 1u << 20;
        cfg.mode = GPTPS_RUN_MANUAL; cfg.max_items = 4; cfg.max_payload_bytes = 16; cfg.max_result_bytes = 16;
        CHECK(gptps_open_ex(&cfg, &e) == GPTPS_OK);
        if (e) {
            deftask(e, "b_ok", ok_task);
            CHECK(gptps_submit(e, "b_ok", NULL, 0, &h) == GPTPS_OK);
            CHECK(install(e) == NULL);
            CHECK(log_count("the engine refused the dashboard's observer") == 1);
            drain(e);
            gptps_shutdown(e);
        }
    }
    log_end();
}

/* 17) */
static void test_counters_64(void)
{
    gptps *e = open_manual(); gptps_tui *t; tui_task *k; static char frame[16384]; char b[16];
    if (!e) return;
    t = install(e);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    emit(t, GPTPS_EV_QUEUED, "big", 1, GPTPS_OK, 1);
    k = row(t, "big");
    CHECK(k != NULL);
    if (k) {
        t->q = 6000000000ULL; t->fin = 5000000000ULL;      /* past 2^32, where 32 bits wrapped */
        k->started = 5000000000ULL; k->finished = 5000000000ULL;
        k->lat_n = 5000000000ULL; k->lat_sum_ms = 10000000000ULL;   /* avg 2.0 ms */
        t->cols = 80; t->rows = 24;
        gptps_tui_render(t, frame, sizeof frame);
        CHECK(strstr(frame, "queued 6000000000") != NULL);
        CHECK(strstr(frame, "finished 5000000000") != NULL);
        CHECK(strstr(frame, "  5.0G  5.0G") != NULL);      /* run and ok, in 5 columns */
        CHECK(strstr(frame, "100%      2.0") != NULL);
        CHECK(widest(frame) <= 80);
        /* 1.11: below FULL the pending gauge is q - ends - in-flight, here 6e9: compact */
        CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_MINIMAL) == GPTPS_OK);
        gptps_tui_render(t, frame, sizeof frame);
        CHECK(strstr(frame, "pending+in-flight ~6.0G") != NULL && widest(frame) <= 80);   /* MINIMAL: one sum */
    }
    CHECK(strcmp(fmt_count(b, sizeof b, 99999u), "99999") == 0);
    CHECK(strcmp(fmt_count(b, sizeof b, 100000u), "100k") == 0);
    CHECK(strcmp(fmt_count(b, sizeof b, 999499u), "999k") == 0);
    CHECK(strcmp(fmt_count(b, sizeof b, 999500u), "1.0M") == 0);
    CHECK(strcmp(fmt_count(b, sizeof b, 1234567u), "1.2M") == 0);
    CHECK(strcmp(fmt_count(b, sizeof b, UINT64_MAX), "18E") == 0);
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 18) */
static void test_clamps_logged(void)
{
    gptps *e = open_manual(); gptps_tui *t1, *t2; gptps_tui_config c; static char frame[32768]; int i;
    if (!e) return;
    log_begin();
    memset(&c, 0, sizeof c);
    /* 1.11: max_recent's cap is 4096 (was 64), and 9 is a tier (LATENCY) */
    c.struct_size = sizeof c; c.max_recent = 5000; c.kpi = 12; c.mode = 7; c.latency_window = (1 << 20) + 1;
    t1 = gptps_tui_install(e, &c);
    CHECK(t1 != NULL);
    CHECK(log_count("cfg.max_recent 5000 is above the cap of 4096") == 1);
    CHECK(log_count("cfg.kpi 12 is not a gptps_tui_kpi") == 1);
    CHECK(log_count("cfg.mode 7 is not a gptps_tui_mode") == 1);
    CHECK(log_count("cfg.latency_window 1048577 is above the cap") == 1);
    if (t1) {
        CHECK(t1->cfg.max_recent == 4096 && t1->kpi == GPTPS_TUI_KPI_FULL && t1->mode == GPTPS_TUI_CONTINUOUS &&
              t1->lat_cap == (1 << 20));
        CHECK(gptps_tui_set_latency_window(t1, (1 << 20) + 5) == GPTPS_OK);
        CHECK(log_count("gptps_tui_set_latency_window: 1048581 is above the cap") == 1);
        CHECK(gptps_tui_set_kpi(t1, GPTPS_TUI_KPI_MINIMAL) == GPTPS_OK);   /* give back the 24 MiB */
    }
    t2 = install(e);                                       /* a second dashboard: its tui.* keys are taken */
    CHECK(t2 != NULL);
    CHECK(log_count("setting tui.kpi was not registered") == 1);
    CHECK(log_count("setting tui.latency_window was not registered") == 1);
    if (t2) {
        for (i = 0; i < TUI_MAX_TASKS + 2; ++i) {          /* 66 types: two get no row */
            char nm[32];
            if (i == TUI_MAX_TASKS) {                      /* the table is full: add_task says so, */
                CHECK(gptps_tui_add_task(t2, "hostkey", "h", 'z', NULL, 0) == GPTPS_E_FULL);
                CHECK(t2->nfold == 0 && log_count("gets no row") == 0);   /* without using up the WARN */
            }
            snprintf(nm, sizeof nm, "type_%02d", i);
            emit(t2, GPTPS_EV_QUEUED, nm, (gptps_handle)(i + 1), GPTPS_OK, 1);
            emit(t2, GPTPS_EV_STARTED, nm, (gptps_handle)(i + 1), GPTPS_OK, 1);
            emit(t2, GPTPS_EV_QUEUED, nm, (gptps_handle)(i + 100), GPTPS_OK, 1);
        }
        CHECK(nrows(t2) == TUI_MAX_TASKS && t2->nfold == 2);
        CHECK(t2->tasks[0].started == 2);                  /* the folded types count in '(other)' */
        CHECK(log_count("task type 'type_64' gets no row") == 1);   /* once, for the first */
        CHECK(log_count("gets no row") == 1);
        CHECK(health(t2).types_folded == 2 && health(t2).task_rows == TUI_MAX_TASKS);
        t2->rows = 200;
        gptps_tui_render(t2, frame, sizeof frame);
        CHECK(strstr(frame, "+2 types folded into (other) (the table holds 64)") != NULL);
        CHECK(strstr(frame, "  (other)              2") != NULL);   /* its row, with their counts */
        CHECK(strstr(frame, "lost: 2 types folded into (other)") != NULL);
    }
    /* cfg.max_tasks (1.11): a new field, refused out of range rather than clamped */
    memset(&c, 0, sizeof c);
    c.struct_size = sizeof c; c.max_tasks = TUI_TASKS_CAP + 1;
    CHECK(gptps_tui_install(e, &c) == NULL);
    CHECK(log_count("cfg.max_tasks 4097 is outside 1..4096") == 1);
    c.max_tasks = -1;
    CHECK(gptps_tui_install(e, &c) == NULL);
    CHECK(log_count("cfg.max_tasks -1 is outside") == 1);
    log_end();
    gptps_shutdown(e);
    gptps_tui_close(t1);
    gptps_tui_close(t2);
}

/* 19) */
static void test_frame_truncated(void)
{
    gptps *e = open_manual(); gptps_tui *t; char small[400]; static char frame[16384]; size_t n; int i;
    if (!e) return;
    t = install_cfg(e, GPTPS_TUI_KPI_FULL, 64, 1);         /* colour on: escapes to cut through */
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    t->cfg.unicode = 1;
    for (i = 0; i < 12; ++i) {
        char nm[32];
        snprintf(nm, sizeof nm, "t\xc3\xa9l\xc3\xa9_%02d", i);
        emit(t, GPTPS_EV_QUEUED, nm, (gptps_handle)(i + 1), GPTPS_OK, 1);
    }
    n = gptps_tui_render(t, frame, sizeof frame);
    CHECK(n < sizeof frame && strstr(frame, "[frame truncated]") == NULL);   /* it fits: no marker */
    n = gptps_tui_render(t, small, sizeof small);
    CHECK(n < sizeof small && small[n] == 0 && strlen(small) == n);
    CHECK(n >= 18 && strcmp(small + n - 18, "[frame truncated]\n") == 0);
    CHECK(strstr(small, "\n\x1b[0m[frame truncated]\n") != NULL);   /* after a whole line, colour reset */
    CHECK(valid_utf8(small) && escapes_whole(small));
    t->pane = TUI_PANE_HELP;                               /* every pane ends the same way */
    n = gptps_tui_render(t, small, sizeof small);
    CHECK(n >= 18 && strcmp(small + n - 18, "[frame truncated]\n") == 0 && valid_utf8(small) && escapes_whole(small));
    t->pane = TUI_PANE_DASH;
    n = gptps_tui_render(t, small, 8);                     /* no room for the marker: still a string */
    CHECK(n < 8 && small[n] == 0);
    /* one line, no newline to cut at: frame_finish itself. cap 40 with colour leaves
     * cut = 40 - 24 = 16 for the text */
    {
        char b[64];
        memset(b, 'b', sizeof b);
        memcpy(b, "aaaaaaaaaaaaa\x1b[31m", 18);           /* ESC at 13: cut 16 is inside "\x1b[3" */
        n = frame_finish(1, b, 40, 40);
        CHECK(strcmp(b, "aaaaaaaaaaaaa\x1b[0m\n[frame truncated]\n") == 0 && n == strlen(b));
        CHECK(escapes_whole(b) && valid_utf8(b));
        memset(b, 'b', sizeof b);
        memcpy(b, "aaaaaaaaaaaaaaa\xc3\xa9", 17);          /* U+00E9 at 15..16: cut 16 is inside it */
        n = frame_finish(1, b, 40, 40);
        CHECK(strcmp(b, "aaaaaaaaaaaaaaa\x1b[0m\n[frame truncated]\n") == 0 && n == strlen(b));
        CHECK(valid_utf8(b));
        /* cap too small for the marker: the cut (cap - 1 = 7) still keeps whole ones */
        memset(b, 'b', sizeof b);
        memcpy(b, "aaaaa\xe2\x82\xac", 8);                /* U+20AC at 5..7 */
        n = frame_finish(1, b, 8, 8);
        CHECK(n == 5 && strcmp(b, "aaaaa") == 0 && valid_utf8(b));
        memset(b, 'z', sizeof b);
        memcpy(b, "aaaa\x1b[31m", 9);                       /* "\x1b[3" at 4..6 */
        n = frame_finish(1, b, 8, 8);
        CHECK(n == 4 && strcmp(b, "aaaa") == 0 && escapes_whole(b));
    }
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 20) */
static void test_toast_fades(void)
{
    static const int panes[] = { TUI_PANE_SETTINGS, TUI_PANE_TASKS, TUI_PANE_DL, TUI_PANE_DASH };
    gptps *e = open_manual(); gptps_tui *t; static char frame[16384]; size_t i; uint64_t now;
    if (!e) return;
    t = install(e);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    for (i = 0; i < sizeof panes / sizeof panes[0]; ++i) {
        t->pane = panes[i];
        snprintf(t->status, sizeof t->status, "toast-%d-here", panes[i]);
        t->status_ms = gptps_now_ms(NULL);
        gptps_tui_render(t, frame, sizeof frame);
        CHECK(strstr(frame, t->status) != NULL);          /* shown while fresh */
        t->status_ms = gptps_now_ms(NULL) - 4000;
        gptps_tui_render(t, frame, sizeof frame);
        if (strstr(frame, t->status)) printf("pane %d kept its toast\n", panes[i]);
        CHECK(strstr(frame, t->status) == NULL);          /* gone after 3 s, on every pane */
    }
    t->pane = TUI_PANE_DASH;
    now = gptps_now_ms(NULL);
    t->status_ms = now;
    CHECK(toast_until(t, now) == now + TUI_TOAST_MS);
    t->status_ms = now - 4000;
    CHECK(toast_until(t, now) == 0);
    /* ON_DEMAND repaints once when the toast it showed expires; not before, not after */
    CHECK(paint_due(0, 0, GPTPS_TUI_ON_DEMAND, 0, 1000, 999) == 0);
    CHECK(paint_due(0, 0, GPTPS_TUI_ON_DEMAND, 0, 1000, 1000) == 1);
    CHECK(paint_due(0, 0, GPTPS_TUI_ON_DEMAND, 0, 0, 5000) == 0);
    CHECK(paint_due(0, 0, GPTPS_TUI_ON_DEMAND, 1, 0, 5000) == 1);
    CHECK(paint_due(0, 0, GPTPS_TUI_PAUSED, 1, 1000, 5000) == 0);   /* PAUSED stays frozen */
    CHECK(paint_due(0, 1, GPTPS_TUI_PAUSED, 0, 0, 5000) == 1);
    /* 1.11: CONTINUOUS too paints only when something it shows changed (the generation
     * skip; the loop asks every refresh_ms) - it used to repaint every frame */
    CHECK(paint_due(0, 0, GPTPS_TUI_CONTINUOUS, 0, 0, 5000) == 0);
    CHECK(paint_due(0, 0, GPTPS_TUI_CONTINUOUS, 1, 0, 5000) == 1);
    CHECK(paint_due(0, 0, GPTPS_TUI_CONTINUOUS, 0, 4000, 5000) == 1);
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 21) - one engine per dashboard: each registers its observer and settings there */
static void test_struct_size(void)
{
    union { gptps_tui_config c; unsigned char b[sizeof(gptps_tui_config) + 64]; } u;
    gptps_tui_config c0;
    gptps *e; gptps_tui *t;
    log_begin();
    memset(&c0, 0, sizeof c0);
    c0.color = 0; c0.interactive = 0; c0.max_recent = 4; c0.title = "old caller";
    /* an older caller's struct, ending at `title`, and garbage after it */
    memset(&u, 0xab, sizeof u);
    memcpy(&u, &c0, TUI_CFG_MIN);
    u.c.struct_size = TUI_CFG_MIN;
    if ((e = open_manual()) != NULL) {
        t = gptps_tui_install(e, &u.c);
        CHECK(t != NULL);
        if (t) {
            CHECK(t->cfg.out == stdout && t->cfg.settings_path == NULL && t->cfg.unicode == 0);
            CHECK(strcmp(t->cfg.title, "old caller") == 0 && t->cfg.max_recent == 4);
            CHECK(t->cfg.struct_size == sizeof t->cfg);
        }
        gptps_shutdown(e);
        gptps_tui_close(t);
    }
    /* a newer caller's bigger struct: only what this build knows is read */
    memset(&u, 0xab, sizeof u);
    memcpy(&u, &c0, sizeof c0);
    u.c.struct_size = sizeof u;
    if ((e = open_manual()) != NULL) {
        t = gptps_tui_install(e, &u.c);
        CHECK(t != NULL && t->cfg.unicode == 0 && t->cfg.out == stdout && t->cfg.struct_size == sizeof t->cfg);
        gptps_shutdown(e);
        gptps_tui_close(t);
    }
    /* too short to be a config: refused, and said */
    u.c.struct_size = 8;
    if ((e = open_manual()) != NULL) {
        CHECK(gptps_tui_install(e, &u.c) == NULL);
        CHECK(log_count("cfg.struct_size 8 is smaller than any gptps_tui_config") == 1);
        gptps_shutdown(e);
    }
    /* 0: the whole struct, as before */
    c0.struct_size = 0;
    if ((e = open_manual()) != NULL) {
        t = gptps_tui_install(e, &c0);
        CHECK(t != NULL && t->cfg.max_recent == 4);
        gptps_shutdown(e);
        gptps_tui_close(t);
    }
    log_end();
}

/* ---- 1.11 ------------------------------------------------------------------------ */

/* 24) a FAILED's why looks at the last 512 bytes of its stderr tail only, and with no
 * line break there it shows the END of that line (the start was 64 KiB back) */
static void test_why_bounded(void)
{
    static char tail[65536 + 64];
    char out[96];
    size_t n = 65536;
    memset(tail, 'x', n);
    memcpy(tail + n - 11, "LAST-WORDS\n", 11);          /* no newline before it: one 64 KiB line */
    why_of(tail, n, out, sizeof out);
    CHECK(strstr(out, "\xe2\x80\xa6") != NULL);         /* an ellipsis: the line is longer than shown */
    CHECK(strstr(out, "LAST-WORDS") != NULL);           /* and what was written last is there */
    CHECK(clean_utf8(out) && strlen(out) < sizeof out);
    /* a line break inside the window: that line, from its start, as before */
    memcpy(tail + n - 200, "\nstart of the last line", 23);
    why_of(tail, n, out, sizeof out);
    CHECK(strstr(out, "start of the last line") != NULL && strstr(out, "\xe2\x80\xa6") == NULL);
    /* a line break just outside it does not count: the window holds no line start */
    memset(tail, 'x', n);
    tail[n - 600] = '\n';
    memcpy(tail + n - 11, "LAST-WORDS\n", 11);
    why_of(tail, n, out, sizeof out);
    CHECK(strstr(out, "\xe2\x80\xa6") != NULL && strstr(out, "LAST-WORDS") != NULL);
    /* the ellipsis cut lands on a whole character */
    {
        size_t k;
        for (k = 0; k + 2 <= 600; k += 2) { tail[k] = '\xc3'; tail[k + 1] = '\xa9'; }
        why_of(tail, 600, out, sizeof out);
        CHECK(clean_utf8(out) && strstr(out, "\xe2\x80\xa6\xc3\xa9") != NULL);
        /* an odd length: the event path's own cut (TUI_TAIL_KEEP from the end) falls on a
         * continuation byte, and the record must still start on a whole character */
        {
            gptps_event ev; tui_event re;
            memset(&ev, 0, sizeof ev);
            ev.struct_size = sizeof ev; ev.kind = GPTPS_EV_FAILED; ev.status = GPTPS_E_TASK;
            ev.stderr_tail = tail; ev.stderr_tail_len = 599;    /* ends on a lone lead byte */
            why_record(&ev, &re);
            CHECK(re.tail_cut == 1 && re.tail_len > 0 && ((unsigned char)re.tail[0] & 0xc0u) != 0x80u);
            ev.stderr_tail_len = 600 - 1 - 2 * 10;              /* and another odd one */
            why_record(&ev, &re);
            CHECK(re.tail_cut == 1 && re.tail_len > 0 && ((unsigned char)re.tail[0] & 0xc0u) != 0x80u);
        }
    }
    /* a line that starts exactly where the window starts is whole: no ellipsis, its start kept */
    {
        size_t s0 = 0, e0 = 0;
        memset(tail, 'y', sizeof tail);
        tail[0] = 'x'; tail[1] = '\n';                         /* lo = 2: tail[lo - 1] is the line break */
        memcpy(tail + 2, "FIRST", 5);
        CHECK(why_tail(tail, 2 + TUI_WHY_SCAN, 80, &s0, &e0) == 0 && s0 == 2 && e0 == 82);
        why_of(tail, 2 + TUI_WHY_SCAN, out, sizeof out);
        CHECK(strstr(out, "\xe2\x80\xa6") == NULL && strstr(out, "FIRST") != NULL);
    }
}

/* 25) the event path allocates nothing, at any level - folding, a full handle table,
 * markers expiring, a FAILED with a long tail, and every kind of event */
static void test_event_path_no_alloc(void)
{
    static const int lv[] = { GPTPS_TUI_KPI_OFF, GPTPS_TUI_KPI_MINIMAL, GPTPS_TUI_KPI_GAUGE, GPTPS_TUI_KPI_PROBLEMS,
                              GPTPS_TUI_KPI_TASKS, GPTPS_TUI_KPI_NORMAL, GPTPS_TUI_KPI_LATENCY, GPTPS_TUI_KPI_FULL };
    static char tail[4096];
    gptps *e = open_manual(); gptps_tui *t; gptps_tui_config c;
    size_t l;
    int i;
    if (!e) return;
    memset(tail, 'y', sizeof tail);
    memset(&c, 0, sizeof c);
    c.struct_size = sizeof c; c.max_tasks = 4; c.latency_window = 8; c.max_recent = 8;
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    for (l = 0; l < sizeof lv / sizeof lv[0]; ++l) {
        int before;
        CHECK(gptps_tui_set_kpi(t, (gptps_tui_kpi)lv[l]) == GPTPS_OK);
        before = tui_allocs;
        for (i = 0; i < 2000; ++i) {
            char nm[16];
            gptps_handle h = (gptps_handle)(l * 100000 + (size_t)i + 1);
            gptps_event ev;
            snprintf(nm, sizeof nm, "ty%d", i % 9);         /* 9 types, 4 rows: folding */
            if (i % 3 == 0) emit(t, GPTPS_EV_QUEUED, nm, h, GPTPS_OK, (uint64_t)i);
            if (i % 3 == 0) emit(t, GPTPS_EV_QUEUED, nm, h + 50000, GPTPS_OK, (uint64_t)i);   /* never ends: fills the table */
            emit_a(t, GPTPS_EV_STARTED, nm, h, GPTPS_OK, (uint64_t)i, 1);
            if (i % 5 == 0) {
                memset(&ev, 0, sizeof ev);
                ev.struct_size = sizeof ev; ev.kind = GPTPS_EV_FAILED; ev.status = GPTPS_E_TASK; ev.attempt = 1;
                ev.task_name = nm; ev.handle = h; ev.stderr_tail = tail; ev.stderr_tail_len = sizeof tail;
                tui_on_event(&ev, t);
                emit_a(t, GPTPS_EV_RETRIED, nm, h, GPTPS_E_TASK, (uint64_t)i, 2);
                emit_a(t, GPTPS_EV_DEAD_LETTERED, nm, h, GPTPS_E_TASK, (uint64_t)i, 2);
            } else if (i % 7 == 0) {
                emit_a(t, GPTPS_EV_FAILED, nm, h, GPTPS_E_CANCELLED, (uint64_t)i, 1);
            } else if (i % 11 == 0) {
                emit_a(t, GPTPS_EV_DROPPED, nm, h, GPTPS_E_TASK, (uint64_t)i, 1);
            } else {
                emit_a(t, GPTPS_EV_FINISHED, nm, h, GPTPS_OK, (uint64_t)i + 3, 1);   /* most without a QUEUED */
            }
        }
        if (tui_allocs != before) printf("level %s: %d allocations on the event path\n", kpi_str(lv[l]), tui_allocs - before);
        CHECK(tui_allocs == before);
    }
    CHECK(health(t).types_folded == 5 && health(t).orphans_expired > 0 && health(t).handles_untracked > 0);
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 26) cancel while queued: the gauges stay right at FULL (the handle table knows the item
 * never ran); below FULL the counters cannot know, and the frame says '~' */
static gptps_tui *cq_t;
static gptps *cq_e;
static gptps_handle cq_h2;
static char cq_full[16384], cq_normal[16384];
static gptps_tui_health_info cq_hf;
static gptps_status cq_body(gptps_ctx *c, void *u)
{
    (void)c; (void)u;
    CHECK(gptps_cancel(cq_e, cq_h2) == GPTPS_OK);          /* the queued one: its FAILED/E_CANCELLED now */
    gptps_tui_render(cq_t, cq_full, sizeof cq_full);       /* this one still runs */
    cq_hf = health(cq_t);
    return GPTPS_OK;
}
static void test_cancel_in_queue(void)
{
    int pass;
    for (pass = 0; pass < 2; ++pass) {
        gptps *e = open_manual(); gptps_handle h1;
        if (!e) return;
        cq_e = e;
        cq_t = install_cfg(e, pass == 0 ? GPTPS_TUI_KPI_FULL : GPTPS_TUI_KPI_NORMAL, 64, 0);
        CHECK(cq_t != NULL);
        if (!cq_t) { gptps_shutdown(e); return; }
        deftask(e, "cq", cq_body);
        CHECK(gptps_submit(e, "cq", NULL, 0, &h1) == GPTPS_OK);
        CHECK(gptps_submit(e, "cq", NULL, 0, &cq_h2) == GPTPS_OK);   /* max 1 at a time: this one waits */
        drain(e);
        if (pass == 0) {
            if (!strstr(cq_full, "pending 0  in-flight 1")) printf("%s\n", cq_full);
            CHECK(strstr(cq_full, "pending 0  in-flight 1") != NULL);   /* exact: 1 runs, 0 waits */
            CHECK(cq_hf.gauges == GPTPS_TUI_GAUGES_EXACT && cq_hf.in_flight == 1 && cq_hf.pending == 0);
        } else {
            memcpy(cq_normal, cq_full, sizeof cq_normal);
            /* the documented approximation: every FAILED leaves the running set */
            CHECK(strstr(cq_normal, "in-flight ~0") != NULL);
            CHECK(cq_hf.gauges == GPTPS_TUI_GAUGES_APPROX && cq_hf.in_flight == 0);
        }
        CHECK(health(cq_t).in_flight == 0 && health(cq_t).pending == 0);   /* and both end at 0 */
        gptps_shutdown(e);
        gptps_tui_close(cq_t);
    }
}

/* 27) gptps_tui_health honours struct_size: an older, smaller struct gets what it has
 * and nothing past it */
static void test_health_struct_size(void)
{
    union { gptps_tui_health_info h; unsigned char b[sizeof(gptps_tui_health_info) + 32]; } u;
    gptps *e = open_manual(); gptps_tui *t;
    size_t small = offsetof(gptps_tui_health_info, alloc_failures) + sizeof(uint64_t), k;
    if (!e) return;
    t = install(e);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    t->h_folded = 7; t->h_nomem = 3;
    memset(&u, 0xa5, sizeof u);
    u.h.struct_size = small;
    CHECK(gptps_tui_health(t, &u.h) == GPTPS_OK);
    CHECK(u.h.struct_size == small && u.h.types_folded == 7 && u.h.alloc_failures == 3);
    for (k = small; k < sizeof u.b; ++k) if (u.b[k] != 0xa5) break;
    CHECK(k == sizeof u.b);                                /* the canary past it is untouched */
    u.h.struct_size = offsetof(gptps_tui_health_info, journal_not_shown);   /* too short to hold anything */
    CHECK(gptps_tui_health(t, &u.h) == GPTPS_E_INVAL);
    CHECK(gptps_tui_health(NULL, &u.h) == GPTPS_E_INVAL && gptps_tui_health(t, NULL) == GPTPS_E_INVAL);
    u.h.struct_size = sizeof u.h;
    CHECK(gptps_tui_health(t, &u.h) == GPTPS_OK && u.h.bytes_tasks > 0 && u.h.bytes_handles > 0 &&
          u.h.bytes_activity > 0 && u.h.bytes_base > 0);
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_MINIMAL) == GPTPS_OK);
    CHECK(gptps_tui_health(t, &u.h) == GPTPS_OK && u.h.bytes_tasks == 0 && u.h.bytes_handles == 0 &&
          u.h.bytes_activity == 0);                       /* MINIMAL holds no per-event part */
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 28) many types, each its own row through the name index; rows keep their ids */
static void test_name_index(void)
{
    gptps *e = open_manual(); gptps_tui *t; gptps_tui_config c;
    int i, ok = 1;
    if (!e) return;
    memset(&c, 0, sizeof c);
    c.struct_size = sizeof c; c.max_tasks = 1000; c.kpi = GPTPS_TUI_KPI_NORMAL;
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    for (i = 0; i < 3000; ++i) {
        char nm[16];
        snprintf(nm, sizeof nm, "type_%04d", i % 1000);
        emit(t, GPTPS_EV_STARTED, nm, (gptps_handle)(i + 1), GPTPS_OK, 1);
    }
    for (i = 0; i < 1000 && ok; ++i) {
        char nm[16];
        tui_task *k;
        snprintf(nm, sizeof nm, "type_%04d", i);
        k = row(t, nm);
        if (!k || k->started != 3 || k != &t->tasks[i + 1]) ok = 0;   /* its own, in first-seen order */
    }
    CHECK(ok && nrows(t) == 1000 && t->nfold == 0 && t->tasks[0].started == 0);
    emit(t, GPTPS_EV_STARTED, "one_more", 9999, GPTPS_OK, 1);
    CHECK(t->nfold == 1 && t->tasks[0].started == 1 && nrows(t) == 1000);
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 30) ABI: the released layout and values are where 1.10 put them (static asserts), and a
 * 1.10 caller's struct_size - its sizeof, tail padding included - covers none of the 1.11
 * fields: garbage in that padding and after it is not read */
#define TUI_STATIC_ASSERT(name, c) typedef char tui_assert_##name[(c) ? 1 : -1]
TUI_STATIC_ASSERT(kpi_default, GPTPS_TUI_KPI_DEFAULT == 0);
TUI_STATIC_ASSERT(kpi_off, GPTPS_TUI_KPI_OFF == 1);
TUI_STATIC_ASSERT(kpi_minimal, GPTPS_TUI_KPI_MINIMAL == 2);
TUI_STATIC_ASSERT(kpi_normal, GPTPS_TUI_KPI_NORMAL == 3);
TUI_STATIC_ASSERT(kpi_full, GPTPS_TUI_KPI_FULL == 4);
/* 1.11: appended, never renumbered */
TUI_STATIC_ASSERT(kpi_gauge, GPTPS_TUI_KPI_GAUGE == 5);
TUI_STATIC_ASSERT(kpi_problems, GPTPS_TUI_KPI_PROBLEMS == 6);
TUI_STATIC_ASSERT(kpi_tasks, GPTPS_TUI_KPI_TASKS == 7);
TUI_STATIC_ASSERT(kpi_activity, GPTPS_TUI_KPI_ACTIVITY == 8);
TUI_STATIC_ASSERT(kpi_latency, GPTPS_TUI_KPI_LATENCY == 9);
TUI_STATIC_ASSERT(kpi_count, GPTPS_TUI_KPI_COUNT == GPTPS_TUI_KPI_MINIMAL);
TUI_STATIC_ASSERT(clock_vals, GPTPS_TUI_CLOCK_DEFAULT == 0 && GPTPS_TUI_CLOCK_OFF == 1 &&
                              GPTPS_TUI_CLOCK_MINUTES == 2 && GPTPS_TUI_CLOCK_SECONDS == 3);
TUI_STATIC_ASSERT(gauges_vals, GPTPS_TUI_GAUGES_APPROX == 0 && GPTPS_TUI_GAUGES_EXACT == 1 &&
                               GPTPS_TUI_GAUGES_LOWER_BOUND == 2 && GPTPS_TUI_GAUGES_SUM == 3);
/* the 1.11 fields, in their order after `reserved` */
TUI_STATIC_ASSERT(f111_order, offsetof(gptps_tui_config, max_tasks) == offsetof(gptps_tui_config, reserved) + 4 &&
                  offsetof(gptps_tui_config, recent_lines) == offsetof(gptps_tui_config, max_tasks) + 4 &&
                  offsetof(gptps_tui_config, activity_lines) == offsetof(gptps_tui_config, recent_lines) + 4 &&
                  offsetof(gptps_tui_config, clock) == offsetof(gptps_tui_config, activity_lines) + 4 &&
                  offsetof(gptps_tui_config, rate_window_s) == offsetof(gptps_tui_config, clock) + 4 &&
                  offsetof(gptps_tui_config, pane_refresh_ms) == offsetof(gptps_tui_config, rate_window_s) + 4);
TUI_STATIC_ASSERT(mode_cont, GPTPS_TUI_CONTINUOUS == 0);
TUI_STATIC_ASSERT(mode_od, GPTPS_TUI_ON_DEMAND == 1);
TUI_STATIC_ASSERT(mode_paused, GPTPS_TUI_PAUSED == 2);
/* the 1.10 struct's size on this target: through `unicode`, rounded to its alignment */
#define TUI_V110_SIZE ((offsetof(gptps_tui_config, unicode) + sizeof(int) + sizeof(void *) - 1) / sizeof(void *) * sizeof(void *))
TUI_STATIC_ASSERT(new_fields_past_110, offsetof(gptps_tui_config, max_tasks) >= TUI_V110_SIZE);
/* every released field in its 1.10 order, on every target */
TUI_STATIC_ASSERT(f110_order, offsetof(gptps_tui_config, struct_size) == 0 &&
                  offsetof(gptps_tui_config, refresh_ms) >= sizeof(size_t) &&
                  offsetof(gptps_tui_config, color) == offsetof(gptps_tui_config, refresh_ms) + 4 &&
                  offsetof(gptps_tui_config, interactive) == offsetof(gptps_tui_config, color) + 4 &&
                  offsetof(gptps_tui_config, max_recent) == offsetof(gptps_tui_config, interactive) + 4 &&
                  offsetof(gptps_tui_config, show_tasks) == offsetof(gptps_tui_config, max_recent) + 4 &&
                  offsetof(gptps_tui_config, show_recent) == offsetof(gptps_tui_config, show_tasks) + 4 &&
                  offsetof(gptps_tui_config, kpi) == offsetof(gptps_tui_config, show_recent) + 4 &&
                  offsetof(gptps_tui_config, mode) == offsetof(gptps_tui_config, kpi) + 4 &&
                  offsetof(gptps_tui_config, latency_window) == offsetof(gptps_tui_config, mode) + 4 &&
                  offsetof(gptps_tui_config, title) > offsetof(gptps_tui_config, latency_window) &&
                  offsetof(gptps_tui_config, out) == offsetof(gptps_tui_config, title) + sizeof(void *) &&
                  offsetof(gptps_tui_config, settings_path) == offsetof(gptps_tui_config, out) + sizeof(void *) &&
                  offsetof(gptps_tui_config, unicode) == offsetof(gptps_tui_config, settings_path) + sizeof(void *));
#if UINTPTR_MAX > 0xffffffffu
TUI_STATIC_ASSERT(lp64_struct_size, offsetof(gptps_tui_config, struct_size) == 0);
TUI_STATIC_ASSERT(lp64_refresh_ms, offsetof(gptps_tui_config, refresh_ms) == 8);
TUI_STATIC_ASSERT(lp64_color, offsetof(gptps_tui_config, color) == 12);
TUI_STATIC_ASSERT(lp64_interactive, offsetof(gptps_tui_config, interactive) == 16);
TUI_STATIC_ASSERT(lp64_max_recent, offsetof(gptps_tui_config, max_recent) == 20);
TUI_STATIC_ASSERT(lp64_show_tasks, offsetof(gptps_tui_config, show_tasks) == 24);
TUI_STATIC_ASSERT(lp64_show_recent, offsetof(gptps_tui_config, show_recent) == 28);
TUI_STATIC_ASSERT(lp64_kpi, offsetof(gptps_tui_config, kpi) == 32);
TUI_STATIC_ASSERT(lp64_mode, offsetof(gptps_tui_config, mode) == 36);
TUI_STATIC_ASSERT(lp64_latwin, offsetof(gptps_tui_config, latency_window) == 40);
TUI_STATIC_ASSERT(lp64_title, offsetof(gptps_tui_config, title) == 48);
TUI_STATIC_ASSERT(lp64_out, offsetof(gptps_tui_config, out) == 56);
TUI_STATIC_ASSERT(lp64_settings, offsetof(gptps_tui_config, settings_path) == 64);
TUI_STATIC_ASSERT(lp64_unicode, offsetof(gptps_tui_config, unicode) == 72);
TUI_STATIC_ASSERT(lp64_v110, TUI_V110_SIZE == 80);
TUI_STATIC_ASSERT(lp64_reserved, offsetof(gptps_tui_config, reserved) == 76);
TUI_STATIC_ASSERT(lp64_max_tasks, offsetof(gptps_tui_config, max_tasks) == 80);
TUI_STATIC_ASSERT(lp64_recent_lines, offsetof(gptps_tui_config, recent_lines) == 84);
TUI_STATIC_ASSERT(lp64_pane_refresh, offsetof(gptps_tui_config, pane_refresh_ms) == 100);
TUI_STATIC_ASSERT(lp64_size111, sizeof(gptps_tui_config) == 104);
#endif
static void test_config_abi(void)
{
    union { gptps_tui_config c; unsigned char b[sizeof(gptps_tui_config) + 64]; } u;
    gptps_tui_config c0;
    gptps *e; gptps_tui *t;
    memset(&c0, 0, sizeof c0);
    c0.color = 0; c0.interactive = 0; c0.max_recent = 4; c0.title = "v110";
    memset(&u, 0xab, sizeof u);                            /* padding and all past it: garbage */
    memcpy(&u, &c0, offsetof(gptps_tui_config, unicode) + sizeof(int));
    u.c.struct_size = TUI_V110_SIZE;                       /* what a 1.10 caller's sizeof says */
    if ((e = open_manual()) != NULL) {
        t = gptps_tui_install(e, &u.c);
        CHECK(t != NULL);
        if (t) CHECK(t->max_tasks == TUI_MAX_TASKS && strcmp(t->cfg.title, "v110") == 0 && t->cfg.max_recent == 4);
        gptps_shutdown(e);
        gptps_tui_close(t);
    }
    u.c.struct_size = 0;                                   /* 0: the 1.10 layout, likewise */
    if ((e = open_manual()) != NULL) {
        t = gptps_tui_install(e, &u.c);
        CHECK(t != NULL && t->max_tasks == TUI_MAX_TASKS);
        gptps_shutdown(e);
        gptps_tui_close(t);
    }
    memcpy(&u, &c0, sizeof c0);                            /* a 1.11 caller: max_tasks is read */
    u.c.struct_size = sizeof u.c; u.c.max_tasks = 7;
    if ((e = open_manual()) != NULL) {
        t = gptps_tui_install(e, &u.c);
        CHECK(t != NULL && t->max_tasks == 7 && t->tasks_cap == 8);
        gptps_shutdown(e);
        gptps_tui_close(t);
    }
}

#if !defined(_WIN32)
/* 29) storage: parts swapped in and out at runtime while four threads emit, one renders
 * and reads health, one binds tasks - under ASan and TSan this is the free protocol's
 * test (allocated unlocked, swapped under the lock, freed after it is swapped out) */
static gptps_tui *st_t;
static int st_stop;
static void *st_emitter(void *arg)
{
    long id = (long)(intptr_t)arg, n = 0;
    char nm[16];
    while (!__atomic_load_n(&st_stop, __ATOMIC_ACQUIRE)) {
        gptps_handle h = (gptps_handle)(id * 100000000L + (++n));
        /* At OFF the observer returns before its lock, so this loop would make no call a
         * cooperative scheduler sees: the simulation HAL (tests/hal_sim.c) runs one thread
         * at a time and switches only at HAL / pthread calls, and called it a stall. The
         * engine clock is one such call - as an engine's own emitting thread makes. */
        (void)gptps_now_ms(NULL);
        snprintf(nm, sizeof nm, "s%ld", n % 13);
        emit(st_t, GPTPS_EV_QUEUED, nm, h, GPTPS_OK, (uint64_t)n);
        emit_a(st_t, GPTPS_EV_STARTED, nm, h, GPTPS_OK, (uint64_t)n, 1);
        if (n % 4 == 0) emit_a(st_t, GPTPS_EV_FAILED, nm, h, GPTPS_E_CANCELLED, (uint64_t)n, 1);
        else emit_a(st_t, GPTPS_EV_FINISHED, nm, h, GPTPS_OK, (uint64_t)n + 1, 1);
    }
    return NULL;
}
static void *st_viewer(void *arg)
{
    static char frame[16384];
    int i = 0;
    (void)arg;
    while (!__atomic_load_n(&st_stop, __ATOMIC_ACQUIRE)) {
        char nm[16];
        gptps_tui_health_info h;
        gptps_tui_render(st_t, frame, sizeof frame);
        memset(&h, 0, sizeof h); h.struct_size = sizeof h;
        gptps_tui_health(st_t, &h);
        snprintf(nm, sizeof nm, "s%d", i++ % 20);
        gptps_tui_add_task(st_t, nm, nm, 'a' + i % 20, "pl", 2);
    }
    return NULL;
}
static void test_storage_storm(void)
{
    static const int lv[] = { GPTPS_TUI_KPI_FULL, GPTPS_TUI_KPI_OFF, GPTPS_TUI_KPI_NORMAL, GPTPS_TUI_KPI_MINIMAL,
                              GPTPS_TUI_KPI_FULL, GPTPS_TUI_KPI_NORMAL,
                              /* 1.11: every tier, up and down */
                              GPTPS_TUI_KPI_GAUGE, GPTPS_TUI_KPI_LATENCY, GPTPS_TUI_KPI_PROBLEMS, GPTPS_TUI_KPI_TASKS,
                              GPTPS_TUI_KPI_FULL, GPTPS_TUI_KPI_ACTIVITY, GPTPS_TUI_KPI_GAUGE };
    gptps *e = open_manual(); gptps_tui_config c;
    pthread_t th[6];
    int i, made = 0;
    uint64_t until;
    if (!e) return;
    memset(&c, 0, sizeof c);
    c.struct_size = sizeof c; c.max_tasks = 8; c.latency_window = 32;
    st_t = gptps_tui_install(e, &c);
    CHECK(st_t != NULL);
    if (!st_t) { gptps_shutdown(e); return; }
    __atomic_store_n(&st_stop, 0, __ATOMIC_RELEASE);
    for (i = 0; i < 4; ++i) if (pthread_create(&th[made], NULL, st_emitter, (void *)(intptr_t)(i + 1)) == 0) ++made;
    if (pthread_create(&th[made], NULL, st_viewer, NULL) == 0) ++made;
    until = gptps_now_ms(NULL) + 600;
    for (i = 0; gptps_now_ms(NULL) < until; ++i) {
        gptps_tui_set_kpi(st_t, (gptps_tui_kpi)lv[i % (int)(sizeof lv / sizeof lv[0])]);
        if (i % 3 == 0) gptps_tui_set_latency_window(st_t, i % 2 ? 16 : 64);
        if (i % 5 == 0) gptps_tui_set_recent(st_t, i % 4, (i / 5) % 3 * 7);   /* 1.11: journals resized, off, on */
    }
    __atomic_store_n(&st_stop, 1, __ATOMIC_RELEASE);
    for (i = 0; i < made; ++i) pthread_join(th[i], NULL);
    CHECK(made == 5);
    /* the counters stayed whole: at FULL with the storm over, nothing is live */
    CHECK(gptps_tui_set_kpi(st_t, GPTPS_TUI_KPI_FULL) == GPTPS_OK);
    CHECK(health(st_t).in_flight == 0 && health(st_t).pending == 0);
    gptps_shutdown(e);
    gptps_tui_close(st_t);
}
#endif

/* 31) the '>=' mark (a lower bound) is cleared only on the engine's word that nothing is
 * queued or running, never from the counters: an always-up service's FINISHED per run
 * (and no QUEUED) put term above q, so 'q <= term' called a table that had refused
 * handles exact; a table built while work waited (a late install, a new window) was
 * called exact with that work missing from it */
static void test_lower_bound_sticks(void)
{
    gptps *e = open_manual(); gptps_tui *t; gptps_handle h; gptps_tui_health_info hh;
    static char frame[16384];
    int i;
    if (!e) return;
    deftask(e, "job", ok_task);
    log_begin();
    t = install_cfg(e, GPTPS_TUI_KPI_FULL, 2, 0);
    CHECK(t != NULL && t->tab != NULL);
    if (!t || !t->tab) { log_end(); gptps_shutdown(e); gptps_tui_close(t); return; }
    CHECK(health(t).gauges == GPTPS_TUI_GAUGES_EXACT);    /* an idle engine: exact at once */
    /* a service that has run 10 times: 10 FINISHED, no QUEUED (term 10, q 0) */
    emit_a(t, GPTPS_EV_STARTED, "svc", 900000, GPTPS_OK, 1, 1);
    for (i = 0; i < 10; ++i) {
        emit_a(t, GPTPS_EV_FINISHED, "svc", 900000, GPTPS_OK, 2, 1);
        emit_a(t, GPTPS_EV_STARTED, "svc", 900000, GPTPS_OK, 3, 1);
    }
    for (i = 0; i < 5; ++i) CHECK(gptps_submit(e, "job", NULL, 0, &h) == GPTPS_OK);   /* window 2: 1 has room */
    CHECK(t->q == 5 && t->term == 10 && t->g_pend == 1 && t->g_run == 1);
    hh = health(t);
    CHECK(hh.gauges == GPTPS_TUI_GAUGES_LOWER_BOUND && hh.handles_untracked == 4);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "pending >=1  in-flight >=1") != NULL);
    /* a new window, built while the 5 wait: none of them is in it */
    CHECK(gptps_tui_set_latency_window(t, 64) == GPTPS_OK);
    hh = health(t);
    CHECK(hh.gauges == GPTPS_TUI_GAUGES_LOWER_BOUND && hh.pending == 0);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "pending >=0") != NULL);
    drain(e);                                              /* all 5 run and end: the engine is idle */
    hh = health(t);
    CHECK(hh.gauges == GPTPS_TUI_GAUGES_EXACT && hh.pending == 0 && hh.in_flight == 0);
    log_end();
    gptps_shutdown(e);
    gptps_tui_close(t);
    /* installed late: 5 already waiting, so a table built now is a lower bound */
    e = open_manual();
    if (!e) return;
    deftask(e, "job", ok_task);
    for (i = 0; i < 5; ++i) CHECK(gptps_submit(e, "job", NULL, 0, &h) == GPTPS_OK);
    t = install_cfg(e, GPTPS_TUI_KPI_FULL, 64, 0);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    hh = health(t);
    CHECK(hh.gauges == GPTPS_TUI_GAUGES_LOWER_BOUND && hh.pending == 0);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "pending >=0") != NULL);
    drain(e);
    CHECK(health(t).gauges == GPTPS_TUI_GAUGES_EXACT);
    /* raised to FULL while work waits: the same */
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_NORMAL) == GPTPS_OK);
    CHECK(gptps_submit(e, "job", NULL, 0, &h) == GPTPS_OK);
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_FULL) == GPTPS_OK);
    CHECK(health(t).gauges == GPTPS_TUI_GAUGES_LOWER_BOUND);
    drain(e);
    CHECK(health(t).gauges == GPTPS_TUI_GAUGES_EXACT);
    gptps_shutdown(e);
    gptps_tui_close(t);
}

#if !defined(_WIN32)
/* 32) POSIX: gptps_tui_health on another thread while the dashboard takes dead letters
 * into its keeping (the array grows): held_cap is written under the lock health reads it
 * under (a thread sanitizer flagged the pair) */
static gptps_tui *dh_t;
static int dh_stop;
static void *dh_reader(void *a)
{
    (void)a;
    while (!__atomic_load_n(&dh_stop, __ATOMIC_SEQ_CST)) (void)health(dh_t);
    return NULL;
}
static void test_health_vs_dead_letters(void)
{
    gptps *e = open_manual(); pthread_t th; gptps_handle h;
    int i, ok;
    if (!e) return;
    deftask(e, "bad", fail_task);
    dh_t = install(e);
    CHECK(dh_t != NULL);
    if (!dh_t) { gptps_shutdown(e); return; }
    __atomic_store_n(&dh_stop, 0, __ATOMIC_SEQ_CST);
    ok = pthread_create(&th, NULL, dh_reader, NULL) == 0;
    CHECK(ok);
    gptps_tui_press(dh_t, 'l');
    for (i = 0; i < 40; ++i) {
        CHECK(gptps_submit(e, "bad", NULL, 0, &h) == GPTPS_OK);
        drain(e);
        gptps_tui_press(dh_t, 'g');                        /* take it: the array grows 16, 32, 64 */
    }
    __atomic_store_n(&dh_stop, 1, __ATOMIC_SEQ_CST);
    if (ok) pthread_join(th, NULL);
    CHECK(dh_t->nheld == 40 && health(dh_t).bytes_dead_letters == (uint64_t)dh_t->held_cap * sizeof(tui_held));
    gptps_shutdown(e);
    gptps_tui_close(dh_t);
}
#endif


/* ---- 1.11: the KPI ladder and the journals ----------------------------------------- */

/* the same events at every tier: one item that finishes (5 ms), one that fails and is
 * dead-lettered, one still running, one still waiting */
static void ladder_events(gptps_tui *t)
{
    emit(t, GPTPS_EV_QUEUED, "lad", 1, GPTPS_OK, 10);
    emit_a(t, GPTPS_EV_STARTED, "lad", 1, GPTPS_OK, 11, 1);
    emit_a(t, GPTPS_EV_FINISHED, "lad", 1, GPTPS_OK, 15, 1);
    emit(t, GPTPS_EV_QUEUED, "lad", 2, GPTPS_OK, 20);
    emit_a(t, GPTPS_EV_STARTED, "lad", 2, GPTPS_OK, 21, 1);
    emit_a(t, GPTPS_EV_FAILED, "lad", 2, GPTPS_E_TASK, 22, 1);
    emit_a(t, GPTPS_EV_DEAD_LETTERED, "lad", 2, GPTPS_E_TASK, 22, 1);
    emit(t, GPTPS_EV_QUEUED, "lad", 3, GPTPS_OK, 30);
    emit_a(t, GPTPS_EV_STARTED, "lad", 3, GPTPS_OK, 31, 1);
    emit(t, GPTPS_EV_QUEUED, "lad", 4, GPTPS_OK, 40);
}

/* what tier `r` (TUI_T_*) must hold and have written after ladder_events, nothing more */
static int ladder_ok(gptps_tui *t, int r, int show_tasks, int show_recent)
{
    gptps_tui_health_info h = health(t);
    tui_task *k = row(t, "lad");
    int ok = 1;
#define LCHK(c) do { if (!(c)) { printf("tier %d: %s\n", r, #c); ok = 0; } } while (0)
    LCHK(t->tier == r && h.kpi == TUI_LADDER[r]);
    LCHK(t->q == (r >= TUI_T_COUNT ? 4u : 0u) && t->fin == (r >= TUI_T_COUNT ? 1u : 0u));
    LCHK(t->inflight == (r >= TUI_T_GAUGE ? 1u : 0u) && (r >= TUI_T_GAUGE || t->peak == 0));
    LCHK(h.gauges == (r < TUI_T_GAUGE ? GPTPS_TUI_GAUGES_SUM : r < TUI_T_LATENCY ? GPTPS_TUI_GAUGES_APPROX
                                                                                : GPTPS_TUI_GAUGES_EXACT));
    LCHK(r < TUI_T_COUNT || (r < TUI_T_GAUGE ? h.pending == 2 && h.in_flight == 0 : h.in_flight == 1));
    LCHK(r < TUI_T_LATENCY || (h.pending == 1 && h.in_flight == 1));
    /* PROBLEMS: the FAILED and the DEAD, nothing else */
    LCHK((t->prob != NULL) == (r >= TUI_T_PROBLEMS && show_recent) && (h.bytes_problems != 0) == (t->prob != NULL));
    LCHK(t->p_seq == (r >= TUI_T_PROBLEMS && show_recent ? 2u : 0u));
    /* TASKS */
    LCHK((t->tasks != NULL) == (r >= TUI_T_TASKS && show_tasks) && (h.bytes_tasks != 0) == (t->tasks != NULL));
    LCHK(!t->tasks || (k && k->finished == 1 && k->dead == 1 && k->started == 3));
    /* ACTIVITY: 4 QUEUED, 3 STARTED, 1 FINISHED */
    LCHK((t->act != NULL) == (r >= TUI_T_ACTIVITY && show_recent) && (h.bytes_activity != 0) == (t->act != NULL));
    LCHK(t->a_seq == (r >= TUI_T_ACTIVITY && show_recent ? 8u : 0u));
    LCHK(!t->act || t->act_nm == NULL || !t->tasks);    /* names copied only without rows */
    /* LATENCY */
    LCHK((t->tab != NULL) == (r >= TUI_T_LATENCY) && (h.bytes_handles != 0) == (t->tab != NULL));
    LCHK(!k || k->lat_n == (r >= TUI_T_LATENCY ? 1u : 0u));
    /* FULL */
    LCHK((t->hist != NULL) == (r >= TUI_T_FULL && show_tasks) && (h.bytes_histogram != 0) == (t->hist != NULL));
    LCHK(!t->hist || !k || t->hist[(size_t)(k - t->tasks) * TUI_HIST_B + 3] == 1);   /* 5 ms: [4, 8) */
#undef LCHK
    return ok;
}

/* 33) */
static void test_ladder_layers(void)
{
    int r, pass;
    for (r = TUI_T_OFF; r <= TUI_T_FULL; ++r) {          /* installed at each tier */
        gptps *e = open_manual(); gptps_tui *t;
        if (!e) return;
        t = install_cfg(e, TUI_LADDER[r], 64, 0);
        CHECK(t != NULL);
        if (!t) { gptps_shutdown(e); return; }
        ladder_events(t);
        CHECK(ladder_ok(t, r, 1, 1));
        gptps_shutdown(e);
        gptps_tui_close(t);
    }
    for (pass = 0; pass < 2; ++pass) {                   /* moved there at runtime, down then up */
        gptps *e = open_manual(); gptps_tui *t;
        if (!e) return;
        t = install_cfg(e, pass ? GPTPS_TUI_KPI_OFF : GPTPS_TUI_KPI_FULL, 64, 0);
        CHECK(t != NULL);
        if (!t) { gptps_shutdown(e); return; }
        for (r = pass ? TUI_T_OFF : TUI_T_FULL; pass ? r <= TUI_T_FULL : r >= TUI_T_OFF; r += pass ? 1 : -1) {
            CHECK(gptps_tui_set_kpi(t, (gptps_tui_kpi)TUI_LADDER[r]) == GPTPS_OK);
            /* a fresh start for the counts this check reads: the dashboard's own, reset */
            mu_lock(&t->mu);
            t->q = t->s = t->fin = t->fail = t->retr = t->dead = t->drop = t->term = t->cancel = 0;
            t->inflight = t->peak = 0;
            t->p_seq = t->p_seen = t->p_base = 0; t->a_seq = t->a_seen = t->a_base = 0;
            t->phead = t->ahead = 0;
            mu_unlock(&t->mu);
            if (t->tasks || t->tab) {                    /* rows and table start again: a new tier's state */
                gptps_tui_set_kpi(t, GPTPS_TUI_KPI_GAUGE);
                gptps_tui_set_kpi(t, (gptps_tui_kpi)TUI_LADDER[r]);
                mu_lock(&t->mu); t->inflight = 0; mu_unlock(&t->mu);
            }
            ladder_events(t);
            CHECK(ladder_ok(t, r, 1, 1));
            emit_a(t, GPTPS_EV_FINISHED, "lad", 3, GPTPS_OK, 50, 1);   /* end what this round left alive */
            emit_a(t, GPTPS_EV_STARTED, "lad", 4, GPTPS_OK, 51, 1);
            emit_a(t, GPTPS_EV_FINISHED, "lad", 4, GPTPS_OK, 52, 1);
        }
        gptps_shutdown(e);
        gptps_tui_close(t);
    }
    for (pass = 0; pass < 3; ++pass) {                   /* a 1.10 caller's struct hiding a pane: not collected */
        union { gptps_tui_config c; unsigned char b[sizeof(gptps_tui_config) + 64]; } u;
        gptps *e = open_manual(); gptps_tui *t;
        static char frame[16384];
        if (!e) return;
        memset(&u, 0, sizeof u);
        u.c.struct_size = TUI_V110_SIZE; u.c.max_recent = 8;
        u.c.show_tasks = pass != 1 ? -1 : 0; u.c.show_recent = pass != 0 ? -1 : 0;
        t = gptps_tui_install(e, &u.c);
        CHECK(t != NULL);
        if (!t) { gptps_shutdown(e); return; }
        ladder_events(t);
        CHECK(ladder_ok(t, TUI_T_FULL, pass == 1, pass == 0));
        gptps_tui_render(t, frame, sizeof frame);
        CHECK((strstr(frame, "TASKS") != NULL) == (pass == 1) && (strstr(frame, "RECENT") != NULL) == (pass == 0));
        CHECK(strstr(frame, "no memory") == NULL);       /* hidden is not missing */
        gptps_shutdown(e);
        gptps_tui_close(t);
    }
}

/* 34) */
static void test_ladder_names(void)
{
    static const struct { const char *set, *reads; int kpi; } N[] = {
        { "off", "off", GPTPS_TUI_KPI_OFF }, { "minimal", "minimal", GPTPS_TUI_KPI_MINIMAL },
        { "normal", "normal", GPTPS_TUI_KPI_NORMAL }, { "full", "full", GPTPS_TUI_KPI_FULL },
        { "gauge", "gauge", GPTPS_TUI_KPI_GAUGE }, { "problems", "problems", GPTPS_TUI_KPI_PROBLEMS },
        { "tasks", "tasks", GPTPS_TUI_KPI_TASKS }, { "activity", "normal", GPTPS_TUI_KPI_NORMAL },
        { "latency", "latency", GPTPS_TUI_KPI_LATENCY }, { "count", "minimal", GPTPS_TUI_KPI_MINIMAL },
    };
    static const int up[] = { GPTPS_TUI_KPI_MINIMAL, GPTPS_TUI_KPI_GAUGE, GPTPS_TUI_KPI_PROBLEMS, GPTPS_TUI_KPI_TASKS,
                              GPTPS_TUI_KPI_NORMAL, GPTPS_TUI_KPI_LATENCY, GPTPS_TUI_KPI_FULL, GPTPS_TUI_KPI_MINIMAL,
                              GPTPS_TUI_KPI_GAUGE };
    gptps *e = open_manual(); gptps_tui *t;
    char b[GPTPS_SETTINGS_VALUE_MAX], want[32];
    static char frame[16384];
    size_t i;
    if (!e) return;
    t = install_cfg(e, GPTPS_TUI_KPI_OFF, 64, 0);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    for (i = 0; i < sizeof N / sizeof N[0]; ++i) {
        CHECK(gptps_settings_set(e, "tui.kpi", N[i].set) == GPTPS_OK);
        CHECK(gptps_settings_get(e, "tui.kpi", b, sizeof b) == GPTPS_OK && strcmp(b, N[i].reads) == 0);
        CHECK(t->kpi == N[i].kpi);
        gptps_tui_render(t, frame, sizeof frame);
        snprintf(want, sizeof want, "kpi:%s mode:", N[i].reads);
        CHECK(strstr(frame, want) != NULL);
    }
    CHECK(gptps_settings_set(e, "tui.kpi", "custom") == GPTPS_E_CONFIG);   /* no such tier */
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_ACTIVITY) == GPTPS_OK && t->kpi == GPTPS_TUI_KPI_NORMAL);
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_DEFAULT) == GPTPS_OK && t->kpi == GPTPS_TUI_KPI_FULL);
    CHECK(gptps_tui_set_kpi(t, (gptps_tui_kpi)10) == GPTPS_E_INVAL && t->kpi == GPTPS_TUI_KPI_FULL);
    CHECK(gptps_tui_set_kpi(t, (gptps_tui_kpi)-1) == GPTPS_E_INVAL);
    /* 'm': from off to minimal, up one tier a press, from full back to minimal */
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_OFF) == GPTPS_OK);
    for (i = 0; i < sizeof up / sizeof up[0]; ++i) {
        CHECK(gptps_tui_press(t, 'm') == 3);
        CHECK(t->kpi == up[i]);
        CHECK(t->kpi != GPTPS_TUI_KPI_OFF);
    }
    for (i = 0; i < 40; ++i) { gptps_tui_press(t, 'm'); CHECK(t->kpi != GPTPS_TUI_KPI_OFF); }
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* how many times needle occurs in s */
static int occurs(const char *s, const char *needle)
{
    int c = 0;
    while ((s = strstr(s, needle)) != NULL) { ++c; s += strlen(needle); }
    return c;
}

/* 35) */
static void test_journals(void)
{
    gptps *e = open_manual(); gptps_tui *t; gptps_tui_config c;
    static char frame[32768];
    gptps_tui_health_info h;
    char b[GPTPS_SETTINGS_VALUE_MAX];
    int i;
    if (!e) return;
    memset(&c, 0, sizeof c);
    c.struct_size = sizeof c; c.color = 0; c.interactive = 0; c.kpi = GPTPS_TUI_KPI_NORMAL;
    c.recent_lines = 4; c.activity_lines = 4;
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    t->rows = 60;
    for (i = 0; i < 3; ++i) emit_a(t, GPTPS_EV_FAILED, "jr", (gptps_handle)(1 + i), GPTPS_E_TASK, (uint64_t)(1 + i), 1);
    gptps_tui_render(t, frame, sizeof frame);            /* seen: nothing lost */
    CHECK(occurs(frame, "FAILED") == 3 && strstr(frame, "not shown") == NULL);
    for (i = 0; i < 1000; ++i) emit(t, GPTPS_EV_QUEUED, "jr", (gptps_handle)(100 + i), GPTPS_OK, (uint64_t)(10 + i));
    h = health(t);
    CHECK(h.journal_not_shown == 996 && h.problems_not_shown == 0);   /* before a frame: exact already */
    CHECK(h.problems_held == 3 && h.activity_held == 4);   /* the failures are still there */
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "RECENT  996 not shown") != NULL);
    CHECK(occurs(frame, "QUEUED") == 4 && occurs(frame, "FAILED") == 0);   /* newest 4 lines: activity */
    CHECK(strstr(frame, "lost:") == NULL);               /* an activity loss is not a lost problem */
    for (i = 0; i < 4; ++i) CHECK(gptps_tui_press(t, 'k') == 2);
    gptps_tui_render(t, frame, sizeof frame);            /* scrolled back: the failures, kept */
    CHECK(occurs(frame, "FAILED") == 3 && strstr(frame, "not shown") == NULL);
    CHECK(health(t).journal_not_shown == 996);
    /* problems overflow: 10 more before a frame, 4 kept, 6 lost - counted, on the lost: line,
     * and logged at WARN once (the first frame that finds one), not on every frame after */
    log_begin();
    for (i = 0; i < 10; ++i) emit_a(t, GPTPS_EV_DROPPED, "jr", (gptps_handle)(2000 + i), GPTPS_E_TASK, 5000, 1);
    CHECK(health(t).problems_not_shown == 6 && health(t).journal_not_shown == 1002);
    CHECK(log_count("problem events") == 0);             /* the event path logs nothing */
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "lost: 6 problem events not shown") != NULL);
    CHECK(health(t).problems_not_shown == 6);
    CHECK(log_count("6 problem events") == 1 && log_count("tui.recent") == 1);
    for (i = 0; i < 10; ++i) emit_a(t, GPTPS_EV_DROPPED, "jr", (gptps_handle)(2100 + i), GPTPS_E_TASK, 5001, 1);
    gptps_tui_render(t, frame, sizeof frame);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "lost: 12 problem events not shown") != NULL);
    CHECK(log_count("problem events") == 1);             /* once: no flood */
    log_end();
    /* ACTIVITY: the unseen records a resize, a 0 or a tier drop takes out count too - in
     * health and on the next frame's RECENT line */
    {
        uint64_t a0;
        h = health(t); a0 = h.journal_not_shown - h.problems_not_shown;
        for (i = 0; i < 3; ++i) emit(t, GPTPS_EV_QUEUED, "jr", (gptps_handle)(2500 + i), GPTPS_OK, 5500);
        h = health(t); CHECK(h.journal_not_shown - h.problems_not_shown == a0);   /* held, not lost */
        CHECK(gptps_tui_set_recent(t, -1, 6) == GPTPS_OK && t->acap == 6);
        h = health(t); CHECK(h.journal_not_shown - h.problems_not_shown == a0 + 3);
        gptps_tui_render(t, frame, sizeof frame);
        CHECK(strstr(frame, "RECENT  3 not shown") != NULL);
        gptps_tui_render(t, frame, sizeof frame);       /* shown once: the next frame lost nothing */
        CHECK(strstr(frame, "RECENT\n") != NULL);
        for (i = 0; i < 2; ++i) emit(t, GPTPS_EV_QUEUED, "jr", (gptps_handle)(2600 + i), GPTPS_OK, 5600);
        CHECK(gptps_tui_set_recent(t, -1, 0) == GPTPS_OK && t->act == NULL);
        h = health(t); CHECK(h.journal_not_shown - h.problems_not_shown == a0 + 5);
        gptps_tui_render(t, frame, sizeof frame);
        CHECK(strstr(frame, "RECENT  2 not shown  problems only") != NULL);
        CHECK(gptps_tui_set_recent(t, -1, 4) == GPTPS_OK && t->acap == 4);
        for (i = 0; i < 2; ++i) emit(t, GPTPS_EV_STARTED, "jr", (gptps_handle)(2700 + i), GPTPS_OK, 5700);
        CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_TASKS) == GPTPS_OK && t->act == NULL);
        h = health(t); CHECK(h.journal_not_shown - h.problems_not_shown == a0 + 7);
        CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_NORMAL) == GPTPS_OK && t->acap == 4);
        gptps_tui_render(t, frame, sizeof frame);
        CHECK(strstr(frame, "RECENT  2 not shown") != NULL);
        CHECK(health(t).problems_not_shown == 12);       /* none of that was a problem */
    }
    /* resized: what no frame showed counts; the new one starts empty */
    emit_a(t, GPTPS_EV_DROPPED, "jr", 3000, GPTPS_E_TASK, 6000, 1);
    emit_a(t, GPTPS_EV_DROPPED, "jr", 3001, GPTPS_E_TASK, 6000, 1);
    CHECK(gptps_tui_set_recent(t, 5, -1) == GPTPS_OK && t->pcap == 5 && t->acap == 4);
    h = health(t);
    CHECK(h.problems_not_shown == 14 && h.problems_held == 0 && h.bytes_problems == 5 * sizeof(tui_event));
    /* 0 lines: off and freed */
    CHECK(gptps_tui_set_recent(t, 0, 0) == GPTPS_OK && t->prob == NULL && t->act == NULL);
    h = health(t);
    CHECK(h.bytes_problems == 0 && h.bytes_activity == 0);
    emit_a(t, GPTPS_EV_FAILED, "jr", 4000, GPTPS_E_TASK, 7000, 1);   /* written nowhere */
    CHECK(t->p_seq == 25 && health(t).problems_not_shown == 14);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "RECENT") == NULL);
    /* the settings, with ranges; tui.activity 0 leaves problems only */
    CHECK(gptps_settings_set(e, "tui.recent", "4097") == GPTPS_E_CONFIG);
    CHECK(gptps_settings_set(e, "tui.recent", "3") == GPTPS_OK && t->pcap == 3 && t->prob != NULL);
    CHECK(gptps_settings_set(e, "tui.activity", "5000") == GPTPS_E_CONFIG && t->act == NULL);
    CHECK(gptps_settings_get(e, "tui.activity", b, sizeof b) == GPTPS_OK && strcmp(b, "0") == 0);
    emit_a(t, GPTPS_EV_FAILED, "jr", 4001, GPTPS_E_TASK, 7001, 1);
    emit(t, GPTPS_EV_QUEUED, "jr", 4002, GPTPS_OK, 7002);
    gptps_tui_render(t, frame, sizeof frame);
    /* the 2 unseen DROPPED the resize to 5 took out, on the first frame that has a RECENT line */
    CHECK(strstr(frame, "RECENT  2 not shown  problems only") != NULL && occurs(frame, "FAILED") == 1 &&
          !strstr(frame, "QUEUED"));
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "RECENT  problems only") != NULL);
    CHECK(gptps_settings_set(e, "tui.activity", "2") == GPTPS_OK && t->acap == 2);
    CHECK(gptps_settings_get(e, "tui.recent", b, sizeof b) == GPTPS_OK && strcmp(b, "3") == 0);
    /* below its tier a journal holds nothing, whatever its size */
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_TASKS) == GPTPS_OK && t->act == NULL && t->prob != NULL);
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_GAUGE) == GPTPS_OK && t->prob == NULL);
    CHECK(gptps_settings_get(e, "tui.recent", b, sizeof b) == GPTPS_OK && strcmp(b, "3") == 0);
    CHECK(gptps_tui_set_recent(t, -2, 1) == GPTPS_E_INVAL && gptps_tui_set_recent(t, 1, 4097) == GPTPS_E_INVAL);
    CHECK(gptps_tui_set_recent(NULL, 1, 1) == GPTPS_E_INVAL);
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 36) */
static void test_save_reload(void)
{
    static const struct { const char *kpi, *reads, *recent, *activity; } W[] = {
        { "gauge", "gauge", "5", "0" }, { "problems", "problems", "0", "7" }, { "tasks", "tasks", "4096", "1" },
        { "activity", "normal", "9", "9" }, { "latency", "latency", "1", "4096" }, { "count", "minimal", "8", "8" },
    };
    const char *path = "tui_save_reload.toml";
    size_t i;
    for (i = 0; i < sizeof W / sizeof W[0]; ++i) {
        FILE *f = fopen(path, "w");
        gptps *e = NULL; gptps_tui *t; gptps_tui_config c;
        char b[GPTPS_SETTINGS_VALUE_MAX];
        if (!f) { printf("note: cannot write %s; 36) skipped\n", path); return; }
        fputs("# saved by the dashboard\n[tui]\nkpi = \"full\"\n", f);
        fclose(f);
        CHECK(gptps_open(path, &e) == GPTPS_OK);
        if (!e) break;
        memset(&c, 0, sizeof c);
        c.struct_size = sizeof c; c.color = 0; c.interactive = 0;
        t = gptps_tui_install(e, &c);
        CHECK(t != NULL);
        CHECK(gptps_settings_set(e, "tui.kpi", W[i].kpi) == GPTPS_OK);
        CHECK(gptps_settings_set(e, "tui.recent", W[i].recent) == GPTPS_OK);
        CHECK(gptps_settings_set(e, "tui.activity", W[i].activity) == GPTPS_OK);
        if (t) {
            CHECK(gptps_tui_press(t, 's') == 4 && gptps_tui_press(t, 'w') == 4);   /* the pane's own save */
            CHECK(strstr(t->status, "saved your changes") != NULL);
        }
        gptps_shutdown(e);
        gptps_tui_close(t);
        e = NULL;
        CHECK(gptps_open(path, &e) == GPTPS_OK);           /* and back */
        if (!e) break;
        t = gptps_tui_install(e, &c);
        CHECK(t != NULL);
        CHECK(gptps_config_check(e) == GPTPS_OK);           /* 0 config errors */
        CHECK(gptps_settings_get(e, "tui.kpi", b, sizeof b) == GPTPS_OK && strcmp(b, W[i].reads) == 0);
        CHECK(gptps_settings_get(e, "tui.recent", b, sizeof b) == GPTPS_OK && strcmp(b, W[i].recent) == 0);
        CHECK(gptps_settings_get(e, "tui.activity", b, sizeof b) == GPTPS_OK && strcmp(b, W[i].activity) == 0);
        gptps_shutdown(e);
        gptps_tui_close(t);
    }
    remove(path);
}

/* 37) */
static void test_percentiles(void)
{
    gptps *e = open_manual(); gptps_tui *t; char pq[16];
    static char frame[16384];
    static const uint64_t lat[] = { 0, 5, 100, 5000 };
    static const int times[] = { 10, 80, 8, 2 };
    uint64_t hb[TUI_HIST_B];
    gptps_handle h = 1;
    int i, j;
    CHECK(hist_bucket(0) == 0 && hist_bucket(1) == 1 && hist_bucket(2) == 2 && hist_bucket(3) == 2);
    CHECK(hist_bucket(4) == 3 && hist_bucket(((uint64_t)1 << 30) - 1) == 30 && hist_bucket((uint64_t)1 << 30) == 31);
    CHECK(hist_bucket(UINT64_MAX) == 31);
    memset(hb, 0, sizeof hb);
    CHECK(strcmp(hist_pct(hb, 50, pq, sizeof pq), "--") == 0);
    hb[31] = 1;
    CHECK(strcmp(hist_pct(hb, 50, pq, sizeof pq), ">=1.1G") == 0);
    if (!e) return;
    t = install_cfg(e, GPTPS_TUI_KPI_FULL, 256, 0);
    CHECK(t != NULL && t->hist != NULL);
    if (!t || !t->hist) { gptps_shutdown(e); gptps_tui_close(t); return; }
    for (i = 0; i < 4; ++i)
        for (j = 0; j < times[i]; ++j, ++h) {
            emit(t, GPTPS_EV_QUEUED, "pc", h, GPTPS_OK, 1000);
            emit_a(t, GPTPS_EV_STARTED, "pc", h, GPTPS_OK, 1000, 1);
            emit_a(t, GPTPS_EV_FINISHED, "pc", h, GPTPS_OK, 1000 + lat[i], 1);
        }
    t->cols = 120;
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "    p50    p95    p99") != NULL);
    CHECK(strstr(frame, "     <8   <128  <8192") != NULL);   /* 0,5 -> <8; 95th: 100 -> <128; 99th: 5000 -> <8192 */
    CHECK(widest(frame) <= 120);
    CHECK(strstr(frame, "   max ms") != NULL && strstr(frame, "     5000") != NULL);   /* the slowest: 5000 ms */
    t->cols = 100;                                       /* room for max ms, not the percentiles too */
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "max ms") != NULL && strstr(frame, "p50") == NULL && widest(frame) <= 100);
    t->cols = 80;                                        /* no room: not shown, the table stays in the width */
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "p50") == NULL && strstr(frame, "max ms") == NULL && widest(frame) <= 80);
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_LATENCY) == GPTPS_OK && t->hist == NULL);
    t->cols = 120;
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "p50") == NULL && strstr(frame, "avg ms") != NULL && strstr(frame, "max ms") != NULL);
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_TASKS) == GPTPS_OK && t->tab == NULL);   /* below LATENCY: none */
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "max ms") == NULL && strstr(frame, "avg ms") != NULL);
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 38) */
static void test_recent_merged(void)
{
    int pass;
    for (pass = 0; pass < 2; ++pass) {
        gptps *e = open_manual(); gptps_tui *t; gptps_tui_config c;
        static char frame[16384];
        const char *q, *f, *s1, *d;
        if (!e) return;
        memset(&c, 0, sizeof c);
        c.struct_size = sizeof c; c.color = 0; c.interactive = 0; c.kpi = GPTPS_TUI_KPI_NORMAL;
        c.activity_lines = pass ? -1 : 0;                 /* 0: as recent_lines; -1: off */
        t = gptps_tui_install(e, &c);
        CHECK(t != NULL);
        if (!t) { gptps_shutdown(e); return; }
        CHECK((t->act == NULL) == (pass == 1) && t->pcap == 8);
        t->start_ms = 0;
        emit(t, GPTPS_EV_QUEUED, "mg", 1, GPTPS_OK, 1000);
        emit_a(t, GPTPS_EV_FAILED, "mg", 2, GPTPS_E_TASK, 1500, 1);
        emit_a(t, GPTPS_EV_STARTED, "mg", 1, GPTPS_OK, 2000, 1);
        emit_a(t, GPTPS_EV_DROPPED, "mg", 3, GPTPS_E_TASK, 2000, 1);   /* the same ms as the STARTED */
        gptps_tui_render(t, frame, sizeof frame);
        q = strstr(frame, "QUEUED"); f = strstr(frame, "FAILED"); s1 = strstr(frame, "STARTED"); d = strstr(frame, "DROPPED");
        if (pass == 0) {
            CHECK(q && f && s1 && d && q < f && f < s1 && s1 < d);
            CHECK(strstr(frame, "problems only") == NULL);
        } else {
            CHECK(!q && !s1 && f && d && f < d && strstr(frame, "RECENT  problems only") != NULL);
        }
        gptps_shutdown(e);
        gptps_tui_close(t);
    }
}

/* 40) every line of a narrow frame is one row: MINIMAL's sum line and the totals line are
 * fitted to the width (the vertical fit counts rows by '\n'), at MINIMAL and at GAUGE */
static void test_narrow_lines(void)
{
    static const int K[] = { GPTPS_TUI_KPI_MINIMAL, GPTPS_TUI_KPI_GAUGE };
    gptps *e = open_manual(); gptps_tui *t; gptps_tui_config c;
    static char frame[16384];
    int i, k;
    if (!e) return;
    memset(&c, 0, sizeof c);
    c.struct_size = sizeof c; c.color = 0; c.unicode = 0; c.interactive = 0; c.kpi = GPTPS_TUI_KPI_MINIMAL;
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    t->rows = 40; t->cols = 40;
    for (i = 0; i < 1500; ++i) emit(t, GPTPS_EV_QUEUED, "nw", (gptps_handle)(1 + i), GPTPS_OK, 10);
    for (i = 0; i < 1200; ++i) emit(t, GPTPS_EV_STARTED, "nw", (gptps_handle)(1 + i), GPTPS_OK, 11);
    for (i = 0; i < 1100; ++i) emit_a(t, GPTPS_EV_FAILED, "nw", (gptps_handle)(1 + i), GPTPS_E_CANCELLED, 12, 1);
    for (k = 0; k < 2; ++k) {
        const char *p, *q;
        int found = 0;
        CHECK(gptps_tui_set_kpi(t, (gptps_tui_kpi)K[k]) == GPTPS_OK);
        gptps_tui_render(t, frame, sizeof frame);
        for (p = frame; *p; p = q + 1) {
            q = strchr(p, '\n');
            if (!q) break;
            if (strncmp(p, "queued ", 7) == 0 && k == 0) found |= 1;
            if (strncmp(p, "finished ", 9) == 0) found |= 2;
            if ((strncmp(p, "queued ", 7) == 0 && k == 0) || strncmp(p, "finished ", 9) == 0) {
                if (!(q - p <= 39)) fprintf(stderr, "  wide line (%d cols): %.*s\n", (int)(q - p), (int)(q - p), p);
                CHECK(q - p <= 39);
            }
        }
        CHECK(found == (k == 0 ? 3 : 2));
    }
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 39) */
static void test_config_111(void)
{
    static const struct { int field; int v; const char *msg; } B[] = {
        { 0, 4097, "cfg.recent_lines 4097 is out of range" }, { 0, -2, "cfg.recent_lines -2 is out of range" },
        { 1, 5000, "cfg.activity_lines 5000 is out of range" }, { 2, 4, "cfg.clock 4 is out of range" },
        { 3, 301, "cfg.rate_window_s 301 is out of range" }, { 4, 99, "cfg.pane_refresh_ms 99 is out of range" },
        { 4, 600001, "cfg.pane_refresh_ms 600001 is out of range" },
    };
    size_t i;
    gptps *e = open_manual(); gptps_tui *t; gptps_tui_config c;
    if (!e) return;
    log_begin();
    for (i = 0; i < sizeof B / sizeof B[0]; ++i) {
        memset(&c, 0, sizeof c); c.struct_size = sizeof c;
        if (B[i].field == 0) c.recent_lines = B[i].v;
        if (B[i].field == 1) c.activity_lines = B[i].v;
        if (B[i].field == 2) c.clock = B[i].v;
        if (B[i].field == 3) c.rate_window_s = B[i].v;
        if (B[i].field == 4) c.pane_refresh_ms = B[i].v;
        CHECK(gptps_tui_install(e, &c) == NULL);
        CHECK(log_count(B[i].msg) == 1);
    }
    memset(&c, 0, sizeof c); c.struct_size = sizeof c; c.max_recent = 6;   /* the defaults */
    t = gptps_tui_install(e, &c);
    CHECK(t != NULL);
    if (t) {
        CHECK(t->pcap == 6 && t->acap == 6 && t->cfg.recent_lines == 6 && t->cfg.activity_lines == 6);
        CHECK(t->cfg.clock == GPTPS_TUI_CLOCK_MINUTES && t->cfg.rate_window_s == 10 && t->cfg.pane_refresh_ms == 2000);
    }
    log_end();
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* ---- 1.11, the frame and the live loop -------------------------------------------- */

/* lines in a frame (its newlines) */
static int frame_rows(const char *f) { int n = 0; for (; *f; ++f) n += *f == '\n'; return n; }

/* 41) the line diff: an identical frame is 0 bytes; one changed line is rewritten in place
 * (and nothing else); a shorter frame wipes what is below it; more than half changed, or
 * no old frame, is a full repaint from home */
static void test_frame_diff(void)
{
    static const char a[] = "one\ntwo\nthree\nfour\nfive\n", b[] = "one\nTWO\nthree\nfour\nfive\n",
                      c[] = "1\n2\n3\n4\n5\n6\n", c0[] = "1\n2\n3\n4\n5\n6\n7\n8\n", d[] = "1\n2\n3\n4\nfive\n";
    char o[512];
    size_t n;
    CHECK(frame_diff(a, a, 0, o) == 0);
    n = frame_diff(b, a, 0, o); o[n] = 0;
    CHECK(strcmp(o, "\x1b[2;1HTWO\x1b[K") == 0);
    n = frame_diff(c, c0, 0, o); o[n] = 0;                 /* rows 7..8 go; row 7 is now the empty last line */
    CHECK(strcmp(o, "\x1b[7;1H\x1b[K\x1b[8;1H\x1b[J") == 0);
    n = frame_diff(d, a, 0, o); o[n] = 0;                  /* 4 of 6 lines: all of it */
    CHECK(strcmp(o, "\x1b[H1\x1b[K\n2\x1b[K\n3\x1b[K\n4\x1b[K\nfive\x1b[K\n\x1b[J") == 0);
    n = frame_diff(b, NULL, 0, o); o[n] = 0;
    CHECK(strncmp(o, "\x1b[Hone\x1b[K\nTWO", 13) == 0);
    n = frame_diff(a, a, 1, o);                            /* asked for: a full one, identical or not */
    CHECK(n == strlen(a) + 5 * 3 + 6);
    CHECK(frame_out_cap(strlen(a), frame_nlines(a)) >= n);
}

/* 42) the header: the clock (off / minutes / seconds; minutes by default), done/s over a
 * window (not since install) with "-" before there is anything to measure and "n/a" at
 * OFF, peak over the window (not for ever), GPTPS once in a title that starts with it */
static void test_header(void)
{
    gptps *e = open_manual(); gptps_tui *t; static char frame[16384]; char b[64]; int st;
    uint64_t nx, now;
    double r;
    if (!e) return;
    t = install(e);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    fmt_up(b, sizeof b, 59999, GPTPS_TUI_CLOCK_MINUTES);  CHECK(strcmp(b, "up <1m") == 0);
    fmt_up(b, sizeof b, 125000, GPTPS_TUI_CLOCK_MINUTES); CHECK(strcmp(b, "up 2m") == 0);
    fmt_up(b, sizeof b, 3725000, GPTPS_TUI_CLOCK_MINUTES); CHECK(strcmp(b, "up 1h02m") == 0);
    fmt_up(b, sizeof b, 90000000, GPTPS_TUI_CLOCK_MINUTES); CHECK(strcmp(b, "up 1d01h") == 0);
    fmt_up(b, sizeof b, 9000, GPTPS_TUI_CLOCK_SECONDS);   CHECK(strcmp(b, "up 9s") == 0);
    fmt_up(b, sizeof b, 125000, GPTPS_TUI_CLOCK_SECONDS); CHECK(strcmp(b, "up 2m05s") == 0);
    CHECK(clock_next(59000, GPTPS_TUI_CLOCK_MINUTES) == 60000 && clock_next(60000, GPTPS_TUI_CLOCK_MINUTES) == 120000);
    CHECK(clock_next(1500, GPTPS_TUI_CLOCK_SECONDS) == 2000 && clock_next(1500, GPTPS_TUI_CLOCK_OFF) == 0);
    CHECK(t->clock == GPTPS_TUI_CLOCK_MINUTES);            /* the default, for a 1.10-layout caller too */
    now = gptps_now_ms(NULL);
    t->start_ms = now - 125000;
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "GPTPS \xc2\xb7 tasks   up 2m   ") != NULL && strstr(frame, " done/s") != NULL);
    CHECK(gptps_tui_set_clock(t, GPTPS_TUI_CLOCK_SECONDS) == GPTPS_OK);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "up 2m0") != NULL);
    CHECK(gptps_settings_set(e, "tui.clock", "off") == GPTPS_OK && t->clock == GPTPS_TUI_CLOCK_OFF);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "up ") == NULL);
    CHECK(gptps_settings_set(e, "tui.clock", "fortnights") == GPTPS_E_CONFIG);
    CHECK(gptps_settings_get(e, "tui.clock", b, sizeof b) == GPTPS_OK && strcmp(b, "off") == 0);
    CHECK(gptps_tui_set_clock(t, (gptps_tui_clock)9) == GPTPS_E_INVAL);
    CHECK(gptps_tui_set_clock(t, GPTPS_TUI_CLOCK_DEFAULT) == GPTPS_OK && t->clock == GPTPS_TUI_CLOCK_MINUTES);
    /* done/s over the last 10 s: 50 finished in the second after the first snapshot... */
    mu_lock(&t->mu);
    t->rate_n = 0; t->rate_head = 0; t->fin = 0;
    r = rate_take(t, 1000, &st, &nx);
    CHECK(st == 0 && nx == 1001);                          /* nothing to measure against: "-", until the next ms */
    t->fin = 50;
    r = rate_take(t, 2000, &st, &nx);
    CHECK(st == 1 && r > 49.9 && r < 50.1 && nx == 3000);  /* and it settles with no event: a deadline */
    r = rate_take(t, 6000, &st, &nx);
    CHECK(st == 1 && r > 9.9 && r < 10.1);                 /* the same 50, over 5 s */
    r = rate_take(t, 12500, &st, &nx);                     /* ...and 10.5 s later they have left the window */
    CHECK(st == 1 && r == 0.0 && nx == 0);                 /* 0, and no more deadlines (since install: 4.8) */
    /* peak: the highest in-flight over the last one or two windows */
    t->peak = 7; t->peak_prev = 0; t->peak_ms = 0;
    CHECK(peak_take(t, 5000, 2, &nx) == 7 && nx == 10000);
    CHECK(peak_take(t, 10000, 2, &nx) == 7 && nx == 20000);   /* the last window's */
    CHECK(peak_take(t, 20000, 2, &nx) == 2 && nx == 0);       /* gone: it was 1.10's for ever */
    /* a frame whose time was read before another's (two threads render): no snapshot out
     * of order, no early rotation - the unsigned differences must not wrap */
    {
        int n0 = t->rate_n;
        r = rate_take(t, 11000, &st, &nx);
        CHECK(t->rate_n == n0 && t->rate[(t->rate_head + TUI_RATE_N - 1) % TUI_RATE_N].ms == 12500);
        CHECK(st == 1 && r == 0.0);
        t->peak = 5;
        CHECK(peak_take(t, 15000, 2, &nx) == 5 && t->peak_ms == 20000 && t->peak == 5);
    }
    mu_unlock(&t->mu);
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_OFF) == GPTPS_OK);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "n/a done/s") != NULL);             /* the totals stand still at OFF */
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_MINIMAL) == GPTPS_OK);
    gptps_tui_render(t, frame, sizeof frame);               /* back: measured from now, "-" or a rate */
    CHECK(strstr(frame, "n/a done/s") == NULL);
    gptps_shutdown(e);
    gptps_tui_close(t);
    {   /* a title that starts with GPTPS gets no second one: plain, colour, and the panes */
        gptps_tui_config c;
        e = open_manual();
        if (!e) return;
        memset(&c, 0, sizeof c);
        c.struct_size = sizeof c; c.title = "GPTPS live demo";
        t = gptps_tui_install(e, &c);
        CHECK(t != NULL);
        if (t) {
            gptps_tui_render(t, frame, sizeof frame);
            CHECK(strncmp(frame, "GPTPS live demo   up <1m", 24) == 0);
            t->cfg.color = 1;
            gptps_tui_render(t, frame, sizeof frame);
            CHECK(strstr(frame, " GPTPS live demo ") != NULL && strstr(frame, "GPTPS  GPTPS") == NULL);
            t->cfg.color = 0;
            t->pane = TUI_PANE_HELP;
            gptps_tui_render(t, frame, sizeof frame);
            CHECK(strncmp(frame, "GPTPS live demo \xc2\xb7 help", 22) == 0);
            t->pane = TUI_PANE_DASH;
        }
        gptps_shutdown(e);
        gptps_tui_close(t);
    }
}

/* 43) run's and snapshot's frame grows to what it needs, to 256 KiB (16 KiB cut a tall
 * colour frame); past that the cut is visible and counted */
static void test_frame_grows(void)
{
    static char title[300000];
    gptps *e = open_manual(); gptps_tui *t; gptps_tui_config c; FILE *out;
    long n;
    char tail[64];
    if (!e) return;
    out = tmpfile();
    memset(&c, 0, sizeof c);
    c.struct_size = sizeof c; c.out = out; c.title = title;
    memset(title, 'x', 40000); title[40000] = 0;         /* a frame of ~40 KiB */
    t = out ? gptps_tui_install(e, &c) : NULL;
    CHECK(t != NULL);
    if (t) {
        gptps_tui_snapshot(t);
        n = ftell(out);
        CHECK(n > 40000 && health(t).frames_truncated == 0);
        fseek(out, n - 40, SEEK_SET);
        CHECK(fread(tail, 1, 40, out) == 40);
        tail[40] = 0;
        CHECK(strstr(tail, "q quit") != NULL);           /* all of it: the legend, at the end */
        memset(title, 'y', sizeof title - 1); title[sizeof title - 1] = 0;   /* past the cap */
        rewind(out);
        gptps_tui_snapshot(t);
        n = ftell(out);
        CHECK(n <= (long)TUI_FRAME_MAX + 2 && n > (long)TUI_FRAME_MAX / 2);
        CHECK(health(t).frames_truncated == 1);
        gptps_tui_render(t, title, 64);                  /* a caller's buffer is never grown */
        CHECK(health(t).frames_truncated == 2);
    }
    gptps_shutdown(e);
    gptps_tui_close(t);
    if (out) fclose(out);
}

/* 44) the TASKS table and the tasks pane fit the terminal's height, with "+N more" */
static void test_height_fit(void)
{
    gptps *e = open_manual(); gptps_tui *t; static char frame[65536]; int i;
    if (!e) return;
    t = install(e);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    for (i = 0; i < TUI_MAX_TASKS; ++i) {
        char nm[32];
        snprintf(nm, sizeof nm, "fit_%02d", i);
        emit(t, GPTPS_EV_QUEUED, nm, (gptps_handle)(i + 1), GPTPS_OK, 1);
        deftask(e, nm, ok_task);
    }
    t->cols = 80; t->rows = 24;
    gptps_tui_render(t, frame, sizeof frame);
    if (frame_rows(frame) > 24) printf("%s\n", frame);
    CHECK(frame_rows(frame) <= 23);                        /* and the last newline does not scroll */
    CHECK(strstr(frame, " more (a taller terminal shows them)") != NULL);
    CHECK(strstr(frame, "fit_00") != NULL && strstr(frame, "RECENT") != NULL);
    t->rows = 200;
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "fit_63") != NULL && strstr(frame, " more (") == NULL);   /* room: all of them */
    t->rows = 24;
    t->pane = TUI_PANE_TASKS;
    t->task_sel = 40;
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(frame_rows(frame) <= 23);
    {
        gptps_task_info a, b;
        char sel[GPTPS_TASK_NAME_MAX + 8];
        CHECK(tui_task_at(e, 0, &a) && tui_task_at(e, 40, &b));
        snprintf(sel, sizeof sel, "> %s ", b.name_copy);
        CHECK(strstr(frame, sel) != NULL && strstr(frame, a.name_copy) == NULL);   /* around the selection */
    }
    CHECK(strstr(frame, " more (") != NULL);
    t->pane = TUI_PANE_DASH;
    drain(e);
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 45) the legend is cached, and rebuilt when a binding or the width changes */
static void test_legend_cache(void)
{
    gptps *e = open_manual(); gptps_tui *t; static char frame[16384];
    uint64_t g, b;
    if (!e) return;
    t = install(e);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "[w] Wash") == NULL && t->view != NULL);
    g = t->view->leg_gen; b = t->view->leg_builds;
    CHECK(b == 1);
    gptps_tui_render(t, frame, sizeof frame);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(t->view->leg_gen == g && t->view->leg_builds == b);   /* nothing changed: not rebuilt */
    CHECK(gptps_tui_add_task(t, "wash", "Wash", 'w', NULL, 0) == GPTPS_OK);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "[w] Wash") != NULL && t->view->leg_builds == b + 1);   /* a new binding: rebuilt */
    t->cols = 200;
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(t->view->legrows == 1 && strstr(frame, "[w] Wash  \xc2\xb7  Up/Dn scroll  s settings") != NULL);
    t->cols = 40;
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(t->view->legrows > 2 && widest(t->view->legend) <= 40);   /* a new width: rewrapped */
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* The index of `key` in the pane cache, -1 when it is not there. */
static int pc_find(const gptps_tui *t, const char *key)
{
    size_t i;
    for (i = 0; i < t->pc.n; ++i) if (strcmp(t->pc.pool + t->pc.sets[i].key, key) == 0) return (int)i;
    return -1;
}

/* 48) The engine-reading panes (tasks, settings, detail) read the engine in one pass into
 * a cache - on open, after a change made from the pane, on 'R', and when due - and their
 * frames and j/k read only the cache: no engine call between passes. A pass is timed, and
 * a slow one stretches the interval to 100x its cost (the engine's lock <= 1% of the time).
 * trk_resync is held back the same way after a slow look. An edit reaches the key the
 * operator chose even when the registry moved since the pass. */
static void test_pane_cache(void)
{
    gptps *e = open_manual(); gptps_tui *t; static char frame[32768];
    gptps_tui_health_info h;
    int i, k, r0, idx, ntask = 0;
    size_t ns;
    gptps_handle hd;
    char key0[GPTPS_SETTINGS_VALUE_MAX];
    if (!e) return;
    for (i = 0; i < 8; ++i) { char nm[16]; snprintf(nm, sizeof nm, "pc_%d", i); deftask(e, nm, ok_task); }
    t = install(e);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    /* tasks pane: one pass on open (a count, then one call per type), shown with its cost */
    t->pane = TUI_PANE_TASKS;
    r0 = reads();
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(reads() - r0 == 1 + 8);
    h = health(t);
    CHECK(h.pane_reads == 1 && h.bytes_panes > 0 && h.pane_every_ms == 2000);
    CHECK(strstr(frame, "core counts: ") != NULL && strstr(frame, "every 2.0 s, age 0 s") != NULL);
    CHECK(strstr(frame, "pc_7") != NULL);
    /* between passes, frames and keys cost the engine nothing */
    r0 = reads();
    for (k = 0; k < 20; ++k) gptps_tui_render(t, frame, sizeof frame);
    gptps_tui_press(t, 'j'); gptps_tui_press(t, 'k');
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(reads() == r0);
    /* cached: work queued now shows at the next pass - 'R' makes it now */
    CHECK(gptps_submit(e, "pc_0", NULL, 0, &hd) == GPTPS_OK);
    gptps_tui_render(t, frame, sizeof frame);
    for (i = 0; i < (int)t->pc.n; ++i) if (strcmp(t->pc.tasks[i].name, "pc_0") == 0) ntask = i;
    CHECK(t->pc.tasks[ntask].queued == 0);
    CHECK(gptps_tui_press(t, 'R') == 4);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(t->pc.tasks[ntask].queued == 1 && health(t).pane_reads == 2);
    /* due again after pane_refresh_ms */
    t->pc.read_ms -= 2001;
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(health(t).pane_reads == 3);
    /* a slow pass (9 reads of >= 4 ms) waits 100x its cost before the next, not 2 s */
    tui_read_delay_us = 4000;
    gptps_tui_press(t, 'R');
    gptps_tui_render(t, frame, sizeof frame);
    tui_read_delay_us = 0;
    h = health(t);
    CHECK(h.pane_reads == 4 && h.pane_read_us >= 9 * 4000);
    CHECK(h.pane_every_ms * 1000 >= h.pane_read_us * 100 && h.pane_every_ms > 3000);
    t->pc.read_ms -= 2001;                                 /* pane_refresh_ms is not enough now */
    r0 = reads();
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(reads() == r0 && health(t).pane_reads == 4);
    /* settings pane: one call per setting, plus the selected one read to explain it */
    t->pane = TUI_PANE_SETTINGS; t->sel = 0;
    ns = gptps_settings_count(e);
    r0 = reads();
    gptps_tui_render(t, frame, sizeof frame);
    CHECK((size_t)(reads() - r0) == 1 + ns + 1);
    CHECK(t->pc.n == ns && strstr(frame, "settings read: ") != NULL);
    r0 = reads();
    for (k = 0; k < 10; ++k) gptps_tui_render(t, frame, sizeof frame);
    CHECK(reads() == r0);                                  /* 1.10: 2n calls a frame */
    gptps_tui_press(t, 'j');
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(reads() == r0 + 1);                              /* the new selection, read once */
    /* a value set elsewhere shows at the next pass; one edited here at once */
    CHECK(gptps_settings_set(e, "tui.refresh_ms", "777") == GPTPS_OK);
    gptps_tui_render(t, frame, sizeof frame);
    idx = pc_find(t, "tui.refresh_ms");
    CHECK(idx >= 0 && idx < (int)t->pc.n && strcmp(t->pc.pool + t->pc.sets[idx].val, "777") != 0);
    gptps_tui_press(t, 'R');
    gptps_tui_render(t, frame, sizeof frame);
    idx = pc_find(t, "tui.refresh_ms");
    CHECK(idx >= 0 && strcmp(t->pc.pool + t->pc.sets[idx].val, "777") == 0);
    t->sel = idx;
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(gptps_tui_press(t, '\r') == 4 && t->editing);
    for (k = 0; k < 4; ++k) gptps_tui_press(t, 127);
    gptps_tui_press(t, '5'); gptps_tui_press(t, '5'); gptps_tui_press(t, '5');
    gptps_tui_press(t, '\r');
    gptps_tui_render(t, frame, sizeof frame);
    idx = pc_find(t, "tui.refresh_ms");
    CHECK(idx >= 0 && strcmp(t->pc.pool + t->pc.sets[idx].val, "555") == 0 && t->cfg.refresh_ms == 555);
    /* a long list fitted to 24 rows, with its edit prompt and a status line too */
    t->rows = 24;
    CHECK(gptps_tui_press(t, '\r') == 4 && t->editing);
    snprintf(t->status, sizeof t->status, "a status line"); t->status_ms = gptps_now_ms(NULL);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(t->pc.n > 24 && frame_nlines(frame) <= 24 && strstr(frame, "edit:") != NULL);
    gptps_tui_press(t, 27);
    t->status[0] = 0;
    /* detail pane: an edit goes to the key chosen, though the registry moved since the pass */
    t->pane = TUI_PANE_DETAIL; t->detail_sel = 0;
    copy_name(t->detail_task, sizeof t->detail_task, "pc_5");
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(t->pc.n > 0 && strstr(frame, "task 'pc_5'") != NULL);
    copy_name(key0, sizeof key0, t->pc.n ? t->pc.pool + t->pc.sets[0].key : "");
    CHECK(strncmp(key0, "tasks.pc_5.", 11) == 0);
    CHECK(gptps_unregister_task(e, "pc_2", GPTPS_REMOVE_REJECT_IF_BUSY) == GPTPS_OK);   /* its settings go: indexes shift */
    CHECK(gptps_tui_press(t, '\r') == 4);
    CHECK(t->editing && strcmp(t->edit_key, key0) == 0);
    gptps_tui_press(t, 27);
    /* leaving them frees the cache */
    t->pane = TUI_PANE_DASH;
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(health(t).bytes_panes == 0 && t->pc.pane == -1);
    /* trk_resync: a slow look holds the next back for 99x as long */
    mu_lock(&t->mu); t->trk_lb = 1; t->lb_gen++; mu_unlock(&t->mu);   /* pc_0's item is queued: not idle */
    tui_read_delay_us = 1000;
    r0 = reads();
    gptps_tui_render(t, frame, sizeof frame);
    tui_read_delay_us = 0;
    CHECK(reads() > r0 && t->rs_next_us > tui_now_us() && strstr(frame, ">=") != NULL);
    r0 = reads();
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(reads() == r0);                                  /* held back */
    (void)health(t);
    CHECK(reads() > r0);                                   /* health: the host asks - it looks */
    r0 = reads();
    t->rs_next_us = 1;
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(reads() > r0);                                   /* due again */
    /* a cheap look (well under 200 us) is held back too: 99x its cost (1.11 review: these
     * were not limited, 32-45% of the time in the engine's lock rendering back to back) */
    t->rs_next_us = 1;
    tui_read_delay_us = 100;
    r0 = reads();
    gptps_tui_render(t, frame, sizeof frame);
    tui_read_delay_us = 0;
    CHECK(reads() > r0 && t->rs_cost_us >= 100 && t->rs_next_us >= tui_now_us());
    r0 = reads();
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(reads() == r0);                                  /* held back: within 9.9 ms of it */
    t->rs_next_us = 1;
    r0 = reads();
    gptps_tui_render(t, frame, sizeof frame);              /* the fastest look still sets a hold */
    CHECK(reads() > r0 && t->rs_cost_us >= 1 && t->rs_next_us > 1);
    /* the engine goes idle within a hold: the live loop's frame says when to look again,
     * so '>=' clears then and not only at some unrelated change */
    CHECK(gptps_tui_set_clock(t, GPTPS_TUI_CLOCK_OFF) == GPTPS_OK);
    drain(e);
    {
        tui_fbuf fb; tui_fmeta meta; uint64_t now;
        fb.b = (char *)malloc(TUI_FRAME_MIN); fb.cap = fb.b ? TUI_FRAME_MIN : 0;
        CHECK(fb.b != NULL);
        if (fb.b) {
            t->rs_next_us = tui_now_us() + 50000;          /* 50 ms of hold left */
            r0 = reads();
            now = gptps_now_ms(NULL);
            tui_frame(t, &fb, 1, &meta);
            CHECK(reads() == r0 && t->trk_lb == 1 && strstr(fb.b, ">=") != NULL);   /* held: still '>=' */
            CHECK(meta.next_change != 0 && meta.next_change <= now + 52);
            t->rs_next_us = 1;                             /* the hold over: that frame clears it */
            tui_frame(t, &fb, 1, &meta);
            CHECK(t->trk_lb == 0 && strstr(fb.b, ">=") == NULL);
            free(fb.b);
        }
    }
    /* a tier change on an idle engine is exact at once, a hold or not; health always looks */
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_NORMAL) == GPTPS_OK);
    t->rs_next_us = tui_now_us() + 60000000u;
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_FULL) == GPTPS_OK);
    CHECK(t->trk_lb == 0);
    mu_lock(&t->mu); t->trk_lb = 1; t->lb_gen++; mu_unlock(&t->mu);
    t->rs_next_us = tui_now_us() + 60000000u;
    CHECK(health(t).gauges == GPTPS_TUI_GAUGES_EXACT && t->trk_lb == 0);
    /* the detail pane at 24 rows: a task's 11 default settings all show (stage D's fixed
     * 14 lines hid the last), and while editing with a status the frame still fits */
    t->rows = 24;
    t->pane = TUI_PANE_DETAIL; t->detail_sel = 0;
    copy_name(t->detail_task, sizeof t->detail_task, "pc_5");
    gptps_tui_press(t, 'R');
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(t->pc.n >= 2 && t->pc.n <= 11);
    if (t->pc.n >= 2) {
        const char *last = t->pc.pool + t->pc.sets[t->pc.n - 1].key + strlen("tasks.pc_5.");
        CHECK(strstr(frame, last) != NULL);
        CHECK(frame_nlines(frame) <= 24);
        CHECK(gptps_tui_press(t, '\r') == 4 && t->editing);
        snprintf(t->status, sizeof t->status, "a status line"); t->status_ms = gptps_now_ms(NULL);
        gptps_tui_render(t, frame, sizeof frame);
        CHECK(frame_nlines(frame) <= 24 && strstr(frame, "edit:") != NULL);
        gptps_tui_press(t, 27);
    }
    /* a tasks pass with no memory says so, not "(no task types registered)" */
    t->pane = TUI_PANE_TASKS;                              /* the detail cache freed: cap 0 */
    tui_realloc_fail = 1000;
    gptps_tui_render(t, frame, sizeof frame);
    tui_realloc_fail = 0;
    CHECK(t->pc.n == 0 && t->pc.short_by > 0);
    CHECK(strstr(frame, "no task types registered") == NULL && strstr(frame, "no memory to list") != NULL);
    CHECK(strstr(frame, "not read: no memory") != NULL);
    gptps_tui_press(t, 'R');
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(t->pc.n == 7 && strstr(frame, "no memory") == NULL);   /* 8 less pc_2 */
    drain(e);
    gptps_shutdown(e);
    gptps_tui_close(t);
}

#if !defined(_WIN32)
/* 46) the dashboard is formatted with the lock released: only the copy-out holds it (the
 * format would deadlock here, holding mu, if it took it) */
static void test_format_unlocked(void)
{
    gptps *e = open_manual(); gptps_tui *t; static char frame[16384];
    size_t n;
    if (!e) return;
    t = install(e);
    CHECK(t != NULL);
    if (!t) { gptps_shutdown(e); return; }
    emit(t, GPTPS_EV_QUEUED, "fmt", 1, GPTPS_OK, 1);
    gptps_tui_render(t, frame, sizeof frame);              /* takes the view */
    alarm(10);
    mu_lock(&t->mu);
    n = dash_format(t, t->view, frame, sizeof frame);
    mu_unlock(&t->mu);
    alarm(0);
    CHECK(n > 0 && n < sizeof frame);
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 47) the live loop on a pseudo-terminal: idle costs nothing */
static gptps_tui *lv_t;
static void *lv_run(void *p) { (void)p; gptps_tui_run(lv_t); return NULL; }
static int lv_m = -1;
static size_t lv_got;                          /* bytes read from the terminal's other side */
static char lv_buf[65536];
static size_t lv_buflen;
static void lv_drain(void)
{
    char b[4096];
    ssize_t r;
    while ((r = read(lv_m, b, sizeof b)) > 0) {
        if (lv_buflen + (size_t)r < sizeof lv_buf) { memcpy(lv_buf + lv_buflen, b, (size_t)r); lv_buflen += (size_t)r; }
        lv_got += (size_t)r;
    }
}
static void lv_sleep(int ms)               /* sleep, reading what the loop writes meanwhile */
{
    uint64_t end = gptps_now_ms(NULL) + (uint64_t)ms;
    while (gptps_now_ms(NULL) < end) {
        struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 5000;
        select(0, NULL, NULL, NULL, &tv);
        lv_drain();
    }
    lv_drain();
}
static int lv_until_painted(uint64_t n)       /* wait for the n-th frame (3 s at most) */
{
    int i;
    for (i = 0; i < 300 && health(lv_t).frames_painted < n; ++i) lv_sleep(10);
    return health(lv_t).frames_painted >= n;
}
static gptps *lv_e;
static FILE *lv_out;
static pthread_t lv_th;
/* install on a fresh engine with this config, and start gptps_tui_run on its own thread */
static int lv_start(int mode, int kpi, uint32_t refresh)
{
    gptps_tui_config c;
    lv_e = open_manual();
    if (!lv_e) return 0;
    memset(&c, 0, sizeof c);
    c.struct_size = sizeof c; c.color = 0; c.unicode = 0; c.interactive = 1; c.out = lv_out;
    c.mode = mode; c.kpi = kpi; c.refresh_ms = refresh;
    lv_t = gptps_tui_install(lv_e, &c);
    if (!lv_t) { gptps_shutdown(lv_e); return 0; }
    CHECK(gptps_tui_add_task(lv_t, "nosuch", "Nosuch", 'n', NULL, 0) == GPTPS_OK);   /* refused: a toast */
    lv_got = 0; lv_buflen = 0;
    if (pthread_create(&lv_th, NULL, lv_run, NULL) != 0) { gptps_shutdown(lv_e); gptps_tui_close(lv_t); return 0; }
    CHECK(lv_until_painted(1));
    lv_sleep(300);                                        /* settled */
    return 1;
}
static void lv_stop(void)
{
    tui_quit(lv_t);
    pthread_join(lv_th, NULL);
    CHECK(sc_get(&lv_t->wake_h) == 0);                    /* the wake handle is gone with the loop */
    gptps_shutdown(lv_e);
    gptps_tui_close(lv_t);
    lv_t = NULL;
}
static int lv_burst_done;
static void *lv_burst(void *p)               /* 100k events over ~2 s */
{
    int i, k;
    (void)p;
    for (i = 0; i < 100; ++i) {
        struct timeval tv;
        for (k = 0; k < 500; ++k) {
            gptps_handle h = (gptps_handle)(i * 500 + k + 1);
            emit_a(lv_t, GPTPS_EV_QUEUED, "burst", h, GPTPS_OK, gptps_now_ms(NULL), 0);
            emit_a(lv_t, GPTPS_EV_FINISHED, "burst", h, GPTPS_OK, gptps_now_ms(NULL), 1);
        }
        tv.tv_sec = 0; tv.tv_usec = 20000;
        select(0, NULL, NULL, NULL, &tv);
    }
    __atomic_store_n(&lv_burst_done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}
static int open_fds(void) { int fd, n = 0; for (fd = 0; fd < 1024; ++fd) n += fcntl(fd, F_GETFD) != -1; return n; }
static void test_live_idle(void)
{
    static const int modes[] = { GPTPS_TUI_ON_DEMAND, GPTPS_TUI_PAUSED };
    gptps_tui_health_info h0, h1;
    struct winsize ws;
    int s, k, fd, before;
    size_t i;
    lv_m = posix_openpt(O_RDWR | O_NOCTTY);
    if (lv_m < 0 || grantpt(lv_m) != 0 || unlockpt(lv_m) != 0) { printf("note: no pty here; live loop checks skipped\n"); return; }
    s = open(ptsname(lv_m), O_RDWR | O_NOCTTY);
    if (s < 0 || !freopen(ptsname(lv_m), "r", stdin) || (lv_out = fdopen(s, "w")) == NULL) {
        printf("note: cannot set up a pty; live loop checks skipped\n"); close(lv_m); return;
    }
    fcntl(lv_m, F_SETFL, fcntl(lv_m, F_GETFL) | O_NONBLOCK);
    memset(&ws, 0, sizeof ws); ws.ws_col = 80; ws.ws_row = 24;
    ioctl(s, TIOCSWINSZ, &ws);
    alarm(120);
    /* PAUSED and ON_DEMAND idle 3 s: no frame, no byte, no wake */
    for (i = 0; i < sizeof modes / sizeof modes[0]; ++i) {
        if (!lv_start(modes[i], 0, 50)) continue;
        fd = (int)sc_get(&lv_t->wake_h) - 1;
        CHECK(fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC) && (fcntl(fd, F_GETFL) & O_NONBLOCK));   /* the pipe */
        h0 = health(lv_t); lv_got = 0;
        lv_sleep(3000);
        h1 = health(lv_t);
        if (h1.wakes != h0.wakes || h1.frames_painted != h0.frames_painted)
            printf("mode %d idle: %llu wakes, %llu frames\n", modes[i], (unsigned long long)(h1.wakes - h0.wakes),
                   (unsigned long long)(h1.frames_painted - h0.frames_painted));
        CHECK(h1.frames_painted == h0.frames_painted && h1.bytes_written == h0.bytes_written && lv_got == 0);
        CHECK(h1.wakes == h0.wakes);                      /* blocked, no timeout */
        if (modes[i] == GPTPS_TUI_ON_DEMAND) {
            /* gptps_tui_wake: one wake, a frame made, nothing to write (nothing changed) */
            lv_buflen = 0;
            gptps_tui_wake(lv_t);
            lv_sleep(200);
            h0 = health(lv_t);
            if (getenv("LVDBG")) { lv_buf[lv_buflen] = 0; printf("[[%s]]\n", lv_buf); }
            if (!(h0.wakes == h1.wakes + 1 && h0.bytes_written == h1.bytes_written && h0.frames_skipped == h1.frames_skipped + 1))
                printf("wake: %llu wakes, %llu bytes, %llu skipped, %llu painted\n", (unsigned long long)(h0.wakes - h1.wakes),
                       (unsigned long long)(h0.bytes_written - h1.bytes_written), (unsigned long long)(h0.frames_skipped - h1.frames_skipped),
                       (unsigned long long)(h0.frames_painted - h1.frames_painted));
            CHECK(h0.wakes == h1.wakes + 1 && h0.bytes_written == h1.bytes_written && h0.frames_skipped == h1.frames_skipped + 1);
            /* ...and on a resize, all of it at the new size */
            ws.ws_col = 100; ioctl(s, TIOCSWINSZ, &ws);
            gptps_tui_wake(lv_t);
            lv_sleep(200);
            h1 = health(lv_t);
            mu_lock(&lv_t->rmu); k = lv_t->cols; mu_unlock(&lv_t->rmu);
            CHECK(h1.frames_painted == h0.frames_painted + 1 && k == 100);
            ws.ws_col = 80; ioctl(s, TIOCSWINSZ, &ws);
            /* a toast clears by itself at 3 s: one frame for the key, one when it fades */
            h0 = health(lv_t);
            CHECK(write(lv_m, "n", 1) == 1);
            lv_sleep(500);
            h1 = health(lv_t);
            CHECK(h1.frames_painted == h0.frames_painted + 1 && strstr(lv_buf, "NOT submitted Nosuch") != NULL);
            lv_sleep(2000);
            CHECK(health(lv_t).frames_painted == h1.frames_painted);   /* nothing until it fades */
            lv_buflen = 0;
            lv_sleep(1000);
            h0 = health(lv_t);
            CHECK(h0.frames_painted == h1.frames_painted + 1 && lv_buflen > 0);
            lv_buf[lv_buflen] = 0;
            CHECK(strstr(lv_buf, "NOT submitted") == NULL);
        }
        lv_stop();
        CHECK(fcntl(fd, F_GETFD) == -1);                  /* gone with the loop */
    }
    /* CONTINUOUS idle: it looks every refresh_ms, makes no frame, writes nothing */
    if (lv_start(GPTPS_TUI_CONTINUOUS, 0, 50)) {
        uint64_t made;
        h0 = health(lv_t); lv_got = 0;
        mu_lock(&lv_t->mu); made = lv_t->h_made; mu_unlock(&lv_t->mu);
        lv_sleep(2000);
        h1 = health(lv_t);
        CHECK(h1.bytes_written == h0.bytes_written && lv_got == 0 && h1.frames_painted == h0.frames_painted);
        CHECK(h1.frames_skipped >= h0.frames_skipped + 10);
        mu_lock(&lv_t->mu); CHECK(lv_t->h_made == made); mu_unlock(&lv_t->mu);   /* not even rendered */
        lv_stop();
    }
    /* CONTINUOUS follows a resize with no SIGWINCH handler (no gptps_tui_wake): one ioctl
     * each refresh_ms, as 1.10 - and the 'lost:' line follows a truncated render */
    if (lv_start(GPTPS_TUI_CONTINUOUS, 0, 50)) {
        static char tiny[64];
        h0 = health(lv_t);
        ws.ws_col = 120; ioctl(s, TIOCSWINSZ, &ws);
        lv_sleep(1500);
        h1 = health(lv_t);
        mu_lock(&lv_t->rmu); k = lv_t->cols; mu_unlock(&lv_t->rmu);
        if (k != 120 || h1.frames_painted != h0.frames_painted + 1)
            printf("continuous resize, no wake: cols %d, %llu frames\n", k,
                   (unsigned long long)(h1.frames_painted - h0.frames_painted));
        CHECK(k == 120 && h1.frames_painted == h0.frames_painted + 1);
        ws.ws_col = 80; ioctl(s, TIOCSWINSZ, &ws);
        lv_sleep(500);
        h0 = health(lv_t); lv_buflen = 0;
        gptps_tui_render(lv_t, tiny, sizeof tiny);        /* a host's render, cut: counted */
        lv_sleep(500);
        h1 = health(lv_t);
        lv_buf[lv_buflen] = 0;
        CHECK(h1.frames_truncated == h0.frames_truncated + 1);
        CHECK(h1.frames_painted == h0.frames_painted + 1 && strstr(lv_buf, "1 frames truncated") != NULL);
        lv_sleep(500);
        CHECK(health(lv_t).frames_painted == h1.frames_painted);   /* and then quiet again */
        lv_stop();
    }
    /* ON_DEMAND on another pane: an event costs one frame, then it waits again with no
     * timeout; the help pane is taller than 24 rows, and the same frame again writes 0 */
    if (lv_start(GPTPS_TUI_ON_DEMAND, 0, 50)) {
        lv_buflen = 0;
        CHECK(write(lv_m, "?", 1) == 1);
        lv_sleep(500);
        lv_buf[lv_buflen] = 0;
        CHECK(strstr(lv_buf, "Up/Down move") != NULL);   /* the help pane is up */
        h0 = health(lv_t);
        emit(lv_t, GPTPS_EV_QUEUED, "one", 1, GPTPS_OK, gptps_now_ms(NULL));
        lv_sleep(500);
        h1 = health(lv_t);
        CHECK(h1.bytes_written == h0.bytes_written);      /* the help frame did not change */
        lv_sleep(2000);
        h0 = health(lv_t);
        if (h0.wakes != h1.wakes || h0.bytes_written != h1.bytes_written)
            printf("help pane idle after an event: %llu wakes, %llu bytes\n", (unsigned long long)(h0.wakes - h1.wakes),
                   (unsigned long long)(h0.bytes_written - h1.bytes_written));
        CHECK(h0.wakes == h1.wakes && h0.bytes_written == h1.bytes_written);
        lv_stop();
    }
    /* keys typed after 'q' in the same read are not applied ('n' would try to submit) */
    if (lv_start(GPTPS_TUI_ON_DEMAND, 0, 50)) {
        CHECK(write(lv_m, "qn", 2) == 2);
        pthread_join(lv_th, NULL);                        /* it quit */
        CHECK(strstr(lv_t->status, "Nosuch") == NULL);
        mu_lock(&lv_t->mu); lv_t->quit = 0; mu_unlock(&lv_t->mu);
        CHECK(pthread_create(&lv_th, NULL, lv_run, NULL) == 0);   /* for lv_stop to join */
        lv_sleep(100);
        lv_stop();
    }
    /* one changed line is one line's bytes (GAUGE: an event changes the gauge line only) */
    if (lv_start(GPTPS_TUI_ON_DEMAND, GPTPS_TUI_KPI_GAUGE, 50)) {
        static char full[16384];
        size_t fl = gptps_tui_render(lv_t, full, sizeof full);
        h0 = health(lv_t);
        emit(lv_t, GPTPS_EV_QUEUED, "one", 1, GPTPS_OK, gptps_now_ms(NULL));
        CHECK(lv_until_painted(h0.frames_painted + 1));
        h1 = health(lv_t);
        if (!(h1.bytes_written - h0.bytes_written < fl / 2))
            printf("one line changed: %llu bytes written, the frame is %lu\n",
                   (unsigned long long)(h1.bytes_written - h0.bytes_written), (unsigned long)fl);
        CHECK(h1.bytes_written > h0.bytes_written && h1.bytes_written - h0.bytes_written < fl / 2);
        lv_stop();
    }
    /* a burst of 100k events at refresh 250 ms: frames bounded by the time, one wake each */
    for (k = 0; k < 2; ++k) {
        pthread_t b;
        uint64_t t0, dt;
        if (!lv_start(k ? GPTPS_TUI_CONTINUOUS : GPTPS_TUI_ON_DEMAND, GPTPS_TUI_KPI_MINIMAL, 250)) continue;
        h0 = health(lv_t);
        mu_lock(&lv_t->mu); lv_t->h_kicks = 0; mu_unlock(&lv_t->mu);
        t0 = gptps_now_ms(NULL);
        lv_burst_done = 0;
        CHECK(pthread_create(&b, NULL, lv_burst, NULL) == 0);
        while (!__atomic_load_n(&lv_burst_done, __ATOMIC_SEQ_CST)) lv_sleep(20);
        pthread_join(b, NULL);
        dt = gptps_now_ms(NULL) - t0;
        lv_sleep(600);
        h1 = health(lv_t);
        {
            uint64_t frames = h1.frames_painted - h0.frames_painted, kicks;
            mu_lock(&lv_t->mu); kicks = lv_t->h_kicks; mu_unlock(&lv_t->mu);
            if (frames > dt / 250 + 4 || frames < 2 || kicks > frames + 2 || getenv("LVDBG"))
                printf("burst (%s): %llu frames, %llu wakes sent, in %llu ms\n", k ? "realtime" : "on-demand",
                       (unsigned long long)frames, (unsigned long long)kicks, (unsigned long long)dt);
            CHECK(frames >= 2 && frames <= dt / 250 + 4);
            CHECK(kicks <= frames + 2);                   /* on-demand: one signal a frame; realtime: none */
            if (k) CHECK(kicks == 0);
        }
        lv_stop();
    }
    /* headless: no pipe at all */
    CHECK(freopen("/dev/null", "r", stdin) != NULL);
    before = open_fds();
    lv_e = open_manual();
    if (lv_e) {
        gptps_tui_config c;
        memset(&c, 0, sizeof c);
        c.struct_size = sizeof c; c.interactive = 1; c.out = lv_out;
        lv_t = gptps_tui_install(lv_e, &c);
        CHECK(lv_t != NULL);
        if (lv_t) {
            gptps_tui_run(lv_t);                          /* returns at once: stdin is no terminal */
            CHECK(sc_get(&lv_t->wake_h) == 0 && health(lv_t).wakes == 0 && health(lv_t).frames_painted == 0);
            gptps_tui_wake(lv_t);                         /* a no-op with no loop */
        }
        gptps_shutdown(lv_e);
        gptps_tui_close(lv_t);
    }
    CHECK(open_fds() == before);
    /* a host with more than FD_SETSIZE descriptors open: the wake pipe lands past it, and the
     * loop still waits on it (select's fd_set could not hold it) */
    {
        struct rlimit rl;
        int dn[FD_SETSIZE + 16], nd = 0, j;
        if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && (rl.rlim_cur >= FD_SETSIZE + 64 ||
            (rl.rlim_max >= FD_SETSIZE + 64 && (rl.rlim_cur = FD_SETSIZE + 64, setrlimit(RLIMIT_NOFILE, &rl) == 0)))) {
            int z = open("/dev/null", O_RDONLY);
            while (z >= 0 && nd < (int)(sizeof dn / sizeof dn[0]) && (dn[nd] = dup(z)) >= 0 && dn[nd] < FD_SETSIZE + 4) ++nd;
            if (z >= 0 && nd < (int)(sizeof dn / sizeof dn[0]) && dn[nd] >= 0) ++nd;
            /* z stays open until the end: a hole below FD_SETSIZE would take the pipe's read end */
            CHECK(freopen(ptsname(lv_m), "r", stdin) != NULL);
            if (lv_start(GPTPS_TUI_ON_DEMAND, 0, 50)) {
                CHECK(sc_get(&lv_t->wake_h) - 1 >= FD_SETSIZE);
                h0 = health(lv_t);
                emit(lv_t, GPTPS_EV_QUEUED, "one", 1, GPTPS_OK, gptps_now_ms(NULL));
                CHECK(lv_until_painted(h0.frames_painted + 1));   /* woken through the pipe */
                lv_stop();
            }
            for (j = 0; j < nd; ++j) close(dn[j]);
            if (z >= 0) close(z);
        } else printf("note: cannot open %d descriptors; the FD_SETSIZE check is skipped\n", FD_SETSIZE + 64);
    }
    alarm(0);
    fclose(lv_out);
    close(lv_m);
}
#endif

#if defined(_WIN32)
/* 49) Windows: the live loop on a pseudo console (ConPTY), as test_live_idle on a pty.
 * ctest gives a test no console, so the test runs itself as a child attached to one it
 * makes: the child runs gptps_tui_run and checks, through gptps_tui_health, that an idle
 * ON_DEMAND / PAUSED loop makes no wake, no frame and no write; that gptps_tui_wake makes
 * one wake and no write; and that a resize (ResizePseudoConsole: the console's
 * WINDOW_BUFFER_SIZE_EVENT) repaints once at the new width. The parent drains the console's
 * output (or the child would block writing) and resizes when the child asks. Skipped,
 * saying so, where there is no ConPTY (before Windows 10 1809). */
typedef HRESULT (WINAPI *cpc_fn)(COORD, HANDLE, HANDLE, DWORD, void **);
typedef HRESULT (WINAPI *rpc_fn)(void *, COORD);
typedef void (WINAPI *clpc_fn)(void *);
typedef BOOL (WINAPI *ipal_fn)(void *, DWORD, DWORD, SIZE_T *);
typedef BOOL (WINAPI *upal_fn)(void *, DWORD, DWORD_PTR, void *, SIZE_T, void *, SIZE_T *);
typedef void (WINAPI *dpal_fn)(void *);
#define TUI_PTA_PSEUDOCONSOLE ((DWORD_PTR)0x00020016)
static HANDLE wc_rd;
static volatile LONG wc_got;
static DWORD WINAPI wc_drain(void *p)
{
    char b[4096]; DWORD n;
    (void)p;
    while (ReadFile(wc_rd, b, sizeof b, &n, NULL) && n) InterlockedExchangeAdd(&wc_got, (LONG)n);
    return 0;
}
static void wc_name(char *b, size_t cap, unsigned long pid, const char *what)
{ snprintf(b, cap, "Local\\gptps_tui_unit_%lu_%s", pid, what); }
/* where the child's printf (CHECK's FAIL lines) goes: the parent prints it on a failure */
static void wc_log(char *b, size_t cap, unsigned long pid)
{
    char d[MAX_PATH];
    DWORD n = GetTempPathA(sizeof d, d);
    snprintf(b, cap, "%sgptps_tui_unit_%lu.log", (n && n < sizeof d) ? d : ".\\", pid);
}
/* the child: on the console it was started on */
static gptps_tui *wc_t;
static DWORD WINAPI wc_run(void *p) { (void)p; gptps_tui_run(wc_t); return 0; }
static int wc_until_painted(uint64_t n)
{
    int i;
    for (i = 0; i < 300 && health(wc_t).frames_painted < n; ++i) Sleep(10);
    return health(wc_t).frames_painted >= n;
}
static int win_conpty_child(unsigned long ppid)
{
    static const int modes[] = { GPTPS_TUI_ON_DEMAND, GPTPS_TUI_PAUSED };
    char na[96], nb[96], nc[96], lg[MAX_PATH + 64];
    HANDLE ask, done, norz;
    FILE *con;
    size_t i;
    wc_name(na, sizeof na, ppid, "ask"); wc_name(nb, sizeof nb, ppid, "done"); wc_log(lg, sizeof lg, ppid);
    ask = OpenEventA(EVENT_MODIFY_STATE, FALSE, na);
    done = OpenEventA(SYNCHRONIZE, FALSE, nb);
    wc_name(nc, sizeof nc, ppid, "norz");
    norz = OpenEventA(SYNCHRONIZE, FALSE, nc);
    /* frames to the console, diagnostics to the log; then the console's own handles as
     * the std handles the add-on asks for (GetStdHandle), whatever the CRT left there */
    if (!freopen(lg, "w", stdout) || !freopen("CONIN$", "r", stdin) || (con = fopen("CONOUT$", "w")) == NULL ||
        !ask || !done || !norz) return 90;
    setvbuf(stdout, NULL, _IONBF, 0);
    SetStdHandle(STD_OUTPUT_HANDLE, CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                NULL, OPEN_EXISTING, 0, NULL));
    SetStdHandle(STD_INPUT_HANDLE, CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                               NULL, OPEN_EXISTING, 0, NULL));
    for (i = 0; i < sizeof modes / sizeof modes[0]; ++i) {
        gptps_tui_config c;
        gptps_tui_health_info h0, h1;
        gptps *e = open_manual();
        HANDLE th;
        int k;
        if (!e) return 91;
        memset(&c, 0, sizeof c);
        c.struct_size = sizeof c; c.color = 0; c.unicode = 0; c.interactive = 1; c.out = con;
        c.mode = modes[i]; c.refresh_ms = 50;
        wc_t = gptps_tui_install(e, &c);
        if (!wc_t) { gptps_shutdown(e); return 92; }
        th = CreateThread(NULL, 0, wc_run, NULL, 0, NULL);
        if (!th) { gptps_shutdown(e); gptps_tui_close(wc_t); return 93; }
        CHECK(wc_until_painted(1));
        CHECK(sc_get(&wc_t->wake_h) != 0);                    /* the event exists while it runs */
        Sleep(300);                                           /* settled */
        h0 = health(wc_t);
        Sleep(3000);
        h1 = health(wc_t);
        if (h1.wakes != h0.wakes || h1.frames_painted != h0.frames_painted)
            printf("mode %d idle: %llu wakes, %llu frames\n", modes[i], (unsigned long long)(h1.wakes - h0.wakes),
                   (unsigned long long)(h1.frames_painted - h0.frames_painted));
        CHECK(h1.frames_painted == h0.frames_painted && h1.bytes_written == h0.bytes_written);
        CHECK(h1.wakes == h0.wakes);                          /* blocked, no timeout */
        if (modes[i] == GPTPS_TUI_ON_DEMAND) {
            gptps_tui_wake(wc_t);                             /* one wake, a frame made, nothing written */
            Sleep(300);
            h0 = health(wc_t);
            CHECK(h0.wakes == h1.wakes + 1 && h0.bytes_written == h1.bytes_written && h0.frames_skipped == h1.frames_skipped + 1);
            SetEvent(ask);                                    /* the parent resizes the console to 100 x 24 */
            CHECK(WaitForSingleObject(done, 10000) == WAIT_OBJECT_0);
            if (WaitForSingleObject(norz, 0) == WAIT_OBJECT_0) goto stop;   /* the parent could not resize */
            for (k = 0; k < 300 && health(wc_t).frames_painted < h0.frames_painted + 1; ++k) Sleep(10);
            Sleep(300);
            h1 = health(wc_t);
            mu_lock(&wc_t->rmu); k = wc_t->cols; mu_unlock(&wc_t->rmu);
            if (h1.frames_painted != h0.frames_painted + 1 || k != 100)
                printf("resize: %llu frames, cols %d\n", (unsigned long long)(h1.frames_painted - h0.frames_painted), k);
            CHECK(h1.frames_painted == h0.frames_painted + 1 && k == 100);
        }
    stop:
        tui_quit(wc_t);
        WaitForSingleObject(th, INFINITE);
        CloseHandle(th);
        CHECK(sc_get(&wc_t->wake_h) == 0);                    /* gone with the loop */
        gptps_shutdown(e);
        gptps_tui_close(wc_t);
        wc_t = NULL;
    }
    return fails > 80 ? 80 : fails;
}
static void test_live_idle_win(const char *self)
{
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    cpc_fn cpc = k32 ? (cpc_fn)(void (*)(void))GetProcAddress(k32, "CreatePseudoConsole") : NULL;
    rpc_fn rpc = k32 ? (rpc_fn)(void (*)(void))GetProcAddress(k32, "ResizePseudoConsole") : NULL;
    clpc_fn clpc = k32 ? (clpc_fn)(void (*)(void))GetProcAddress(k32, "ClosePseudoConsole") : NULL;
    ipal_fn ipal = k32 ? (ipal_fn)(void (*)(void))GetProcAddress(k32, "InitializeProcThreadAttributeList") : NULL;
    upal_fn upal = k32 ? (upal_fn)(void (*)(void))GetProcAddress(k32, "UpdateProcThreadAttribute") : NULL;
    dpal_fn dpal = k32 ? (dpal_fn)(void (*)(void))GetProcAddress(k32, "DeleteProcThreadAttributeList") : NULL;
    HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL, ask, done, drain, hs[2];
    void *pc = NULL, *al = NULL;
    SIZE_T asz = 0;
    STARTUPINFOEXA si;
    PROCESS_INFORMATION pi;
    COORD sz;
    char cmd[MAX_PATH + 64], na[96], nb[96], nc[96];
    HANDLE norz;
    DWORD code = 99, r;
    if (!cpc || !rpc || !clpc || !ipal || !upal || !dpal) {
        printf("note: no ConPTY here (Windows 10 1809+); the Windows live loop checks are skipped\n");
        return;
    }
    wc_name(na, sizeof na, (unsigned long)GetCurrentProcessId(), "ask");
    wc_name(nb, sizeof nb, (unsigned long)GetCurrentProcessId(), "done");
    ask = CreateEventA(NULL, FALSE, FALSE, na);
    done = CreateEventA(NULL, FALSE, FALSE, nb);
    wc_name(nc, sizeof nc, (unsigned long)GetCurrentProcessId(), "norz");
    norz = CreateEventA(NULL, TRUE, FALSE, nc);           /* manual reset: set = no resize was made */
    sz.X = 80; sz.Y = 24;
    if (!ask || !done || !norz || !CreatePipe(&in_r, &in_w, NULL, 0) || !CreatePipe(&out_r, &out_w, NULL, 0) ||
        FAILED(cpc(sz, in_r, out_w, 0, &pc))) {
        printf("note: cannot make a pseudo console here; the Windows live loop checks are skipped\n");
        return;
    }
    CloseHandle(in_r); CloseHandle(out_w);                /* the console holds its own */
    wc_rd = out_r;
    drain = CreateThread(NULL, 0, wc_drain, NULL, 0, NULL);
    ipal(NULL, 1, 0, &asz);
    al = malloc(asz);
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    CHECK(al && drain && ipal(al, 1, 0, &asz) && upal(al, 0, TUI_PTA_PSEUDOCONSOLE, pc, sizeof pc, NULL, NULL));
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)al;
    snprintf(cmd, sizeof cmd, "\"%s\" --conpty-child %lu", self, (unsigned long)GetCurrentProcessId());
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi)) {
        printf("FAIL: CreateProcess on the pseudo console: error %lu\n", (unsigned long)GetLastError());
        ++fails;
    } else {
        hs[0] = pi.hProcess; hs[1] = ask;
        for (;;) {
            r = WaitForMultipleObjects(2, hs, FALSE, 60000);
            if (r == WAIT_OBJECT_0 + 1) {                 /* the child asks for a resize */
                HRESULT hr;
                sz.X = 100; sz.Y = 24;
                hr = rpc(pc, sz);
                if (FAILED(hr)) {
                    /* Wine (9) does not implement it: say so and skip. On Windows itself it
                     * is a failure - so a green Windows CI run means the check ran. */
                    HMODULE nt = GetModuleHandleA("ntdll.dll");
                    int wine = nt && GetProcAddress(nt, "wine_get_version") != NULL;
                    if (wine) printf("note: ResizePseudoConsole is not implemented on Wine (0x%08lx); the resize "
                                     "repaint check is skipped\n", (unsigned long)hr);
                    else { printf("FAIL: ResizePseudoConsole: 0x%08lx\n", (unsigned long)hr); ++fails; }
                    SetEvent(norz);
                }
                SetEvent(done);
                continue;
            }
            break;
        }
        if (r != WAIT_OBJECT_0) { printf("FAIL: the pseudo-console child did not finish in 60 s\n"); ++fails;
                                  TerminateProcess(pi.hProcess, 98); WaitForSingleObject(pi.hProcess, 5000); }
        GetExitCodeProcess(pi.hProcess, &code);
        if (code) {
            char lg[MAX_PATH + 64], ln[512];
            FILE *f;
            wc_log(lg, sizeof lg, (unsigned long)GetCurrentProcessId());
            printf("FAIL: the pseudo-console child failed %lu check(s) (90+: it could not set up); its log:\n",
                   (unsigned long)code);
            ++fails;
            if ((f = fopen(lg, "r")) != NULL) { while (fgets(ln, sizeof ln, f)) printf("  child: %s", ln); fclose(f); }
        }
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        CHECK(InterlockedExchangeAdd(&wc_got, 0) > 0);    /* it did paint on that console */
    }
    {
        char lg[MAX_PATH + 64];
        wc_log(lg, sizeof lg, (unsigned long)GetCurrentProcessId());
        remove(lg);
    }
    clpc(pc);                                             /* ends the drain thread's ReadFile */
    if (drain) { WaitForSingleObject(drain, 10000); CloseHandle(drain); }
    CloseHandle(out_r); CloseHandle(in_w);
    if (al) { dpal(al); free(al); }
    CloseHandle(ask); CloseHandle(done); CloseHandle(norz);
}
#endif

int main(int argc, char **argv)
{
#if defined(_WIN32)
    if (argc == 3 && strcmp(argv[1], "--conpty-child") == 0) return win_conpty_child(strtoul(argv[2], NULL, 10));
#else
    (void)argc; (void)argv;
#endif
    test_sanitiser();
    test_recent_fits();
    test_editor_and_commit();
    test_hotkey_refused();
    test_dead_letters();
    test_untrusted_settings();
    test_editor_bytes();
    test_long_names();
    test_no_memory();
    test_legend_full();
    test_latency_release();
    test_latency_overflow();
    test_latency_below_window();
    test_no_memory_frames();
    test_counters_64();
    test_clamps_logged();
    test_frame_truncated();
    test_toast_fades();
    test_struct_size();
    test_why_bounded();
    test_event_path_no_alloc();
    test_cancel_in_queue();
    test_health_struct_size();
    test_name_index();
    test_config_abi();
    test_lower_bound_sticks();
    test_ladder_layers();
    test_ladder_names();
    test_journals();
    test_save_reload();
    test_percentiles();
    test_recent_merged();
    test_config_111();
    test_narrow_lines();
    test_frame_diff();
    test_header();
    test_frame_grows();
    test_height_fit();
    test_legend_cache();
    test_pane_cache();
#if !defined(_WIN32)
    test_format_unlocked();
#if defined(GPTPS_TEST_HAL_SIM)
    printf("note: on the simulation HAL the live loop on a pseudo-terminal is not run: it waits in "
           "poll(), which the simulation does not see (docs/HAL.md); the native legs run it\n");
#else
    test_live_idle();
#endif
    test_health_vs_dead_letters();
    test_storage_storm();
    test_run_without_stdin();
    test_render_while_unregistering();
#else
    test_windows_keys();
    test_live_idle_win(argv[0]);
#endif
    if (fails) { printf("%d tui unit check(s) FAILED\n", fails); return 1; }
    printf("all tui unit checks passed\n");
    return 0;
}
