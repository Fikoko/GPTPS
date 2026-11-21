/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Fikoko. See LICENSE for the full text. */
/*
 * tui.h - a real-time terminal dashboard for GPTPS (optional, portable add-on).
 *
 * Attaches to an engine via the observer seam and shows live task activity:
 * cumulative counts (queued/started/finished/failed/retried/dead/dropped),
 * in-flight gauge, a per-task table, and a scrolling recent-events log. Fully
 * portable - ANSI/VT escapes only (no ncurses), with Windows VT enabled
 * automatically.
 *
 * Settings are layered:
 *   - GLOBAL (gptps_tui_config): refresh rate, color, interactivity, which panes
 *     to show, title, output stream.
 *   - LOCAL  (gptps_tui_add_task): per task type - a display label, a hotkey, and
 *     a payload to submit when that hotkey is pressed (add work straight from the
 *     dashboard).
 *
 * Design split (so the logic stays testable without a terminal):
 *   - gptps_tui_render() turns the live state into a frame STRING (pure).
 *   - gptps_tui_press() applies a key (submit a bound task, or quit) - the unit a
 *     real keystroke drives.
 *   - gptps_tui_run() is the blocking live loop (render + poll keys); it self-skips
 *     when stdout/stdin is not a TTY, so it is safe to call headlessly.
 *
 * Lifecycle: install -> add tasks -> (submit work / run) -> gptps_shutdown(e) ->
 * gptps_tui_close(t)  (close AFTER shutdown: close does not unregister the
 * dashboard's observer, which the engine may call until shutdown returns).
 *
 * Portable: POSIX (termios) + Windows (console mode); pure C99 + the public API.
 */
#ifndef GPTPS_TUI_H
#define GPTPS_TUI_H

#include "gptps.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gptps_tui gptps_tui;

/* How much the dashboard computes per event - its own CPU/RAM cost. A LADDER: each tier
 * is the one below it plus exactly one layer, in the order of what that layer costs on
 * the event path (the observer runs on the engine's worker threads, the dispatcher and
 * every submitting thread), so dial it to what you can spare. Tunable at runtime
 * (gptps_tui_set_kpi, `tui.kpi`, the 'm' key).
 *
 *   tier  enum (tui.kpi)          adds                                 per event
 *   0     OFF      ("off")        leaves the event path: one flag      ~1 ns, no lock
 *                                 read, no lock
 *   1     MINIMAL  ("minimal",    totals: queued, started, finished,   one lock +
 *                   or "count")   failed, retried, dead, dropped,      increments
 *                                 cancelled
 *   2     GAUGE    ("gauge")      the in-flight gauge (and pending     + ~0-2 ns
 *                                 beside it), its bar and peak
 *   3     PROBLEMS ("problems")   the PROBLEMS journal: FAILED,        0 on a success;
 *                                 RETRIED, DEAD_LETTERED, DROPPED with a failure: a
 *                                 with why (exit code / signal /       copy + a <= 512 B
 *                                 start error + stderr's last line)    scan
 *   4     TASKS    ("tasks")      the per-task TASKS rows ('(other)'   one hash probe
 *                                 row past max_tasks) and mem.peak
 *   5     NORMAL   ("normal",     the ACTIVITY journal: QUEUED,        one 24 B store
 *                   or "activity") STARTED, FINISHED
 *   6     LATENCY  ("latency")    the handle table: avg ms queue to    one hash op
 *                                 finish, EXACT pending / in-flight
 *   7     FULL     ("full")       latency percentiles p50/p95/p99      one increment
 *                                 (a log2-ms histogram per row)
 *
 * No tier has a per-event term that grows with the window, the task types or a stderr
 * tail. A layer's memory exists only at its tier and up (see STORAGE at install).
 * Values are never renumbered (1.3.0 did once): the released DEFAULT..FULL keep theirs,
 * and the tiers 1.10 had no name for are appended. tui.kpi reads back the released name
 * where a tier has one ("minimal", "normal"); "count" and "activity" are accepted when
 * written. The 'm' key steps UP the ladder, MINIMAL -> GAUGE -> ... -> FULL, and wraps
 * to MINIMAL - never into OFF, so a stray keypress cannot blind the dashboard (from OFF
 * it goes to MINIMAL). */
