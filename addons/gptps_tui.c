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
#  include <sys/ioctl.h>
#  include <fcntl.h>    /* the live loop's self-pipe: O_NONBLOCK, FD_CLOEXEC */
#  include <poll.h>     /* poll, not select: the self-pipe may be past FD_SETSIZE in a busy host */
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
/* The live loop's wake handle is read with no lock (tui_kick, gptps_tui_wake - which a
 * signal handler may call, so it can take no lock): sequentially consistent operations on
 * a `long`, so that "set the handle to none, then see no waker" in the closer and
 * "count myself a waker, then read the handle" in a waker cannot both miss (see
 * "live loop"). The compiler's builtins, or Win32's Interlocked calls; plain volatile
 * accesses elsewhere (no ordering guarantee - a compiler with neither is not a target). */
#if defined(__GNUC__) || defined(__clang__)
static long sc_get(volatile long *p)         { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static void sc_set(volatile long *p, long v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static void sc_add(volatile long *p, long v) { (void)__atomic_add_fetch(p, v, __ATOMIC_SEQ_CST); }
#elif defined(_WIN32)
static long sc_get(volatile long *p)         { return InterlockedCompareExchange(p, 0, 0); }
static void sc_set(volatile long *p, long v) { (void)InterlockedExchange(p, v); }
static void sc_add(volatile long *p, long v) { (void)InterlockedExchangeAdd(p, v); }
#else
static long sc_get(volatile long *p)         { return *p; }
static void sc_set(volatile long *p, long v) { *p = v; }
static void sc_add(volatile long *p, long v) { *p += v; }
#endif

/* A monotonic clock in microseconds, to MEASURE what a read of the engine costs (the
 * panes', trk_resync's): gptps_now_ms has millisecond resolution, and most of these reads
 * take less. The platform's own clock - no HAL, so the drop-in build has it too. */
#if defined(_WIN32)
static uint64_t tui_now_us(void)
{
    LARGE_INTEGER f, c;
    if (!QueryPerformanceFrequency(&f) || !QueryPerformanceCounter(&c) || f.QuadPart <= 0)
        return gptps_now_ms(NULL) * 1000u;
    return (uint64_t)(c.QuadPart / f.QuadPart) * 1000000u + (uint64_t)(c.QuadPart % f.QuadPart) * 1000000u / (uint64_t)f.QuadPart;
}
#else
#  include <time.h>
static uint64_t tui_now_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return gptps_now_ms(NULL) * 1000u;
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
#endif

/* The dashboard's clock (ms): gptps_now_ms, unless a test that compiles this file in
 * defines TUI_NOW_MS first (tests/test_tui_unit.c drives ADAPT's windows with it). */
#ifndef TUI_NOW_MS
#  define TUI_NOW_MS() gptps_now_ms(NULL)
#endif
/* ADAPT's frame timing (us) and the gap between tui_adapt's two locked sections: hooks
 * for that test too (it fakes the clock, so a real frame's time is not the window's). */
#ifndef TUI_FRAME_US
#  define TUI_FRAME_US(us) (us)
#endif
#ifndef TUI_ADAPT_MID
#  define TUI_ADAPT_MID(t) ((void)0)
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
/* The frame buffer of gptps_tui_run and gptps_tui_snapshot: it starts at the first size and
 * grows to what a frame needs, up to the second; past that a frame is cut, with a visible
 * "[frame truncated]" line (counted). */
#define TUI_FRAME_MIN (16u * 1024u)
#define TUI_FRAME_MAX (256u * 1024u)
/* The rows a frame is fitted to at most (a taller terminal draws this many). */
#define TUI_ROWS_MAX  1000
/* done/s: the snapshots of the finished count the windowed rate is computed from, taken at
 * render - none on the event path - at most one per max(1 s, window / TUI_RATE_N). */
#define TUI_RATE_N    32
/* The RECENT journals' sizes (cfg.recent_lines / activity_lines, tui.recent / tui.activity):
 * the most a journal may hold, and the default (1.10's max_recent default). */
#define TUI_JOURNAL_CAP 4096
#define TUI_JOURNAL_DEF 8
/* An ACTIVITY record's task name, when there is no TASKS table to name it from by row. */
#define TUI_ACT_NAME  64
/* Latency percentiles (FULL): a log2-ms histogram per row - bucket 0 holds 0 ms, bucket b
 * (1..30) [2^(b-1), 2^b) ms, bucket 31 everything from 2^30 ms. */
#define TUI_HIST_B    32
/* The engine-reading panes (tasks, settings, detail) and trk_resync hold the engine's lock
 * for each call they make. Before ABI 2.9 a gptps_task_get_info call walked every queue
 * under it (~1 ms a call, 116 ms a 64-row pass at 64 types and 100k queued); since 2.9 it
 * reads counters, O(1), and trk_resync makes one gptps_engine_counts call. A pass is still
 * timed, and the next waits at least this many times as long as the last one took - so
 * they hold it at most 1/TUI_DUTY of the time, a bound that now rarely binds. */
#define TUI_DUTY      100u
/* ADAPT (gptps_tui.h): a condition WARNs at most once this often; DEGRADE sheds after this
 * many windows of pressure in a row; ACTIVITY records lost unseen at this rate (a second)
 * or more are pressure; a frame taking more than 1/TUI_RENDER_DIV of refresh_ms is; the
 * default estimated-cost budget, in percent of one core. */
#define TUI_WARN_GAP_MS  60000u
#define TUI_SHED_AFTER   3u
#define TUI_ACT_FLOOD    1000u
#define TUI_RENDER_DIV   10u
#define TUI_COST_DEF     5
#define TUI_P_N          9          /* GPTPS_TUI_P_* bits */
/* ADAPT, GPTPS_TUI_SHED_PANE (1.13): each such step doubles the engine-reading panes' interval
 * (what runs, pane_every_ms), up to this (ms) - past a minute a pane is no longer read live */
#define TUI_PANE_SHED_MAX 60000u

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
/* ADAPT's unit cost of an event at each tier, ns (gptps_tui.h, ADAPT): the upper end of
 * what one thread measured (the KPI table's "measured" column, re-measured for 1.12 with
 * tests/test_tui_perf.c's harness: 5.5 / 9.3 / 10.0 / 10.1 / 17.0 / 19.1 / 30.2 / 30.1 ns
 * at 512 in flight). OFF's is never used: its totals stand still, so it has no rate.
 * TUI_COST_ACT: what the ACTIVITY journal adds (NORMAL - TASKS). */
static const unsigned TUI_COST_NS[TUI_T_FULL + 1] = { 3, 9, 10, 10, 21, 24, 38, 38 };
#define TUI_COST_ACT 3u
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

/* The tier that runs for a requested `tier` once the DEGRADE steps `shed` are taken out
 * (gptps_tui.h, ADAPT): each later step caps the ladder lower. `act`: the ACTIVITY journal
 * is wanted (show_recent, a size above 0). Since 1.13 a step may be taken on its own (the
 * first that relieves what is present), so -latency caps at NORMAL while the journal still
 * runs - the handle table out, the journal kept - and at TASKS once it does not (1.12's
 * tier after "-activity -percentiles -latency", unchanged). GPTPS_TUI_SHED_PANE caps no tier. */
static int shed_cap(int tier, unsigned shed, int act)
{
    int cap = (shed & GPTPS_TUI_SHED_TASKS) ? TUI_T_PROBLEMS
            : (shed & GPTPS_TUI_SHED_LATENCY) ? (act && !(shed & GPTPS_TUI_SHED_ACTIVITY) ? TUI_T_ACTIVITY : TUI_T_TASKS)
            : (shed & GPTPS_TUI_SHED_PERCENTILES) ? TUI_T_LATENCY : TUI_T_FULL;
    return tier < cap ? tier : cap;
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

/* The dashboard pane's legend: an item per printable hotkey bound with gptps_tui_add_task
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

/* ---- the frame's copy-out (see "render") ----
 * A dashboard frame is made in two steps: what it shows is COPIED out of the live state
 * under `mu` - counters, gauges, the TASKS rows and RECENT records that fit the terminal,
 * nothing more - and then FORMATTED with the lock released, so an engine thread waits for
 * a few microseconds of copying, not for the formatting (render held `mu` ~50 us). The
 * copy lives in a tui_view, allocated at the first frame and kept (only render touches it,
 * under `rmu`); the legend and the rule are cached in it and rebuilt only when the
 * bindings, the width or the glyphs change. */
typedef struct {
    char     label[TUI_NAME_CAP];      /* the row's label (or name), raw: cleaned when shown */
    int      hotkey, has_mem;
    uint64_t started, finished, failed, dead, dropped, lat_sum, lat_max, lat_n, mem_peak;
    char     pq[3][16];                /* p50 / p95 / p99 (FULL, a wide terminal) */
} tui_vrow;
typedef struct { uint64_t ms, fin; } tui_rs;   /* a done/s snapshot: when, and the finished count */
typedef struct {
    /* header */
    int      tier, kpi, mode, color, uni, W, R, clock, rate_state;   /* rate_state: 0 "-", 1 a rate, 2 "n/a" */
    uint32_t refresh;
    uint64_t up_ms;
    double   rate;
    uint64_t q, s, fin, fail, retr, dead, drop, cancel, alive, inflt, pend, peak;
    int      exact, lb, miss;          /* miss: the parts the tier runs without (bits, see dash_format) */
    uint64_t l_folded, l_untracked, l_plost, l_trunc, l_nomem;
    /* TASKS */
    int      has_tasks, pcols, mcols, nrows, more, nfold, fold_more, max_tasks, lat_cap, has_tab;
    uint64_t lat_untracked, lat_unmatched;
    size_t   rows_at;                  /* where this frame's rows begin, after the header (tui_finish) */
    /* RECENT */
    int      has_rec, act_on, scroll, nrec;
    uint64_t frame_unseen, rec_t0;     /* rec_t0: install time, RECENT's 0 */
    int      toast_on;
    char     status[320];
    /* ADAPT: the PRESSURE line (empty: none) and what DEGRADE shed (0: nothing), with why */
    char     pline[1280];
    int      nplines;
    unsigned shed;
    uint64_t shed_why;
    int      kpi_req;
    /* the live loop: when this frame next changes with no event (0: never), and the state
     * generation it shows */
    uint64_t next_change, gen;
    /* caches: rebuilt only when what they are made of changes */
    char     legend[TUI_LEGEND_CAP];
    int      legrows, leg_W, leg_uni, leg_ok;
    uint64_t leg_builds;                 /* times the legend was rebuilt (tests: the cache holds) */
    uint64_t leg_gen;
    char     rule[16 + 200 * 3 + 16];
    int      rule_W, rule_uni;
    /* the rows and records this frame shows: at most R of each */
    tui_vrow  *rows;
    tui_event *recs;
    int        rows_cap, recs_cap;
    /* SOURCE (gptps_tui_set_source): this frame's read - src 0 none, 1 read, 2 failed (own
     * counts shown); gline: the gauge line (else MINIMAL's one sum); src_lat: a row has
     * latency (the max ms column); src_warn: WARN about the failure once mu is dropped */
    int        src, gline, src_lat, src_warn;
    gptps_status src_st;
    uint64_t   src_errors;
    size_t     src_n;
    char       src_name[32];
    gptps_tui_source_totals stot;
    gptps_tui_source_row   *srows;   /* the read's rows: srows_cap (= rows_cap) of them */
    int        srows_cap;
} tui_view;

/* ---- the engine-reading panes' cache (see "pane cache") ----
 * 1.10's tasks pane read gptps_task_get_info for every row on every frame - each call held
 * the engine's lock while it walked every queue before ABI 2.9 (116 ms a frame at 64 types
 * and 100k queued: 46% of the time at 250 ms; O(1) a call since 2.9) - and the settings and
 * detail panes walked the whole registry two or three times a frame. Now an open pane reads the engine in ONE pass into
 * this cache, and its frames - and j/k - work from the cache. A pass is made when the pane
 * opens, after a change made from it, on 'R', and then every pane_refresh_ms - or, when
 * that is sooner than TUI_DUTY times what the last pass took (it is timed), only then, so
 * the panes hold the engine's lock at most 1% of the time. The pane says how old what it
 * shows is. Touched under rmu only (render, and press); freed when no such pane is shown. */
typedef struct { char name[TUI_NAME_CAP]; int exec, enabled, removed; int32_t pri; uint32_t queued, running, dead; } tui_ctask;
/* a setting: its key and value in `pool` (offsets), its index in the registry when read */
typedef struct { size_t key, val, idx; int hot; } tui_cset;
typedef struct {
    int        pane;              /* the pane read for (TUI_PANE_*), -1: none */
    char       task[128];         /* the detail pane's task, as read */
    tui_ctask *tasks;
    tui_cset  *sets;
    char      *pool;
    size_t     n, cap, pool_len, pool_cap;
    size_t     total;             /* settings: the registry's size at the pass */
    size_t     short_by;          /* entries the pass had no memory for (counted, shown) */
    int        kw;                /* the key column's width */
    int        stale;             /* a change made from the pane, or 'R': read at the next frame */
    uint64_t   read_ms, cost_us, every_ms;
    /* the selected setting, explained under the list: read (one call) when the selection
     * moves; its kind ("one of: ...") formatted at once - the choices are borrowed */
    int        sel_for, have_sel;
    gptps_setting_info sel;
    char       sel_kind[640];
} tui_pcache;

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
    uint64_t         drop_pend;     /* unseen records a resize / free / tier drop took out, not yet
                                     * on a frame's RECENT line (added there by the next one) */
    int              p_warned;      /* the WARN for the first PROBLEMS record lost has been logged */
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
    uint64_t         h_trunc_dash;  /* the part of h_trunc that were dashboard frames (ADAPT, 1.13) */
    uint64_t         h_painted, h_skipped, h_written, h_wakes, h_made;   /* h_made: frames the loop formatted */
    size_t           frame_bytes;   /* the frame buffer gptps_tui_run holds while it runs */
    int              scroll;        /* recent-log scroll offset (lines back) */
    int              kpi;           /* effective gptps_tui_kpi (never DEFAULT) */
    int              mode;          /* effective gptps_tui_mode */
    int              dirty;         /* state changed since the live loop's last frame */
    /* ---- the live loop (see "live loop") ----
     * gen: bumped by every event and every UI change (under mu) - CONTINUOUS skips a frame
     * whose generation, clock and toast are unchanged. ui_gen: the add_task bindings
     * changed (the cached legend). waiting: the loop blocks in ON_DEMAND, so an event that
     * turns `dirty` 0 -> 1 wakes it (after mu_unlock: never a syscall under the lock). */
    uint64_t         gen, ui_gen, h_kicks;
    int              waiting;
    /* The wake handle, only while gptps_tui_run runs (POSIX: the self-pipe's write end, -1
     * else; Windows: an auto-reset event, 0 else), read with no lock through sc_* (see
     * there); wakers counts the calls between reading it and using it, so run closes it only
     * once none is; wake_req: gptps_tui_wake asked for a resize check and a repaint. POSIX:
     * wake_h is the pipe's write end + 1; Windows: 1 while wake_ev is the event. 0: none. */
    volatile long    wake_h, wakers, wake_req;
    /* the windowed done/s (TUI_RATE_N snapshots, a ring) and peak (two buckets: this window
     * and the last), advanced at render, under mu; clock is the effective gptps_tui_clock */
    tui_rs           rate[TUI_RATE_N];
    int              rate_n, rate_head, clock;
    uint64_t         peak_prev, peak_ms;
    /* render (dashboard pane): its copy-out, and the lock that makes render one at a time -
     * taken before mu (and before any engine lock), never while holding one */
    tui_mutex        rmu;
    tui_view        *view;
    int              view_warned;
    tui_pcache       pc;            /* the engine-reading panes' cache (under rmu) */
    /* their passes, for gptps_tui_health (under mu): count, the last one's cost, the
     * interval it set, the cache's bytes; h_pane_us: every pass's cost, summed */
    uint64_t         h_pane_reads, h_pane_us, pane_cost_us, pane_every_ms, pane_bytes;
    /* trk_resync's own duty limit (under mu): no look before this (tui_now_us; 0: any time) */
    uint64_t         rs_next_us, rs_cost_us;
    int              rs_held;     /* a look was held back since the last one: a frame is due at rs_next_us */
    /* ---- ADAPT (1.12; see "adapt"), under mu ----
     * kpi_req: the tier asked for (kpi is what runs: kpi_req minus shed). adapt: the
     * gptps_tui_adapt in effect; cost_pct: the estimated-cost budget. ad_ms: when this window
     * began; ad_*: the counters then, so a window's conditions are what changed in it;
     * ad_rmax_us: the slowest frame made in it. The last window's results: pressure (bits),
     * pr_seen, p_row (windows in a row with a sheddable one), ad_rate, ad_ppm; ad_line: the
     * PRESSURE line's text; ad_warn_ms: each condition's last WARN. shed: GPTPS_TUI_SHED_*,
     * shed_why: the conditions behind the last step; ad_floor_ms: the last "nothing left" WARN;
     * ad_gen: a new window (adapt_base) since, so a late PRESSURE text is not stored.
     * 1.13: shed_whys: the conditions behind every step since the operator last cleared it
     * (the DEGRADED line's "(pressure: ...)"); pane_shed_ms: the engine-reading panes'
     * interval floor GPTPS_TUI_SHED_PANE set (0: none); ad_trd: h_trunc_dash at the window's
     * start (TRUNCATED acts only for dashboard frames). */
    int              kpi_req, adapt, cost_pct;
    unsigned         shed;
    uint64_t         ad_ms, ad_ev, ad_plost, ad_alost, ad_untr, ad_fold, ad_trunc, ad_nomem, ad_preads, ad_rmax_us;
    uint64_t         pressure, pr_seen, p_row, shed_why, ad_windows, ad_steps, ad_warns, ad_rate, ad_ppm, ad_rmax_last;
    uint64_t         shed_whys, pane_shed_ms, ad_trd;
    uint64_t         ad_warn_ms[TUI_P_N], ad_floor_ms, ad_gen;
    char             ad_line[1280];   /* one line a condition, '\n' between */
    /* ---- SOURCE (1.12; gptps_tui_set_source) ----
     * src: the host's source (src.read NULL: none; src.name not kept - src_name is the copy).
     * Written under rmu AND mu, so a frame - which calls it under rmu, mu released - never
     * sees it change, and set_source returns only once no frame is using the old one.
     * Under mu: src_reads / src_errors / src_rows / src_status for health; src_warned and
     * src_warn_ms, the failure WARN's rate limit; src_kind, what the last frame's totals came
     * from (0 own, 1 the source, -1 changed: done/s starts again, as its counts changed
     * hands); sp_*: peak's two buckets over the source's in-flight gauge. */
    gptps_tui_source src;
    char             src_name[32];
    uint64_t         src_reads, src_errors, src_rows, src_warn_ms;
    int              src_status, src_kind, src_warned;
    uint64_t         sp_peak, sp_prev, sp_ms;
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
    DWORD            saved_in_mode;  int in_mode_saved;
    UINT             saved_cp;      /* console output codepage to restore (0 = none saved) */
    HANDLE           wake_ev;       /* the live loop's event (see wake_h) */
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

/* Wake the live loop, if one runs (see "live loop"): one byte down its self-pipe, or its
 * event set. Takes no lock and calls only write() / SetEvent, so a signal handler may call
 * it (gptps_tui_wake). A full pipe already holds a wake: EAGAIN is ignored. */
static void tui_kick(gptps_tui *t)
{
    sc_add(&t->wakers, 1);                       /* before reading the handle: see run's close */
#if !defined(_WIN32)
    {
        long h = sc_get(&t->wake_h);
        if (h > 0) { char c = 0; ssize_t r = write((int)(h - 1), &c, 1); (void)r; }
    }
#else
    if (sc_get(&t->wake_h)) SetEvent(t->wake_ev);
#endif
    sc_add(&t->wakers, -1);
}

void gptps_tui_wake(gptps_tui *t)
{
    int saved = errno;                           /* a signal handler must leave errno as it was */
    if (!t) return;
    sc_set(&t->wake_req, 1);
    tui_kick(t);
    errno = saved;
}

/* mu held: the state changed (a setter, a binding): the next frame is due. The caller
 * calls tui_kick once it has dropped mu (a loop may be waiting, in any mode). */
static void tui_touch(gptps_tui *t) { t->gen++; t->dirty = 1; t->waiting = 0; }
/* Ask the live loop to return (it is woken: it may be blocked with no deadline). */
static void tui_quit(gptps_tui *t) { mu_lock(&t->mu); t->quit = 1; tui_touch(t); mu_unlock(&t->mu); tui_kick(t); }


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
 * One gptps_engine_counts call (ABI 2.9: O(1), counters the engine keeps) - 1.11 asked
 * gptps_task_get_info of every type, and each of those walked every queue. Takes the
 * engine's lock, so never under mu. */
static int engine_idle(gptps *e)
{
    gptps_engine_totals c;
    if (!e) return 0;
    memset(&c, 0, sizeof c);
    c.struct_size = sizeof c;
    if (gptps_engine_counts(e, &c) != GPTPS_OK) return 0;   /* a forked child: say no */
    return c.queued == 0 && c.running == 0;
}

/* Clear the lower-bound mark (trk_lb) when that is sound: the engine has nothing queued
 * or running, the table holds nothing live, and no handle was missed since this call
 * began (lb_gen). Then nothing alive can be missing from the table - anything later
 * sends its events to the observer, which is registered. Any other moment is not
 * sound: an end still on its way to the observer can stand in for an item the table
 * never saw. So an engine that is never idle (an always-up service) keeps '>=', which
 * stays true. Called by render, gptps_tui_health and tui_reconfig, never under mu.
 * The look holds the engine's lock (one gptps_engine_counts call since 1.12, O(1); in 1.11
 * a gptps_task_get_info walk of every queue, ~2 ms at 100k queued), so it is timed (at
 * least 1 us), and the next look waits TUI_DUTY - 1 times as long as this one took: at most 1%
 * of the time in the engine's lock, however often a host renders. A look held back sets
 * rs_held, and tui_frame then asks for a frame when the hold ends (rs_due_ms), so an
 * engine gone idle meanwhile does not keep '>=' until some other change. `force`: not
 * held back (install and tui_reconfig: a new table on an idle engine is exact at once;
 * gptps_tui_health: the host asks, at a rate it chooses), but it still sets the hold. */
static void trk_resync(gptps_tui *t, int force)
{
    uint64_t gen, t0, t1;
    int cleared = 0, idle;
    mu_lock(&t->mu);
    if (!t->tab || !t->trk_lb || !t->observing) { t->rs_held = 0; mu_unlock(&t->mu); return; }
    if (!force && t->rs_next_us && tui_now_us() < t->rs_next_us) { t->rs_held = 1; mu_unlock(&t->mu); return; }
    t->rs_held = 0;
    gen = t->lb_gen;
    mu_unlock(&t->mu);
    t0 = tui_now_us();
    idle = engine_idle(t->e);
    t1 = tui_now_us();
    mu_lock(&t->mu);
    t->rs_cost_us = t1 > t0 ? t1 - t0 : 1u;                  /* below the clock's resolution: 1 us */
    t->rs_next_us = t1 + t->rs_cost_us * (TUI_DUTY - 1u);
    mu_unlock(&t->mu);
    if (!idle) return;
    mu_lock(&t->mu);
    if (t->tab && t->trk_lb && t->lb_gen == gen && t->tab_n == t->tab_done) { t->trk_lb = 0; tui_touch(t); cleared = 1; }
    mu_unlock(&t->mu);
    if (cleared) tui_kick(t);
}

/* When a frame is due for a look trk_resync held back (gptps_now_ms terms; 0: none). */
static uint64_t rs_due_ms(gptps_tui *t, uint64_t now)
{
    uint64_t due = 0, nu;
    mu_lock(&t->mu);
    if (t->rs_held && t->trk_lb) {
        nu = tui_now_us();
        due = now + (t->rs_next_us > nu ? (t->rs_next_us - nu + 999u) / 1000u : 0u);
        if (due <= now) due = now + 1u;
    }
    mu_unlock(&t->mu);
    return due;
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
    int first_fold = 0, win = 0, g, kick = 0;
    uint64_t depth = 0, running = 0;
    if (TUI_FLAG_GET(&t->off)) return;   /* KPI OFF: before the lock, deliberately - see the field */
    /* A running job's samples (measure.sample_ms) are not part of any item's life: they
     * would only flood the log. The peak column reads the attempts' own measurements. */
    if (ev->kind == GPTPS_EV_SAMPLE) return;
    mu_lock(&t->mu);
    t->gen++;
    /* Something changed. The first change after a frame wakes a live loop that waits in
     * ON_DEMAND - once: until the loop paints, `dirty` stays 1 and nothing more is sent, so
     * a burst costs one wake per frame interval, and none when no loop waits. */
    if (!t->dirty) {
        t->dirty = 1;
        if (t->waiting) { t->waiting = 0; t->h_kicks++; kick = 1; }
    }
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

    /* MINIMAL and GAUGE stop here. Each part below runs only when it exists, and it exists
     * only at its tier and up (tui_reconfig); the tier test only skips the probes. */
    if (t->tier < TUI_T_PROBLEMS) goto out;
    if (t->prob && ev_problem(ev->kind)) {       /* nothing on a success */
        tui_event *re = &t->prob[t->phead];
        re->ts = ev->ts_ms; re->kind = ev->kind; re->handle = ev->handle; re->status = ev->status;
        copy_u8(re->name, sizeof re->name, ev->task_name);   /* whole: a name is at most GPTPS_TASK_NAME_MAX */
        why_record(ev, re);
        t->phead = t->phead + 1 == t->pcap ? 0 : t->phead + 1;
        t->p_seq++;
    }
    if (t->tier < TUI_T_TASKS) goto out;
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
out:
    mu_unlock(&t->mu);
    if (kick) tui_kick(t);                       /* after the lock: never a syscall under it */
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

/* Bounded formatted append into buf at pos. Returns the new pos AS IF the buffer had room:
 * past cap nothing is written, but the length is still counted, so a frame that did not
 * fit says how much room it needed (gptps_tui_run and gptps_tui_snapshot grow their buffer
 * to it) and frame_finish cuts it. Never index buf with a pos that may be past cap. */
static size_t appendf(char *buf, size_t cap, size_t pos, const char *fmt, ...)
{
    va_list ap; int n;
    va_start(ap, fmt);
    n = pos < cap ? vsnprintf(buf + pos, cap - pos, fmt, ap) : vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return pos;
    return pos + (size_t)n;
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
 * with rmu held, mu NOT held. A dashboard frame cut after its header (tui_view.rows_at,
 * set by this frame's dash_format) is one that fewer rows can make fit: h_trunc_dash, what
 * DEGRADE acts on - whatever the buffer (run's / snapshot's, grown as far as memory let it,
 * or a host's). One whose header alone did not fit - a host's small gptps_tui_render buffer
 * (a status widget), or a title longer than the frame - is counted in h_trunc and reported,
 * never shed for: no step shortens the header. (The rows alone stay far under TUI_FRAME_MAX:
 * at most TUI_ROWS_MAX of at most the width.) */
static size_t tui_finish(gptps_tui *t, char *buf, size_t cap, size_t pos)
{
    if (cap && pos >= cap) {
        int rows = t->pane == TUI_PANE_DASH && t->view && t->view->rows_at < cap;
        mu_lock(&t->mu);
        t->h_trunc++;
        if (rows) t->h_trunc_dash++;
        mu_unlock(&t->mu);
    }
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
    /* %.8s: fmt_count writes at most 5 characters ("99999", "1.1G"); the bound lets the
     * compiler see the result fits a 16-byte cell (gcc -O3 -Wformat-truncation) */
    if (b == 0) snprintf(out, cap, "0");
    else if (b == TUI_HIST_B - 1) snprintf(out, cap, ">=%.8s", fmt_count(v, sizeof v, (uint64_t)1 << (TUI_HIST_B - 2)));
    else snprintf(out, cap, "<%.8s", fmt_count(v, sizeof v, (uint64_t)1 << b));
    return out;
}

/* Arrow glyphs for the key legends: real arrows when unicode is on, ASCII words
 * otherwise. Arrows reuse j/k/Enter/Esc, so both still work; the legend just
 * showcases the arrows. */
static const char *a_ud(const gptps_tui *t) { return t->cfg.unicode ? "\xe2\x86\x91\xe2\x86\x93" : "Up/Dn"; }
static const char *a_lt(const gptps_tui *t) { return t->cfg.unicode ? "\xe2\x86\x90"             : "Left";  }
static const char *a_rt(const gptps_tui *t) { return t->cfg.unicode ? "\xe2\x86\x92"             : "Right"; }

/* "GPTPS · " before the title on every pane - unless the title already starts with GPTPS
 * (examples/dashboard.c's "GPTPS live demo" read "GPTPS · GPTPS live demo"). */
static const char *brand(const gptps_tui *t) { return strncmp(t->cfg.title, "GPTPS", 5) == 0 ? "" : "GPTPS \xc2\xb7 "; }

/* The rows a frame is fitted to: the terminal's, 24 headless, at most TUI_ROWS_MAX. */
static int draw_rows(const gptps_tui *t)
{
    int R = t->rows > 0 ? t->rows : 24;
    return R > TUI_ROWS_MAX ? TUI_ROWS_MAX : R;
}

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
static size_t setting_details(gptps_tui *t, char *buf, size_t cap, size_t pos, const gptps_setting_info *in,
                              const char *kind)
{
    int color = t->cfg.color, W = draw_width(t) - 4, lines = 0;
    const char *B = color ? "\x1b[1m" : "", *D = color ? "\x1b[2m" : "", *X = color ? "\x1b[0m" : "";
    char desc[GPTPS_SETTINGS_VALUE_MAX * 2], key[GPTPS_SETTINGS_VALUE_MAX * 2], ckind[640],
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
    tui_cl(kind, ckind, sizeof ckind);              /* an enum's choices are the host's text */
    pos = appendf(buf, cap, pos, "  %s%s \xc2\xb7 default %s \xc2\xb7 %s%s\n", D, ckind,
                  in->defval[0] ? tui_cl(in->defval, dv, sizeof dv) : "(empty)",
                  in->hot ? "applies at once" : "applies at the next start (save with w, then restart)", X);
    return pos;
}

/* One row of a settings list: "> key = value (46.9 GiB) (restart)", the key padded
 * to kw columns. Key and value are cleaned: a STRING setting can hold any byte the
 * host or a config file put there (TOML's \u001B is an ESC). */
static size_t setting_row(char *buf, size_t cap, size_t pos, const char *value, int hot, const char *key,
                          int kw, int selected, const char *INV, const char *X)
{
    char k[GPTPS_SETTINGS_VALUE_MAX * 2], v[GPTPS_SETTINGS_VALUE_MAX * 2], hint[32];
    int pad;
    tui_cl(key, k, sizeof k);
    tui_cl(value, v, sizeof v);
    bytes_hint(k, value, hint, sizeof hint);
    pad = kw - u8_cols(k);
    if (pad < 0) pad = 0;
    return appendf(buf, cap, pos, "%s%s%s%*s = %s%s%s %s%s\n", selected ? INV : "", selected ? "> " : "  ",
                   k, pad, "", v, hint, X, hot ? "" : "(restart)", X);
}

/* The status line of a pane, cleaned: set_status puts the committed value in it, and
 * the task panes put task names - text the dashboard did not write. */
static size_t status_line(gptps_tui *t, char *buf, size_t cap, size_t pos, const char *lead, const char *D, const char *X)
{
    char shown[sizeof t->status * 2];
    if (!status_live(t->status, t->status_ms, TUI_NOW_MS())) return pos;   /* faded, as on the dashboard */
    return appendf(buf, cap, pos, "%s%s%s%s\n", lead, D, tui_cl(t->status, shown, sizeof shown), X);
}

/* The rows a list may use: what the terminal has, less the `used` lines the pane prints
 * around it. */
static int list_rows(const gptps_tui *t, int used)
{
    int r = (t->rows > 0 ? t->rows : 24) - used;
    return r < 3 ? 3 : r;
}

static size_t setting_details(gptps_tui *t, char *buf, size_t cap, size_t pos, const gptps_setting_info *in,
                              const char *kind);
static size_t pc_footer(gptps_tui *t, char *buf, size_t cap, size_t pos, const char *what, const char *D, const char *X);

/* The lines a settings or detail pane prints besides its list, as this frame has them:
 * the title and its blank line, the selected setting's details (4 to 6), the edit prompt
 * (2), the status, the age line, the blank line and the legend, and the frame's last
 * newline. 1.11 stage D took a fixed 14, which hid one of a task's 11 settings at 24 rows. */
static int settings_used(gptps_tui *t)
{
    const tui_pcache *c = &t->pc;
    int used = 2 + (t->editing ? 2 : 0) + (status_live(t->status, t->status_ms, TUI_NOW_MS()) ? 1 : 0) + 1 + 2 + 1;
    if (c->n && c->have_sel) {
        char tmp[4096];
        size_t n = setting_details(t, tmp, sizeof tmp, 0, &c->sel, c->sel_kind), i;
        int nl = 0;
        if (n >= sizeof tmp) nl = 6;                       /* the most it prints */
        else for (i = 0; i < n; ++i) nl += tmp[i] == '\n';
        used += nl;
    }
    return used;
}

static size_t edit_line(gptps_tui *t, char *buf, size_t cap, size_t pos, const char *B, const char *D, const char *X);

/* bounded name copy (avoids -Wformat-truncation and silent over-read) */
static void copy_name(char *dst, size_t cap, const char *src)
{ if (cap) snprintf(dst, cap, "%.*s", (int)(cap - 1), src ? src : ""); }

/* ---- pane cache (tasks, settings, detail; see tui_pcache) --------------------------
 * Everything here runs with rmu held and mu NOT held: a pass calls the engine (its locks,
 * and the dashboard's own tui.* read callbacks, which take mu). */
static int pc_kind(int pane) { return pane == TUI_PANE_TASKS || pane == TUI_PANE_SETTINGS || pane == TUI_PANE_DETAIL; }
static size_t pc_bytes(const tui_pcache *c)
{ return c->cap * (c->tasks ? sizeof(tui_ctask) : c->sets ? sizeof(tui_cset) : 0) + c->pool_cap; }

static void pc_free(gptps_tui *t)
{
    tui_pcache *c = &t->pc;
    free(c->tasks); free(c->sets); free(c->pool);
    memset(c, 0, sizeof *c);
    c->pane = -1; c->sel_for = -1;
    mu_lock(&t->mu); t->pane_bytes = 0; mu_unlock(&t->mu);
}

static int pc_grow(tui_pcache *c, int tasks, size_t want)
{
    size_t nc = c->cap ? c->cap : 16;
    if (want <= c->cap) return 1;
    while (nc < want) nc *= 2;
    if (tasks) { tui_ctask *n = (tui_ctask *)realloc(c->tasks, nc * sizeof *n); if (!n) return 0; c->tasks = n; }
    else       { tui_cset *n = (tui_cset *)realloc(c->sets, nc * sizeof *n);   if (!n) return 0; c->sets = n; }
    c->cap = nc;
    return 1;
}

/* s into the pool: its offset, or (size_t)-1 with no memory */
static size_t pc_str(tui_pcache *c, const char *s)
{
    size_t l = strlen(s) + 1, off;
    if (c->pool_len + l > c->pool_cap) {
        size_t nc = c->pool_cap ? c->pool_cap : 4096;
        char *np;
        while (nc < c->pool_len + l) nc *= 2;
        if ((np = (char *)realloc(c->pool, nc)) == NULL) return (size_t)-1;
        c->pool = np; c->pool_cap = nc;
    }
    off = c->pool_len;
    memcpy(c->pool + off, s, l);
    c->pool_len += l;
    return off;
}

/* The detail pane's key prefix, "tasks.<name>.", and its length. */
static size_t detail_prefix(const char *task, char *out, size_t cap)
{
    int n = snprintf(out, cap, "tasks.%s.", task);
    return n < 0 ? 0 : (size_t)n >= cap ? cap - 1 : (size_t)n;
}

/* ONE pass over the engine for the pane shown: every task type (tasks), every setting
 * (settings), or the settings of the detail pane's task. Timed: the next pass is due
 * pane_refresh_ms later, or TUI_DUTY times what this one took when that is longer. */
static void pc_read(gptps_tui *t, uint64_t now)
{
    tui_pcache *c = &t->pc;
    int p = t->pane, nomem = 0, tasks = p == TUI_PANE_TASKS;
    size_t i, n, plen = 0;
    char prefix[160];
    uint64_t t0 = tui_now_us(), t1, every;
    if (tasks && c->sets)       { free(c->sets);  c->sets = NULL;  c->cap = 0; }
    else if (!tasks && c->tasks) { free(c->tasks); c->tasks = NULL; c->cap = 0; }
    c->n = 0; c->pool_len = 0; c->short_by = 0; c->kw = p == TUI_PANE_DETAIL ? 16 : 20;
    if (p == TUI_PANE_DETAIL) plen = detail_prefix(t->detail_task, prefix, sizeof prefix);
    n = tasks ? gptps_task_count(t->e) : gptps_settings_count(t->e);
    c->total = n;
    if (p != TUI_PANE_DETAIL && n > c->cap && !pc_grow(c, tasks, n)) nomem = 1;   /* then entry by entry */
    for (i = 0; i < n; ++i) {
        if (tasks) {
            gptps_task_info info;
            tui_ctask *k;
            if (!tui_task_at(t->e, i, &info)) continue;          /* gone meanwhile */
            if (c->n == c->cap && !pc_grow(c, 1, c->n + 1)) { c->short_by++; nomem = 1; continue; }
            k = &c->tasks[c->n++];
            copy_name(k->name, sizeof k->name, info.name_copy);
            k->exec = (int)info.exec; k->pri = info.priority; k->enabled = info.enabled; k->removed = info.removed;
            k->queued = info.queued; k->running = info.running; k->dead = info.dead;
        } else {
            gptps_setting_info info;
            tui_cset *st;
            size_t ko, vo = (size_t)-1;
            int kc;
            if (!tui_setting_at(t->e, i, &info)) continue;
            if (plen && strncmp(info.key_copy, prefix, plen) != 0) continue;
            if (c->n == c->cap && !pc_grow(c, 0, c->n + 1)) { c->short_by++; nomem = 1; continue; }
            if ((ko = pc_str(c, skey(&info))) != (size_t)-1) vo = pc_str(c, info.value);
            if (vo == (size_t)-1) { c->short_by++; nomem = 1; continue; }
            st = &c->sets[c->n++];
            st->key = ko; st->val = vo; st->idx = i; st->hot = info.hot;
            kc = u8_cols(skey(&info) + plen);
            if (kc > c->kw) c->kw = kc;
        }
    }
    if (c->kw > (p == TUI_PANE_DETAIL ? 40 : 44)) c->kw = p == TUI_PANE_DETAIL ? 40 : 44;
    t1 = tui_now_us();
    c->cost_us = t1 > t0 ? t1 - t0 : 0;
    every = (c->cost_us * TUI_DUTY + 999u) / 1000u;           /* ms: at most 1/TUI_DUTY of the time */
    mu_lock(&t->mu);
    {   /* never sooner than pane_refresh_ms - or what GPTPS_TUI_SHED_PANE raised it to */
        uint64_t base = t->cfg.pane_refresh_ms;
        if (t->pane_shed_ms > base) base = t->pane_shed_ms;
        if (every < base) every = base;
    }
    c->every_ms = every; c->read_ms = now; c->stale = 0; c->pane = p;
    copy_name(c->task, sizeof c->task, p == TUI_PANE_DETAIL ? t->detail_task : "");
    c->sel_for = -1; c->have_sel = 0;
    t->h_pane_reads++; t->h_pane_us += c->cost_us; t->pane_cost_us = c->cost_us; t->pane_every_ms = every;
    t->pane_bytes = pc_bytes(c);
    if (nomem) t->h_nomem++;                                  /* never silent: shown on the pane too */
    mu_unlock(&t->mu);
}

/* Entry *k of the settings cache, read fresh from the registry into *out (one call) and
 * checked to be the same key: the registry may have changed since the pass (a task
 * unregistered shifts the indexes after its settings). If it has, the pane is read again
 * and the key looked up - *k follows it. 0: it is gone. */
static int pc_fetch(gptps_tui *t, int *k, gptps_setting_info *out, uint64_t now)
{
    tui_pcache *c = &t->pc;
    char key[GPTPS_SETTINGS_VALUE_MAX];
    int pass;
    if (*k < 0 || (size_t)*k >= c->n || !c->sets) return 0;
    copy_name(key, sizeof key, c->pool + c->sets[*k].key);
    for (pass = 0; pass < 2; ++pass) {
        if (pass) {
            size_t i;
            pc_read(t, now);
            for (i = 0; i < c->n && strcmp(c->pool + c->sets[i].key, key) != 0; ++i) { }
            if (i == c->n) return 0;
            *k = (int)i;
        }
        if (tui_setting_at(t->e, c->sets[*k].idx, out) && strcmp(skey(out), key) == 0) return 1;
    }
    return 0;
}

/* Make the cache match the pane shown at `now`: read it when it is for another pane or
 * task, stale, or due; free it on a pane that reads nothing. Then the selection: kept
 * inside the list, and the selected setting read (once per move) for its explanation. */
static void pane_prepare(gptps_tui *t, uint64_t now)
{
    tui_pcache *c = &t->pc;
    int p = t->pane;
    if (!pc_kind(p)) { if (c->pane >= 0 || c->cap || c->pool_cap) pc_free(t); return; }
    if (c->pane != p || c->stale || (p == TUI_PANE_DETAIL && strcmp(c->task, t->detail_task) != 0) ||
        now < c->read_ms || now - c->read_ms >= c->every_ms)
        pc_read(t, now);
    if (p == TUI_PANE_TASKS) {
        if (t->task_sel < 0) t->task_sel = 0;
        if (c->n && (size_t)t->task_sel >= c->n) t->task_sel = (int)c->n - 1;
    } else {
        int *sel = p == TUI_PANE_SETTINGS ? &t->sel : &t->detail_sel;
        if (*sel < 0) *sel = 0;
        if (c->n && (size_t)*sel >= c->n) *sel = (int)c->n - 1;
        if (c->sel_for != *sel || c->pane != p) {
            c->have_sel = 0;
            if (pc_fetch(t, sel, &c->sel, now)) {
                setting_kind(&c->sel, c->sel_kind, sizeof c->sel_kind);
                c->sel.choices = NULL;                         /* borrowed: formatted, then let go */
                c->have_sel = 1;
            }
            c->sel_for = *sel;
        }
    }
}

/* When the pane shown next changes with no key (0: never): its age, a second on, or its
 * next pass, whichever is first. */
static uint64_t pc_next(const tui_pcache *c, uint64_t now)
{
    uint64_t due, tick;
    if (c->pane < 0) return 0;
    due = c->read_ms + c->every_ms;
    tick = c->read_ms + ((now >= c->read_ms ? now - c->read_ms : 0) / 1000u + 1u) * 1000u;
    return tick < due ? tick : due;
}

/* The pane's last line before its legend: what the pass cost, how often it is made, and
 * how old what it shows is - "core counts: 116.2 ms, every 11.6 s, age 3 s". */
static size_t pc_footer(gptps_tui *t, char *buf, size_t cap, size_t pos, const char *what, const char *D, const char *X)
{
    const tui_pcache *c = &t->pc;
    uint64_t now = TUI_NOW_MS();
    char ln[256], cost[32];
    int n;
    if (c->cost_us < 1000u) snprintf(cost, sizeof cost, "%llu us", (unsigned long long)c->cost_us);
    else                    snprintf(cost, sizeof cost, "%.1f ms", (double)c->cost_us / 1000.0);
    /* what was not read comes first: the line is cut to the width, and a loss is the part
     * that must show (at the end, 80 columns cut it once a pass took a millisecond) */
    n = c->short_by ? snprintf(ln, sizeof ln, "  %s: %lu not read: no memory; ", what, (unsigned long)c->short_by)
                    : snprintf(ln, sizeof ln, "  %s: ", what);
    if (n > 0 && (size_t)n < sizeof ln)
        snprintf(ln + n, sizeof ln - (size_t)n, "%s, every %.1f s, age %llu s (R reads now)", cost,
                 (double)c->every_ms / 1000.0, (unsigned long long)(now > c->read_ms ? (now - c->read_ms) / 1000u : 0u));
    return appendf(buf, cap, pos, "%s%.*s%s\n", D, (int)u8_fit(ln, draw_width(t) - 1), ln, X);
}

/* Settings pane (no t->mu: reads run-thread-local pane state and the cache, which
 * pane_prepare filled with rmu held). The list scrolls to keep the selection in view;
 * the selected setting is explained under it. */
static size_t render_settings(gptps_tui *t, char *buf, size_t cap)
{
    const tui_pcache *c = &t->pc;
    size_t pos = 0, i, n = c->pane == TUI_PANE_SETTINGS ? c->n : 0;
    int color = t->cfg.color, kw = c->kw, rows = list_rows(t, settings_used(t)), first;
    const char *B = color ? "\x1b[1m" : "", *INV = color ? "\x1b[7m" : "", *D = color ? "\x1b[2m" : "", *X = color ? "\x1b[0m" : "";
    first = t->sel - rows / 2;
    if (first > (int)n - rows) first = (int)n - rows;
    if (first < 0) first = 0;
    pos = appendf(buf, cap, pos, "%s%s%s \xc2\xb7 settings%s  %s(%d of %lu)%s\n\n", B, brand(t), t->cfg.title, X,
                  D, n ? t->sel + 1 : 0, (unsigned long)n, X);
    for (i = (size_t)first; i < n && (int)(i - (size_t)first) < rows; ++i) {
        const tui_cset *st = &c->sets[i];
        pos = setting_row(buf, cap, pos, c->pool + st->val, st->hot, c->pool + st->key, kw, i == (size_t)t->sel, INV, X);
    }
    if (n && c->have_sel) pos = setting_details(t, buf, cap, pos, &c->sel, c->sel_kind);
    if (t->editing) pos = edit_line(t, buf, cap, pos, B, D, X);
    pos = status_line(t, buf, cap, pos, "", D, X);
    pos = pc_footer(t, buf, cap, pos, "settings read", D, X);
    pos = appendf(buf, cap, pos, "\n%skeys:%s %s move  %s edit  %s back  w save  R re-read  q quit\n", D, X, a_ud(t), a_rt(t), a_lt(t));
    return pos;                           /* unclamped: gptps_tui_render / tui_frame finish it */
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
    t->status_ms = TUI_NOW_MS();
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
    t->status_ms = TUI_NOW_MS();
}

/* set a transient action message ("toast") shown briefly on the dashboard */
static void toast(gptps_tui *t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t->status, sizeof t->status, fmt, ap);
    va_end(ap);
    t->status_ms = TUI_NOW_MS();
}


/* Help overlay: every key + what it does. Pure; reads only cfg. */
static size_t render_help(gptps_tui *t, char *buf, size_t cap)
{
    size_t pos = 0;
    int color = t->cfg.color;
    const char *B = color ? "\x1b[1m" : "", *D = color ? "\x1b[2m" : "", *X = color ? "\x1b[0m" : "";
    pos = appendf(buf, cap, pos, "%s%s%s \xc2\xb7 help%s\n\n", B, brand(t), t->cfg.title, X);
    pos = appendf(buf, cap, pos, "%sDashboard%s\n", B, X);
    pos = appendf(buf, cap, pos, "  %-9s submit its task\n", "<hotkey>");
    pos = appendf(buf, cap, pos, "  %-9s scroll the event log (older / newer)\n", "k / j");
    pos = appendf(buf, cap, pos, "  %-9s open the settings editor\n", "s");
    pos = appendf(buf, cap, pos, "  %-9s open the task manager\n", "t");
    pos = appendf(buf, cap, pos, "  %-9s open the dead-letter view\n", "l");
    pos = appendf(buf, cap, pos, "  %-9s KPI tier up: minimal gauge problems tasks normal latency full,\n", "m");
    pos = appendf(buf, cap, pos, "  %-9s then minimal again (never off)\n", "");
    pos = appendf(buf, cap, pos, "  %-9s undo a degrade (tui.adapt=degrade): the requested KPI tier again\n", "u");
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
    pos = appendf(buf, cap, pos, "  %-9s read the settings again now (else every %.1f s, or less often when\n", "R",
                  (double)t->cfg.pane_refresh_ms / 1000.0);
    pos = appendf(buf, cap, pos, "  %-9s a read is slow: the pane shows what it costs and how old it is)\n", "");
    pos = appendf(buf, cap, pos, "\n%sTask manager%s\n", B, X);
    pos = appendf(buf, cap, pos, "  %-9s inspect a task's settings\n", "Enter");
    pos = appendf(buf, cap, pos, "  %-9s pause/resume \xc2\xb7 clone \xc2\xb7 new \xc2\xb7 delete\n", "a/c/n/d");
    pos = appendf(buf, cap, pos, "  %-9s read the engine's counts again now (the same rule)\n", "R");
    pos = appendf(buf, cap, pos, "\n%sDead letters%s\n", B, X);
    pos = appendf(buf, cap, pos, "  %-9s load the engine's here (takes them), to see each one and why\n", "g");
    pos = appendf(buf, cap, pos, "  %-9s re-submit all \xc2\xb7 this one \xc2\xb7 discard this one \xc2\xb7 all\n", "r/e/x/D");
    pos = appendf(buf, cap, pos, "  %-9s %s(a re-submit the engine refuses stays here, with the reason)%s\n", "", D, X);
    pos = appendf(buf, cap, pos, "\n%spress any key to return%s\n", D, X);
    return pos;                           /* unclamped: gptps_tui_render / tui_frame finish it */
}

static const char *exec_str(int k)
{ return k == GPTPS_EXEC_OOP ? "oop" : k == GPTPS_EXEC_PROGRAM ? "program" : "inproc"; }


/* Tasks pane: the registry as an editable list, from the cache (pane_prepare read it;
 * no t->mu, no engine call: a frame between passes costs the engine nothing). */
static size_t render_tasks(gptps_tui *t, char *buf, size_t cap)
{
    const tui_pcache *c = &t->pc;
    size_t pos = 0, i, n = c->pane == TUI_PANE_TASKS ? c->n : 0;
    int color = t->cfg.color;
    const char *B = color ? "\x1b[1m" : "", *INV = color ? "\x1b[7m" : "", *D = color ? "\x1b[2m" : "",
               *R = color ? "\x1b[31m" : "", *G = color ? "\x1b[32m" : "", *Y = color ? "\x1b[33m" : "", *X = color ? "\x1b[0m" : "";
    char pn[sizeof t->prompt_name * 2];
    int room, first, more;
    /* Fitted to the terminal's height, around the selection, with "+N more" for the rest:
     * a list taller than the screen scrolled it on every paint. Rows: the title and its
     * blank line, the column header, a prompt (2), the status, the age line, the legend
     * (two rows: one was wider than 80 columns) and its blank line, and the frame's last
     * newline. */
    room = draw_rows(t) - 3 - (t->prompt_kind != TUI_PK_NONE ? 2 : 0) -
           (status_live(t->status, t->status_ms, TUI_NOW_MS()) ? 1 : 0) - 1 - 3 - 1;
    if (room < 2) room = 2;
    if ((int)n > room) --room;                       /* the "+N more" line */
    first = t->task_sel - room / 2;
    if (first > (int)n - room) first = (int)n - room;
    if (first < 0) first = 0;
    more = (int)n > room ? (int)n - room : 0;
    pos = appendf(buf, cap, pos, "%s%s%s \xc2\xb7 tasks%s  (%lu)\n\n", B, brand(t), t->cfg.title, X, (unsigned long)n);
    if (n == 0 && c->short_by)            /* not "none": they could not be read */
        pos = appendf(buf, cap, pos, "  %sno memory to list %lu task types%s\n", D, (unsigned long)c->short_by, X);
    else if (n == 0) pos = appendf(buf, cap, pos, "  %s(no task types registered)%s\n", D, X);
    else pos = appendf(buf, cap, pos, "%s  %-18s %-7s %4s %4s %4s %4s  %s%s\n", D, "name", "exec", "pri", "que", "run", "dl", "state", X);
    for (i = (size_t)first; i < n && (int)(i - (size_t)first) < room; ++i) {
        const tui_ctask *k = &c->tasks[i];
        const char *st, *sc;
        char nm[GPTPS_TASK_NAME_MAX * 2 + 2];
        st = k->removed ? "draining" : !k->enabled ? "paused" : "on";
        sc = k->removed ? Y : !k->enabled ? D : G;
        pos = appendf(buf, cap, pos, "%s%s%s %-7s %4d %4u %4u %4u  %s%s%s%s\n",
                      (i == (size_t)t->task_sel) ? INV : "", (i == (size_t)t->task_sel) ? "> " : "  ",
                      tui_cell(k->name, nm, sizeof nm, 18), exec_str(k->exec), (int)k->pri, k->queued, k->running, k->dead,
                      sc, st, color ? X : "", X);
    }
    if (more) pos = appendf(buf, cap, pos, "  %s+%d more (%s to scroll)%s\n", D, more, a_ud(t), X);
    if (t->prompt_kind == TUI_PK_CONFIRM_DELETE) {
        const tui_ctask *k = NULL;            /* resolve by the captured NAME, not a stale index */
        for (i = 0; i < n && !k; ++i) if (strcmp(c->tasks[i].name, t->prompt_name) == 0) k = &c->tasks[i];
        if (k)
            pos = appendf(buf, cap, pos, "\n%sDelete '%s'?%s  %u queued, %u in-flight.  %s[y]%s remove (pause+drain)  %s[x]%s cancel-force  %s[n]%s no\n",
                          R, tui_cl(t->prompt_name, pn, sizeof pn), X, k->queued, k->running, B, X, B, X, B, X);
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
    pos = pc_footer(t, buf, cap, pos, "core counts", D, X);
    pos = appendf(buf, cap, pos, "\n%skeys:%s %s move  %s inspect  %s back  a pause  c clone  n new\n"
                  "      d delete  R re-read  q quit\n", D, X, a_ud(t), a_rt(t), a_lt(t));
    return pos;                           /* unclamped: gptps_tui_render / tui_frame finish it */
}

/* Detail pane: one task's resolved settings (built-in policy + generic per-task),
 * inline-editable - the registry's tasks.<name>. keys, from the cache. */
static size_t render_detail(gptps_tui *t, char *buf, size_t cap)
{
    const tui_pcache *c = &t->pc;
    size_t pos = 0, i, n = c->pane == TUI_PANE_DETAIL ? c->n : 0, plen;
    int color = t->cfg.color, kw = c->kw, rows = list_rows(t, settings_used(t)), first;
    char prefix[160];   /* "tasks.<name>." - sized for the full detail_task buffer */
    char title[sizeof t->detail_task * 2];
    const char *B = color ? "\x1b[1m" : "", *INV = color ? "\x1b[7m" : "", *D = color ? "\x1b[2m" : "", *X = color ? "\x1b[0m" : "";
    plen = detail_prefix(t->detail_task, prefix, sizeof prefix);
    first = t->detail_sel - rows / 2;
    if (first > (int)n - rows) first = (int)n - rows;
    if (first < 0) first = 0;
    pos = appendf(buf, cap, pos, "%sGPTPS \xc2\xb7 task '%s'%s  %s(%lu settings)%s\n\n", B,
                  tui_cl(t->detail_task, title, sizeof title), X, D, (unsigned long)n, X);
    if (n == 0 && c->pane == TUI_PANE_DETAIL && c->short_by)
        pos = appendf(buf, cap, pos, "  %sno memory to list its %lu settings%s\n", D, (unsigned long)c->short_by, X);
    else if (n == 0) pos = appendf(buf, cap, pos, "  %s(task no longer present)%s\n", D, X);
    for (i = (size_t)first; i < n && (int)(i - (size_t)first) < rows; ++i) {
        const tui_cset *st = &c->sets[i];
        const char *k = c->pool + st->key;
        pos = setting_row(buf, cap, pos, c->pool + st->val, st->hot, strlen(k) >= plen ? k + plen : k, kw,
                          i == (size_t)t->detail_sel, INV, X);
    }
    if (n && c->have_sel) pos = setting_details(t, buf, cap, pos, &c->sel, c->sel_kind);
    if (t->editing) pos = edit_line(t, buf, cap, pos, B, D, X);
    pos = status_line(t, buf, cap, pos, "", D, X);
    pos = pc_footer(t, buf, cap, pos, "settings read", D, X);
    pos = appendf(buf, cap, pos, "\n%skeys:%s %s move  %s edit  %s back  w save  R re-read  q quit\n", D, X, a_ud(t), a_rt(t), a_lt(t));
    return pos;                           /* unclamped: gptps_tui_render / tui_frame finish it */
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
    int color = t->cfg.color, rows = list_rows(t, 14), first, W = draw_width(t);
    const char *B = color ? "\x1b[1m" : "", *D = color ? "\x1b[2m" : "", *R = color ? "\x1b[31m" : "",
               *INV = color ? "\x1b[7m" : "", *X = color ? "\x1b[0m" : "";
    pos = appendf(buf, cap, pos, "%s%s%s \xc2\xb7 dead letter%s\n\n", B, brand(t), t->cfg.title, X);
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
    return pos;                           /* unclamped: gptps_tui_render / tui_frame finish it */
}


/* ---- the header's clock, done/s and peak (1.11) -------------------------------------
 * None of them costs anything per event: each is advanced when a frame is made, from the
 * counters, and each says when it next changes with no event (the live loop's deadlines),
 * so an idle dashboard can stay still. On a host whose clock is not real (the freestanding
 * stub HAL), set cfg.clock = OFF. */

/* The uptime as the header shows it: MINUTES "up <1m", "up 42m", "up 3h07m", "up 2d05h";
 * SECONDS "up 9s", "up 4m09s", "up 3h07m09s". */
static void fmt_up(char *b, size_t cap, uint64_t ms, int clock)
{
    unsigned long long s = (unsigned long long)(ms / 1000u), m = s / 60u, h = m / 60u;
    if (clock == GPTPS_TUI_CLOCK_SECONDS) {
        if (s < 60u)      snprintf(b, cap, "up %llus", s);
        else if (m < 60u) snprintf(b, cap, "up %llum%02llus", m, s % 60u);
        else              snprintf(b, cap, "up %lluh%02llum%02llus", h, m % 60u, s % 60u);
    } else {
        if (m < 1u)       snprintf(b, cap, "up <1m");
        else if (h < 1u)  snprintf(b, cap, "up %llum", m);
        else if (h < 24u) snprintf(b, cap, "up %lluh%02llum", h, m % 60u);
        else              snprintf(b, cap, "up %llud%02lluh", h / 24u, h % 24u);
    }
}

/* When the uptime shown at `up` ms next changes, as an uptime (0: never - the clock is off). */
static uint64_t clock_next(uint64_t up, int clock)
{
    uint64_t unit = clock == GPTPS_TUI_CLOCK_SECONDS ? 1000u : clock == GPTPS_TUI_CLOCK_MINUTES ?
                    (up >= 24u * 3600000u ? 3600000u : 60000u) : 0u;
    return unit ? (up / unit + 1u) * unit : 0u;
}

/* The step between done/s snapshots for a window of win_s seconds. */
static uint64_t rate_step(int win_s)
{
    uint64_t w = (uint64_t)win_s * 1000u / TUI_RATE_N;
    return w < 1000u ? 1000u : w;
}

/* mu held: done/s over the last rate_window_s. A snapshot of the finished count is taken
 * when the newest is a step old; the rate is the count since the oldest snapshot inside the
 * window (with none inside: the newest one before it), over the time since it. *state: 0
 * when no earlier snapshot exists ("-": nothing to measure against yet - and *next is the
 * next ms, when there is), else 1. *next:
 * when it next changes with no event - while it is not 0, at the next snapshot (so it falls
 * to 0, frame by frame, as the last completions leave the window); 0 once it is 0. */
static double rate_take(gptps_tui *t, uint64_t now, uint64_t fin, int *state, uint64_t *next)
{
    uint64_t win = (uint64_t)t->cfg.rate_window_s * 1000u, step = rate_step(t->cfg.rate_window_s);
    const tui_rs *pick = NULL, *older = NULL, *newest;
    int i;
    /* a count that went back (a source's host reset its totals): measure from here */
    if (t->rate_n && fin < t->rate[(t->rate_head + TUI_RATE_N - 1) % TUI_RATE_N].fin) t->rate_n = 0;
    if (now < t->rate[(t->rate_head + TUI_RATE_N - 1) % TUI_RATE_N].ms && t->rate_n)
        now = t->rate[(t->rate_head + TUI_RATE_N - 1) % TUI_RATE_N].ms;   /* never back in time */
    if (!t->rate_n || now - t->rate[(t->rate_head + TUI_RATE_N - 1) % TUI_RATE_N].ms >= step) {
        t->rate[t->rate_head].ms = now; t->rate[t->rate_head].fin = fin;
        t->rate_head = (t->rate_head + 1) % TUI_RATE_N;
        if (t->rate_n < TUI_RATE_N) t->rate_n++;
    }
    newest = &t->rate[(t->rate_head + TUI_RATE_N - 1) % TUI_RATE_N];
    for (i = t->rate_n; i >= 1; --i) {                     /* oldest first */
        const tui_rs *r = &t->rate[(t->rate_head + TUI_RATE_N - i) % TUI_RATE_N];
        if (r->ms >= now) continue;                        /* taken now: no time to measure over */
        if (r->ms + win >= now) { pick = r; break; }
        older = r;                                         /* the newest before the window, so far */
    }
    if (!pick) pick = older;
    *next = 0;
    if (!pick) { *state = 0; *next = newest->ms + 1; return 0.0; }   /* measurable from the next ms */
    *state = 1;
    if (fin != pick->fin) *next = newest->ms + step > now ? newest->ms + step : now + step;
    return (double)(fin - pick->fin) * 1000.0 / (double)(now - pick->ms);
}

/* mu held: the in-flight peak over the window - two buckets, this one (`peak`, raised by
 * the event path with one compare) and the last (peak_prev), rotated here - so it covers
 * the last one to two windows (longer when no frame was made in between: never less).
 * `cur` is the gauge now. *next: when it next falls with no event (0: it equals cur). */
static uint64_t peak_take(gptps_tui *t, uint64_t now, uint64_t cur, uint64_t *next)
{
    uint64_t win = (uint64_t)t->cfg.rate_window_s * 1000u, shown;
    if (now >= t->peak_ms && now - t->peak_ms >= win) { t->peak_prev = t->peak; t->peak = cur; t->peak_ms = now; }
    if (cur > t->peak) t->peak = cur;
    shown = t->peak > t->peak_prev ? t->peak : t->peak_prev;
    *next = shown > cur ? t->peak_ms + win : 0;
    return shown;
}

/* mu held: peak_take over a source's in-flight gauge, in buckets of its own (the event path
 * raises `peak` with the dashboard's own gauge, which is not the source's) */
static uint64_t src_peak_take(gptps_tui *t, uint64_t now, uint64_t cur, uint64_t *next)
{
    uint64_t win = (uint64_t)t->cfg.rate_window_s * 1000u, shown;
    if (now >= t->sp_ms && now - t->sp_ms >= win) { t->sp_prev = t->sp_peak; t->sp_peak = cur; t->sp_ms = now; }
    if (cur > t->sp_peak) t->sp_peak = cur;
    shown = t->sp_peak > t->sp_prev ? t->sp_peak : t->sp_prev;
    *next = shown > cur ? t->sp_ms + win : 0;
    return shown;
}

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


/* rmu held: the view, with room for `R` rows and records (allocated here - never under mu,
 * never on the event path). 0 when there is no memory for the view itself; with too little
 * for the rows, it keeps its old room (the frame shows fewer, and "+N more"). */
static int view_ready(gptps_tui *t, int R)
{
    tui_view *v = t->view;
    int nomem = 0;
    if (!v) {
        v = (tui_view *)calloc(1, sizeof *v);
        if (!v) nomem = 1;
        else { v->rule_W = -1; t->view = v; }
    }
    if (v && v->rows_cap < R) {
        tui_vrow *r = (tui_vrow *)realloc(v->rows, (size_t)R * sizeof *r);
        if (r) { v->rows = r; v->rows_cap = R; } else nomem = 1;
    }
    if (v && v->recs_cap < R) {
        tui_event *r = (tui_event *)realloc(v->recs, (size_t)R * sizeof *r);
        if (r) { v->recs = r; v->recs_cap = R; } else nomem = 1;
    }
    if (v && t->src.read && v->srows_cap < v->rows_cap) {   /* a source's rows: as many as the frame's */
        gptps_tui_source_row *r = (gptps_tui_source_row *)realloc(v->srows, (size_t)v->rows_cap * sizeof *r);
        if (r) { v->srows = r; v->srows_cap = v->rows_cap; } else nomem = 1;
    }
    if (nomem) { mu_lock(&t->mu); t->h_nomem++; mu_unlock(&t->mu); }
    return v != NULL;
}

/* rmu held: copy out under mu what the dashboard pane shows at `now` (see "render").
 * Layout first - which TASKS rows and RECENT records the terminal has room for - so that
 * only those are copied. `loop`: the live loop's frame, which clears `dirty`. *warn_plost:
 * the PROBLEMS losses to WARN about, once mu is dropped (0: none). */
static void view_take(gptps_tui *t, tui_view *v, uint64_t now, int loop, uint64_t *warn_plost)
{
    int R = v->R, W = v->W, i, k, n;
    uint64_t nx_rate = 0, nx_peak = 0, nx_clock, nx_adapt = 0, nx_src = 0;
    mu_lock(&t->mu);
    *warn_plost = 0;
    /* This frame shows the journals: what left them since the last one was not shown. */
    {
        uint64_t lp = ring_unseen_lost(t->p_seq, t->p_seen, t->prob ? t->pcap : 0),
                 la = ring_unseen_lost(t->a_seq, t->a_seen, t->act ? t->acap : 0);
        t->p_lost += lp; t->a_lost += la; t->p_seen = t->p_seq; t->a_seen = t->a_seq;
        t->frame_unseen = lp + la;
        if (t->prob || t->act) { t->frame_unseen += t->drop_pend; t->drop_pend = 0; }  /* RECENT is shown */
        if (t->p_lost && !t->p_warned) { t->p_warned = 1; *warn_plost = t->p_lost; }
    }
    v->gen = t->gen;
    if (loop) t->dirty = 0;
    v->src_warn = 0;
    if (v->src) {                        /* the source's read, made just before (see src_read) */
        t->src_reads++;
        t->src_status = (int)v->src_st;
        if (v->src == 2) {               /* never silent: counted, shown, WARNed once a minute */
            t->src_errors++;
            if (!t->src_warned || now - t->src_warn_ms >= TUI_WARN_GAP_MS) {
                t->src_warned = 1; t->src_warn_ms = now; v->src_warn = 1;
            }
        } else t->src_rows = v->src_n;
        v->src_errors = t->src_errors;
        memcpy(v->src_name, t->src_name, sizeof v->src_name);
    }
    if ((v->src == 1) != t->src_kind) {  /* the totals changed hands: done/s and peak start again */
        t->src_kind = v->src == 1;
        t->rate_n = 0; t->rate_head = 0;
        t->sp_peak = t->sp_prev = 0; t->sp_ms = now;
    }
    v->tier = t->tier; v->kpi = t->kpi; v->mode = t->mode; v->refresh = t->cfg.refresh_ms;
    v->color = t->cfg.color; v->uni = t->cfg.unicode; v->clock = t->clock;
    v->up_ms = now >= t->start_ms ? now - t->start_ms : 0;
    v->toast_on = status_live(t->status, t->status_ms, now);
    if (v->toast_on) memcpy(v->status, t->status, sizeof v->status);
    /* ADAPT: the last window's PRESSURE line - taken away at the next window, so an idle
     * dashboard showing one has a frame due then (and none once it is gone) - and the
     * DEGRADED marker */
    memcpy(v->pline, t->ad_line, sizeof v->pline);
    v->nplines = 0;
    if (v->pline[0]) { const char *c = v->pline; v->nplines = 1; while ((c = strchr(c, '\n')) != NULL) { ++c; ++v->nplines; } }
    if (v->pline[0] && t->adapt != GPTPS_TUI_ADAPT_OFF) nx_adapt = t->ad_ms + (uint64_t)t->cfg.rate_window_s * 1000u;
    v->shed = t->shed; v->shed_why = t->shed_whys; v->kpi_req = t->kpi_req;
    if (!v->leg_ok || v->leg_gen != t->ui_gen || v->leg_W != W || v->leg_uni != v->uni) {
        v->legrows = legend_text(t, W, v->legend, sizeof v->legend);
        v->leg_ok = 1; v->leg_gen = t->ui_gen; v->leg_W = W; v->leg_uni = v->uni; v->leg_builds++;
    }
    if (v->src == 1) {                   /* the totals and gauges are the source's */
        const gptps_tui_source_totals *so = &v->stot;
        int g = so->gauges;
        v->rate = rate_take(t, now, so->finished, &v->rate_state, &nx_rate);
        v->q = so->queued; v->s = so->started; v->fin = so->finished; v->fail = so->failed;
        v->retr = so->retried; v->dead = so->dead; v->drop = so->dropped; v->cancel = so->cancelled;
        v->gline = g != GPTPS_TUI_GAUGES_SUM;
        v->exact = g == GPTPS_TUI_GAUGES_EXACT || g == GPTPS_TUI_GAUGES_LOWER_BOUND;
        v->lb = g == GPTPS_TUI_GAUGES_LOWER_BOUND;
        v->inflt = v->gline ? so->in_flight : 0;
        v->pend = so->pending;
        v->alive = v->gline ? so->pending + so->in_flight : so->pending;
        v->peak = v->gline ? src_peak_take(t, now, v->inflt, &nx_peak) : 0;
    } else {
        /* done/s needs only the totals; at OFF they stand still, so there is no rate to show */
        if (t->tier == TUI_T_OFF) { v->rate_state = 2; v->rate = 0.0; }
        else v->rate = rate_take(t, now, t->fin, &v->rate_state, &nx_rate);
        v->q = t->q; v->s = t->s; v->fin = t->fin; v->fail = t->fail; v->retr = t->retr; v->dead = t->dead;
        v->drop = t->drop; v->cancel = t->cancel;
        v->alive = t->q > t->term ? t->q - t->term : 0;
        /* EXACT from the handle table (LATENCY+); '>=' while that is a lower bound (see
         * trk_lb); '~' from the counters, which cannot be exact (see `inflight`) */
        v->gline = t->tier >= TUI_T_GAUGE;
        v->exact = t->tab != NULL; v->lb = t->trk_lb;
        v->inflt = v->exact ? t->g_run : t->inflight;
        v->pend = v->exact ? t->g_pend : (v->alive > v->inflt ? v->alive - v->inflt : 0);
        v->peak = t->tier >= TUI_T_GAUGE ? peak_take(t, now, v->inflt, &nx_peak) : 0;
    }
    v->miss = 0;
    if (t->tier >= TUI_T_PROBLEMS && t->show_recent && t->prob_want && !t->prob) v->miss |= 1;
    if (t->tier >= TUI_T_TASKS && t->show_tasks && !t->tasks)                    v->miss |= 2;
    if (t->tier >= TUI_T_ACTIVITY && t->show_recent && t->act_want && !t->act)   v->miss |= 4;
    if (t->tier >= TUI_T_LATENCY && !t->tab)                                     v->miss |= 8;
    if (t->tier >= TUI_T_FULL && t->tasks && !t->hist)                           v->miss |= 16;
    v->l_folded = t->h_folded; v->l_untracked = t->h_untracked; v->l_plost = t->p_lost;
    v->l_trunc = t->h_trunc; v->l_nomem = t->h_nomem; v->fold_more = t->fold_more;
    v->has_tasks = t->tasks != NULL; v->has_tab = t->tab != NULL;
    /* the columns past the 80-wide row (77 + key), each only when the width has room for it:
     * max ms (9 columns, LATENCY and up) from 88, then the percentiles (21, FULL) */
    v->mcols = t->tab != NULL && W >= 88;
    v->pcols = t->hist && W >= (v->mcols ? 109 : 100);
    v->nfold = t->nfold; v->max_tasks = t->max_tasks; v->lat_cap = t->lat_cap;
    if (v->src == 1) {                   /* the TASKS rows are the source's: none of the own table's notes */
        v->has_tasks = t->show_tasks != 0; v->has_tab = 0; v->nfold = 0;
        v->mcols = v->src_lat && W >= 88; v->pcols = 0;
    }
    v->lat_untracked = t->lat_untracked; v->lat_unmatched = t->lat_unmatched;
    v->has_rec = t->prob || t->act; v->act_on = t->act != NULL;
    v->frame_unseen = t->frame_unseen;
    v->nrows = v->more = v->nrec = 0;
    {   /* the layout: every line of the frame is one terminal row (each is fitted to W), and
         * the frame's last newline must not scroll the screen */
        int budget = R - 1 - (v->color ? 2 : 1) - 3 - (v->miss ? 1 : 0) - v->nplines - (v->shed ? 1 : 0)
                   - (v->src == 2 ? 1 : 0)
                   - ((v->l_folded || v->l_untracked || v->l_plost || v->l_trunc || v->l_nomem) ? 1 : 0)
                   - (v->toast_on ? 2 : 0) - 1 - v->legrows;
        int want_rows = 0, other = 0, want_rec = 0, np = 0, na = 0, lines = 0;
        int srcrows = v->src == 1 && v->has_tasks;
        if (srcrows) {
            want_rows = v->src_n > ((size_t)1 << 30) ? 1 << 30 : (int)v->src_n;
            budget -= 3;
        } else if (t->tasks) {
            tui_task *o = &t->tasks[0];
            uint64_t oterm = o->finished + o->dead + o->dropped;
            other = t->nfold || o->started || o->failed || o->retried || oterm;   /* '(other)' once it counted something */
            want_rows = t->ntasks - 1 + other;
            budget -= 3 + (t->nfold ? 1 : 0) + ((t->tab && (t->lat_untracked || t->lat_unmatched)) ? 1 : 0);
        }
        if (v->has_rec) {
            np = (int)ring_held(t->p_seq, t->p_base, t->prob ? t->pcap : 0);
            na = (int)ring_held(t->a_seq, t->a_base, t->act ? t->acap : 0);
            lines = (t->prob && t->pcap > (t->act ? t->acap : 0)) ? t->pcap : t->acap;
            want_rec = np + na < lines ? np + na : lines;
            budget -= 2;
        }
        if (srcrows || t->tasks) {   /* TASKS first, leaving RECENT a few lines; "+N more" for the rest */
            int room = budget - (want_rec < 3 ? want_rec : 3), show;
            if (want_rows <= room) show = want_rows;
            else show = room - 1 > 0 ? room - 1 : 0;          /* one line says how many more */
            if (show > v->rows_cap) show = v->rows_cap;      /* no memory for more: still counted */
            if (srcrows && show > v->srows_cap) show = v->srows_cap;
            v->more = want_rows - show;
            budget -= show + (v->more ? 1 : 0);
            for (i = 0, n = 0; srcrows && n < show; ++i) {   /* the source's rows, in its order */
                const gptps_tui_source_row *sr = &v->srows[i];
                const tui_ui *u = NULL;
                tui_vrow *o = &v->rows[n];
                for (k = 0; k < t->nui; ++k) if (strcmp(t->ui[k].name, sr->name) == 0) { u = &t->ui[k]; break; }
                copy_u8(o->label, sizeof o->label, u ? u->label : sr->name);
                o->hotkey = u ? u->hotkey : 0;
                o->started = sr->started; o->finished = sr->finished; o->failed = sr->failed;
                o->dead = sr->dead; o->dropped = sr->dropped;
                if (sr->flags & GPTPS_TUI_SRC_ROW_LATENCY) { o->lat_sum = sr->lat_sum_ms; o->lat_max = sr->lat_max_ms; o->lat_n = sr->lat_n; }
                else o->lat_sum = o->lat_max = o->lat_n = 0;
                o->mem_peak = 0; o->has_mem = 0;
                ++n;
            }
            for (i = 1, n = srcrows ? n : 0; !srcrows && i <= t->ntasks && n < show; ++i) {
                tui_task *src = &t->tasks[i < t->ntasks ? i : 0];
                const tui_ui *u = (src->ui >= 0 && src->ui < t->nui) ? &t->ui[src->ui] : NULL;
                tui_vrow *o = &v->rows[n];
                if (i == t->ntasks && !other) break;
                copy_u8(o->label, sizeof o->label, u ? u->label : src->name);
                o->hotkey = u ? u->hotkey : 0;
                o->started = src->started; o->finished = src->finished; o->failed = src->failed;
                o->dead = src->dead; o->dropped = src->dropped; o->lat_sum = src->lat_sum_ms; o->lat_max = src->lat_max_ms; o->lat_n = src->lat_n;
                o->mem_peak = src->mem_peak; o->has_mem = src->mem_method[0] != 0;
                if (v->pcols) {
                    const uint64_t *hb = &t->hist[(size_t)(src - t->tasks) * TUI_HIST_B];
                    hist_pct(hb, 50, o->pq[0], sizeof o->pq[0]); hist_pct(hb, 95, o->pq[1], sizeof o->pq[1]);
                    hist_pct(hb, 99, o->pq[2], sizeof o->pq[2]);
                }
                ++n;
            }
            v->nrows = n;
        }
        if (v->has_rec) {
            /* The two journals merged by time, newest last; `scroll` lines back. Shown: at
             * most the larger journal's size, and what the screen has room for. */
            int shown = want_rec, total = np + na, maxoff, ip = 0, ia = 0, m;
            if (shown > budget) shown = budget;
            if (shown < 1) shown = 1;
            if (shown > v->recs_cap) shown = v->recs_cap;
            maxoff = total - shown; if (maxoff < 0) maxoff = 0;
            if (t->scroll > maxoff) t->scroll = maxoff;   /* clamp (keys can over-scroll) */
            if (t->scroll < 0) t->scroll = 0;
            v->scroll = t->scroll;
            /* newest first: the k-th newest of a ring is (head - 1 - k) mod cap; a tie in time
             * puts the problem after the activity (a STARTED, then its FAILED, in one ms).
             * Stored newest first; the format prints them oldest at the top. */
            for (k = 0, m = 0; k < t->scroll + shown && (ip < np || ia < na); ++k) {
                int pi = ip < np ? (t->phead - 1 - ip + t->pcap * 2) % t->pcap : -1,
                    ai = ia < na ? (t->ahead - 1 - ia + t->acap * 2) % t->acap : -1;
                int isp = pi >= 0 && (ai < 0 || t->prob[pi].ts >= t->act[ai].ts);
                if (isp) ++ip; else ++ia;
                if (k < t->scroll || m >= shown) continue;
                if (isp) v->recs[m] = t->prob[pi];
                else {
                    const tui_act *ra = &t->act[ai];
                    tui_event *o = &v->recs[m];
                    o->ts = ra->ts; o->kind = ra->kind; o->handle = ra->handle; o->status = GPTPS_OK;
                    copy_u8(o->name, sizeof o->name,
                            (ra->row >= 0 && t->tasks && ra->row < t->ntasks) ? t->tasks[ra->row].name
                            : t->act_nm ? t->act_nm[ai] : "?");
                    o->why[0] = 0; o->tail_len = 0; o->tail_cut = 0;
                }
                ++m;
            }
            v->nrec = m;
        }
    }
    v->rec_t0 = t->start_ms;
    nx_clock = v->clock != GPTPS_TUI_CLOCK_OFF ? t->start_ms + clock_next(v->up_ms, v->clock) : 0;
    /* a source changes with no event the dashboard sees: the live loop reads it every refresh_ms */
    if (v->src) nx_src = now + (t->cfg.refresh_ms ? t->cfg.refresh_ms : 250u);
    mu_unlock(&t->mu);
    v->next_change = 0;
    if (nx_src && (!v->next_change || nx_src < v->next_change)) v->next_change = nx_src;
    if (nx_rate && (!v->next_change || nx_rate < v->next_change)) v->next_change = nx_rate;
    if (nx_peak && (!v->next_change || nx_peak < v->next_change)) v->next_change = nx_peak;
    if (nx_clock && (!v->next_change || nx_clock < v->next_change)) v->next_change = nx_clock;
    if (nx_adapt && (!v->next_change || nx_adapt < v->next_change)) v->next_change = nx_adapt;
}

static void tui_logf(gptps_log_level lvl, const char *fmt, ...);

/* ---- adapt (gptps_tui.h, ADAPT) ------------------------------------------------------
 * Nothing here runs on the event path. Once a rate window, when a frame is made or health
 * is read, tui_adapt compares the counters the dashboard keeps anyway with what they were
 * when the window began: the window's conditions are what changed in it. REPORT shows them
 * (the PRESSURE line, health) and WARNs - each condition at most once a minute; DEGRADE
 * also sheds one step once a sheddable condition has lasted TUI_SHED_AFTER windows in a row
 * (shed_next: the fixed order, the PROBLEMS floor), through tui_reconfig - which only FREES
 * when it sheds, so a step allocates nothing. Sticky: only the operator clears t->shed
 * (tui_reconfig's `unshed`). */
static gptps_status tui_reconfig(gptps_tui *t, int lvl, int win, int plines, int alines, int unshed,
                                 char *what, size_t cap);

/* The DEGRADE steps in shedding order (gptps_tui.h, ADAPT): each one's bit, its name as the
 * DEGRADED line shows it, and the conditions it relieves - from what each step takes out:
 *   pane         the engine-reading panes read half as often: their engine-lock duty, the
 *                PANE condition (the interval is stretched past its floor no longer)
 *   activity     the journal's write per lifecycle event (COST: 3 ns of the estimate), its
 *                records (ACTIVITY_LOST), the RECENT rows a frame merges and prints (RENDER,
 *                TRUNCATED: a frame is rows of at most W columns), its memory (NOMEM)
 *   percentiles  the histograms' memory (NOMEM) and the per-row p50/p95/p99 a frame computes
 *                (RENDER); NOT the event cost - FULL and LATENCY estimate the same 38 ns
 *   latency      the handle table: the QUEUEDs it had no room for (UNTRACKED), its per-event
 *                lookup (COST: 38 -> 24 ns), its memory (NOMEM). A frame reads two sums a row
 *                from it, so not RENDER; and no row goes, so not TRUNCATED
 *   tasks        the per-type rows: their bump an event (COST), their rows on a frame (RENDER,
 *                TRUNCATED), their memory (NOMEM)
 * Not relieved by any: PROBLEMS_LOST (the PROBLEMS journal is the floor and no step changes
 * how many failures arrive) and FOLDED (folding into '(other)' is the cheap fallback already;
 * only -tasks ends it, by dropping every row to save merging a few) - report-only. */
#define TUI_SHED_N 5
static const struct { unsigned bit; const char *name; uint64_t relieves; } TUI_SHED[TUI_SHED_N] = {
    { GPTPS_TUI_SHED_PANE,        "pane",        GPTPS_TUI_P_PANE },
    { GPTPS_TUI_SHED_ACTIVITY,    "activity",    GPTPS_TUI_P_ACTIVITY_LOST | GPTPS_TUI_P_COST | GPTPS_TUI_P_RENDER |
                                                 GPTPS_TUI_P_TRUNCATED | GPTPS_TUI_P_NOMEM },
    { GPTPS_TUI_SHED_PERCENTILES, "percentiles", GPTPS_TUI_P_RENDER | GPTPS_TUI_P_NOMEM },
    { GPTPS_TUI_SHED_LATENCY,     "latency",     GPTPS_TUI_P_UNTRACKED | GPTPS_TUI_P_COST | GPTPS_TUI_P_NOMEM },
    { GPTPS_TUI_SHED_TASKS,       "tasks",       GPTPS_TUI_P_COST | GPTPS_TUI_P_RENDER | GPTPS_TUI_P_TRUNCATED |
                                                 GPTPS_TUI_P_NOMEM }
};
/* the conditions, named as the DEGRADED line's "(pressure: ...)" and the WARNs show them */
static const char *const TUI_P_NAME[TUI_P_N] = { "problems lost", "activity lost", "untracked", "folded",
                                                 "truncated", "no memory", "render", "pane", "cost" };
static const char *adapt_str(int a)
{ return a == GPTPS_TUI_ADAPT_OFF ? "off" : a == GPTPS_TUI_ADAPT_DEGRADE ? "degrade" : "report"; }

/* ADAPT's estimated ns an event at `tier`, the ACTIVITY journal on or off */
static unsigned cost_ns(int tier, int act_on)
{
    unsigned u = TUI_COST_NS[tier];
    return tier >= TUI_T_ACTIVITY && !act_on ? u - TUI_COST_ACT : u;
}

/* mu held: whether the ACTIVITY journal is wanted (shed_cap's `act`) */
static int act_wanted(const gptps_tui *t) { return t->show_recent && t->act_want > 0; }
/* mu held: the tier that runs, the request minus what is shed */
static int shed_tier(const gptps_tui *t) { return shed_cap(kpi_tier(t->kpi_req), t->shed, act_wanted(t)); }
/* mu held: the pane interval GPTPS_TUI_SHED_PANE would set next - twice what runs, up to
 * TUI_PANE_SHED_MAX - or 0 when it would change nothing (at the cap) */
static uint64_t pane_shed_next(const gptps_tui *t)
{
    uint64_t cur = t->pane_every_ms, base = t->cfg.pane_refresh_ms;
    if (t->pane_shed_ms > base) base = t->pane_shed_ms;
    if (cur < base) cur = base;
    if (cur >= TUI_PANE_SHED_MAX) return 0;
    return cur * 2u < TUI_PANE_SHED_MAX ? cur * 2u : TUI_PANE_SHED_MAX;
}
/* mu held: whether step `bit` would change what runs */
static int shed_avail(const gptps_tui *t, unsigned bit)
{
    int eff = shed_tier(t);
    switch (bit) {
        case GPTPS_TUI_SHED_PANE:        return pane_shed_next(t) != 0;
        case GPTPS_TUI_SHED_ACTIVITY:    return !(t->shed & bit) && eff >= TUI_T_ACTIVITY && act_wanted(t);
        case GPTPS_TUI_SHED_PERCENTILES: return !(t->shed & bit) && eff >= TUI_T_FULL;
        case GPTPS_TUI_SHED_LATENCY:     return !(t->shed & bit) && eff >= TUI_T_LATENCY;
        case GPTPS_TUI_SHED_TASKS:       return !(t->shed & bit) && eff >= TUI_T_TASKS;
        default:                         return 0;
    }
}

/* mu held: the next DEGRADE step for the conditions `why` (GPTPS_TUI_P_*): in the fixed order,
 * the first that would change what runs AND relieves one of them (1.12 took the next in the
 * order whatever it relieved). Returned as the bits it sets: a tier step also sets the steps
 * above it in the ladder that it takes out with it and that still ran (-latency takes the
 * percentiles, -tasks the journal, the percentiles and the table), so the DEGRADED line
 * names all that went. 0: none - the floor, or nothing left relieves `why`. *chosen: the
 * step itself (its `relieves` is the window's shed_why). */
static unsigned shed_next(const gptps_tui *t, uint64_t why, int *chosen)
{
    int k, j;
    for (k = 0; k < TUI_SHED_N; ++k)
        if ((TUI_SHED[k].relieves & why) && shed_avail(t, TUI_SHED[k].bit)) {
            unsigned bits = TUI_SHED[k].bit;
            if (bits != GPTPS_TUI_SHED_PANE)
                for (j = 1; j < k; ++j)                      /* the tier steps before it */
                    if (shed_avail(t, TUI_SHED[j].bit) &&
                        (TUI_SHED[k].bit == GPTPS_TUI_SHED_TASKS || TUI_SHED[j].bit == GPTPS_TUI_SHED_PERCENTILES))
                        bits |= TUI_SHED[j].bit;
            *chosen = k;
            return bits;
        }
    *chosen = -1;
    return 0;
}

/* "-pane -activity -latency": the steps in `shed`, in order */
static void shed_text(unsigned shed, char *out, size_t cap)
{
    size_t p = 0;
    int k;
    out[0] = 0;
    for (k = 0; k < TUI_SHED_N; ++k)
        if ((shed & TUI_SHED[k].bit) && p < cap)
            p += (size_t)snprintf(out + p, cap - p, "%s-%s", p ? " " : "", TUI_SHED[k].name);
}
/* "cost, render": the conditions in `bits` */
static void pnames(uint64_t bits, char *out, size_t cap)
{
    size_t p = 0;
    int k;
    out[0] = 0;
    for (k = 0; k < TUI_P_N; ++k)
        if ((bits & (1u << k)) && p < cap)
            p += (size_t)snprintf(out + p, cap - p, "%s%s", p ? ", " : "", TUI_P_NAME[k]);
}

/* What one window found, copied out under mu to be formatted and logged without it. */
typedef struct {
    uint64_t flags, d[TUI_P_N], rate, ppm, pane_every;
    unsigned unit;
    uint32_t refresh;
    int      tier, fit, budget, lat_cap, max_tasks, prob_want, pane_cfg, adapt, kpi_req;
} tui_adsnap;

/* Condition k of a window in one phrase: what, then the setting to change, then detail -
 * a PRESSURE line is fitted to the width, so what is cut is the detail. */
static void adapt_phrase(const tui_adsnap *a, int k, char *out, size_t cap)
{
    char c[16];
    switch (k) {
        case 0: snprintf(out, cap, "PROBLEMS lost %s unseen: raise tui.recent (%d) - report-only: PROBLEMS is "
                         "the floor, never shed", fmt_count(c, sizeof c, a->d[0]), a->prob_want); break;
        case 1: snprintf(out, cap, "ACTIVITY lost %s unseen: tui.activity=0", fmt_count(c, sizeof c, a->d[1])); break;
        case 2: snprintf(out, cap, "%s items untracked: raise tui.latency_window (%d)", fmt_count(c, sizeof c, a->d[2]),
                         a->lat_cap); break;
        case 3: snprintf(out, cap, "%s types folded into (other): raise cfg.max_tasks (%d) - report-only: "
                         "folding is the cheap fallback", fmt_count(c, sizeof c, a->d[3]), a->max_tasks); break;
        case 4: snprintf(out, cap, "%s frames truncated: lower tui.recent / tui.activity", fmt_count(c, sizeof c, a->d[4]));
                break;
        case 5: snprintf(out, cap, "%s allocations failed: a lower tui.kpi", fmt_count(c, sizeof c, a->d[5])); break;
        /* tenths as integers, not %.1f: a double's %f has no bound the compiler can see */
        case 6: snprintf(out, cap, "%llu.%u ms frames: raise tui.refresh_ms (%u; over 1/%u of it)",
                         (unsigned long long)(a->d[6] / 1000u), (unsigned)(a->d[6] / 100u % 10u),
                         (unsigned)a->refresh, TUI_RENDER_DIV); break;
        case 7: snprintf(out, cap, "%llu.%u ms pane reads: raise cfg.pane_refresh_ms (%d) or leave the pane (reads "
                         "every %llu.%u s)", (unsigned long long)(a->d[7] / 1000u), (unsigned)(a->d[7] / 100u % 10u),
                         a->pane_cfg, (unsigned long long)(a->pane_every / 1000u), (unsigned)(a->pane_every / 100u % 10u));
                break;
        default: snprintf(out, cap, "est. cost %llu.%u%% of a core > tui.cost_budget_pct %d: tui.kpi=%s (%s ev/s x %u ns "
                          "at %s)", (unsigned long long)(a->ppm / 10000u), (unsigned)(a->ppm / 1000u % 10u), a->budget,
                          a->fit >= 0 ? kpi_str(TUI_LADDER[a->fit]) : "off", fmt_count(c, sizeof c, a->rate), a->unit,
                          kpi_str(TUI_LADDER[a->tier]));
                 break;
    }
}

/* mu held: a window begins at `now` - the counters as they are, its slowest frame none yet */
static uint64_t adapt_events(const gptps_tui *t) { return t->q + t->s + t->fin + t->fail + t->retr + t->dead + t->drop; }
static uint64_t adapt_plost(const gptps_tui *t) { return t->p_lost + ring_unseen_lost(t->p_seq, t->p_seen, t->prob ? t->pcap : 0); }
static uint64_t adapt_alost(const gptps_tui *t) { return t->a_lost + ring_unseen_lost(t->a_seq, t->a_seen, t->act ? t->acap : 0); }
static void adapt_base(gptps_tui *t, uint64_t now)
{
    t->ad_ms = now; t->ad_ev = adapt_events(t); t->ad_plost = adapt_plost(t); t->ad_alost = adapt_alost(t);
    t->ad_untr = t->h_untracked; t->ad_fold = t->h_folded; t->ad_trunc = t->h_trunc; t->ad_nomem = t->h_nomem;
    t->ad_preads = t->h_pane_reads; t->ad_rmax_us = 0; t->ad_trd = t->h_trunc_dash; t->ad_gen++;
}

/* Evaluate the window, if one has passed (see above). `now`: TUI_NOW_MS. Never under a lock. */
static void tui_adapt(gptps_tui *t, uint64_t now)
{
    tui_adsnap a;
    unsigned step = 0;
    uint64_t win, el, ev, pl, al, wmask = 0, why = 0, gen, acting, pane_to = 0;
    int k, floor_warn = 0, changed = 0, eff = 0, chosen = -1;
    char line[sizeof t->ad_line], ph[256];
    size_t lp;
    mu_lock(&t->mu);
    win = (uint64_t)t->cfg.rate_window_s * 1000u;
    if (t->adapt == GPTPS_TUI_ADAPT_OFF || now < t->ad_ms || now - t->ad_ms < win) { mu_unlock(&t->mu); return; }
    memset(&a, 0, sizeof a);
    el = now - t->ad_ms;
    ev = adapt_events(t); pl = adapt_plost(t); al = adapt_alost(t);
    a.d[0] = pl - t->ad_plost;
    a.d[1] = al - t->ad_alost;
    a.d[2] = t->h_untracked - t->ad_untr;
    a.d[3] = t->h_folded - t->ad_fold;
    a.d[4] = t->h_trunc - t->ad_trunc;
    a.d[5] = t->h_nomem - t->ad_nomem;
    a.d[6] = t->ad_rmax_us;
    {   /* PANE: a pass in the window, and its interval stretched past its floor - pane_refresh_ms,
         * or what GPTPS_TUI_SHED_PANE raised it to (so a step that covers the pass relieves it) */
        uint64_t base = t->cfg.pane_refresh_ms;
        if (t->pane_shed_ms > base) base = t->pane_shed_ms;
        a.d[7] = t->h_pane_reads != t->ad_preads && t->pane_every_ms > base ? t->pane_cost_us : 0;
    }
    a.rate = (ev - t->ad_ev) * 1000u / el;
    a.unit = t->tier == TUI_T_OFF ? 0u : cost_ns(t->tier, t->act != NULL);   /* OFF: the totals stand still */
    a.ppm = a.rate * a.unit / 1000u;                 /* ns a second / 1e9 x 1e6 */
    a.budget = t->cost_pct; a.tier = t->tier; a.refresh = t->cfg.refresh_ms; a.lat_cap = t->lat_cap;
    a.max_tasks = t->max_tasks; a.prob_want = t->prob_want; a.pane_cfg = t->cfg.pane_refresh_ms;
    a.pane_every = t->pane_every_ms; a.adapt = t->adapt; a.kpi_req = t->kpi_req;
    for (a.fit = t->tier; a.fit >= TUI_T_COUNT; --a.fit)            /* the highest tier the budget fits */
        if (a.rate * cost_ns(a.fit, a.fit < t->tier || t->act != NULL) / 1000u <= (uint64_t)a.budget * 10000u) break;
    if (a.fit < TUI_T_COUNT) a.fit = -1;
    for (k = 0; k < 6; ++k) if (a.d[k] && k != 1) a.flags |= 1u << k;
    if (a.d[1] * 1000u / el >= TUI_ACT_FLOOD) a.flags |= GPTPS_TUI_P_ACTIVITY_LOST;
    if (a.d[6] * TUI_RENDER_DIV > (uint64_t)a.refresh * 1000u) a.flags |= GPTPS_TUI_P_RENDER;
    if (a.d[7]) a.flags |= GPTPS_TUI_P_PANE;
    if (a.ppm > (uint64_t)a.budget * 10000u) a.flags |= GPTPS_TUI_P_COST;
    t->pressure = a.flags; t->pr_seen |= a.flags; t->ad_windows++;
    t->ad_rate = a.rate; t->ad_ppm = a.ppm; t->ad_rmax_last = a.d[6];
    /* What counts toward DEGRADE (1.13: every condition a step relieves - TUI_SHED). Frames cut
     * on another pane, or before the dashboard's rows begin (a host's small render buffer, a
     * title too long), are not ones fewer rows can make fit: TRUNCATED acts for h_trunc_dash
     * only (tui_finish). */
    acting = a.flags & GPTPS_TUI_P_SHEDDABLE;
    if (t->h_trunc_dash == t->ad_trd) acting &= ~(uint64_t)GPTPS_TUI_P_TRUNCATED;
    if (acting) t->p_row++; else t->p_row = 0;
    if (t->adapt == GPTPS_TUI_ADAPT_DEGRADE && t->p_row >= TUI_SHED_AFTER) {
        t->p_row = 0;                                /* the next step needs as many windows again */
        why = acting;
        if ((step = shed_next(t, acting, &chosen)) != 0) {
            why = acting & TUI_SHED[chosen].relieves;   /* what this step answers */
            if (step & GPTPS_TUI_SHED_PANE) { pane_to = pane_shed_next(t); t->pane_shed_ms = pane_to; }
            t->shed |= step; t->shed_why = why; t->shed_whys |= why; t->ad_steps++; t->ad_warns++;
            eff = shed_tier(t);
        } else if (!t->ad_floor_ms || now - t->ad_floor_ms >= TUI_WARN_GAP_MS) {
            t->ad_floor_ms = now ? now : 1; floor_warn = 1; t->ad_warns++;
            if (why & GPTPS_TUI_P_PANE) {            /* the interval in effect, said as it is */
                pane_to = t->cfg.pane_refresh_ms;
                if (t->pane_shed_ms > pane_to) pane_to = t->pane_shed_ms;
                if (t->pane_every_ms > pane_to) pane_to = t->pane_every_ms;
            }
        }
    }
    for (k = 0; k < TUI_P_N; ++k)                   /* a WARN per condition, at most one a minute */
        if ((a.flags & (1u << k)) && (!t->ad_warn_ms[k] || now - t->ad_warn_ms[k] >= TUI_WARN_GAP_MS)) {
            t->ad_warn_ms[k] = now ? now : 1; wmask |= 1u << k; t->ad_warns++;
        }
    adapt_base(t, now);
    gen = t->ad_gen;
    mu_unlock(&t->mu);
    TUI_ADAPT_MID(t);

    line[0] = 0; lp = 0;                             /* the PRESSURE lines: one a condition */
    for (k = 0; k < TUI_P_N; ++k)
        if (a.flags & (1u << k)) {
            static const char P[] = "PRESSURE: ";
            size_t n;
            adapt_phrase(&a, k, ph, sizeof ph);
            n = strlen(ph);
            if (lp + (lp ? 1u : 0u) + sizeof P - 1u + n + 1u > sizeof line) break;   /* whole lines only */
            if (lp) line[lp++] = '\n';
            memcpy(line + lp, P, sizeof P - 1u); lp += sizeof P - 1u;
            memcpy(line + lp, ph, n + 1u); lp += n;
        }
    mu_lock(&t->mu);
    /* not if, meanwhile, gptps_tui_set_adapt (OFF: it cleared the line) or a newer window ran */
    if (t->ad_gen == gen && t->adapt != GPTPS_TUI_ADAPT_OFF && strcmp(t->ad_line, line) != 0) {
        memcpy(t->ad_line, line, sizeof line); tui_touch(t); changed = 1;
    }
    mu_unlock(&t->mu);
    if (changed) tui_kick(t);
    for (k = 0; k < TUI_P_N; ++k)                    /* never silent; logged with no lock held */
        if (wmask & (1u << k)) {
            adapt_phrase(&a, k, ph, sizeof ph);
            tui_logf(GPTPS_LOG_WARN, "gptps_tui: pressure (%s, tui.adapt=%s): %s - on the dashboard's PRESSURE line "
                     "and in gptps_tui_health.pressure; this WARN at most once a minute", TUI_P_NAME[k],
                     adapt_str(a.adapt), ph);
        }
    if (step || floor_warn) {
        char wn[160], sn[64];
        pnames(why, wn, sizeof wn);
        if (step) {
            char pn[128];
            shed_text(step, sn, sizeof sn);
            pn[0] = 0;
            if (step & GPTPS_TUI_SHED_PANE)
                snprintf(pn, sizeof pn, " (the tasks / settings / detail panes read the engine at most every %llu.%u s)",
                         (unsigned long long)(pane_to / 1000u), (unsigned)(pane_to / 100u % 10u));
            tui_logf(GPTPS_LOG_WARN, "gptps_tui: tui.adapt=degrade: pressure (%s) for %u windows in a row; shedding "
                     "%s%s - kpi %s requested, %s runs. Sticky: nothing comes back by itself; 'u', "
                     "gptps_tui_set_kpi or a tui.kpi / tui.adapt write restores the request ('m' clears it too, "
                     "stepping up from what runs)", wn, TUI_SHED_AFTER, sn, pn,
                     kpi_str(a.kpi_req), kpi_str(TUI_LADDER[eff]));
        } else {
            char pn[160];
            pn[0] = 0;
            if (why & GPTPS_TUI_P_PANE)                  /* only when the panes are in it */
                snprintf(pn, sizeof pn, "; the tasks / settings / detail panes read the engine every %llu.%u s, "
                         "and shedding never stretches them past %u s", (unsigned long long)(pane_to / 1000u),
                         (unsigned)(pane_to / 100u % 10u), TUI_PANE_SHED_MAX / 1000u);
            tui_logf(GPTPS_LOG_WARN, "gptps_tui: tui.adapt=degrade: pressure (%s) for %u windows in a row, but nothing "
                     "is left to shed that relieves it at kpi %s - the totals, the gauges and the PROBLEMS journal "
                     "never are%s", wn, TUI_SHED_AFTER, kpi_str(TUI_LADDER[a.tier]), pn);
        }
    }
    if (step & ~(unsigned)GPTPS_TUI_SHED_PANE) {      /* a tier step; the request is kept */
        /* It frees what the step sheds; a part the tier still wants and that failed to
         * allocate before is tried again here (tui_reconfig builds what is missing) - and
         * may fail again, which is said, as every caller of tui_reconfig does. */
        char what[400];
        (void)tui_reconfig(t, -1, 0, -1, -1, 0, what, sizeof what);
        if (what[0]) tui_logf(GPTPS_LOG_WARN, "gptps_tui: tui.adapt=degrade: no memory for %s; the dashboard runs "
                              "without it", what);
    }
}

/* A frame was made in `us` microseconds: the window's slowest (ADAPT's RENDER). */
static void adapt_frame_us(gptps_tui *t, uint64_t us)
{
    mu_lock(&t->mu);
    if (us > t->ad_rmax_us) t->ad_rmax_us = us;
    mu_unlock(&t->mu);
}

/* full-width inverse title bar (color mode): left title, right-aligned stats */
static size_t titlebar(char *buf, size_t cap, size_t pos, const gptps_tui *t, int W, const char *stats)
{
    char line[256];
    int len, sl = (int)strlen(stats);
    if (W > (int)sizeof line - 1) W = (int)sizeof line - 1;
    len = snprintf(line, sizeof line, *brand(t) ? " GPTPS  %s" : " %s", t->cfg.title);
    if (len < 0) len = 0;
    if (len > (int)sizeof line - 1) len = (int)sizeof line - 1;
    if (len + 2 + sl + 1 <= W) {
        while (len < W - sl - 1) line[len++] = ' ';
        memcpy(line + len, stats, (size_t)sl); len += sl;
    }
    if (len > W) len = W;
    while (len < W) line[len++] = ' ';
    line[len] = 0;
    return appendf(buf, cap, pos, "\x1b[7m\x1b[1m%s\x1b[0m\n", line);
}

/* The dashboard pane, formatted from its view with no lock held. Returns the length it
 * needed (see appendf); the caller finishes the frame. */
static size_t dash_format(const gptps_tui *t, tui_view *v, char *buf, size_t cap)
{
    size_t pos = 0;
    int i, color = v->color, uni = v->uni, W = v->W;
    const char *B = color ? "\x1b[1m" : "", *D = color ? "\x1b[2m" : "", *R = color ? "\x1b[31m" : "",
               *G = color ? "\x1b[32m" : "", *X = color ? "\x1b[0m" : "";
    char stats[96], up[40], rt[32];

    /* header: a framed title bar when color is on; a plain line otherwise */
    up[0] = 0;
    if (v->clock != GPTPS_TUI_CLOCK_OFF) fmt_up(up, sizeof up, v->up_ms, v->clock);
    if (v->rate_state == 1) snprintf(rt, sizeof rt, "%.1f", v->rate);
    else snprintf(rt, sizeof rt, "%s", v->rate_state == 2 ? "n/a" : "-");
    if (color) {
        int k;
        snprintf(stats, sizeof stats, "%s%s%s done/s", up, up[0] ? "  " : "", rt);
        pos = titlebar(buf, cap, pos, t, W, stats);
        if (v->rule_W != W || v->rule_uni != uni) {        /* the rule: cached, by width and glyphs */
            size_t rp = (size_t)snprintf(v->rule, sizeof v->rule, "\x1b[2m");
            for (k = 0; k < W && rp + 4 < sizeof v->rule - 6; ++k) {
                memcpy(v->rule + rp, uni ? "\xe2\x94\x80" : "-", uni ? 3 : 1); rp += uni ? 3 : 1;
            }
            memcpy(v->rule + rp, "\x1b[0m\n", 6);
            v->rule_W = W; v->rule_uni = uni;
        }
        pos = appendf(buf, cap, pos, "%s", v->rule);
    } else {
        pos = appendf(buf, cap, pos, "%s%s%s%s   %s done/s\n", brand(t), t->cfg.title, up[0] ? "   " : "", up, rt);
    }
    if (v->shed) {                       /* ADAPT: what DEGRADE shed, under the header, until undone */
        char dl[320], sn[64], wn[160];
        shed_text(v->shed, sn, sizeof sn);
        pnames(v->shed_why, wn, sizeof wn);
        snprintf(dl, sizeof dl, "DEGRADED: %s (pressure: %s) - 'u' restores %s", sn, wn, kpi_str(v->kpi_req));
        pos = appendf(buf, cap, pos, "%s%.*s%s\n", R, (int)u8_fit(dl, W - 1), dl, X);
    }

    if (!v->gline) {                 /* MINIMAL keeps no gauge (nor a SUM source): what is alive, as one sum */
        char pc[16], gl[128];
        snprintf(gl, sizeof gl, "queued %llu  started %llu  pending+in-flight ~%s",
                 (unsigned long long)v->q, (unsigned long long)v->s, fmt_count(pc, sizeof pc, v->alive));
        pos = appendf(buf, cap, pos, "%.*s\n", (int)u8_fit(gl, W - 1), gl);   /* one row: the fit counts '\n' */
    } else {   /* the gauges, and an in-flight bar fitted to what the line leaves of the width */
        const char *mk = v->exact ? (v->lb ? ">=" : "") : "~";
        unsigned j, gw, w;
        int ll, rl;
        char left[160], right[48], pc[16];
        const char *fillc = uni ? "\xe2\x96\x88" : "#";   /* full block / hash */
        const char *trakc = uni ? "\xe2\x96\x91" : ".";   /* light shade / dot  */
        const char *C = color ? "\x1b[36m" : "";          /* cyan bar */
        ll = snprintf(left, sizeof left, "queued %llu  started %llu  pending %s%s  in-flight %s%llu  ",
                      (unsigned long long)v->q, (unsigned long long)v->s, mk, fmt_count(pc, sizeof pc, v->pend), mk,
                      (unsigned long long)v->inflt);
        rl = snprintf(right, sizeof right, " peak %llu", (unsigned long long)v->peak);
        gw = (unsigned)(W / 3); if (gw > 40) gw = 40;
        if ((int)gw > W - 1 - ll - 2 - rl) gw = (unsigned)(W - 1 - ll - 2 - rl > 0 ? W - 1 - ll - 2 - rl : 0);
        if (gw < 10) gw = 10;
        w = (v->peak && v->inflt) ? (unsigned)((double)v->inflt * gw / (double)v->peak) : 0; if (w > gw) w = gw;
        pos = appendf(buf, cap, pos, "%s%s[%s", left, D, C);
        for (j = 0; j < gw; ++j) pos = appendf(buf, cap, pos, "%s", j < w ? fillc : trakc);
        pos = appendf(buf, cap, pos, "%s%s]%s%s\n", X, D, X, right);
    }
    {
        char cn[48], pl[256];
        int plen;
        cn[0] = 0;                                   /* cancelled: part of failed, named when any */
        if (v->cancel) snprintf(cn, sizeof cn, " (%llu cancelled)", (unsigned long long)v->cancel);
        plen = snprintf(pl, sizeof pl, "finished %llu  failed %llu%s  retried %llu  dead %llu  dropped %llu",
                        (unsigned long long)v->fin, (unsigned long long)v->fail, cn, (unsigned long long)v->retr,
                        (unsigned long long)v->dead, (unsigned long long)v->drop);
        if (plen >= 0 && (size_t)plen < sizeof pl && (int)u8_fit(pl, W - 1) == plen)
            pos = appendf(buf, cap, pos, "%sfinished %llu%s  %sfailed %llu%s%s  retried %llu  %sdead %llu%s  %sdropped %llu%s\n",
                          G, (unsigned long long)v->fin, X, (v->fail ? R : ""), (unsigned long long)v->fail, X, cn,
                          (unsigned long long)v->retr, (v->dead ? R : ""), (unsigned long long)v->dead, X,
                          (v->drop ? R : ""), (unsigned long long)v->drop, X);
        else   /* narrower than the line: one row, cut, without its colors (the fit counts '\n') */
            pos = appendf(buf, cap, pos, "%.*s\n", (int)u8_fit(pl, W - 1), pl);
    }
    {   /* the tier, and the parts it runs without for want of memory (logged) */
        static const char *const MISS[5] = { "problems log", "tasks", "activity log", "latency", "percentiles" };
        char kl[320];
        size_t kp = 0;
        int k, nm = 0;
        char sn[sizeof v->src_name * 2];
        snprintf(kl, sizeof kl, "kpi:%s mode:%s refresh:%ums%s%s", kpi_str(v->kpi), mode_str(v->mode), v->refresh,
                 v->src ? " source:" : "", v->src ? tui_cl(v->src_name, sn, sizeof sn) : "");
        pos = appendf(buf, cap, pos, "%s%.*s%s\n", D, (int)u8_fit(kl, W - 1), kl, X);
        if (v->src == 2) {               /* never silent: what this frame shows instead */
            snprintf(kl, sizeof kl, "source %s: read failed (%s), %llu errors - own counts shown",
                     tui_cl(v->src_name, sn, sizeof sn), gptps_strerror(v->src_st), (unsigned long long)v->src_errors);
            pos = appendf(buf, cap, pos, "%s%.*s%s\n", R, (int)u8_fit(kl, W - 1), kl, X);
        }
        for (k = 0; k < 5 && kp < sizeof kl; ++k)    /* and, when any, the parts it lacks: a line of its own */
            if (v->miss & (1 << k))
                kp += (size_t)snprintf(kl + kp, sizeof kl - kp, "%s%s", nm++ ? ", " : "no memory for: ", MISS[k]);
        if (nm) pos = appendf(buf, cap, pos, "%s%.*s%s\n", R, (int)u8_fit(kl, W - 1), kl, X);
    }
    if (v->pline[0]) {                   /* ADAPT: the last window's conditions, with what to change */
        const char *c = v->pline;
        while (c) {
            const char *nl = strchr(c, '\n');
            char one[400];
            size_t len = nl ? (size_t)(nl - c) : strlen(c);
            if (len >= sizeof one) len = sizeof one - 1;
            memcpy(one, c, len); one[len] = 0;
            pos = appendf(buf, cap, pos, "%s%.*s%s\n", R, (int)u8_fit(one, W - 1), one, X);
            c = nl ? nl + 1 : NULL;
        }
    }
    /* never silent: a line of what has been lost, only once something has (gptps_tui_health) */
    if (v->l_folded || v->l_untracked || v->l_plost || v->l_trunc || v->l_nomem) {
        char ls[320];
        size_t lp = (size_t)snprintf(ls, sizeof ls, "lost:");
        if (v->l_folded)
            lp += (size_t)snprintf(ls + lp, sizeof ls - lp, " %llu%s types folded into (other);",
                                   (unsigned long long)v->l_folded, v->fold_more ? "+" : "");
        if (v->l_untracked && lp < sizeof ls)
            lp += (size_t)snprintf(ls + lp, sizeof ls - lp, " %llu items not tracked (latency window full);",
                                   (unsigned long long)v->l_untracked);
        if (v->l_plost && lp < sizeof ls)
            lp += (size_t)snprintf(ls + lp, sizeof ls - lp, " %llu problem events not shown (tui.recent too small);",
                                   (unsigned long long)v->l_plost);
        if (v->l_trunc && lp < sizeof ls)
            lp += (size_t)snprintf(ls + lp, sizeof ls - lp, " %llu frames truncated;", (unsigned long long)v->l_trunc);
        if (v->l_nomem && lp < sizeof ls)
            lp += (size_t)snprintf(ls + lp, sizeof ls - lp, " %llu allocations failed;", (unsigned long long)v->l_nomem);
        if (lp >= sizeof ls) lp = sizeof ls - 1;
        if (lp && ls[lp - 1] == ';') ls[--lp] = 0;
        pos = appendf(buf, cap, pos, "%s%.*s%s\n", R, (int)u8_fit(ls, W - 1), ls, X);
    }

    v->rows_at = pos;                    /* what a DEGRADE step can shorten starts here */
    if (v->has_tasks) {                  /* the TASKS tier and up, and shown (else not built) */
        pos = appendf(buf, cap, pos, "\n%sTASKS%s\n", B, X);
        /* max ms beside avg ms once the handle table is on (LATENCY and up) and the width has
         * room: the slowest queue->finish a FINISHED item of the type took since install */
        pos = appendf(buf, cap, pos, "%s  %-16s %5s %5s %5s %5s %5s %5s %8s%s%s %8s  key%s\n",
                      D, "label", "run", "ok", "fail", "dead", "drop", "ok%", "avg ms",
                      v->mcols ? "   max ms" : "", v->pcols ? "    p50    p95    p99" : "", "peak MB", X);
        /* the types in the order first seen, then '(other)' once anything was folded there */
        for (i = 0; i < v->nrows; ++i) {
            const tui_vrow *k = &v->rows[i];
            char key[8], pct[8], lat[24], mx[24], peak[32], lb[TUI_NAME_CAP * 2], c[5][16], pcs[64];
            uint64_t terminal = k->finished + k->dead + k->dropped;
            const char *pc = "";
            if (k->hotkey > 32 && k->hotkey < 127) snprintf(key, sizeof key, "[%c]", k->hotkey); else key[0] = 0;
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
            if (k->lat_n) snprintf(lat, sizeof lat, "%8.1f", (double)k->lat_sum / (double)k->lat_n);
            else          strcpy(lat, "      --");
            mx[0] = 0;
            if (v->mcols) {
                if (k->lat_n) snprintf(mx, sizeof mx, " %8llu", (unsigned long long)k->lat_max);
                else          strcpy(mx, "       --");
            }
            /* What a process job's attempts measured at their peak (docs/MEASUREMENTS.md);
             * "--" for an in-process task, which is never measured. */
            if (k->has_mem) snprintf(peak, sizeof peak, "%8.1f", (double)k->mem_peak / 1048576.0);
            else            strcpy(peak, "      --");
            pcs[0] = 0;
            if (v->pcols) snprintf(pcs, sizeof pcs, " %6s %6s %6s", k->pq[0], k->pq[1], k->pq[2]);
            pos = appendf(buf, cap, pos, "  %s %5s %5s %5s %5s %5s %s%5s%s %8s%s%s %8s  %s\n",
                          tui_cell(k->label, lb, sizeof lb, 16), fmt_count(c[0], sizeof c[0], k->started),
                          fmt_count(c[1], sizeof c[1], k->finished), fmt_count(c[2], sizeof c[2], k->failed),
                          fmt_count(c[3], sizeof c[3], k->dead), fmt_count(c[4], sizeof c[4], k->dropped),
                          pc, pct, color ? X : "", lat, mx, pcs, peak, key);
        }
        if (v->more)                     /* fitted to the terminal: the rows it has no room for */
            pos = appendf(buf, cap, pos, "  %s+%d more (a taller terminal shows them)%s\n", D, v->more, X);
        /* never silent: the types the full table folds, and the latency it lost */
        if (v->nfold)
            pos = appendf(buf, cap, pos, "  %s+%d%s types folded into (other) (the table holds %d)%s\n", R, v->nfold,
                          v->fold_more ? " or more" : "", v->max_tasks, X);
        if (v->has_tab && (v->lat_untracked || v->lat_unmatched))
            pos = appendf(buf, cap, pos, "  %slatency: %llu not tracked (window %d full), %llu ends unmatched%s\n",
                          D, (unsigned long long)v->lat_untracked, v->lat_cap, (unsigned long long)v->lat_unmatched, X);
    }

    if (v->has_rec) {                    /* a journal's tier and up, and shown (else not built) */
        char lost[48];
        lost[0] = 0;
        if (v->frame_unseen)
            snprintf(lost, sizeof lost, "  %llu not shown", (unsigned long long)v->frame_unseen);
        if (v->scroll > 0) pos = appendf(buf, cap, pos, "\n%sRECENT%s  %s(scrolled +%d)%s%s%s%s\n", B, X, D, v->scroll, X,
                                         R, lost, X);
        else               pos = appendf(buf, cap, pos, "\n%sRECENT%s%s%s%s%s%s%s\n", B, X, R, lost, X,
                                         D, v->act_on ? "" : "  problems only", X);
        for (i = v->nrec - 1; i >= 0; --i) {   /* oldest at the top */
            const tui_event *re = &v->recs[i];
            char pre[128], nm[TUI_NAME_CAP * 2], why[sizeof re->why + TUI_TAIL_KEEP + 16];
            const char *kc;
            int pl, room, wl = 0;
            double rel;
            why[0] = 0;
            if (re->why[0] || re->tail_len) why_join(re->why, re->tail, re->tail_len, re->tail_cut, why, sizeof why);
            kc = (re->kind == GPTPS_EV_FAILED || re->kind == GPTPS_EV_DEAD_LETTERED || re->kind == GPTPS_EV_DROPPED) ? R
               : (re->kind == GPTPS_EV_FINISHED) ? G : D;
            rel = (double)(re->ts - v->rec_t0) / 1000.0; /* seconds since install */
            /* One terminal row each: the vertical fit counts rows, so a line that wrapped took
             * two and pushed the frame past the screen. The fixed fields are measured as
             * printed; the why text gets what is left. */
            pl = snprintf(pre, sizeof pre, "  %6.1f %-8s %s #%llu", rel, kind_str(re->kind), tui_cell(re->name, nm, sizeof nm, 14),
                          (unsigned long long)re->handle);
            room = W - 1 - (pl < 0 ? 0 : pl) - 2;
            wl = (why[0] && room >= 4) ? (int)u8_fit(why, room) : 0;
            pos = appendf(buf, cap, pos, "  %s%6.1f%s %s%-8s%s %s #%llu%s%s%.*s%s\n",
                          D, rel, X, kc, kind_str(re->kind), X, nm, (unsigned long long)re->handle,
                          wl ? "  " : "", D, wl, why, X);
        }
    }

    /* transient action toast (fades after a few seconds), one row */
    if (v->toast_on) {
        char shown[sizeof v->status * 2];
        tui_clean(v->status, strlen(v->status), shown, sizeof shown, W - 1, NULL);
        pos = appendf(buf, cap, pos, "\n%s%s%s\n", (color ? "\x1b[36m" : ""), shown, X);
    }

    /* key legend: task hotkeys first, then the global keys, wrapped to the width (cached) */
    pos = appendf(buf, cap, pos, "\n%s%s%s\n", D, v->legend, X);
    return pos;
}

/* What a pane needs, formatted into buf: the length it needed (see appendf), not yet
 * finished. The dashboard formats its view, which the caller has just taken. */
static size_t pane_format(gptps_tui *t, char *buf, size_t cap)
{
    switch (t->pane) {
        case TUI_PANE_SETTINGS: return render_settings(t, buf, cap);
        case TUI_PANE_HELP:     return render_help(t, buf, cap);
        case TUI_PANE_TASKS:    return render_tasks(t, buf, cap);
        case TUI_PANE_DETAIL:   return render_detail(t, buf, cap);
        case TUI_PANE_DL:       return render_deadletter(t, buf, cap);
        default:                return t->view ? dash_format(t, t->view, buf, cap)
                                               : appendf(buf, cap, 0, "%s%s: no memory for the frame (see the log)\n",
                                                         brand(t), t->cfg.title);
    }
}

/* The frame gptps_tui_run / gptps_tui_snapshot paint: a buffer grown to what a frame needs,
 * from TUI_FRAME_MIN to TUI_FRAME_MAX (16 KiB cut a tall colour frame silently in 1.10). */
typedef struct { char *b; size_t cap; } tui_fbuf;

/* What the live loop learns from a frame it has made. */
typedef struct { uint64_t gen, next_change; int pane; } tui_fmeta;

/* rmu held, mu NOT held: the host's source, read into the view for view_take (gptps_tui.h,
 * SOURCE) - here, once a dashboard frame, and nowhere else. v->src: 0 no source, 1 read, 2
 * failed. What it returns is taken defensively: a row's name is cut at its last byte, and
 * every read is handed totals set afresh (struct_size the dashboard's, whatever the last
 * read wrote there; nothing reads struct_size after a read). */
static void src_read(gptps_tui *t, tui_view *v)
{
    size_t n = 0, cap = v->srows ? (size_t)v->srows_cap : 0, i;
    gptps_status st;
    v->src = 0; v->src_n = 0; v->src_lat = 0;
    if (!t->src.read) return;            /* set under rmu, which is held */
    memset(&v->stot, 0, sizeof v->stot);
    v->stot.struct_size = sizeof v->stot;
    if (cap) memset(v->srows, 0, cap * sizeof *v->srows);
    st = t->src.read(t->src.ctx, &v->stot, v->srows, sizeof *v->srows, cap, &n);
    v->src_st = st;
    if (st != GPTPS_OK) { v->src = 2; return; }
    v->src = 1; v->src_n = n;
    for (i = 0; i < cap && i < n; ++i) {
        v->srows[i].name[sizeof v->srows[i].name - 1] = 0;
        if ((v->srows[i].flags & GPTPS_TUI_SRC_ROW_LATENCY) && v->srows[i].lat_n) v->src_lat = 1;
    }
}

/* rmu held, the pane is the dashboard: take its view (allocating it first, unlocked). 0
 * with no memory for it (counted; WARNed once). */
static int view_frame(gptps_tui *t, uint64_t now, int loop, uint64_t *warn_plost)
{
    int R = draw_rows(t);
    *warn_plost = 0;
    if (!view_ready(t, R)) {
        if (!t->view_warned) {
            t->view_warned = 1;
            gptps_log(NULL, GPTPS_LOG_WARN, "gptps_tui: no memory for the dashboard frame's view; the frame "
                      "says so until there is");
        }
        return 0;
    }
    t->view->W = draw_width(t);
    t->view->R = R;
    src_read(t, t->view);
    view_take(t, t->view, now, loop, warn_plost);
    if (t->view->src_warn) {             /* never silent; at most once a minute, outside mu */
        char msg[384], sn[sizeof t->view->src_name * 2];   /* the host's name, cleaned as on the frame */
        snprintf(msg, sizeof msg, "gptps_tui: the dashboard's source '%s' failed to read (%s); the frame shows "
                 "the dashboard's own counts and says so (gptps_tui_health.source_errors: %llu)",
                 tui_cl(t->view->src_name, sn, sizeof sn), gptps_strerror(t->view->src_st),
                 (unsigned long long)t->view->src_errors);
        gptps_log(NULL, GPTPS_LOG_WARN, msg);
    }
    return 1;
}

static void warn_problems_lost(uint64_t n)
{
    char msg[384];
    if (!n) return;              /* never silent; once, and outside the lock, as the event path's WARNs */
    snprintf(msg, sizeof msg, "gptps_tui: %llu problem events (FAILED, RETRIED, DEAD_LETTERED, DROPPED) "
             "left the dashboard's PROBLEMS journal before a frame showed them; they are counted on the "
             "'lost:' line and in gptps_tui_health.problems_not_shown; raise tui.recent (cfg.recent_lines) "
             "or refresh more often to see them all", (unsigned long long)n);
    gptps_log(NULL, GPTPS_LOG_WARN, msg);
}

/* One frame into f, grown as it needs (run / snapshot). `loop`: the live loop's (clears
 * `dirty`; *meta filled). Returns its length. */
static size_t tui_frame(gptps_tui *t, tui_fbuf *f, int loop, tui_fmeta *meta)
{
    uint64_t warn = 0, now, t0;
    size_t pos, n;
    int dash = t->pane == TUI_PANE_DASH;
    tui_adapt(t, TUI_NOW_MS());          /* before any lock: a window's ADAPT (it may shed) */
    if (dash) trk_resync(t, 0);          /* before any lock: it asks the engine */
    mu_lock(&t->rmu);
    now = TUI_NOW_MS();            /* under rmu: frames take their times in order */
    pane_prepare(t, now);                /* the engine-reading panes: a pass only when due */
    t0 = tui_now_us();                   /* the frame's own making (ADAPT's RENDER): after the pass */
    if (!dash || !view_frame(t, now, loop, &warn)) {
        /* every pane's frame shows the state as of now: the next change is a 0 -> 1 again
         * (on the dashboard view_take clears it, under the same lock as the copy-out) */
        if (loop) { mu_lock(&t->mu); t->dirty = 0; mu_unlock(&t->mu); }
    }
    pos = pane_format(t, f->b, f->cap);
    if (pos >= f->cap && f->cap < TUI_FRAME_MAX) {   /* grow to what it needs, then format it again */
        size_t want = next_pow2(pos + 1);
        char *nb;
        if (want > TUI_FRAME_MAX) want = TUI_FRAME_MAX;
        nb = (char *)realloc(f->b, want);
        if (nb) {
            f->b = nb; f->cap = want;
            pos = pane_format(t, f->b, f->cap);
        } else {
            mu_lock(&t->mu); t->h_nomem++; mu_unlock(&t->mu);
        }
    }
    n = tui_finish(t, f->b, f->cap, pos);
    if (meta) {
        meta->pane = t->pane;
        meta->gen = dash && t->view ? t->view->gen : 0;
        meta->next_change = dash ? (t->view ? t->view->next_change : 0) : pc_next(&t->pc, now);
        if (dash) {                      /* a look held back: a frame when it may look again */
            uint64_t rd = rs_due_ms(t, now);
            if (rd && (!meta->next_change || rd < meta->next_change)) meta->next_change = rd;
        }
    }
    mu_unlock(&t->rmu);
    adapt_frame_us(t, TUI_FRAME_US(tui_now_us() - t0));
    warn_problems_lost(warn);
    return n;
}

size_t gptps_tui_render(gptps_tui *t, char *buf, size_t cap)
{
    uint64_t warn = 0, t0;
    size_t n;
    if (!t || !buf || cap == 0) { if (buf && cap) buf[0] = 0; return 0; }
    tui_adapt(t, TUI_NOW_MS());                       /* before any lock: a window's ADAPT */
    if (t->pane == TUI_PANE_DASH) trk_resync(t, 0);   /* before any lock: it asks the engine */
    mu_lock(&t->rmu);
    pane_prepare(t, TUI_NOW_MS());
    t0 = tui_now_us();
    if (t->pane == TUI_PANE_DASH) view_frame(t, TUI_NOW_MS(), 0, &warn);
    n = tui_finish(t, buf, cap, pane_format(t, buf, cap));
    mu_unlock(&t->rmu);
    adapt_frame_us(t, TUI_FRAME_US(tui_now_us() - t0));
    warn_problems_lost(warn);
    return n;
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
    t->status_ms = TUI_NOW_MS();
}
static void edit_append(gptps_tui *t, const unsigned char *b, int n)
{
    int cap = (int)sizeof t->editbuf - 1;
    if (t->editlen + n > cap) {
        snprintf(t->status, sizeof t->status, "the value is full (%d bytes)", cap);
        t->status_ms = TUI_NOW_MS();
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
        t->status_ms = TUI_NOW_MS();
        return;
    }
    memcpy(t->edit_key, in->key_copy, strlen(in->key_copy) + 1);
    snprintf(t->editbuf, sizeof t->editbuf, "%s", in->value);
    t->editlen = (int)strlen(t->editbuf); t->editing = 1; t->edit_need = 0;
}

/* The panes' keys work from the cache too: rmu taken and the cache made ready for the pane
 * shown (a pass only when one is due) - pc_unlock gives rmu back. A change made from a pane
 * (an edit, pause, clone, new, delete) and 'R' make it stale: the next frame reads again. */
static tui_pcache *pc_lock(gptps_tui *t) { mu_lock(&t->rmu); pane_prepare(t, TUI_NOW_MS()); return &t->pc; }
static void pc_unlock(gptps_tui *t) { mu_unlock(&t->rmu); }
static void pc_stale(gptps_tui *t) { mu_lock(&t->rmu); t->pc.stale = 1; mu_unlock(&t->rmu); }

/* Enter on a settings list: the selected setting read fresh (its key checked), to edit. */
static void pc_edit(gptps_tui *t, int *sel)
{
    gptps_setting_info info;
    int ok;
    pc_lock(t);
    ok = pc_fetch(t, sel, &info, TUI_NOW_MS());
    pc_unlock(t);
    if (ok) edit_begin(t, &info);
    else toast(t, "that setting is gone (the registry changed); the list is read again");
}

/* Settings-pane key handling. Returns 4 (settings interaction) or -1 (quit). */
static int settings_press(gptps_tui *t, int key)
{
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
            pc_stale(t);                                          /* show the value it now has */
            return 4;
        }
        edit_input(t, key);
        return 4;
    }
    switch (key) {
        case 'q': case 'Q': tui_quit(t); return -1;
        case 27: case 's': case 'S': case '\t': t->pane = 0; t->status[0] = 0; return 4;   /* back to dashboard */
        case 'j': case 'J': { tui_pcache *c = pc_lock(t); if (c->n && (size_t)t->sel + 1 < c->n) t->sel++; pc_unlock(t); return 4; }
        case 'k': case 'K': if (t->sel > 0) t->sel--; return 4;
        case '\r': case '\n': pc_edit(t, &t->sel); return 4;                                /* start editing */
        case 'R': case 'r': pc_stale(t); return 4;                                          /* read it again now */
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
    size_t n;
    tui_ctask info;                                   /* the selected row, from the cache */
    int have;
    memset(&info, 0, sizeof info);
    {
        tui_pcache *c = pc_lock(t);
        n = c->pane == TUI_PANE_TASKS ? c->n : 0;
        have = t->task_sel >= 0 && (size_t)t->task_sel < n;
        if (have) info = c->tasks[t->task_sel];
        pc_unlock(t);
    }

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
        pc_stale(t);
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
            pc_stale(t);                                   /* a type made: list it */
            return 4;
        }
        edit_input(t, key);
        return 4;
    }
    switch (key) {
        case 'q': case 'Q': tui_quit(t); return -1;
        case 27: case 's': case 'S': case 't': case 'T': case '\t': t->pane = TUI_PANE_DASH; t->status[0] = 0; return 4;
        case 'j': case 'J': if (n && (size_t)t->task_sel + 1 < n) t->task_sel++; return 4;
        case 'k': case 'K': if (t->task_sel > 0) t->task_sel--; return 4;
        case 'R': case 'r': pc_stale(t); return 4;           /* read the engine again now */
        case '\r': case '\n':                              /* inspect: open the detail pane */
            if (have) {
                copy_name(t->detail_task, sizeof t->detail_task, info.name);
                t->detail_sel = 0; t->status[0] = 0; t->pane = TUI_PANE_DETAIL;
            }
            return 4;
        case 'a': case 'A':                                /* pause / resume */
            if (have) {
                char nm[128]; int en = info.enabled; copy_name(nm, sizeof nm, info.name);
                gptps_set_task_enabled(t->e, nm, !en);
                toast(t, en ? "paused %s" : "resumed %s", nm);
                pc_stale(t);
            }
            return 4;
        case 'c': case 'C':                                /* clone */
            if (have) {
                copy_name(t->prompt_name, sizeof t->prompt_name, info.name);
                t->prompt_kind = TUI_PK_CLONE_NAME; t->editbuf[0] = 0; t->editlen = 0; t->edit_need = 0; t->status[0] = 0;
            }
            return 4;
        case 'n': case 'N':                                /* new program task */
            t->prompt_kind = TUI_PK_NEW_NAME; t->editbuf[0] = 0; t->editlen = 0; t->edit_need = 0; t->status[0] = 0; return 4;
        case 'd': case 'D':                                /* delete: capture the NAME now (index may shift) */
            if (have) {
                copy_name(t->prompt_name, sizeof t->prompt_name, info.name);
                t->prompt_kind = TUI_PK_CONFIRM_DELETE; t->status[0] = 0;
                pc_stale(t);                               /* its queued / in-flight, as they are now */
            }
            return 4;
        default: return 4;
    }
}

/* Detail-pane key handling: edit one task's settings inline (returns 4 / -1). */
static int detail_press(gptps_tui *t, int key)
{
    if (t->editing) {
        if (key == 27) { t->editing = 0; t->editbuf[0] = 0; t->editlen = 0; t->edit_need = 0; return 4; }
        if (key == '\r' || key == '\n') {
            char why[256];
            gptps_status st = gptps_settings_set_ex(t->e, t->edit_key, t->editbuf, why, sizeof why);
            set_status(t, t->edit_key, t->editbuf, st, why);
            t->editing = 0; t->editbuf[0] = 0; t->editlen = 0; t->edit_need = 0;
            pc_stale(t);
            return 4;
        }
        edit_input(t, key);
        return 4;
    }
    switch (key) {
        case 'q': case 'Q': tui_quit(t); return -1;
        case 27: case 's': case 'S': case '\t': t->pane = TUI_PANE_TASKS; t->status[0] = 0; return 4;
        case 'j': case 'J': { tui_pcache *c = pc_lock(t); if (c->n && (size_t)t->detail_sel + 1 < c->n) t->detail_sel++; pc_unlock(t); return 4; }
        case 'k': case 'K': if (t->detail_sel > 0) t->detail_sel--; return 4;
        case 'R': case 'r': pc_stale(t); return 4;
        case '\r': case '\n': pc_edit(t, &t->detail_sel); return 4;
        case 'w': case 'W': save_status(t, gptps_settings_save(t->e, t->cfg.settings_path)); return 4;
        default: return 4;
    }
}

/* Dead-letter pane key handling (returns 4 / -1). See render_deadletter. */
static int deadletter_press(gptps_tui *t, int key)
{
    if (t->dl_sel < 0 || (size_t)t->dl_sel >= t->nheld) t->dl_sel = t->nheld ? (int)t->nheld - 1 : 0;
    switch (key) {
        case 'q': case 'Q': tui_quit(t); return -1;
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
        if (key == 'q' || key == 'Q') { tui_quit(t); return -1; }
        t->pane = TUI_PANE_DASH; return 4;
    }
    if (key == 'q' || key == 'Q' || key == 27) {
        tui_quit(t);
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
    if (key == 'u' || key == 'U') {                 /* ADAPT: undo a degrade - the requested tier again */
        int rk, sh; mu_lock(&t->mu); rk = t->kpi_req; sh = t->shed != 0; mu_unlock(&t->mu);
        if (!sh) { toast(t, "nothing is degraded (kpi %s)", kpi_str(rk)); return 3; }
        if (gptps_tui_set_kpi(t, (gptps_tui_kpi)rk) == GPTPS_OK) toast(t, "restored kpi %s", kpi_str(rk));
        else toast(t, "restored kpi %s, without its buffers: no memory (see the log)", kpi_str(rk));
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
        t->ui_gen++;                                          /* the legend is rebuilt */
        tui_touch(t);
        mu_unlock(&t->mu);
        tui_kick(t);
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
    struct pollfd pf; unsigned char c;
    pf.fd = STDIN_FILENO; pf.events = POLLIN; pf.revents = 0;
    if (poll(&pf, 1, ms) > 0) {
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r == 1) return c;
        if (r == 0 || (errno != EINTR && errno != EAGAIN)) {
            /* readable but nothing to read: end of input (the terminal went away, or
             * stdin was never one). poll() would report that at once forever, and
             * the loop spun repainting with no wait: wait the interval anyway. */
            poll(NULL, 0, ms);
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
/* The next key typed, without waiting (-1: none). *eof: stdin is at its end (the terminal
 * went away): poll() would report it readable at once forever, so the loop stops
 * watching it rather than spin. */
static int term_key(int *eof, int *resized)
{
    struct pollfd pf; unsigned char c; ssize_t r;
    (void)resized;                                 /* POSIX: a resize is the host's SIGWINCH */
    pf.fd = STDIN_FILENO; pf.events = POLLIN; pf.revents = 0;
    if (poll(&pf, 1, 0) <= 0) return -1;
    r = read(STDIN_FILENO, &c, 1);
    if (r == 1) return c == 27 ? term_decode_escape() : c;   /* may be an arrow key; else a bare Esc */
    if (r == 0 || (errno != EINTR && errno != EAGAIN)) *eof = 1;
    return -1;
}
static int term_size_set(gptps_tui *t, int c, int r);
/* The terminal's size into t->cols / rows; 1 when it changed. */
static int term_size(gptps_tui *t)
{
    struct winsize ws;
    if (ioctl(fileno(t->cfg.out), TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)   /* the terminal drawn on */
        return term_size_set(t, ws.ws_col, ws.ws_row > 0 ? ws.ws_row : t->rows);
    return 0;
}
/* The wake handle: a self-pipe, both ends non-blocking and close-on-exec (a job the engine
 * starts meanwhile must not inherit it). *rfd: the end the loop reads. Where pipe2 exists
 * the pipe is born close-on-exec, so a job the engine forks on another thread between
 * pipe() and fcntl() cannot inherit it; elsewhere (macOS) that window remains. */
#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#  define TUI_HAVE_PIPE2 1
int pipe2(int fds[2], int flags);                  /* hidden by _POSIX_C_SOURCE; in libc all the same */
#endif
static int wake_open(gptps_tui *t, int *rfd)
{
    int fds[2], k;
#if defined(TUI_HAVE_PIPE2) && defined(O_CLOEXEC)
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0) {
        *rfd = fds[0];
        sc_set(&t->wake_h, (long)fds[1] + 1);
        return 1;
    }
    if (errno != ENOSYS) return 0;
#endif
    if (pipe(fds) != 0) return 0;
    for (k = 0; k < 2; ++k) {
        int fl = fcntl(fds[k], F_GETFL);
        if (fl < 0 || fcntl(fds[k], F_SETFL, fl | O_NONBLOCK) != 0 || fcntl(fds[k], F_SETFD, FD_CLOEXEC) != 0) {
            close(fds[0]); close(fds[1]);
            return 0;
        }
    }
    *rfd = fds[0];
    sc_set(&t->wake_h, (long)fds[1] + 1);          /* published last: tui_kick may use it from now */
    return 1;
}
/* Close it: first make it unreachable, then wait out a tui_kick / gptps_tui_wake that read
 * it before that (wakers), so none writes into a descriptor number reused meanwhile. */
static void wake_close(gptps_tui *t, int rfd)
{
    long h = sc_get(&t->wake_h);
    sc_set(&t->wake_h, 0);
    while (sc_get(&t->wakers)) poll(NULL, 0, 1);
    if (h > 0) close((int)(h - 1));
    if (rfd >= 0) close(rfd);
}
/* Wait for a key, a wake or `ms` (< 0: no limit). Bits: 1 stdin is readable, 2 woken (the
 * pipe, drained). */
static int term_wait(gptps_tui *t, int rfd, int use_stdin, long ms)
{
    struct pollfd pf[2]; int n = 0, r, got = 0, si = -1, wi = -1;
    (void)t;
    if (use_stdin) { si = n; pf[n].fd = STDIN_FILENO; pf[n].events = POLLIN; pf[n].revents = 0; ++n; }
    if (rfd >= 0)  { wi = n; pf[n].fd = rfd;          pf[n].events = POLLIN; pf[n].revents = 0; ++n; }
    if (n == 0 && ms < 0) ms = 250;                /* nothing to wait on: never block for good */
    if (ms > 86400000L) ms = 86400000L;
    r = poll(n ? pf : NULL, (nfds_t)n, ms < 0 ? -1 : (int)ms);
    if (r <= 0) return 0;                          /* the timeout, or a signal (EINTR) */
    if (wi >= 0 && pf[wi].revents) { char b[64]; while (read(rfd, b, sizeof b) > 0) { } got |= 2; }
    if (si >= 0 && pf[si].revents) got |= 1;       /* POLLHUP too: term_key then sees the end */
    return got;
}
/* One frame's bytes in one write() (stdio would send a line-buffered terminal one per
 * line). Returns what was written. */
static size_t term_write(FILE *out, const char *b, size_t n)
{
    size_t done = 0;
    int fd = fileno(out);
    fflush(out);
    while (done < n) {
        ssize_t w = write(fd, b + done, n - done);
        if (w < 0) { if (errno == EINTR) continue; break; }
        done += (size_t)w;
    }
    return done;
}
#else
static void term_enable(gptps_tui *t)
{
    HANDLE ho = GetStdHandle(STD_OUTPUT_HANDLE); DWORD m;
    if (GetConsoleMode(ho, &m)) { t->saved_out_mode = m; t->modes_saved = 1;
        SetConsoleMode(ho, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING); }
    /* The console sends WINDOW_BUFFER_SIZE_EVENT only with ENABLE_WINDOW_INPUT, which the
     * default input mode leaves out: set it (the other bits kept) so a resize repaints. */
    {
        HANDLE hi = GetStdHandle(STD_INPUT_HANDLE); DWORD im;
        if (hi != NULL && hi != INVALID_HANDLE_VALUE && GetConsoleMode(hi, &im)) {
            t->saved_in_mode = im; t->in_mode_saved = 1;
            SetConsoleMode(hi, im | ENABLE_WINDOW_INPUT);
        }
    }
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
  if (t->in_mode_saved) { SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), t->saved_in_mode); t->in_mode_saved = 0; }
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
/* The next key typed, without waiting (-1: none): a character's later UTF-8 bytes first,
 * then the console's input records - keys, and WINDOW_BUFFER_SIZE_EVENT, which sets
 * *resized (1.10 read and dropped it). *eof: there is no console input to read. */
static int term_key(int *eof, int *resized)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (tui_kq_i < tui_kq_n) return tui_kq[tui_kq_i++];
    if (in == NULL || in == INVALID_HANDLE_VALUE) { *eof = 1; return -1; }
    for (;;) {
        DWORD n = 0;
        INPUT_RECORD rec;
        if (!GetNumberOfConsoleInputEvents(in, &n)) { *eof = 1; return -1; }
        if (n == 0) return -1;
        if (!ReadConsoleInputW(in, &rec, 1, &n) || n == 0) return -1;
        if (rec.EventType == KEY_EVENT) {
            int c = tui_key_of(&rec.Event.KeyEvent);
            if (c >= 0) return c;
        } else if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            *resized = 1;
        }
        /* a key release, a mouse or focus event: on to the next record */
    }
}
static int term_size_set(gptps_tui *t, int c, int r);
static int term_size(gptps_tui *t)
{
    CONSOLE_SCREEN_BUFFER_INFO ci;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &ci)) {
        int c = ci.srWindow.Right - ci.srWindow.Left + 1, r = ci.srWindow.Bottom - ci.srWindow.Top + 1;
        return term_size_set(t, c > 0 ? c : t->cols, r > 0 ? r : t->rows);
    }
    return 0;
}
/* The wake handle: an auto-reset event (not inheritable). */
static int wake_open(gptps_tui *t, int *rfd)
{
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    *rfd = -1;
    if (!ev) return 0;
    t->wake_ev = ev;
    sc_set(&t->wake_h, 1);                         /* published last: tui_kick may use it from now */
    return 1;
}
/* Close it once no tui_kick / gptps_tui_wake that saw it is still using it. */
static void wake_close(gptps_tui *t, int rfd)
{
    int had = sc_get(&t->wake_h) != 0;
    (void)rfd;
    sc_set(&t->wake_h, 0);
    while (sc_get(&t->wakers)) Sleep(1);
    if (had) CloseHandle(t->wake_ev);
    t->wake_ev = NULL;
}
/* Wait for console input, a wake or `ms` (< 0: no limit). Bits: 1 input, 2 woken. */
static int term_wait(gptps_tui *t, int rfd, int use_stdin, long ms)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE), hs[2];
    DWORD n = 0, r;
    int ev = -1;
    (void)rfd;
    if (tui_kq_i < tui_kq_n) return 1;             /* a character's bytes still queued */
    if (use_stdin && in != NULL && in != INVALID_HANDLE_VALUE) hs[n++] = in;
    if (sc_get(&t->wake_h)) { ev = (int)n; hs[n++] = t->wake_ev; }
    if (n == 0) { Sleep(ms < 0 ? 250u : (DWORD)ms); return 0; }   /* never block for good */
    r = WaitForMultipleObjects(n, hs, FALSE, ms < 0 ? INFINITE : (DWORD)ms);
    if (r == WAIT_FAILED) { Sleep(15); return 0; }  /* never spin */
    if (r >= WAIT_OBJECT_0 + n) return 0;          /* the timeout */
    return (int)(r - WAIT_OBJECT_0) == ev ? 2 : 1;
}
/* One frame's bytes in one write. */
static size_t term_write(FILE *out, const char *b, size_t n)
{
    size_t done = fwrite(b, 1, n, out);
    fflush(out);
    return done;
}
#endif

/* The size, stored only when it changed, and then under rmu: a host's render on another
 * thread reads it while it formats (the loop, its only writer, reads it unlocked). */
static int term_size_set(gptps_tui *t, int c, int r)
{
    if (c == t->cols && r == t->rows) return 0;
    mu_lock(&t->rmu); t->cols = c; t->rows = r; mu_unlock(&t->rmu);
    return 1;
}

/* ---- output: one write a frame, only the lines that changed ----------------------
 * A frame's lines are its text between newlines (the last, after the final newline, is
 * often empty). frame_diff turns the screen that shows `old` into `nw`: each changed line
 * is rewritten in place (ESC[row;1H, the line, ESC[K - erase its old tail), a shorter
 * frame wipes what is below it (ESC[J); a frame identical to the last is 0 bytes. A full
 * repaint - home, every line with ESC[K, ESC[J: still no full-screen clear, so nothing
 * flashes - when asked (first frame, a resize, another pane), when there is no old frame,
 * or when more than half the lines changed (cheaper than addressing each). */
static size_t frame_nlines(const char *f) { size_t n = 1; for (; *f; ++f) n += *f == '\n'; return n; }
/* the room frame_diff needs for a frame of len bytes and n lines */
static size_t frame_out_cap(size_t len, size_t n) { return len + n * 24u + 64u; }
static size_t line_len(const char *p) { const char *e = strchr(p, '\n'); return e ? (size_t)(e - p) : strlen(p); }
static const char *line_next(const char *p) { const char *e = p ? strchr(p, '\n') : NULL; return e ? e + 1 : NULL; }

static size_t frame_diff(const char *nw, const char *old, int full, char *o)
{
    size_t op = 0, nn = frame_nlines(nw), no = old ? frame_nlines(old) : 0, ch = 0, i;
    const char *p, *q;
    if (!old) full = 1;
    if (!full) {
        for (i = 0, p = nw, q = old; i < nn || i < no; ++i, p = line_next(p), q = line_next(q)) {
            size_t pl = p ? line_len(p) : 0, ql = q ? line_len(q) : 0;
            if (!p || !q || pl != ql || memcmp(p, q, pl) != 0) ++ch;
        }
        if (ch == 0) return 0;                       /* nothing changed: nothing to send */
        if (ch * 2 > nn) full = 1;
    }
    if (full) {
        memcpy(o + op, "\x1b[H", 3); op += 3;        /* home */
        for (p = nw; (q = strchr(p, '\n')) != NULL; p = q + 1) {
            memcpy(o + op, p, (size_t)(q - p)); op += (size_t)(q - p);
            memcpy(o + op, "\x1b[K\n", 4); op += 4;  /* erase the old tail */
        }
        if (*p) { size_t l = strlen(p); memcpy(o + op, p, l); op += l; memcpy(o + op, "\x1b[K", 3); op += 3; }
        memcpy(o + op, "\x1b[J", 3); op += 3;        /* wipe the lines a longer frame left below */
        return op;
    }
    for (i = 0, p = nw, q = old; i < nn; ++i, p = line_next(p), q = line_next(q)) {
        size_t pl = line_len(p), ql = q ? line_len(q) : 0;
        if (q && pl == ql && memcmp(p, q, pl) == 0) continue;
        op += (size_t)snprintf(o + op, 24, "\x1b[%lu;1H", (unsigned long)(i + 1));
        memcpy(o + op, p, pl); op += pl;
        memcpy(o + op, "\x1b[K", 3); op += 3;
    }
    if (no > nn) op += (size_t)snprintf(o + op, 32, "\x1b[%lu;1H\x1b[J", (unsigned long)(nn + 1));
    return op;
}

/* Until when the frame just painted shows a toast (0: none): the loop must paint
 * again then to take it away, in every pane. */
static uint64_t toast_until(const gptps_tui *t, uint64_t now)
{
    return status_live(t->status, t->status_ms, now) ? t->status_ms + TUI_TOAST_MS : 0;
}

/* The live loop's cadence, with when a frame is due: CONTINUOUS every refresh_ms, but only
 * when something visible changed (see below); ON_DEMAND when state changed (no sooner than
 * refresh_ms after the last frame), a key was pressed, or what the frame shows changes
 * with time (a toast's expiry, the clock, done/s or peak settling); PAUSED only after a key
 * (or the first frame). `vis` (0: none): when the painted frame next changes with no
 * event. */
static int paint_due(int first, int kp, int mode, int dirty, uint64_t vis, uint64_t now)
{
    if (first || kp) return 1;
    if (mode == GPTPS_TUI_PAUSED) return 0;
    return dirty || (vis && now >= vis);
}

/* ---- live loop -------------------------------------------------------------------
 * Idle costs nothing: the loop blocks on {stdin, a wake handle} until the earliest
 * deadline of what it shows, and with none, for good (1.10 woke every refresh_ms in every
 * mode, PAUSED included, and repainted the whole frame in CONTINUOUS). The deadlines:
 *   - refresh_ms after the last frame, when state changed since (the rate limit);
 *   - a toast's expiry, in every pane (CONTINUOUS and ON_DEMAND: PAUSED is frozen - a
 *     repaint would also move the numbers it holds - so its toast stays until the next key);
 *   - the clock's next visible tick, when the header shows it;
 *   - done/s and peak settling, while they are not yet what an idle engine shows;
 *   - CONTINUOUS: every refresh_ms - and then, when the state's generation, the clock and
 *     the toast are as they were (the dashboard pane), no frame is even made: no render, no
 *     ioctl, no write (frames_skipped). The other panes read the engine, so CONTINUOUS
 *     formats them every refresh_ms, and an unchanged one writes 0 bytes.
 * The wake handle (a self-pipe on POSIX, an auto-reset event on Windows) exists only while
 * gptps_tui_run runs: a headless or MANUAL host, or one that only snapshots, has none. The
 * observer signals it only on a dirty 0 -> 1 transition while the loop waits in ON_DEMAND,
 * and only after it has dropped the lock - at most one signal a frame interval, none when
 * no loop waits. Setters, quit and gptps_tui_wake signal it too. */
void gptps_tui_run(gptps_tui *t)
{
    tui_fbuf fb, pv;                    /* the frame being made, and the one the screen shows */
    char *ob = NULL;
    size_t ocap = 0;
    int first = 1, kp = 0, rfd = -1, have_wake, use_stdin = 1, resized = 0, painted_pane = -1;
    uint64_t last = 0, painted_gen = 0, painted_lost = 0, vis = 0;
    if (!t || !t->cfg.interactive) return;
    /* Needs a terminal both ways: frames out, keys in. With stdin at end of input
     * (</dev/null, a closed pipe, a launcher that gives it none) the key poll returned
     * at once and the loop repainted tens of thousands of frames a second. */
    if (!fd_is_tty(t->cfg.out) || !fd_is_tty(stdin)) {
        gptps_log(NULL, GPTPS_LOG_WARN, "gptps_tui_run: stdin or the output is not a terminal; "
                  "the live dashboard does not run (gptps_tui_snapshot renders a frame)");
        return;
    }
    fb.b = (char *)malloc(TUI_FRAME_MIN);
    pv.b = fb.b ? (char *)malloc(TUI_FRAME_MIN) : NULL;
    if (!pv.b) {
        free(fb.b);
        mu_lock(&t->mu); t->h_nomem++; mu_unlock(&t->mu);
        gptps_log(NULL, GPTPS_LOG_WARN, "gptps_tui_run: no memory for the 16 KiB frame buffer; "
                  "the live dashboard does not run");
        return;
    }
    fb.cap = pv.cap = TUI_FRAME_MIN;
    pv.b[0] = 0;
    have_wake = wake_open(t, &rfd);
    if (!have_wake) {                   /* never silent: it still runs, as 1.10 did */
        mu_lock(&t->mu); t->h_nomem++; mu_unlock(&t->mu);
        gptps_log(NULL, GPTPS_LOG_WARN, "gptps_tui_run: could not make the live loop's wake handle (a pipe / "
                  "an event); it wakes every refresh_ms to look for changes instead of only when there is one");
    }
    term_enable(t);
    for (;;) {
        int q, md, dirty, paint, eof = 0, rz = 0, w;
        uint32_t rms;
        uint64_t now, gen, lost, deadline = 0;
        long ms;
        mu_lock(&t->mu); q = t->quit; md = t->mode; rms = t->cfg.refresh_ms; gen = t->gen; dirty = t->dirty;
        lost = t->h_trunc + t->h_nomem; mu_unlock(&t->mu);
        if (q) break;
        now = TUI_NOW_MS();
        if (sc_get(&t->wake_req)) { sc_set(&t->wake_req, 0); resized |= 2; }   /* gptps_tui_wake: look again */
        paint = first || kp || resized;
        if (!paint && md != GPTPS_TUI_PAUSED && now >= last + rms) {
            /* CONTINUOUS: the generation skip - the dashboard pane's state, clock and toast
             * as they were painted: no frame (the other panes read the engine: formatted) */
            int changed = md == GPTPS_TUI_CONTINUOUS
                        ? gen != painted_gen || lost != painted_lost || t->pane != TUI_PANE_DASH : dirty;
            paint = paint_due(0, 0, md, changed, vis, now);
            if (!paint && md == GPTPS_TUI_CONTINUOUS) {
                /* still follow the window (one ioctl: no render, no write), as 1.10 did
                 * every refresh_ms - a host need not install a SIGWINCH handler */
                if (term_size(t)) { resized |= 1; paint = 1; }
                else { last = now; mu_lock(&t->mu); t->h_skipped++; mu_unlock(&t->mu); }
            }
        } else if (!paint && md == GPTPS_TUI_ON_DEMAND && vis && now >= vis) {
            paint = 1;                          /* a toast to take away: not held to the rate limit */
        }
        if (paint) {
            int full, asked;
            size_t n, nl, olen = 0, wrote = 0, hdr = first ? 12 : 0;
            tui_fmeta meta;
            if (term_size(t)) resized |= 1;     /* the layout follows the window */
            n = tui_frame(t, &fb, 1, &meta);
            mu_lock(&t->mu); t->h_made++; mu_unlock(&t->mu);
            nl = frame_nlines(fb.b);
            asked = first || (resized & 1) || meta.pane != painted_pane;
            full = asked || (int)nl > draw_rows(t);     /* taller than the screen: scrolled, so redraw it all */
            if (ocap < hdr + frame_out_cap(n, nl)) {   /* the output buffer, grown with the frame */
                char *nb = (char *)realloc(ob, hdr + frame_out_cap(n, nl));
                if (nb) { ob = nb; ocap = hdr + frame_out_cap(n, nl); }
                else { mu_lock(&t->mu); t->h_nomem++; mu_unlock(&t->mu); }
            }
            if (!asked && strcmp(fb.b, pv.b) == 0) {
                olen = 0;                       /* the same frame, even one taller than the screen */
            } else if (ocap >= hdr + frame_out_cap(n, nl)) {
                if (first) memcpy(ob, "\x1b[?25l\x1b[2J", 10);   /* hide the cursor, clear once */
                olen = frame_diff(fb.b, first ? NULL : pv.b, full, ob + (first ? 10 : 0)) + (first ? 10 : 0);
                if (olen) wrote = term_write(t->cfg.out, ob, olen);
            } else {                            /* no room to build it: the frame as it is, in pieces */
                fputs(first ? "\x1b[?25l\x1b[2J\x1b[H" : "\x1b[H", t->cfg.out);
                fputs(fb.b, t->cfg.out); fputs("\x1b[J", t->cfg.out); fflush(t->cfg.out);
                wrote = n;
            }
            mu_lock(&t->mu);
            if (wrote) { t->h_painted++; t->h_written += wrote; } else t->h_skipped++;
            t->frame_bytes = fb.cap + pv.cap + ocap;
            painted_lost = t->h_trunc + t->h_nomem;   /* as this frame left them (it may add one) */
            mu_unlock(&t->mu);
            { tui_fbuf x = fb; fb = pv; pv = x; }   /* what the screen shows now */
            painted_pane = meta.pane;
            painted_gen = meta.pane == TUI_PANE_DASH ? meta.gen : gen;
            vis = meta.next_change;
            {
                uint64_t tu = toast_until(t, TUI_NOW_MS());
                if (tu && (!vis || tu < vis)) vis = tu;
            }
            last = now; first = 0; kp = 0; resized = 0;
        }
        /* how long to wait: the earliest deadline, none (block) when there is none */
        mu_lock(&t->mu);
        if (md == GPTPS_TUI_CONTINUOUS) deadline = last + rms;
        else if (md == GPTPS_TUI_ON_DEMAND) {
            if (t->dirty) deadline = last + rms;
            else if (have_wake) t->waiting = 1;  /* the next change signals (see tui_on_event) */
            if (vis && (!deadline || vis < deadline)) deadline = vis;
        }
        mu_unlock(&t->mu);
        if (!have_wake && md != GPTPS_TUI_CONTINUOUS && (!deadline || deadline > now + rms))
            deadline = TUI_NOW_MS() + rms;    /* no wake handle: look every refresh_ms */
        now = TUI_NOW_MS();
        ms = !deadline ? -1 : deadline > now ? (deadline - now > 86400000u ? 86400000L : (long)(deadline - now)) : 0;
        w = term_wait(t, rfd, use_stdin, ms);
        mu_lock(&t->mu); t->waiting = 0; t->h_wakes++; mu_unlock(&t->mu);
        if (w & 1) {                            /* every key that arrived (a paste), then one frame */
            int k;
            while ((k = term_key(&eof, &rz)) >= 0) {
                uint64_t sm = t->status_ms;
                int pr = gptps_tui_press(t, k);
                if (pr != 0 || t->status_ms != sm) kp = 1;   /* a key that did nothing: no frame */
                if (pr < 0) break;              /* quit: the keys typed after it are not applied */
            }
            if (rz) resized |= 1;
            if (eof && use_stdin) {
                use_stdin = 0;
                gptps_log(NULL, GPTPS_LOG_WARN, "gptps_tui_run: stdin reached its end (the terminal went away?); "
                          "keys are no longer read - the dashboard keeps drawing until gptps_tui_press(t, 'q')");
            }
        }
    }
    {
        static const char show[] = "\x1b[?25h";        /* show the cursor */
        term_write(t->cfg.out, show, sizeof show - 1);
    }
    term_restore(t);
    if (have_wake) wake_close(t, rfd);
    mu_lock(&t->mu); t->frame_bytes = 0; t->waiting = 0; mu_unlock(&t->mu);
    free(fb.b); free(pv.b); free(ob);
}

/* ---- settings registry bindings (target = the gptps_tui) ---- */
/* The released choices first, in their 1.10 order; the 1.11 tiers and synonyms appended. */
static const char *const TUI_KPI_CHOICES[]  = { "off", "minimal", "normal", "full",
                                                "gauge", "problems", "tasks", "activity", "latency", "count", 0 };
static const int TUI_KPI_CHOICE_VAL[] = { GPTPS_TUI_KPI_OFF, GPTPS_TUI_KPI_MINIMAL, GPTPS_TUI_KPI_NORMAL, GPTPS_TUI_KPI_FULL,
                                          GPTPS_TUI_KPI_GAUGE, GPTPS_TUI_KPI_PROBLEMS, GPTPS_TUI_KPI_TASKS,
                                          GPTPS_TUI_KPI_ACTIVITY, GPTPS_TUI_KPI_LATENCY, GPTPS_TUI_KPI_COUNT };
static const char *const TUI_MODE_CHOICES[] = { "realtime", "on-demand", "paused", 0 };
static const char *const TUI_CLOCK_CHOICES[] = { "off", "minutes", "seconds", 0 };   /* GPTPS_TUI_CLOCK_* - 1 */
static const char *const TUI_ADAPT_CHOICES[] = { "off", "report", "degrade", 0 };     /* GPTPS_TUI_ADAPT_* - 1 */

static size_t ts_rd_refresh(void *p, char *b, size_t c) { gptps_tui *t = (gptps_tui *)p; unsigned ms; mu_lock(&t->mu); ms = t->cfg.refresh_ms; mu_unlock(&t->mu); return (size_t)snprintf(b, c, "%u", ms); }
static gptps_status ts_wr_refresh(void *p, const char *v) { return gptps_tui_set_refresh((gptps_tui *)p, (uint32_t)strtoul(v, NULL, 10)); }
/* tui.kpi reads back what was REQUESTED (ADAPT: a degrade is not saved by 'w') */
static size_t ts_rd_kpi(void *p, char *b, size_t c) { gptps_tui *t = (gptps_tui *)p; int k; mu_lock(&t->mu); k = t->kpi_req; mu_unlock(&t->mu); return (size_t)snprintf(b, c, "%s", kpi_str(k)); }
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
static size_t ts_rd_clock(void *p, char *b, size_t c)
{
    gptps_tui *t = (gptps_tui *)p; int k;
    mu_lock(&t->mu); k = t->clock; mu_unlock(&t->mu);
    return (size_t)snprintf(b, c, "%s", k >= GPTPS_TUI_CLOCK_OFF && k <= GPTPS_TUI_CLOCK_SECONDS ? TUI_CLOCK_CHOICES[k - 1] : "?");
}
static gptps_status ts_wr_clock(void *p, const char *v)
{
    int k;
    for (k = 0; TUI_CLOCK_CHOICES[k]; ++k)
        if (strcmp(v, TUI_CLOCK_CHOICES[k]) == 0) return gptps_tui_set_clock((gptps_tui *)p, (gptps_tui_clock)(k + 1));
    return GPTPS_E_INVAL;              /* the registry checks the choices first: not reached */
}
static size_t ts_rd_adapt(void *p, char *b, size_t c)
{
    gptps_tui *t = (gptps_tui *)p; int k;
    mu_lock(&t->mu); k = t->adapt; mu_unlock(&t->mu);
    return (size_t)snprintf(b, c, "%s", adapt_str(k));
}
static gptps_status ts_wr_adapt(void *p, const char *v)
{
    int k;
    for (k = 0; TUI_ADAPT_CHOICES[k]; ++k)
        if (strcmp(v, TUI_ADAPT_CHOICES[k]) == 0) return gptps_tui_set_adapt((gptps_tui *)p, (gptps_tui_adapt)(k + 1));
    return GPTPS_E_INVAL;              /* the registry checks the choices first: not reached */
}
static size_t ts_rd_budget(void *p, char *b, size_t c) { gptps_tui *t = (gptps_tui *)p; int n; mu_lock(&t->mu); n = t->cost_pct; mu_unlock(&t->mu); return (size_t)snprintf(b, c, "%d", n); }
static gptps_status ts_wr_budget(void *p, const char *v) { return gptps_tui_set_cost_budget((gptps_tui *)p, (int)strtol(v, NULL, 10)); }
static size_t ts_rd_mode(void *p, char *b, size_t c) { gptps_tui *t = (gptps_tui *)p; int m; mu_lock(&t->mu); m = t->mode; mu_unlock(&t->mu); return (size_t)snprintf(b, c, "%s", mode_str(m)); }
static gptps_status ts_wr_mode(void *p, const char *v) { int m = (strcmp(v, "realtime") == 0) ? GPTPS_TUI_CONTINUOUS : (strcmp(v, "on-demand") == 0) ? GPTPS_TUI_ON_DEMAND : GPTPS_TUI_PAUSED; return gptps_tui_set_mode((gptps_tui *)p, (gptps_tui_mode)m); }

/* A formatted line to the engine's log (gptps_log takes a plain string). */
static void tui_logf(gptps_log_level lvl, const char *fmt, ...)
{
    char msg[1024];
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
        { "tui.latency_window", GPTPS_SETTING_UINT, "handles tracked for latency at LATENCY and FULL", NULL,             ts_rd_latwin,  ts_wr_latwin, 0, 0, 0 },
        /* 1.11 */
        { "tui.recent",         GPTPS_SETTING_UINT, "failed/retried/dead/dropped events the RECENT log keeps "
                                                    "(the PROBLEMS journal, kpi problems and up); 0 = off, "
                                                    "its memory freed",
                                                    NULL, ts_rd_recent, ts_wr_recent, 1, 0, TUI_JOURNAL_CAP },
        { "tui.activity",       GPTPS_SETTING_UINT, "queued/started/finished events the RECENT log keeps "
                                                    "(the ACTIVITY journal, kpi normal and up); by default "
                                                    "as tui.recent; 0 = off, its memory freed",
                                                    NULL, ts_rd_activity, ts_wr_activity, 1, 0, TUI_JOURNAL_CAP },
        { "tui.clock",          GPTPS_SETTING_ENUM, "the header's uptime: off, whole minutes (an idle "
                                                    "dashboard repaints once a minute) or seconds (once a "
                                                    "second)",
                                                    TUI_CLOCK_CHOICES, ts_rd_clock, ts_wr_clock, 0, 0, 0 },
        /* 1.12 */
        { "tui.adapt",          GPTPS_SETTING_ENUM, "what the dashboard does about its own pressure (its cost and "
                                                    "losses, checked once a rate window when a frame is made): off; "
                                                    "report - a PRESSURE line and a WARN naming the setting to "
                                                    "change; degrade - also shed collection a step at a time "
                                                    "(sticky: tui.kpi or 'u' restores)",
                                                    TUI_ADAPT_CHOICES, ts_rd_adapt, ts_wr_adapt, 0, 0, 0 },
        { "tui.cost_budget_pct", GPTPS_SETTING_UINT, "the dashboard observer's estimated cost (event rate x a "
                                                    "measured cost per kpi tier), in percent of one core, past "
                                                    "which tui.adapt reports pressure",
                                                    NULL, ts_rd_budget, ts_wr_budget, 1, 1, 100 },
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
    t->p_lost += t->p_seq - t->p_seen; t->drop_pend += t->p_seq - t->p_seen; t->p_seen = t->p_base = t->p_seq;
    old->prob = t->prob; t->prob = NULL; t->pcap = t->phead = 0;
}
static void act_out(gptps_tui *t, tui_parts *old)
{
    t->a_lost += t->a_seq - t->a_seen; t->drop_pend += t->a_seq - t->a_seen; t->a_seen = t->a_base = t->a_seq;
    old->act = t->act; old->act_nm = t->act_nm; t->act = NULL; t->act_nm = NULL; t->acap = t->ahead = 0;
}

/* Make the storage match KPI level `lvl` (-1: keep; a gptps_tui_kpi, DEFAULT and ACTIVITY
 * already resolved), latency window `win` (0: keep) and journal sizes `plines` / `alines`
 * (-1: keep; 0: off): build what they need and is missing, drop what they do not hold.
 * `lvl` and the sizes are what is REQUESTED; what runs is that minus what DEGRADE shed
 * (t->shed; `unshed`: the operator's word - clear it first). A
 * part that cannot be allocated is left out - the dashboard runs without it (a journal
 * being resized keeps its old size) - and `what` (cap bytes, "" when all is well) names
 * it, for the caller to log; GPTPS_E_NOMEM then. */
static gptps_status tui_reconfig(gptps_tui *t, int lvl, int win, int plines, int alines, int unshed,
                                 char *what, size_t cap)
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
    if (unshed) { t->shed = 0; t->shed_why = 0; t->shed_whys = 0; t->pane_shed_ms = 0; t->p_row = 0; }
    if (lvl < 0) lvl = t->kpi_req;
    t->kpi_req = lvl;
    if (win <= 0) win = t->lat_cap;
    if (plines >= 0) t->prob_want = plines;
    if (alines >= 0) t->act_want = alines;
    tier = shed_tier(t);
    cur_win = t->lat_cap;
    want_tasks = tier >= TUI_T_TASKS && t->show_tasks;
    want_prob  = tier >= TUI_T_PROBLEMS && t->show_recent && t->prob_want > 0;
    want_act   = tier >= TUI_T_ACTIVITY && t->show_recent && t->act_want > 0 && !(t->shed & GPTPS_TUI_SHED_ACTIVITY);
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
    if (otier < TUI_T_GAUGE && tier >= TUI_T_GAUGE) {
        t->inflight = 0; t->peak = 0; t->peak_prev = 0; t->peak_ms = TUI_NOW_MS();
    }
    /* done/s starts again when the totals do: at OFF they stood still, so a snapshot from
     * before would average the time off in as no work */
    if (otier == TUI_T_OFF && tier != TUI_T_OFF) {
        t->rate_n = 1; t->rate_head = 1 % TUI_RATE_N;
        t->rate[0].ms = TUI_NOW_MS(); t->rate[0].fin = t->fin;
    }
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
    t->kpi = TUI_LADDER[tier];          /* what runs; kpi_req what was asked for */
    t->tier = tier;
    tui_touch(t);
    /* Published LAST: tui_on_event reads this without the lock, so everything it
     * guards must already be consistent before the event path is allowed back in. */
    TUI_FLAG_SET(&t->off, tier == TUI_T_OFF);
    mu_unlock(&t->mu);
    parts_free(&old);                   /* swapped out above: nothing can reach them now */
    parts_free(&nw);                    /* nothing left, unless a part was not needed after all */
    mu_unlock(&t->smu);
    tui_kick(t);                        /* a live loop shows the new tier */
    trk_resync(t, 1);                   /* a table built on an idle engine is exact at once */
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
    if (t->view) { free(t->view->rows); free(t->view->recs); free(t->view->srows); free(t->view); }
    free(t->pc.tasks); free(t->pc.sets); free(t->pc.pool);
    mu_destroy(&t->rmu);
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
    mu_init(&t->rmu);
    t->e = e;
    t->pc.pane = -1; t->pc.sel_for = -1;    /* no pane read yet */
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
            { "adapt",           offsetof(gptps_tui_config, adapt),            0, GPTPS_TUI_ADAPT_DEGRADE },
            { "cost_budget_pct", offsetof(gptps_tui_config, cost_budget_pct),  0, 100 },
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
        t->clock = t->cfg.clock;
        if (!t->cfg.rate_window_s)   t->cfg.rate_window_s = 10;
        if (!t->cfg.pane_refresh_ms) t->cfg.pane_refresh_ms = 2000;
        if (!t->cfg.adapt)           t->cfg.adapt = GPTPS_TUI_ADAPT_REPORT;
        if (!t->cfg.cost_budget_pct) t->cfg.cost_budget_pct = TUI_COST_DEF;
        t->adapt = t->cfg.adapt; t->cost_pct = t->cfg.cost_budget_pct;
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
    t->kpi = t->kpi_req = GPTPS_TUI_KPI_OFF;
    TUI_FLAG_SET(&t->off, 1);
    if (tui_reconfig(t, kpi, 0, -1, -1, 1, what, sizeof what) != GPTPS_OK)
        tui_logf(GPTPS_LOG_WARN, "gptps_tui_install: no memory for %s; the dashboard runs without it", what);
    t->start_ms = TUI_NOW_MS();
    t->ad_ms = t->start_ms;               /* ADAPT's first window starts with the dashboard */
    if ((st = gptps_register_observer(e, tui_on_event, t)) != GPTPS_OK) {
        /* everything, the handle table too (64 KiB by default, 64 MiB at the cap): the
         * caller gets NULL back and has no handle to reclaim it with */
        tui_logf(GPTPS_LOG_ERROR, "gptps_tui_install: the engine refused the dashboard's observer (%s); "
                 "the dashboard was not installed", gptps_strerror(st));
        tui_destroy(t);
        return NULL;
    }
    mu_lock(&t->mu); t->observing = 1; mu_unlock(&t->mu);
    trk_resync(t, 1);              /* only now: an item queued before this is missed by the table */
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
    st = tui_reconfig(t, -1, handles, -1, -1, 0, what, sizeof what);
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
    st = tui_reconfig(t, (int)level, 0, -1, -1, 1, what, sizeof what);   /* build what the tier needs, free the rest;
                                                                          * the operator's word: nothing shed */
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
    st = tui_reconfig(t, -1, 0, problems, activity, 0, what, sizeof what);
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
    tui_adapt(t, TUI_NOW_MS());          /* before any lock: a window's ADAPT (it may shed) */
    trk_resync(t, 1);                    /* before the lock: it asks the engine - every call: the
                                          * host's own question, at the host's own rate */
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
    h.pane_reads        = t->h_pane_reads;
    h.pane_read_us      = t->pane_cost_us;
    h.pane_every_ms     = t->pane_every_ms;
    h.bytes_panes       = t->pane_bytes;
    h.adapt             = t->adapt;
    h.kpi_requested     = t->kpi_req;
    h.pressure          = t->pressure;
    h.pressure_seen     = t->pr_seen;
    h.pressure_windows  = t->p_row;
    h.windows           = t->ad_windows;
    h.pressure_warns    = t->ad_warns;
    h.shed              = t->shed;
    h.shed_pressure     = t->shed_why;
    h.degrade_steps     = t->ad_steps;
    h.event_rate        = t->ad_rate;
    h.cost_est_ppm      = t->ad_ppm;
    h.render_max_us     = t->ad_rmax_last;
    h.source            = t->src.read != NULL;
    h.source_status     = t->src_status;
    h.source_reads      = t->src_reads;
    h.source_errors     = t->src_errors;
    h.source_rows       = t->src_rows;
    h.pane_shed_ms      = t->pane_shed_ms;
    h.shed_pressure_all = t->shed_whys;
    mu_unlock(&t->mu);
    n = out->struct_size < sizeof h ? out->struct_size : sizeof h;   /* what the caller's struct has */
    memcpy((char *)out + sizeof(size_t), (char *)&h + sizeof(size_t), n - sizeof(size_t));
    return GPTPS_OK;
}

