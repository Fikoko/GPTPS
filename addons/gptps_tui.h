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
 *   tier  enum (tui.kpi)          adds                                 per event         measured  memory it adds
 *   0     OFF      ("off")        leaves the event path: one flag      ~1 ns, no lock    2-3 ns    -
 *                                 read, no lock
 *   1     MINIMAL  ("minimal",    totals: queued, started, finished,   one lock +        7-9 ns    -
 *                   or "count")   failed, retried, dead, dropped,      increments
 *                                 cancelled
 *   2     GAUGE    ("gauge")      the in-flight gauge (and pending     + ~0-2 ns         9-10 ns   -
 *                                 beside it), its bar and peak
 *   3     PROBLEMS ("problems")   the PROBLEMS journal: FAILED,        0 on a success;   10 ns     recent_lines x ~300 B
 *                                 RETRIED, DEAD_LETTERED, DROPPED      with a failure: a           (2.4 KiB)
 *                                 with why (exit code / signal /       copy + a <= 512 B
 *                                 start error + stderr's last line)    scan
 *   4     TASKS    ("tasks")      the per-task TASKS rows ('(other)'   one hash probe    18-21 ns  (max_tasks + 1) x ~250 B
 *                                 row past max_tasks) and mem.peak                                 + index (~19 KiB)
 *   5     NORMAL   ("normal",     the ACTIVITY journal: QUEUED,        one 24 B store    18-24 ns  activity_lines x 24 B
 *                   or "activity") STARTED, FINISHED
 *   6     LATENCY  ("latency")    the handle table: avg ms queue to    one hash op       23-38 ns  32 B x next_pow2(4/3 x
 *                                 finish, EXACT pending / in-flight                                (window + 64)) (64 KiB)
 *   7     FULL     ("full")       latency percentiles p50/p95/p99      one increment     23-38 ns  256 B a row (16 KiB)
 *                                 (a log2-ms histogram per row)
 *
 * "measured": a QUEUED, STARTED, FINISHED mix of one task type, the observer called on
 * one thread (gcc -O2, x86-64 Linux, 4 cores), the defaults; memory in brackets at the
 * defaults. The low end is nothing in flight; the high end of LATENCY and FULL is 512
 * items in flight (the handle table no longer fits a cache line's reach). Cache misses,
 * not an algorithmic term, add more: ~50-70 ns at 500k in flight in a 1<<20 window, and
 * ~50-60 ns at 1000 task types. A FAILED event with a stderr tail at PROBLEMS and up costs
 * ~90-170 ns (the why copy and its bounded scan). With several threads emitting, MINIMAL and up also wait for one another on
 * the dashboard's one lock - the price of no atomics in the drop-in build; OFF never does.
 * No tier has a per-event term that grows with the window, the task types or a stderr
 * tail (tests/test_tui_perf.c gates each, as a ratio). A layer's memory exists only at its
 * tier and up (see STORAGE at install).
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
     * thread (1.12 measured lock stripes and did not ship them: through a real
     * engine the engine's own lock serializes first - docs/MEASUREMENTS.md,
     * "Dashboard lock stripes"). This tier is for leaving the dashboard installed after you are done
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

/* cfg.adapt / tui.adapt (1.12): what the dashboard does about its own PRESSURE - see
 * ADAPT at gptps_tui_health. 0 = the default. */
typedef enum {
    GPTPS_TUI_ADAPT_DEFAULT = 0,  /* = REPORT (also for a 1.11-layout caller) */
    GPTPS_TUI_ADAPT_OFF,          /* nothing is computed: no PRESSURE line, no WARN, no shedding */
    GPTPS_TUI_ADAPT_REPORT,       /* the PRESSURE line, a WARN per condition (at most one a
                                   * minute each) and the health fields; nothing is changed */
    GPTPS_TUI_ADAPT_DEGRADE       /* REPORT, and - opt-in - shed one collection step at a time
                                   * while the pressure lasts (STICKY: only an operator undoes it) */
} gptps_tui_adapt;

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
    int         pane_refresh_ms;/* how often an open tasks / settings / detail pane reads the engine
                                * again (0 => 2000; 100..600000; install-time) - or less often, when
                                * a read is slow: see PANES at gptps_tui_run */
    /* ---- 1.12: read only when struct_size covers them (0 = the default each) ---- */
    int         adapt;         /* gptps_tui_adapt (0 => REPORT; out of range => install returns NULL,
                                * with an ERROR). Also `tui.adapt`. See ADAPT at gptps_tui_health */
    int         cost_budget_pct;/* the observer's ESTIMATED cost, in percent of one core, above which
                                * it is a pressure condition (0 => 5; 1..100; else install returns
                                * NULL, with an ERROR). Also `tui.cost_budget_pct` */
    /* ---- 1.13: read only when struct_size covers them (0 = the default each) ---- */
    int         budget_kb;     /* a MEMORY budget for the dashboard, in KiB (1024 bytes): 0 => none (the
                                * default, as before 1.13); 1..2147483647; negative => install returns
                                * NULL, with an ERROR. A configuration whose footprint (see MEMORY at
                                * install) is over it is refused - install returns NULL with a WARN
                                * naming the figure needed. Also `tui.budget_kb`, gptps_tui_set_budget_kb */
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
 * again when the TASKS table is built again.
 *
 * MEMORY (1.13). Everything the dashboard allocates is one of three kinds, and
 * gptps_tui_health reports each exactly (tests/test_tui_budget.c holds the formula to the
 * allocator, byte for byte, at every tier):
 *  1. The FOOTPRINT (health.footprint_bytes): what the configuration holds - exact, from
 *     the sizes alone:
 *       footprint = D + U x ui_cap + payloads                         (the dashboard)
 *                 + [TASKS]    R x (max_tasks + 1) + 4 x max(8, next_pow2(2 x max_tasks)) + 2048
 *                 + [PROBLEMS] E x recent_lines
 *                 + [NORMAL]   A x activity_lines ((A + 64) x when there is no TASKS table)
 *                 + [LATENCY]  S x next_pow2((latency_window + 64) x 4/3 + 1)
 *                 + [FULL]     256 x (max_tasks + 1)
 *     a [part] counted only where it is held: at its tier and up (the tier that RUNS, the
 *     request minus what ADAPT shed), its pane shown (show_tasks / show_recent), a journal
 *     of more than 0 lines, the ACTIVITY journal not shed. ui_cap: the gptps_tui_add_task
 *     entries' array (8, then doubling, at most max_tasks; 0 before the first); payloads:
 *     the bytes of their payload copies. The struct sizes on x86-64 Linux (other targets
 *     differ a little - health has the exact figure on every one; tests/test_tui_budget.c
 *     holds these to the code): D = 7080 (the dashboard itself), U = 280 (an add_task
 *     entry), R = 264 (a TASKS row), E = 304 (a PROBLEMS record), A = 24 (an ACTIVITY
 *     record), S = 32 (a handle-table slot; 28 on i386). At the defaults (FULL,
 *     max_tasks 64, 8 + 8 journal lines, window 1024): 7080 + 19720 + 2432 + 192 + 65536 +
 *     16640 = 111600 bytes, 109 KiB.
 *  2. The BUFFERS (health.runtime_bytes) a frame is made in, held while they are used and
 *     grown as a frame needs: the dashboard frame's copy-out (the view: 13560 B, plus 560 B
 *     a row of the terminal's height - 208 B more with a source - kept once made), the open
 *     tasks / settings / detail pane's cache (bytes_panes), gptps_tui_run's two frame
 *     buffers and its output buffer (16 KiB to 256 KiB each, while it runs) and
 *     gptps_tui_snapshot's frame buffer (16 KiB up, during the call).
 *  3. Outside both, and outside the budget: the dead letters the dead-letter view HOLDS
 *     (bytes_dead_letters and their copies - the engine's items in the dashboard's keeping
 *     on an operator's key: refusing them memory would lose them), the copy of a hotkey's
 *     payload while its submit is made, and - while a part is rebuilt - the old part, freed
 *     as soon as the new one is in. The engine's own memory for the observer and the
 *     tui.* settings is the engine's.
 * BUDGET (cfg.budget_kb / tui.budget_kb / gptps_tui_set_budget_kb; 0 = none, the default).
 * Every change that would make the footprint of the REQUEST (the formula for what was asked
 * for, nothing shed - health.footprint_request_bytes) exceed the budget is REFUSED, and
 * changes nothing: install (NULL), gptps_tui_set_kpi, set_recent, set_latency_window,
 * add_task / add_task_ex (a new entry, a bigger payload), the 'm' key, a tui.kpi /
 * tui.recent / tui.activity / tui.latency_window write - each GPTPS_E_INVAL with one WARN
 * naming what it needs ("tui.kpi=full needs 109 KiB (111600 bytes: ...), over tui.budget_kb
 * 64 (65536 bytes): refused, nothing changed"), counted in health.budget_refusals and budget_need_bytes - and
 * so is a budget below the footprint already asked for. Never a silent clamp. A change that
 * does not grow the footprint is never refused. The BUFFERS are not refused - a frame is
 * always attempted - but they grow only while footprint + buffers stays within the budget:
 * past it a growth STOPS (health.budget_stops; a WARN once a minute naming the budget that
 * would fit): the frame is cut where its buffer ends ("[frame truncated]"), a pane's cache
 * reads fewer entries (it says so; one stop a pass - no growth is tried after it - and an
 * allocation that fails in the same pass is counted in alloc_failures too), a dashboard
 * frame with no room for its view says so, and gptps_tui_run / gptps_tui_snapshot without room for their first 16 KiB make no frame
 * (gptps_tui_run's output buffer, when it has no room, writes the frame whole, in pieces).
 * So leave room for them over the footprint: about 27 KiB for gptps_tui_render at 24 rows
 * (the view), 16 KiB more for gptps_tui_snapshot, 32 KiB more for gptps_tui_run (and its
 * output buffer, a little more than a frame), and more for a tall frame or an open pane.
 * With ADAPT (see gptps_tui_health): a STOP is a pressure condition (GPTPS_TUI_P_BUDGET) -
 * REPORT shows it and WARNs; DEGRADE sheds for it like NOMEM, as every tier step lowers the
 * footprint and so makes room for the buffers. A REFUSAL never sheds (it changed nothing),
 * and a shed never lowers the request, so a budget set under DEGRADE is checked against
 * the request ('u' must fit). Settings files: when the tui.* settings are DEFINED (at
 * install), a file's tui.budget_kb is applied last - it is the last tui key registered - so
 * the footprint the file sets is in place before its budget is checked against it. A
 * gptps_settings_reload applies the file's keys in the order the FILE lists them: a reload
 * that RAISES the footprint and the budget must list tui.budget_kb before the tui keys that
 * raise it, and one that LOWERS both must list it after them. In the other order the first
 * key is refused (an ERROR naming it, the reload returns GPTPS_E_CONFIG) and the rest are
 * applied - the old tier with the new budget, say; reload again, or set them in that order. */
gptps_tui *gptps_tui_install(gptps *e, const gptps_tui_config *cfg);

/* LOCAL per-task settings. `label` (NULL => task name) is shown in the table;
 * `hotkey` (0 => none) submits `payload` (len bytes, copied) for `task_name` when
 * pressed in the live loop / via gptps_tui_press. Safe to call before the task
 * type is registered, and at any KPI level (the bindings are kept at every level).
 * GPTPS_E_FULL past cfg.max_tasks names, or when the TASKS table exists and every row
 * is taken by the types its events brought; GPTPS_E_INVAL for a name longer than
 * GPTPS_TASK_NAME_MAX, or (1.13) when the entry - its room in the entries' array, its
 * payload's copy - would take the footprint past tui.budget_kb (a WARN says what it needs;
 * see BUDGET at gptps_tui_install). */
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
 * threads wait for the copy, not the formatting. The tasks, settings and detail panes
 * are formatted from their cache and read the engine only when a pass is due (see PANES
 * at gptps_tui_run). One render at a time per dashboard: a second thread's render waits
 * for the first. */
size_t gptps_tui_render(gptps_tui *t, char *buf, size_t cap);

/* Apply a key as if pressed: a bound hotkey submits its task (returns 1; if the engine
 * refuses the submit - the type is paused or unknown, the queue is full, a constraint
 * denies it - the toast says why and it returns 0); 'q'/ESC
 * requests quit (returns -1); 'k'/'j' scroll the log (returns 2); 'm' steps the
 * KPI tier up (FULL wraps to MINIMAL; never into OFF), 'u' restores the requested tier after a
 * degrade (see ADAPT at gptps_tui_health) and 'p' toggles pause (returns 3, "reconfigured"); 's' opens the
 * settings editor and '?' toggles the help overlay (returns 4, "UI"); else 0. In the
 * tasks, settings and detail panes 'R' (or 'r') reads the engine again now (returns 4;
 * see PANES at gptps_tui_run) - in the dead-letter view 'r' still re-submits all. */
int gptps_tui_press(gptps_tui *t, int key);

/* Runtime reconfiguration (thread-safe; the live loop picks changes up next frame).
 * Any gptps_tui_kpi value: DEFAULT => FULL, ACTIVITY => NORMAL, else GPTPS_E_INVAL.
 * Lowering the KPI tier frees the parts the new tier does not hold (see STORAGE)
 * and stops their per-event work; raising it builds them again. GPTPS_E_NOMEM when a
 * part the tier needs cannot be allocated: the tier is set, the dashboard runs
 * without that part (a journal, the TASKS table, the latency table and so avg ms and
 * the exact gauges, or the percentiles), and a WARN says so. It is the operator's word on
 * the tier: it clears what tui.adapt = degrade shed (see ADAPT at gptps_tui_health) and
 * becomes the requested tier, which tui.kpi reads back. 1.13: GPTPS_E_INVAL, nothing changed,
 * when the tier's footprint is over tui.budget_kb (see BUDGET at gptps_tui_install) - as for
 * gptps_tui_set_recent and gptps_tui_set_latency_window. */
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
    /* 1.11, the engine-reading panes (tasks, settings, detail; see PANES at gptps_tui_run) */
    uint64_t pane_reads;          /* passes they made over the engine's registry */
    uint64_t pane_read_us;        /* what the last pass took - in the engine's lock, call by call */
    uint64_t pane_every_ms;       /* how often an open pane reads again: pane_refresh_ms, or
                                   * 100 times the last pass when that is longer */
    uint64_t bytes_panes;         /* the open pane's cache (0 on the dashboard) */
    /* 1.12, ADAPT (see below) */
    int      adapt;               /* the gptps_tui_adapt in effect (never DEFAULT) */
    int      kpi_requested;       /* the tier asked for (set_kpi, tui.kpi, 'm', cfg.kpi); `kpi`
                                   * above is what runs: lower only while something is shed */
    uint64_t pressure;            /* GPTPS_TUI_P_* bits: the conditions of the last window */
    uint64_t pressure_seen;       /* GPTPS_TUI_P_* bits: every condition any window has had */
    uint64_t pressure_windows;    /* windows in a row with a condition shedding relieves
                                   * (GPTPS_TUI_P_SHEDDABLE); DEGRADE sheds at 3 */
    uint64_t windows;             /* windows evaluated since install */
    uint64_t pressure_warns;      /* pressure / degrade WARNs logged */
    uint64_t shed;                /* GPTPS_TUI_SHED_* bits: what DEGRADE has shed (0: nothing) */
    uint64_t shed_pressure;       /* GPTPS_TUI_P_* bits: the conditions behind the last step (1.13:
                                   * those of the window that the step relieves) */
    uint64_t degrade_steps;       /* steps shed since install (cleared ones included) */
    uint64_t event_rate;          /* events a second in the last window (from the totals) */
    uint64_t cost_est_ppm;        /* the observer's ESTIMATED cost in the last window, in
                                   * millionths of one core: event_rate x the unit cost of the
                                   * tier in effect (see ADAPT) - an estimate, not a measurement */
    uint64_t render_max_us;       /* the slowest frame made in the last window (render, snapshot
                                   * or the live loop; any pane) */
    /* 1.12, SOURCE (see gptps_tui_set_source) */
    int      source;              /* 1 while a host source is set */
    int      source_status;       /* what its last read returned (GPTPS_OK before any) */
    uint64_t source_reads;        /* reads made (one a dashboard frame), since install */
    uint64_t source_errors;       /* reads that failed, since install: those frames showed the
                                   * dashboard's own counts, and said so */
    uint64_t source_rows;         /* task rows the last good read said the source has */
    /* 1.13, ADAPT (see below) */
    uint64_t pane_shed_ms;        /* the engine-reading panes' interval floor GPTPS_TUI_SHED_PANE
                                   * set (0: not shed; pane_refresh_ms is the floor then) */
    uint64_t shed_pressure_all;   /* GPTPS_TUI_P_* bits: the conditions behind every step shed
                                   * since an operator last cleared it (the DEGRADED line's) */
    /* 1.13, MEMORY and BUDGET (see gptps_tui_install) */
    uint64_t budget_bytes;        /* tui.budget_kb x 1024 (0: no budget) */
    uint64_t footprint_bytes;     /* the footprint held now: the formula for what runs (less a
                                   * part an allocation failure left out) */
    uint64_t footprint_request_bytes; /* the formula for the request, nothing shed: what every
                                   * change is checked against */
    uint64_t runtime_bytes;       /* the frame buffers, view and pane cache held now */
    uint64_t budget_refusals;     /* changes refused as over the budget, since install */
    uint64_t budget_stops;        /* buffer growths stopped at the budget, since install */
    uint64_t budget_need_bytes;   /* the footprint the last refused change needed (0: none) */
} gptps_tui_health_info;

/* GPTPS_TUI_P_*: a pressure condition, one bit each (gptps_tui_health_info.pressure). */
#define GPTPS_TUI_P_PROBLEMS_LOST 0x001u  /* PROBLEMS records overwritten before a frame showed them */
#define GPTPS_TUI_P_ACTIVITY_LOST 0x002u  /* ACTIVITY records lost unseen, 1000 a second or more */
#define GPTPS_TUI_P_UNTRACKED     0x004u  /* QUEUEDs the full latency table could not take */
#define GPTPS_TUI_P_FOLDED        0x008u  /* new task types folded into '(other)' */
#define GPTPS_TUI_P_TRUNCATED     0x010u  /* frames cut at 256 KiB */
#define GPTPS_TUI_P_NOMEM         0x020u  /* allocations that failed */
#define GPTPS_TUI_P_RENDER        0x040u  /* a frame took more than a tenth of refresh_ms to make */
#define GPTPS_TUI_P_PANE          0x080u  /* an engine read was slow enough to stretch its interval */
#define GPTPS_TUI_P_COST          0x100u  /* the estimated observer cost is over cost_budget_pct */
#define GPTPS_TUI_P_BUDGET        0x200u  /* 1.13: a buffer's growth stopped at tui.budget_kb */
/* The conditions a DEGRADE step relieves - the ones DEGRADE acts on (see ADAPT below for
 * which step relieves which). 1.12: ACTIVITY_LOST | RENDER | COST (0x142). 1.13 adds
 * UNTRACKED, TRUNCATED (dashboard frames only), NOMEM, PANE and BUDGET (0x3f6). It is a
 * macro: a caller built against 1.12's header still sees 0x142; what the dashboard acts on
 * is the library's. */
#define GPTPS_TUI_P_SHEDDABLE (GPTPS_TUI_P_ACTIVITY_LOST | GPTPS_TUI_P_UNTRACKED | GPTPS_TUI_P_TRUNCATED | \
                               GPTPS_TUI_P_NOMEM | GPTPS_TUI_P_RENDER | GPTPS_TUI_P_PANE | GPTPS_TUI_P_COST | \
                               GPTPS_TUI_P_BUDGET)
/* 1.13: the conditions no step relieves - reported, never acted on (see ADAPT for why) */
#define GPTPS_TUI_P_REPORT_ONLY (GPTPS_TUI_P_PROBLEMS_LOST | GPTPS_TUI_P_FOLDED)
/* GPTPS_TUI_SHED_*: a DEGRADE step (gptps_tui_health_info.shed). The bits keep their 1.12
 * values; the ORDER steps are considered in is PANE, ACTIVITY, PERCENTILES, LATENCY, TASKS. */
#define GPTPS_TUI_SHED_ACTIVITY    0x1u   /* 2: the ACTIVITY journal off ("-activity") */
#define GPTPS_TUI_SHED_PERCENTILES 0x2u   /* 3: FULL -> LATENCY, the histograms ("-percentiles") */
#define GPTPS_TUI_SHED_LATENCY     0x4u   /* 4: the handle table: avg ms, exact gauges ("-latency"):
                                           * -> NORMAL while the ACTIVITY journal runs, else TASKS */
#define GPTPS_TUI_SHED_TASKS       0x8u   /* 5: -> PROBLEMS, the TASKS rows and mem.peak ("-tasks") */
#define GPTPS_TUI_SHED_PANE        0x10u  /* 1 (1.13): the panes read the engine half as often ("-pane");
                                           * no tier changes and nothing is lost but freshness */

/* Fill *out (struct_size bytes of it; the rest of a newer struct is left alone, and
 * out->struct_size is kept). GPTPS_E_INVAL for a NULL argument or a struct_size that
 * does not reach journal_not_shown. Thread-safe; takes the dashboard's lock briefly, and
 * while the gauges are a lower bound it asks the engine (gptps_engine_counts, O(1) since 1.12)
 * whether it is idle, on every call (not held to the frames' 1%), and once a rate window
 * it evaluates ADAPT (below) - with DEGRADE that may shed a step, freeing memory - so it may be called from an observer, which runs with the engine lock
 * released, but not from a callback gptps.h (THREADING) forbids to call the engine. */
gptps_status gptps_tui_health(gptps_tui *t, gptps_tui_health_info *out);

/* ADAPT (1.12). The dashboard watches its own cost and losses - with nothing added to the
 * event path: every condition is computed from counters it already keeps, once a rate
 * window (cfg.rate_window_s, 10 s), when a frame is made (render, snapshot, the live loop)
 * or health is read. With no viewer and no health call nothing is computed at all. A
 * window's conditions (GPTPS_TUI_P_*), each from what changed in that window:
 *   PROBLEMS_LOST  PROBLEMS records lost unseen          -> raise tui.recent (report-only)
 *   ACTIVITY_LOST  ACTIVITY records lost unseen at 1000/s -> tui.activity=0 (a log
 *                  or more (a log scrolling too fast to     scrolling that fast is not read)
 *                  read; a few lost between frames are not pressure)
 *   UNTRACKED      items the latency table had no room for -> raise tui.latency_window
 *   FOLDED         types folded into '(other)'             -> raise cfg.max_tasks (report-only)
 *   TRUNCATED      frames cut at 256 KiB                   -> lower tui.recent / tui.activity
 *   NOMEM          failed allocations                      -> a lower tui.kpi
 *   RENDER         the slowest frame took more than 10% of  -> raise tui.refresh_ms
 *                  refresh_ms to make
 *   PANE           a pane pass (or the idle check) slow      -> raise cfg.pane_refresh_ms, or
 *                  enough to stretch its interval past        leave the pane
 *                  cfg.pane_refresh_ms (the 1% rule)
 *   COST           the ESTIMATED observer cost over         -> the highest tui.kpi that fits
 *                  cost_budget_pct (5% of one core)
 *   BUDGET         a buffer's growth stopped at            -> raise tui.budget_kb, or a lower
 *                  tui.budget_kb (1.13; see BUDGET)           tui.kpi
 * The cost is an estimate, labelled "est" wherever it is shown: the event rate (from the
 * totals) times a unit cost per tier - OFF 3, MINIMAL 9, GAUGE 10, PROBLEMS 10, TASKS 21,
 * NORMAL 24, LATENCY 38, FULL 38 ns (35 at LATENCY / FULL with the ACTIVITY journal off),
 * the upper end measured on one thread (gcc -O2, x86-64, the KPI table above;
 * tests/test_tui_perf.c's harness). It cannot be timed: gptps_now_ms has millisecond
 * resolution and an event takes nanoseconds. Contention between emitting threads is not in
 * it. At the default 5% FULL reaches it at about 1.3 million events a second.
 * REPORT (the default): any condition shows a PRESSURE line on the dashboard naming it and
 * the setting to change ("PRESSURE: ACTIVITY lost 41k unseen: tui.activity=0"), logs a WARN
 * - at most once a minute per condition - and sets the health fields. Nothing is changed.
 * DEGRADE (opt-in; the one exception to "report, never act", and it acts only when a frame
 * is made or health is read): when conditions a step relieves (GPTPS_TUI_P_SHEDDABLE) last
 * 3 windows in a row - any of them, in each window - one step is shed: of the steps, in this
 * fixed order, the FIRST that would change what runs and relieves one of the last window's
 * conditions (1.12 took the next one in the order, whatever it relieved):
 *   step (GPTPS_TUI_SHED_*)    relieves
 *   PANE        "-pane"        PANE: the tasks / settings / detail panes' interval (what runs,
 *                              pane_every_ms) doubled, up to 60 s - their hold on the engine's
 *                              lock halved. Repeatable while under 60 s; no data is lost
 *   ACTIVITY    "-activity"    ACTIVITY_LOST, COST (3 ns of the estimate), RENDER and TRUNCATED
 *                              (the RECENT rows a frame merges and prints), NOMEM and BUDGET
 *                              (its memory)
 *   PERCENTILES "-percentiles" RENDER (the per-row p50/p95/p99), NOMEM, BUDGET - not COST: FULL
 *                              and LATENCY estimate the same 38 ns
 *   LATENCY     "-latency"     UNTRACKED (the table is gone), COST (38 -> 24 ns), NOMEM, BUDGET;
 *                              it also takes the percentiles, and runs NORMAL while the ACTIVITY
 *                              journal does (TASKS once it is shed, as in 1.12)
 *   TASKS       "-tasks"       COST, RENDER, TRUNCATED (the TASKS rows), NOMEM, BUDGET; it also
 *                              takes what runs above PROBLEMS (journal, percentiles, table)
 * TRUNCATED acts only for dashboard frames (a frame is rows of at most the width, so only
 * fewer rows shorten it; a frame cut on another pane is reported only), and only for those
 * cut after the header, where the rows begin, in any buffer (run's / snapshot's, or a
 * host's gptps_tui_render one): a frame whose header alone did not fit - a host's small
 * render buffer (a status widget), a title longer than the frame - is reported, never shed
 * for, as no step shortens the header. NOMEM counts a failed allocation
 * anywhere in the dashboard (journals, tables, the frame buffer, the tasks / settings pane
 * caches): every step but PANE frees heap, which a failing allocation anywhere can use, so
 * failed allocations shed down the ladder, one step per 3 windows, until they stop (a
 * step's rebuild that fails again is counted and logged, so it keeps counting). Report-only, never shed for: PROBLEMS_LOST - PROBLEMS is the floor, and no step
 * changes how many failures arrive (raise tui.recent, or look more often); FOLDED - folding
 * into '(other)' is already the cheap fallback (the totals stay exact) and the only step that
 * ends it, -tasks, drops every row to save merging a few (raise cfg.max_tasks). Their
 * PRESSURE text says "report-only" and why. A step that would change nothing (at a lower
 * request, or PANE at 60 s) is passed over; after a step the count starts again. PROBLEMS is
 * the floor: the totals, the gauges and the PROBLEMS journal are never shed; when nothing
 * left relieves the conditions, a WARN says so (at most once a minute). Each step logs a
 * WARN naming all it takes out. What was asked for is kept: tui.kpi, tui.activity and
 * set_kpi's level read back as requested (so a file saved with 'w' holds the request, not
 * the degraded state); what runs is the request minus what is shed - health.kpi,
 * health.shed, health.pane_shed_ms, and the line under the header: "DEGRADED: -activity
 * -percentiles (pressure: render) - 'u' restores full" (the conditions behind every step;
 * health.shed_pressure has the last step's). STICKY: nothing is restored when the pressure
 * goes. It is cleared only by an operator. The request is built again (and the pane interval
 * back to pane_refresh_ms) by gptps_tui_set_kpi (any level, the same one too), a tui.kpi
 * write, the 'u' key, gptps_tui_set_adapt or a tui.adapt write; the 'm' key clears it too,
 * but steps up one tier from what RUNS (FULL degraded to PROBLEMS, then 'm': TASKS). */
gptps_status gptps_tui_set_adapt(gptps_tui *t, gptps_tui_adapt mode);   /* DEFAULT => REPORT; else
                                   * GPTPS_E_INVAL. Clears what DEGRADE shed (above) */
/* The estimated-cost budget, in percent of one core: 1..100 (else GPTPS_E_INVAL). Also
 * `tui.cost_budget_pct`. */
gptps_status gptps_tui_set_cost_budget(gptps_tui *t, int pct);
/* The memory budget (1.13), in KiB: 0 = none, 1..2147483647; else - or below the footprint the
 * request holds - GPTPS_E_INVAL with a WARN naming the footprint, and nothing changed. See
 * MEMORY and BUDGET at gptps_tui_install. Also `tui.budget_kb`. */
gptps_status gptps_tui_set_budget_kb(gptps_tui *t, int kb);

/* SOURCE (1.12). A host that already aggregates - it runs gptps_stats, say - can let the
 * dashboard SHOW its numbers instead of collecting its own: the header's totals and gauges
 * (and so done/s and peak) and the TASKS rows then come from the source, and the dashboard's
 * own collection can be turned down to what the source does not give (tui.kpi = off, or
 * problems for the failure journal). The interface is read-only: one callback that fills
 * a snapshot. There is no reset and no close in it, so the dashboard can never reset or
 * close what the host owns - not on set_source, not on any key or setting, not in
 * gptps_tui_close.
 *
 * WHEN it is read: once per dashboard frame (gptps_tui_render, gptps_tui_snapshot, the live
 * loop) on the thread making it, just before the frame's copy-out, with the dashboard's own
 * lock released (only its render lock is held, so the callback may take the host's locks and
 * may call gptps_tui_health; it must not render, snapshot or set_source this dashboard).
 * NEVER on the event path: events cost what tui.kpi says, and nothing for the source. The
 * other panes (tasks, settings, detail, dead letters) do not read it. While a source is set
 * the live loop polls it: a frame is due every refresh_ms (it writes only lines that changed).
 *
 * ERRORS: a read that returns anything but GPTPS_OK is counted (gptps_tui_health
 * source_errors, source_status), that frame shows the dashboard's own counts with a line
 * saying so ("source <name>: read failed (...), N errors - own counts shown"), and a WARN is
 * logged - at most once a minute. The next frame reads again.
 *
 * OWNERSHIP: ctx and the callback stay the host's. The dashboard copies the gptps_tui_source
 * (and its name) at set_source; once gptps_tui_set_source (another source, or NULL) or
 * gptps_tui_close has returned, the old one is never called again, so the host may then
 * free it. Close order: gptps_shutdown, gptps_tui_close (or set_source(t, NULL)), then the
 * host's own (gptps_stats_close). No link dependency: the dashboard knows nothing of
 * gptps_stats; examples/dashboard_stats.c is the adapter (gptps_stats_snapshot: totals and
 * every row under one lock). */
typedef struct {
    size_t   struct_size;         /* set by the dashboard; the source fills what it covers */
    uint64_t queued, started, finished, failed, retried, dead, dropped;
    uint64_t cancelled;           /* FAILED with GPTPS_E_CANCELLED (part of failed) */
    uint64_t pending, in_flight;  /* the gauges */
    int      gauges;              /* gptps_tui_gauges: EXACT (no mark), LOWER_BOUND ('>='), APPROX
                                   * ('~'), or SUM (no gauges: `pending` is what is queued or
                                   * running, as one sum). Anything else is shown as APPROX */
} gptps_tui_source_totals;

#define GPTPS_TUI_SOURCE_NAME    128      /* a row's name, with its NUL (= GPTPS_TASK_NAME_MAX + 1) */
#define GPTPS_TUI_SRC_ROW_LATENCY 0x1u    /* flags: lat_* hold this row's latency */
typedef struct {
    size_t   struct_size;         /* the dashboard sets row_size instead; ignored */
    uint32_t flags;               /* GPTPS_TUI_SRC_ROW_* */
    char     name[GPTPS_TUI_SOURCE_NAME]; /* the task type; a gptps_tui_add_task label and hotkey
                                   * for that name are shown with it. Cut at its last byte */
    uint64_t started, finished, failed, dead, dropped;
    uint64_t lat_n, lat_sum_ms, lat_max_ms;  /* avg ms and max ms (the source says what latency
                                   * it is: the stats adapter's is run time); lat_n 0: "--" */
} gptps_tui_source_row;

/* Fill *totals (struct_size bytes of it) and rows[0 .. min(cap, rows there are)) - an array
 * of `cap` elements of `row_size` bytes each, row_size >= the 1.12 sizeof(gptps_tui_source_row);
 * fill each row only as far as row_size covers - and set *nrows to the rows there are (more
 * than cap: the frame says "+N more"). cap is what the frame has room for (at most its height).
 * Totals and rows should be one consistent snapshot. Return GPTPS_OK, or any error (above). */
typedef gptps_status (*gptps_tui_source_read_fn)(void *ctx, gptps_tui_source_totals *totals,
                                                 gptps_tui_source_row *rows, size_t row_size,
                                                 size_t cap, size_t *nrows);
typedef struct {
    size_t                   struct_size; /* = sizeof(gptps_tui_source) */
    void                    *ctx;         /* the host's: passed to read, never freed */
    gptps_tui_source_read_fn read;        /* required */
    const char              *name;        /* shown on the kpi line ("source:<name>"; NULL =>
                                           * "host"); copied, up to 31 bytes */
} gptps_tui_source;

/* Set (src) or clear (NULL: the dashboard's own collection again) the source. GPTPS_E_INVAL
 * for a NULL t, or a src with a struct_size below this layout's or no read. Waits for a frame
 * being made to finish (see OWNERSHIP). done/s and peak start again from the next frame
 * (the counts they are made from changed hands); the error and read counts are kept. */
gptps_status gptps_tui_set_source(gptps_tui *t, const gptps_tui_source *src);

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
 * a toast's expiry (in every pane; PAUSED is frozen, so its toast stays until the next key); the clock's next visible tick (cfg.clock: once a minute
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
 * PANES (1.11). The tasks, settings and detail panes show what they read from the engine,
 * one call per task type or setting, each in the engine's lock. Before ABI 2.9 (1.12) a
 * gptps_task_get_info call walked every queue to count - ~1 ms a call at 64 types and 100k
 * queued, 116 ms for a 1.10 frame that read every row - and 1.10 read every row on every
 * frame (46% of the time at 250 ms, stalling submit and dispatch); since 2.9 it reads
 * counters the engine keeps, O(1) a call, so a pass costs microseconds a type. The rule
 * below stays, as a bound that now rarely binds. An open pane reads the engine in ONE pass
 * into a cache: when it opens, after a change made from it (an edit, pause, clone, new, delete),
 * on 'R', and then every pane_refresh_ms (2 s) - or, when a pass took more than 1% of that,
 * 100x what it took, so the panes hold the engine's lock at most 1% of the time. Its frames
 * and j/k read only the cache (no engine call between passes), and a value set elsewhere
 * shows at the next pass. The pane's last line says so: "core counts: 19.0 ms, every 2.0 s,
 * age 1 s" (tasks), "settings read: ..." (settings, detail). One pass is one call per task
 * type or setting (1.10: two or three walks of the registry a frame); the selected setting
 * is read again, once, when the selection moves, to explain it, and an edit reads its
 * setting fresh by key, so it lands on the setting chosen even when the registry changed
 * since the pass. The cache is freed when the dashboard pane is back (bytes_panes in
 * gptps_tui_health, with pane_reads, pane_read_us and pane_every_ms). The dashboard's own
 * engine read - asking whether the engine is idle, to clear the gauges' '>=' - follows the
 * same 1% rule: each look is timed and the next waits 99x as long, however often frames
 * are rendered (a frame is then due when the hold ends, so an engine gone idle clears
 * '>=' then). gptps_tui_health, install and a tier change always look. Since 1.12 the look
 * is one gptps_engine_counts call (ABI 2.9, O(1)) instead of a gptps_task_get_info per type.
 *
 * HOST PROFILES.
 *   - Embedded / MANUAL (no thread; render or snapshot only): no loop, no pipe, no timer -
 *     done/s, peak and the clock advance inside render. Pick the tier by what you can
 *     spare per event - MINIMAL (totals) or GAUGE (+ in-flight and peak), which hold no
 *     per-event memory; with the stub HAL's clock, cfg.clock = OFF.
 *   - A server at 100k events/s: every tier is O(1) an event (23-38 ns at FULL on one
 *     thread: ~0.2-0.4% of a core), but MINIMAL and up take one mutex an event on every thread
 *     that emits. Prefer ON_DEMAND (at most one wake a frame interval); size
 *     latency_window to the in-flight depth, not the rate; activity_lines = -1 (or
 *     tui.activity = 0) when the lifecycle log is not read - failures stay in PROBLEMS.
 *     The tasks pane is safe to leave open (<= 1% of the engine's lock).
 *   - Task Scheduler / a service / headless: run returns at once (no console) and makes
 *     no pipe; only the observer's cost remains - set kpi to what snapshots need, or OFF
 *     (installed, ~2 ns an event, no lock; raise it when someone looks).
 *   - An operator's console: the defaults (FULL, CONTINUOUS, clock in minutes) - idle, it
 *     writes nothing and repaints the header once a minute; ON_DEMAND to also make no
 *     frame between changes.
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