typedef enum {
    GPTPS_TUI_KPI_DEFAULT = 0, /* = FULL (sensible zero-init default) */
    /* Installed but OFF the event path: the observer returns before it takes the
     * dashboard lock, so an event costs one predictable-branch read of a flag and
     * nothing else, and every per-event part (TASKS table, journals, latency table)
     * is freed. MINIMAL and up are
     * cheap in work but NOT free in contention - each acquires one global
     * mutex per event, on every worker, the dispatcher and every submitting
     * thread. This tier is for leaving the dashboard installed after you are done
     * looking at it: the handle, the settings and the per-task labels all stay
     * valid, so raising the tier again costs nothing but the re-allocation.
     * Counters do not advance while OFF, so totals resume from where they stopped
     * rather than from where the engine actually is - the price of paying nothing. */
    GPTPS_TUI_KPI_OFF,
    GPTPS_TUI_KPI_MINIMAL,     /* tier 1, "count": the totals only - no gauges (the frame shows what is
                                * queued or running as one sum, '~'), no allocation */
    GPTPS_TUI_KPI_NORMAL,      /* tier 5: + PROBLEMS journal + TASKS table + ACTIVITY journal */
    GPTPS_TUI_KPI_FULL,        /* tier 7: + the handle table (avg ms, EXACT gauges) + percentiles */
    /* 1.11, appended: the tiers between */
    GPTPS_TUI_KPI_GAUGE = 5,   /* tier 2: + the in-flight / pending gauges and peak ('~') */
    GPTPS_TUI_KPI_PROBLEMS,    /* tier 3: + the PROBLEMS journal (failures, with why) */
    GPTPS_TUI_KPI_TASKS,       /* tier 4: + the per-task TASKS table and mem.peak */
    GPTPS_TUI_KPI_ACTIVITY,    /* tier 5 under its ladder name: the same tier as NORMAL, stored
                                * and read back as NORMAL */
    GPTPS_TUI_KPI_LATENCY,     /* tier 6: + the handle table (avg ms, EXACT gauges) */
    GPTPS_TUI_KPI_COUNT = GPTPS_TUI_KPI_MINIMAL   /* tier 1 under its ladder name */
} gptps_tui_kpi;

/* cfg.clock (1.11): the header's uptime. 0 = the default. 1.10 printed "up 12.3s", which
 * changed on every frame, so no frame could ever be skipped as unchanged. */
typedef enum {
    GPTPS_TUI_CLOCK_DEFAULT = 0,  /* = MINUTES (also for a 1.10-layout caller) */
    GPTPS_TUI_CLOCK_OFF,          /* no uptime in the header: an idle frame never changes */
    GPTPS_TUI_CLOCK_MINUTES,      /* "up <1m", "up 42m", "up 3h07m", "up 2d05h": an idle
                                   * dashboard repaints the header once a minute */
    GPTPS_TUI_CLOCK_SECONDS       /* "up 9s", "up 4m09s", "up 3h07m09s": once a second */
} gptps_tui_clock;

/* When the live loop (gptps_tui_run) repaints. Tunable at runtime. See IDLE at
 * gptps_tui_run for what each costs when nothing happens. */
typedef enum {
    GPTPS_TUI_CONTINUOUS = 0,  /* real-time: looks every refresh_ms, and repaints when something it
                                * shows changed (1.11: an unchanged dashboard is not even rendered;
                                * 1.10 repainted the whole frame every time) */
    GPTPS_TUI_ON_DEMAND,       /* repaint only when state changed (no sooner than refresh_ms after the
                                * last frame), on a key, or when what it shows changes with time - a
                                * toast fading, the clock, done/s and peak settling; blocked, no
                                * timer, in between */
    GPTPS_TUI_PAUSED           /* frozen; repaint only on an explicit key / gptps_tui_snapshot */
} gptps_tui_mode;

