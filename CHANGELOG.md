# Changelog

All notable changes to GPTPS are recorded here. Format follows
[Keep a Changelog](https://keepachangelog.com/); the project aims for semantic
versioning once it reaches 1.0.

## [Unreleased]

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
