# GPTPS — General Purpose Task Processing System

**An embeddable, in-process C99 task processor (Control Plane) aimed for modularity and portability — the "General Purpose Task Processing System"**
Link one library, register a task, submit work. GPTPS runs it on a worker pool under
declared resource budgets, with retries / timeouts / dead-letter, and gives you the
result back — plus an optional **live terminal dashboard** to watch and steer it. No
server, no broker, no mandatory dependency. Runs on Linux, macOS, and Windows, and can
even run **single-threaded with no libc heap** for embedded / bare-metal targets.

---

## Contents

- [Quick start](#quick-start) · [Getting started](#getting-started) — build → run → embed
- [API at a glance](#api-at-a-glance) · [Common tasks](#common-tasks) — step-by-step recipes
- [Configuration file](#configuration-file-optional) · [Settings](#settings-runtime-introspectable-persistable)
- [Live terminal dashboard](#live-terminal-dashboard) · [Executor kinds](#executor-kinds-per-task-via-defexec)
- [Embedded / single-threaded mode](#embedded-and-single-threaded-mode) · [Resource budgets & failures](#resource-budgets-failures-add-ons)
- [Project layout](#project-layout) · [Status](#status) · [Design notes](#design-notes)

## Quick start

```c
#include "gptps.h"
#include <stdio.h>
#include <string.h>

/* a task: sum the payload bytes, return the sum */
static gptps_status sum(gptps_ctx *ctx, void *ud) {
    size_t n, i; const unsigned char *p = gptps_payload(ctx, &n);
    unsigned long s = 0; (void)ud;
    for (i = 0; i < n; ++i) s += p[i];
    return gptps_result_set(ctx, &s, sizeof s);
}

static void on_event(const gptps_event *ev, void *ud) {
    (void)ud;
    if (ev->kind == GPTPS_EV_FINISHED)
        printf("task %s done: %lu\n", ev->task_name, *(const unsigned long*)ev->result);
}

int main(void) {
    gptps *e;
    gptps_task_def d = {0};
    gptps_handle h;

    gptps_open(NULL, &e);                       /* auto-tunes to the machine  */
    gptps_set_event_cb(e, on_event, NULL);

    d.struct_size = sizeof d; d.name = "sum"; d.run = sum; d.exec = GPTPS_EXEC_INPROC;
    d.default_cost.struct_size = sizeof d.default_cost; d.default_cost.mem_bytes = 4096;
    d.default_policy.struct_size = sizeof d.default_policy; d.default_policy.timeout_seconds = 5;
    gptps_register_task(e, &d);

    gptps_submit(e, "sum", "hello", 5, &h);     /* runs on the pool           */
    gptps_shutdown(e);                          /* drains, then returns       */
    return 0;
}
```

## Getting started

**Build → run → embed** — the whole path from zero to your own program. You need a C99 compiler
(`gcc`/`clang`) and **CMake ≥ 3.13** — nothing else on Linux/macOS (Windows uses MSVC or
mingw-w64; the build auto-selects the Win32 backend).

**1. Build.**

```sh
cmake -S . -B build          # configure (once)
cmake --build build -j       # libgptps.a + the examples + the test suite
```

**2. See it run — the live dashboard.** Run it in a real terminal (it needs an interactive
TTY; with no TTY it just prints a line and exits, which is how CI runs it headless):

```sh
./build/example_dashboard    # keys: w/f submit · k/j scroll · m KPI · p pause · s settings · ? help · q quit
```

It looks like [this](#live-terminal-dashboard) (screenshot below).

**3. Run the other examples + the tests.**

```sh
./build/demo                 # in-process tasks + events (prints results)
./build/example_config       # tune from a TOML file
./build/example_embedded     # no threads + a static memory pool (manual mode)
ctest --test-dir build --output-on-failure   # full suite (should be 100%)
```

More in [`examples/`](examples/): `external_program` (run any binary as a task) and
`wasm_program` (run a `.wasm` module via a wasm runtime CLI — see WebAssembly below).

**4. Embed it in your own program.** Generate the single-file amalgamation (two files, zero
build system), drop the [Quick start](#quick-start) program into `myapp.c`, and link:

```sh
sh tools/amalgamate.sh out   # writes out/gptps.c and out/gptps.h
cc -std=c99 myapp.c out/gptps.c -Iout -lpthread -ldl   # Linux
cc -std=c99 myapp.c out/gptps.c -Iout -lpthread        # macOS (dlopen is in libSystem)
./myapp
```

To embed the **dashboard** too, also compile the add-on: add `addons/tui.c -Iaddons`
(`examples/demo.c` is a fuller starting template).

**Install / consume (optional).** `cmake --install build --prefix <dir>` installs the header,
static library, a CMake package config, and a pkg-config file. Downstream projects then use
either `find_package(gptps)` → link `gptps::gptps`, or `pkg-config --cflags --libs gptps`.

## API at a glance

| Call | Purpose |
|---|---|
| `gptps_open(path, &e)` / `gptps_open_ex(cfg, &e)` | create an engine (auto-tunes workers + memory budget) |
| `gptps_register_task(e, &def)` | register a task type (in-process fn **or** external program) |
| `gptps_set_task_priority(e, name, prio)` | set a task type's scheduling priority (higher runs first) |
| `gptps_submit(e, name, payload, len, &handle)` | enqueue work |
| `gptps_set_event_cb(e, cb, ud)` | observe lifecycle events (results arrive on `FINISHED`) |
| `gptps_register_constraint(e, fn, ud)` | gate admission (rate limit, quota, time window) |
| `gptps_register_observer(e, cb, ud)` | extra event sink (e.g. analytics) |
| `gptps_dead_letter_count(e)` / `gptps_dead_letter_drain(e, cb, ud)` | inspect / reprocess retained failures |
| `gptps_settings_get/set(e, key, …)` · `gptps_settings_count/get_info(e, …)` | read / change / introspect any setting at runtime |
| `gptps_settings_save/reload(e, path)` | persist settings to / from a TOML file |
| `gptps_register_setting(e, &def)` | add a custom setting (also a host-table routine for add-ons) |
| `gptps_load_addon(e, path)` | load a shared-library add-on over the stable ABI |
| `gptps_step(e, &ran)` | MANUAL mode: pump the engine on the calling thread (no worker threads) |
| `gptps_set_allocator(&a)` | redirect all core allocation to a custom `malloc`/`realloc`/`free` |
| `gptps_shutdown(e)` | drain in-flight + queued work, then free |

Inside a task you get a `gptps_ctx *`: `gptps_payload()`, `gptps_is_cancelled()` (poll it
for cooperative timeout), `gptps_result_set()` / `gptps_result_set_nocopy()`.

## Common tasks

Short recipes for the things you'll actually do — each links to the full details below.

**1. Run work and get the result back.** Register a task with a `run` function, set an
event callback, and read the result on the `FINISHED` event — that's the
[Quick start](#quick-start) above.

**2. Give a task a timeout, retries, and a failure policy.** Set the policy on the task
def *before* registering it:

```c
d.default_policy.timeout_seconds       = 5;
d.default_policy.max_retries           = 3;
d.default_policy.retry_backoff_seconds = 1;
d.default_policy.on_failure            = GPTPS_ON_FAILURE_DEAD_LETTER;  /* or _DROP / _REQUEUE */
```

In-process tasks must poll `gptps_is_cancelled()` to honor the timeout. Tasks that
exhaust their retries are kept in the dead-letter list — reprocess them with
`gptps_dead_letter_drain()`.

**3. Cap how much runs at once (the budget).** Open with explicit limits (or a
[config file](#configuration-file-optional), so you can re-tune without recompiling):

```c
gptps_config cfg = { .struct_size = sizeof cfg };
cfg.limits.struct_size = sizeof cfg.limits;
cfg.limits.max_concurrent_tasks = 4;          /* 0 => auto-detect cores      */
cfg.limits.max_memory_bytes     = 512u << 20; /* admission budget (declared) */
gptps_open_ex(&cfg, &e);
```

**4. Run heavy or untrusted work isolated and killable.** Set the executor kind on the
task: `d.exec = GPTPS_EXEC_OOP` (forked child, memory-capped, hard-killed on timeout) or
`GPTPS_EXEC_PROGRAM` to run any external binary — see [Executor kinds](#executor-kinds-per-task-via-defexec).

**5. Watch it live.** Install the dashboard add-on and run your program in a terminal —
see [Live terminal dashboard](#live-terminal-dashboard).

**6. Change a setting at runtime and persist it.**

```c
gptps_settings_set(e, "tasks.resize.timeout_seconds", "60");  /* validated + applied live */
gptps_settings_save(e, "gptps.toml");                          /* atomic round-trip       */
```

See [Settings](#settings-runtime-introspectable-persistable).

**7. Run with no background threads (embedded / bare-metal).** Open with
`cfg.mode = GPTPS_RUN_MANUAL` and drive it yourself with `gptps_step()` — see
[Embedded / single-threaded mode](#embedded-and-single-threaded-mode).

## Configuration file (optional)

`gptps_open("gptps.toml", &e)` tunes the engine from a config file — no recompile to
re-tune for a new machine or change a task's failure policy. Pass `NULL` to skip it and
auto-tune. A subset of TOML is supported (tables, `int`/`float`/`bool`/`"string"` and
single-line string arrays, `#` comments):

```toml
# top level: shared-library add-ons to auto-load at open
addons = ["./libmytasks.so"]

[limits]
max_concurrent_tasks = 8       # 0 / omitted => detected cores
max_memory_gb        = 4.0     # or max_memory_bytes = 4294967296

[scheduler]
reserve_after_skips = 8        # starvation guard (0 => strict priority, no backfill)

[task_defaults]                # applied to every task...
max_retries = 2
on_failure  = "dead_letter"    # dead_letter | drop | requeue
priority    = 0                # higher => admitted first

[tasks.resize]                 # ...then overridden per task name
timeout_seconds = 60
max_retries     = 1
on_failure      = "drop"
mem_bytes       = 268435456
priority        = 10
```

Precedence for a task's policy: compiled-in `def` defaults → `[task_defaults]` → `[tasks.<name>]`
(most specific wins). Explicit `[limits]` values win over auto-tune. See `gptps.example.toml`.

## Settings (runtime, introspectable, persistable)

The same knobs are also a live, typed **settings registry** — one API over core,
per-task, and add-on settings (dotted keys like `scheduler.reserve_after_skips`,
`tasks.resize.timeout_seconds`, `tui.kpi`, `gpu_quota.total_units`):

```c
char v[256];
gptps_settings_get(e, "scheduler.reserve_after_skips", v, sizeof v);  /* read current */
gptps_settings_set(e, "tasks.resize.timeout_seconds", "60");          /* validated + applied live */
gptps_settings_save(e, "gptps.toml");                                 /* persist (atomic) */
size_t n = gptps_settings_count(e);                                   /* enumerate for a UI */
```

- **Typed + validated:** `set()` parses and range/enum-checks before applying (so a bad
  `on_failure` or out-of-range value is rejected with `GPTPS_E_CONFIG`, not silently dropped).
- **Hot vs restart:** most settings apply immediately; a few (e.g. the worker-pool size) are
  flagged effective-on-restart. `gptps_settings_get_info` exposes type, default, range, and the
  hot flag for building a UI.
- **Round-trip:** `gptps_settings_save` regenerates a grouped TOML file (atomically; comments
  not preserved); `gptps_settings_reload` re-applies it.
- **Extensible:** add-ons register their own settings (via `gptps_register_setting` or the
  host-table routine), so they show up in the registry, TOML, and editor uniformly.
- **Editor:** the `tui` add-on includes a live **Settings pane** (`s`) to browse/edit/save.

## Live terminal dashboard

GPTPS ships an optional, dependency-free terminal dashboard (`addons/tui.c`) — link it,
point it at your engine, and watch tasks flow in real time. Pure ANSI/VT (no ncurses),
auto-enabled on a TTY, with the Windows console put into VT mode automatically.

```text
GPTPS · live demo   up 0.1s   28.6 done/s
queued 18  started 8  in-flight 4  [##########################] peak 4
finished 4  failed 0  retried 0  dead 0
kpi:full mode:realtime refresh:250ms

TASKS
  label              run    ok  fail  dead   ok%   avg ms  key
  Resize               8     4     0     0  100%     81.0  [r]
  Thumbnail            0     0     0     0    --       --  [t]

RECENT
     0.1 FINISHED resize         #4
     0.1 FINISHED resize         #1
     0.1 STARTED  resize         #7
     0.1 STARTED  resize         #8

keys: [r] Resize  [t] Thumbnail   ·  ? help  s settings  m kpi  p pause  k/j scroll  q quit
```

- **Live metrics:** throughput, an in-flight gauge, cumulative counts, and a per-task
  table (runs / ok / fail / dead / success-rate / average latency).
- **Interactive:** hotkeys submit tasks; `k`/`j` scroll the event log; `m` dials the
  dashboard's own CPU/RAM cost (minimal/normal/full) live; `p` pauses; `s` opens the live
  **settings editor**; `?` shows a help overlay of every key.
- **Friendly:** adapts to the terminal size, redraws flicker-free, confirms actions with a
  toast, and on a real terminal adds a framed title bar, a Unicode block gauge, and color
  (with an ASCII fallback shown above). Built on a pure render-to-string core, so it is
  fully testable headlessly.

Run it live in a terminal: `./build/example_dashboard` (source:
[`examples/dashboard.c`](examples/dashboard.c)).

## Executor kinds (per task, via `def.exec`)

| Kind | Runs as | Enforcement | Platforms |
|---|---|---|---|
| `GPTPS_EXEC_INPROC` | your C function, in-process | cooperative cancel (advisory) | all |
| `GPTPS_EXEC_OOP` | the same C function in a forked child | memory cap + hard-kill on timeout | POSIX only (needs `fork`) |
| `GPTPS_EXEC_PROGRAM` | an external program (`def.argv`); payload→stdin, stdout→result | memory cap + hard-kill on timeout | all (POSIX `fork`+exec; Windows `CreateProcess` + Job Object) |

On POSIX the out-of-process executors enforce the per-task memory cap accurately with
**cgroup v2** (`memory.max`, exceeding it ⇒ `GPTPS_E_NOMEM`) when `GPTPS_CGROUP_PARENT`
points at a memory-delegated cgroup (e.g. a systemd `Delegate=yes` scope), else a coarse
`RLIMIT_AS` cap; on Windows the program executor uses a **Job Object** (memory limit +
kill-on-close). Either way it's real, killable enforcement the in-process path can't give.

**WebAssembly.** A `.wasm` module is portable, sandboxed task code — and a wasm runtime CLI
is just a program, so you can run one through `GPTPS_EXEC_PROGRAM` with **no new code**:
`def.argv = {"wasmtime", "run", "module.wasm", NULL}` (argv[0] is PATH-resolved). The payload
flows to the module's stdin and its stdout comes back as the result, under the usual budget /
timeout / retry. See [`examples/wasm_program.c`](examples/wasm_program.c). For a tighter,
in-process binding, the [`wasm_exec`](addons/) add-on takes a pluggable runtime hook instead.

## Embedded and single-threaded mode

GPTPS runs in two execution modes, chosen at `gptps_open_ex` via `cfg.mode`:

- **`GPTPS_RUN_THREADED`** (default) — a dispatcher + worker pool; the engine runs
  itself. The headline hosted path.
- **`GPTPS_RUN_MANUAL`** — **no threads.** You drive the engine cooperatively on
  your own thread with `gptps_step(e, &ran)`: one call completes finished work,
  promotes backoff-ready retries, admits within budget, and runs the admitted tasks
  to completion inline. Drain with `while (gptps_step(e,&n)==GPTPS_OK && n);`, or
  call it from your existing main loop / RTOS tick. Same admission, priority, retry,
  and dead-letter semantics as threaded mode (one shared `engine_pass`).

```c
gptps_config cfg = { .struct_size = sizeof cfg, .mode = GPTPS_RUN_MANUAL };
cfg.limits.struct_size = sizeof cfg.limits;
gptps_open_ex(&cfg, &e);
/* ... register + submit ... */
size_t n; do { gptps_step(e, &n); } while (n);   /* runs on THIS thread */
```

Pair it with **`gptps_set_allocator()`** to take GPTPS off the libc heap entirely —
point it at a static pool (SQLite-style; covers all core allocation). Together,
MANUAL mode + a custom allocator are the **bare-metal shape**: zero threads, fixed
RAM. The only thing a real MCU/RTOS port adds is a HAL backend (`hal_<target>.c`)
for the mutex/clock/flag primitives — MANUAL mode never calls `gptps_thread_start`
or `cond_wait`. Worked end-to-end in [`examples/embedded.c`](examples/embedded.c).

> Caveat: a MANUAL task runs to completion on your thread, so a wall-clock timeout
> can't preempt it — cooperative tasks should poll `gptps_is_cancelled()` /
> `gptps_deadline_ms()`. For hard kill/timeout, use an out-of-process executor.

## Resource budgets, failures, add-ons

- **Admission:** each task type declares a rough cost (`mem` / `gpu` / duration). The core
  starts a task only if it fits the live budget — not an all-or-nothing cap. `max_concurrent_tasks=1`
  is strictly sequential; `>1` is concurrent.
- **Scheduling:** the dispatcher admits the **highest-priority** pending task that fits the live
  budget. A too-large task does **not** head-of-line-block — smaller work behind it *backfills*
  (skip-to-fit), while a bounded *reservation* keeps the skipped task from starving (it's admitted
  once enough budget frees). Set priority via `gptps_set_task_priority()` or config; tune the
  reservation with `[scheduler] reserve_after_skips`.
- **Failure policy** (per task, overridable): `timeout_seconds`, `max_retries`,
  `retry_backoff_seconds`, `on_failure` = `dead_letter` (default) / `drop` / `requeue`.
- **Dead letter:** tasks that exhaust retries (or that a constraint denies) are retained.
  `gptps_dead_letter_drain()` hands each back to a callback — with the engine lock released, so
  the callback may re-submit to retry — and empties the list (`gptps_shutdown()` frees the rest).
- **Durability (optional):** `addons/durable_queue.c` journals submissions to disk (fsync before
  enqueue) and replays survivors after a crash — at-least-once delivery. See `addons/README.md`.
- **Live dashboard (optional):** a portable real-time terminal UI with live metrics, a
  per-task table, a scrollable event log, a settings editor, and hotkeys — see
  [Live terminal dashboard](#live-terminal-dashboard) above.
- **Add-ons** keep the core small. Task logic, transports, GPU quotas, rate limits,
  priority, time-of-day windows, analytics sinks — all live in **add-ons** that attach over a
  versioned host-table ABI, in-process (C ABI) or out-of-process (any language). See
  `tests/addon_demo.c` for a minimal add-on.

## Project layout

```
gptps/
├── include/
│   ├── gptps.h          ← the public API (start here)
│   └── gptps_hal.h      ← internal platform-abstraction interface
├── src/                 ← the library
│   ├── engine.c         core: dispatcher, queue, admission, failure engine, loader
│   ├── config.c         config model + hardware auto-tune
│   ├── config_toml.c    TOML-subset config-file parser
│   ├── hal_posix.c      POSIX backend (threads, clock, dynload, detection)
│   └── exec_oop_posix.c out-of-process + external-program executors
├── addons/              ← optional modules on the public API (durable_queue, gpu_quota, wasm_exec, tui)
├── examples/            ← runnable examples (demo, config_file, external_program, dashboard, embedded, wasm_program)
├── gptps.example.toml   ← annotated sample config file
├── docs/ARCHITECTURE.md ← how it works inside
├── tests/               ← CTest suite (engine, failure, oop, program, constraint, ...)
├── tools/amalgamate.sh  ← generates the single-file gptps.c + gptps.h
├── CMakeLists.txt
└── .github/workflows/ci.yml   Linux + macOS + Windows build/test, ASan/UBSan, ThreadSanitizer
```

## Status

Working today (tested + ThreadSanitizer-clean): the engine, all three executors,
result delivery, retries/timeout/dead-letter + dead-letter drain, priority scheduling
with skip-to-fit + reservation, accurate cgroup v2 memory enforcement (with RLIMIT_AS
fallback), the add-on loader + ABI, constraints + observers, TOML config-file loading
(limits + scheduler + per-task overrides + add-on auto-load), the unified settings
registry (typed get/set + validation + round-trip persistence + add-on-extensible),
**single-threaded MANUAL mode** (`gptps_step`) and a **custom-allocator hook** for
embedded / bare-metal hosts, the **live terminal dashboard** (with the settings editor),
the crash-durable queue, GPU-quota, and WASM-executor add-ons, the examples, CMake + CI +
single-file amalgamation.

Platforms (all CI-verified): **Linux** and **macOS** are full. **Windows** (Win32 HAL
via `src/hal_win.c`) runs the engine, scheduler, config, the in-process and
external-program executors (`CreateProcess` + Job Object), and the add-on loader; only
`GPTPS_EXEC_OOP` is POSIX-only, since it forks an in-process function (no `fork()` on
Windows — use `GPTPS_EXEC_PROGRAM` there for isolated, killable, memory-capped work).

Running WebAssembly works today two ways: via `GPTPS_EXEC_PROGRAM` + a wasm runtime CLI
(`examples/wasm_program.c`), or the `wasm_exec` add-on with a pluggable runtime. Optional
future work: a bundled default wasm runtime so neither a CLI nor an adapter is needed.

## Design notes

The core is deliberately small and general: a mechanism-only engine with four add-on seams
(task / constraint / transport / observer). Anything specific — GPU quotas, rate limits,
priority, time-of-day windows — is a **constraint add-on**, so the core stays minimal while
the variety lives in add-ons. The novel piece is single-process *self-throttling* admission:
"can my own process afford to start this task right now, given my own remaining budget?"

For the full internals — concurrency model, dispatch loop, scheduler, executors, HAL, and the
add-on ABI — see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## License

MIT — see [LICENSE](LICENSE). No third-party code is vendored; the TOML parser, journal, and
all add-ons are first-party.