typedef struct {
    size_t      struct_size;   /* = sizeof(gptps_tui_config) */
    uint32_t    refresh_ms;    /* redraw interval in the live loop (0 => 250) */
    int         color;         /* -1 = auto (TTY), 0 = off, 1 = on */
    int         interactive;   /* -1 = auto (TTY), 0 = off, 1 = on (read hotkeys) */
    int         max_recent;    /* recent-event lines to keep/show (0 => 8, cap 4096 - more is clamped,
                                * with a WARN; 1.10 capped it at 64). Since 1.11 the PROBLEMS journal's
                                * size when recent_lines is 0, and so the ACTIVITY journal's too */
    int         show_tasks;    /* per-task table: >=0 show (default), <0 hide - and, since 1.11, not
                                * collected at all: no rows, no mem.peak, no percentiles */
    int         show_recent;   /* recent-events log: >=0 show (default), <0 hide - and, since 1.11, not
                                * collected at all: neither journal is allocated or written */
    int         kpi;           /* gptps_tui_kpi: how much to compute (0 => FULL; out of range is
                                * clamped to OFF or FULL, with a WARN) */
    int         mode;          /* gptps_tui_mode: live-loop cadence (0 => CONTINUOUS; out of range
                                * => CONTINUOUS, with a WARN) */
    int         latency_window;/* handles the latency (handle) table tracks at LATENCY and FULL (0 => 1024, cap
                                * 1<<20 - more is clamped, with a WARN). Since 1.11 a hash table of
                                * 32-byte slots, next_pow2((window + 64) * 4/3 + 1) of them on 64-bit
                                * targets: 64 KiB by default, 64 MiB at the cap (the 1.10 ring was
                                * 24 KiB / 24 MiB, and cost O(window) per FINISHED; the table is one
                                * hash operation an event at any size). Size it to your in-flight
                                * DEPTH, not your throughput: an entry lives from QUEUED to the
                                * item's terminal event (FINISHED, DEAD_LETTERED, DROPPED, or FAILED
                                * with GPTPS_E_CANCELLED). Past this many live entries a new item is
                                * NOT tracked - no tracked item is ever evicted - so it gives no avg
                                * ms sample and the gauges show a lower bound ('>='); that is
                                * counted, shown under the TASKS table and in gptps_tui_health, and
                                * logged (a WARN once per window size, naming the depth reached). avg
                                * ms is queue-to-finish of FINISHED items only. The table also makes
                                * the pending / in-flight gauges EXACT for the work queued after it
                                * was built (a lower bound until the engine is idle: see
                                * gptps_tui_gauges). Also
                                * settable at runtime as `tui.latency_window`. */
    const char *title;         /* dashboard title (default "tasks") */
    FILE       *out;           /* output stream (NULL => stdout) */
    const char *settings_path; /* where the Settings pane's 'w' (save) writes (NULL => engine's open path) */
    int         unicode;       /* -1 = auto (follow color), 0 = ASCII, 1 = box rules + block gauge */
    /* ---- 1.11: read only when struct_size covers them (0 = the default each) ---- */
    int         reserved;      /* never read. On 64-bit targets a 1.10 struct ended in 4 bytes of
                                * tail padding after `unicode`, and its sizeof - the struct_size a
                                * 1.10 caller passes - counts them; this fills them, so the fields
                                * below start past the end of every 1.10 struct and its padding is
                                * never read as one of them. */
    int         max_tasks;     /* TASKS rows, and gptps_tui_add_task entries (0 => 64; 1..4096,
                                * install-time; out of range => install returns NULL, with an ERROR).
                                * A row is about 250 bytes, allocated at the TASKS tier and up only
                                * (+ 256 B of histogram at FULL). The row
                                * of a task type is found by a hash of its name - one probe and one
                                * compare an event, whatever the number of types - and kept for the
                                * life of the table (its id never names another type). A type that
                                * arrives once every row is taken is counted in an '(other)' row,
                                * and the types folded there are counted (a WARN for the first, a
                                * line under the table, gptps_tui_health.types_folded). */
    /* The RECENT log is two journals, so lifecycle noise never pushes a failure out: the
     * PROBLEMS journal (FAILED, RETRIED, DEAD_LETTERED, DROPPED, each with why) from the
     * PROBLEMS tier up, and the ACTIVITY journal (QUEUED, STARTED, FINISHED; 24 B a record)
     * from NORMAL up. The pane merges them by time - so a 1.10 frame's FINISHED lines are
     * still there - and shows problems only when ACTIVITY is off. Each record has a sequence
     * number, so a frame knows exactly how many were overwritten before any frame showed
     * them: "N not shown" on the RECENT line (what left either journal since the last
     * frame - overwritten, or dropped by a resize, a 0 or a tier below the journal's),
     * gptps_tui_health.journal_not_shown (the running total), and for PROBLEMS the "lost:"
     * line (its running total) and one WARN, from the first frame that finds one lost.
     * Both resizable at runtime (gptps_tui_set_recent, `tui.recent`, `tui.activity`); a
     * journal of 0 lines there is off and holds no memory - NOTE: here, in the config, 0
     * means the default and -1 means off.
     * An ACTIVITY record names its task by its TASKS row, so a type folded into '(other)'
     * (past max_tasks) shows as '(other)' there; its PROBLEMS records keep the full name. */
    int         recent_lines;  /* PROBLEMS journal lines (0 => max_recent's value, by default 8;
                                * -1 => off; 1..4096; else install returns NULL, with an ERROR) */
    int         activity_lines;/* ACTIVITY journal lines (0 => as recent_lines; -1 => off; 1..4096) */
    int         clock;         /* gptps_tui_clock: the header's uptime (0 => MINUTES; out of range =>
                                * install returns NULL, with an ERROR) */
    int         rate_window_s; /* the window of the header's done/s and of peak (0 => 10 s; 1..300).
                                * done/s: what finished in the last this-many seconds, over that
                                * time (1.10: since install), from snapshots of the count a frame
                                * takes (none on the event path); "-" until there is an earlier
                                * snapshot to measure against, "n/a" at OFF (the totals stand
                                * still). peak: the highest in-flight in the last one to two
                                * windows (1.10: ever) */
    int         pane_refresh_ms;/* the tasks / settings / detail panes re-read the engine at most this
                                * often (0 => 2000; 100..600000) */
} gptps_tui_config;

