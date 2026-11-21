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
/* The one word the event path reads without the lock (see `off`): the compiler's atomic
 * builtins where there are some - no HAL, so the drop-in build has them too - else a
 * volatile access (MSVC gives volatile acquire/release semantics on its targets). */
#if defined(__GNUC__) || defined(__clang__)
#  define TUI_FLAG_GET(p)    __atomic_load_n((p), __ATOMIC_ACQUIRE)
#  define TUI_FLAG_SET(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#else
#  define TUI_FLAG_GET(p)    (*(p))
#  define TUI_FLAG_SET(p, v) (*(p) = (v))
#endif

/* cfg.max_tasks: the TASKS rows (and gptps_tui_add_task entries) by default, and the most
 * an install may ask for. */
#define TUI_MAX_TASKS 64
#define TUI_TASKS_CAP 4096
/* The latency window's cap (cfg.latency_window / tui.latency_window). */
#define TUI_LAT_CAP   (1 << 20)
/* Terminal events that outran the event they are owed (their QUEUED, or a RETRIED) are
 * kept as markers in the handle table, and the newest this many are remembered in a FIFO:
 * the oldest marker is dropped (and counted) when a newer one needs the room, so markers
 * whose late event never comes - work queued before the table existed, an always-up
 * service's later runs - cannot pile up. See "handle table". */
#define TUI_ORPHANS   64
/* A row and a RECENT record hold the FULL task name: the lookup compares whole names, so
 * a shorter copy made every 64..127-byte name a new row on every event, and one such task
 * used up all 64 rows. The label is display only. */
#define TUI_NAME_CAP  (GPTPS_TASK_NAME_MAX + 1)
#define TUI_LABEL_CAP 64
/* Task types past max_tasks fold into the '(other)' row; the distinct ones are counted by
 * a hash of the name in a set of this many slots (filled to 3/4, then "or more"). */
#define TUI_FOLD_SLOTS 256
#define TUI_FOLD_MAX   192
/* A FAILED's "why": at most this many bytes of its stderr tail are looked at, from the
 * end, for the last line - a 64 KiB tail with no newline was scanned whole, under the
 * lock, on the thread that emitted the event. */
#define TUI_WHY_SCAN  512
/* A toast or status message is shown for this long, on every pane. */
#define TUI_TOAST_MS  3000u
/* The RECENT journals' sizes (cfg.recent_lines / activity_lines, tui.recent / tui.activity):
 * the most a journal may hold, and the default (1.10's max_recent default). */
#define TUI_JOURNAL_CAP 4096
#define TUI_JOURNAL_DEF 8
/* An ACTIVITY record's task name, when there is no TASKS table to name it from by row. */
#define TUI_ACT_NAME  64
/* Latency percentiles (FULL): a log2-ms histogram per row - bucket 0 holds 0 ms, bucket b
 * (1..30) [2^(b-1), 2^b) ms, bucket 31 everything from 2^30 ms. */
#define TUI_HIST_B    32

/* ---- the KPI ladder ----------------------------------------------------------------
 * The public gptps_tui_kpi values keep their 1.10 numbers, so they are not in ladder
 * order; the event path and the storage work on the TIER (0..7), each tier the one below
 * plus one layer (gptps_tui.h). */
enum { TUI_T_OFF = 0, TUI_T_COUNT, TUI_T_GAUGE, TUI_T_PROBLEMS, TUI_T_TASKS, TUI_T_ACTIVITY,
       TUI_T_LATENCY, TUI_T_FULL };
/* the gptps_tui_kpi of each tier (the name it reads back as) */
static const int TUI_LADDER[TUI_T_FULL + 1] = {
    GPTPS_TUI_KPI_OFF, GPTPS_TUI_KPI_MINIMAL, GPTPS_TUI_KPI_GAUGE, GPTPS_TUI_KPI_PROBLEMS,
    GPTPS_TUI_KPI_TASKS, GPTPS_TUI_KPI_NORMAL, GPTPS_TUI_KPI_LATENCY, GPTPS_TUI_KPI_FULL
};
/* The tier of a gptps_tui_kpi value (DEFAULT is FULL, ACTIVITY is NORMAL), -1 for none. */
static int kpi_tier(int k)
{
    switch (k) {
        case GPTPS_TUI_KPI_OFF:      return TUI_T_OFF;
        case GPTPS_TUI_KPI_MINIMAL:  return TUI_T_COUNT;
        case GPTPS_TUI_KPI_GAUGE:    return TUI_T_GAUGE;
        case GPTPS_TUI_KPI_PROBLEMS: return TUI_T_PROBLEMS;
        case GPTPS_TUI_KPI_TASKS:    return TUI_T_TASKS;
        case GPTPS_TUI_KPI_NORMAL:
        case GPTPS_TUI_KPI_ACTIVITY: return TUI_T_ACTIVITY;
        case GPTPS_TUI_KPI_LATENCY:  return TUI_T_LATENCY;
        case GPTPS_TUI_KPI_DEFAULT:
        case GPTPS_TUI_KPI_FULL:     return TUI_T_FULL;
        default:                     return -1;
    }
}

/* panes: 0 dashboard, 1 settings editor, 2 help, 3 tasks, 4 task detail, 5 dead-letter */
enum { TUI_PANE_DASH = 0, TUI_PANE_SETTINGS, TUI_PANE_HELP, TUI_PANE_TASKS, TUI_PANE_DETAIL, TUI_PANE_DL };
/* active prompt / confirm in the tasks pane (0 = none) */
enum { TUI_PK_NONE = 0, TUI_PK_CONFIRM_DELETE, TUI_PK_CLONE_NAME, TUI_PK_NEW_NAME, TUI_PK_NEW_ARGV };

/* What gptps_tui_add_task said about a task type: its label, hotkey and payload. Kept at
 * every KPI level (the tiers own the counting, not the operator's bindings), in an array
 * that grows only in add_task, up to max_tasks. */
typedef struct {
    char     name[TUI_NAME_CAP], label[TUI_LABEL_CAP];
    int      hotkey;
    void    *payload;
    size_t   plen;
    int      has_opts;                 /* gptps_tui_add_task_ex: the hotkey submits with these */
    gptps_submit_options opts;
} tui_ui;

/* A TASKS row: one task type's counts. Rows are found by name through an open-addressing
 * index (FNV-1a of the whole name, then one strcmp), are never reused while the table
 * exists, so a row id names one type for good, and row 0 is '(other)': the types that
 * arrive once all max_tasks rows are taken are counted there instead of nowhere. */
typedef struct {
    char     name[TUI_NAME_CAP];
    uint64_t hash;                     /* name_hash(name): compared before the strcmp */
    int      ui;                       /* its tui_ui entry, or -1 (label = name, no hotkey) */
    /* 64-bit: a 32-bit count wraps after ~12 h at 100k events/s */
    uint64_t started, finished, failed, retried, dead, dropped;
    /* queue->finish latency of FINISHED items only: an item that ends dead-lettered,
     * dropped or cancelled releases its entry without a sample */
    uint64_t lat_sum_ms, lat_max_ms, lat_n;
    /* The highest mem.peak an attempt reported (docs/MEASUREMENTS.md), with its method:
     * values measured differently are not compared, so a different method replaces the
     * column's value and its method rather than joining them. */
    uint64_t mem_peak;
    char     mem_method[40];
} tui_task;

/* A handle-table slot (see "handle table"). `ts` is the QUEUED time once the QUEUED has
 * been seen, and before that the time of a FINISHED that outran it (owe_fin), whose
 * sample the late QUEUED then takes. */
enum { TS_NONE = 0, TS_PENDING, TS_RUNNING, TS_LIMBO, TS_DONE };
typedef struct {
    gptps_handle h;                    /* 0: empty (the core's handles start at 1) */
    uint64_t     ts;
    uint32_t     att;                  /* highest attempt started or ended this cycle */
    uint32_t     ann;                  /* highest attempt a RETRIED announced this cycle */
    uint8_t      state, seen_q, owe_fin;
} tui_slot;

/* A RECENT record. Why a FAILED failed is kept as the event left it - the dashboard's own
 * words (exit code, signal, start error or status) and the RAW bytes of its stderr's last
 * line (at most TUI_TAIL_KEEP, cut at a character) - and cleaned when a frame shows it
 * (why_join): cleaning on the event path cost more than the rest of the event. */
#define TUI_TAIL_KEEP 80
typedef struct { uint64_t ts; int kind; char name[TUI_NAME_CAP]; uint64_t handle; gptps_status status;
                 char why[64];                  /* its own words: "exit 3", "task failed", ... */
                 char tail[TUI_TAIL_KEEP];      /* the raw stderr line (not NUL-terminated) */
                 unsigned char tail_len, tail_cut; } tui_event;

/* An ACTIVITY record: 24 bytes. Its task is named by its TASKS row - a row id names one
 * type for the life of the table, which outlives the journal - or, when the journal was
 * built with no table to name from, by a copy beside it (act_nm), cut at a character; -1:
 * the copy. */
typedef struct { uint64_t ts; gptps_handle handle; int32_t row; uint8_t kind; } tui_act;

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
        size_t len;
        const char *rep = NULL;
        int w;
        if (u[i] >= 0x20 && u[i] < 0x7f) {                   /* printable ASCII, the common case */
            if (maxcols >= 0 && used + 1 > maxcols) break;
            if (!measure) { if (pos + 2 > cap) break; out[pos] = (char)u[i]; }
            ++pos; ++used; ++i;
            continue;
        }
        len = u8_decode(u + i, n - i, &cp);
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
    size_t pos;
    if (!cap) return "";
    pos = tui_clean(s ? s : "", s ? strlen(s) : 0, out, cap, cols, &used);
    if (pos >= cap) pos = cap - 1;           /* never past the buffer, whatever came back */
    for (; used < cols && pos + 1 < cap; ++pos, ++used) out[pos] = ' ';
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
 * 2.8), cleaned (tui_clean). From an event or a dead letter alike. In three steps, so the
 * event path does only the cheap ones: why_head (its own words), why_tail (which bytes
 * of the tail), and why_join (the line, cleaned - at render for an event). */
static size_t why_head(const gptps_measure *start_err, const gptps_measure *exit_code,
                       const gptps_measure *exit_sig, gptps_status status, char *out, size_t n)
{
    int k;
    if (start_err)      k = snprintf(out, n, "not started (error %llu)", (unsigned long long)start_err->value);
    else if (exit_code) k = snprintf(out, n, "exit %llu", (unsigned long long)exit_code->value);
    else if (exit_sig)  k = snprintf(out, n, "signal %llu", (unsigned long long)exit_sig->value);
    else                k = snprintf(out, n, "%s", gptps_strerror(status));
    if (k < 0) { out[0] = 0; return 0; }
    return (size_t)k >= n ? n - 1 : (size_t)k;
}

/* Which bytes of a stderr tail a why shows: [*start, *end), its last line without the
 * trailing blanks, at most `keep` bytes. Only the last TUI_WHY_SCAN bytes are looked at:
 * this runs under the lock on the thread that emitted the FAILED, and a 64 KiB tail with
 * no newline was walked whole to find a line start. When that window holds no line
 * break, the line is longer than it: its END is kept (what was written last), from a
 * whole character, and 1 is returned - the line is shown after an ellipsis. A line that
 * starts in the window but is longer than `keep` keeps its start, as before. */
static int why_tail(const char *tail, size_t tail_len, size_t keep, size_t *start, size_t *end)
{
    size_t lo = tail_len > TUI_WHY_SCAN ? tail_len - TUI_WHY_SCAN : 0, e = tail_len, s;
    int cut = 0;
    while (e > lo && (tail[e - 1] == '\n' || tail[e - 1] == '\r' || tail[e - 1] == ' ')) --e;
    s = e;
    while (s - lo >= 8) {                                        /* its last line: 8 bytes a step */
        uint64_t v;
        memcpy(&v, tail + s - 8, 8);
        v ^= 0x0a0a0a0a0a0a0a0aULL;                              /* a '\n' byte becomes 0 */
        if ((v - 0x0101010101010101ULL) & ~v & 0x8080808080808080ULL) break;   /* one is in these 8 */
        s -= 8;
    }
    while (s > lo && tail[s - 1] != '\n') --s;
    if (s == lo && lo > 0 && tail[lo - 1] != '\n') cut = 1;     /* ...longer than the window */
    if (e - s > keep) {
        if (cut) {
            s = e - keep;
            while (s < e && ((unsigned char)tail[s] & 0xc0u) == 0x80u) ++s;   /* a whole character */
        } else {
            e = s + keep;
            while (e > s && ((unsigned char)tail[e] & 0xc0u) == 0x80u) --e;   /* tail[e] starts the next one */
        }
    }
    *start = s; *end = e;
    return cut;
}

/* head + " \xc2\xb7 " + (an ellipsis when cut) + the raw line, cleaned, into out (n bytes). A cut
 * line keeps its end when it does not all fit: cleaning never makes text longer (a byte
 * it replaces becomes one byte), so the last `room` raw bytes fit. */
static void why_join(const char *head, const char *raw, size_t rawlen, int cut, char *out, size_t n)
{
    size_t pos;
    if (!n) return;
    pos = (size_t)snprintf(out, n, "%s", head);
    if (pos >= n) pos = n - 1;
    if (!rawlen || pos + 6 >= n) return;
    memcpy(out + pos, " \xc2\xb7 ", 4); pos += 4; out[pos] = 0;
    if (cut && pos + 4 < n) {
        size_t room = n - pos - 4, s = 0;
        memcpy(out + pos, "\xe2\x80\xa6", 3); pos += 3; out[pos] = 0;
        if (rawlen > room) {
            s = rawlen - room;
            while (s < rawlen && ((unsigned char)raw[s] & 0xc0u) == 0x80u) ++s;
        }
        raw += s; rawlen -= s;
    }
    tui_clean(raw, rawlen, out + pos, n - pos, -1, NULL);
}

static void why_line(const gptps_measure *start_err, const gptps_measure *exit_code,
                     const gptps_measure *exit_sig, gptps_status status,
                     const char *tail, size_t tail_len, char *out, size_t n)
{
    char head[64];
    size_t s = 0, e = 0;
    int cut = 0;
    out[0] = 0;
    if (n < 8) return;
    why_head(start_err, exit_code, exit_sig, status, head, sizeof head);
    if (tail && tail_len) cut = why_tail(tail, tail_len, n, &s, &e);
    why_join(head, tail ? tail + s : NULL, e - s, cut, out, n);
}

/* The event path's part: a FAILED's own words and the raw bytes of its stderr line, into
 * the record; nothing cleaned (see tui_event). */
