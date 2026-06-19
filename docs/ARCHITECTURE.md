# GPTPS Architecture

GPTPS (General Purpose Task Processing System) is an embeddable, in-process C99
task-processing library — "the SQLite of task processors." A single host process
links it, registers task types, and submits work; GPTPS schedules and runs that
work under a declared resource budget, with per-task failure policies and an
add-on seam for anything domain-specific.

This document describes how it is built. For *usage*, see the [README](../Readme.md);
for the *API contract*, see [`include/gptps.h`](../include/gptps.h).

---

## 1. Design goals (and how they shape the code)

| Goal | Consequence in the codebase |
|---|---|
| **Portability** — one task, any hardware | Strict C99 core; every OS primitive is behind the HAL (`gptps_hal.h`). No `_Atomic` in the core. |
| **Modularity** — handle all outcomes | Mechanism-only core + four add-on *seams* (task / constraint / transport / observer). Domain policy lives in add-ons, not the core. |
| **Embeddability** | No mandatory third-party dependency. Single-file amalgamation (`gptps.c` + `gptps.h`). Stable, versioned ABI. |
| **General purpose** | Tasks are opaque (`payload` bytes → `result` bytes). Three executors cover in-process, isolated, and any-language work. |

When a design fork appears, the tie-break order is: **modularity → portability →
general-purpose**, pushing platform specifics into the HAL.

---

## 2. Component map

```
                         host application
                                │  gptps.h (public ABI)
        ┌───────────────────────┼─────────────────────────────┐
        │                    GPTPS core (C99)                  │
        │                                                      │
        │   registry      single-writer DISPATCHER thread      │
        │   (task defs)        │  admission ledger             │
        │                      │  failure engine               │
        │                      │  deadline watchdog            │
        │   queues:  intake → ready → (worker pool) → done      │
        │            delayed (backoff)   running_items          │
        │            dead_letter (retained terminal failures)   │
        │                                                      │
        │   executors:  INPROC | OOP (fork) | PROGRAM (exec)    │
        └───────────┬───────────────────────────┬──────────────┘
                    │ gptps_hal.h                │ host-table ABI
            ┌───────┴────────┐          ┌────────┴─────────┐
            │  HAL backend   │          │   dlopen add-ons │
            │ hal_posix.c    │          │ (task/constraint │
            │ (threads,clock,│          │  /observer/...)  │
            │  dynload,detect│          └──────────────────┘
            └────────────────┘
```

Source layout:

- `include/gptps.h` — the public API + host-table ABI (the frozen contract).
- `include/gptps_hal.h` — internal platform-abstraction interface.
- `src/engine.c` — lifecycle, registry, queues, dispatcher, worker pool, in-process
  executor, failure engine, scheduler, add-on loader, dead-letter drain.
- `src/config.c` — config model + hardware auto-tune.
- `src/config_toml.c` — the TOML-subset config-file parser.
- `src/hal_posix.c` — the POSIX HAL backend.
- `src/exec_oop_posix.c` — the out-of-process and external-program executors.
- `addons/` — optional modules built on the public API (e.g. the durable queue).

---

## 3. Concurrency model

One mutex `m` guards all shared engine state. Two thread roles:

- **The dispatcher** (exactly one thread) is the *sole writer* of the admission
  ledger (`reserved_mem`, `running`) and the *only* timing authority (deadline
  enforcement, retry backoff). It owns all queue transitions except the two a
  worker performs.
- **Workers** (`max_concurrent_tasks` threads) pop from `ready`, run the task
  with the lock **released**, and post the finished item to `done`. They never
  touch the ledger.

Because a single thread owns admission and timing, the hard concurrency
questions (double-admission, budget races, lost deadlines) collapse to "is the
dispatcher's view consistent?" — which it is, by construction.

Two condition variables: `cv_work` (dispatcher → workers: "work is ready") and
`cv_disp` (workers/submitters → dispatcher: "state changed, re-evaluate").

**Event callbacks always run with the lock released.** The dispatcher buffers
pending events, releases the lock, emits, then re-acquires — so a user callback
can call back into the engine (e.g. `gptps_submit`, `gptps_dead_letter_drain`)
without deadlock or re-entrancy under the lock.

