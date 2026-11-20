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
 */
#if !defined(_WIN32)
#  define _XOPEN_SOURCE 700      /* posix_openpt & co., for 7) */
#endif
/* 1.10.1 13): the add-on's calloc, made to fail on demand - after `tui_calloc_skip`
 * more calls succeed, the next `tui_calloc_fail` calls return NULL. Defined before
 * the macro, so it reaches the real calloc. */
#include <stdlib.h>
static int tui_calloc_skip, tui_calloc_fail;
static void *tui_test_calloc(size_t n, size_t s)
{
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
    if (tui_malloc_fail > 0) { --tui_malloc_fail; return NULL; }
    return malloc(n);
}
#define calloc(n, s) tui_test_calloc((n), (s))
#define malloc(n) tui_test_malloc((n))
#include "../addons/gptps_tui.c"
#undef calloc
#undef malloc
#include <stdio.h>
#if !defined(_WIN32)
#  include <fcntl.h>
#  include <pthread.h>
#  include <signal.h>
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
    tui_why(&ev, out, n);
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
    CHECK(term_poll_key(0) == 0xA0);               /* the queued byte, no wait */
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

static tui_task *row(gptps_tui *t, const char *name)
{
    int i;
    for (i = 0; i < t->ntasks; ++i) if (strcmp(t->tasks[i].name, name) == 0) return &t->tasks[i];
    return NULL;
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
    CHECK(t->ntasks == 1);                                 /* one row, not five */
    CHECK(strcmp(t->recent[(t->rhead + t->rcap - 1) % t->rcap].name, name) == 0);   /* the whole name */
    for (k = 0; k < 63; ++k) { name[2 * k] = '\xc3'; name[2 * k + 1] = '\xa9'; }    /* 63 x U+00E9 */
    name[126] = 'x'; name[127] = 0;                        /* 127 bytes: GPTPS_TASK_NAME_MAX */
    emit(t, GPTPS_EV_QUEUED, name, 20, GPTPS_OK, gptps_now_ms(NULL));
    emit(t, GPTPS_EV_STARTED, name, 20, GPTPS_OK, gptps_now_ms(NULL));
    CHECK(t->ntasks == 2);
    CHECK(row(t, name) != NULL && row(t, name)->started == 1);
    CHECK(strlen(t->tasks[1].label) == 62);                /* cut at a character: 31 whole ones */
    CHECK(valid_utf8(t->tasks[1].label));
    CHECK(gptps_tui_add_task(t, name, NULL, 'z', NULL, 0) == GPTPS_OK && t->ntasks == 2);   /* the same row */
    memset(big, 'b', sizeof big - 1); big[sizeof big - 1] = 0;
    CHECK(gptps_tui_add_task(t, big, NULL, 0, NULL, 0) == GPTPS_E_INVAL);   /* never a task's name */
    CHECK(t->ntasks == 2);
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 13) */
static void test_no_memory(void)
{
    gptps *e = open_manual(); gptps_tui *t; static char frame[16384];
    if (!e) return;
    log_begin();
    tui_calloc_skip = 1; tui_calloc_fail = 2;              /* the dashboard; then the recent log and the ring */
    t = install_cfg(e, GPTPS_TUI_KPI_FULL, 64, 0);
    tui_calloc_skip = 0; tui_calloc_fail = 0;
    CHECK(t != NULL);                                      /* it installs anyway */
    if (!t) { log_end(); gptps_shutdown(e); return; }
    CHECK(t->recent == NULL && t->lat == NULL);
    CHECK(log_count("no memory for the recent-events log") == 1);
    CHECK(log_count("no memory for the latency ring") == 1);
    emit(t, GPTPS_EV_QUEUED, "job", 1, GPTPS_OK, 1);       /* and runs without them */
    emit(t, GPTPS_EV_STARTED, "job", 1, GPTPS_OK, 2);
    emit(t, GPTPS_EV_FINISHED, "job", 1, GPTPS_OK, 3);
    CHECK(t->fin == 1 && row(t, "job") != NULL && row(t, "job")->finished == 1);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "latency off: no memory") != NULL);
    tui_calloc_fail = 1;                                   /* the ring again: the setter says so */
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_FULL) == GPTPS_E_NOMEM);
    CHECK(t->lat == NULL && t->recent != NULL);            /* the log's allocation went through */
    tui_calloc_fail = 1;
    CHECK(gptps_tui_set_latency_window(t, 128) == GPTPS_E_NOMEM && t->lat == NULL);
    CHECK(gptps_tui_set_latency_window(t, 128) == GPTPS_OK && t->lat != NULL);   /* retried */
    tui_calloc_fail = 1;                                   /* below FULL first, then up: the ring fails */
    CHECK(gptps_tui_set_kpi(t, GPTPS_TUI_KPI_NORMAL) == GPTPS_OK && t->lat == NULL);
    CHECK(gptps_tui_press(t, 'm') == 3 && t->kpi == GPTPS_TUI_KPI_FULL && t->lat == NULL);
    CHECK(strstr(t->status, "no memory") != NULL);         /* the 'm' toast says so */
    CHECK(log_count("gptps_tui_set_kpi(full): no memory") == 2);
    CHECK(log_count("gptps_tui_set_latency_window: no memory") == 1);
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

