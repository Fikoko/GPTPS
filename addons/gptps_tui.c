/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * tui.c - real-time terminal dashboard for GPTPS (see tui.h).
 *
 * An observer aggregates live state (cumulative counts, per-task stats, a recent-
 * events ring). gptps_tui_render() formats that state into a frame string (pure,
 * testable). gptps_tui_press() applies a key (submit a bound task / quit).
 * gptps_tui_run() is the blocking live loop and the only part that touches the
 * terminal (ANSI escapes; Windows VT enabled); it self-skips without a TTY.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "gptps_tui.h"

#include <stddef.h>   /* offsetof: the appended fields of option and dead-letter structs */
#include <stdlib.h>
#include <string.h>
#include <locale.h>   /* the decimal point, for numbers shown as they are typed */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

/* ---- portable mutex + tty + terminal control ---- */
#if defined(_WIN32)
#  include <windows.h>
#  include <io.h>
typedef CRITICAL_SECTION tui_mutex;
static void mu_init(tui_mutex *m)    { InitializeCriticalSection(m); }
static void mu_lock(tui_mutex *m)    { EnterCriticalSection(m); }
static void mu_unlock(tui_mutex *m)  { LeaveCriticalSection(m); }
static void mu_destroy(tui_mutex *m) { DeleteCriticalSection(m); }
static int  fd_is_tty(FILE *f)       { return _isatty(_fileno(f)); }
#else
#  include <unistd.h>
#  include <termios.h>
#  include <sys/select.h>
#  include <sys/ioctl.h>
#  include <pthread.h>
typedef pthread_mutex_t tui_mutex;
static void mu_init(tui_mutex *m)    { pthread_mutex_init(m, NULL); }
static void mu_lock(tui_mutex *m)    { pthread_mutex_lock(m); }
static void mu_unlock(tui_mutex *m)  { pthread_mutex_unlock(m); }
static void mu_destroy(tui_mutex *m) { pthread_mutex_destroy(m); }
static int  fd_is_tty(FILE *f)       { return isatty(fileno(f)); }
#endif

#define TUI_MAX_TASKS 64

/* panes: 0 dashboard, 1 settings editor, 2 help, 3 tasks, 4 task detail, 5 dead-letter */
enum { TUI_PANE_DASH = 0, TUI_PANE_SETTINGS, TUI_PANE_HELP, TUI_PANE_TASKS, TUI_PANE_DETAIL, TUI_PANE_DL };
/* active prompt / confirm in the tasks pane (0 = none) */
enum { TUI_PK_NONE = 0, TUI_PK_CONFIRM_DELETE, TUI_PK_CLONE_NAME, TUI_PK_NEW_NAME, TUI_PK_NEW_ARGV };

typedef struct {
    char     name[64], label[64];
    int      hotkey;
    void    *payload;
    size_t   plen;
    int      has_opts;                 /* gptps_tui_add_task_ex: the hotkey submits with these */
    gptps_submit_options opts;
    unsigned started, finished, failed, retried, dead, dropped;
    uint64_t lat_sum_ms, lat_max_ms;   /* queue->finish latency accumulators */
    unsigned lat_n;
    /* The highest mem.peak an attempt reported (docs/MEASUREMENTS.md), with its method:
     * values measured differently are not compared, so a different method replaces the
     * column's value and its method rather than joining them. */
    uint64_t mem_peak;
    char     mem_method[40];
} tui_task;

typedef struct { uint64_t ts; int kind; char name[64]; uint64_t handle; gptps_status status;
                 char why[96]; } tui_event;   /* why a FAILED failed: its exit and its stderr's last line */

/* ---- text that is not ours: a job's stderr, a value typed or configured ----------
 *
 * Everything the dashboard prints that it did not write itself goes through
 * tui_clean, because the operator's terminal executes what it is sent: an ESC starts a
 * control sequence (clear the screen, retitle the window, recolour, worse), and so do
 * BEL, the other C0 bytes, DEL and the C1 controls - U+0080..U+009F, which a terminal
 * also honours when they arrive UTF-8 encoded (\xc2\x9b is CSI). So the text is
 * decoded as strict UTF-8 and only printable characters survive:
 *   - every continuation byte is checked to be 10xxxxxx: the old filter checked only a
 *     lead byte and then copied the next 1-3 bytes blind, so "\xc3\x1b[2J" carried
 *     a raw ESC through;
 *   - overlong forms (C0, C1, E0 80..9F, F0 80..8F), surrogates (ED A0..BF), code
 *     points past U+10FFFF and leads F5..FF are invalid;
 *   - C0 controls and DEL become a space (a tab is still a gap); an invalid byte, a
 *     C1 control, and the invisible formatting characters that reorder or hide text
 *     (zero-width, bidi embeddings and isolates, line/paragraph separators, BOM)
 *     become '?'. An invalid sequence costs its first byte only, so the bytes after it
 *     are judged on their own - an ESC after a stray lead byte is a space too.
 * The result is valid UTF-8, never cut inside a character, and also measured in
 * terminal columns (u8_width, an approximation of wcwidth good for the common wide
 * and combining ranges), so a line can be fitted to the screen. */

/* Display columns of code point cp: 0 for a combining mark, 2 for East Asian wide and
 * full-width forms and emoji, else 1. */
static int u8_width(uint32_t cp)
{
    if ((cp >= 0x0300 && cp <= 0x036f) || (cp >= 0x1ab0 && cp <= 0x1aff) || (cp >= 0x1dc0 && cp <= 0x1dff) ||
        (cp >= 0x20d0 && cp <= 0x20ff) || (cp >= 0xfe20 && cp <= 0xfe2f)) return 0;
    if ((cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0xa4cf && cp != 0x303f) ||
        (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xfe30 && cp <= 0xfe4f) ||
        (cp >= 0xff00 && cp <= 0xff60) || (cp >= 0xffe0 && cp <= 0xffe6) || (cp >= 0x1f300 && cp <= 0x1f64f) ||
        (cp >= 0x1f900 && cp <= 0x1f9ff) || (cp >= 0x20000 && cp <= 0x3fffd)) return 2;
    return 1;
}

/* The well-formed UTF-8 character at s (n bytes available): its length, with its code
 * point in *cp; 0 if no well-formed character starts there. */
static size_t u8_decode(const unsigned char *s, size_t n, uint32_t *cp)
{
    unsigned c = s[0];
    size_t len, k;
    uint32_t v;
    unsigned char lo = 0x80, hi = 0xbf;                /* the second byte's range */
    if (c < 0x80) { *cp = c; return 1; }
    if      (c >= 0xc2 && c <= 0xdf) { len = 2; v = c & 0x1f; }
    else if (c >= 0xe0 && c <= 0xef) { len = 3; v = c & 0x0f; if (c == 0xe0) lo = 0xa0; if (c == 0xed) hi = 0x9f; }
    else if (c >= 0xf0 && c <= 0xf4) { len = 4; v = c & 0x07; if (c == 0xf0) lo = 0x90; if (c == 0xf4) hi = 0x8f; }
    else return 0;                                     /* a continuation byte, C0/C1, F5..FF */
    if (n < len || s[1] < lo || s[1] > hi) return 0;
    v = (v << 6) | (s[1] & 0x3fu);
    for (k = 2; k < len; ++k) {
        if ((s[k] & 0xc0u) != 0x80u) return 0;
        v = (v << 6) | (s[k] & 0x3fu);
    }
    *cp = v;
    return len;
}

/* Whether code point cp may be shown as it is. */
static int u8_printable(uint32_t cp)
{
    if (cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp <= 0x9f)) return 0;       /* C0, DEL, C1 */
    if ((cp >= 0x200b && cp <= 0x200f) || (cp >= 0x2028 && cp <= 0x202e) ||
        (cp >= 0x2060 && cp <= 0x206f) || cp == 0xfeff) return 0;               /* invisible / bidi */
    return 1;
}

/* Copy the printable text of s[0..n) into out (cap bytes; NUL-terminated), at most
 * maxcols terminal columns (< 0: no limit). Returns the bytes written; *cols (may be
 * NULL) gets the columns used. See above for what is kept. */
static size_t tui_clean(const char *s, size_t n, char *out, size_t cap, int maxcols, int *cols)
{
    const unsigned char *u = (const unsigned char *)s;
    size_t i = 0, pos = 0;
    int used = 0, measure = (out == NULL);   /* NULL: only count the columns */
    if (!cap && !measure) return 0;
    while (i < n) {
        uint32_t cp = 0;
        size_t len = u8_decode(u + i, n - i, &cp);
        const char *rep = NULL;
        int w;
        if (len == 0)                rep = "?", len = 1;      /* not UTF-8: this byte only */
        else if (cp < 0x20 || cp == 0x7f) rep = " ";          /* C0 / DEL: a gap */
        else if (!u8_printable(cp))  rep = "?";
        w = rep ? 1 : u8_width(cp);
        if (maxcols >= 0 && used + w > maxcols) break;
        if (measure) {
            pos += rep ? 1 : len;
        } else if (rep) {
            if (pos + 2 > cap) break;
            out[pos++] = rep[0];
        } else {
            if (pos + len + 1 > cap) break;                    /* never half a character */
            memcpy(out + pos, u + i, len); pos += len;
        }
        used += w; i += len;
    }
    if (!measure) out[pos] = 0;
    if (cols) *cols = used;
    return pos;
}

/* The number of bytes of s (already clean) that fit in `cols` columns. */
static size_t u8_fit(const char *s, int cols)
{
    const unsigned char *u = (const unsigned char *)s;
    size_t i = 0, n = strlen(s);
    int used = 0;
    while (i < n) {
        uint32_t cp = 0;
        size_t len = u8_decode(u + i, n - i, &cp);
        int w;
        if (len == 0) len = 1, cp = '?';
        w = u8_width(cp);
        if (used + w > cols) break;
        used += w; i += len;
    }
    return i;
}

/* Columns that s takes on a terminal. */
static int u8_cols(const char *s)
{
    int c = 0;
    tui_clean(s, strlen(s), NULL, 0, -1, &c);
    return c;
}

/* s cleaned into out (cap bytes) - the form every string not written here is printed
 * in: a setting's key, value, default and description, a task's name, the status. */
static const char *tui_cl(const char *s, char *out, size_t cap)
{
    tui_clean(s ? s : "", s ? strlen(s) : 0, out, cap, -1, NULL);
    return out;
}

/* s cleaned and fitted to exactly `cols` columns: cut at a character, padded with
 * spaces - a table cell (what "%-16.16s" did by bytes, which can cut a character). */
static const char *tui_cell(const char *s, char *out, size_t cap, int cols)
{
    int used = 0;
    size_t pos = tui_clean(s ? s : "", s ? strlen(s) : 0, out, cap, cols, &used);
    while (used < cols && pos + 1 < cap) { out[pos++] = ' '; ++used; }
    out[pos] = 0;
    return out;
}

/* ---- settings, read so that nothing borrowed outlives the call ----------------
 * gptps_setting_info's `key` and `desc` point into the registry, and a task
 * unregistered on another thread frees them (gptps_settings_remove_task) - while
 * this thread, which holds no lock of the engine's, is still printing or comparing
 * them. So the dashboard reads only the copies taken under the registry's lock
 * (key_copy / desc_copy, ABI 2.8) and never dereferences info.key or info.desc. A
 * key too long for its copy is shown as TUI_KEY_LONG and cannot be edited here. */
#define TUI_KEY_LONG "(a key too long to show)"
static int tui_setting_at(gptps *e, size_t i, gptps_setting_info *in)
{
    memset(in, 0, sizeof *in);
    in->struct_size = sizeof *in;
    if (gptps_settings_get_info(e, i, in) != GPTPS_OK) return 0;
    in->key = NULL; in->desc = NULL;          /* borrowed: not to be used past the call */
    return 1;
}
static const char *skey(const gptps_setting_info *in) { return in->key_copy[0] ? in->key_copy : TUI_KEY_LONG; }

/* A task type's record, the same way: its name is read from name_copy (ABI 2.8), never
 * from the borrowed `name` an unregister on another thread frees. */
static int tui_task_at(gptps *e, size_t i, gptps_task_info *out)
{
    memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    if (gptps_task_get_info(e, i, out) != GPTPS_OK) return 0;
    out->name = NULL;
    return 1;
}

/* Why an attempt failed, in one short line: "exit 3", "signal 9", "not started (error
 * 2)" or the status, then the last line of its stderr tail when it kept one (ABI 2.7 /
 * 2.8), cleaned (tui_clean). From an event or a dead letter alike. */
static void why_line(const gptps_measure *start_err, const gptps_measure *exit_code,
                     const gptps_measure *exit_sig, gptps_status status,
                     const char *tail, size_t tail_len, char *out, size_t n)
{
    size_t pos;
    out[0] = 0;
    if (n < 8) return;
    if (start_err)      pos = (size_t)snprintf(out, n, "not started (error %llu)", (unsigned long long)start_err->value);
    else if (exit_code) pos = (size_t)snprintf(out, n, "exit %llu", (unsigned long long)exit_code->value);
    else if (exit_sig)  pos = (size_t)snprintf(out, n, "signal %llu", (unsigned long long)exit_sig->value);
    else                pos = (size_t)snprintf(out, n, "%s", gptps_strerror(status));
    if (pos >= n) pos = n - 1;
    if (tail && tail_len) {
        size_t end = tail_len, start;
        while (end && (tail[end - 1] == '\n' || tail[end - 1] == '\r' || tail[end - 1] == ' ')) --end;
        start = end;
        while (start && tail[start - 1] != '\n') --start;            /* its last line */
        if (end > start && pos + 6 < n) {
            pos += (size_t)snprintf(out + pos, n - pos, " \xc2\xb7 ");
            if (pos >= n) pos = n - 1;
            tui_clean(tail + start, end - start, out + pos, n - pos, -1, NULL);
        }
    }
}

static void tui_why(const gptps_event *ev, char *out, size_t n)
{
    int has_tail = ev->struct_size >= offsetof(gptps_event, stderr_tail_len) + sizeof ev->stderr_tail_len;
    out[0] = 0;
    if (ev->kind != GPTPS_EV_FAILED) return;
    why_line(gptps_event_measure(ev, GPTPS_M_START_ERROR), gptps_event_measure(ev, GPTPS_M_EXIT_CODE),
             gptps_event_measure(ev, GPTPS_M_EXIT_SIGNAL), ev->status,
             has_tail ? ev->stderr_tail : NULL, has_tail ? ev->stderr_tail_len : 0, out, n);
}

/* A dead letter's measurement by name (ABI 2.8), NULL when it has none. */
static const gptps_measure *dl_measure(const gptps_dead_letter *dl, const char *name)
{
    size_t i;
    if (dl->struct_size < offsetof(gptps_dead_letter, n_measures) + sizeof dl->n_measures || !dl->measures) return NULL;
    for (i = 0; i < dl->n_measures; ++i)
        if (dl->measures[i].name && strcmp(dl->measures[i].name, name) == 0) return &dl->measures[i];
    return NULL;
}