/* Install the dashboard on engine `e` (registers an observer). cfg NULL => all
 * defaults. cfg->struct_size: the fields it covers are read and the rest are taken as
 * 0 (their defaults); 0 means the 1.10 layout (through `unicode`), so set it to
 * sizeof(gptps_tui_config) to use the fields after that. Returns NULL - logged - on
 * allocation failure; when the engine refuses the observer (e.g. GPTPS_E_BUSY on a
 * bounded engine once work has been submitted: install before the first submit); when
 * cfg->struct_size is nonzero but smaller than a config through `title`; or when
 * cfg.max_tasks is out of range. Without the memory for the TASKS table, the
 * recent-events log or the latency table it installs anyway, runs without that part
 * and logs a WARN.
 *
 * STORAGE. Each part belongs to a KPI tier and exists only at that tier and above (and
 * only while its pane is shown: show_tasks / show_recent < 0 drop it at every tier):
 *   PROBLEMS  the PROBLEMS journal, recent_lines x ~300 B;
 *   TASKS     the TASKS table: max_tasks + 1 rows of ~250 B, its name index and fold set
 *             (~19 KiB at the default 64);
 *   NORMAL    the ACTIVITY journal, activity_lines x 24 B (x 88 B when it has no TASKS
 *             table to name its tasks from);
 *   LATENCY   the latency (handle) table, 64 KiB at the default window;
 *   FULL      the percentile histograms, 256 B a row.
 * MINIMAL and GAUGE hold nothing per event. A part is
 * allocated with the dashboard's lock released, swapped in under it, and freed after it
 * has been swapped out - never on the event path, which allocates nothing at any level.
 * Lowering the level frees what the new level does not hold; the per-task counts start
 * again when the TASKS table is built again. */
gptps_tui *gptps_tui_install(gptps *e, const gptps_tui_config *cfg);

/* LOCAL per-task settings. `label` (NULL => task name) is shown in the table;
 * `hotkey` (0 => none) submits `payload` (len bytes, copied) for `task_name` when
 * pressed in the live loop / via gptps_tui_press. Safe to call before the task
 * type is registered, and at any KPI level (the bindings are kept at every level).
 * GPTPS_E_FULL past cfg.max_tasks names, or when the TASKS table exists and every row
 * is taken by the types its events brought; GPTPS_E_INVAL for a name longer than
 * GPTPS_TASK_NAME_MAX. */