static int live_entries(gptps_tui *t, gptps_handle h)
{
    int j, c = 0;
    for (j = 0; j < t->lat_cap; ++j) if (t->lat[j].h == h && !t->lat[j].done) ++c;
    return c;
}

/* 15) */
static void test_latency_release(void)
{
    gptps *e = open_manual(); gptps_tui *t; tui_task *k;
    if (!e) return;
    t = install_cfg(e, GPTPS_TUI_KPI_FULL, 16, 0);
    CHECK(t != NULL && t->lat != NULL);
    if (!t || !t->lat) { gptps_shutdown(e); gptps_tui_close(t); return; }
    emit(t, GPTPS_EV_QUEUED, "rel", 1, GPTPS_OK, 100);
    emit(t, GPTPS_EV_STARTED, "rel", 1, GPTPS_OK, 101);
    emit(t, GPTPS_EV_FAILED, "rel", 1, GPTPS_E_TASK, 102);
    CHECK(live_entries(t, 1) == 1);                        /* an attempt failed: not the end */
    emit(t, GPTPS_EV_DEAD_LETTERED, "rel", 1, GPTPS_E_TASK, 103);
    CHECK(live_entries(t, 1) == 0);                        /* the end: released */
    emit(t, GPTPS_EV_QUEUED, "rel", 2, GPTPS_OK, 110);
    emit(t, GPTPS_EV_FAILED, "rel", 2, GPTPS_E_CANCELLED, 111);   /* cancelled while queued */
    CHECK(live_entries(t, 2) == 0);
    emit(t, GPTPS_EV_QUEUED, "rel", 3, GPTPS_OK, 120);
    emit(t, GPTPS_EV_DROPPED, "rel", 3, GPTPS_E_TASK, 121);
    CHECK(live_entries(t, 3) == 0);
    k = row(t, "rel");
    CHECK(k != NULL && k->lat_n == 0);                     /* no sample: avg ms is finished items */
    /* a retried attempt keeps its entry: the sample spans queue to the final finish */
    emit(t, GPTPS_EV_QUEUED, "rel", 4, GPTPS_OK, 200);
    emit(t, GPTPS_EV_STARTED, "rel", 4, GPTPS_OK, 201);
    emit(t, GPTPS_EV_FAILED, "rel", 4, GPTPS_E_TASK, 202);
    emit(t, GPTPS_EV_RETRIED, "rel", 4, GPTPS_E_TASK, 203);
    emit(t, GPTPS_EV_STARTED, "rel", 4, GPTPS_OK, 204);
    emit(t, GPTPS_EV_FINISHED, "rel", 4, GPTPS_OK, 230);
    CHECK(k != NULL && k->lat_n == 1 && k->lat_sum_ms == 30);
    /* a cancel that outruns its own QUEUED: the late QUEUED clears it, no sample, no entry */
    emit(t, GPTPS_EV_FAILED, "rel", 5, GPTPS_E_CANCELLED, 300);
    emit(t, GPTPS_EV_QUEUED, "rel", 5, GPTPS_OK, 299);
    CHECK(live_entries(t, 5) == 0 && t->lat_tomb == 0 && k != NULL && k->lat_n == 1);
    CHECK(t->lat_live == 0);
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 16) */
static void test_latency_overflow(void)
{
    gptps *e = open_manual(); gptps_tui *t; tui_task *k; static char frame[16384];
    gptps_handle h;
    int maxtomb = 0, round;
    if (!e) return;
    log_begin();
    t = install_cfg(e, GPTPS_TUI_KPI_FULL, 64, 0);
    CHECK(t != NULL && t->lat != NULL);
    if (!t || !t->lat) { log_end(); gptps_shutdown(e); gptps_tui_close(t); return; }
    for (round = 0; round < 2; ++round) {                  /* depth 200 at window 64, twice */
        gptps_handle base = (gptps_handle)(1 + round * 1000);
        for (h = base; h < base + 200; ++h) emit(t, GPTPS_EV_QUEUED, "deep", h, GPTPS_OK, 1000 + h);
        for (h = base; h < base + 200; ++h) {
            emit(t, GPTPS_EV_FINISHED, "deep", h, GPTPS_OK, 5000 + h);
            if (t->lat_tomb > maxtomb) maxtomb = t->lat_tomb;
        }
    }
    k = row(t, "deep");
    CHECK(k != NULL && k->lat_n == 128);                   /* the 64 still tracked, each round */
    CHECK(maxtomb <= 64 && t->lat_tomb == 0);              /* bounded; none left to scan for */
    CHECK(t->lat_evicted == 272 && t->lat_live == 0);      /* the loss, counted: 136 a round */
    CHECK(log_count("tui.latency_window = 64") == 1);      /* WARNed, once for this window */
    CHECK(log_count("about 65 items were queued or running") == 1);  /* the depth when it began */
    CHECK(t->lat_skipped == 272);                          /* the ends of the evicted: no room, counted */
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "latency: 272 not sampled, 272 ends unmatched (window 64 too small)") != NULL);
    CHECK(gptps_tui_set_latency_window(t, 32) == GPTPS_OK);   /* a new window: counted afresh, WARNed again */
    for (h = 5000; h < 5040; ++h) emit(t, GPTPS_EV_QUEUED, "deep", h, GPTPS_OK, 1);
    CHECK(t->lat_evicted == 8 && log_count("tui.latency_window = 32") == 1);
    log_end();
    gptps_shutdown(e);
    gptps_tui_close(t);
}