static void why_record(const gptps_event *ev, tui_event *re)
{
    int has_tail = ev->struct_size >= offsetof(gptps_event, stderr_tail_len) + sizeof ev->stderr_tail_len;
    re->why[0] = 0; re->tail_len = 0; re->tail_cut = 0;
    if (ev->kind != GPTPS_EV_FAILED) return;
    why_head(gptps_event_measure(ev, GPTPS_M_START_ERROR), gptps_event_measure(ev, GPTPS_M_EXIT_CODE),
             gptps_event_measure(ev, GPTPS_M_EXIT_SIGNAL), ev->status, re->why, sizeof re->why);
    if (has_tail && ev->stderr_tail && ev->stderr_tail_len) {
        size_t s, e;
        re->tail_cut = (unsigned char)why_tail(ev->stderr_tail, ev->stderr_tail_len, TUI_TAIL_KEEP, &s, &e);
        memcpy(re->tail, ev->stderr_tail + s, e - s);
        re->tail_len = (unsigned char)(e - s);
    }
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
    /* Serialises the setters that swap storage in and out (see "storage"), so one of them
     * can allocate with `mu` released and still find, when it takes `mu` back, the state it
     * allocated for. Taken before `mu`, never while holding it. */
    tui_mutex        smu;
    uint64_t         start_ms;
    /* 64-bit: 32-bit counters wrapped after ~12 h at 100k events/s. `term` counts the
     * terminal events (FINISHED, DEAD_LETTERED, DROPPED, FAILED/E_CANCELLED), so that
     * q - term is the number of items not yet ended (queued, running or waiting to retry)
     * - exact once every item's QUEUED and terminal event have been seen. */
    uint64_t         q, s, fin, fail, retr, dead, drop, peak, term;
    /* The APPROXIMATE in-flight gauge, the one shown at GAUGE..NORMAL (and at LATENCY+ without a table). Counted with a floor, not
     * derived as started - finished - failed (which wrapped to 4294967295). It cannot be
     * exact from the events alone: a FAILED / GPTPS_E_CANCELLED for an attempt that ran and
     * one for an item cancelled while queued (or parked for a retry) carry the same kind,
     * status, attempt and flags, so the event cannot say whether something left the
     * running set - every FAILED decrements it, and a cancel-in-queue undercounts while
     * other items run. The frame marks it '~'. The handle table (LATENCY and up) keeps each handle's
     * state and gives the exact gauges (g_pend, g_run). */
    uint64_t         inflight;
    /* gptps_tui_add_task's entries (see tui_ui); max_tasks is cfg.max_tasks, resolved. */
    tui_ui          *ui;
    int              nui, ui_cap, max_tasks;
    /* TASKS storage (the TASKS tier and up, while shown; NULL else): rows[0..nrows), row 0 = '(other)'; the
     * name index (tidx_mask + 1 int32 slots, -1 empty, at most half full); and the set of
     * the folded types' name hashes (TUI_FOLD_SLOTS; 0 empty). */
    tui_task        *tasks;
    int              ntasks, tasks_cap;
    int32_t         *tidx;
    uint32_t         tidx_mask;
    uint64_t        *fold;
    int              nfold, fold_more;
    /* The RECENT journals (see "journals"): rings of pcap / acap records (NULL, 0: off),
     * written at phead / ahead. *_seq counts the records ever written (never reset), *_seen
     * is *_seq when a frame last showed the journal, *_base is *_seq when the ring was built
     * (so it holds min(seq - base, cap) records), *_lost the records that left before any
     * frame showed them. prob_want / act_want: the sizes asked for (0: off). */
    tui_event       *prob;
    int              pcap, phead, prob_want;
    uint64_t         p_seq, p_seen, p_base, p_lost;
    tui_act         *act;
    char           (*act_nm)[TUI_ACT_NAME];
    int              acap, ahead, act_want;
    uint64_t         a_seq, a_seen, a_base, a_lost;
    uint64_t         frame_unseen;  /* records the last frame found overwritten (its RECENT line) */
    /* FULL: the percentile histograms, TUI_HIST_B buckets per TASKS row (NULL below) */
    uint64_t        *hist;
    uint64_t         cancel;        /* FAILED with GPTPS_E_CANCELLED (also in fail and term) */
    int              tier;          /* TUI_T_*: the tier of `kpi` */
    /* The handle table (LATENCY and up; NULL below): tab_mask + 1 slots, open addressing keyed by
     * the handle (murmur finalizer, linear probing, backward-shift delete), never more
     * than lat_cap live entries plus the markers the orphan FIFO bounds. tab_n slots are
     * used, tab_done of them markers (TS_DONE). g_pend / g_run are the EXACT gauges: the
     * slots in TS_PENDING and TS_RUNNING. trk_lb: they are a lower bound just now - the
     * table was just built (work queued before it is not in it), or a handle found it
     * full. Only trk_resync clears it, and only on the ENGINE's word that nothing is
     * queued or running while the table holds nothing live: no count of events can say
     * that (a service's every run ends with a FINISHED and no QUEUED, a REQUEUE re-admits
     * with none, and nothing from before the table was built was seen). lb_gen counts
     * the times it was set, so a resync that raced a new miss leaves it alone. */
    tui_slot        *tab;
    size_t           tab_mask, tab_n, tab_done;
    gptps_handle     orph[TUI_ORPHANS];
    int              orph_n, orph_head;
    uint64_t         g_pend, g_run;
    int              trk_lb;
    uint64_t         lb_gen;
    int              observing;     /* the observer is registered: a resync can be trusted */
    int              lat_cap;       /* tui.latency_window: the live entries the table may hold */
    uint64_t         lat_untracked; /* this window: QUEUEDs that found the table full (no sample) */
    uint64_t         lat_unmatched; /* this window: markers dropped from the orphan FIFO */
    int              lat_warned;    /* the window size the table-full WARN was given for (0: none) */
    /* gptps_tui_health: counted since install, never reset (h_*). */
    uint64_t         h_untracked, h_orphans, h_folded, h_trunc, h_nomem;
    uint64_t         h_painted, h_skipped, h_written, h_wakes;
    size_t           frame_bytes;   /* the frame buffer gptps_tui_run holds while it runs */
    int              scroll;        /* recent-log scroll offset (lines back) */
    int              kpi;           /* effective gptps_tui_kpi (never DEFAULT) */
    int              mode;          /* effective gptps_tui_mode */
    int              dirty;         /* state changed since last paint (for ON_DEMAND) */
    /* Read by tui_on_event WITHOUT the lock, so that KPI OFF can skip acquiring it at
     * all - taking a mutex to discover you have nothing to do is the entire cost this
     * tier exists to remove. Not the HAL's gptps_flag: the amalgamation ships gptps.c
     * and gptps.h only, so it is not declared there. Read and written through TUI_FLAG_*:
     * the compiler's own atomic builtins where it has them (GCC, Clang - so a thread
     * sanitizer sees a release/acquire pair, not a race), else a volatile word. The race
     * is benign by construction either way - one writer (tui_reconfig, under the lock), a
     * single word, and a stale read costs one extra event processed or skipped at the
     * instant the tier changes: the event path re-reads every part under the lock. */
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

/* Copy src into dst (cap bytes, NUL-terminated), cut at a UTF-8 character boundary
 * when it does not fit, so a shortened label never ends in half a character. */
static void copy_u8(char *dst, size_t cap, const char *src)
{
    size_t n;
    if (!cap) return;
    n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n && ((unsigned char)src[n] & 0xc0u) == 0x80u) --n;   /* src[n] starts the next character */
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* FNV-1a of a name: the name index hashes by it, and the folded types are counted by it. */
static uint64_t name_hash(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}

static size_t next_pow2(size_t n) { size_t p = 1; while (p < n) p <<= 1; return p; }

/* ---- the name index (TASKS) ------------------------------------------------------
 * rows[1..ntasks) are the task types in the order they were first seen; their ids never
 * change while the table exists. tidx holds row ids by name_hash, open addressing with
 * linear probing at most half full, so a lookup is one or two probes and one strcmp,
 * whatever the number of types: the old lookup compared the name against every row
 * (33 -> 250 ns an event from 1 to 64 types). A type that arrives once every row is
 * taken is counted in row 0, '(other)', and its name hash goes in the fold set, so the
 * types folded are counted, distinct (up to TUI_FOLD_MAX of them, then "or more"). */

/* The row for `name` (hash h), or -1 when it has none. Caller holds mu, rows exist. */
static int row_lookup(const gptps_tui *t, const char *name, uint64_t h)
{
    uint32_t i = (uint32_t)h & t->tidx_mask;
    for (;; i = (i + 1) & t->tidx_mask) {
        int32_t r = t->tidx[i];
        if (r < 0) return -1;
        if (t->tasks[r].hash == h && strcmp(t->tasks[r].name, name) == 0) return r;
    }
}

/* A new row for `name` (hash h, not in the index), or -1 when every row is taken. */
static int row_add(gptps_tui *t, const char *name, uint64_t h)
{
    uint32_t i = (uint32_t)h & t->tidx_mask;
    size_t n = strlen(name);
    tui_task *k;
    if (t->ntasks >= t->tasks_cap || n > GPTPS_TASK_NAME_MAX) return -1;
    while (t->tidx[i] >= 0) i = (i + 1) & t->tidx_mask;
    k = &t->tasks[t->ntasks];
    memset(k, 0, sizeof *k);
    memcpy(k->name, name, n + 1);             /* whole: what the lookup compares */
    k->hash = h; k->ui = -1;
    t->tidx[i] = (int32_t)t->ntasks;
    return t->ntasks++;
}

/* A type with no row: count it in the fold set. 1 for the first type ever folded - the
 * caller WARNs, once, after it drops the lock. */
static int fold_note(gptps_tui *t, uint64_t h)
{
    uint32_t i;
    if (!h) h = 1;                            /* 0 marks an empty slot */
    for (i = (uint32_t)h & (TUI_FOLD_SLOTS - 1); t->fold[i]; i = (i + 1) & (TUI_FOLD_SLOTS - 1))
        if (t->fold[i] == h) return 0;
    if (t->nfold >= TUI_FOLD_MAX) { t->fold_more = 1; return 0; }
    t->fold[i] = h; ++t->nfold;
    return ++t->h_folded == 1;
}

/* The row an event of task type `name` counts in: its own, made on first sight, or
 * '(other)' once the table is full (*first_fold set for the first type folded ever).
 * NULL without rows (below TASKS, hidden, or no memory for them). */
static tui_task *row_for_event(gptps_tui *t, const char *name, int *first_fold)
{
    uint64_t h;
    int r;
    if (!t->tasks) return NULL;
    h = name_hash(name);
    r = row_lookup(t, name, h);
    if (r < 0 && (r = row_add(t, name, h)) < 0) {
        if (fold_note(t, h)) *first_fold = 1;
        r = 0;
    }
    return &t->tasks[r];
}

/* The histogram bucket of a latency of l ms (see TUI_HIST_B): at most 31 shifts. */
static int hist_bucket(uint64_t l)
{
    int b = 0;
    while (l && b < TUI_HIST_B - 1) { ++b; l >>= 1; }
    return b;
}

/* A queue->finish latency sample, into the task's row (and its histogram at FULL). */
static void lat_sample(gptps_tui *t, tui_task *tk, uint64_t from, uint64_t to)
{
    uint64_t l = (to >= from) ? to - from : 0;
    if (!tk) return;
    tk->lat_sum_ms += l; tk->lat_n++;
    if (l > tk->lat_max_ms) tk->lat_max_ms = l;
    if (t->hist) t->hist[(size_t)(tk - t->tasks) * TUI_HIST_B + (size_t)hist_bucket(l)]++;
}

/* The item's life ends with this event (gptps.h, EVENTS): FINISHED, DEAD_LETTERED,
 * DROPPED, or a FAILED carrying GPTPS_E_CANCELLED - never a retry or a dead letter. A
 * FAILED attempt that is retried (or dead-lettered next) is not the end. */
static int ev_terminal(const gptps_event *ev)
{
    return ev->kind == GPTPS_EV_FINISHED || ev->kind == GPTPS_EV_DEAD_LETTERED || ev->kind == GPTPS_EV_DROPPED ||
           (ev->kind == GPTPS_EV_FAILED && ev->status == GPTPS_E_CANCELLED);
}

/* ---- handle table (LATENCY, FULL) ----------------------------------------------------
 * Keyed by handle, it gives two things nothing else here can: queue->finish latency, and
 * EXACT pending / in-flight gauges (the counters cannot tell a cancelled attempt that
 * ran from an item cancelled while queued - see `inflight`). One hash operation an event:
 * the 1.10 ring scanned its whole window for every FINISHED (89 ns at 1024, 4.5 us at
 * 65536, under the lock).
 *
 * Event order is NOT guaranteed across threads (gptps.h, EVENTS), so every transition is
 * keyed on the slot's state and on the attempt an event names, never on arrival order -
 * the rules of gptps_stats (whose order matrix tests/test_tui_order.c replays here):
 *   - a terminal event that outruns its QUEUED leaves a marker (TS_DONE) that the late
 *     QUEUED clears - taking the sample a FINISHED left owed, since those are the fastest
 *     items and dropping them would pull avg ms up;
 *   - a terminal event that outruns the RETRIED it is owed (an attempt > 1 whose RETRIED
 *     has not arrived) leaves a marker that RETRIED clears, and a late RETRIED moves
 *     nothing for an attempt already started or ended;
 *   - a STARTED with no slot (it outran its QUEUED) opens one, running.
 * Every terminal kind - FINISHED, DEAD_LETTERED, DROPPED, FAILED/E_CANCELLED - deletes
 * the slot or leaves a marker; only FINISHED gives a sample.
 *
 * Bounded, never evicting: at most lat_cap live entries. A QUEUED that finds them all
 * taken is not tracked (counted, WARNed once per window, shown under the table, and the
 * gauges show '>=' - a lower bound - until trk_resync hears the engine is idle); a live entry is
 * never overwritten. Markers whose late event never comes (work queued before the table
 * existed; an always-up service's later runs, which have no QUEUED; the end of a handle
 * the table was too full to take) are bounded by the orphan FIFO: the newest
 * TUI_ORPHANS are kept, the oldest dropped and counted. The table has
 * next_pow2((lat_cap + TUI_ORPHANS) * 4 / 3 + 1) slots, so it is never more than 3/4
 * full. */

/* Where a handle's probe starts: murmur3's 64-bit finalizer over the handle's BLOCK of
 * TUI_TRK_RUN consecutive handles, the low bits kept. The engine numbers handles in
 * submission order, and items submitted together live together, so a block's handles
 * share a few cache lines instead of each taking a line of its own somewhere in the
 * table: a 1<<20 window is 64 MiB, and one cache (and TLB) miss per new handle made it
 * 3x the cost of the 64 KiB default; per block it is one miss in eight. Blocks are still
 * spread at random, so any set of handles - strided, sparse, adversarial - probes like
 * murmur-hashed keys, in runs of at most a block. */
#define TUI_TRK_RUN 8u
static size_t trk_home(gptps_handle h, size_t mask)
{
    uint64_t x = (uint64_t)h / TUI_TRK_RUN;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
    return (size_t)(x * TUI_TRK_RUN + (uint64_t)h % TUI_TRK_RUN) & mask;
}
static size_t trk_slots(int window) { return next_pow2(((size_t)window + TUI_ORPHANS) * 4 / 3 + 1); }

static tui_slot *trk_find(gptps_tui *t, gptps_handle h)
{
    size_t i;
    for (i = trk_home(h, t->tab_mask); ; i = (i + 1) & t->tab_mask) {
        if (t->tab[i].h == h) return &t->tab[i];
        if (t->tab[i].h == 0) return NULL;
    }
}

/* Move a slot to state `st`, keeping the gauges and the marker count true. */
static void trk_state(gptps_tui *t, tui_slot *sl, int st)
{
    if (sl->state == st) return;
    if (sl->state == TS_PENDING) t->g_pend--;
    else if (sl->state == TS_RUNNING) t->g_run--;
    else if (sl->state == TS_DONE) t->tab_done--;
    sl->state = (uint8_t)st;
    if (st == TS_PENDING) t->g_pend++;
    else if (st == TS_RUNNING) t->g_run++;
    else if (st == TS_DONE) t->tab_done++;
}

static tui_slot *trk_insert(gptps_tui *t, gptps_handle h)
{
    size_t i = trk_home(h, t->tab_mask);
    while (t->tab[i].h) i = (i + 1) & t->tab_mask;
    memset(&t->tab[i], 0, sizeof t->tab[i]);
    t->tab[i].h = h;
    t->tab_n++;
    return &t->tab[i];
}

/* Delete a slot, tombstone-free (backward shift), so probes stay short. Moves entries:
 * any slot pointer taken before this call is stale after it. */
static void trk_del(gptps_tui *t, tui_slot *sl)
{
    size_t mask = t->tab_mask, i = (size_t)(sl - t->tab), j = i;
    trk_state(t, sl, TS_NONE);
    for (;;) {
        size_t k;
        j = (j + 1) & mask;
        if (t->tab[j].h == 0) break;
        k = trk_home(t->tab[j].h, mask);
        /* can the entry at j move to i? yes iff its home k is not in (i, j] cyclically */
        if ((i <= j) ? (k <= i || k > j) : (k <= i && k > j)) { t->tab[i] = t->tab[j]; i = j; }
    }
    memset(&t->tab[i], 0, sizeof t->tab[i]);
    t->tab_n--;
}

/* Remember a new marker; drop the oldest when the FIFO is full (it may delete a slot). */
static void trk_orphan(gptps_tui *t, gptps_handle h)
{
    if (t->orph_n == TUI_ORPHANS) {
        tui_slot *o = trk_find(t, t->orph[t->orph_head]);
        if (o && o->state == TS_DONE) { trk_del(t, o); t->h_orphans++; t->lat_unmatched++; }
        t->orph[t->orph_head] = h;
        t->orph_head = (t->orph_head + 1) % TUI_ORPHANS;
    } else {
        t->orph[(t->orph_head + t->orph_n) % TUI_ORPHANS] = h;
        t->orph_n++;
    }
}

/* An event of attempt N > 1 has arrived and the RETRIED announcing attempt N has not. */
#define TRK_OWES_RETRIED(sl) ((sl)->att > 1 && (sl)->ann < (sl)->att)

/* The handle's life is over: delete its slot, or keep it as a marker for the event it
 * still owes (its QUEUED, or a RETRIED). Last use of `sl`. */
static void trk_finish(gptps_tui *t, tui_slot *sl)
{
    if (sl->seen_q && !TRK_OWES_RETRIED(sl)) { trk_del(t, sl); return; }
    if (sl->state != TS_DONE) {
        gptps_handle h = sl->h;
        trk_state(t, sl, TS_DONE);
        trk_orphan(t, h);
    }
}

/* One event into the handle table; `tk` is the event's row (for the sample), or NULL.
 * Returns 1 when a QUEUED found the table full for the first time at this window size
 * (the caller WARNs once it drops the lock). */
static int trk_event(gptps_tui *t, const gptps_event *ev, tui_task *tk)
{
    tui_slot *sl;
    int term = ev_terminal(ev), warn = 0;
    if (!ev->handle) return 0;
    sl = trk_find(t, ev->handle);
    if (!sl) {
        /* A terminal event may always take a slot: it becomes a marker at once, and the
         * orphan FIFO bounds those. Anything else needs a live entry's room. */
        if (!term && t->tab_n - t->tab_done >= (size_t)t->lat_cap) {
            t->trk_lb = 1; t->lb_gen++;                     /* the gauges miss this handle */
            if (ev->kind == GPTPS_EV_QUEUED) {
                t->h_untracked++; t->lat_untracked++;
                if (t->lat_warned != t->lat_cap) { t->lat_warned = t->lat_cap; warn = 1; }
            }
            return warn;
        }
        sl = trk_insert(t, ev->handle);
    }
    switch (ev->kind) {
    case GPTPS_EV_QUEUED:
        sl->seen_q = 1;
        if (sl->owe_fin) { lat_sample(t, tk, ev->ts_ms, sl->ts); sl->owe_fin = 0; }   /* the FINISHED that beat it */
        sl->ts = ev->ts_ms;
        if (sl->state == TS_DONE) {                         /* already over */
            if (!TRK_OWES_RETRIED(sl)) trk_del(t, sl);
            break;
        }
        if (sl->state == TS_NONE) trk_state(t, sl, TS_PENDING);
        break;
    case GPTPS_EV_STARTED:
        if (ev->attempt <= 1) sl->ann = 0;                  /* a new cycle (REQUEUE, service restart) */
        sl->att = ev->attempt;
        trk_state(t, sl, TS_RUNNING);
        break;
    case GPTPS_EV_FINISHED:
        if (ev->attempt > sl->att) sl->att = ev->attempt;
        if (sl->seen_q) lat_sample(t, tk, sl->ts, ev->ts_ms);
        else if (!sl->owe_fin) { sl->owe_fin = 1; sl->ts = ev->ts_ms; }   /* its QUEUED takes the sample */
        trk_finish(t, sl);
        break;
    case GPTPS_EV_FAILED:
        if (ev->attempt > sl->att) sl->att = ev->attempt;
        if (term) trk_finish(t, sl);                        /* cancelled: whether it ran or not */
        else trk_state(t, sl, TS_LIMBO);                    /* a retry, a dead letter or a drop follows */
        break;
    case GPTPS_EV_RETRIED:
        if (ev->attempt > sl->ann) sl->ann = ev->attempt;
        /* LATE: the attempt it announces has already started or ended, or the handle is
         * over. It moves nothing, and clears the marker a terminal event left for it. */
        if (sl->att >= ev->attempt || sl->state == TS_DONE) {
            if (sl->state == TS_DONE && sl->seen_q && !TRK_OWES_RETRIED(sl)) trk_del(t, sl);
            break;
        }
        trk_state(t, sl, TS_PENDING);
        break;
    case GPTPS_EV_DEAD_LETTERED:
    case GPTPS_EV_DROPPED:
        if (ev->attempt > sl->att) sl->att = ev->attempt;
        trk_finish(t, sl);
        break;
    default:
        break;
    }
    if (t->g_run > t->peak) t->peak = t->g_run;
    return warn;
}

/* 1 when the engine says no item of any type is queued (intake or backoff) or running.
 * Takes the engine's lock (gptps_task_get_info), so never under mu. */
static int engine_idle(gptps *e)
{
    gptps_task_info info;
    size_t i, n;
    if (!e) return 0;
    n = gptps_task_count(e);
    for (i = 0; i < n; ++i) {
        if (!tui_task_at(e, i, &info)) return 0;
        if (info.queued || info.running) return 0;
    }
    return gptps_task_count(e) == n;      /* the registry moved under the walk: say no */
}

/* Clear the lower-bound mark (trk_lb) when that is sound: the engine has nothing queued
 * or running, the table holds nothing live, and no handle was missed since this call
 * began (lb_gen). Then nothing alive can be missing from the table - anything later
 * sends its events to the observer, which is registered. Any other moment is not
 * sound: an end still on its way to the observer can stand in for an item the table
 * never saw. So an engine that is never idle (an always-up service) keeps '>=', which
 * stays true. Called by render, gptps_tui_health and tui_reconfig, never under mu. */
static void trk_resync(gptps_tui *t)
{
    uint64_t gen;
    mu_lock(&t->mu);
    if (!t->tab || !t->trk_lb || !t->observing) { mu_unlock(&t->mu); return; }
    gen = t->lb_gen;
    mu_unlock(&t->mu);
    if (!engine_idle(t->e)) return;
    mu_lock(&t->mu);
    if (t->tab && t->trk_lb && t->lb_gen == gen && t->tab_n == t->tab_done) { t->trk_lb = 0; t->dirty = 1; }
    mu_unlock(&t->mu);
}

/* ---- journals --------------------------------------------------------------------
 * PROBLEMS (FAILED, RETRIED, DEAD_LETTERED, DROPPED) and ACTIVITY (QUEUED, STARTED,
 * FINISHED) are separate rings, so the lifecycle flood of a busy engine - three records a
 * success - never pushes a failure out. A record's sequence number is implicit: the ring
 * holds the newest min(seq - base, cap) of the seq records written. A frame that shows the
 * journals sets seen = seq; what was written since the previous frame beyond what the ring
 * holds was overwritten unseen, and counted - exactly, with no per-event cost. */
static int ev_problem(int k)
{ return k == GPTPS_EV_FAILED || k == GPTPS_EV_RETRIED || k == GPTPS_EV_DEAD_LETTERED || k == GPTPS_EV_DROPPED; }
static int ev_activity(int k)
{ return k == GPTPS_EV_QUEUED || k == GPTPS_EV_STARTED || k == GPTPS_EV_FINISHED; }
/* records a ring holds */
static uint64_t ring_held(uint64_t seq, uint64_t base, int cap) { uint64_t n = seq - base; return cap > 0 && n > (uint64_t)cap ? (uint64_t)cap : (cap > 0 ? n : 0); }
/* records written since `seen` that the ring (cap; 0: none) no longer holds */
static uint64_t ring_unseen_lost(uint64_t seq, uint64_t seen, int cap)
{ uint64_t u = seq - seen; return u > (uint64_t)(cap > 0 ? cap : 0) ? u - (uint64_t)(cap > 0 ? cap : 0) : 0; }

static void tui_on_event(const gptps_event *ev, void *ud)
{
    gptps_tui *t = (gptps_tui *)ud;
    tui_task *tk;
    int first_fold = 0, win = 0, g;
    uint64_t depth = 0, running = 0;
    if (TUI_FLAG_GET(&t->off)) return;   /* KPI OFF: before the lock, deliberately - see the field */
    /* A running job's samples (measure.sample_ms) are not part of any item's life: they
     * would only flood the log. The peak column reads the attempts' own measurements. */
    if (ev->kind == GPTPS_EV_SAMPLE) return;
    mu_lock(&t->mu);
    g = t->tier >= TUI_T_GAUGE;          /* MINIMAL keeps the totals only */
    switch (ev->kind) {
        case GPTPS_EV_QUEUED:        t->q++;    break;
        case GPTPS_EV_STARTED:       t->s++;    if (g) t->inflight++; break;
        /* the floor is what a terminal event with no matching STARTED hits - a cancel
         * of a still-queued item, or a forced task removal (see the field comment) */
        case GPTPS_EV_FINISHED:      t->fin++;  t->term++; if (g && t->inflight) t->inflight--; break;
        case GPTPS_EV_FAILED:        /* terminal only when cancelled: else a retry or a dead letter follows */
            t->fail++;
            if (ev->status == GPTPS_E_CANCELLED) { t->term++; t->cancel++; }
            if (g && t->inflight) t->inflight--;
            break;
        case GPTPS_EV_RETRIED:       t->retr++; break;
        case GPTPS_EV_DEAD_LETTERED: t->dead++; t->term++; break;
        /* a terminal failure like a dead letter, just not retained (on_failure =
         * drop): it was missing from every count and showed as "?" in the log */
        case GPTPS_EV_DROPPED:       t->drop++; t->term++; break;
        default: break;
    }
    if (g && !t->tab && t->inflight > t->peak) t->peak = t->inflight;   /* with the table: its exact gauge */
    t->dirty = 1;                                /* something changed (for ON_DEMAND repaint) */

    /* MINIMAL and GAUGE stop here. Each part below runs only when it exists, and it exists
     * only at its tier and up (tui_reconfig); the tier test only skips the probes. */
    if (t->tier < TUI_T_PROBLEMS) { mu_unlock(&t->mu); return; }
    if (t->prob && ev_problem(ev->kind)) {       /* nothing on a success */
        tui_event *re = &t->prob[t->phead];
        re->ts = ev->ts_ms; re->kind = ev->kind; re->handle = ev->handle; re->status = ev->status;
        copy_u8(re->name, sizeof re->name, ev->task_name);   /* whole: a name is at most GPTPS_TASK_NAME_MAX */
        why_record(ev, re);
        t->phead = t->phead + 1 == t->pcap ? 0 : t->phead + 1;
        t->p_seq++;
    }
    if (t->tier < TUI_T_TASKS) { mu_unlock(&t->mu); return; }
    tk = row_for_event(t, ev->task_name, &first_fold);

    if (t->tab) {
        if (trk_event(t, ev, tk)) {
            win = t->lat_cap;
            depth = t->q > t->term ? t->q - t->term : 0;
            running = t->g_run;
        }
    }

    if (tk) {
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
            case GPTPS_EV_FINISHED:      tk->finished++; break;
            case GPTPS_EV_FAILED:        tk->failed++;   break;
            case GPTPS_EV_RETRIED:       tk->retried++;  break;
            case GPTPS_EV_DEAD_LETTERED: tk->dead++;     break;
            case GPTPS_EV_DROPPED:       tk->dropped++;  break;
            default: break;
        }
    }
    if (t->act && ev_activity(ev->kind)) {       /* 24 bytes, no copy when a row names it */
        tui_act *ra = &t->act[t->ahead];
        ra->ts = ev->ts_ms; ra->handle = ev->handle; ra->kind = (uint8_t)ev->kind;
        ra->row = tk ? (int32_t)(tk - t->tasks) : -1;
        if (!tk && t->act_nm) copy_u8(t->act_nm[t->ahead], TUI_ACT_NAME, ev->task_name);
        t->ahead = t->ahead + 1 == t->acap ? 0 : t->ahead + 1;
        t->a_seq++;
    }
    mu_unlock(&t->mu);
    /* never silent - logged outside the lock, so a log sink cannot stall the event path's lock */
    if (first_fold) {
        char msg[512], nm[GPTPS_TASK_NAME_MAX * 2 + 2];
        snprintf(msg, sizeof msg, "gptps_tui: task type '%s' gets no row of its own in the dashboard's TASKS "
                 "table, which holds %d types (cfg.max_tasks); its events count in the '(other)' row, and the "
                 "table shows how many types it folds there", tui_cl(ev->task_name, nm, sizeof nm), t->max_tasks);
        gptps_log(NULL, GPTPS_LOG_WARN, msg);
    }
    if (win) {
        char msg[640];
        snprintf(msg, sizeof msg, "gptps_tui: latency samples are being lost: the handle table of "
                 "tui.latency_window = %d entries has no room left - an entry lives from an item's QUEUED to its "
                 "end, and about %llu items were queued or running when this began (%llu running) - so new "
                 "items are not tracked: no avg ms sample, and the pending / in-flight gauges show a lower "
                 "bound ('>='); no tracked item is evicted; raise tui.latency_window (cfg.latency_window) to "
                 "cover the items in flight, long-running ones included",
                 win, (unsigned long long)depth, (unsigned long long)running);
        gptps_log(NULL, GPTPS_LOG_WARN, msg);
    }
}