gptps_status gptps_tui_add_task(gptps_tui *t, const char *task_name, const char *label,
                                int hotkey, const void *payload, size_t len);

/* gptps_tui_add_task whose hotkey submits with gptps_submit_ex's options (NULL:
 * identical to gptps_tui_add_task): a priority, a policy, a timeout, or a due time -
 * which counts from each press, so a "due in 5 s" key is due 5 s after it is pressed.
 * GPTPS_E_INVAL for an `opts` smaller than gptps_submit_ex accepts.
 * Re-running dead letters from the dead-letter view re-submits each with the options
 * it was first given (gptps_dead_letter.options), except a due time that has passed. */
gptps_status gptps_tui_add_task_ex(gptps_tui *t, const char *task_name, const char *label,
                                   int hotkey, const void *payload, size_t len,
                                   const gptps_submit_options *opts);

/* Render one frame into `buf` (NUL-terminated, truncated to cap - with a "[frame
 * truncated]" line, counted). Returns the length written (excluding NUL). Pure read of
 * live state - no terminal needed. The dashboard pane copies what it shows out under the
 * dashboard's lock - only the TASKS rows and RECENT lines that fit the terminal's height
 * (cols / rows: 80x24 headless) - and formats it with the lock released, so the engine's
 * threads wait for the copy, not the formatting. One render at a time per dashboard: a
 * second thread's render waits for the first. */
size_t gptps_tui_render(gptps_tui *t, char *buf, size_t cap);

/* Apply a key as if pressed: a bound hotkey submits its task (returns 1; if the engine
 * refuses the submit - the type is paused or unknown, the queue is full, a constraint
 * denies it - the toast says why and it returns 0); 'q'/ESC
 * requests quit (returns -1); 'k'/'j' scroll the log (returns 2); 'm' steps the
 * KPI tier up (FULL wraps to MINIMAL; never into OFF) and 'p' toggles pause (returns 3, "reconfigured"); 's' opens the
 * settings editor and '?' toggles the help overlay (returns 4, "UI"); else 0. */
int gptps_tui_press(gptps_tui *t, int key);

/* Runtime reconfiguration (thread-safe; the live loop picks changes up next frame).
 * Any gptps_tui_kpi value: DEFAULT => FULL, ACTIVITY => NORMAL, else GPTPS_E_INVAL.
 * Lowering the KPI tier frees the parts the new tier does not hold (see STORAGE)
 * and stops their per-event work; raising it builds them again. GPTPS_E_NOMEM when a
 * part the tier needs cannot be allocated: the tier is set, the dashboard runs
 * without that part (a journal, the TASKS table, the latency table and so avg ms and
 * the exact gauges, or the percentiles), and a WARN says so. */
gptps_status gptps_tui_set_kpi(gptps_tui *t, gptps_tui_kpi level);
/* Resize the RECENT journals (1.11): `problems` lines for the PROBLEMS journal, `activity`
 * lines for the ACTIVITY journal; -1 leaves one as it is, 0 turns it off and frees it,
 * 1..4096 sizes it (else GPTPS_E_INVAL, nothing changed). NOTE: 0 here is off, while 0 in
 * cfg.recent_lines / activity_lines is the default (-1 is off there). A resized journal starts empty;
 * the records it held that no frame had shown are counted as not shown. The sizes hold at
 * every tier and apply from the journal's tier up. Also `tui.recent` and `tui.activity`.
 * GPTPS_E_NOMEM (with a WARN) as gptps_tui_set_kpi. */
gptps_status gptps_tui_set_recent(gptps_tui *t, int problems, int activity);
/* Resize the latency (handle) table of the LATENCY and FULL tiers (0 => 1024, capped at 1<<20 with a WARN).
 * Size it to the in-flight DEPTH you expect; see cfg.latency_window. Drops the entries in
 * flight and the counts of what the old window lost; the gauges are a lower bound ('>=')
 * until the work then in flight has ended. GPTPS_E_NOMEM (with a WARN) when the new
 * table cannot be allocated: the dashboard runs without avg ms until a later resize or
 * set_kpi succeeds. */