### The dispatch loop (one pass)

1. **Drain `done`** — release each finished item's budget, then decide its fate
   (ok → free; failed with retries left → `delayed`; failed & exhausted →
   `on_failure`).
2. **Promote `delayed`** — move backoff-ready items back to `intake`.
3. **Enforce deadlines** — flip the cancel flag on any running task past its
   deadline (cooperative for in-process; the OOP path hard-kills separately).
4. **Admit** — priority-ordered, skip-to-fit, with reservation (see §5).
5. **Emit** buffered events with the lock released, then `continue` (re-runs the
   loop so a signal arriving during the emit window can't be lost).
6. **Shutdown** check — when fully drained, wake workers to exit and break.
7. **Sleep** — `cond_timedwait` until the nearest deadline/backoff, or `cond_wait`
   until signalled. (Steps 1–7 hold the lock continuously when no events were
   emitted, so no wakeup can be lost.)

> A subtle lost-wakeup bug lived in the emit window (step 5): releasing the lock
> to emit could drop a `cv_disp` signal. The fix is the `continue` — re-running
> the loop re-drains `done` and re-admits `intake` rather than risking a sleep.

---

## 4. Admission: self-throttling budget

Each task type declares a rough **cost** (`mem_bytes`, `gpu_units`,
`est_duration_ms`). The engine resolves a budget at open time
(`max_concurrent_tasks`, `max_memory_bytes`) — explicit values win, else
hardware auto-tune (cores; ~0.75× RAM).

Admission is **declared-cost-fits-live-budget**, *not* an all-or-nothing cap:

```
admit X  ⟺  running < max_concurrent  ∧  reserved_mem + cost(X) ≤ max_memory
```

This is the novel framing: single-process **self-throttling** — "can my own
process afford to start this task right now, given my own remaining budget?" A
task whose cost can never fit `max_memory` is rejected at submit time
(`GPTPS_E_BUDGET`) so it can't wedge the queue.

---

## 5. Scheduling: priority + skip-to-fit + reservation

Step 4 of the dispatch loop does more than FIFO. Each admission pass scans
`intake` for:

- **`top`** — the highest-priority item overall (ties resolve to the older item,
  preserving FIFO within a priority);
- **`best`** — the highest-priority item that *fits* the live budget.

Then:

- If `best == top`, admit it (the normal case).
- If `best != top`, admitting `best` **skips** the higher-priority `top` because
  `top` doesn't fit the budget yet. This is **skip-to-fit backfill**: a too-large
  task does not head-of-line-block smaller work behind it. Each skip charges
  `top` a counter.
- Once `top` has been skipped `reserve_after_skips` times (default 8, configurable),
  the dispatcher **reserves** for it: backfill is suspended and the engine drains
  running tasks until `top` fits. This bounds starvation to at most
  `reserve_after_skips` backfills. Since over-budget submits are rejected up
  front, a reserved task is always eventually admittable.

Priority is per task type, set via `gptps_set_task_priority()` or config
(`[task_defaults]` / `[tasks.<name>]`). Default-0 priorities reduce the scan to
plain FIFO when the budget isn't the constraint.

---

## 6. Failure engine

Per-task `gptps_failure_policy`: `timeout_seconds`, `max_retries`,
`retry_backoff_seconds`, `on_failure`.

- **Timeout** — the dispatcher's watchdog flips the cancel flag at the deadline
  (in-process tasks must poll `gptps_is_cancelled`); the OOP/PROGRAM paths
  hard-kill the child.
- **Retry** — a failed attempt with retries remaining goes to `delayed` and is
  re-admitted after `retry_backoff_seconds` (emitting `RETRIED`).
- **`on_failure`** when retries are exhausted:
  - `dead_letter` (default, safe) — retained in the in-memory dead-letter list,
    emits `DEAD_LETTERED`;
  - `drop` — discarded (no event);
  - `requeue` — re-enqueued for another cycle (bodies MUST be idempotent; never
    re-admitted during shutdown, to avoid an always-failing task hanging the drain).
- **Dead-letter drain** — `gptps_dead_letter_drain()` detaches the retained list
  under the lock, then hands each item to a callback with the lock released (so it
  may re-submit). A constraint `DENY` is also retained, with status `GPTPS_E_DENIED`.

---

## 7. Executors

Selected per task type via `def.exec`:

| Kind | Runs as | Memory cap | Kill | Platforms |
|---|---|---|---|---|
| `GPTPS_EXEC_INPROC` | the C function, in-process | none (shared address space) | cooperative cancel flag | all |
| `GPTPS_EXEC_OOP` | the same C function in a `fork()`ed child | cgroup v2 `memory.max`, else `RLIMIT_AS` | `SIGKILL` on timeout | POSIX only |
| `GPTPS_EXEC_PROGRAM` | an external program (`def.argv`); payload→stdin, stdout→result | cgroup/`RLIMIT_AS` (POSIX) · Job Object (Windows) | group `SIGKILL` (POSIX) · `TerminateJobObject` (Windows) | all |

`GPTPS_EXEC_OOP` forks an in-process function into an isolated child, so it is
POSIX-only; on Windows use `GPTPS_EXEC_PROGRAM` (`CreateProcess` + a Job Object for
the memory cap and a single kill) for isolated, killable, capped work.

The POSIX out-of-process executors apply the memory cap accurately with **cgroup v2** when
`GPTPS_CGROUP_PARENT` names a memory-delegated cgroup: each task gets a child
cgroup with `memory.max` + `memory.swap.max=0`; the child moves itself in before
allocating; exceeding the cap is a real OOM-kill surfaced as `GPTPS_E_NOMEM`.
Every step is best-effort and Linux-gated — anything missing falls back to the
coarse `RLIMIT_AS` (VSZ) cap, so behavior degrades gracefully, never breaks.

`fork()` in a multithreaded process keeps only the calling worker in the child,
so OOP tasks must be fork-safe / self-contained (CPU/memory-bound work, or
untrusted code you want isolated and killable).

---

## 8. The HAL (`gptps_hal.h`)

The only platform-specific seam. Pure C99 interface; each backend uses the best
primitive its platform offers, and **all atomics are confined to the backend**
so the core never includes an `_Atomic` type.

Surface: hardware detection (CPU/RAM/GPU hint), monotonic clock, the cancel flag
(opaque, atomic inside), mutex / condvar / thread, and dynamic loading. Two
backends implement it: `hal_posix.c` (pthreads, `clock_gettime`,
`sysctl`/`sysinfo`, `dlopen`) and `hal_win.c` (`_beginthreadex`,
`CRITICAL_SECTION` + `CONDITION_VARIABLE`, `Interlocked*`, `GetTickCount64`,
`GetSystemInfo`/`GlobalMemoryStatusEx`, `LoadLibrary`). CMake picks the backend by
platform; both are CI-verified (Windows via mingw-w64 on a `windows-latest`
runner). The external-program executor exists on both POSIX (`exec_oop_posix.c`)
and Windows (`exec_win.c`, `CreateProcess` + Job Object); the forked `EXEC_OOP`
kind is POSIX-only (it forks an in-process function — no `fork()` on Windows).

Feature-test macros (`_GNU_SOURCE` / `_DARWIN_C_SOURCE`) are defined **in-source**
at the top of each backend (and at the top of the amalgamation), so a plain
`cc -std=c99 gptps.c yourapp.c` exposes the POSIX APIs the HAL needs without any
build-system `-D` flags.

---

## 9. Add-ons and the host-table ABI

The core is mechanism-only; variety lives in add-ons across four seams:

- **task** — register task types (frozen v1.0);
- **constraint** — an admission hook (`ADMIT` / `DENY` / `DEFER`), consulted in
  the dispatcher so it MUST be non-blocking; used for rate limits, quotas,
  time-of-day windows, GPU caps;
- **observer** — an extra event sink (analytics, durable queue, dead-letter sink);
- **transport** — an exec bridge (experimental; frozen with its first consumer).

A dlopen'd add-on calls the core **only** through a passed, version-stamped
`gptps_api_routines` table and links **no** core symbols (this also avoids symbol
capture in the amalgamated build, where core symbols are namespaced `gptps_`/
`gptps__`). It exports exactly one symbol, `gptps_addon_init`, conventionally via
the `GPTPS_ADDON_INIT(...)` macro. The loader validates `magic` /
`abi_version_major` / `struct_size` **before** using the add-on.

Not every add-on must be a shared object — a module that only uses the public API
and the observer/constraint seams can simply be compiled into the host. Three ship
in `addons/`: `durable_queue.c` (observer seam → crash-durable journal),
`gpu_quota.c` (constraint + observer composed → GPU-unit admission quota), and
`wasm_exec.c` (module-as-task with a pluggable wasm runtime). See
[`addons/README.md`](../addons/README.md).

---

## 10. Configuration

`gptps_open(path)` parses a config file (a TOML *subset* — tables, dotted tables,
int/float/bool/string scalars, single-line string arrays, `#` comments — parsed
by `config_toml.c`, no external dependency). It maps to:

- `[limits]` → engine budget (concurrency, memory);
- `[scheduler]` → `reserve_after_skips`;
- `[task_defaults]` then `[tasks.<name>]` → per-task policy / cost / priority,
  applied at registration (def < `task_defaults` < `tasks.<name>`);
- top-level `addons = [...]` → shared libraries auto-loaded at open.

`gptps_open_ex(cfg, ...)` is the explicit, file-free path.

---

## 11. ABI versioning & stability

- Every caller-extensible struct's first field is `size_t struct_size`; the core
  rejects an undersized struct and reads appended fields conditionally.
- Structs may **only** grow by appending fields. Never reorder/remove/retype.
- `GPTPS_ABI_VERSION_MINOR` bumps on additive change, `MAJOR` on an incompatible
  one; the add-on loader refuses a `MAJOR` mismatch.

New capabilities have so far been added without breaking the ABI: result
delivery, the external-program executor, constraints/observers, task priority
(`gptps_set_task_priority`), and the dead-letter drain — each a new symbol or an
appended field, never a reshape.

---

## 12. Verification discipline

Every increment ships with tests and is held to: CTest green on Linux + macOS,
**AddressSanitizer + UBSan**-clean across the whole suite and **ThreadSanitizer**-
clean on the concurrent paths (ASLR disabled in CI), stress loops on timing-
sensitive tests, fuzzing of the two hand-rolled parsers (TOML + journal), and a
check that all three build paths work (CMake, the single-file amalgamation, and a
plain `cc -std=c99`). Platform-specific tests (OOP memory caps, cgroup enforcement)
**self-skip** where the facility is absent rather than failing (cgroup delegation, a
wasm runtime CLI). CI runs six jobs:
`build-test` (Linux + macOS), `windows` (mingw-w64), `amalgamation`, `asan`, and `tsan`.

---

## 13. Future work (intentionally not built yet)

Some planned work is partial or unbuilt, because this project only ships code it
can verify, and these could not be fully tested in the environment they were
developed in:

- **WASM — works today; only a *bundled default* runtime is unbuilt.** Two ready
  paths: (1) `GPTPS_EXEC_PROGRAM` + a wasm runtime CLI — `argv = ["wasmtime",
  "run", "module.wasm"]`, argv[0] PATH-resolved — shown in
  `examples/wasm_program.c` (which embeds a hand-assembled, validated `.wasm` and
  self-skips when no runtime is on PATH); (2) `addons/wasm_exec.c`, an in-process
  binding that takes a pluggable `gptps_wasm_run_fn` hook (wasm3/wasmtime/WAMR),
  fully tested with a mock runtime. What's **not** bundled is an actual interpreter
  — left pluggable to keep the core dependency-free; a bundled default would need a
  runtime to vendor + a wasm toolchain to test against (the sandbox blocks fetching
  external code, so this is owner-gated).
Windows is otherwise complete: the Win32 HAL (`hal_win.c`), the external-program
executor (`exec_win.c`, `CreateProcess` + Job Object), and a `windows-latest` CI
job all ship and are green. The only platform gap is `GPTPS_EXEC_OOP`, which forks
an in-process function — inherently POSIX (no `fork()` on Windows); Windows callers
use `GPTPS_EXEC_PROGRAM` for the same isolated/killable/capped guarantees.

This is tracked here rather than stubbed misleadingly, so the tree stays fully
tested and green.
