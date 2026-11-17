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
 */
#if !defined(_WIN32)
#  define _XOPEN_SOURCE 700      /* posix_openpt & co., for 7) */
#endif
#include "../addons/gptps_tui.c"
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

int main(void)
{
    test_sanitiser();
    test_recent_fits();
    test_editor_and_commit();
    test_hotkey_refused();
    test_dead_letters();
    test_untrusted_settings();
    test_editor_bytes();
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