gptps_status gptps_tui_set_adapt(gptps_tui *t, gptps_tui_adapt mode)
{
    char what[400];
    gptps_status st;
    if (!t) return GPTPS_E_INVAL;
    if (mode == GPTPS_TUI_ADAPT_DEFAULT) mode = GPTPS_TUI_ADAPT_REPORT;
    if (mode != GPTPS_TUI_ADAPT_OFF && mode != GPTPS_TUI_ADAPT_REPORT && mode != GPTPS_TUI_ADAPT_DEGRADE)
        return GPTPS_E_INVAL;
    mu_lock(&t->mu);
    t->adapt = mode; t->cfg.adapt = mode;
    if (mode == GPTPS_TUI_ADAPT_OFF) { t->pressure = 0; t->ad_line[0] = 0; }   /* nothing computed: nothing shown */
    adapt_base(t, TUI_NOW_MS());          /* a fresh window from now */
    mu_unlock(&t->mu);
    /* the operator's word: what DEGRADE shed comes back (built again: it may allocate) */
    st = tui_reconfig(t, -1, 0, -1, -1, 1, what, sizeof what);
    if (what[0]) tui_logf(GPTPS_LOG_WARN, "gptps_tui_set_adapt: no memory for %s; the dashboard runs without it", what);
    return st;
}

