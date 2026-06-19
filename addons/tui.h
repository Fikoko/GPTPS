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

typedef struct {
    size_t      struct_size;   /* = sizeof(gptps_tui_config) */
    uint32_t    refresh_ms;    /* redraw interval in the live loop (0 => 250) */
    int         color;         /* -1 = auto (TTY), 0 = off, 1 = on */
    int         interactive;   /* -1 = auto (TTY), 0 = off, 1 = on (read hotkeys) */
    int         max_recent;    /* recent-event lines to keep/show (0 => 8, cap 64) */
    int         show_tasks;    /* per-task table: >=0 show (default), <0 hide */
    int         show_recent;   /* recent-events log: >=0 show (default), <0 hide */
    const char *title;         /* dashboard title (default "tasks") */
    FILE       *out;           /* output stream (NULL => stdout) */
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

/* Apply a key as if pressed: a bound hotkey submits its task (returns 1), 'q' or
 * ESC requests quit (returns -1), anything else is ignored (returns 0). */
int gptps_tui_press(gptps_tui *t, int key);

/* Blocking live loop: clear screen, then redraw every refresh_ms and dispatch
 * keystrokes until quit. No-op (returns immediately) when not attached to a TTY. */
void gptps_tui_run(gptps_tui *t);

/* Restore the terminal and free. Call AFTER gptps_shutdown(e). */
void gptps_tui_close(gptps_tui *t);

#ifdef __cplusplus
}
#endif
#endif /* GPTPS_TUI_H */