/* ---- dead letters the dashboard holds (see "dead-letter pane") ---- */
#define TUI_HELD_MAX 4096
typedef struct {
    char                *task;
    void                *payload;
    size_t               plen;
    gptps_submit_options opts;     /* as it was submitted; flags 0: the type's defaults */
    gptps_handle         handle;   /* its original handle */
    gptps_status         status;   /* why it was dead-lettered */
    gptps_status         refused;  /* GPTPS_OK, or what the last re-submit was refused with */
    char                 why[96];
} tui_held;

struct gptps_tui {
    gptps           *e;
    gptps_tui_config cfg;
    tui_mutex        mu;
    uint64_t         start_ms;
    unsigned         q, s, fin, fail, retr, dead, drop, peak;
    /* In-flight is COUNTED, not derived as started - finished - failed. A cancel that
     * lands while the item is still queued emits a terminal FAILED with no preceding
     * STARTED, so the subtraction wrapped to 4294967295 on unsigned - and since peak
     * is a running maximum, that wrap poisoned the gauge's divisor for the life of
     * the process, leaving the bar empty forever. Counted with a floor, the unpaired
     * terminal event is absorbed instead. */
    unsigned         inflight;
    tui_task         tasks[TUI_MAX_TASKS];
    int              ntasks;
    tui_event       *recent;
    int              rcap, rn, rhead;
    /* handle -> queued-ts ring for latency. `done` marks a TOMBSTONE: a FINISHED that
     * arrived before its own QUEUED, holding the finish time until the late QUEUED
     * turns the pair into a sample. See tui_on_event. */
    struct { gptps_handle h; uint64_t ts; int done; } *lat;
    int              lat_cap, lat_head;
    int              lat_tomb;      /* tombstones live in the ring (see tui_on_event) */
    int              scroll;        /* recent-log scroll offset (lines back) */
    int              kpi;           /* effective gptps_tui_kpi (never DEFAULT) */
    int              mode;          /* effective gptps_tui_mode */
    int              dirty;         /* state changed since last paint (for ON_DEMAND) */
    /* Read by tui_on_event WITHOUT the lock, so that KPI OFF can skip acquiring it at
     * all - taking a mutex to discover you have nothing to do is the entire cost this
     * tier exists to remove. `volatile` rather than an atomic because an add-on has no
     * atomic to reach for in the drop-in build: the amalgamation ships gptps.c and
     * gptps.h only, so the HAL's gptps_flag is not declared there. The race is benign
     * by construction - one writer (gptps_tui_set_kpi, under the lock), a single word,
     * and a stale read costs one extra event processed or skipped at the instant the
     * tier changes, which breaks no invariant the dashboard keeps. */
    volatile int     off;           /* KPI OFF: leave the event path entirely */
    /* Settings pane state - touched only by the run/press/render thread (not the
     * observer), so it needs no lock. */
    int              pane;          /* TUI_PANE_* */
    int              sel;           /* selected setting index */
    int              editing, editlen;
    char             editbuf[GPTPS_SETTINGS_VALUE_MAX];
    /* task-management panes (tasks / detail / dead-letter) - run-thread-local */
    int              task_sel;      /* selection in the tasks pane */
    int              detail_sel;    /* selection in the per-task detail pane */
    char             detail_task[128];  /* task whose settings the detail pane shows */
    int              prompt_kind;   /* TUI_PK_*: active text prompt / confirm */
    char             prompt_name[128];  /* stash carried across prompt steps (clone/new/delete target) */
    char             edit_key[320]; /* settings key being edited (settings or detail pane) */
    unsigned char    edit_part[4];  /* the bytes so far of a character still being typed */
    int              edit_partn;    /*   how many */
    int              edit_need;     /*   continuation bytes it still needs (0: none pending) */
    /* Dead letters drained into the dashboard's keeping (dead-letter pane) - run-
     * thread-local like the rest of the pane state. */
    tui_held        *held;
    size_t           nheld, held_cap;
    int              dl_sel;
    unsigned long    held_lost;     /* could not be kept (no memory, or past TUI_HELD_MAX) */
    char             status[320];   /* last save/validation message + dashboard action toast */
    uint64_t         status_ms;     /* when the toast was set (it fades after a few seconds) */
    int              cols, rows;    /* terminal size: run() refreshes it; 80x24 headless default */
    int              quit, show_tasks, show_recent;
    int              raw_active;
#if !defined(_WIN32)
    struct termios   saved_termios;
#else
    DWORD            saved_out_mode; int modes_saved;
    UINT             saved_cp;      /* console output codepage to restore (0 = none saved) */
#endif
};

/* ---- state (under mu) ---- */
static tui_task *task_for(gptps_tui *t, const char *name)
{
    int i;
    for (i = 0; i < t->ntasks; ++i) if (strcmp(t->tasks[i].name, name) == 0) return &t->tasks[i];
    if (t->ntasks < TUI_MAX_TASKS) {
        tui_task *tk = &t->tasks[t->ntasks++];
        memset(tk, 0, sizeof *tk);
        strncpy(tk->name, name, sizeof tk->name - 1);
        strncpy(tk->label, name, sizeof tk->label - 1);
        return tk;
    }
    return NULL;
}

static void tui_on_event(const gptps_event *ev, void *ud)
{
    gptps_tui *t = (gptps_tui *)ud;
    tui_task *tk;
    if (t->off) return;              /* KPI OFF: before the lock, deliberately - see the field */
    /* A running job's samples (measure.sample_ms) are not part of any item's life: they
     * would only flood the log. The peak column reads the attempts' own measurements. */
    if (ev->kind == GPTPS_EV_SAMPLE) return;
    mu_lock(&t->mu);
    switch (ev->kind) {
        case GPTPS_EV_QUEUED:        t->q++;    break;
        case GPTPS_EV_STARTED:       t->s++;    t->inflight++; break;
        /* the floor is what a terminal event with no matching STARTED hits - a cancel
         * of a still-queued item, or a forced task removal (see the field comment) */
        case GPTPS_EV_FINISHED:      t->fin++;  if (t->inflight) t->inflight--; break;
        case GPTPS_EV_FAILED:        t->fail++; if (t->inflight) t->inflight--; break;
        case GPTPS_EV_RETRIED:       t->retr++; break;
        case GPTPS_EV_DEAD_LETTERED: t->dead++; break;
        /* a terminal failure like a dead letter, just not retained (on_failure =
         * drop): it was missing from every count and showed as "?" in the log */
        case GPTPS_EV_DROPPED:       t->drop++; break;
        default: break;
    }
    if (t->inflight > t->peak) t->peak = t->inflight;
    t->dirty = 1;                                /* something changed (for ON_DEMAND repaint) */

    /* MINIMAL stops here - counts only, ~no per-event cost. NORMAL+ does the
     * per-task table + recent log; latency tracking runs only when its ring exists
     * (FULL), and that ring is freed when the KPI level is lowered. */
    if (t->kpi < GPTPS_TUI_KPI_NORMAL) { mu_unlock(&t->mu); return; }

    /* Latency, and why this is not simply "record at QUEUED, resolve at FINISHED".
     *
     * Event order is NOT guaranteed across threads (see the EVENTS block in gptps.h):
     * QUEUED is emitted by the submitting thread after the engine lock is dropped, and
     * the dispatcher was signalled while it was still held, so a task that runs in under
     * a microsecond reports FINISHED before its own QUEUED callback has run. Resolving
     * only at FINISHED silently loses every one of those, and the loss is not neutral -
     * the items that win that race are by construction the ones that waited least, so
     * dropping them drags the reported average UP. gptps_stats meets the same inversion
     * and answers it the same way, with a tombstone the late event clears.
     *
     * So: a FINISHED with no entry leaves one (done=1, holding the finish time), and the
     * QUEUED that arrives afterwards completes the sample instead of starting a new one.
     *
     * This buys back the sub-millisecond tail; it does NOT make avg ms exact. The larger
     * distortion in the same column is the ring being too small for the in-flight depth:
     * an entry lives from QUEUED to FINISHED, so a burst that backs up past lat_cap items
     * overwrites live entries before they resolve, and those samples are lost too - with
     * the same upward skew and no late QUEUED involved. That one is the caller's to size
     * (cfg.latency_window, or tui.latency_window at runtime), which is why it is a knob
     * and not a guess. */
    if (t->lat && ev->kind == GPTPS_EV_QUEUED) {
        int j, slot = -1;
        if (t->lat_tomb) {
            for (j = 0; j < t->lat_cap; ++j)           /* a terminal event beat us here */
                if (t->lat[j].h == ev->handle && t->lat[j].done) { slot = j; break; }
        }
        if (slot >= 0) {
            tui_task *lk = task_for(t, ev->task_name);
            if (lk) {
                uint64_t l = (t->lat[slot].ts >= ev->ts_ms) ? t->lat[slot].ts - ev->ts_ms : 0;
                lk->lat_sum_ms += l; lk->lat_n++;
                if (l > lk->lat_max_ms) lk->lat_max_ms = l;
            }
            t->lat[slot].h = 0; t->lat[slot].done = 0; --t->lat_tomb;
        } else {                                      /* the ordinary path: start the clock */
            if (t->lat[t->lat_head].done && t->lat[t->lat_head].h) --t->lat_tomb;  /* evicting one */
            t->lat[t->lat_head].h = ev->handle; t->lat[t->lat_head].ts = ev->ts_ms;
            t->lat[t->lat_head].done = 0;
            t->lat_head = (t->lat_head + 1) % t->lat_cap;
        }
    }

    if ((tk = task_for(t, ev->task_name)) != NULL) {
        const gptps_measure *pm = gptps_event_measure(ev, GPTPS_M_MEM_PEAK);
        if (pm && pm->method) {
            if (strncmp(tk->mem_method, pm->method, sizeof tk->mem_method - 1) != 0) {
                strncpy(tk->mem_method, pm->method, sizeof tk->mem_method - 1);
                tk->mem_method[sizeof tk->mem_method - 1] = 0;
                tk->mem_peak = pm->value;
            } else if (pm->value > tk->mem_peak) {
                tk->mem_peak = pm->value;
            }
        }
        switch (ev->kind) {
            case GPTPS_EV_STARTED:       tk->started++;  break;
            case GPTPS_EV_FINISHED:
                tk->finished++;
                if (t->lat) {                             /* FULL: resolve this handle's latency */
                    int j, found = 0;
                    for (j = 0; j < t->lat_cap; ++j)
                        if (t->lat[j].h == ev->handle && !t->lat[j].done) {
                            uint64_t l = (ev->ts_ms >= t->lat[j].ts) ? ev->ts_ms - t->lat[j].ts : 0;
                            tk->lat_sum_ms += l; tk->lat_n++;
                            if (l > tk->lat_max_ms) tk->lat_max_ms = l;
                            t->lat[j].h = 0;              /* consume */
                            found = 1;
                            break;
                        }
                    /* No entry: this FINISHED outran its own QUEUED. Leave the finish time
                     * behind so the late QUEUED can complete the sample, instead of losing
                     * it - these are the FASTEST items, so dropping them skews avg ms up. */
                    if (!found) {
                        if (t->lat[t->lat_head].done && t->lat[t->lat_head].h) --t->lat_tomb;
                        t->lat[t->lat_head].h = ev->handle;
                        t->lat[t->lat_head].ts = ev->ts_ms;
                        t->lat[t->lat_head].done = 1;
                        t->lat_head = (t->lat_head + 1) % t->lat_cap;
                        ++t->lat_tomb;
                    }
                }
                break;
            case GPTPS_EV_FAILED:        tk->failed++;   break;
            case GPTPS_EV_RETRIED:       tk->retried++;  break;
            case GPTPS_EV_DEAD_LETTERED: tk->dead++;     break;
            case GPTPS_EV_DROPPED:       tk->dropped++;  break;
            default: break;
        }
    }
    if (t->rcap > 0) {
        tui_event *re = &t->recent[t->rhead];
        re->ts = ev->ts_ms; re->kind = ev->kind; re->handle = ev->handle; re->status = ev->status;
        strncpy(re->name, ev->task_name, sizeof re->name - 1); re->name[sizeof re->name - 1] = 0;
        tui_why(ev, re->why, sizeof re->why);
        t->rhead = (t->rhead + 1) % t->rcap;
        if (t->rn < t->rcap) t->rn++;
    }
    mu_unlock(&t->mu);
}

static const char *kpi_str(int k)
{ return k == GPTPS_TUI_KPI_OFF ? "off" : k == GPTPS_TUI_KPI_MINIMAL ? "minimal" : k == GPTPS_TUI_KPI_NORMAL ? "normal" : "full"; }
static const char *mode_str(int m)
{ return m == GPTPS_TUI_CONTINUOUS ? "realtime" : m == GPTPS_TUI_ON_DEMAND ? "on-demand" : "paused"; }

static const char *kind_str(int k)
{
    switch (k) {
        case GPTPS_EV_QUEUED:        return "QUEUED";
        case GPTPS_EV_STARTED:       return "STARTED";
        case GPTPS_EV_FINISHED:      return "FINISHED";
        case GPTPS_EV_FAILED:        return "FAILED";
        case GPTPS_EV_RETRIED:       return "RETRIED";
        case GPTPS_EV_DEAD_LETTERED: return "DEAD";
        case GPTPS_EV_DROPPED:       return "DROPPED";
        case GPTPS_EV_SAMPLE:        return "SAMPLE";
        default:                     return "?";
    }
}

/* bounded formatted append into buf at pos; returns new pos (clamped to cap) */
static size_t appendf(char *buf, size_t cap, size_t pos, const char *fmt, ...)
{
    va_list ap; int n;
    if (pos >= cap) return pos;
    va_start(ap, fmt);
    n = vsnprintf(buf + pos, cap - pos, fmt, ap);
    va_end(ap);
    if (n < 0) return pos;
    pos += (size_t)n;
    return pos > cap ? cap : pos;
}

/* Arrow glyphs for the key legends: real arrows when unicode is on, ASCII words
 * otherwise. Arrows reuse j/k/Enter/Esc, so both still work; the legend just
 * showcases the arrows. */
static const char *a_ud(const gptps_tui *t) { return t->cfg.unicode ? "\xe2\x86\x91\xe2\x86\x93" : "Up/Dn"; }
static const char *a_lt(const gptps_tui *t) { return t->cfg.unicode ? "\xe2\x86\x90"             : "Left";  }
static const char *a_rt(const gptps_tui *t) { return t->cfg.unicode ? "\xe2\x86\x92"             : "Right"; }

/* terminal width clamped to a sane drawing range */
static int draw_width(const gptps_tui *t) { int W = t->cols > 0 ? t->cols : 80; if (W < 24) W = 24; if (W > 200) W = 200; return W; }

/* A number with '.' for its decimal point whatever the locale: the form a value is
 * typed in, and the form a config file holds. */
static void fmt_num(char *b, size_t cap, double v)
{
    const char *dp = localeconv()->decimal_point;
    char *at;
    int p;
    if (v > -1e15 && v < 1e15 && (double)(long long)v == v) {
        snprintf(b, cap, "%.0f", v);       /* a whole number as one: 10, not 1e+01 */
    } else {
        for (p = 1; p <= 17; ++p) {        /* the shortest that reads back: 0.1, not 0.10000000000000001 */
            snprintf(b, cap, "%.*g", p, v);
            if (strtod(b, NULL) == v) break;
        }
    }
    if (dp && *dp && strcmp(dp, ".") != 0 && (at = strstr(b, dp)) != NULL) {
        size_t dl = strlen(dp);
        *at = '.';
        memmove(at + 1, at + dl, strlen(at + dl) + 1);
    }
}