gptps_status gptps_tui_set_latency_window(gptps_tui *t, int handles);
gptps_status gptps_tui_set_mode(gptps_tui *t, gptps_tui_mode mode);
gptps_status gptps_tui_set_refresh(gptps_tui *t, uint32_t refresh_ms);
/* The header's uptime (1.11): OFF, MINUTES (DEFAULT) or SECONDS; else GPTPS_E_INVAL. Also
 * `tui.clock` ("off", "minutes", "seconds"). See IDLE at gptps_tui_run for what each costs. */
gptps_status gptps_tui_set_clock(gptps_tui *t, gptps_tui_clock clock);
/* Wake gptps_tui_run (1.11): it re-reads the terminal's size and repaints - the lines that
 * changed, or all of them when the size did. Async-signal-safe (it calls only write(2) on
 * POSIX, SetEvent on Windows, and lock-free atomics), so a host's own SIGWINCH handler may
 * call it: the dashboard installs no signal handler - the host owns its process. Without
 * one, CONTINUOUS still notices a resize within refresh_ms; an idle ON_DEMAND or PAUSED
 * loop, which waits with no timeout, notices it only at its next frame (an event, a key,
 * the clock) - a change from 1.10, which looked every refresh_ms in every mode. A no-op
 * when no live loop runs. */
void gptps_tui_wake(gptps_tui *t);

/* How the pending / in-flight gauges were obtained (gptps_tui_health.gauges, and the
 * mark before them on the frame). EXACT needs the handle table (LATENCY and up): from the events
 * alone it cannot be exact, because a FAILED / GPTPS_E_CANCELLED for an attempt that
 * ran and one for an item cancelled while still queued (or parked for a retry) carry
 * the same kind, status, attempt and flags - so below LATENCY every FAILED is taken to
 * leave the running set, and an item cancelled while queued undercounts in-flight
 * while others run. */
typedef enum {
    GPTPS_TUI_GAUGES_APPROX = 0,      /* from the counters ('~' on the frame): GAUGE to TASKS,
                                       * and NORMAL; LATENCY and FULL without their table */
    GPTPS_TUI_GAUGES_EXACT,           /* from each handle's state (no mark) */
    GPTPS_TUI_GAUGES_LOWER_BOUND,     /* exact for the handles tracked, but some may not be
                                       * ('>='): every new table starts here (install, a tier
                                       * raised to LATENCY, a window resized - work queued before
                                       * it is not in it), and a handle that finds it full puts
                                       * it back. EXACT again only once the ENGINE reports
                                       * nothing queued or running while the table holds
                                       * nothing live - checked when a frame is rendered or
                                       * health is read; on an idle engine a new table is EXACT
                                       * at once. No event count can say it: an always-up
                                       * service ends every run with a FINISHED and no QUEUED,
                                       * so an engine that is never idle keeps '>=' (true) */
    GPTPS_TUI_GAUGES_SUM              /* MINIMAL (1.11): no gauges are kept; `pending` is what is
                                       * queued or running, as one sum ('~'), in_flight 0 */
} gptps_tui_gauges;

/* What the dashboard has lost, holds and does (1.11). Counters are since install and
 * never reset; bytes are what is held now. A frame shows a "lost:" line under the KPI
 * line once types_folded, handles_untracked, problems_not_shown, frames_truncated or
 * alloc_failures is nonzero (orphans_expired is not a loss of anything shown: see below;
 * the ACTIVITY journal's losses are on the RECENT line, frame by frame). */