/* A tier's name as tui.kpi and the frame show it: the released name where it has one. */
static const char *kpi_str(int k)
{
    static const char *const N[TUI_T_FULL + 1] = { "off", "minimal", "gauge", "problems", "tasks", "normal",
                                                    "latency", "full" };
    int r = kpi_tier(k);
    return r < 0 ? "?" : N[r];
}
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

#define TUI_TRUNC_MARK "[frame truncated]\n"
/* Where a frame cut at `cut` (exclusive) may end: not inside a UTF-8 character, and
 * not inside an escape sequence - a CSI left open makes the terminal swallow whatever
 * is printed next. */
static size_t frame_cut(const char *buf, size_t cut)
{
    size_t e;
    while (cut && ((unsigned char)buf[cut] & 0xc0u) == 0x80u) --cut;
    for (e = cut; e && cut - e < 32; --e)
        if (buf[e - 1] == '\x1b') {
            size_t z = e;                                    /* ESC [ params final(0x40..0x7e) complete? */
            if (z < cut && buf[z] == '[') { ++z; while (z < cut && (unsigned char)buf[z] < 0x40) ++z; }
            if (z >= cut) cut = e - 1;
            break;
        }
    return cut;
}

/* End a frame built with appendf: NUL-terminate it and return its length. A frame
 * that did not fit (appendf clamps pos to cap) used to be cut wherever the buffer
 * ended - inside a character, inside an escape sequence, with the colour left on and
 * no sign anything was missing. Now the last whole line that fits is kept, the
 * attributes are reset when colour is on, and a visible marker line says so. */