/* What a setting takes, in words: "whole number, 0 to 65536", "one of: drop requeue". */
static void setting_kind(const gptps_setting_info *in, char *out, size_t cap)
{
    char range[96];
    range[0] = 0;
    if (in->has_range) {
        char a[40], b[40];
        fmt_num(a, sizeof a, in->min);
        fmt_num(b, sizeof b, in->max);
        snprintf(range, sizeof range, ", %s to %s", a, b);
    }
    switch (in->type) {
        case GPTPS_SETTING_INT:    snprintf(out, cap, "whole number%s", range); break;
        case GPTPS_SETTING_UINT:   snprintf(out, cap, "whole number%s", *range ? range : ", 0 or more"); break;
        case GPTPS_SETTING_DOUBLE: snprintf(out, cap, "number%s", range); break;
        case GPTPS_SETTING_BOOL:   snprintf(out, cap, "true or false"); break;
        case GPTPS_SETTING_STRING: snprintf(out, cap, "text"); break;
        case GPTPS_SETTING_ENUM: default: {
            const char *const *c;
            size_t k = (size_t)snprintf(out, cap, "one of:");
            for (c = in->choices; c && *c && k < cap; ++c) k += (size_t)snprintf(out + k, cap - k, " %s", *c);
            break;
        }
    }
}

/* " (46.9 GiB)" after a *_bytes value of 1 KiB or more; "" otherwise. */
static void bytes_hint(const char *key, const char *v, char *out, size_t cap)
{
    static const char *const U[] = { "KiB", "MiB", "GiB", "TiB", "PiB", "EiB" };
    size_t kl = strlen(key);
    double x;
    int u = -1;
    out[0] = 0;
    if (kl < 6 || strcmp(key + kl - 6, "_bytes") != 0) return;
    x = strtod(v, NULL);
    while (x >= 1024.0 && u < 5) { x /= 1024.0; ++u; }
    if (u >= 0) snprintf(out, cap, " (%.1f %s)", x, U[u]);
}

/* The selected setting, explained: its key, what it does (word-wrapped to the
 * width, three lines at most), what it takes, its default, and when it applies. */
static size_t setting_details(gptps_tui *t, char *buf, size_t cap, size_t pos, const gptps_setting_info *in)
{
    int color = t->cfg.color, W = draw_width(t) - 4, lines = 0;
    const char *B = color ? "\x1b[1m" : "", *D = color ? "\x1b[2m" : "", *X = color ? "\x1b[0m" : "";
    char desc[GPTPS_SETTINGS_VALUE_MAX * 2], key[GPTPS_SETTINGS_VALUE_MAX * 2], kind[320], ckind[640],
         dv[GPTPS_SETTINGS_VALUE_MAX * 2];
    const char *p = tui_cl(in->desc_copy, desc, sizeof desc);
    if (W < 20) W = 20;
    pos = appendf(buf, cap, pos, "\n  %s%s%s\n", B, tui_cl(skey(in), key, sizeof key), X);
    while (*p && lines < 3) {                       /* word wrap, by columns, whole characters */
        int n = (int)u8_fit(p, W);
        if (n == 0) n = 1;                          /* a character wider than the line */
        if (p[n]) {
            int sp = n;
            while (sp > 0 && p[sp] != ' ') --sp;
            if (sp > 0) n = sp;
            if (lines == 2) {                       /* the last line: cut, and say so */
                pos = appendf(buf, cap, pos, "  %.*s...\n", n, p);
                break;
            }
        }
        pos = appendf(buf, cap, pos, "  %.*s\n", n, p);
        p += n;
        while (*p == ' ') ++p;
        ++lines;
    }
    setting_kind(in, kind, sizeof kind);
    tui_cl(kind, ckind, sizeof ckind);              /* an enum's choices are the host's text */
    pos = appendf(buf, cap, pos, "  %s%s \xc2\xb7 default %s \xc2\xb7 %s%s\n", D, ckind,
                  in->defval[0] ? tui_cl(in->defval, dv, sizeof dv) : "(empty)",
                  in->hot ? "applies at once" : "applies at the next start (save with w, then restart)", X);
    return pos;
}

/* One row of a settings list: "> key = value (46.9 GiB) (restart)", the key padded
 * to kw columns. Key and value are cleaned: a STRING setting can hold any byte the
 * host or a config file put there (TOML's \u001B is an ESC). */
static size_t setting_row(char *buf, size_t cap, size_t pos, const gptps_setting_info *in, const char *key,
                          int kw, int selected, const char *INV, const char *X)
{
    char k[GPTPS_SETTINGS_VALUE_MAX * 2], v[GPTPS_SETTINGS_VALUE_MAX * 2], hint[32];
    int pad;
    tui_cl(key, k, sizeof k);
    tui_cl(in->value, v, sizeof v);
    bytes_hint(k, in->value, hint, sizeof hint);
    pad = kw - u8_cols(k);
    if (pad < 0) pad = 0;
    return appendf(buf, cap, pos, "%s%s%s%*s = %s%s%s %s%s\n", selected ? INV : "", selected ? "> " : "  ",
                   k, pad, "", v, hint, X, in->hot ? "" : "(restart)", X);
}

/* The status line of a pane, cleaned: set_status puts the committed value in it, and
 * the task panes put task names - text the dashboard did not write. */
static size_t status_line(gptps_tui *t, char *buf, size_t cap, size_t pos, const char *lead, const char *D, const char *X)
{
    char shown[sizeof t->status * 2];
    if (!t->status[0]) return pos;
    return appendf(buf, cap, pos, "%s%s%s%s\n", lead, D, tui_cl(t->status, shown, sizeof shown), X);
}

/* The rows a settings list may use: what the terminal has, less the title, the
 * details, the edit and status lines and the key legend. */
static int list_rows(const gptps_tui *t)
{
    int r = (t->rows > 0 ? t->rows : 24) - 13;
    return r < 3 ? 3 : r;
}

static size_t edit_line(gptps_tui *t, char *buf, size_t cap, size_t pos, const char *B, const char *D, const char *X);

/* Settings pane (no t->mu: reads only run-thread-local pane state + the registry,
 * whose get_info takes its own locks - holding t->mu here would deadlock the tui
 * read_fns). The list scrolls to keep the selection in view; the selected
 * setting is explained under it. */
static size_t render_settings(gptps_tui *t, char *buf, size_t cap)
{
    size_t pos = 0, i, n;
    int color = t->cfg.color, kw = 20, rows = list_rows(t), first;
    const char *B = color ? "\x1b[1m" : "", *INV = color ? "\x1b[7m" : "", *D = color ? "\x1b[2m" : "", *X = color ? "\x1b[0m" : "";
    gptps_setting_info info, sel;
    int have_sel = 0;
    memset(&sel, 0, sizeof sel);
    n = gptps_settings_count(t->e);
    if (t->sel < 0) t->sel = 0;
    if (n && (size_t)t->sel >= n) t->sel = (int)n - 1;
    for (i = 0; i < n; ++i)                         /* the key column fits the longest key */
        if (tui_setting_at(t->e, i, &info) && u8_cols(skey(&info)) > kw) kw = u8_cols(skey(&info));
    if (kw > 44) kw = 44;
    first = t->sel - rows / 2;
    if (first > (int)n - rows) first = (int)n - rows;
    if (first < 0) first = 0;
    pos = appendf(buf, cap, pos, "%sGPTPS \xc2\xb7 %s \xc2\xb7 settings%s  %s(%d of %lu)%s\n\n", B, t->cfg.title, X,
                  D, n ? t->sel + 1 : 0, (unsigned long)n, X);
    for (i = (size_t)first; i < n && (int)(i - (size_t)first) < rows; ++i) {
        if (!tui_setting_at(t->e, i, &info)) continue;
        if (i == (size_t)t->sel) { sel = info; have_sel = 1; }
        pos = setting_row(buf, cap, pos, &info, skey(&info), kw, i == (size_t)t->sel, INV, X);
    }
    if (have_sel) pos = setting_details(t, buf, cap, pos, &sel);
    if (t->editing) pos = edit_line(t, buf, cap, pos, B, D, X);
    pos = status_line(t, buf, cap, pos, "", D, X);
    pos = appendf(buf, cap, pos, "\n%skeys:%s %s move  %s edit  %s back  w save  q quit\n", D, X, a_ud(t), a_rt(t), a_lt(t));
    if (pos >= cap) pos = cap - 1;
    buf[pos] = 0;
    return pos;
}

/* After a set: what happened, in the status line. */
static void set_status(gptps_tui *t, const char *key, const char *value, gptps_status st, const char *why)
{
    if (st == GPTPS_OK) {
        gptps_setting_info info;
        size_t i, n = gptps_settings_count(t->e);
        int hot = 1;
        for (i = 0; i < n; ++i)
            if (tui_setting_at(t->e, i, &info) && !strcmp(info.key_copy, key)) { hot = info.hot; break; }
        snprintf(t->status, sizeof t->status, "set %.80s = %.80s%s", key, value,
                 hot ? "" : " - applies at the next start; w saves it");
    } else {
        snprintf(t->status, sizeof t->status, "not set: %.240s", why && *why ? why : gptps_strerror(st));
    }
    t->status_ms = gptps_now_ms(NULL);
}

/* After w: where it went, or why it did not. */
static void save_status(gptps_tui *t, gptps_status st)
{
    const char *where = t->cfg.settings_path ? t->cfg.settings_path : "the config file";
    switch (st) {
        case GPTPS_OK:       snprintf(t->status, sizeof t->status, "saved your changes to %.200s", where); break;
        case GPTPS_E_INVAL:  snprintf(t->status, sizeof t->status, "nowhere to save: the engine has no config file "
                                      "(set gptps_tui_config.settings_path)"); break;
        case GPTPS_E_CONFIG: snprintf(t->status, sizeof t->status, "not saved: %.160s has an error (see the log); "
                                      "it was left as it is", where); break;
        default:             snprintf(t->status, sizeof t->status, "not saved: %s", gptps_strerror(st)); break;
    }
    t->status_ms = gptps_now_ms(NULL);
}

/* set a transient action message ("toast") shown briefly on the dashboard */
static void toast(gptps_tui *t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t->status, sizeof t->status, fmt, ap);
    va_end(ap);
    t->status_ms = gptps_now_ms(NULL);
}


/* full-width inverse title bar (color mode): left title, right-aligned stats */
static size_t titlebar(char *buf, size_t cap, size_t pos, gptps_tui *t, int W, double up, double rate)
{
    char line[256], stats[64];
    int len, sl;
    if (W > (int)sizeof line - 1) W = (int)sizeof line - 1;
    len = snprintf(line, sizeof line, " GPTPS  %s", t->cfg.title);
    if (len < 0) len = 0;
    sl = snprintf(stats, sizeof stats, "up %.1fs  %.1f done/s ", up, rate);
    if (sl > 0 && len + 2 + sl <= W) { while (len < W - sl) line[len++] = ' '; memcpy(line + len, stats, (size_t)sl); len += sl; }
    if (len > W) len = W;
    while (len < W) line[len++] = ' ';
    line[len] = 0;
    return appendf(buf, cap, pos, "\x1b[7m\x1b[1m%s\x1b[0m\n", line);
}

/* horizontal rule across the width (color mode) */
static size_t rule(char *buf, size_t cap, size_t pos, int W, int uni)
{
    int i;
    pos = appendf(buf, cap, pos, "\x1b[2m");
    for (i = 0; i < W; ++i) pos = appendf(buf, cap, pos, "%s", uni ? "\xe2\x94\x80" : "-");
    return appendf(buf, cap, pos, "\x1b[0m\n");
}

/* Help overlay: every key + what it does. Pure; reads only cfg. */
static size_t render_help(gptps_tui *t, char *buf, size_t cap)
{
    size_t pos = 0;
    int color = t->cfg.color;
    const char *B = color ? "\x1b[1m" : "", *D = color ? "\x1b[2m" : "", *X = color ? "\x1b[0m" : "";
    pos = appendf(buf, cap, pos, "%sGPTPS \xc2\xb7 %s \xc2\xb7 help%s\n\n", B, t->cfg.title, X);
    pos = appendf(buf, cap, pos, "%sDashboard%s\n", B, X);
    pos = appendf(buf, cap, pos, "  %-9s submit its task\n", "<hotkey>");
    pos = appendf(buf, cap, pos, "  %-9s scroll the event log (older / newer)\n", "k / j");
    pos = appendf(buf, cap, pos, "  %-9s open the settings editor\n", "s");
    pos = appendf(buf, cap, pos, "  %-9s open the task manager\n", "t");
    pos = appendf(buf, cap, pos, "  %-9s open the dead-letter view\n", "l");
    pos = appendf(buf, cap, pos, "  %-9s cycle KPI detail (minimal/normal/full)\n", "m");
    pos = appendf(buf, cap, pos, "  %-9s pause / resume live updates\n", "p");
    pos = appendf(buf, cap, pos, "  %-9s toggle this help\n", "?");
    pos = appendf(buf, cap, pos, "  %-9s quit\n", "q / Esc");
    pos = appendf(buf, cap, pos, "  %s%-9s Up/Down move, Right select, Left back (= k / j / Enter / Esc)%s\n\n", D, "Arrows", X);
    pos = appendf(buf, cap, pos, "%sSettings editor%s\n", B, X);
    pos = appendf(buf, cap, pos, "  %-9s move; the selected setting is explained under the list\n", "k / j");
    pos = appendf(buf, cap, pos, "  %-9s edit its value (Enter applies, Esc cancels)\n", "Enter");
    pos = appendf(buf, cap, pos, "  %-9s save the values you changed into the config file, in place\n", "w");
    pos = appendf(buf, cap, pos, "  %-9s %s(the file keeps its comments and the lines you did not change)%s\n", "", D, X);
    pos = appendf(buf, cap, pos, "  %-9s back to dashboard\n", "s / Esc");
    pos = appendf(buf, cap, pos, "\n%sTask manager%s\n", B, X);
    pos = appendf(buf, cap, pos, "  %-9s inspect a task's settings\n", "Enter");
    pos = appendf(buf, cap, pos, "  %-9s pause/resume \xc2\xb7 clone \xc2\xb7 new \xc2\xb7 delete\n", "a/c/n/d");
    pos = appendf(buf, cap, pos, "\n%sDead letters%s\n", B, X);
    pos = appendf(buf, cap, pos, "  %-9s load the engine's here (takes them), to see each one and why\n", "g");
    pos = appendf(buf, cap, pos, "  %-9s re-submit all \xc2\xb7 this one \xc2\xb7 discard this one \xc2\xb7 all\n", "r/e/x/D");
    pos = appendf(buf, cap, pos, "  %-9s %s(a re-submit the engine refuses stays here, with the reason)%s\n", "", D, X);
    pos = appendf(buf, cap, pos, "\n%spress any key to return%s\n", D, X);
    if (pos >= cap) pos = cap - 1;
    buf[pos] = 0;
    return pos;
}

