# Changelog

All notable changes to GPTPS are recorded here. Format follows
[Keep a Changelog](https://keepachangelog.com/); the project aims for semantic
versioning once it reaches 1.0.

## [Unreleased]

### Added — runtime task management + generic settings (control plane)
- **Task lifecycle API** turns the registry into a live control surface:
  - `gptps_unregister_task(e, name, flags)` removes a task type at runtime with a
    chosen policy — `GPTPS_REMOVE_REJECT_IF_BUSY` (default; fails `E_BUSY` if work
    is queued/in-flight), `GPTPS_REMOVE_DRAIN` (tombstone, let queued + in-flight
    finish without retries, then free), or `GPTPS_REMOVE_CANCEL` (drop queued,
    cooperatively cancel in-flight, then free). THREADED mode blocks until the
    drain/cancel completes; MANUAL mode (no in-flight work between `gptps_step`s)
    drains by stepping first, or CANCEL drops the backlog. A removed name is free
    to re-register, its `tasks.<name>.*` settings are torn down, and retained
    dead-letter items survive (their name still resolves after the type is gone).
  - `gptps_task_count` / `gptps_task_get_info` / `gptps_task_exists` enumerate the
    registry (name, exec kind, priority, cost, policy, enabled/draining state, and
    live queued/running/dead counts) — the introspection the TUI renders from.
  - `gptps_set_task_enabled` pauses/resumes a type reversibly (rejects new submits
    while keeping its config and stats).
  - `gptps_clone_task` duplicates a type under a new name (shares run/exec/argv,
    copies cost+policy+priority, re-layers `[tasks.<dst>]` config) — the "tweak a
    copy" operation.
- **Generic settings without per-key glue:**
  - `gptps_define_global` registers an engine-stored, typed, validated global knob
    under any dotted key (round-trips through TOML, editable in the settings pane).
  - `gptps_define_task_setting` registers a per-task schema materialized as
    `tasks.<name>.<leaf>` on every task (existing + future), each instance carrying
    its own value; `gptps_task_setting_int` / `gptps_task_setting_str` read this
    task's resolved value from inside an in-process `run()`.
  - Both validate by type with `"min..max"` ranges and `"a|b|c"` enum choices.
- **Host-table ABI** grows by four routines (`unregister_task`, `task_exists`,
  `define_global`, `define_task_setting`) so add-ons share the control plane.
- **Terminal control plane (`tui` add-on):** a **task manager** pane (list with
  live counts; inspect → per-task settings editor; pause/resume; clone; create a
  `GPTPS_EXEC_PROGRAM` task from a typed name + argv; delete with a confirm dialog
  showing the queued/in-flight count, drain or cancel-force) and a **dead-letter**
  pane (bulk re-submit / discard). New dashboard keys `t` (tasks) and `l` (dead
  letter). All still pure render-to-string + headless-testable.
- ABI minor 7 → 8 (additive). New status `GPTPS_E_BUSY`.

### Added — portability: single-threaded / embeddable execution
- **MANUAL execution mode** (`gptps_config.mode = GPTPS_RUN_MANUAL`): the engine
  spawns **no threads** and is driven cooperatively by the caller via the new
  `gptps_step()` pump, which runs runnable tasks to completion on the calling
  thread. Needs only the HAL mutex/clock/flag primitives — never
  `gptps_thread_start`/`cond_wait` — so it ports to single-threaded hosts and
  bare-metal. The threaded dispatcher and the manual pump share one `engine_pass()`
  (admission/retry/dead-letter logic), so scheduling semantics are identical.
  ABI minor 5 → 6 (additive). Threaded engines reject `gptps_step` with `E_INVAL`.
- **Allocator hook** (`gptps_set_allocator`): redirect *all* core allocation
  process-wide to a custom `malloc`/`realloc`/`free` (e.g. a static pool on a host
  with no libc heap), SQLite-style. Defaults to the C library; pass `NULL` to reset.
  Covers the portable core (engine, settings, config, executors); the HAL manages
  its own memory (replace it for exotic RAM). ABI minor 6 → 7 (additive).
- **`examples/embedded.c`**: GPTPS with **no worker threads and no libc heap** —
  MANUAL mode + a static-arena allocator — the bare-metal shape end to end.

### Changed — friendlier terminal dashboard (`tui` add-on)
- **Discoverability:** a `?` **help overlay** documenting every key, and a complete
  inline legend so `s`/`m`/`p`/`j`/`k` are no longer hidden.
- **Action feedback:** a transient toast confirms actions ("submitted Work",
  "paused", "kpi -> full").
- **Adaptive layout:** the dashboard reads the terminal size (`TIOCGWINSZ` /
  `GetConsoleScreenBufferInfo`, fallback 80×24) and scales the gauge, fits the
  recent-log to the window height, and spans the title bar/rule to width.
- **Polish:** a framed title bar, flicker-free redraw (per-line erase instead of a
  full-screen clear), a Unicode block gauge with ASCII fallback, and semantic color
  (ok% green/yellow/red). New `gptps_tui_config.unicode` (-1 auto / 0 ASCII / 1 on).
  All changes preserve the pure render-to-string model and stay headless-testable.

## [0.2.0] - 2026-06-21

A unified, runtime, persistable **settings subsystem** layered over the existing
config — every knob (core, per-task, add-on) is now introspectable, validated,
editable live, savable, and watchable from one API. ABI minor 3 → 5 (additive).

### Added — settings registry
- Typed **registry**: introspection (`gptps_settings_count` /
  `gptps_settings_get_info`), validated string get/set (`gptps_settings_get` /
  `gptps_settings_set`), and `gptps_register_setting` — schema + accessor binding,
  so the live engine/add-on state stays the single source of truth (no drift).
- Dotted keys over core (`limits.*`, `scheduler.*`), per-task (`tasks.<name>.*`),
  and add-on (`tui.*`, `gpu_quota.*`) settings; per-setting `hot` vs restart-only.
- **Validation** the raw TOML path lacked: bad enum / out-of-range / wrong type are
  rejected with `GPTPS_E_CONFIG` instead of being silently ignored.

### Added — persistence
- `gptps_settings_save` / `gptps_settings_reload` round-trip, with a portable
  atomic-replace HAL primitive (`gptps_hal_atomic_replace`: `rename` / `MoveFileEx`).

### Added — extensibility & UI
- `register_setting` host-table routine (append-only) so dlopen'd add-ons register
  their own settings; the `tui` and `gpu_quota` add-ons register theirs.
- A live **Settings pane** in the `tui` dashboard (`s`): browse / edit / save at runtime.
- `gptps_settings_watch` change-watch callback — react to live edits (audit / auto-save).

## [0.1.0] - 2026-06-19

First tagged release: a complete, embeddable C99 general-purpose task processor,
tested under CTest + ASan/UBSan + ThreadSanitizer on Linux, macOS, and Windows.

### Core
- Single-writer dispatcher + worker pool; one mutex guards shared state, atomics
  confined to the HAL.
- Declared-cost-fits-live-budget admission ("self-throttling"): a task starts only
  if it fits the live memory budget and a worker slot.
- Priority scheduling with **skip-to-fit** backfill and bounded **reservation** so a
  too-large task never head-of-line-blocks and never starves
  (`gptps_set_task_priority`, `[scheduler] reserve_after_skips`).
- Failure engine: per-task `timeout` / `max_retries` / `retry_backoff` /
  `on_failure` (dead_letter | drop | requeue); cooperative-cancel deadline watchdog.
- Dead-letter retention + `gptps_dead_letter_drain` / `gptps_dead_letter_count`.
- Lifecycle events + multiple observers; admission constraints (admit/deny/defer).

### Executors
- `GPTPS_EXEC_INPROC` (in-process, cooperative cancel).
- `GPTPS_EXEC_OOP` (POSIX: fork + run, OS-capped, hard-killed).
- `GPTPS_EXEC_PROGRAM` (any binary; payload→stdin, stdout→result) — POSIX
  fork+exec with process-group kill, and Windows `CreateProcess` + Job Object.
- Accurate memory enforcement: cgroup v2 `memory.max` (`GPTPS_E_NOMEM` on OOM) with
  `RLIMIT_AS` fallback on POSIX; Job Object memory limit on Windows.

### Configuration
- TOML-subset config file (`gptps_open(path)`): `[limits]`, `[scheduler]`,
  `[task_defaults]`/`[tasks.<name>]` overrides, and `addons = [...]` auto-load.

### Add-ons (in `addons/`, built on the public API)
- `durable_queue` — crash-durable submission (append-only journal, fsync-before-
  enqueue, replay survivors; at-least-once).
- `gpu_quota` — GPU-unit admission quota (constraint + observer).
- `wasm_exec` — run `.wasm` modules as tasks via a pluggable runtime hook; also
  runnable with no add-on via `GPTPS_EXEC_PROGRAM` + a wasm runtime CLI.

### Platforms & packaging
- Linux + macOS (full) and Windows (Win32 HAL; in-process + external-program
  executors). `GPTPS_EXEC_OOP` is POSIX-only (needs `fork`).
- Stable, versioned host-table ABI for dlopen'd add-ons (currently 1.3).
- Builds three ways: CMake (with `install()` + `find_package(gptps)` + pkg-config),
  the single-file amalgamation (cross-platform), and a plain `cc -std=c99`.
- CI: build/test on Linux + macOS + Windows, single-file amalgamation, ASan/UBSan,
  ThreadSanitizer.

[0.2.0]: https://github.com/Fikoko/GPTPS/releases/tag/v0.2.0
[0.1.0]: https://github.com/Fikoko/GPTPS/releases/tag/v0.1.0