gptps_status gptps_tui_set_source(gptps_tui *t, const gptps_tui_source *src)
{
    gptps_tui_source c;
    char name[sizeof t->src_name];
    if (!t) return GPTPS_E_INVAL;
    memset(&c, 0, sizeof c);
    name[0] = 0;
    if (src) {
        if (src->struct_size < sizeof c || !src->read) return GPTPS_E_INVAL;
        c.struct_size = sizeof c; c.ctx = src->ctx; c.read = src->read;   /* the name is copied, not kept */
        copy_u8(name, sizeof name, src->name && src->name[0] ? src->name : "host");
    }
    /* rmu first (the order everywhere): no frame is reading the old source once this has it */
    mu_lock(&t->rmu);
    mu_lock(&t->mu);
    t->src = c;
    memcpy(t->src_name, name, sizeof name);
    t->src_kind = -1;                    /* done/s and peak start again at the next frame */
    tui_touch(t);
    mu_unlock(&t->mu);
    mu_unlock(&t->rmu);
    tui_kick(t);
    return GPTPS_OK;
}

gptps_status gptps_tui_set_cost_budget(gptps_tui *t, int pct)
{
    if (!t || pct < 1 || pct > 100) return GPTPS_E_INVAL;
    mu_lock(&t->mu); t->cost_pct = pct; t->cfg.cost_budget_pct = pct; mu_unlock(&t->mu);
    return GPTPS_OK;
}