static const char *exec_str(int k)
{ return k == GPTPS_EXEC_OOP ? "oop" : k == GPTPS_EXEC_PROGRAM ? "program" : "inproc"; }

/* bounded name copy (avoids -Wformat-truncation and silent over-read) */
static void copy_name(char *dst, size_t cap, const char *src)
{ if (cap) snprintf(dst, cap, "%.*s", (int)(cap - 1), src ? src : ""); }

/* fill *out for the registered task named `name`; 1 if found */
static int find_task_by_name(gptps_tui *t, const char *name, gptps_task_info *out)
{
    size_t i, n = gptps_task_count(t->e);
    for (i = 0; i < n; ++i) {
        if (tui_task_at(t->e, i, out) && strcmp(out->name_copy, name) == 0) return 1;
    }
    return 0;
}

/* count settings whose key begins with `prefix` */
static int count_prefixed(gptps_tui *t, const char *prefix)
{
    size_t i, n = gptps_settings_count(t->e), plen = strlen(prefix);
    int c = 0;
    for (i = 0; i < n; ++i) {
        gptps_setting_info info;
        if (tui_setting_at(t->e, i, &info) && strncmp(info.key_copy, prefix, plen) == 0) ++c;
    }
    return c;
}

/* fill *out with the nth (0-based) setting whose key begins with `prefix`; 1 ok */
static int nth_prefixed(gptps_tui *t, const char *prefix, int nth, gptps_setting_info *out)
{
    size_t i, n = gptps_settings_count(t->e), plen = strlen(prefix);
    int c = 0;
    for (i = 0; i < n; ++i) {
        if (tui_setting_at(t->e, i, out) && strncmp(out->key_copy, prefix, plen) == 0) {
            if (c == nth) return 1;
            ++c;
        }
    }
    return 0;
}

/* Tasks pane: the live registry as an editable list (no t->mu; reads engine state
 * through its own-locking APIs + run-thread-local pane fields). */
static size_t render_tasks(gptps_tui *t, char *buf, size_t cap)
{
    size_t pos = 0, i, n;
    int color = t->cfg.color;
    const char *B = color ? "\x1b[1m" : "", *INV = color ? "\x1b[7m" : "", *D = color ? "\x1b[2m" : "",
               *R = color ? "\x1b[31m" : "", *G = color ? "\x1b[32m" : "", *Y = color ? "\x1b[33m" : "", *X = color ? "\x1b[0m" : "";
    char pn[sizeof t->prompt_name * 2];
    n = gptps_task_count(t->e);
    if (t->task_sel < 0) t->task_sel = 0;
    if (n && (size_t)t->task_sel >= n) t->task_sel = (int)n - 1;
    pos = appendf(buf, cap, pos, "%sGPTPS \xc2\xb7 %s \xc2\xb7 tasks%s  (%lu)\n\n", B, t->cfg.title, X, (unsigned long)n);
    if (n == 0) pos = appendf(buf, cap, pos, "  %s(no task types registered)%s\n", D, X);
    else pos = appendf(buf, cap, pos, "%s  %-18s %-7s %4s %4s %4s %4s  %s%s\n", D, "name", "exec", "pri", "que", "run", "dl", "state", X);
    for (i = 0; i < n; ++i) {
        gptps_task_info info; const char *st; const char *sc; char nm[GPTPS_TASK_NAME_MAX * 2 + 2];
        if (!tui_task_at(t->e, i, &info)) continue;
        st = info.removed ? "draining" : !info.enabled ? "paused" : "on";
        sc = info.removed ? Y : !info.enabled ? D : G;
        pos = appendf(buf, cap, pos, "%s%s%s %-7s %4d %4u %4u %4u  %s%s%s%s\n",
                      (i == (size_t)t->task_sel) ? INV : "", (i == (size_t)t->task_sel) ? "> " : "  ",
                      tui_cell(info.name_copy, nm, sizeof nm, 18), exec_str(info.exec), info.priority, info.queued, info.running, info.dead,
                      sc, st, color ? X : "", X);
    }
    if (t->prompt_kind == TUI_PK_CONFIRM_DELETE) {
        gptps_task_info info;                 /* resolve by the captured NAME, not a stale index */
        if (find_task_by_name(t, t->prompt_name, &info))
            pos = appendf(buf, cap, pos, "\n%sDelete '%s'?%s  %u queued, %u in-flight.  %s[y]%s remove (pause+drain)  %s[x]%s cancel-force  %s[n]%s no\n",
                          R, tui_cl(t->prompt_name, pn, sizeof pn), X, info.queued, info.running, B, X, B, X, B, X);
        else
            pos = appendf(buf, cap, pos, "\n%s'%s' is gone.%s  any key to dismiss\n", R, tui_cl(t->prompt_name, pn, sizeof pn), X);
    } else if (t->prompt_kind == TUI_PK_CLONE_NAME || t->prompt_kind == TUI_PK_NEW_NAME || t->prompt_kind == TUI_PK_NEW_ARGV) {
        char shown[GPTPS_SETTINGS_VALUE_MAX * 2];
        tui_clean(t->editbuf, (size_t)t->editlen, shown, sizeof shown, -1, NULL);
        if (t->prompt_kind == TUI_PK_CLONE_NAME)
            pos = appendf(buf, cap, pos, "\n%sclone '%s' as:%s %s_  %s(enter ok, esc cancel)%s\n", B, tui_cl(t->prompt_name, pn, sizeof pn), X, shown, D, X);
        else if (t->prompt_kind == TUI_PK_NEW_NAME)
            pos = appendf(buf, cap, pos, "\n%snew program task \xc2\xb7 name:%s %s_  %s(enter next, esc cancel)%s\n", B, X, shown, D, X);
        else
            pos = appendf(buf, cap, pos, "\n%snew task '%s' \xc2\xb7 argv:%s %s_  %s(space-separated; enter ok, esc cancel)%s\n", B, tui_cl(t->prompt_name, pn, sizeof pn), X, shown, D, X);
    }
    pos = status_line(t, buf, cap, pos, "", D, X);
    pos = appendf(buf, cap, pos, "\n%skeys:%s %s move  %s inspect  %s back  a pause  c clone  n new  d delete  q quit\n", D, X, a_ud(t), a_rt(t), a_lt(t));
    if (pos >= cap) pos = cap - 1;
    buf[pos] = 0;
    return pos;
}

/* Detail pane: one task's resolved settings (built-in policy + generic per-task),
 * inline-editable. Filtered view over the registry by the tasks.<name>. prefix. */
static size_t render_detail(gptps_tui *t, char *buf, size_t cap)
{
    size_t pos = 0;
    int color = t->cfg.color, c = 0, shown, kw = 16, rows = list_rows(t), first, have_sel = 0;
    char prefix[160];   /* "tasks.<name>." - sized for the full detail_task buffer */
    char title[sizeof t->detail_task * 2];
    const char *B = color ? "\x1b[1m" : "", *INV = color ? "\x1b[7m" : "", *D = color ? "\x1b[2m" : "", *X = color ? "\x1b[0m" : "";
    size_t i, n = gptps_settings_count(t->e), plen;
    gptps_setting_info info, sel;
    memset(&sel, 0, sizeof sel);
    snprintf(prefix, sizeof prefix, "tasks.%s.", t->detail_task);
    plen = strlen(prefix);
    shown = count_prefixed(t, prefix);
    if (t->detail_sel < 0) t->detail_sel = 0;
    if (shown && t->detail_sel >= shown) t->detail_sel = shown - 1;
    for (i = 0; i < n; ++i)
        if (tui_setting_at(t->e, i, &info) && strncmp(info.key_copy, prefix, plen) == 0 &&
            u8_cols(info.key_copy + plen) > kw) kw = u8_cols(info.key_copy + plen);
    if (kw > 40) kw = 40;
    first = t->detail_sel - rows / 2;
    if (first > shown - rows) first = shown - rows;
    if (first < 0) first = 0;
    pos = appendf(buf, cap, pos, "%sGPTPS \xc2\xb7 task '%s'%s  %s(%d settings)%s\n\n", B,
                  tui_cl(t->detail_task, title, sizeof title), X, D, shown, X);
    if (shown == 0) pos = appendf(buf, cap, pos, "  %s(task no longer present)%s\n", D, X);
    for (i = 0; i < n; ++i) {
        if (!tui_setting_at(t->e, i, &info)) continue;
        if (strncmp(info.key_copy, prefix, plen) != 0) continue;
        if (c == t->detail_sel) { sel = info; have_sel = 1; }
        if (c >= first && c < first + rows)
            pos = setting_row(buf, cap, pos, &info, info.key_copy + plen, kw, c == t->detail_sel, INV, X);
        ++c;
    }
    if (have_sel) pos = setting_details(t, buf, cap, pos, &sel);
    if (t->editing) pos = edit_line(t, buf, cap, pos, B, D, X);
    pos = status_line(t, buf, cap, pos, "", D, X);
    pos = appendf(buf, cap, pos, "\n%skeys:%s %s move  %s edit  %s back  w save  q quit\n", D, X, a_ud(t), a_rt(t), a_lt(t));
    if (pos >= cap) pos = cap - 1;
    buf[pos] = 0;
    return pos;
}

/* Dead-letter pane. The engine keeps its dead letters where only a DRAIN can read
 * them, and a drain takes them (gptps.h: destructive, exactly once), so this pane
 * does not read them behind the host's back: it shows how many the engine retains
 * and, once the operator loads them (g, or any action - which takes them), each one
 * with its task, its status and why it failed (exit code or start error, and its
 * stderr's last line; ABI 2.8). Loaded dead letters are HELD here: re-submitted or
 * discarded one at a time or all at once, and a re-submit the engine refuses (the
 * task is paused or gone, the queue is full, a constraint says no, shutdown) keeps the
 * item here, with the reason, instead of losing it - "re-submit all" used to drop
 * every such item for good and blame a removed task. */
static size_t render_deadletter(gptps_tui *t, char *buf, size_t cap)
{
    size_t pos = 0, dn = gptps_dead_letter_count(t->e), i;
    int color = t->cfg.color, rows = list_rows(t), first, W = draw_width(t);
    const char *B = color ? "\x1b[1m" : "", *D = color ? "\x1b[2m" : "", *R = color ? "\x1b[31m" : "",
               *INV = color ? "\x1b[7m" : "", *X = color ? "\x1b[0m" : "";
    pos = appendf(buf, cap, pos, "%sGPTPS \xc2\xb7 %s \xc2\xb7 dead letter%s\n\n", B, t->cfg.title, X);
    pos = appendf(buf, cap, pos, "  %s%lu%s retained by the engine \xc2\xb7 %s%lu%s held here\n",
                  dn ? R : "", (unsigned long)dn, X, t->nheld ? R : "", (unsigned long)t->nheld, X);
    pos = appendf(buf, cap, pos, "  %sthese exhausted retries (or were denied). g loads the engine's here (it takes them\n"
                  "  from the engine); re-submit them, or discard them.%s\n", D, X);
    if (t->held_lost)
        pos = appendf(buf, cap, pos, "  %s%lu could not be kept here (no memory, or more than %d) and are gone%s\n",
                      R, t->held_lost, TUI_HELD_MAX, X);
    if (t->nheld) {
        if (t->dl_sel < 0) t->dl_sel = 0;
        if ((size_t)t->dl_sel >= t->nheld) t->dl_sel = (int)t->nheld - 1;
        first = t->dl_sel - rows / 2;
        if (first > (int)t->nheld - rows) first = (int)t->nheld - rows;
        if (first < 0) first = 0;
        pos = appendf(buf, cap, pos, "\n%s  %-16s %-10s %-22s %s%s\n", D, "task", "handle", "status", "why", X);
        for (i = (size_t)first; i < t->nheld && (int)(i - (size_t)first) < rows; ++i) {
            const tui_held *h = &t->held[i];
            char pre[128], why[160], tn[GPTPS_TASK_NAME_MAX * 2 + 2];
            int pl, room, wl;
            if (h->refused != GPTPS_OK)
                snprintf(why, sizeof why, "[re-submit refused: %s] %s", gptps_strerror(h->refused), h->why);
            else
                snprintf(why, sizeof why, "%s", h->why);
            pl = snprintf(pre, sizeof pre, "%s%s #%-9llu %-22.22s ", (i == (size_t)t->dl_sel) ? "> " : "  ",
                          tui_cell(h->task, tn, sizeof tn, 16), (unsigned long long)h->handle, gptps_strerror(h->status));
            room = W - 1 - (pl < 0 ? 0 : pl);
            wl = room > 0 ? (int)u8_fit(why, room) : 0;
            pos = appendf(buf, cap, pos, "%s%s%.*s%s\n", (i == (size_t)t->dl_sel) ? INV : "",
                          pre, wl, why, X);
        }
    }
    pos = status_line(t, buf, cap, pos, "\n", D, X);
    pos = appendf(buf, cap, pos, "\n%skeys:%s g load  r re-submit all  e re-submit this one  x discard this one  D discard all\n"
                  "      %s move  %s back  q quit\n", D, X, a_ud(t), a_lt(t));
    if (pos >= cap) pos = cap - 1;
    buf[pos] = 0;
    return pos;
}

/* The dashboard's key legend, wrapped to W columns: one line was over 80 columns with
 * no hotkeys bound, so it wrapped and the frame was a row taller than counted. Plain
 * text, lines joined by '\n', no trailing one; returns the rows. Caller holds mu. */
static int legend_text(gptps_tui *t, int W, char *out, size_t cap)
{
    char items[TUI_MAX_TASKS + 8][96];
    int n = 0, i, rows = 1, col;
    size_t pos = 0;
    int any = 0;
    for (i = 0; i < t->ntasks && n < TUI_MAX_TASKS; ++i)
        if (t->tasks[i].hotkey > 32 && t->tasks[i].hotkey < 127) {   /* a key that can be typed and shown */
            char lb[sizeof items[0] - 8];           /* cleaned, cut at a character, fits the item */
            snprintf(items[n++], sizeof items[0], "[%c] %.*s", t->tasks[i].hotkey, (int)sizeof lb - 1,
                     tui_cl(t->tasks[i].label, lb, sizeof lb));
            any = 1;
        }
    if (any) snprintf(items[n++], sizeof items[0], "\xc2\xb7");
    snprintf(items[n++], sizeof items[0], "%s scroll", a_ud(t));
    snprintf(items[n++], sizeof items[0], "s settings");
    snprintf(items[n++], sizeof items[0], "t tasks");
    snprintf(items[n++], sizeof items[0], "l dead-letter");
    snprintf(items[n++], sizeof items[0], "m kpi");
    snprintf(items[n++], sizeof items[0], "p pause");
    snprintf(items[n++], sizeof items[0], "? help");
    snprintf(items[n++], sizeof items[0], "q quit");
    pos = (size_t)snprintf(out, cap, "keys:");
    col = 5;
    for (i = 0; i < n; ++i) {
        int w = u8_cols(items[i]);
        if (col > 6 && col + 2 + w > W - 1) {         /* the next row, indented */
            pos += (size_t)snprintf(out + pos, pos < cap ? cap - pos : 0, "\n     ");
            col = 5; ++rows;
        }
        pos += (size_t)snprintf(out + pos, pos < cap ? cap - pos : 0, "%s%s", col > 5 ? "  " : " ", items[i]);
        col += (col > 5 ? 2 : 1) + w;
        if (pos >= cap) { out[cap - 1] = 0; break; }
    }
    return rows;
}