typedef struct {
    size_t   struct_size;         /* = sizeof(gptps_tui_health_info); the call fills what it covers */
    /* losses */
    uint64_t journal_not_shown;   /* journal records (PROBLEMS and ACTIVITY) overwritten, resized or
                                   * freed away before a frame showed them - exact, by sequence
                                   * number; problems_not_shown is the PROBLEMS part */
    uint64_t handles_untracked;   /* QUEUEDs the full latency table could not take: no avg ms
                                   * sample, gauges a lower bound while they live */
    uint64_t orphans_expired;     /* markers the orphan FIFO dropped: a terminal event whose own
                                   * QUEUED (or RETRIED) never came - work queued before the table
                                   * existed, an always-up service's later runs, the end of an
                                   * untracked handle */
    uint64_t types_folded;        /* distinct task types counted in '(other)' (exact up to 192) */
    uint64_t frames_truncated;    /* frames that did not fit their buffer ("[frame truncated]") */
    uint64_t alloc_failures;      /* parts, buffers and copies the dashboard could not allocate */
    /* bytes held now, by part */
    uint64_t bytes_base;          /* the dashboard and its gptps_tui_add_task entries */
    uint64_t bytes_tasks;         /* TASKS rows, name index, fold set (TASKS and up) */
    uint64_t bytes_problems;      /* the PROBLEMS journal (PROBLEMS and up) */
    uint64_t bytes_activity;      /* the ACTIVITY journal (NORMAL and up) */
    uint64_t bytes_handles;       /* the latency (handle) table (LATENCY and up) */
    uint64_t bytes_histogram;     /* the percentile histograms (FULL) */
    uint64_t bytes_frame;         /* gptps_tui_run's frame buffers (16 KiB up to 256 KiB each),
                                   * while it runs */
    uint64_t bytes_dead_letters;  /* the dead-letter view's held items (array only) */
    /* the live loop */
    uint64_t frames_painted;      /* frames gptps_tui_run wrote (at least one byte) */
    uint64_t frames_skipped;      /* frames it did not write, as nothing visible had changed: not
                                   * made at all (CONTINUOUS, the dashboard unchanged) or made
                                   * identical to the one on the screen (0 bytes) */
    uint64_t bytes_written;       /* what those frames wrote to the output, escapes included: the
                                   * lines that changed, not the whole frame (see OUTPUT) */
    uint64_t wakes;               /* times gptps_tui_run's loop woke: a key, a wake (an event in
                                   * ON_DEMAND, a setter, gptps_tui_wake), or a deadline */
    /* gauges, as the frame shows them */
    int      gauges;              /* gptps_tui_gauges */
    uint64_t pending, in_flight;
    uint64_t handles_tracked;     /* live entries in the latency table */
    uint64_t handles_markers;     /* markers in it (at most 64 + 1) */
    uint64_t task_rows;           /* TASKS rows in use, '(other)' not counted */
    /* 1.11, journals */
    uint64_t problems_not_shown;  /* the PROBLEMS part of journal_not_shown */
    uint64_t problems_held, activity_held;   /* records each journal holds now */
    int      kpi;                 /* the tier in effect (gptps_tui_kpi, never DEFAULT / ACTIVITY) */
} gptps_tui_health_info;

/* Fill *out (struct_size bytes of it; the rest of a newer struct is left alone, and
 * out->struct_size is kept). GPTPS_E_INVAL for a NULL argument or a struct_size that
 * does not reach journal_not_shown. Thread-safe; takes the dashboard's lock briefly, and
 * while the gauges are a lower bound it asks the engine (gptps_task_get_info) whether it
 * is idle - so it may be called from an observer, which runs with the engine lock
 * released, but not from a callback gptps.h (THREADING) forbids to call the engine. */
gptps_status gptps_tui_health(gptps_tui *t, gptps_tui_health_info *out);

/* Render one frame to the configured output stream right now ("once" / on-demand),
 * independent of the live loop or mode. */
void gptps_tui_snapshot(gptps_tui *t);