gptps_status gptps_tui_set_mode(gptps_tui *t, gptps_tui_mode mode)
{
    if (!t) return GPTPS_E_INVAL;
    if (mode < GPTPS_TUI_CONTINUOUS || mode > GPTPS_TUI_PAUSED) return GPTPS_E_INVAL;
    mu_lock(&t->mu); t->mode = mode; tui_touch(t); mu_unlock(&t->mu);
    tui_kick(t);                         /* a loop blocked in PAUSED wakes to the new cadence */
    return GPTPS_OK;
}

gptps_status gptps_tui_set_refresh(gptps_tui *t, uint32_t refresh_ms)
{
    if (!t) return GPTPS_E_INVAL;
    mu_lock(&t->mu); t->cfg.refresh_ms = refresh_ms ? refresh_ms : 250; tui_touch(t); mu_unlock(&t->mu);
    tui_kick(t);
    return GPTPS_OK;
}

gptps_status gptps_tui_set_clock(gptps_tui *t, gptps_tui_clock clock)
{
    if (!t) return GPTPS_E_INVAL;
    if (clock == GPTPS_TUI_CLOCK_DEFAULT) clock = GPTPS_TUI_CLOCK_MINUTES;
    if (clock != GPTPS_TUI_CLOCK_OFF && clock != GPTPS_TUI_CLOCK_MINUTES && clock != GPTPS_TUI_CLOCK_SECONDS)
        return GPTPS_E_INVAL;
    mu_lock(&t->mu); t->clock = clock; t->cfg.clock = clock; tui_touch(t); mu_unlock(&t->mu);
    tui_kick(t);
    return GPTPS_OK;
}

void gptps_tui_snapshot(gptps_tui *t)
{
    tui_fbuf f;
    if (!t) return;
    f.cap = TUI_FRAME_MIN;
    f.b = (char *)malloc(f.cap);
    if (!f.b) {
        mu_lock(&t->mu); t->h_nomem++; mu_unlock(&t->mu);
        gptps_log(NULL, GPTPS_LOG_WARN, "gptps_tui_snapshot: no memory for the 16 KiB frame buffer; no frame");
        return;
    }
    tui_frame(t, &f, 0, NULL);         /* one frame, right now, regardless of mode; grown to fit */
    fputs(f.b, t->cfg.out); fputc('\n', t->cfg.out); fflush(t->cfg.out);
    free(f.b);
}