size_t gptps_tui_render(gptps_tui *t, char *buf, size_t cap)
{
    size_t pos = 0;
    int i, color, uni, W, toast_on, legrows;
    const char *B, *D, *R, *G, *X;
    double up, rate;
    char legend[2048];
    if (!t || !buf || cap == 0) { if (buf && cap) buf[0] = 0; return 0; }
    if (t->pane == TUI_PANE_SETTINGS) return render_settings(t, buf, cap);
    if (t->pane == TUI_PANE_HELP)     return render_help(t, buf, cap);
    if (t->pane == TUI_PANE_TASKS)    return render_tasks(t, buf, cap);
    if (t->pane == TUI_PANE_DETAIL)   return render_detail(t, buf, cap);
    if (t->pane == TUI_PANE_DL)       return render_deadletter(t, buf, cap);
    color = t->cfg.color; uni = t->cfg.unicode; W = draw_width(t);
    B = color ? "\x1b[1m" : ""; D = color ? "\x1b[2m" : ""; R = color ? "\x1b[31m" : "";
    G = color ? "\x1b[32m" : ""; X = color ? "\x1b[0m" : "";

    mu_lock(&t->mu);
    toast_on = t->status[0] && t->status_ms && (gptps_now_ms(NULL) - t->status_ms) < 3000u;
    legrows = legend_text(t, W, legend, sizeof legend);
    up = (double)(gptps_now_ms(NULL) - t->start_ms) / 1000.0;
    rate = (up > 0.05) ? (double)t->fin / up : 0.0;

    /* header: a framed title bar when color is on; a plain line otherwise */
    if (color) { pos = titlebar(buf, cap, pos, t, W, up, rate); pos = rule(buf, cap, pos, W, uni); }
    else pos = appendf(buf, cap, pos, "GPTPS \xc2\xb7 %s   up %.1fs   %.1f done/s\n", t->cfg.title, up, rate);

    {   /* in-flight gauge, scaled to the terminal width */
        unsigned inflt = t->inflight, j, gw, w;   /* counted, not derived: see the field */
        const char *fillc = uni ? "\xe2\x96\x88" : "#";   /* full block / hash */
        const char *trakc = uni ? "\xe2\x96\x91" : ".";   /* light shade / dot  */
        const char *C = color ? "\x1b[36m" : "";          /* cyan bar */
        gw = (unsigned)(W / 3); if (gw < 10) gw = 10; if (gw > 40) gw = 40;
        w = (t->peak && inflt) ? (inflt * gw / t->peak) : 0; if (w > gw) w = gw;
        pos = appendf(buf, cap, pos, "queued %u  started %u  in-flight %u  %s[%s", t->q, t->s, inflt, D, C);
        for (j = 0; j < gw; ++j) pos = appendf(buf, cap, pos, "%s", j < w ? fillc : trakc);
        pos = appendf(buf, cap, pos, "%s%s]%s peak %u\n", X, D, X, t->peak);
    }
    pos = appendf(buf, cap, pos, "%sfinished %u%s  %sfailed %u%s  retried %u  %sdead %u%s  %sdropped %u%s\n",
                  G, t->fin, X, (t->fail ? R : ""), t->fail, X, t->retr, (t->dead ? R : ""), t->dead, X,
                  (t->drop ? R : ""), t->drop, X);
    pos = appendf(buf, cap, pos, "%skpi:%s mode:%s refresh:%ums%s\n",
                  D, kpi_str(t->kpi), mode_str(t->mode), t->cfg.refresh_ms, X);

    if (t->show_tasks && t->kpi >= GPTPS_TUI_KPI_NORMAL) {
        pos = appendf(buf, cap, pos, "\n%sTASKS%s\n", B, X);
        pos = appendf(buf, cap, pos, "%s  %-16s %5s %5s %5s %5s %5s %5s %8s %8s  key%s\n",
                      D, "label", "run", "ok", "fail", "dead", "drop", "ok%", "avg ms", "peak MB", X);
        for (i = 0; i < t->ntasks; ++i) {
            tui_task *k = &t->tasks[i];
            char key[8], pct[8], lat[12], peak[32], lb[sizeof k->label * 2];
            unsigned terminal = k->finished + k->dead + k->dropped;
            const char *pc = "";
            if (k->hotkey > 32 && k->hotkey < 127) snprintf(key, sizeof key, "[%c]", k->hotkey); else key[0] = 0;
            if (terminal) {
                /* 64-bit intermediate, then clamp. k->finished is a plain unsigned counter that
                 * only ever climbs, so a long-lived engine crosses 42,949,673 finished items on
                 * one task - twelve hours at a thousand a second - and `finished * 100u` wraps
                 * past 2^32: the ok% column flips from 100 to 0 with nothing else in the row
                 * changing, and stays wrong for the life of the process. The clamp is not
                 * decoration either. `terminal` is itself a 32-bit sum, so once finished + dead
                 * + dropped passes UINT_MAX it wraps to a tiny divisor and the quotient reaches ten
                 * digits - more than pct holds, which is what GCC 16's -Wformat-truncation
                 * objects to. Clamped, the invariant both the %3u here and the %5s column
                 * below already assume - okp is 0..100, three digits - is true rather than
                 * merely hoped for, so the table cannot shear either. */
                unsigned okp = (unsigned)((unsigned long long)k->finished * 100u / terminal);
                if (okp > 100u) okp = 100u;
                snprintf(pct, sizeof pct, "%3u%%", okp);
                if (color) pc = (okp >= 90) ? G : (okp >= 50) ? "\x1b[33m" : R;   /* green/yellow/red */
            } else strcpy(pct, "  --");
            if (k->lat_n) snprintf(lat, sizeof lat, "%8.1f", (double)k->lat_sum_ms / k->lat_n); else strcpy(lat, "      --");
            /* What a process job's attempts measured at their peak (docs/MEASUREMENTS.md);
             * "--" for an in-process task, which is never measured. */
            if (k->mem_method[0]) snprintf(peak, sizeof peak, "%8.1f", (double)k->mem_peak / 1048576.0);
            else                  strcpy(peak, "      --");
            pos = appendf(buf, cap, pos, "  %s %5u %5u %5u %5u %5u %s%5s%s %8s %8s  %s\n",
                          tui_cell(k->label, lb, sizeof lb, 16), k->started, k->finished, k->failed, k->dead, k->dropped,
                          pc, pct, color ? X : "", lat, peak, key);
        }
    }

    if (t->show_recent && t->rcap > 0 && t->kpi >= GPTPS_TUI_KPI_NORMAL) {
        int shown = t->rn, start, maxoff, used = 0, avail;
        size_t z;
        if (shown > t->cfg.max_recent) shown = t->cfg.max_recent;
        for (z = 0; z < pos; ++z) if (buf[z] == '\n') ++used;     /* vertical fit: stay within rows */
        /* every line below is one row (each is fitted to W): the RECENT heading and
         * its blank line, the toast and its blank line, the legend and its blank line;
         * and the frame's last newline must not scroll the screen */
        avail = (t->rows > 0 ? t->rows : 24) - 1 - used - 2 - (toast_on ? 2 : 0) - 1 - legrows;
        if (avail < 1) avail = 1;
        if (shown > avail) shown = avail;
        maxoff = t->rn - shown; if (maxoff < 0) maxoff = 0;
        if (t->scroll > maxoff) t->scroll = maxoff;     /* clamp (keys can over-scroll) */
        if (t->scroll < 0) t->scroll = 0;
        start = (t->rhead - t->scroll - shown + t->rcap * 4) % t->rcap; /* window end = newest - scroll */
        if (t->scroll > 0) pos = appendf(buf, cap, pos, "\n%sRECENT%s  %s(scrolled +%d)%s\n", B, X, D, t->scroll, X);
        else               pos = appendf(buf, cap, pos, "\n%sRECENT%s\n", B, X);
        for (i = 0; i < shown; ++i) {
            tui_event *re = &t->recent[(start + i) % t->rcap];
            const char *kc = (re->kind == GPTPS_EV_FAILED || re->kind == GPTPS_EV_DEAD_LETTERED ||
                              re->kind == GPTPS_EV_DROPPED) ? R
                           : (re->kind == GPTPS_EV_FINISHED) ? G : D;
            double rel = (double)(re->ts - t->start_ms) / 1000.0; /* seconds since install */
            /* One terminal row each: the vertical fit above counts rows by '\n', so a
             * line that wrapped took two and pushed the frame past the screen. The fixed
             * fields are measured as printed; the why text gets what is left. */
            char pre[128], nm[sizeof re->name * 2];
            int pl = snprintf(pre, sizeof pre, "  %6.1f %-8s %s #%llu", rel, kind_str(re->kind), tui_cell(re->name, nm, sizeof nm, 14),
                              (unsigned long long)re->handle);
            int room = W - 1 - (pl < 0 ? 0 : pl) - 2;
            int wl = (re->why[0] && room >= 4) ? (int)u8_fit(re->why, room) : 0;
            pos = appendf(buf, cap, pos, "  %s%6.1f%s %s%-8s%s %s #%llu%s%s%.*s%s\n",
                          D, rel, X, kc, kind_str(re->kind), X, nm, (unsigned long long)re->handle,
                          wl ? "  " : "", D, wl, re->why, X);
        }
    }

    /* transient action toast (fades after a few seconds), one row */
    if (toast_on) {
        char shown[sizeof t->status * 2];
        tui_clean(t->status, strlen(t->status), shown, sizeof shown, W - 1, NULL);
        pos = appendf(buf, cap, pos, "\n%s%s%s\n", (color ? "\x1b[36m" : ""), shown, X);
    }

    /* key legend: task hotkeys first, then the global keys, wrapped to the width */
    pos = appendf(buf, cap, pos, "\n%s%s%s\n", D, legend, X);
    mu_unlock(&t->mu);

    if (pos >= cap) pos = cap - 1;
    buf[pos] = 0;
    return pos;
}

/* One key into the line editor shared by every pane (settings, detail, the task
 * prompts). Text is UTF-8: the live loop hands over a character's bytes one key at a
 * time (POSIX reads them as they come; on Windows term_poll_key encodes the console's
 * UTF-16), and they are kept - only ASCII used to be, so a value with a non-ASCII
 * path could not be typed.
 *
 * A character's bytes are collected in edit_part and reach editbuf only once they
 * are complete and well-formed (u8_decode: no overlong, no surrogate), so editbuf -
 * what is shown and what Enter commits - is valid UTF-8 at every moment. Bytes that
 * do not make a character - a terminal that is not in UTF-8 sends Latin-1's 'c
 * cedilla' as the lone byte E7 - are left out, and the status line says so, once per
 * character: committing them would put invalid UTF-8 into a setting such as a
 * work_dir. Backspace removes a whole character (or the unfinished one), and a
 * character that does not fit is left out whole, also with a word in the status. */
static void edit_bad(gptps_tui *t)
{
    snprintf(t->status, sizeof t->status, "left out a byte that is not UTF-8 text (is the terminal set to UTF-8?)");
    t->status_ms = gptps_now_ms(NULL);
}
static void edit_append(gptps_tui *t, const unsigned char *b, int n)
{
    int cap = (int)sizeof t->editbuf - 1;
    if (t->editlen + n > cap) {
        snprintf(t->status, sizeof t->status, "the value is full (%d bytes)", cap);
        t->status_ms = gptps_now_ms(NULL);
        return;
    }
    memcpy(t->editbuf + t->editlen, b, (size_t)n);
    t->editlen += n;
    t->editbuf[t->editlen] = 0;
}
static void edit_input(gptps_tui *t, int key)
{
    unsigned char b = (unsigned char)key;
    if (key == 8 || key == 127) {                                   /* backspace: one character */
        if (t->edit_need) { t->edit_need = 0; return; }             /* the one not finished */
        while (t->editlen > 0 && ((unsigned char)t->editbuf[t->editlen - 1] & 0xc0u) == 0x80u) --t->editlen;
        if (t->editlen > 0) --t->editlen;
        t->editbuf[t->editlen] = 0;
        return;
    }
    if (key < 32 || key > 255) return;
    if (key >= 0x80 && key < 0xc0) {                                /* a continuation byte */
        uint32_t cp;
        int n;
        if (!t->edit_need) { edit_bad(t); return; }                 /* with no character open */
        t->edit_part[t->edit_partn++] = b;
        if (--t->edit_need) return;
        n = t->edit_partn;
        if ((int)u8_decode(t->edit_part, (size_t)n, &cp) != n) { edit_bad(t); return; }   /* overlong, surrogate */
        edit_append(t, t->edit_part, n);
        return;
    }
    if (t->edit_need) { t->edit_need = 0; edit_bad(t); }           /* a character cut short */
    if (key < 0x80) { edit_append(t, &b, 1); return; }
    if (key < 0xc2 || key > 0xf4) { edit_bad(t); return; }         /* never a lead byte */
    t->edit_part[0] = b; t->edit_partn = 1;
    t->edit_need = key >= 0xf0 ? 3 : key >= 0xe0 ? 2 : 1;
}

/* the edit line, cleaned: a value set elsewhere is shown, never executed */
static size_t edit_line(gptps_tui *t, char *buf, size_t cap, size_t pos, const char *B, const char *D, const char *X)
{
    char shown[GPTPS_SETTINGS_VALUE_MAX * 2];
    tui_clean(t->editbuf, (size_t)t->editlen, shown, sizeof shown, -1, NULL);
    return appendf(buf, cap, pos, "\n%sedit:%s %s_  %s(Enter applies, Esc cancels)%s\n", B, X, shown, D, X);
}

/* Start editing the setting `in` describes: its key, copied now (commits go to it), and
 * its value. A key too long for the copy is not editable here - and the status says so. */
static void edit_begin(gptps_tui *t, const gptps_setting_info *in)
{
    if (!in->key_copy[0] || strlen(in->key_copy) >= sizeof t->edit_key) {
        snprintf(t->status, sizeof t->status, "this setting's key is too long to edit here; "
                 "use gptps_settings_set or the config file");
        t->status_ms = gptps_now_ms(NULL);
        return;
    }
    memcpy(t->edit_key, in->key_copy, strlen(in->key_copy) + 1);
    snprintf(t->editbuf, sizeof t->editbuf, "%s", in->value);
    t->editlen = (int)strlen(t->editbuf); t->editing = 1; t->edit_need = 0;
}

