# Changelog

All notable changes to GPTPS are recorded here. Format follows
[Keep a Changelog](https://keepachangelog.com/); the project aims for semantic
versioning once it reaches 1.0.

## [Unreleased]

### Added — optional platform-optimized HAL (scale knob)
- **`-DGPTPS_HAL_FAST=ON`** builds the POSIX HAL with **adaptive (spin-then-block)
  mutexes** on glibc — a latency knob for the engine's short, contended critical
  sections under high submit/dispatch load. Same lock semantics (no correctness
  change); it stays **OFF by default**, so the portable pthread HAL is the untouched
  default. The whole HAL is a module boundary, so scaling here needs no core change —
  and a downstream can swap the HAL source wholesale for its own platform-optimized
  one. CI builds and tests the fast variant (`hal_fast` job).

### Added — ABI 1.12: pluggable scheduler seam
- **`gptps_set_scheduler(e, fn, ud)`** makes the admission ORDERING a swappable
  policy without touching the core. The dispatcher's *mechanism* stays fixed and
  general — admit the best-ordered pending item that fits the live budget, skip a
  too-large item to backfill smaller work (no head-of-line blocking), reserve for a
  repeatedly-skipped top item so it can't starve. What "best-ordered" *means* was
  hard-wired to scheduling priority; now a hook returns an `int64` score per item
  (`gptps_sched_input`: task, cost, priority, attempt, enqueue time, payload) and the
  dispatcher admits the highest score that fits — so deadline-first, per-tenant
  fair-share, cost-aware, or aging disciplines are composable, not core forks. Default
  (no hook) is unchanged priority/FIFO ordering, with zero added overhead. Also on the
  host-table ABI (`GPTPS_SEAM_SCHEDULER`) so add-ons can install one. (`test_sched_seam`.)

### Added — scale-up by composition: the shard/router add-on
- **`addons/gptps_pool`** runs N independent engine shards (each its own lock +
  dispatcher + worker pool) and routes each submit to one of them, scaling past the
  single-node single-writer ceiling **without any core change** — the proof that the
  engine scales the modular way. `gptps_pool_submit` spreads load round-robin;
  `gptps_pool_submit_keyed` pins a key to a fixed shard (per-tenant affinity / per-key
  order); a returned `gptps_pool_handle` tags the shard so `gptps_pool_cancel` routes
  back. Built entirely on the public API. (`test_pool`: even spread, affinity,
  handle-routed cancel, cross-shard dead-letter aggregation.)

### Added — ABI 1.11: long-running service tasks
- **`GPTPS_TASK_SERVICE`** (a new `gptps_task_def.flags` bit) marks a task type as a
  supervised, long-running **service** instead of a one-shot job. You start an
  instance with `gptps_submit` (start several for a pool); its `run()` is expected to
  loop until told to stop (polling `gptps_is_cancelled()`), and when it returns for
  any reason other than a stop request it is **automatically restarted** after
  `retry_backoff_seconds` (crash-restart supervision). The engine normalizes the
  failure policy for you (`on_failure = REQUEUE`, `max_retries = 0`, no timeout), at
  registration and again per submit so neither a config file nor a live settings edit
  nor a `submit_ex` override can quietly un-service an instance.
  - The submit **handle stays valid across restarts**, so `gptps_cancel(handle)` stops
    that one instance for good (no restart). `gptps_unregister_task` stops every
    instance of the type — a `DRAIN` is auto-upgraded to `CANCEL`, since a service
    never drains on its own — and **`gptps_shutdown` now stops running services**
    (raising their cooperative cancel flag) so a resident service no longer hangs
    teardown. Non-service in-flight work still drains gracefully.
  - v1 restrictions, rejected at registration with `GPTPS_E_INVAL`: `INPROC` executor
    only, `THREADED` mode only (an infinite loop cannot be run to completion by the
    `MANUAL` `gptps_step` pump), and no `timeout_seconds`.
  - **`GPTPS_TASK_RETIRE_ON_OK`** (a second flag) opts a service out of "always up":
    a clean `GPTPS_OK` return then terminally retires that instance (only a non-OK
    return restarts it) — the `Restart=on-failure` semantic vs. the default
    `Restart=always`.
- **Append-safe ABI struct guards.** Input structs are now validated against a frozen
  minimum size (`GPTPS_TASK_DEF_MIN_SIZE`) and later-appended fields are read only when
  the caller's `struct_size` covers them (`GPTPS_STRUCT_HAS`), instead of rejecting any
  struct smaller than the current `sizeof`. This is what lets `gptps_task_def` grow the
  `flags` field without breaking a caller compiled against an older header — honoring
  the header's append-only ABI promise. `gptps_task_def.flags` is a `uint64_t` (not
  `uint32_t`) specifically so the appended field cannot fall inside a pre-v1.11 struct's
  trailing padding on 32-bit ABIs (ARM32/AAPCS, MIPS32) — which would have made
  `struct_size` detection ambiguous; a compile-time assertion enforces this invariant.

### Fixed
- **External-program executor deadlock on a large payload (POSIX).** `gptps_program_execute`
  wrote the *entire* payload to the child's stdin before it began reading stdout, so a
  streaming child (one that emits output while still consuming input) deadlocked once both
  pipes filled — reachable with any payload larger than the pipe buffer. The parent now pumps
  stdin and stdout **concurrently** in a single `poll` loop (non-blocking stdin writes
  interleaved with stdout reads). The same bug also meant a large payload to a stdin-ignoring
  child blocked *before* the deadline was ever enforced; the deadline now governs the whole
  exchange.
