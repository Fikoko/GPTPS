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

/* How much the dashboard computes per event - its own CPU/RAM cost. Higher tiers
 * do strictly more work in the observer that runs on the engine's worker threads,
 * so dial it to what you can spare. Tunable at runtime (gptps_tui_set_kpi). */
typedef enum {
    GPTPS_TUI_KPI_DEFAULT = 0, /* = FULL (sensible zero-init default) */
    /* Installed but OFF the event path: the observer returns before it takes the
     * dashboard lock, so an event costs one predictable-branch read of a flag and
     * nothing else, and every per-event part (TASKS table, recent log, latency table)
     * is freed. MINIMAL is
     * cheap in work but NOT free in contention - it still acquires one global
     * mutex per event, on every worker, the dispatcher and every submitting
     * thread. This tier is for leaving the dashboard installed after you are done
     * looking at it: the handle, the settings and the per-task labels all stay
     * valid, so raising the tier again costs nothing but the re-allocation.
     * Counters do not advance while OFF, so totals resume from where they stopped
     * rather than from where the engine actually is - the price of paying nothing. */
    GPTPS_TUI_KPI_OFF,
    GPTPS_TUI_KPI_MINIMAL,     /* cumulative counts only - one lock per event, no allocation; gauges ~ */
    GPTPS_TUI_KPI_NORMAL,      /* + per-task table + recent-events log */
    GPTPS_TUI_KPI_FULL         /* + the handle table: avg ms, and EXACT pending / in-flight gauges */
} gptps_tui_kpi;

/* When the live loop (gptps_tui_run) repaints. Tunable at runtime. */
typedef enum {
    GPTPS_TUI_CONTINUOUS = 0,  /* repaint every refresh_ms (real-time) */
    GPTPS_TUI_ON_DEMAND,       /* repaint only when state changed since last frame, or on a key */
    GPTPS_TUI_PAUSED           /* frozen; repaint only on an explicit key / gptps_tui_snapshot */
} gptps_tui_mode;