/* Settings-pane key handling. Returns 4 (settings interaction) or -1 (quit). */
static int settings_press(gptps_tui *t, int key)
{
    gptps_setting_info info;
    if (t->editing) {
        if (key == 27) { t->editing = 0; t->editbuf[0] = 0; t->editlen = 0; t->edit_need = 0; return 4; }   /* cancel */
        if (key == '\r' || key == '\n') {                                                 /* commit */
            /* To the key the edit STARTED on (edit_key), not to whatever setting is at
             * the selected index now: a task unregistered meanwhile removes its
             * settings and shifts every index after them, and the value used to land
             * on a neighbour - "set <wrong key> = 600000". */
            char why[256];
            gptps_status st = gptps_settings_set_ex(t->e, t->edit_key, t->editbuf, why, sizeof why);
            set_status(t, t->edit_key, t->editbuf, st, why);
            t->editing = 0; t->editbuf[0] = 0; t->editlen = 0; t->edit_need = 0;
            return 4;
        }
        edit_input(t, key);
        return 4;
    }
    switch (key) {
        case 'q': case 'Q': mu_lock(&t->mu); t->quit = 1; mu_unlock(&t->mu); return -1;
        case 27: case 's': case 'S': case '\t': t->pane = 0; t->status[0] = 0; return 4;   /* back to dashboard */
        case 'j': case 'J': { size_t n = gptps_settings_count(t->e); if (n && (size_t)t->sel + 1 < n) t->sel++; return 4; }
        case 'k': case 'K': if (t->sel > 0) t->sel--; return 4;
        case '\r': case '\n':                                                              /* start editing */
            if (tui_setting_at(t->e, (size_t)t->sel, &info)) edit_begin(t, &info);
            return 4;
        case 'w': case 'W': save_status(t, gptps_settings_save(t->e, t->cfg.settings_path)); return 4;
        default: return 4;
    }
}

/* Take one drained dead letter into the dashboard's keeping (drain runs this with the
 * engine lock released). A copy of everything a re-submit needs, and of why it failed. */
static void tui_take(const gptps_dead_letter *dl, void *ud)
{
    gptps_tui *t = (gptps_tui *)ud;
    tui_held *h;
    int has_tail;
    if (t->nheld == t->held_cap) {
        size_t nc = t->held_cap ? t->held_cap * 2 : 16;
        tui_held *nh = nc <= TUI_HELD_MAX ? (tui_held *)realloc(t->held, nc * sizeof *nh) : NULL;
        if (!nh) { t->held_lost += 1; return; }
        t->held = nh; t->held_cap = nc;
    }
    h = &t->held[t->nheld];
    memset(h, 0, sizeof *h);
    h->task = (char *)malloc(strlen(dl->task_name) + 1);
    h->payload = dl->payload_len ? malloc(dl->payload_len) : NULL;
    if (!h->task || (dl->payload_len && !h->payload)) { free(h->task); free(h->payload); t->held_lost += 1; return; }
    strcpy(h->task, dl->task_name);
    if (dl->payload_len) memcpy(h->payload, dl->payload, dl->payload_len);
    h->plen = dl->payload_len; h->handle = dl->handle; h->status = dl->status; h->refused = GPTPS_OK;
    if (dl->struct_size >= offsetof(gptps_dead_letter, options) + sizeof dl->options) h->opts = dl->options;
    has_tail = dl->struct_size >= offsetof(gptps_dead_letter, stderr_tail_len) + sizeof dl->stderr_tail_len;
    why_line(dl_measure(dl, GPTPS_M_START_ERROR), dl_measure(dl, GPTPS_M_EXIT_CODE),
             dl_measure(dl, GPTPS_M_EXIT_SIGNAL), dl->status,
             has_tail ? dl->stderr_tail : NULL, has_tail ? dl->stderr_tail_len : 0, h->why, sizeof h->why);
    t->nheld += 1;
}

/* Drain the engine's dead letters into the dashboard's keeping; how many arrived. */
static size_t tui_load(gptps_tui *t)
{
    size_t before = t->nheld;
    unsigned long lost = t->held_lost;
    gptps_dead_letter_drain(t->e, tui_take, t);
    if (t->held_lost != lost) {                     /* never silent */
        char msg[200];
        snprintf(msg, sizeof msg, "gptps_tui: %lu dead letter(s) drained from the engine could not be kept by the "
                 "dashboard (no memory, or more than %d held) and are gone", t->held_lost - lost, TUI_HELD_MAX);
        gptps_log(NULL, GPTPS_LOG_WARN, msg);
    }
    return t->nheld - before;
}

static void held_free(tui_held *h) { free(h->task); free(h->payload); }

static void held_remove(gptps_tui *t, size_t i)
{
    held_free(&t->held[i]);
    memmove(&t->held[i], &t->held[i + 1], (t->nheld - i - 1) * sizeof *t->held);
    t->nheld -= 1;
    if (t->dl_sel > 0 && (size_t)t->dl_sel >= t->nheld) t->dl_sel = (int)t->nheld - 1;
}

/* Re-submit held item i as it was submitted - its own priority, policy and timeout
 * (gptps_dead_letter.options), but not a due time that has passed: the operator asked
 * for it to run, and a shed-late constraint would only refuse it again. On success it
 * leaves the dashboard; refused, it stays, with the reason. */
static gptps_status held_resubmit(gptps_tui *t, size_t i)
{
    tui_held *h = &t->held[i];
    gptps_submit_options o = h->opts;
    gptps_handle hd;
    gptps_status st;
    if ((o.flags & GPTPS_SUBMIT_DEADLINE) && o.deadline_in_ms == 0) o.flags &= ~(unsigned)GPTPS_SUBMIT_DEADLINE;
    st = gptps_submit_ex(t->e, h->task, h->payload, h->plen, o.flags ? &o : NULL, &hd);
    if (st == GPTPS_OK) held_remove(t, i);
    else h->refused = st;
    return st;
}

/* register a GPTPS_EXEC_PROGRAM task from a name + a space-separated argv string
 * (the only task kind whose behavior can be created entirely from the terminal). */
static gptps_status tui_new_program(gptps *e, const char *name, char *argvstr)
{
    char *toks[32]; int nt = 0; char *p = argvstr;
    gptps_task_def d;
    while (*p && nt < 31) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        toks[nt++] = p;
        while (*p && *p != ' ' && *p != '\t') ++p;
        if (*p) *p++ = 0;
    }
    toks[nt] = NULL;
    if (nt == 0 || !name || !*name) return GPTPS_E_INVAL;
    memset(&d, 0, sizeof d);
    d.struct_size = sizeof d; d.name = name; d.exec = GPTPS_EXEC_PROGRAM;
    d.argv = (const char *const *)toks;            /* register_task deep-copies name + argv */
    d.default_cost.struct_size = sizeof d.default_cost;
    d.default_policy.struct_size = sizeof d.default_policy;
    return gptps_register_task(e, &d);
}

/* Tasks-pane key handling (returns 4 = UI, -1 = quit). */
static int tasks_press(gptps_tui *t, int key)
{
    size_t n = gptps_task_count(t->e);
    gptps_task_info info;

    if (t->prompt_kind == TUI_PK_CONFIRM_DELETE) {
        if (key == 'y' || key == 'Y') {
            /* responsive remove: pause (instant) so no new work arrives, then remove
             * if idle. If still busy we leave it PAUSED to drain on its own (no UI
             * block) - the operator deletes again once it's idle. */
            gptps_status st;
            gptps_set_task_enabled(t->e, t->prompt_name, 0);
            st = gptps_unregister_task(t->e, t->prompt_name, GPTPS_REMOVE_REJECT_IF_BUSY);
            if (st == GPTPS_OK)          toast(t, "deleted %s", t->prompt_name);
            else if (st == GPTPS_E_BUSY) toast(t, "%s paused; in-flight work will finish \xc2\xb7 delete again when idle", t->prompt_name);
            else                         toast(t, "delete %s: %s", t->prompt_name, gptps_strerror(st));
            if (t->task_sel > 0) t->task_sel--;
        } else if (key == 'x' || key == 'X') {           /* force: drop queued + cancel in-flight (brief block) */
            gptps_status st = gptps_unregister_task(t->e, t->prompt_name, GPTPS_REMOVE_CANCEL);
            if (st == GPTPS_OK) toast(t, "deleted %s", t->prompt_name);
            else                toast(t, "delete %s: %s", t->prompt_name, gptps_strerror(st));
            if (t->task_sel > 0) t->task_sel--;
        } else toast(t, "delete cancelled");
        t->prompt_kind = TUI_PK_NONE;
        return 4;
    }
    if (t->prompt_kind == TUI_PK_CLONE_NAME || t->prompt_kind == TUI_PK_NEW_NAME || t->prompt_kind == TUI_PK_NEW_ARGV) {
        if (key == 27) { t->prompt_kind = TUI_PK_NONE; t->editbuf[0] = 0; t->editlen = 0; t->edit_need = 0; toast(t, "cancelled"); return 4; }
        if (key == '\r' || key == '\n') {
            if (t->prompt_kind == TUI_PK_CLONE_NAME) {
                gptps_status st = gptps_clone_task(t->e, t->prompt_name, t->editbuf);
                if (st == GPTPS_OK) toast(t, "cloned %s -> %s", t->prompt_name, t->editbuf);
                else                toast(t, "clone failed: %s", gptps_strerror(st));
                t->prompt_kind = TUI_PK_NONE;
            } else if (t->prompt_kind == TUI_PK_NEW_NAME) {
                if (t->editlen > 0) { copy_name(t->prompt_name, sizeof t->prompt_name, t->editbuf);
                                      t->prompt_kind = TUI_PK_NEW_ARGV; t->editbuf[0] = 0; t->editlen = 0; t->edit_need = 0; return 4; }
                t->prompt_kind = TUI_PK_NONE;
            } else { /* TUI_PK_NEW_ARGV */
                gptps_status st = tui_new_program(t->e, t->prompt_name, t->editbuf);
                if (st == GPTPS_OK) toast(t, "created %s", t->prompt_name);
                else                toast(t, "create failed: %s", gptps_strerror(st));
                t->prompt_kind = TUI_PK_NONE;
            }
            t->editbuf[0] = 0; t->editlen = 0; t->edit_need = 0;
            return 4;
        }
        edit_input(t, key);
        return 4;
    }
    switch (key) {
        case 'q': case 'Q': mu_lock(&t->mu); t->quit = 1; mu_unlock(&t->mu); return -1;
        case 27: case 's': case 'S': case 't': case 'T': case '\t': t->pane = TUI_PANE_DASH; t->status[0] = 0; return 4;
        case 'j': case 'J': if (n && (size_t)t->task_sel + 1 < n) t->task_sel++; return 4;
        case 'k': case 'K': if (t->task_sel > 0) t->task_sel--; return 4;
        case '\r': case '\n':                              /* inspect: open the detail pane */
            if (tui_task_at(t->e, (size_t)t->task_sel, &info)) {
                copy_name(t->detail_task, sizeof t->detail_task, info.name_copy);
                t->detail_sel = 0; t->status[0] = 0; t->pane = TUI_PANE_DETAIL;
            }
            return 4;
        case 'a': case 'A':                                /* pause / resume */
            if (tui_task_at(t->e, (size_t)t->task_sel, &info)) {
                char nm[128]; int en = info.enabled; copy_name(nm, sizeof nm, info.name_copy);
                gptps_set_task_enabled(t->e, nm, !en);
                toast(t, en ? "paused %s" : "resumed %s", nm);
            }
            return 4;
        case 'c': case 'C':                                /* clone */
            if (tui_task_at(t->e, (size_t)t->task_sel, &info)) {
                copy_name(t->prompt_name, sizeof t->prompt_name, info.name_copy);
                t->prompt_kind = TUI_PK_CLONE_NAME; t->editbuf[0] = 0; t->editlen = 0; t->edit_need = 0; t->status[0] = 0;
            }
            return 4;
        case 'n': case 'N':                                /* new program task */
            t->prompt_kind = TUI_PK_NEW_NAME; t->editbuf[0] = 0; t->editlen = 0; t->edit_need = 0; t->status[0] = 0; return 4;
        case 'd': case 'D':                                /* delete: capture the NAME now (index may shift) */
            if (n && tui_task_at(t->e, (size_t)t->task_sel, &info)) {
                copy_name(t->prompt_name, sizeof t->prompt_name, info.name_copy);
                t->prompt_kind = TUI_PK_CONFIRM_DELETE; t->status[0] = 0;
            }
            return 4;
        default: return 4;
    }
}

/* Detail-pane key handling: edit one task's settings inline (returns 4 / -1). */
static int detail_press(gptps_tui *t, int key)
{
    char prefix[160];   /* "tasks.<name>." - sized for the full detail_task buffer */
    gptps_setting_info info;
    snprintf(prefix, sizeof prefix, "tasks.%s.", t->detail_task);
    if (t->editing) {
        if (key == 27) { t->editing = 0; t->editbuf[0] = 0; t->editlen = 0; t->edit_need = 0; return 4; }
        if (key == '\r' || key == '\n') {
            char why[256];
            gptps_status st = gptps_settings_set_ex(t->e, t->edit_key, t->editbuf, why, sizeof why);
            set_status(t, t->edit_key, t->editbuf, st, why);
            t->editing = 0; t->editbuf[0] = 0; t->editlen = 0; t->edit_need = 0; return 4;
        }
        edit_input(t, key);
        return 4;
    }
    switch (key) {
        case 'q': case 'Q': mu_lock(&t->mu); t->quit = 1; mu_unlock(&t->mu); return -1;
        case 27: case 's': case 'S': case '\t': t->pane = TUI_PANE_TASKS; t->status[0] = 0; return 4;
        case 'j': case 'J': { int c = count_prefixed(t, prefix); if (c && t->detail_sel + 1 < c) t->detail_sel++; return 4; }
        case 'k': case 'K': if (t->detail_sel > 0) t->detail_sel--; return 4;
        case '\r': case '\n':
            if (nth_prefixed(t, prefix, t->detail_sel, &info)) edit_begin(t, &info);
            return 4;
        case 'w': case 'W': save_status(t, gptps_settings_save(t->e, t->cfg.settings_path)); return 4;
        default: return 4;
    }
}