- **Out-of-process / program tasks are now cancellable.** Both POSIX executors and the Win32
  program executor take the item's cooperative cancel flag and wait in bounded (~200 ms)
  slices, so `gptps_cancel(handle)` and `gptps_unregister_task(..., CANCEL)` hard-kill a
  running child — even one with `timeout_seconds == 0`, which previously waited forever and
  could not be stopped. The child's cgroup-join path (`cg_write_file`) is now allocation-free,
  closing a malloc-between-fork-and-exec hazard under a custom allocator.
- **Named-resource reservation leak on retry/restart.** A task with a `gptps_define_resource`
  cost allocated a per-item reservation snapshot on admission that was only released from the
  budget ledger — not freed — on completion, so a re-admitted item (a retry, or a service's
  REQUEUE restart) leaked the previous snapshot. For a long-running service this was an
  unbounded leak. The snapshot is now freed at release, symmetric with admission.
- **`gptps_cancel` could miss an item briefly sitting in the completion queue.** A cancel
  arriving in the narrow window between a worker posting a finished item and the dispatcher
  reaping it returned `GPTPS_E_NOTFOUND` without cancelling, so a crash-restarting service
  could dodge the cancel and restart. `gptps_cancel` now also scans that queue, honoring the
  "stops the instance for good" guarantee.

### Added — ABI 1.10: generic named-resource budgets
- **`gptps_define_resource(e, name, budget)`** declares an arbitrary named,
  budgeted admission resource (GPUs, I/O bandwidth, license seats, a per-tenant
  quota — anything). **`gptps_set_task_resource_cost(e, task, resource, amount)`**
  declares a task type's per-item cost against it, and **`gptps_resource_usage`**
  introspects budget vs. reserved. The dispatcher admits an item only while every
  resource it costs still fits, reserving on admit and releasing on terminal —
  generalizing admission beyond the dedicated memory budget. An item whose cost
  exceeds a whole budget is rejected at submit with `GPTPS_E_BUDGET`. This makes
  the cost/admission side as generic as the settings registry; "gpu" is now just a
  named resource (the `gpu_units` field and gpu_quota add-on remain for back-compat).
  All three are also on the host-table ABI for add-ons.

### Added — ABI 1.9: modularity & portability gap-closure
- **Per-item constraint context.** The constraint/admission hook now receives a
  `gptps_constraint_input` (task name, cost, **item handle**, and **payload**)
  instead of just `(name, cost)`. This is the keystone that makes per-item
  add-ons — dependencies, dedup/idempotency, per-tenant admission — buildable on
  the seam. The `GPTPS_SEAM_CONSTRAINT`/`GPTPS_SEAM_OBSERVER` seams are now frozen.
- **`gptps_cancel(handle)`** cancels a single submitted item (queued, admitted, or
  in-flight) with a terminal event and a no-op on unknown/already-terminal handles.
- **Backpressure.** `gptps_limits.max_intake_depth` (and the live
  `limits.max_intake_depth` setting) bound the intake queue; `gptps_submit` returns
  the long-reserved `GPTPS_E_FULL` once it is full.
- **`gptps_submit_ex`** applies per-submit overrides (priority / failure policy /
  sub-second deadline) without cloning the task type.
- **`gptps_unregister_constraint` / `gptps_unregister_observer`** (also in the
  host table) close the register-only asymmetry and enable add-on hot-unload.
- **`gptps_set_log_sink`** redirects/silences the core's diagnostics (no-stdio hosts).
- **`gptps_version()` / `GPTPS_VERSION_*`** expose the release version (distinct
  from the ABI version).
- **`gptps_task_def.child_setup`** — an optional fork-time hook for OOP/PROGRAM
  children to harden themselves (chdir, setenv, setrlimit, drop privs, seccomp,
  close fds) before exec.
- **`GPTPS_EV_DROPPED`** — a terminal event when an item is discarded under the
  DROP policy, so observers can reconcile every submitted item.
- **Orchestration add-on** (`addons/gptps_orch.*`): run-after / fan-in task
  dependencies built purely on the public seams (observer + submit), no core changes.
- **Freestanding reference** (`freestanding/`): a stub HAL + demo proving the C99
  core runs in MANUAL mode with no pthread/dl/fork and no libc heap, compiled
  `-ffreestanding` and run in CI.
- **CI:** a 32-bit (i386) + big-endian (s390x under QEMU) job, a freestanding job,
  and TSan coverage extended from 5 to 10 tests.

### Changed
- Container-aware auto-tune: `gptps_hal_detect` clamps CPU/RAM to cgroup v2
  `cpu.max` / `memory.max` and the CPU affinity mask, so sizing fits the container.
- `GPTPS_EV_QUEUED` is emitted with the engine lock released (a slow observer no
  longer stalls admission); it now fires on the submitting thread.

### Fixed
- **`durable_queue`**: propagate fsync/fflush errors as `GPTPS_E_IO` (was a silent
  false-success), fsync the parent directory after the compaction rename, and
  **quarantine** dead-lettered records (retain the poison payload across crashes;
  `gptps_dq_quarantined` / `gptps_dq_drain_quarantine`) instead of dropping it.
- OOP/PROGRAM pipe fds are now close-on-exec, fixing a hang where a concurrent
  PROGRAM child could pin another executor's pipe open.
- Documentation truthfulness: removed the stale "no implementation yet" header
  banner; corrected `event.mem_bytes` (declared cost, not measured RSS, for OOP);
  clarified the embedded example's "no libc heap" claim (core only; see
  `freestanding/` for a true no-libc build).
- Test suite: fixed a timing race in `test_taskmgmt` (deterministic under load).

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