typedef struct {
    size_t      struct_size;   /* = sizeof(gptps_tui_config) */
    uint32_t    refresh_ms;    /* redraw interval in the live loop (0 => 250) */
    int         color;         /* -1 = auto (TTY), 0 = off, 1 = on */
    int         interactive;   /* -1 = auto (TTY), 0 = off, 1 = on (read hotkeys) */
    int         max_recent;    /* recent-event lines to keep/show (0 => 8, cap 64; more is clamped, with a WARN) */
    int         show_tasks;    /* per-task table: >=0 show (default), <0 hide */
    int         show_recent;   /* recent-events log: >=0 show (default), <0 hide */
    int         kpi;           /* gptps_tui_kpi: how much to compute (0 => FULL; out of range is
                                * clamped to OFF or FULL, with a WARN) */
    int         mode;          /* gptps_tui_mode: live-loop cadence (0 => CONTINUOUS; out of range
                                * => CONTINUOUS, with a WARN) */
    int         latency_window;/* handles the latency (handle) table tracks at FULL (0 => 1024, cap
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
                                * A row is about 250 bytes, allocated at NORMAL and up only. The row
                                * of a task type is found by a hash of its name - one probe and one
                                * compare an event, whatever the number of types - and kept for the
                                * life of the table (its id never names another type). A type that
                                * arrives once every row is taken is counted in an '(other)' row,
                                * and the types folded there are counted (a WARN for the first, a
                                * line under the table, gptps_tui_health.types_folded). */
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
 * STORAGE. Each part belongs to a KPI level and exists only at that level and above:
 * the TASKS table (max_tasks rows + its name index, ~16 KiB at the default 64) and the
 * recent-events log (max_recent x ~250 B) at NORMAL, the latency table at FULL. A part is
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

/* Render one frame into `buf` (NUL-terminated, truncated to cap). Returns the
 * length written (excluding NUL). Pure read of live state - no terminal needed. */
size_t gptps_tui_render(gptps_tui *t, char *buf, size_t cap);

/* Apply a key as if pressed: a bound hotkey submits its task (returns 1; if the engine
 * refuses the submit - the type is paused or unknown, the queue is full, a constraint
 * denies it - the toast says why and it returns 0); 'q'/ESC
 * requests quit (returns -1); 'k'/'j' scroll the log (returns 2); 'm' cycles the
 * KPI level and 'p' toggles pause (returns 3, "reconfigured"); 's' opens the
 * settings editor and '?' toggles the help overlay (returns 4, "UI"); else 0. */
int gptps_tui_press(gptps_tui *t, int key);

/* Runtime reconfiguration (thread-safe; the live loop picks changes up next frame).
 * Lowering the KPI level frees the parts the new level does not hold (see STORAGE)
 * and stops their per-event work; raising it builds them again. GPTPS_E_NOMEM when a
 * part the level needs cannot be allocated: the level is set, the dashboard runs
 * without that part (the TASKS table, the recent-events log, or the latency table and
 * so avg ms and the exact gauges), and a WARN says so. */
gptps_status gptps_tui_set_kpi(gptps_tui *t, gptps_tui_kpi level);
/* Resize the FULL-tier latency (handle) table (0 => 1024, capped at 1<<20 with a WARN).
 * Size it to the in-flight DEPTH you expect; see cfg.latency_window. Drops the entries in
 * flight and the counts of what the old window lost; the gauges are a lower bound ('>=')
 * until the work then in flight has ended. GPTPS_E_NOMEM (with a WARN) when the new
 * table cannot be allocated: the dashboard runs without avg ms until a later resize or
 * set_kpi succeeds. */
gptps_status gptps_tui_set_latency_window(gptps_tui *t, int handles);
gptps_status gptps_tui_set_mode(gptps_tui *t, gptps_tui_mode mode);
gptps_status gptps_tui_set_refresh(gptps_tui *t, uint32_t refresh_ms);

/* How the pending / in-flight gauges were obtained (gptps_tui_health.gauges, and the
 * mark before them on the frame). EXACT needs the handle table (FULL): from the events
 * alone it cannot be exact, because a FAILED / GPTPS_E_CANCELLED for an attempt that
 * ran and one for an item cancelled while still queued (or parked for a retry) carry
 * the same kind, status, attempt and flags - so below FULL every FAILED is taken to
 * leave the running set, and an item cancelled while queued undercounts in-flight
 * while others run. */
typedef enum {
    GPTPS_TUI_GAUGES_APPROX = 0,      /* from the counters ('~' on the frame) */
    GPTPS_TUI_GAUGES_EXACT,           /* from each handle's state (no mark) */
    GPTPS_TUI_GAUGES_LOWER_BOUND      /* exact for the handles tracked, but some may not be
                                       * ('>='): every new table starts here (install, a level
                                       * raised to FULL, a window resized - work queued before
                                       * it is not in it), and a handle that finds it full puts
                                       * it back. EXACT again only once the ENGINE reports
                                       * nothing queued or running while the table holds
                                       * nothing live - checked when a frame is rendered or
                                       * health is read; on an idle engine a new table is EXACT
                                       * at once. No event count can say it: an always-up
                                       * service ends every run with a FINISHED and no QUEUED,
                                       * so an engine that is never idle keeps '>=' (true) */
} gptps_tui_gauges;

/* What the dashboard has lost, holds and does (1.11). Counters are since install and
 * never reset; bytes are what is held now. A frame shows a "lost:" line under the KPI
 * line once types_folded, handles_untracked, frames_truncated or alloc_failures is
 * nonzero (orphans_expired is not a loss of anything shown: see below). */
typedef struct {
    size_t   struct_size;         /* = sizeof(gptps_tui_health_info); the call fills what it covers */
    /* losses */
    uint64_t journal_not_shown;   /* recent-event records overwritten before a frame showed them
                                   * (reserved: 0 until the 1.11 journals) */
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
    uint64_t bytes_tasks;         /* TASKS rows, name index, fold set (NORMAL and up) */
    uint64_t bytes_problems;      /* reserved for the 1.11 journals: 0 */
    uint64_t bytes_activity;      /* the recent-events log (NORMAL and up) */
    uint64_t bytes_handles;       /* the latency (handle) table (FULL) */
    uint64_t bytes_histogram;     /* reserved: 0 */
    uint64_t bytes_frame;         /* gptps_tui_run's frame buffer, while it runs */
    uint64_t bytes_dead_letters;  /* the dead-letter view's held items (array only) */
    /* the live loop */
    uint64_t frames_painted;      /* frames gptps_tui_run wrote */
    uint64_t frames_skipped;      /* reserved: 0 */
    uint64_t bytes_written;       /* what those frames wrote to the output, escapes included */
    uint64_t wakes;               /* times gptps_tui_run's loop woke (a key, or its timeout) */
    /* gauges, as the frame shows them */
    int      gauges;              /* gptps_tui_gauges */
    uint64_t pending, in_flight;
    uint64_t handles_tracked;     /* live entries in the latency table */
    uint64_t handles_markers;     /* markers in it (at most 64 + 1) */
    uint64_t task_rows;           /* TASKS rows in use, '(other)' not counted */
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

/* Blocking live loop: clear screen, then redraw every refresh_ms and dispatch
 * keystrokes until quit. No-op (returns immediately) unless both the output and stdin
 * are terminals - with a WARN when cfg.interactive asked for it anyway. Typed text is
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