/* Blocking live loop: clear screen, then redraw (see the modes) and dispatch keystrokes
 * until quit. No-op (returns immediately) unless both the output and stdin
 * are terminals - with a WARN when cfg.interactive asked for it anyway.
 *
 * IDLE (1.11). Nothing happening costs nothing: the loop blocks on {stdin, a wake handle}
 * until the earliest deadline of what it shows, and with none, for good - PAUSED and an idle
 * ON_DEMAND make no wake, no frame and no write (1.10 woke every refresh_ms in every mode).
 * The deadlines: refresh_ms after the last frame when state changed since (the rate limit);
 * a toast's expiry (in every pane); the clock's next visible tick (cfg.clock: once a minute
 * by default, never with OFF); done/s and peak settling after activity. CONTINUOUS looks
 * every refresh_ms and, when the dashboard's state, clock and toast are as painted, makes no
 * frame at all (frames_skipped: no render, no write - one size ioctl); the other panes read the
 * engine, so CONTINUOUS formats them every refresh_ms and writes only what changed. The wake
 * handle - a self-pipe (O_NONBLOCK, FD_CLOEXEC) on POSIX, an auto-reset event on Windows -
 * exists only while gptps_tui_run runs: a headless or MANUAL host, or one that only renders
 * or snapshots, has none. The observer signals it only when the state turns dirty while the
 * loop waits in ON_DEMAND, and after it has dropped its lock: at most one signal a frame
 * interval, none when no loop waits. Setters, a quit and gptps_tui_wake signal it too. A
 * resize is noticed: at once on Windows (WINDOW_BUFFER_SIZE_EVENT; the loop sets
 * ENABLE_WINDOW_INPUT on the console input while it runs, and restores the mode after) and
 * on POSIX when the host's SIGWINCH handler calls gptps_tui_wake; without that handler,
 * within refresh_ms in CONTINUOUS, and in an idle ON_DEMAND or PAUSED loop only at its
 * next frame (1.10 re-measured every refresh_ms in every mode). The loop waits with poll(),
 * so a host with more than FD_SETSIZE descriptors open is fine.
 *
 * OUTPUT (1.11). A frame is sent in one write. Only the lines that changed are rewritten
 * (ESC[row;1H ... ESC[K); a frame equal to the one on the screen is 0 bytes; the whole
 * frame is repainted after a resize, on another pane, or when more than half its lines
 * changed. The frame buffer grows from 16 KiB to what a frame needs, up to 256 KiB; past
 * that it is cut at a line with a visible "[frame truncated]" line (counted). The TASKS
 * table and the tasks pane are fitted to the terminal's height ("+N more").
 *
 * HOST PROFILES.
 *   - Embedded / MANUAL (no thread, render or snapshot only): no loop, no pipe, no timer -
 *     done/s, peak and the clock advance inside render. Pick the tier by what you can
 *     spare per event (MINIMAL or GAUGE); with the stub HAL's clock, cfg.clock = OFF.
 *   - A server at 100k events/s: every tier is O(1) an event, but MINIMAL and up take one
 *     mutex an event on every thread that emits; ON_DEMAND costs at most one wake a frame
 *     interval; size latency_window to the in-flight depth.
 *   - Task Scheduler / a service / headless: run returns at once (no console); only the
 *     observer's cost remains - set kpi to what snapshots need, or OFF.
 *   - An operator's console: the defaults (FULL, CONTINUOUS, clock in minutes) - idle, it
 *     writes nothing and repaints the header once a minute.
 *
 * Typed text is
 * UTF-8 (on Windows the console's UTF-16 is converted, keys told apart by their
 * virtual-key code); bytes that do not form UTF-8 characters - a terminal not set to
 * UTF-8 - are left out, and the status line says so. Anything shown that the
 * dashboard did not write - a job's stderr; a setting's key, value, default,
 * description and choices; a task's name and label; the status line, which repeats
 * a committed value - is shown as printable, valid UTF-8 only: control bytes, encoded
 * C1 controls, malformed sequences and invisible bidi/formatting characters never
 * reach the terminal. Settings and task names are read from the copies the engine
 * takes under its lock (ABI 2.8: gptps_setting_info.key_copy / desc_copy,
 * gptps_task_info.name_copy), so a task unregistered on another thread while the
 * dashboard draws cannot leave it reading freed memory.
 *
 * DEAD-LETTER VIEW ('l'). The engine's dead letters can only be read by draining them,
 * which takes them, so the view shows their number until the operator loads them
 * ('g'; 'r' and 'D' load them too). Loaded, they are HELD by the dashboard, each shown
 * with its task, status and why it failed (exit code or start error, and its stderr's
 * last line), and re-submitted ('r' all, 'e' the selected one) or discarded ('x' the
 * selected one, 'D' all). A re-submit the engine refuses keeps the item held, with the
 * reason. gptps_tui_close discards what is still held, with a WARN. */
void gptps_tui_run(gptps_tui *t);

/* Restore the terminal and free. Call AFTER gptps_shutdown(e). */
void gptps_tui_close(gptps_tui *t);

#ifdef __cplusplus
}
#endif
#endif /* GPTPS_TUI_H */