/* 22) */
static void test_latency_below_window(void)
{
    gptps *e = open_manual(); gptps_tui *t; tui_task *k; static char frame[16384]; gptps_handle h;
    if (!e) return;
    log_begin();
    t = install_cfg(e, GPTPS_TUI_KPI_FULL, 4, 0);
    CHECK(t != NULL && t->lat != NULL);
    if (!t || !t->lat) { log_end(); gptps_shutdown(e); gptps_tui_close(t); return; }
    emit(t, GPTPS_EV_QUEUED, "x", 1, GPTPS_OK, 100);       /* long-running: stays live, at slot 0 */
    for (h = 2; h <= 4; ++h) { emit(t, GPTPS_EV_QUEUED, "x", h, GPTPS_OK, 200); emit(t, GPTPS_EV_FINISHED, "x", h, GPTPS_OK, 210); }
    CHECK(t->lat_head == 0 && live_entries(t, 1) == 1);    /* the ring came round to it; 3 slots free */
    emit(t, GPTPS_EV_FINISHED, "x", 5, GPTPS_OK, 305);     /* outruns its QUEUED */
    emit(t, GPTPS_EV_QUEUED, "x", 5, GPTPS_OK, 300);
    k = row(t, "x");
    CHECK(k != NULL && k->lat_n == 4 && k->lat_sum_ms == 35);   /* the fast one is sampled */
    CHECK(live_entries(t, 5) == 0 && live_entries(t, 1) == 1 && t->lat_tomb == 0);   /* no zombie */
    for (h = 6; h <= 12; ++h) { emit(t, GPTPS_EV_QUEUED, "x", h, GPTPS_OK, 400); emit(t, GPTPS_EV_FINISHED, "x", h, GPTPS_OK, 401); }
    CHECK(live_entries(t, 1) == 1);                        /* a QUEUED does not evict it either */
    emit(t, GPTPS_EV_FINISHED, "x", 1, GPTPS_OK, 1100);
    CHECK(k != NULL && k->lat_n == 12 && k->lat_max_ms == 1000);
    CHECK(t->lat_evicted == 0 && t->lat_skipped == 0 && t->lat_live == 0 && log_count("latency samples") == 0);
    /* no room at all: four live, and an end with no entry - counted, WARNed, and its
     * late QUEUED starts no clock (it would evict a live entry for an ended item) */
    for (h = 20; h < 24; ++h) emit(t, GPTPS_EV_QUEUED, "x", h, GPTPS_OK, 500);
    emit(t, GPTPS_EV_FINISHED, "x", 30, GPTPS_OK, 505);
    emit(t, GPTPS_EV_QUEUED, "x", 30, GPTPS_OK, 501);
    CHECK(t->lat_skipped == 1 && t->lat_evicted == 0 && live_entries(t, 30) == 0 && t->lat_live == 4);
    for (h = 20; h < 24; ++h) CHECK(live_entries(t, h) == 1);
    CHECK(log_count("tui.latency_window = 4 entries has no room left") == 1);
    gptps_tui_render(t, frame, sizeof frame);
    CHECK(strstr(frame, "latency: 0 not sampled, 1 ends unmatched (window 4 too small)") != NULL);
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
    c.struct_size = sizeof c; c.max_recent = 100; c.kpi = 9; c.mode = 7; c.latency_window = (1 << 20) + 1;
    t1 = gptps_tui_install(e, &c);
    CHECK(t1 != NULL);
    CHECK(log_count("cfg.max_recent 100 is above the cap of 64") == 1);
    CHECK(log_count("cfg.kpi 9 is not a gptps_tui_kpi") == 1);
    CHECK(log_count("cfg.mode 7 is not a gptps_tui_mode") == 1);
    CHECK(log_count("cfg.latency_window 1048577 is above the cap") == 1);
    if (t1) {
        CHECK(t1->cfg.max_recent == 64 && t1->kpi == GPTPS_TUI_KPI_FULL && t1->mode == GPTPS_TUI_CONTINUOUS &&
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
                CHECK(t2->nover == 0 && log_count("gets no row") == 0);   /* without using up the WARN */
            }
            snprintf(nm, sizeof nm, "type_%02d", i);
            emit(t2, GPTPS_EV_QUEUED, nm, (gptps_handle)(i + 1), GPTPS_OK, 1);
            emit(t2, GPTPS_EV_QUEUED, nm, (gptps_handle)(i + 100), GPTPS_OK, 1);
        }
        CHECK(t2->ntasks == TUI_MAX_TASKS && t2->nover == 2);
        CHECK(log_count("task type 'type_64' gets no row") == 1);   /* once, for the first */
        CHECK(log_count("gets no row") == 1);
        t2->rows = 200;
        gptps_tui_render(t2, frame, sizeof frame);
        CHECK(strstr(frame, "+2 types not shown (the table holds 64)") != NULL);
    }
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
    CHECK(paint_due(0, 0, GPTPS_TUI_CONTINUOUS, 0, 0, 5000) == 1);
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

int main(void)
{
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
#if !defined(_WIN32)
    test_run_without_stdin();
    test_render_while_unregistering();
#else
    test_windows_keys();
#endif
    if (fails) { printf("%d tui unit check(s) FAILED\n", fails); return 1; }
    printf("all tui unit checks passed\n");
    return 0;
}
