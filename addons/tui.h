/*
 * tui.h - a real-time terminal dashboard for GPTPS (optional, portable add-on).
 *
 * Attaches to an engine via the observer seam and shows live task activity:
 * cumulative counts (queued/started/finished/failed/retried/dead), in-flight
 * gauge, a per-task table, and a scrolling recent-events log. Fully portable -
 * ANSI/VT escapes only (no ncurses), with Windows VT enabled automatically.
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
 * gptps_tui_close(t)  (close AFTER shutdown; the engine has no unregister-observer).
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
    GPTPS_TUI_KPI_MINIMAL,     /* cumulative counts only - cheapest, ~no per-event work */
    GPTPS_TUI_KPI_NORMAL,      /* + per-task table + recent-events log */
    GPTPS_TUI_KPI_FULL         /* + per-handle latency tracking (allocates a ring) */
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
    int         max_recent;    /* recent-event lines to keep/show (0 => 8, cap 64) */
    int         show_tasks;    /* per-task table: >=0 show (default), <0 hide */
    int         show_recent;   /* recent-events log: >=0 show (default), <0 hide */
    int         kpi;           /* gptps_tui_kpi: how much to compute (0 => FULL) */
    int         mode;          /* gptps_tui_mode: live-loop cadence (0 => CONTINUOUS) */
    int         latency_window;/* handles tracked for latency at FULL (0 => 1024) */
    const char *title;         /* dashboard title (default "tasks") */
    FILE       *out;           /* output stream (NULL => stdout) */
    const char *settings_path; /* where the Settings pane's 'w' (save) writes (NULL => engine's open path) */
} gptps_tui_config;

/* Install the dashboard on engine `e` (registers an observer). cfg NULL => all
 * defaults. Returns NULL on allocation failure. */
gptps_tui *gptps_tui_install(gptps *e, const gptps_tui_config *cfg);

/* LOCAL per-task settings. `label` (NULL => task name) is shown in the table;
 * `hotkey` (0 => none) submits `payload` (len bytes, copied) for `task_name` when
 * pressed in the live loop / via gptps_tui_press. Safe to call before the task
 * type is registered. */
gptps_status gptps_tui_add_task(gptps_tui *t, const char *task_name, const char *label,
                                int hotkey, const void *payload, size_t len);

/* Render one frame into `buf` (NUL-terminated, truncated to cap). Returns the
 * length written (excluding NUL). Pure read of live state - no terminal needed. */
size_t gptps_tui_render(gptps_tui *t, char *buf, size_t cap);

/* Apply a key as if pressed: a bound hotkey submits its task (returns 1); 'q'/ESC
 * requests quit (returns -1); 'k'/'j' scroll the log (returns 2); 'm' cycles the
 * KPI level and 'p' toggles pause (returns 3, "reconfigured"); else 0. */
int gptps_tui_press(gptps_tui *t, int key);

/* Runtime reconfiguration (thread-safe; the live loop picks changes up next frame).
 * Lowering the KPI level frees the latency ring and stops the per-event work;
 * raising it to FULL re-allocates it. */
gptps_status gptps_tui_set_kpi(gptps_tui *t, gptps_tui_kpi level);
gptps_status gptps_tui_set_mode(gptps_tui *t, gptps_tui_mode mode);
gptps_status gptps_tui_set_refresh(gptps_tui *t, uint32_t refresh_ms);

/* Render one frame to the configured output stream right now ("once" / on-demand),
 * independent of the live loop or mode. */
void gptps_tui_snapshot(gptps_tui *t);

/* Blocking live loop: clear screen, then redraw every refresh_ms and dispatch
 * keystrokes until quit. No-op (returns immediately) when not attached to a TTY. */
void gptps_tui_run(gptps_tui *t);

/* Restore the terminal and free. Call AFTER gptps_shutdown(e). */
void gptps_tui_close(gptps_tui *t);

#ifdef __cplusplus
}
#endif
#endif /* GPTPS_TUI_H */
