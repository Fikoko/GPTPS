# Changelog

All notable changes to GPTPS are recorded here. Format follows
[Keep a Changelog](https://keepachangelog.com/); the project aims for semantic
versioning once it reaches 1.0.

## [Unreleased]

### Added — unified settings subsystem (ABI 1.4)
- A typed settings **registry**: introspection (`gptps_settings_count` /
  `gptps_settings_get_info`), validated string get/set (`gptps_settings_get` /
  `gptps_settings_set`), and `gptps_register_setting` — schema + accessor binding,
  so live engine/add-on state stays the single source of truth.
- Dotted keys over core (`limits.*`, `scheduler.*`), per-task (`tasks.<name>.*`),
  and add-on (`tui.*`, `gpu_quota.*`) settings; per-setting `hot` vs restart-only.
- **Round-trip persistence:** `gptps_settings_save` / `gptps_settings_reload`, with
  a portable atomic-replace HAL primitive (`rename` / `MoveFileEx`).
- **Add-on extensible:** `register_setting` host-table routine (append-only;
  `GPTPS_ABI_VERSION_MINOR` 3 → 4); the `tui` and `gpu_quota` add-ons register theirs.
- **Live editor:** a Settings pane in the `tui` dashboard (`s`) to browse / edit /
  save settings at runtime.
- Validation that the raw TOML path lacked (bad enum / out-of-range / type → `E_CONFIG`).

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

[0.1.0]: https://github.com/Fikoko/GPTPS/releases/tag/v0.1.0