/* Dead-letter pane key handling (returns 4 / -1). See render_deadletter. */
static int deadletter_press(gptps_tui *t, int key)
{
    if (t->dl_sel < 0 || (size_t)t->dl_sel >= t->nheld) t->dl_sel = t->nheld ? (int)t->nheld - 1 : 0;
    switch (key) {
        case 'q': case 'Q': mu_lock(&t->mu); t->quit = 1; mu_unlock(&t->mu); return -1;
        case 27: case 'l': case 'L': case 's': case 'S': case '\t': t->pane = TUI_PANE_DASH; t->status[0] = 0; return 4;
        case 'j': case 'J': if (t->nheld && (size_t)t->dl_sel + 1 < t->nheld) t->dl_sel++; return 4;
        case 'k': case 'K': if (t->dl_sel > 0) t->dl_sel--; return 4;
        case 'g': case 'G': { size_t k = tui_load(t); toast(t, "loaded %lu from the engine", (unsigned long)k); return 4; }
        case 'r': case 'R': {                          /* re-submit all: the engine's and those held */
            size_t ok = 0, kept = 0, i = 0;
            gptps_status first = GPTPS_OK;
            tui_load(t);
            while (i < t->nheld) {
                gptps_status st = held_resubmit(t, i);
                if (st == GPTPS_OK) { ++ok; continue; }    /* removed: i is the next one */
                if (first == GPTPS_OK) first = st;
                ++kept; ++i;
            }
            if (kept) {
                char msg[256];
                toast(t, "re-submitted %lu; %lu refused and kept here (%s)", (unsigned long)ok, (unsigned long)kept,
                      gptps_strerror(first));
                snprintf(msg, sizeof msg, "gptps_tui: %lu dead letter(s) could not be re-submitted (first: %s); "
                         "the dashboard keeps them", (unsigned long)kept, gptps_strerror(first));
                gptps_log(NULL, GPTPS_LOG_WARN, msg);
            } else {
                toast(t, "re-submitted %lu", (unsigned long)ok);
            }
            return 4;
        }
        case 'e': case 'E': {                          /* re-submit the selected one */
            gptps_status st;
            char task[64];
            if (!t->nheld) { toast(t, "nothing held here (g loads the engine's)"); return 4; }
            copy_name(task, sizeof task, t->held[t->dl_sel].task);
            st = held_resubmit(t, (size_t)t->dl_sel);
            if (st == GPTPS_OK) toast(t, "re-submitted one %s", task);
            else                toast(t, "%s not re-submitted: %s (kept here)", task, gptps_strerror(st));
            return 4;
        }
        case 'x': case 'X':                            /* discard the selected one */
            if (!t->nheld) { toast(t, "nothing held here (g loads the engine's)"); return 4; }
            held_remove(t, (size_t)t->dl_sel);
            toast(t, "discarded one");
            return 4;
        case 'D': {                                    /* discard all: the engine's and those held */
            size_t k = gptps_dead_letter_drain(t->e, NULL, NULL) + t->nheld, i;
            for (i = 0; i < t->nheld; ++i) held_free(&t->held[i]);
            t->nheld = 0; t->dl_sel = 0;
            toast(t, "discarded %lu", (unsigned long)k);
            return 4;
        }
        default: return 4;
    }
}

int gptps_tui_press(gptps_tui *t, int key)
{
    const char *name = NULL, *label = NULL;
    const void *pl = NULL;
    size_t pn = 0;
    int i;
    gptps_submit_options ko;
    int has_ko = 0;
    if (!t) return 0;
    if (t->pane == TUI_PANE_SETTINGS) return settings_press(t, key);   /* settings editor handles its own keys */
    if (t->pane == TUI_PANE_TASKS)    return tasks_press(t, key);
    if (t->pane == TUI_PANE_DETAIL)   return detail_press(t, key);
    if (t->pane == TUI_PANE_DL)       return deadletter_press(t, key);
    if (t->pane == TUI_PANE_HELP) {                     /* help overlay: any key returns */
        if (key == 'q' || key == 'Q') { mu_lock(&t->mu); t->quit = 1; mu_unlock(&t->mu); return -1; }
        t->pane = TUI_PANE_DASH; return 4;
    }
    if (key == 'q' || key == 'Q' || key == 27) {
        mu_lock(&t->mu); t->quit = 1; mu_unlock(&t->mu);
        return -1;
    }
    if (key == '?') { t->pane = TUI_PANE_HELP; return 4; }      /* open the help overlay */
    mu_lock(&t->mu);
    for (i = 0; i < t->ntasks; ++i)
        if (t->tasks[i].hotkey == key) {
            name = t->tasks[i].name; label = t->tasks[i].label; pl = t->tasks[i].payload; pn = t->tasks[i].plen;
            if ((has_ko = t->tasks[i].has_opts) != 0) ko = t->tasks[i].opts;   /* a copy: used unlocked */
            break;
        }
    mu_unlock(&t->mu);                 /* submit OUTSIDE the lock: QUEUED fires our observer */
    if (name) {                        /* a due time counts from this press */
        gptps_handle h;
        gptps_status st = gptps_submit_ex(t->e, name, pl, pn, has_ko ? &ko : NULL, &h);
        if (st != GPTPS_OK) {          /* paused, unknown, full, denied, shutting down: say so */
            toast(t, "NOT submitted %s: %s", label ? label : name, gptps_strerror(st));
            return 0;
        }
        toast(t, "submitted %s", label ? label : name);
        return 1;
    }
    if (key == 'm' || key == 'M') {                 /* cycle KPI level (configure cost live) */
        int nk; mu_lock(&t->mu); nk = (t->kpi >= GPTPS_TUI_KPI_FULL) ? GPTPS_TUI_KPI_MINIMAL : t->kpi + 1; mu_unlock(&t->mu);
        gptps_tui_set_kpi(t, (gptps_tui_kpi)nk); toast(t, "kpi -> %s", kpi_str(nk)); return 3;
    }
    if (key == 'p' || key == 'P') {                 /* toggle pause */
        int nm; mu_lock(&t->mu); nm = (t->mode == GPTPS_TUI_PAUSED) ? GPTPS_TUI_CONTINUOUS : GPTPS_TUI_PAUSED; mu_unlock(&t->mu);
        gptps_tui_set_mode(t, (gptps_tui_mode)nm); toast(t, nm == GPTPS_TUI_PAUSED ? "paused" : "resumed"); return 3;
    }
    if (key == 'k' || key == 'K') { mu_lock(&t->mu); t->scroll += 1; mu_unlock(&t->mu); return 2; } /* scroll to older */
    if (key == 'j' || key == 'J') { mu_lock(&t->mu); if (t->scroll > 0) t->scroll -= 1; mu_unlock(&t->mu); return 2; } /* newer */
    if (key == 's' || key == 'S') { t->pane = TUI_PANE_SETTINGS; t->status[0] = 0; return 4; }   /* open the settings pane */
    if (key == 't' || key == 'T') { t->pane = TUI_PANE_TASKS; t->task_sel = 0; t->status[0] = 0; return 4; }  /* task manager */
    if (key == 'l' || key == 'L') { t->pane = TUI_PANE_DL; t->status[0] = 0; return 4; }          /* dead-letter view */
    return 0;
}

gptps_status gptps_tui_add_task(gptps_tui *t, const char *task_name, const char *label,
                                int hotkey, const void *payload, size_t len)
{
    return gptps_tui_add_task_ex(t, task_name, label, hotkey, payload, len, NULL);
}

gptps_status gptps_tui_add_task_ex(gptps_tui *t, const char *task_name, const char *label,
                                   int hotkey, const void *payload, size_t len,
                                   const gptps_submit_options *opts)
{
    tui_task *tk;
    void *cp = NULL;
    gptps_submit_options o;
    if (!t || !task_name) return GPTPS_E_INVAL;
    if (opts) {                        /* an older caller's struct: copy what it has, the rest 0 */
        size_t n;
        if (opts->struct_size < offsetof(gptps_submit_options, timeout_ms) + sizeof(uint32_t)) return GPTPS_E_INVAL;
        memset(&o, 0, sizeof o);
        n = opts->struct_size < sizeof o ? opts->struct_size : sizeof o;
        memcpy(&o, opts, n);
        o.struct_size = sizeof o;
    }
    if (len) { cp = malloc(len); if (!cp) return GPTPS_E_NOMEM; memcpy(cp, payload, len); }
    mu_lock(&t->mu);
    tk = task_for(t, task_name);
    if (tk) {
        if (label) { strncpy(tk->label, label, sizeof tk->label - 1); tk->label[sizeof tk->label - 1] = 0; }
        tk->hotkey = hotkey;
        free(tk->payload); tk->payload = cp; tk->plen = len; cp = NULL;
        tk->has_opts = opts != NULL;
        if (opts) tk->opts = o;
    }
    mu_unlock(&t->mu);
    if (cp) free(cp);
    return tk ? GPTPS_OK : GPTPS_E_FULL;
}

/* ---- terminal control (live loop only) ---- */
#if !defined(_WIN32)
static void term_enable(gptps_tui *t)
{
    struct termios raw;
    if (tcgetattr(STDIN_FILENO, &t->saved_termios) != 0) return;
    raw = t->saved_termios;
    raw.c_lflag &= ~((tcflag_t)(ICANON | ECHO));
    raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) t->raw_active = 1;
}
static void term_restore(gptps_tui *t)
{ if (t->raw_active) { tcsetattr(STDIN_FILENO, TCSANOW, &t->saved_termios); t->raw_active = 0; } }
/* Read one byte within `ms`; -1 if none arrived. */
static int term_read_byte(int ms)
{
    fd_set fds; struct timeval tv; unsigned char c;
    FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
    tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r == 1) return c;
        if (r == 0 || (errno != EINTR && errno != EAGAIN)) {
            /* readable but nothing to read: end of input (the terminal went away, or
             * stdin was never one). select() would report that at once forever, and
             * the loop spun repainting with no wait: wait the interval anyway. */
            tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
            select(0, NULL, NULL, NULL, &tv);
        }
    }
    return -1;
}
/* After an ESC, decode an arrow / Home / End sequence (CSI "ESC [ X" or SS3
 * "ESC O X") into the matching navigation key. The follow-up reads use a short
 * timeout so a BARE Esc (quit on the dashboard, back in a sub-pane) never blocks.
 * Arrows reuse the existing keys, so no pane logic changes: Up=k, Down=j,
 * Right=Enter (select), Left=Tab (back; a no-op on the dashboard, so an arrow can
 * never quit). */
static int term_decode_escape(void)
{
    int b1 = term_read_byte(30), b2;
    if (b1 != '[' && b1 != 'O') return 27;        /* bare Esc (or an Alt-combo) => Esc */
    b2 = term_read_byte(30);
    switch (b2) {
        case 'A': case 'H': return 'k';            /* Up    / Home */
        case 'B': case 'F': return 'j';            /* Down  / End  */
        case 'C':           return '\r';           /* Right => select */
        case 'D':           return '\t';           /* Left  => back   */
        default:            return 27;             /* unrecognized => Esc */
    }
}
static int term_poll_key(int ms)
{
    int c = term_read_byte(ms);
    if (c == 27) return term_decode_escape();      /* may be an arrow key; else a bare Esc */
    return c;
}
static void term_size(gptps_tui *t)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        t->cols = ws.ws_col;
        if (ws.ws_row > 0) t->rows = ws.ws_row;
    }
}
#else
static void term_enable(gptps_tui *t)
{
    HANDLE ho = GetStdHandle(STD_OUTPUT_HANDLE); DWORD m;
    if (GetConsoleMode(ho, &m)) { t->saved_out_mode = m; t->modes_saved = 1;
        SetConsoleMode(ho, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING); }
    /* Our frames are UTF-8 bytes; put the console in UTF-8 so box-drawing and
     * other glyphs render (a legacy OEM codepage - e.g. CP857 - shows mojibake).
     * Save the old codepage and restore it in term_restore. If UTF-8 can't be
     * established, demote the frame set to pure ASCII. */
    t->saved_cp = GetConsoleOutputCP();
    if (!SetConsoleOutputCP(CP_UTF8)) t->cfg.unicode = 0;
    t->raw_active = 1;
}
static void term_restore(gptps_tui *t)
{ if (t->modes_saved) { SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), t->saved_out_mode); t->modes_saved = 0; }
  if (t->saved_cp) { SetConsoleOutputCP(t->saved_cp); t->saved_cp = 0; }
  t->raw_active = 0; }
/* Keys come from ReadConsoleInputW: each KEY_EVENT carries the character typed
 * (uChar, UTF-16) AND the key (wVirtualKeyCode), so an arrow key - which has no
 * character - is told apart from a letter. _getwch could not: it reports an extended
 * key as the prefix 0xE0 and then a scan code, and 0xE0 is also U+00E0, 'a grave',
 * so typing that letter blocked for a "scan code" and swallowed the next key.
 * The editors take UTF-8, one byte per key, so a character's later bytes wait in
 * tui_kq for the next calls; a surrogate pair's high half waits in tui_hi. */
static unsigned char tui_kq[4];
static int tui_kq_n, tui_kq_i;
static uint32_t tui_hi;
static int tui_key_of(const KEY_EVENT_RECORD *k)
{
    uint32_t cp = (uint32_t)k->uChar.UnicodeChar;
    if (!k->bKeyDown) return -1;
    if (cp == 0) {                                   /* no character: a navigation key? */
        switch (k->wVirtualKeyCode) {
            case VK_UP: case VK_HOME:  return 'k';
            case VK_DOWN: case VK_END: return 'j';
            case VK_RIGHT:             return '\r';  /* select */
            case VK_LEFT:              return '\t';  /* back */
            default:                   return -1;    /* Shift, F-keys, ...: nothing */
        }
    }
    if (cp >= 0xD800 && cp <= 0xDBFF) { tui_hi = cp; return -1; }   /* the pair's first half */
    if (cp >= 0xDC00 && cp <= 0xDFFF) {
        if (!tui_hi) return -1;
        cp = 0x10000u + ((tui_hi - 0xD800u) << 10) + (cp - 0xDC00u);
    }
    tui_hi = 0;
    if (cp < 0x80) return (int)cp;
    tui_kq_i = 0;
    if (cp < 0x800) { tui_kq[0] = (unsigned char)(0xC0 | (cp >> 6)); tui_kq[1] = (unsigned char)(0x80 | (cp & 0x3F)); tui_kq_n = 2; }
    else if (cp < 0x10000) { tui_kq[0] = (unsigned char)(0xE0 | (cp >> 12)); tui_kq[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                             tui_kq[2] = (unsigned char)(0x80 | (cp & 0x3F)); tui_kq_n = 3; }
    else { tui_kq[0] = (unsigned char)(0xF0 | (cp >> 18)); tui_kq[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
           tui_kq[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); tui_kq[3] = (unsigned char)(0x80 | (cp & 0x3F)); tui_kq_n = 4; }
    return tui_kq[tui_kq_i++];
}
static int term_poll_key(int ms)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD start = GetTickCount();
    if (tui_kq_i < tui_kq_n) return tui_kq[tui_kq_i++];
    for (;;) {
        DWORD spent = GetTickCount() - start, n = 0;
        INPUT_RECORD rec;
        if (spent > (DWORD)ms) return -1;
        if (in == NULL || in == INVALID_HANDLE_VALUE ||
            WaitForSingleObject(in, (DWORD)ms - spent) != WAIT_OBJECT_0) {
            if (in == NULL || in == INVALID_HANDLE_VALUE) Sleep((DWORD)ms);   /* never spin */
            return -1;
        }
        if (!ReadConsoleInputW(in, &rec, 1, &n) || n == 0) { Sleep(15); continue; }
        if (rec.EventType == KEY_EVENT) {
            int c = tui_key_of(&rec.Event.KeyEvent);
            if (c >= 0) return c;
        }
        /* a key release, a mouse, focus or resize event: wait on */
    }
}
static void term_size(gptps_tui *t)
{
    CONSOLE_SCREEN_BUFFER_INFO ci;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &ci)) {
        int c = ci.srWindow.Right - ci.srWindow.Left + 1, r = ci.srWindow.Bottom - ci.srWindow.Top + 1;
        if (c > 0) t->cols = c;
        if (r > 0) t->rows = r;
    }
}
#endif

/* write a frame flicker-free: erase each line's tail (ESC[K) instead of clearing
 * the whole screen, so unchanged regions don't flash. */