static size_t frame_finish(int color, char *buf, size_t cap, size_t pos)
{
    const char *reset = color ? "\x1b[0m" : "";
    size_t rl = strlen(reset), ml = sizeof TUI_TRUNC_MARK - 1, cut, k;
    if (!cap) return 0;
    if (pos < cap) { buf[pos] = 0; return pos; }             /* it fit */
    if (cap < rl + 1 + ml + 1 + 1) {                         /* no room even for the marker */
        cut = frame_cut(buf, cap - 1);                       /* still a whole character and escape */
        buf[cut] = 0;
        return cut;
    }
    cut = cap - (rl + 1 + ml + 1);                           /* room for reset, '\n', marker, NUL */
    for (k = cut; k && buf[k - 1] != '\n'; --k) { }
    cut = k ? k                                              /* after the last whole line */
            : frame_cut(buf, cut);                           /* one long line: cut it, whole */
    k = (cut && buf[cut - 1] != '\n');                       /* a line left open */
    pos = cut;
    memcpy(buf + pos, reset, rl); pos += rl;
    if (k) buf[pos++] = '\n';                                /* finish it */
    memcpy(buf + pos, TUI_TRUNC_MARK, ml); pos += ml;
    buf[pos] = 0;
    return pos;
}

/* frame_finish for a pane, counting a frame that did not fit (gptps_tui_health). Called
 * without mu held. */
static size_t tui_finish(gptps_tui *t, char *buf, size_t cap, size_t pos)
{
    if (cap && pos >= cap) { mu_lock(&t->mu); t->h_trunc++; mu_unlock(&t->mu); }
    return frame_finish(t->cfg.color, buf, cap, pos);
}

/* Whether the status / toast is still shown: every pane fades it after TUI_TOAST_MS. */
static int status_live(const char *status, uint64_t status_ms, uint64_t now)
{
    return status[0] && status_ms && now >= status_ms && now - status_ms < TUI_TOAST_MS;
}

/* A count in at most 5 columns: 12345, then 123k, 1.2M, 45G - a 64-bit counter
 * printed in full would shear the table it sits in. */
static const char *fmt_count(char *b, size_t cap, uint64_t v)
{
    static const char sfx[] = "kMGTPE";
    double x;
    int u = 0;
    if (v < 100000u) { snprintf(b, cap, "%llu", (unsigned long long)v); return b; }
    x = (double)v / 1000.0;
    while (x >= 999.5 && u < 5) { x /= 1000.0; ++u; }
    if (x < 9.95) snprintf(b, cap, "%.1f%c", x, sfx[u]);
    else          snprintf(b, cap, "%.0f%c", x, sfx[u]);
    return b;
}

/* The p-th percentile (p in 1..99) of a latency histogram (TUI_HIST_B buckets), as the
 * bucket it falls in: "0", "<16" (under 16 ms), ">=1.1G" (the last, open bucket); "--"
 * with no sample. */
static const char *hist_pct(const uint64_t *hb, unsigned p, char *out, size_t cap)
{
    uint64_t n = 0, r, c = 0;
    int b;
    char v[16];
    for (b = 0; b < TUI_HIST_B; ++b) n += hb[b];
    if (!n) { snprintf(out, cap, "--"); return out; }
    r = n / 100 * p + (n % 100 * p + 99) / 100;     /* ceil(n * p / 100), without overflow */
    if (r < 1) r = 1;
    for (b = 0; b < TUI_HIST_B - 1; ++b) { c += hb[b]; if (c >= r) break; }
    if (b == 0) snprintf(out, cap, "0");
    else if (b == TUI_HIST_B - 1) snprintf(out, cap, ">=%s", fmt_count(v, sizeof v, (uint64_t)1 << (TUI_HIST_B - 2)));
    else snprintf(out, cap, "<%s", fmt_count(v, sizeof v, (uint64_t)1 << b));
    return out;
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
    if (!status_live(t->status, t->status_ms, gptps_now_ms(NULL))) return pos;   /* faded, as on the dashboard */
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
    return tui_finish(t, buf, cap, pos);
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
    pos = appendf(buf, cap, pos, "  %-9s KPI tier up: minimal gauge problems tasks normal latency full,\n", "m");
    pos = appendf(buf, cap, pos, "  %-9s then minimal again (never off)\n", "");
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
    return tui_finish(t, buf, cap, pos);
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
    return tui_finish(t, buf, cap, pos);
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
    return tui_finish(t, buf, cap, pos);
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
    return tui_finish(t, buf, cap, pos);
}

/* The dashboard's key legend: an item per printable hotkey bound with gptps_tui_add_task
 * (the first binding of each key: the one a press submits), a separator, then the global
 * keys. Items are formatted one at a time, so their number is bounded by the keys that can
 * be typed (33..126), never by a guess: an array sized for 64 task rows plus 8 once took
 * 9 items and 64 printable hotkeys wrote one past its end. */
#define TUI_LEGEND_ITEM 96
static const char *const TUI_LEGEND_KEYS[] = {
    NULL /* the scroll keys: arrows or words, by cfg.unicode */,
    "s settings", "t tasks", "l dead-letter", "m kpi", "p pause", "? help", "q quit"
};
#define TUI_LEGEND_NKEYS ((int)(sizeof TUI_LEGEND_KEYS / sizeof TUI_LEGEND_KEYS[0]))
#define TUI_LEGEND_MAX   (94 + 1 + TUI_LEGEND_NKEYS)   /* typeable hotkeys + separator + keys */
/* what legend_text can write: every item and its gap or line break, plus "keys:" */
#define TUI_LEGEND_CAP   (TUI_LEGEND_MAX * (TUI_LEGEND_ITEM + 8) + 16)

/* The legend wrapped to W columns: one line was over 80 columns with no hotkeys bound,
 * so it wrapped and the frame was a row taller than counted. Plain text, lines joined
 * by '\n', no trailing one; returns the rows. Caller holds mu. */
static int legend_text(gptps_tui *t, int W, char *out, size_t cap)
{
    unsigned char seen[128];
    char item[TUI_LEGEND_ITEM];
    int rows = 1, col = 5, nhot = 0, step;
    size_t pos;
    memset(seen, 0, sizeof seen);
    pos = (size_t)snprintf(out, cap, "keys:");
    /* step 0: the hotkeys; 1: the separator (when there were any); 2..: the global keys */
    for (step = 0; step < 2 + TUI_LEGEND_NKEYS; ++step) {
        int last = (step == 0) ? t->nui : 1, k;
        for (k = 0; k < last; ++k) {
            int w;
            if (step == 0) {
                int hk = t->ui[k].hotkey;
                char lb[TUI_LEGEND_ITEM - 8];       /* cleaned, cut at a character, fits the item */
                if (hk <= 32 || hk >= 127 || seen[hk]) continue;   /* a key that can be typed and shown */
                seen[hk] = 1; ++nhot;
                snprintf(item, sizeof item, "[%c] %.*s", hk, (int)sizeof lb - 1, tui_cl(t->ui[k].label, lb, sizeof lb));
            } else if (step == 1) {
                if (!nhot) continue;
                snprintf(item, sizeof item, "\xc2\xb7");
            } else if (TUI_LEGEND_KEYS[step - 2]) {
                snprintf(item, sizeof item, "%s", TUI_LEGEND_KEYS[step - 2]);
            } else {
                snprintf(item, sizeof item, "%s scroll", a_ud(t));
            }
            w = u8_cols(item);
            if (col > 6 && col + 2 + w > W - 1) {     /* the next row, indented */
                pos += (size_t)snprintf(out + pos, pos < cap ? cap - pos : 0, "\n     ");
                col = 5; ++rows;
            }
            pos += (size_t)snprintf(out + pos, pos < cap ? cap - pos : 0, "%s%s", col > 5 ? "  " : " ", item);
            col += (col > 5 ? 2 : 1) + w;
            if (pos >= cap) { out[cap - 1] = 0; return rows; }
        }
    }
    return rows;
}