static void write_frame(FILE *out, const char *s)
{
    const char *p = s, *nl;
    while ((nl = strchr(p, '\n')) != NULL) {
        fwrite(p, 1, (size_t)(nl - p), out);
        fputs("\x1b[K\n", out);
        p = nl + 1;
    }
    if (*p) { fputs(p, out); fputs("\x1b[K", out); }
}

void gptps_tui_run(gptps_tui *t)
{
    char *buf; size_t cap = 16384;
    int first = 1, kp = 0;
    if (!t || !t->cfg.interactive) return;
    /* Needs a terminal both ways: frames out, keys in. With stdin at end of input
     * (</dev/null, a closed pipe, a launcher that gives it none) the key poll returned
     * at once and the loop repainted tens of thousands of frames a second. */
    if (!fd_is_tty(t->cfg.out) || !fd_is_tty(stdin)) {
        gptps_log(NULL, GPTPS_LOG_WARN, "gptps_tui_run: stdin or the output is not a terminal; "
                  "the live dashboard does not run (gptps_tui_snapshot renders a frame)");
        return;
    }
    buf = (char *)malloc(cap);
    if (!buf) return;
    term_enable(t);
    fputs("\x1b[?25l", t->cfg.out);                 /* hide cursor */
    for (;;) {
        int q, md, key, paint;
        uint32_t rms;
        mu_lock(&t->mu); q = t->quit; md = t->mode; rms = t->cfg.refresh_ms; mu_unlock(&t->mu);
        if (q) break;
        /* cadence: CONTINUOUS every frame; ON_DEMAND when state changed or a key
         * was pressed; PAUSED only after a key (or the first frame) */
        paint = first || kp || (md == GPTPS_TUI_CONTINUOUS);
        if (!paint && md == GPTPS_TUI_ON_DEMAND) { mu_lock(&t->mu); paint = t->dirty; mu_unlock(&t->mu); }
        if (paint) {
            mu_lock(&t->mu); t->dirty = 0; mu_unlock(&t->mu);
            term_size(t);                            /* adapt the layout to the current window */
            gptps_tui_render(t, buf, cap);
            if (first) fputs("\x1b[2J", t->cfg.out); /* clear once; then redraw in place */
            fputs("\x1b[H", t->cfg.out);             /* home */
            write_frame(t->cfg.out, buf);            /* per-line erase => no full-screen flash */
            fputs("\x1b[J", t->cfg.out);             /* wipe any lines a shorter frame left behind */
            fflush(t->cfg.out);
        }
        first = 0; kp = 0;
        key = term_poll_key((int)(rms ? rms : 250));
        if (key >= 0) { gptps_tui_press(t, key); kp = 1; }
    }
    fputs("\x1b[?25h", t->cfg.out); fflush(t->cfg.out); /* show cursor */
    term_restore(t);
    free(buf);
}

/* ---- settings registry bindings (target = the gptps_tui) ---- */
static const char *const TUI_KPI_CHOICES[]  = { "off", "minimal", "normal", "full", 0 };
static const char *const TUI_MODE_CHOICES[] = { "realtime", "on-demand", "paused", 0 };

static size_t ts_rd_refresh(void *p, char *b, size_t c) { gptps_tui *t = (gptps_tui *)p; unsigned ms; mu_lock(&t->mu); ms = t->cfg.refresh_ms; mu_unlock(&t->mu); return (size_t)snprintf(b, c, "%u", ms); }
static gptps_status ts_wr_refresh(void *p, const char *v) { return gptps_tui_set_refresh((gptps_tui *)p, (uint32_t)strtoul(v, NULL, 10)); }
static size_t ts_rd_kpi(void *p, char *b, size_t c) { gptps_tui *t = (gptps_tui *)p; int k; mu_lock(&t->mu); k = t->kpi; mu_unlock(&t->mu); return (size_t)snprintf(b, c, "%s", kpi_str(k)); }
static gptps_status ts_wr_kpi(void *p, const char *v) { int k = (strcmp(v, "off") == 0) ? GPTPS_TUI_KPI_OFF : (strcmp(v, "minimal") == 0) ? GPTPS_TUI_KPI_MINIMAL : (strcmp(v, "normal") == 0) ? GPTPS_TUI_KPI_NORMAL : GPTPS_TUI_KPI_FULL; return gptps_tui_set_kpi((gptps_tui *)p, (gptps_tui_kpi)k); }
static size_t ts_rd_latwin(void *p, char *b, size_t c) { gptps_tui *t = (gptps_tui *)p; int n; mu_lock(&t->mu); n = t->lat_cap; mu_unlock(&t->mu); return (size_t)snprintf(b, c, "%d", n); }
static gptps_status ts_wr_latwin(void *p, const char *v) { return gptps_tui_set_latency_window((gptps_tui *)p, (int)strtol(v, NULL, 10)); }
static size_t ts_rd_mode(void *p, char *b, size_t c) { gptps_tui *t = (gptps_tui *)p; int m; mu_lock(&t->mu); m = t->mode; mu_unlock(&t->mu); return (size_t)snprintf(b, c, "%s", mode_str(m)); }
static gptps_status ts_wr_mode(void *p, const char *v) { int m = (strcmp(v, "realtime") == 0) ? GPTPS_TUI_CONTINUOUS : (strcmp(v, "on-demand") == 0) ? GPTPS_TUI_ON_DEMAND : GPTPS_TUI_PAUSED; return gptps_tui_set_mode((gptps_tui *)p, (gptps_tui_mode)m); }

static void tui_register_settings(gptps *e, gptps_tui *t)
{
    gptps_setting_def d;
    memset(&d, 0, sizeof d); d.struct_size = sizeof d; d.target = t; d.hot = 1;
    d.key = "tui.refresh_ms"; d.type = GPTPS_SETTING_UINT; d.desc = "dashboard redraw interval (ms)"; d.choices = NULL; d.read = ts_rd_refresh; d.write = ts_wr_refresh;
    gptps_register_setting(e, &d);
    d.key = "tui.kpi"; d.type = GPTPS_SETTING_ENUM; d.desc = "KPI detail level"; d.choices = TUI_KPI_CHOICES; d.read = ts_rd_kpi; d.write = ts_wr_kpi;
    gptps_register_setting(e, &d);
    d.key = "tui.mode"; d.type = GPTPS_SETTING_ENUM; d.desc = "redraw cadence"; d.choices = TUI_MODE_CHOICES; d.read = ts_rd_mode; d.write = ts_wr_mode;
    gptps_register_setting(e, &d);
    d.key = "tui.latency_window"; d.type = GPTPS_SETTING_UINT; d.desc = "handles tracked for latency at FULL"; d.choices = NULL; d.read = ts_rd_latwin; d.write = ts_wr_latwin;
    gptps_register_setting(e, &d);
}

gptps_tui *gptps_tui_install(gptps *e, const gptps_tui_config *cfg)
{
    gptps_tui *t;
    int max;
    if (!e) return NULL;
    t = (gptps_tui *)calloc(1, sizeof *t);
    if (!t) return NULL;
    mu_init(&t->mu);
    t->e = e;
    if (cfg) t->cfg = *cfg;
    if (!t->cfg.title) t->cfg.title = "tasks";   /* every pane prints it */
    if (!t->cfg.out) t->cfg.out = stdout;
    if (t->cfg.refresh_ms == 0) t->cfg.refresh_ms = 250;
    if (t->cfg.color < 0)       t->cfg.color = fd_is_tty(t->cfg.out);
    if (t->cfg.interactive < 0) t->cfg.interactive = fd_is_tty(t->cfg.out);
    if (t->cfg.unicode < 0)     t->cfg.unicode = t->cfg.color;   /* auto: follow color */
    t->cols = 80; t->rows = 24;   /* headless defaults; the live loop refreshes from the terminal */
    t->show_tasks  = (t->cfg.show_tasks  >= 0);   /* default on; <0 hides */
    t->show_recent = (t->cfg.show_recent >= 0);
    max = (t->cfg.max_recent > 0) ? t->cfg.max_recent : 8;
    if (max > 64) max = 64;
    t->cfg.max_recent = max;
    t->rcap = max;
    t->kpi = (t->cfg.kpi == GPTPS_TUI_KPI_DEFAULT) ? GPTPS_TUI_KPI_FULL : t->cfg.kpi;
    if (t->kpi < GPTPS_TUI_KPI_OFF) t->kpi = GPTPS_TUI_KPI_OFF;
    if (t->kpi > GPTPS_TUI_KPI_FULL) t->kpi = GPTPS_TUI_KPI_FULL;
    t->off = (t->kpi == GPTPS_TUI_KPI_OFF);
    /* Resolved BEFORE either ring is sized, because installing straight at OFF must hold
     * no per-event memory - the same state gptps_tui_set_kpi(OFF) reclaims down to. Both
     * rings are built on the way up, so raising the tier later costs only the calloc. */
    t->recent = NULL;
    if (!t->off) {
        t->recent = (tui_event *)calloc((size_t)max, sizeof(tui_event));
        if (!t->recent) { mu_destroy(&t->mu); free(t); return NULL; }
    }
    t->mode = t->cfg.mode;
    if (t->mode < GPTPS_TUI_CONTINUOUS || t->mode > GPTPS_TUI_PAUSED) t->mode = GPTPS_TUI_CONTINUOUS;
    t->lat_cap = (t->cfg.latency_window > 0) ? t->cfg.latency_window : 1024;
    if (t->lat_cap > (1 << 20)) t->lat_cap = (1 << 20);
    t->lat = NULL;         /* the latency ring exists only at KPI FULL (else its RAM is reclaimed) */
    if (t->kpi == GPTPS_TUI_KPI_FULL)
        t->lat = calloc((size_t)t->lat_cap, sizeof *t->lat);
    t->start_ms = gptps_now_ms(NULL);
    if (gptps_register_observer(e, tui_on_event, t) != GPTPS_OK) {
        /* t->lat too - it is 16 KB by default and up to 16 MB with cfg.latency_window
         * at its cap, and the caller gets NULL back with no handle to reclaim it.
         * Unconditional: t->lat is explicitly NULL below KPI FULL. Mirrors tui_close. */
        free(t->lat); free(t->recent); mu_destroy(&t->mu); free(t); return NULL;
    }
    tui_register_settings(e, t);   /* expose tui.refresh_ms / tui.kpi / tui.mode */
    return t;
}

void gptps_tui_close(gptps_tui *t)
{
    int i;
    if (!t) return;
    term_restore(t);                 /* in case run() was interrupted */
    for (i = 0; i < t->ntasks; ++i) free(t->tasks[i].payload);
    if (t->nheld) {                  /* never silent: they were taken from the engine */
        char msg[160];
        size_t k;
        snprintf(msg, sizeof msg, "gptps_tui_close: %lu dead letter(s) held by the dashboard were discarded",
                 (unsigned long)t->nheld);
        gptps_log(NULL, GPTPS_LOG_WARN, msg);
        for (k = 0; k < t->nheld; ++k) held_free(&t->held[k]);
    }
    free(t->held);
    free(t->recent);
    free(t->lat);
    mu_destroy(&t->mu);
    free(t);
}

/* ---- runtime reconfiguration ---- */
/* caller holds mu: make the allocations and the off-path flag match the KPI level.
 * The latency ring exists only at FULL; the recent-event ring only above OFF, which
 * is the tier that exists to hold no per-event memory at all. Both are rebuilt on the
 * way back up, so raising the tier again is an allocation and nothing more. */
static void lat_apply(gptps_tui *t)
{
    if (t->kpi == GPTPS_TUI_KPI_FULL) {
        if (!t->lat) { t->lat = calloc((size_t)t->lat_cap, sizeof *t->lat); t->lat_head = 0; }
    } else if (t->lat) {
        free(t->lat); t->lat = NULL; t->lat_head = 0; t->lat_tomb = 0;   /* reclaim the RAM */
    }
    if (t->kpi == GPTPS_TUI_KPI_OFF) {
        if (t->recent) { free(t->recent); t->recent = NULL; t->rn = t->rhead = 0; }
    } else if (!t->recent && t->rcap > 0) {
        t->recent = (tui_event *)calloc((size_t)t->rcap, sizeof(tui_event));
        if (!t->recent) t->rcap = 0;      /* out of memory: run without the log, not without the dashboard */
    }
    /* Published LAST: tui_on_event reads this without the lock, so everything it
     * guards must already be consistent before the event path is allowed back in. */
    t->off = (t->kpi == GPTPS_TUI_KPI_OFF);
}

/* Resize the latency ring at runtime. The old contents are dropped rather than
 * copied: every entry is an in-flight handle awaiting its FINISHED, and a resize is
 * an admission that the window was wrong, so carrying a window's worth of samples
 * that were already being lost buys nothing. Samples resume on the next QUEUED. */
gptps_status gptps_tui_set_latency_window(gptps_tui *t, int handles)
{
    if (!t || handles < 0) return GPTPS_E_INVAL;
    if (handles == 0)          handles = 1024;
    if (handles > (1 << 20))   handles = (1 << 20);
    mu_lock(&t->mu);
    if (handles != t->lat_cap) {
        t->lat_cap = handles;
        if (t->lat) {                       /* rebuild at the new size; FULL only */
            free(t->lat);
            t->lat = calloc((size_t)t->lat_cap, sizeof *t->lat);
            t->lat_head = 0; t->lat_tomb = 0;
        }
        t->dirty = 1;
    }
    mu_unlock(&t->mu);
    return GPTPS_OK;
}

gptps_status gptps_tui_set_kpi(gptps_tui *t, gptps_tui_kpi level)
{
    if (!t) return GPTPS_E_INVAL;
    if (level == GPTPS_TUI_KPI_DEFAULT) level = GPTPS_TUI_KPI_FULL;
    if (level < GPTPS_TUI_KPI_OFF || level > GPTPS_TUI_KPI_FULL) return GPTPS_E_INVAL;
    mu_lock(&t->mu);
    t->kpi = level;
    lat_apply(t);            /* allocate at FULL, free below FULL */
    t->dirty = 1;
    mu_unlock(&t->mu);
    return GPTPS_OK;
}

gptps_status gptps_tui_set_mode(gptps_tui *t, gptps_tui_mode mode)
{
    if (!t) return GPTPS_E_INVAL;
    if (mode < GPTPS_TUI_CONTINUOUS || mode > GPTPS_TUI_PAUSED) return GPTPS_E_INVAL;
    mu_lock(&t->mu); t->mode = mode; t->dirty = 1; mu_unlock(&t->mu);
    return GPTPS_OK;
}

gptps_status gptps_tui_set_refresh(gptps_tui *t, uint32_t refresh_ms)
{
    if (!t) return GPTPS_E_INVAL;
    mu_lock(&t->mu); t->cfg.refresh_ms = refresh_ms ? refresh_ms : 250; mu_unlock(&t->mu);
    return GPTPS_OK;
}

void gptps_tui_snapshot(gptps_tui *t)
{
    char *buf; size_t cap = 16384;
    if (!t) return;
    buf = (char *)malloc(cap);
    if (!buf) return;
    gptps_tui_render(t, buf, cap);     /* one frame, right now, regardless of mode */
    fputs(buf, t->cfg.out); fputc('\n', t->cfg.out); fflush(t->cfg.out);
    free(buf);
}