size_t gptps_tui_render(gptps_tui *t, char *buf, size_t cap)
{
    size_t pos = 0;
    int i, color, uni, W, toast_on, legrows;
    const char *B, *D, *R, *G, *X;
    double up, rate;
    char legend[TUI_LEGEND_CAP];
    if (!t || !buf || cap == 0) { if (buf && cap) buf[0] = 0; return 0; }
    if (t->pane == TUI_PANE_SETTINGS) return render_settings(t, buf, cap);
    if (t->pane == TUI_PANE_HELP)     return render_help(t, buf, cap);
    if (t->pane == TUI_PANE_TASKS)    return render_tasks(t, buf, cap);
    if (t->pane == TUI_PANE_DETAIL)   return render_detail(t, buf, cap);
    if (t->pane == TUI_PANE_DL)       return render_deadletter(t, buf, cap);
    color = t->cfg.color; uni = t->cfg.unicode; W = draw_width(t);
    B = color ? "\x1b[1m" : ""; D = color ? "\x1b[2m" : ""; R = color ? "\x1b[31m" : "";
    G = color ? "\x1b[32m" : ""; X = color ? "\x1b[0m" : "";

    trk_resync(t);                       /* before the lock: it asks the engine */
    mu_lock(&t->mu);
    /* This frame shows the journals: what left them since the last one was not shown. */
    {
        uint64_t lp = ring_unseen_lost(t->p_seq, t->p_seen, t->prob ? t->pcap : 0),
                 la = ring_unseen_lost(t->a_seq, t->a_seen, t->act ? t->acap : 0);
        t->p_lost += lp; t->a_lost += la; t->p_seen = t->p_seq; t->a_seen = t->a_seq;
        t->frame_unseen = lp + la;
    }
    toast_on = status_live(t->status, t->status_ms, gptps_now_ms(NULL));
    legrows = legend_text(t, W, legend, sizeof legend);
    up = (double)(gptps_now_ms(NULL) - t->start_ms) / 1000.0;
    rate = (up > 0.05) ? (double)t->fin / up : 0.0;

    /* header: a framed title bar when color is on; a plain line otherwise */
    if (color) { pos = titlebar(buf, cap, pos, t, W, up, rate); pos = rule(buf, cap, pos, W, uni); }
    else pos = appendf(buf, cap, pos, "GPTPS \xc2\xb7 %s   up %.1fs   %.1f done/s\n", t->cfg.title, up, rate);

    if (t->tier < TUI_T_GAUGE) {     /* MINIMAL keeps no gauge: what is alive, as one sum */
        char pc[16];
        uint64_t alive = t->q > t->term ? t->q - t->term : 0;
        pos = appendf(buf, cap, pos, "queued %llu  started %llu  pending+in-flight ~%s\n",
                      (unsigned long long)t->q, (unsigned long long)t->s, fmt_count(pc, sizeof pc, alive));
    } else {   /* the gauges, and an in-flight bar fitted to what the line leaves of the width */
        /* EXACT from the handle table (LATENCY+); '>=' while that is a lower bound (see
         * trk_lb); '~' from the counters, which cannot be exact (see `inflight`) */
        int exact = t->tab != NULL;
        const char *mk = exact ? (t->trk_lb ? ">=" : "") : "~";
        uint64_t inflt = exact ? t->g_run : t->inflight;
        uint64_t alive = t->q > t->term ? t->q - t->term : 0;
        uint64_t pend = exact ? t->g_pend : (alive > inflt ? alive - inflt : 0);
        unsigned j, gw, w;
        int ll, rl;
        char left[160], right[48], pc[16];
        const char *fillc = uni ? "\xe2\x96\x88" : "#";   /* full block / hash */
        const char *trakc = uni ? "\xe2\x96\x91" : ".";   /* light shade / dot  */
        const char *C = color ? "\x1b[36m" : "";          /* cyan bar */
        ll = snprintf(left, sizeof left, "queued %llu  started %llu  pending %s%s  in-flight %s%llu  ",
                      (unsigned long long)t->q, (unsigned long long)t->s, mk, fmt_count(pc, sizeof pc, pend), mk,
                      (unsigned long long)inflt);
        rl = snprintf(right, sizeof right, " peak %llu", (unsigned long long)t->peak);
        gw = (unsigned)(W / 3); if (gw > 40) gw = 40;
        if ((int)gw > W - 1 - ll - 2 - rl) gw = (unsigned)(W - 1 - ll - 2 - rl > 0 ? W - 1 - ll - 2 - rl : 0);
        if (gw < 10) gw = 10;
        w = (t->peak && inflt) ? (unsigned)((double)inflt * gw / (double)t->peak) : 0; if (w > gw) w = gw;
        pos = appendf(buf, cap, pos, "%s%s[%s", left, D, C);
        for (j = 0; j < gw; ++j) pos = appendf(buf, cap, pos, "%s", j < w ? fillc : trakc);
        pos = appendf(buf, cap, pos, "%s%s]%s%s\n", X, D, X, right);
    }
    {
        char cn[48];
        cn[0] = 0;                                   /* cancelled: part of failed, named when any */
        if (t->cancel) snprintf(cn, sizeof cn, " (%llu cancelled)", (unsigned long long)t->cancel);
        pos = appendf(buf, cap, pos, "%sfinished %llu%s  %sfailed %llu%s%s  retried %llu  %sdead %llu%s  %sdropped %llu%s\n",
                      G, (unsigned long long)t->fin, X, (t->fail ? R : ""), (unsigned long long)t->fail, X, cn,
                      (unsigned long long)t->retr, (t->dead ? R : ""), (unsigned long long)t->dead, X,
                      (t->drop ? R : ""), (unsigned long long)t->drop, X);
    }
    {   /* the tier, and the parts it runs without for want of memory (logged) */
        char kl[320];
        int r = t->tier;
        size_t kp = (size_t)snprintf(kl, sizeof kl, "kpi:%s mode:%s refresh:%ums", kpi_str(t->kpi),
                                     mode_str(t->mode), t->cfg.refresh_ms);
        const char *miss[5];
        int nm = 0, k;
        if (r >= TUI_T_PROBLEMS && t->show_recent && t->prob_want && !t->prob) miss[nm++] = "problems log";
        if (r >= TUI_T_TASKS && t->show_tasks && !t->tasks)                    miss[nm++] = "tasks";
        if (r >= TUI_T_ACTIVITY && t->show_recent && t->act_want && !t->act)   miss[nm++] = "activity log";
        if (r >= TUI_T_LATENCY && !t->tab)                                     miss[nm++] = "latency";
        if (r >= TUI_T_FULL && t->tasks && !t->hist)                           miss[nm++] = "percentiles";
        pos = appendf(buf, cap, pos, "%s%.*s%s\n", D, (int)u8_fit(kl, W - 1), kl, X);
        kp = 0;                                      /* and, when any, the parts it lacks: a line of its own */
        for (k = 0; k < nm && kp < sizeof kl; ++k)
            kp += (size_t)snprintf(kl + kp, sizeof kl - kp, "%s%s", k ? ", " : "no memory for: ", miss[k]);
        if (nm) pos = appendf(buf, cap, pos, "%s%.*s%s\n", R, (int)u8_fit(kl, W - 1), kl, X);
    }
    /* never silent: a line of what has been lost, only once something has (gptps_tui_health) */
    if (t->h_folded || t->h_untracked || t->p_lost || t->h_trunc || t->h_nomem) {
        char ls[320];
        size_t lp = (size_t)snprintf(ls, sizeof ls, "lost:");
        if (t->h_folded)
            lp += (size_t)snprintf(ls + lp, sizeof ls - lp, " %llu%s types folded into (other);",
                                   (unsigned long long)t->h_folded, t->fold_more ? "+" : "");
        if (t->h_untracked && lp < sizeof ls)
            lp += (size_t)snprintf(ls + lp, sizeof ls - lp, " %llu items not tracked (latency window full);",
                                   (unsigned long long)t->h_untracked);
        if (t->p_lost && lp < sizeof ls)
            lp += (size_t)snprintf(ls + lp, sizeof ls - lp, " %llu problem events not shown (tui.recent too small);",
                                   (unsigned long long)t->p_lost);
        if (t->h_trunc && lp < sizeof ls)
            lp += (size_t)snprintf(ls + lp, sizeof ls - lp, " %llu frames truncated;", (unsigned long long)t->h_trunc);
        if (t->h_nomem && lp < sizeof ls)
            lp += (size_t)snprintf(ls + lp, sizeof ls - lp, " %llu allocations failed;", (unsigned long long)t->h_nomem);
        if (lp >= sizeof ls) lp = sizeof ls - 1;
        if (lp && ls[lp - 1] == ';') ls[--lp] = 0;
        pos = appendf(buf, cap, pos, "%s%.*s%s\n", R, (int)u8_fit(ls, W - 1), ls, X);
    }

    if (t->tasks) {                      /* the TASKS tier and up, and shown (else not built) */
        /* the percentiles (FULL) when the width has room for them: 21 columns more */
        int pcols = t->hist && W >= 100;
        pos = appendf(buf, cap, pos, "\n%sTASKS%s\n", B, X);
        pos = appendf(buf, cap, pos, "%s  %-16s %5s %5s %5s %5s %5s %5s %8s%s %8s  key%s\n",
                      D, "label", "run", "ok", "fail", "dead", "drop", "ok%", "avg ms",
                      pcols ? "    p50    p95    p99" : "", "peak MB", X);
        /* the types in the order first seen, then '(other)' once anything was folded there */
        for (i = 1; i <= t->ntasks; ++i) {
            tui_task *k = &t->tasks[i < t->ntasks ? i : 0];
            const tui_ui *u = (k->ui >= 0 && k->ui < t->nui) ? &t->ui[k->ui] : NULL;
            char key[8], pct[8], lat[24], peak[32], lb[TUI_LABEL_CAP * 2], c[5][16], pq[3][16], pcs[32];
            uint64_t terminal = k->finished + k->dead + k->dropped;
            const char *pc = "";
            if (i == t->ntasks && !t->nfold && !k->started && !k->failed && !k->retried && !terminal) break;
            if (u && u->hotkey > 32 && u->hotkey < 127) snprintf(key, sizeof key, "[%c]", u->hotkey); else key[0] = 0;
            if (terminal) {
                /* In double, then clamped: the counters are 64-bit, so finished * 100 could
                 * still wrap in integers. Clamped, the invariant the %3u here and the %5s
                 * column below assume - okp is 0..100, three digits - is true rather than
                 * merely hoped for, so the table cannot shear. */
                double r = (double)k->finished * 100.0 / (double)terminal;
                unsigned okp = r >= 100.0 ? 100u : r <= 0.0 ? 0u : (unsigned)r;
                snprintf(pct, sizeof pct, "%3u%%", okp);
                if (color) pc = (okp >= 90) ? G : (okp >= 50) ? "\x1b[33m" : R;   /* green/yellow/red */
            } else strcpy(pct, "  --");
            /* avg ms: queue->finish of FINISHED items only (an item that ends any other
             * way releases its latency entry without a sample) */
            if (k->lat_n) snprintf(lat, sizeof lat, "%8.1f", (double)k->lat_sum_ms / (double)k->lat_n);
            else          strcpy(lat, "      --");
            /* What a process job's attempts measured at their peak (docs/MEASUREMENTS.md);
             * "--" for an in-process task, which is never measured. */
            if (k->mem_method[0]) snprintf(peak, sizeof peak, "%8.1f", (double)k->mem_peak / 1048576.0);
            else                  strcpy(peak, "      --");
            pcs[0] = 0;
            if (pcols) {
                const uint64_t *hb = &t->hist[(size_t)(k - t->tasks) * TUI_HIST_B];
                snprintf(pcs, sizeof pcs, " %6s %6s %6s", hist_pct(hb, 50, pq[0], sizeof pq[0]),
                         hist_pct(hb, 95, pq[1], sizeof pq[1]), hist_pct(hb, 99, pq[2], sizeof pq[2]));
            }
            pos = appendf(buf, cap, pos, "  %s %5s %5s %5s %5s %5s %s%5s%s %8s%s %8s  %s\n",
                          tui_cell(u ? u->label : k->name, lb, sizeof lb, 16), fmt_count(c[0], sizeof c[0], k->started),
                          fmt_count(c[1], sizeof c[1], k->finished), fmt_count(c[2], sizeof c[2], k->failed),
                          fmt_count(c[3], sizeof c[3], k->dead), fmt_count(c[4], sizeof c[4], k->dropped),
                          pc, pct, color ? X : "", lat, pcs, peak, key);
        }
        /* never silent: the types the full table folds, and the latency it lost */
        if (t->nfold)
            pos = appendf(buf, cap, pos, "  %s+%d%s types folded into (other) (the table holds %d)%s\n", R, t->nfold,
                          t->fold_more ? " or more" : "", t->max_tasks, X);
        if (t->tab && (t->lat_untracked || t->lat_unmatched))
            pos = appendf(buf, cap, pos, "  %slatency: %llu not tracked (window %d full), %llu ends unmatched%s\n",
                          D, (unsigned long long)t->lat_untracked, t->lat_cap, (unsigned long long)t->lat_unmatched, X);
    }

    if (t->prob || t->act) {             /* a journal's tier and up, and shown (else not built) */
        /* The two journals merged by time, newest last; `scroll` lines back. Shown: at most
         * the larger journal's size, and what the screen has room for. */
        enum { TUI_VIEW_MAX = 512 };
        int ref[TUI_VIEW_MAX];               /* >= 0: a PROBLEMS index; < 0: -(ACTIVITY index) - 1 */
        int np = (int)ring_held(t->p_seq, t->p_base, t->prob ? t->pcap : 0),
            na = (int)ring_held(t->a_seq, t->a_base, t->act ? t->acap : 0),
            total = np + na, lines = (t->prob && t->pcap > (t->act ? t->acap : 0)) ? t->pcap : t->acap,
            shown, maxoff, used = 0, avail, ip = 0, ia = 0, k, n = 0;
        size_t z;
        char lost[48];
        shown = total < lines ? total : lines;
        for (z = 0; z < pos; ++z) if (buf[z] == '\n') ++used;     /* vertical fit: stay within rows */
        /* every line below is one row (each is fitted to W): the RECENT heading and
         * its blank line, the toast and its blank line, the legend and its blank line;
         * and the frame's last newline must not scroll the screen */
        avail = (t->rows > 0 ? t->rows : 24) - 1 - used - 2 - (toast_on ? 2 : 0) - 1 - legrows;
        if (avail < 1) avail = 1;
        if (shown > avail) shown = avail;
        if (shown > TUI_VIEW_MAX) shown = TUI_VIEW_MAX;
        maxoff = total - shown; if (maxoff < 0) maxoff = 0;
        if (t->scroll > maxoff) t->scroll = maxoff;     /* clamp (keys can over-scroll) */
        if (t->scroll < 0) t->scroll = 0;
        /* newest first: the k-th newest of a ring is (head - 1 - k) mod cap; a tie in time
         * puts the problem after the activity (a STARTED, then its FAILED, in one ms) */
        for (k = 0; k < t->scroll + shown && (ip < np || ia < na); ++k) {
            int pi = ip < np ? (t->phead - 1 - ip + t->pcap * 2) % t->pcap : -1,
                ai = ia < na ? (t->ahead - 1 - ia + t->acap * 2) % t->acap : -1, r;
            if (pi >= 0 && (ai < 0 || t->prob[pi].ts >= t->act[ai].ts)) { r = pi; ++ip; }
            else { r = -ai - 1; ++ia; }
            if (k >= t->scroll) ref[n++] = r;
        }
        lost[0] = 0;
        if (t->frame_unseen)
            snprintf(lost, sizeof lost, "  %llu not shown", (unsigned long long)t->frame_unseen);
        if (t->scroll > 0) pos = appendf(buf, cap, pos, "\n%sRECENT%s  %s(scrolled +%d)%s%s%s%s\n", B, X, D, t->scroll, X,
                                         R, lost, X);
        else               pos = appendf(buf, cap, pos, "\n%sRECENT%s%s%s%s%s%s%s\n", B, X, R, lost, X,
                                         D, t->act ? "" : "  problems only", X);
        for (k = n - 1; k >= 0; --k) {       /* oldest at the top */
            char pre[128], nm[TUI_NAME_CAP * 2], why[sizeof t->prob[0].why + TUI_TAIL_KEEP + 16];
            const char *name, *kc;
            uint64_t ts;
            gptps_handle hd;
            int kind, pl, room, wl = 0;
            double rel;
            why[0] = 0;
            if (ref[k] >= 0) {
                const tui_event *re = &t->prob[ref[k]];
                ts = re->ts; kind = re->kind; hd = re->handle; name = re->name;
                why_join(re->why, re->tail, re->tail_len, re->tail_cut, why, sizeof why);   /* cleaned here */
            } else {
                int ai = -ref[k] - 1;
                const tui_act *ra = &t->act[ai];
                ts = ra->ts; kind = ra->kind; hd = ra->handle;
                name = (ra->row >= 0 && t->tasks && ra->row < t->ntasks) ? t->tasks[ra->row].name
                     : t->act_nm ? t->act_nm[ai] : "?";
            }
            kc = (kind == GPTPS_EV_FAILED || kind == GPTPS_EV_DEAD_LETTERED || kind == GPTPS_EV_DROPPED) ? R
               : (kind == GPTPS_EV_FINISHED) ? G : D;
            rel = (double)(ts - t->start_ms) / 1000.0; /* seconds since install */
            /* One terminal row each: the vertical fit above counts rows by '\n', so a
             * line that wrapped took two and pushed the frame past the screen. The fixed
             * fields are measured as printed; the why text gets what is left. */
            pl = snprintf(pre, sizeof pre, "  %6.1f %-8s %s #%llu", rel, kind_str(kind), tui_cell(name, nm, sizeof nm, 14),
                          (unsigned long long)hd);
            room = W - 1 - (pl < 0 ? 0 : pl) - 2;
            wl = (why[0] && room >= 4) ? (int)u8_fit(why, room) : 0;
            pos = appendf(buf, cap, pos, "  %s%6.1f%s %s%-8s%s %s #%llu%s%s%.*s%s\n",
                          D, rel, X, kc, kind_str(kind), X, nm, (unsigned long long)hd,
                          wl ? "  " : "", D, wl, why, X);
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

    return tui_finish(t, buf, cap, pos);
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
        mu_lock(&t->mu);             /* gptps_tui_health reads held_cap under it */
        t->held = nh; t->held_cap = nc;
        mu_unlock(&t->mu);
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
            char task[TUI_NAME_CAP];
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
    const char *name = NULL;
    char nm[TUI_NAME_CAP], lb[TUI_LABEL_CAP];
    void *plc = NULL;
    size_t pn = 0, want = 0;
    int i, tries;
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
    /* Copies, taken under the lock and used without it: add_task on another thread may
     * grow the entries' array or replace this entry's payload meanwhile. The payload's
     * copy is allocated with the lock released, then filled under it if its size still
     * matches (replaced in between: once more). */
    for (tries = 0; tries < 3; ++tries) {
        mu_lock(&t->mu);
        name = NULL;
        for (i = 0; i < t->nui; ++i)
            if (t->ui[i].hotkey == key) {
                pn = t->ui[i].plen;
                if (pn && (!plc || want != pn)) break;          /* a copy of this size first */
                memcpy(nm, t->ui[i].name, sizeof nm); memcpy(lb, t->ui[i].label, sizeof lb);
                name = nm;
                if (pn) memcpy(plc, t->ui[i].payload, pn);
                if ((has_ko = t->ui[i].has_opts) != 0) ko = t->ui[i].opts;
                break;
            }
        if (name || i == t->nui) break;                         /* copied, or no such key: still locked */
        mu_unlock(&t->mu);
        free(plc);
        want = pn;
        if ((plc = malloc(want)) == NULL) { toast(t, "NOT submitted: no memory for the payload's copy"); return 0; }
    }
    if (tries == 3) { free(plc); return 0; }                    /* replaced each time: not this press */
    mu_unlock(&t->mu);                 /* submit OUTSIDE the lock: QUEUED fires our observer */
    if (name) {                        /* a due time counts from this press */
        gptps_handle h;
        gptps_status st = gptps_submit_ex(t->e, name, pn ? plc : NULL, pn, has_ko ? &ko : NULL, &h);
        free(plc);
        if (st != GPTPS_OK) {          /* paused, unknown, full, denied, shutting down: say so */
            toast(t, "NOT submitted %s: %s", lb[0] ? lb : name, gptps_strerror(st));
            return 0;
        }
        toast(t, "submitted %s", lb[0] ? lb : name);
        return 1;
    }
    free(plc);
    if (key == 'm' || key == 'M') {                 /* cycle KPI level (configure cost live) */
        /* one tier up the ladder, and from FULL back to MINIMAL: never into OFF */
        int nk; mu_lock(&t->mu); nk = TUI_LADDER[t->tier >= TUI_T_FULL ? TUI_T_COUNT : t->tier + 1]; mu_unlock(&t->mu);
        if (gptps_tui_set_kpi(t, (gptps_tui_kpi)nk) == GPTPS_OK) toast(t, "kpi -> %s", kpi_str(nk));
        else toast(t, "kpi -> %s, without its buffers: no memory (see the log)", kpi_str(nk));
        return 3;
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
    void *cp = NULL, *old_pl = NULL;
    tui_ui *grown = NULL, *old_ui = NULL;
    gptps_submit_options o;
    gptps_status st = GPTPS_OK;
    uint64_t h;
    int grown_for = -1;
    if (!t || !task_name) return GPTPS_E_INVAL;
    if (strlen(task_name) > GPTPS_TASK_NAME_MAX) return GPTPS_E_INVAL;   /* never a task's name */
    if (opts) {                        /* an older caller's struct: copy what it has, the rest 0 */
        size_t n;
        if (opts->struct_size < offsetof(gptps_submit_options, timeout_ms) + sizeof(uint32_t)) return GPTPS_E_INVAL;
        memset(&o, 0, sizeof o);
        n = opts->struct_size < sizeof o ? opts->struct_size : sizeof o;
        memcpy(&o, opts, n);
        o.struct_size = sizeof o;
    }
    if (len) { cp = malloc(len); if (!cp) return GPTPS_E_NOMEM; memcpy(cp, payload, len); }
    h = name_hash(task_name);
    /* Under the lock: find the entry, or append one - growing the array means allocating,
     * which is done with the lock released and the grown copy swapped in under it (see
     * "storage"); another add_task may have grown it meanwhile, so look again. */
    for (;;) {
        int k, r = -1;
        tui_ui *u = NULL;
        mu_lock(&t->mu);
        for (k = 0; k < t->nui; ++k) if (strcmp(t->ui[k].name, task_name) == 0) { u = &t->ui[k]; break; }
        if (t->tasks) r = row_lookup(t, task_name, h);
        if (!u) {
            /* no room: max_tasks entries, or a TASKS table whose every row is taken by
             * the types its events brought (GPTPS_E_FULL, as when the table was the list) */
            if (t->nui >= t->max_tasks || (t->tasks && r < 0 && t->ntasks >= t->tasks_cap)) { st = GPTPS_E_FULL; mu_unlock(&t->mu); break; }
            if (t->nui == t->ui_cap) {                        /* grow it, with the lock released */
                int ncap = t->ui_cap ? t->ui_cap * 2 : 8;
                if (ncap > t->max_tasks) ncap = t->max_tasks;
                if (grown && grown_for == t->ui_cap) {        /* made last round for this capacity */
                    if (t->nui) memcpy(grown, t->ui, (size_t)t->nui * sizeof *grown);
                    old_ui = t->ui; t->ui = grown; t->ui_cap = ncap; grown = NULL;
                } else {
                    grown_for = t->ui_cap;
                    mu_unlock(&t->mu);
                    free(grown);
                    grown = (tui_ui *)malloc((size_t)ncap * sizeof *grown);
                    if (!grown) {
                        mu_lock(&t->mu); t->h_nomem++; mu_unlock(&t->mu);
                        st = GPTPS_E_NOMEM; break;
                    }
                    continue;
                }
            }
            u = &t->ui[t->nui++];
            memset(u, 0, sizeof *u);
            memcpy(u->name, task_name, strlen(task_name) + 1);
            copy_u8(u->label, sizeof u->label, task_name);
        }
        if (label) copy_u8(u->label, sizeof u->label, label);   /* display only: cut at a character */
        u->hotkey = hotkey;
        old_pl = u->payload; u->payload = cp; u->plen = len; cp = NULL;
        u->has_opts = opts != NULL;
        if (opts) u->opts = o;
        if (t->tasks) {                                       /* its row, made now: shown in add order */
            if (r < 0) r = row_add(t, task_name, h);
            if (r > 0) t->tasks[r].ui = (int)(u - t->ui);
        }
        t->dirty = 1;
        mu_unlock(&t->mu);
        break;
    }
    free(grown);                       /* grown for a capacity another call reached first */
    free(old_ui);
    free(old_pl);
    free(cp);
    return st;
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
static size_t write_frame(FILE *out, const char *s)
{
    const char *p = s, *nl;
    size_t n = 0;
    while ((nl = strchr(p, '\n')) != NULL) {
        fwrite(p, 1, (size_t)(nl - p), out);
        fputs("\x1b[K\n", out);
        n += (size_t)(nl - p) + 4;
        p = nl + 1;
    }
    if (*p) { fputs(p, out); fputs("\x1b[K", out); n += strlen(p) + 3; }
    return n;
}

/* Until when the frame just painted shows a toast (0: none): the loop must paint
 * again then to take it away, in every pane. */
static uint64_t toast_until(const gptps_tui *t, uint64_t now)
{
    return status_live(t->status, t->status_ms, now) ? t->status_ms + TUI_TOAST_MS : 0;
}

/* The live loop's cadence: CONTINUOUS every frame; ON_DEMAND when state changed, a key
 * was pressed, or a toast it showed has expired (else it stayed up until the next
 * change, however long that took); PAUSED only after a key (or the first frame). */
static int paint_due(int first, int kp, int mode, int dirty, uint64_t shown_until, uint64_t now)
{
    if (first || kp || mode == GPTPS_TUI_CONTINUOUS) return 1;
    if (mode != GPTPS_TUI_ON_DEMAND) return 0;
    return dirty || (shown_until && now >= shown_until);
}

void gptps_tui_run(gptps_tui *t)
{
    char *buf; size_t cap = 16384;
    int first = 1, kp = 0;
    uint64_t shown_until = 0;       /* the painted toast's expiry (see paint_due) */
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
    if (!buf) {
        mu_lock(&t->mu); t->h_nomem++; mu_unlock(&t->mu);
        gptps_log(NULL, GPTPS_LOG_WARN, "gptps_tui_run: no memory for the 16 KiB frame buffer; "
                  "the live dashboard does not run");
        return;
    }
    mu_lock(&t->mu); t->frame_bytes = cap; mu_unlock(&t->mu);
    term_enable(t);
    fputs("\x1b[?25l", t->cfg.out);                 /* hide cursor */
    for (;;) {
        int q, md, key, paint, dirty;
        uint32_t rms;
        uint64_t now = gptps_now_ms(NULL);
        mu_lock(&t->mu); q = t->quit; md = t->mode; rms = t->cfg.refresh_ms; dirty = t->dirty; mu_unlock(&t->mu);
        if (q) break;
        paint = paint_due(first, kp, md, dirty, shown_until, now);   /* the cadence: see paint_due */
        if (paint) {
            size_t wrote;
            mu_lock(&t->mu); t->dirty = 0; mu_unlock(&t->mu);
            term_size(t);                            /* adapt the layout to the current window */
            gptps_tui_render(t, buf, cap);
            shown_until = toast_until(t, gptps_now_ms(NULL));
            if (first) fputs("\x1b[2J", t->cfg.out); /* clear once; then redraw in place */
            fputs("\x1b[H", t->cfg.out);             /* home */
            wrote = write_frame(t->cfg.out, buf);    /* per-line erase => no full-screen flash */
            fputs("\x1b[J", t->cfg.out);             /* wipe any lines a shorter frame left behind */
            fflush(t->cfg.out);
            mu_lock(&t->mu); t->h_painted++; t->h_written += wrote + 6 + (first ? 4 : 0); mu_unlock(&t->mu);
        }
        first = 0; kp = 0;
        key = term_poll_key((int)(rms ? rms : 250));
        mu_lock(&t->mu); t->h_wakes++; mu_unlock(&t->mu);
        if (key >= 0) { gptps_tui_press(t, key); kp = 1; }
    }
    fputs("\x1b[?25h", t->cfg.out); fflush(t->cfg.out); /* show cursor */
    term_restore(t);
    mu_lock(&t->mu); t->frame_bytes = 0; mu_unlock(&t->mu);
    free(buf);
}

/* ---- settings registry bindings (target = the gptps_tui) ---- */
/* The released choices first, in their 1.10 order; the 1.11 tiers and synonyms appended. */
static const char *const TUI_KPI_CHOICES[]  = { "off", "minimal", "normal", "full",
                                                "gauge", "problems", "tasks", "activity", "latency", "count", 0 };
static const int TUI_KPI_CHOICE_VAL[] = { GPTPS_TUI_KPI_OFF, GPTPS_TUI_KPI_MINIMAL, GPTPS_TUI_KPI_NORMAL, GPTPS_TUI_KPI_FULL,
                                          GPTPS_TUI_KPI_GAUGE, GPTPS_TUI_KPI_PROBLEMS, GPTPS_TUI_KPI_TASKS,
                                          GPTPS_TUI_KPI_ACTIVITY, GPTPS_TUI_KPI_LATENCY, GPTPS_TUI_KPI_COUNT };
static const char *const TUI_MODE_CHOICES[] = { "realtime", "on-demand", "paused", 0 };

static size_t ts_rd_refresh(void *p, char *b, size_t c) { gptps_tui *t = (gptps_tui *)p; unsigned ms; mu_lock(&t->mu); ms = t->cfg.refresh_ms; mu_unlock(&t->mu); return (size_t)snprintf(b, c, "%u", ms); }
static gptps_status ts_wr_refresh(void *p, const char *v) { return gptps_tui_set_refresh((gptps_tui *)p, (uint32_t)strtoul(v, NULL, 10)); }
static size_t ts_rd_kpi(void *p, char *b, size_t c) { gptps_tui *t = (gptps_tui *)p; int k; mu_lock(&t->mu); k = t->kpi; mu_unlock(&t->mu); return (size_t)snprintf(b, c, "%s", kpi_str(k)); }
static gptps_status ts_wr_kpi(void *p, const char *v)
{
    size_t i;
    for (i = 0; TUI_KPI_CHOICES[i]; ++i)
        if (strcmp(v, TUI_KPI_CHOICES[i]) == 0) return gptps_tui_set_kpi((gptps_tui *)p, (gptps_tui_kpi)TUI_KPI_CHOICE_VAL[i]);
    return GPTPS_E_INVAL;              /* the registry checks the choices first: not reached */
}
static size_t ts_rd_recent(void *p, char *b, size_t c) { gptps_tui *t = (gptps_tui *)p; int n; mu_lock(&t->mu); n = t->prob_want; mu_unlock(&t->mu); return (size_t)snprintf(b, c, "%d", n); }
static gptps_status ts_wr_recent(void *p, const char *v) { return gptps_tui_set_recent((gptps_tui *)p, (int)strtol(v, NULL, 10), -1); }
static size_t ts_rd_activity(void *p, char *b, size_t c) { gptps_tui *t = (gptps_tui *)p; int n; mu_lock(&t->mu); n = t->act_want; mu_unlock(&t->mu); return (size_t)snprintf(b, c, "%d", n); }
static gptps_status ts_wr_activity(void *p, const char *v) { return gptps_tui_set_recent((gptps_tui *)p, -1, (int)strtol(v, NULL, 10)); }
static size_t ts_rd_latwin(void *p, char *b, size_t c) { gptps_tui *t = (gptps_tui *)p; int n; mu_lock(&t->mu); n = t->lat_cap; mu_unlock(&t->mu); return (size_t)snprintf(b, c, "%d", n); }
static gptps_status ts_wr_latwin(void *p, const char *v) { return gptps_tui_set_latency_window((gptps_tui *)p, (int)strtol(v, NULL, 10)); }
static size_t ts_rd_mode(void *p, char *b, size_t c) { gptps_tui *t = (gptps_tui *)p; int m; mu_lock(&t->mu); m = t->mode; mu_unlock(&t->mu); return (size_t)snprintf(b, c, "%s", mode_str(m)); }
static gptps_status ts_wr_mode(void *p, const char *v) { int m = (strcmp(v, "realtime") == 0) ? GPTPS_TUI_CONTINUOUS : (strcmp(v, "on-demand") == 0) ? GPTPS_TUI_ON_DEMAND : GPTPS_TUI_PAUSED; return gptps_tui_set_mode((gptps_tui *)p, (gptps_tui_mode)m); }

/* A formatted line to the engine's log (gptps_log takes a plain string). */
static void tui_logf(gptps_log_level lvl, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    gptps_log(NULL, lvl, msg);
}

static void tui_register_settings(gptps *e, gptps_tui *t)
{
    static const struct {
        const char *key; gptps_setting_type type; const char *desc; const char *const *choices;
        size_t (*read)(void *, char *, size_t); gptps_status (*write)(void *, const char *);
        int has_range; double min, max;
    } S[] = {
        /* released (1.10): their type, choices and range stay in 1.x - choices are appended */
        { "tui.refresh_ms",     GPTPS_SETTING_UINT, "dashboard redraw interval (ms)",      NULL,             ts_rd_refresh, ts_wr_refresh, 0, 0, 0 },
        { "tui.kpi",            GPTPS_SETTING_ENUM, "KPI detail level",                    TUI_KPI_CHOICES,  ts_rd_kpi,     ts_wr_kpi, 0, 0, 0 },
        { "tui.mode",           GPTPS_SETTING_ENUM, "redraw cadence",                      TUI_MODE_CHOICES, ts_rd_mode,    ts_wr_mode, 0, 0, 0 },
        { "tui.latency_window", GPTPS_SETTING_UINT, "handles tracked for latency at FULL", NULL,             ts_rd_latwin,  ts_wr_latwin, 0, 0, 0 },
        /* 1.11 */
        { "tui.recent",         GPTPS_SETTING_UINT, "failed/retried/dead/dropped events the RECENT log keeps "
                                                    "(the PROBLEMS journal, kpi problems and up); 0 = none",
                                                    NULL, ts_rd_recent, ts_wr_recent, 1, 0, TUI_JOURNAL_CAP },
        { "tui.activity",       GPTPS_SETTING_UINT, "queued/started/finished events the RECENT log keeps "
                                                    "(the ACTIVITY journal, kpi normal and up); 0 = none",
                                                    NULL, ts_rd_activity, ts_wr_activity, 1, 0, TUI_JOURNAL_CAP },
    };
    size_t i;
    gptps_setting_def d;
    memset(&d, 0, sizeof d); d.struct_size = sizeof d; d.target = t; d.hot = 1;
    for (i = 0; i < sizeof S / sizeof S[0]; ++i) {
        gptps_status st;
        d.key = S[i].key; d.type = S[i].type; d.desc = S[i].desc; d.choices = S[i].choices;
        d.read = S[i].read; d.write = S[i].write;
        d.has_range = S[i].has_range; d.min = S[i].min; d.max = S[i].max;
        st = gptps_register_setting(e, &d);
        if (st != GPTPS_OK)                          /* never silent: the dashboard runs, the key does not */
            tui_logf(GPTPS_LOG_WARN, "gptps_tui: setting %s was not registered (%s); the dashboard runs, "
                     "but this knob cannot be read or changed through the settings registry", S[i].key,
                     gptps_strerror(st));
    }
}

/* The oldest gptps_tui_config a caller can have: everything through `title`. */
#define TUI_CFG_MIN (offsetof(gptps_tui_config, title) + sizeof(((gptps_tui_config *)0)->title))
/* What struct_size 0 reads: the 1.10 layout, through `unicode` - the whole struct when 0
 * was given that meaning (1.10.1), and never more, so a zero-init caller built against
 * that header is not read past its end now that fields follow. */
#define TUI_CFG_110 (offsetof(gptps_tui_config, unicode) + sizeof(int))

/* ---- storage ----------------------------------------------------------------------
 * Every buffer belongs to one layer of the ladder and exists only at that tier and up -
 * and only while its pane is shown (show_tasks / show_recent < 0 drop it at every tier):
 * the PROBLEMS journal (PROBLEMS), the TASKS table - rows, name index, fold set - (TASKS),
 * the ACTIVITY journal (NORMAL), the handle table (LATENCY), the histograms (FULL). A part
 * is allocated with `mu` RELEASED, swapped in under `mu`, and freed only after it has been
 * swapped out under `mu`. The observer touches a part only through the pointer it reads
 * while holding `mu`, so it never sees one half-built or freed, and no allocation or free
 * happens while an engine thread waits on the lock. The setters hold `smu` from the first
 * look to the swap, so the state they allocated for is the state they find. */
typedef struct {
    tui_task  *tasks;
    int32_t   *tidx;
    uint64_t  *fold;
    int        tasks_cap;
    uint32_t   tidx_mask;
    tui_event *prob;
    tui_act   *act;
    char     (*act_nm)[TUI_ACT_NAME];
    uint64_t  *hist;
    tui_slot  *tab;
    size_t     tab_mask;
} tui_parts;

static size_t tasks_bytes(int max_tasks)
{
    size_t rows = (size_t)max_tasks + 1, idx = next_pow2(2 * (size_t)max_tasks);
    if (idx < 8) idx = 8;
    return rows * sizeof(tui_task) + idx * sizeof(int32_t) + TUI_FOLD_SLOTS * sizeof(uint64_t);
}
static size_t hist_bytes(int max_tasks) { return ((size_t)max_tasks + 1) * TUI_HIST_B * sizeof(uint64_t); }
static size_t act_bytes(int lines, int names) { return (size_t)lines * (sizeof(tui_act) + (names ? TUI_ACT_NAME : 0)); }

static void parts_free(tui_parts *p)
{
    free(p->tasks); free(p->tidx); free(p->fold); free(p->prob); free(p->act); free(p->act_nm);
    free(p->hist); free(p->tab);
    memset(p, 0, sizeof *p);
}

/* What tui_reconfig builds (0: nothing of that part). */
typedef struct { int tasks, max_tasks, prob, act, act_nm, hist, tab, win; } tui_plan;

/* Allocate the parts asked for (unlocked). A part whose allocation fails stays NULL. */
static void parts_make(tui_parts *p, const tui_plan *w)
{
    memset(p, 0, sizeof *p);
    if (w->tasks) {
        size_t idx = next_pow2(2 * (size_t)w->max_tasks);
        if (idx < 8) idx = 8;
        p->tasks = (tui_task *)calloc((size_t)w->max_tasks + 1, sizeof(tui_task));
        p->tidx = p->tasks ? (int32_t *)malloc(idx * sizeof(int32_t)) : NULL;
        p->fold = p->tidx ? (uint64_t *)calloc(TUI_FOLD_SLOTS, sizeof(uint64_t)) : NULL;
        if (!p->fold) { free(p->tasks); free(p->tidx); p->tasks = NULL; p->tidx = NULL; }
        else {
            memset(p->tidx, 0xff, idx * sizeof(int32_t));        /* -1: empty */
            p->tasks_cap = w->max_tasks + 1; p->tidx_mask = (uint32_t)(idx - 1);
            memcpy(p->tasks[0].name, "(other)", 8); p->tasks[0].ui = -1;
        }
    }
    if (w->prob > 0) p->prob = (tui_event *)calloc((size_t)w->prob, sizeof(tui_event));
    if (w->act > 0) {
        p->act = (tui_act *)calloc((size_t)w->act, sizeof(tui_act));
        if (p->act && w->act_nm) {                                /* the journal with its names, or not at all */
            p->act_nm = (char (*)[TUI_ACT_NAME])calloc((size_t)w->act, TUI_ACT_NAME);
            if (!p->act_nm) { free(p->act); p->act = NULL; }
        }
    }
    if (w->hist) p->hist = (uint64_t *)calloc(((size_t)w->max_tasks + 1) * TUI_HIST_B, sizeof(uint64_t));
    if (w->tab) {
        size_t n = trk_slots(w->win);
        p->tab = (tui_slot *)calloc(n, sizeof(tui_slot));
        p->tab_mask = n - 1;
    }
}

/* mu held, the TASKS table just swapped in: a row for every add_task entry, in its order. */
static void tasks_seed(gptps_tui *t)
{
    int k;
    for (k = 0; k < t->nui; ++k) {
        uint64_t h = name_hash(t->ui[k].name);
        int r = row_lookup(t, t->ui[k].name, h);
        if (r < 0) r = row_add(t, t->ui[k].name, h);
        if (r > 0) t->tasks[r].ui = k;
    }
}

/* mu held: take a journal out (into *old), counting what no frame showed of it. */
static void prob_out(gptps_tui *t, tui_parts *old)
{
    t->p_lost += t->p_seq - t->p_seen; t->p_seen = t->p_base = t->p_seq;
    old->prob = t->prob; t->prob = NULL; t->pcap = t->phead = 0;
}
static void act_out(gptps_tui *t, tui_parts *old)
{
    t->a_lost += t->a_seq - t->a_seen; t->a_seen = t->a_base = t->a_seq;
    old->act = t->act; old->act_nm = t->act_nm; t->act = NULL; t->act_nm = NULL; t->acap = t->ahead = 0;
}

/* Make the storage match KPI level `lvl` (-1: keep; a gptps_tui_kpi, DEFAULT and ACTIVITY
 * already resolved), latency window `win` (0: keep) and journal sizes `plines` / `alines`
 * (-1: keep; 0: off): build what they need and is missing, drop what they do not hold. A
 * part that cannot be allocated is left out - the dashboard runs without it (a journal
 * being resized keeps its old size) - and `what` (cap bytes, "" when all is well) names
 * it, for the caller to log; GPTPS_E_NOMEM then. */
static gptps_status tui_reconfig(gptps_tui *t, int lvl, int win, int plines, int alines, char *what, size_t cap)
{
    tui_parts nw, old;
    tui_plan w;
    int tier, cur_win, want_tasks, want_prob, want_act, want_hist, want_tab, otier;
    size_t pos = 0;
    gptps_status st = GPTPS_OK;
    what[0] = 0;
    memset(&old, 0, sizeof old);
    memset(&w, 0, sizeof w);
    mu_lock(&t->smu);
    mu_lock(&t->mu);
    if (lvl < 0) lvl = t->kpi;
    tier = kpi_tier(lvl);
    if (win <= 0) win = t->lat_cap;
    if (plines >= 0) t->prob_want = plines;
    if (alines >= 0) t->act_want = alines;
    cur_win = t->lat_cap;
    want_tasks = tier >= TUI_T_TASKS && t->show_tasks;
    want_prob  = tier >= TUI_T_PROBLEMS && t->show_recent && t->prob_want > 0;
    want_act   = tier >= TUI_T_ACTIVITY && t->show_recent && t->act_want > 0;
    want_hist  = tier >= TUI_T_FULL && want_tasks;
    want_tab   = tier >= TUI_T_LATENCY;
    w.max_tasks = t->max_tasks; w.win = win;
    w.tasks  = want_tasks && !t->tasks;
    w.prob   = want_prob && (!t->prob || t->pcap != t->prob_want) ? t->prob_want : 0;
    w.act    = want_act && (!t->act || t->acap != t->act_want) ? t->act_want : 0;
    w.act_nm = w.act && !t->tasks;       /* no table to name its tasks from (yet: dropped if one is built) */
    w.hist   = want_hist && !t->hist;
    w.tab    = want_tab && (!t->tab || win != cur_win);
    mu_unlock(&t->mu);

    parts_make(&nw, &w);                 /* unlocked */

    mu_lock(&t->mu);
    otier = t->tier;
    /* out: what the tier does not hold, and what a new size replaces */
    if (!want_tasks && t->tasks) {
        old.tasks = t->tasks; old.tidx = t->tidx; old.fold = t->fold;
        t->tasks = NULL; t->tidx = NULL; t->fold = NULL;
        t->ntasks = t->tasks_cap = 0; t->nfold = t->fold_more = 0;
    }
    if (t->hist && (!want_hist || !t->tasks)) { old.hist = t->hist; t->hist = NULL; }   /* its rows went with them */
    if (t->prob && (!want_prob || nw.prob)) prob_out(t, &old);
    if (t->act && (!want_act || nw.act || (!t->act_nm && !t->tasks))) act_out(t, &old);   /* nothing to name by */
    if (!want_tab || w.tab) {
        old.tab = t->tab; t->tab = NULL;
        t->tab_n = t->tab_done = 0; t->orph_n = t->orph_head = 0; t->g_pend = t->g_run = 0; t->trk_lb = 0;
    }
    if (win != cur_win || w.tab) { t->lat_untracked = 0; t->lat_unmatched = 0; }   /* counted against the old table */
    t->lat_cap = win;
    /* in */
    if (nw.tasks) {
        t->tasks = nw.tasks; t->tidx = nw.tidx; t->fold = nw.fold;
        t->tasks_cap = nw.tasks_cap; t->tidx_mask = nw.tidx_mask; t->ntasks = 1;   /* row 0: '(other)' */
        nw.tasks = NULL; nw.tidx = NULL; nw.fold = NULL;
        tasks_seed(t);
    }
    if (nw.hist && t->tasks && !t->hist) { t->hist = nw.hist; nw.hist = NULL; }   /* zeroed: the rows' own */
    if (nw.prob) { t->prob = nw.prob; t->pcap = w.prob; t->phead = 0; t->p_base = t->p_seq; nw.prob = NULL; }
    if (nw.act) {
        t->act = nw.act; t->acap = w.act; t->ahead = 0; t->a_base = t->a_seq; nw.act = NULL;
        if (!t->tasks) { t->act_nm = nw.act_nm; nw.act_nm = NULL; }      /* else rows name them: not needed */
    }
    if (nw.tab) {
        t->tab = nw.tab; t->tab_mask = nw.tab_mask; nw.tab = NULL;
        /* work queued before it is not in it: a lower bound until trk_resync (below)
         * hears from the engine that nothing is queued or running */
        t->trk_lb = 1; t->lb_gen++;
    }
    /* The gauge counter starts with the tier that keeps it: below, nothing kept it true. */
    if (otier < TUI_T_GAUGE && tier >= TUI_T_GAUGE) { t->inflight = 0; t->peak = 0; }
    if (w.prob && t->pcap != w.prob) {
        st = GPTPS_E_NOMEM; t->h_nomem++;
        pos += (size_t)snprintf(what + pos, cap - pos, "the PROBLEMS journal (%lu bytes%s)",
                                (unsigned long)((size_t)w.prob * sizeof(tui_event)), t->prob ? "; it keeps its old size" : "");
    }
    if (w.tasks && !t->tasks && pos < cap) {
        st = GPTPS_E_NOMEM; t->h_nomem++;
        pos += (size_t)snprintf(what + pos, cap - pos, "%sthe TASKS table (%lu bytes; per-task counts are off)",
                                pos ? " and " : "", (unsigned long)tasks_bytes(w.max_tasks));
    }
    if (w.act && t->acap != w.act && pos < cap) {
        st = GPTPS_E_NOMEM; t->h_nomem++;
        pos += (size_t)snprintf(what + pos, cap - pos, "%sthe ACTIVITY journal (%lu bytes%s)", pos ? " and " : "",
                                (unsigned long)act_bytes(w.act, w.act_nm), t->act ? "; it keeps its old size" : "");
    }
    if (w.tab && !t->tab && pos < cap) {
        st = GPTPS_E_NOMEM; t->h_nomem++;
        pos += (size_t)snprintf(what + pos, cap - pos, "%sthe latency table (%lu bytes; avg ms is off and the gauges "
                                "are approximate)", pos ? " and " : "", (unsigned long)(trk_slots(win) * sizeof(tui_slot)));
    }
    if (w.hist && t->tasks && !t->hist && pos < cap) {
        st = GPTPS_E_NOMEM; t->h_nomem++;
        snprintf(what + pos, cap - pos, "%sthe latency histograms (%lu bytes; the percentiles are off)",
                 pos ? " and " : "", (unsigned long)hist_bytes(w.max_tasks));
    }
    t->kpi = lvl;
    t->tier = tier;
    t->dirty = 1;
    /* Published LAST: tui_on_event reads this without the lock, so everything it
     * guards must already be consistent before the event path is allowed back in. */
    TUI_FLAG_SET(&t->off, tier == TUI_T_OFF);
    mu_unlock(&t->mu);
    parts_free(&old);                   /* swapped out above: nothing can reach them now */
    parts_free(&nw);                    /* nothing left, unless a part was not needed after all */
    mu_unlock(&t->smu);
    trk_resync(t);                      /* a table built on an idle engine is exact at once */
    return st;
}

static void tui_destroy(gptps_tui *t)
{
    int i;
    tui_parts p;
    for (i = 0; i < t->nui; ++i) free(t->ui[i].payload);
    free(t->ui);
    memset(&p, 0, sizeof p);
    p.tasks = t->tasks; p.tidx = t->tidx; p.fold = t->fold; p.prob = t->prob; p.act = t->act;
    p.act_nm = t->act_nm; p.hist = t->hist; p.tab = t->tab;
    parts_free(&p);
    free(t->held);
    mu_destroy(&t->smu);
    mu_destroy(&t->mu);
    free(t);
}

gptps_tui *gptps_tui_install(gptps *e, const gptps_tui_config *cfg)
{
    gptps_tui *t;
    gptps_status st;
    int max, kpi;
    char what[400];
    if (!e) return NULL;
    if (cfg && cfg->struct_size && cfg->struct_size < TUI_CFG_MIN) {
        tui_logf(GPTPS_LOG_ERROR, "gptps_tui_install: cfg.struct_size %lu is smaller than any gptps_tui_config "
                 "(%lu bytes, through title); the dashboard was not installed",
                 (unsigned long)cfg->struct_size, (unsigned long)TUI_CFG_MIN);
        return NULL;
    }
    t = (gptps_tui *)calloc(1, sizeof *t);
    if (!t) {
        gptps_log(NULL, GPTPS_LOG_WARN, "gptps_tui_install: no memory for the dashboard; not installed");
        return NULL;
    }
    mu_init(&t->mu);
    mu_init(&t->smu);
    t->e = e;
    /* As much as the caller's struct holds, the rest zero (its defaults): an older
     * caller's struct ends before the fields appended since, and what follows it in
     * its memory is not a config. 0 = the 1.10 layout (TUI_CFG_110). */
    if (cfg) {
        size_t n = cfg->struct_size ? cfg->struct_size : TUI_CFG_110;
        if (n > sizeof t->cfg) n = sizeof t->cfg;
        memcpy(&t->cfg, cfg, n);
    }
    t->cfg.struct_size = sizeof t->cfg;
    if (t->cfg.max_tasks == 0) {
        t->max_tasks = TUI_MAX_TASKS;
    } else if (t->cfg.max_tasks < 1 || t->cfg.max_tasks > TUI_TASKS_CAP) {   /* a new field: refused, not clamped */
        tui_logf(GPTPS_LOG_ERROR, "gptps_tui_install: cfg.max_tasks %d is outside 1..%d (0 means %d); the "
                 "dashboard was not installed", t->cfg.max_tasks, TUI_TASKS_CAP, TUI_MAX_TASKS);
        tui_destroy(t);
        return NULL;
    } else {
        t->max_tasks = t->cfg.max_tasks;
    }
    t->cfg.max_tasks = t->max_tasks;
    {   /* the other 1.11 fields: refused out of range, like max_tasks - nothing released depends on them */
        static const struct { const char *name; size_t off; int lo, hi; } F[] = {
            { "recent_lines",    offsetof(gptps_tui_config, recent_lines),    -1, TUI_JOURNAL_CAP },
            { "activity_lines",  offsetof(gptps_tui_config, activity_lines),  -1, TUI_JOURNAL_CAP },
            { "clock",           offsetof(gptps_tui_config, clock),            0, GPTPS_TUI_CLOCK_SECONDS },
            { "rate_window_s",   offsetof(gptps_tui_config, rate_window_s),    0, 300 },
            { "pane_refresh_ms", offsetof(gptps_tui_config, pane_refresh_ms),  0, 600000 },
        };
        size_t k;
        for (k = 0; k < sizeof F / sizeof F[0]; ++k) {
            int v;
            memcpy(&v, (const char *)&t->cfg + F[k].off, sizeof v);
            if (v < F[k].lo || v > F[k].hi || (F[k].off == offsetof(gptps_tui_config, pane_refresh_ms) && v && v < 100)) {
                tui_logf(GPTPS_LOG_ERROR, "gptps_tui_install: cfg.%s %d is out of range (see gptps_tui.h); the "
                         "dashboard was not installed", F[k].name, v);
                tui_destroy(t);
                return NULL;
            }
        }
        if (!t->cfg.clock)           t->cfg.clock = GPTPS_TUI_CLOCK_MINUTES;
        if (!t->cfg.rate_window_s)   t->cfg.rate_window_s = 10;
        if (!t->cfg.pane_refresh_ms) t->cfg.pane_refresh_ms = 2000;
    }
    if (!t->cfg.title) t->cfg.title = "tasks";   /* every pane prints it */
    if (!t->cfg.out) t->cfg.out = stdout;
    if (t->cfg.refresh_ms == 0) t->cfg.refresh_ms = 250;
    if (t->cfg.color < 0)       t->cfg.color = fd_is_tty(t->cfg.out);
    if (t->cfg.interactive < 0) t->cfg.interactive = fd_is_tty(t->cfg.out);
    if (t->cfg.unicode < 0)     t->cfg.unicode = t->cfg.color;   /* auto: follow color */
    t->cols = 80; t->rows = 24;   /* headless defaults; the live loop refreshes from the terminal */
    t->show_tasks  = (t->cfg.show_tasks  >= 0);   /* default on; <0 hides */
    t->show_recent = (t->cfg.show_recent >= 0);
    /* Every value below that is not taken as given is logged: never a silent clamp. */
    max = t->cfg.max_recent;
    if (max < 0) {
        tui_logf(GPTPS_LOG_WARN, "gptps_tui_install: cfg.max_recent %d is negative; keeping 8 recent events", max);
        max = 8;
    } else if (max == 0) {
        max = 8;
    } else if (max > TUI_JOURNAL_CAP) {
        tui_logf(GPTPS_LOG_WARN, "gptps_tui_install: cfg.max_recent %d is above the cap of %d; keeping %d "
                 "recent events", max, TUI_JOURNAL_CAP, TUI_JOURNAL_CAP);
        max = TUI_JOURNAL_CAP;
    }
    t->cfg.max_recent = max;
    /* the journals: recent_lines (0: max_recent's), activity_lines (0: recent_lines'); -1 off */
    t->prob_want = t->cfg.recent_lines == 0 ? max : t->cfg.recent_lines < 0 ? 0 : t->cfg.recent_lines;
    t->act_want = t->cfg.activity_lines == 0 ? t->prob_want : t->cfg.activity_lines < 0 ? 0 : t->cfg.activity_lines;
    t->cfg.recent_lines = t->prob_want ? t->prob_want : -1;
    t->cfg.activity_lines = t->act_want ? t->act_want : -1;
    kpi = t->cfg.kpi;
    if (kpi_tier(kpi) < 0) {
        int used = kpi < GPTPS_TUI_KPI_OFF ? GPTPS_TUI_KPI_OFF : GPTPS_TUI_KPI_FULL;
        tui_logf(GPTPS_LOG_WARN, "gptps_tui_install: cfg.kpi %d is not a gptps_tui_kpi (0 to %d); using %s",
                 kpi, (int)GPTPS_TUI_KPI_LATENCY, kpi_str(used));
        kpi = used;
    }
    kpi = TUI_LADDER[kpi_tier(kpi)];               /* DEFAULT is FULL, ACTIVITY is NORMAL */
    t->mode = t->cfg.mode;
    if (t->mode < GPTPS_TUI_CONTINUOUS || t->mode > GPTPS_TUI_PAUSED) {
        tui_logf(GPTPS_LOG_WARN, "gptps_tui_install: cfg.mode %d is not a gptps_tui_mode (0 to %d); using realtime",
                 t->mode, (int)GPTPS_TUI_PAUSED);
        t->mode = GPTPS_TUI_CONTINUOUS;
    }
    t->lat_cap = t->cfg.latency_window;
    if (t->lat_cap < 0) {
        tui_logf(GPTPS_LOG_WARN, "gptps_tui_install: cfg.latency_window %d is negative; tracking 1024 handles",
                 t->lat_cap);
        t->lat_cap = 1024;
    } else if (t->lat_cap == 0) {
        t->lat_cap = 1024;
    } else if (t->lat_cap > TUI_LAT_CAP) {
        tui_logf(GPTPS_LOG_WARN, "gptps_tui_install: cfg.latency_window %d is above the cap of %d handles; "
                 "tracking %d", t->lat_cap, TUI_LAT_CAP, TUI_LAT_CAP);
        t->lat_cap = TUI_LAT_CAP;
    }
    /* Built at the level's needs and no more: installing straight at OFF or MINIMAL holds
     * no per-event memory, the same state gptps_tui_set_kpi reclaims down to. Without the
     * memory for a part, the dashboard runs without it and says so. */
    t->kpi = GPTPS_TUI_KPI_OFF;
    TUI_FLAG_SET(&t->off, 1);
    if (tui_reconfig(t, kpi, 0, -1, -1, what, sizeof what) != GPTPS_OK)
        tui_logf(GPTPS_LOG_WARN, "gptps_tui_install: no memory for %s; the dashboard runs without it", what);
    t->start_ms = gptps_now_ms(NULL);
    if ((st = gptps_register_observer(e, tui_on_event, t)) != GPTPS_OK) {
        /* everything, the handle table too (64 KiB by default, 64 MiB at the cap): the
         * caller gets NULL back and has no handle to reclaim it with */
        tui_logf(GPTPS_LOG_ERROR, "gptps_tui_install: the engine refused the dashboard's observer (%s); "
                 "the dashboard was not installed", gptps_strerror(st));
        tui_destroy(t);
        return NULL;
    }
    mu_lock(&t->mu); t->observing = 1; mu_unlock(&t->mu);
    trk_resync(t);                 /* only now: an item queued before this is missed by the table */
    tui_register_settings(e, t);   /* expose tui.refresh_ms / kpi / mode / latency_window / recent / activity */
    return t;
}

void gptps_tui_close(gptps_tui *t)
{
    if (!t) return;
    term_restore(t);                 /* in case run() was interrupted */
    if (t->nheld) {                  /* never silent: they were taken from the engine */
        char msg[160];
        size_t k;
        snprintf(msg, sizeof msg, "gptps_tui_close: %lu dead letter(s) held by the dashboard were discarded",
                 (unsigned long)t->nheld);
        gptps_log(NULL, GPTPS_LOG_WARN, msg);
        for (k = 0; k < t->nheld; ++k) held_free(&t->held[k]);
    }
    tui_destroy(t);
}

/* ---- runtime reconfiguration ---- */

/* Resize the latency (handle) table at runtime. The old contents are dropped rather than
 * copied: every entry is an in-flight handle awaiting its terminal event, and a resize is
 * an admission that the window was wrong. The gauges are a lower bound ('>=') until the
 * work in flight at the resize has ended; samples resume with the next QUEUED. */
gptps_status gptps_tui_set_latency_window(gptps_tui *t, int handles)
{
    gptps_status st;
    char what[300];
    int changed;
    if (!t || handles < 0) return GPTPS_E_INVAL;
    if (handles == 0) handles = 1024;
    if (handles > TUI_LAT_CAP) {
        tui_logf(GPTPS_LOG_WARN, "gptps_tui_set_latency_window: %d is above the cap of %d handles; tracking %d",
                 handles, TUI_LAT_CAP, TUI_LAT_CAP);
        handles = TUI_LAT_CAP;
    }
    mu_lock(&t->mu);
    /* also when LATENCY or FULL is running without its table (an earlier allocation failed): retry */
    changed = handles != t->lat_cap || (t->tier >= TUI_T_LATENCY && !t->tab);
    mu_unlock(&t->mu);
    if (!changed) return GPTPS_OK;
    st = tui_reconfig(t, -1, handles, -1, -1, what, sizeof what);
    if (what[0]) tui_logf(GPTPS_LOG_WARN, "gptps_tui_set_latency_window: no memory for %s; the dashboard runs "
                          "without it", what);
    return st;
}

gptps_status gptps_tui_set_kpi(gptps_tui *t, gptps_tui_kpi level)
{
    gptps_status st;
    char what[400];
    if (!t) return GPTPS_E_INVAL;
    if (kpi_tier((int)level) < 0) return GPTPS_E_INVAL;
    level = (gptps_tui_kpi)TUI_LADDER[kpi_tier((int)level)];   /* DEFAULT is FULL, ACTIVITY is NORMAL */
    st = tui_reconfig(t, (int)level, 0, -1, -1, what, sizeof what);   /* build what the tier needs, free the rest */
    if (what[0])
        tui_logf(GPTPS_LOG_WARN, "gptps_tui_set_kpi(%s): no memory for %s; the dashboard runs without it",
                 kpi_str(level), what);
    return st;
}

gptps_status gptps_tui_set_recent(gptps_tui *t, int problems, int activity)
{
    gptps_status st;
    char what[400];
    if (!t || problems < -1 || problems > TUI_JOURNAL_CAP || activity < -1 || activity > TUI_JOURNAL_CAP)
        return GPTPS_E_INVAL;
    st = tui_reconfig(t, -1, 0, problems, activity, what, sizeof what);
    if (what[0])
        tui_logf(GPTPS_LOG_WARN, "gptps_tui_set_recent: no memory for %s; the dashboard runs without it", what);
    return st;
}

gptps_status gptps_tui_health(gptps_tui *t, gptps_tui_health_info *out)
{
    gptps_tui_health_info h;
    size_t n;
    if (!t || !out || out->struct_size < offsetof(gptps_tui_health_info, journal_not_shown) + sizeof(uint64_t))
        return GPTPS_E_INVAL;
    memset(&h, 0, sizeof h);
    trk_resync(t);                       /* before the lock: it asks the engine */
    mu_lock(&t->mu);
    {   /* what has left the journals unseen, and what has since the last frame */
        uint64_t lp = t->p_lost + ring_unseen_lost(t->p_seq, t->p_seen, t->prob ? t->pcap : 0);
        h.problems_not_shown = lp;
        h.journal_not_shown = lp + t->a_lost + ring_unseen_lost(t->a_seq, t->a_seen, t->act ? t->acap : 0);
        h.problems_held = ring_held(t->p_seq, t->p_base, t->prob ? t->pcap : 0);
        h.activity_held = ring_held(t->a_seq, t->a_base, t->act ? t->acap : 0);
    }
    h.kpi               = t->kpi;
    h.handles_untracked = t->h_untracked;
    h.orphans_expired   = t->h_orphans;
    h.types_folded      = t->h_folded;
    h.frames_truncated  = t->h_trunc;
    h.alloc_failures    = t->h_nomem;
    h.bytes_base        = sizeof *t + (uint64_t)t->ui_cap * sizeof(tui_ui);
    h.bytes_tasks       = t->tasks ? tasks_bytes(t->max_tasks) : 0;
    h.bytes_problems    = t->prob ? (uint64_t)t->pcap * sizeof(tui_event) : 0;
    h.bytes_activity    = t->act ? (uint64_t)act_bytes(t->acap, t->act_nm != NULL) : 0;
    h.bytes_histogram   = t->hist ? (uint64_t)hist_bytes(t->max_tasks) : 0;
    h.bytes_handles     = t->tab ? (uint64_t)(t->tab_mask + 1) * sizeof(tui_slot) : 0;
    h.bytes_frame       = t->frame_bytes;
    h.bytes_dead_letters = (uint64_t)t->held_cap * sizeof(tui_held);
    h.frames_painted    = t->h_painted;
    h.frames_skipped    = t->h_skipped;
    h.bytes_written     = t->h_written;
    h.wakes             = t->h_wakes;
    h.gauges            = t->tier < TUI_T_GAUGE ? GPTPS_TUI_GAUGES_SUM : !t->tab ? GPTPS_TUI_GAUGES_APPROX
                        : t->trk_lb ? GPTPS_TUI_GAUGES_LOWER_BOUND : GPTPS_TUI_GAUGES_EXACT;
    if (t->tab) { h.pending = t->g_pend; h.in_flight = t->g_run; }
    else if (t->tier < TUI_T_GAUGE) { h.pending = t->q > t->term ? t->q - t->term : 0; h.in_flight = 0; }
    else {
        uint64_t alive = t->q > t->term ? t->q - t->term : 0;
        h.in_flight = t->inflight;
        h.pending = alive > t->inflight ? alive - t->inflight : 0;
    }
    h.handles_tracked   = t->tab ? (uint64_t)(t->tab_n - t->tab_done) : 0;
    h.handles_markers   = t->tab ? (uint64_t)t->tab_done : 0;
    h.task_rows         = t->tasks ? (uint64_t)(t->ntasks - 1) : 0;
    mu_unlock(&t->mu);
    n = out->struct_size < sizeof h ? out->struct_size : sizeof h;   /* what the caller's struct has */
    memcpy((char *)out + sizeof(size_t), (char *)&h + sizeof(size_t), n - sizeof(size_t));
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
    if (!buf) {
        mu_lock(&t->mu); t->h_nomem++; mu_unlock(&t->mu);
        gptps_log(NULL, GPTPS_LOG_WARN, "gptps_tui_snapshot: no memory for the 16 KiB frame buffer; no frame");
        return;
    }
    gptps_tui_render(t, buf, cap);     /* one frame, right now, regardless of mode */
    fputs(buf, t->cfg.out); fputc('\n', t->cfg.out); fflush(t->cfg.out);
    free(buf);
}
