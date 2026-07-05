# GPTPS — General Purpose Task Processing System

**An embeddable, in-process C99 task processor (Control Plane) aimed for modularity, portability, and scalability — the "General Purpose Task Processing System"**
Link one library, register a task, submit work. GPTPS runs it on a worker pool under
declared resource budgets, with retries / timeouts / dead-letter, and gives you the
result back — plus an optional **live terminal dashboard** to watch and steer it. No
server, no broker, no mandatory dependency. Runs on Linux, macOS, and Windows, and can
even run **single-threaded with no libc heap** for embedded / bare-metal targets.

Long-running **services** are supervised (restart-on-exit, stopped cleanly at shutdown),
and scale is **opt-in and by composition** — shard across engines, route across worker
processes, or swap the scheduler — never baked into the mechanism-only core.

---

## Contents

- [Quick start](#quick-start) · [Getting started](#getting-started) — build → run → embed
- [API at a glance](#api-at-a-glance) · [Common tasks](#common-tasks) — step-by-step recipes
- [Long-running services](#long-running-services) · [Scaling](#scaling-opt-in-by-composition) — services + scale-up/out
- [Configuration file](#configuration-file-optional) · [Settings](#settings-runtime-introspectable-persistable)
- [Manage tasks at runtime](#manage-tasks-at-runtime-control-plane) · [Live terminal dashboard](#live-terminal-dashboard) · [Executor kinds](#executor-kinds-per-task-via-defexec)
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

Scaling is opt-in and never in the default build — shard across engines, route across
worker processes, or swap the scheduler (see [Scaling](#scaling-opt-in-by-composition)).
The one build-time knob is `-DGPTPS_HAL_FAST=ON` for a platform-optimized HAL (adaptive
mutexes on glibc; OFF by default keeps the portable pthread path).

**Building on Windows with mingw-w64.** The core is pure C99 and the Win32 backend
(`hal_win.c` + `exec_win.c`) is selected automatically; mingw-w64 gcc is a first-class,
CI-tested toolchain (native PE binaries, full `<windows.h>`, `__atomic` support). Using
[MSYS2](https://www.msys2.org/) with the **UCRT64** environment:

```sh
# in the MSYS2 UCRT64 shell
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug   # gcc + ninja is auto-detected
cmake --build build
ctest --test-dir build --output-on-failure              # full suite (should be 100%)
```

The single-file amalgamation builds the same way (`gnu99` for the `<windows.h>` GNU
extensions): `sh tools/amalgamate.sh && cc -std=gnu99 -I amalgamation amalgamation/gptps.c
myapp.c -o myapp.exe` — no `-lpthread`/`-ldl` needed (the Win32 HAL uses the OS threads
and `LoadLibrary` directly). The native **MSVC** (`cl.exe`) toolchain is also supported —
open the folder in Visual Studio or `cmake -S . -B build` with the default VS generator.

**2. See it run — the live dashboard.** Run it in a real terminal (it needs an interactive
TTY; with no TTY it just prints a line and exits, which is how CI runs it headless):

```sh
./build/example_dashboard    # keys: w/f submit · t tasks · l dead-letter · s settings · m KPI · p pause · ? help · q quit
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
| `gptps_define_resource(e, name, budget)` · `gptps_set_task_resource_cost(e, name, res, n)` | declare a named admission budget (GPU/IO/seats/quota) and a task's per-item cost against it |
| `gptps_submit(e, name, payload, len, &handle)` / `gptps_submit_ex(…, &opts, …)` | enqueue work (`_ex`: per-submit priority / policy / deadline) |
| `gptps_cancel(e, handle)` | cancel one queued / in-flight item |
| `gptps_task_count/get_info/exists(e, …)` | enumerate / introspect registered task types |
| `gptps_set_task_enabled(e, name, on)` | pause / resume a task type (reversible) |
| `gptps_clone_task(e, src, dst)` | duplicate a task type under a new name |
| `gptps_unregister_task(e, name, flags)` | remove a task type (reject-if-busy / drain / cancel) |
| `gptps_define_global/define_task_setting(e, …)` | declare a custom typed global / per-task setting |
| `gptps_task_setting_int/str(ctx, key, …)` | read this task's per-task setting from inside `run()` |
| `gptps_set_event_cb(e, cb, ud)` | observe lifecycle events (results arrive on `FINISHED`) |
| `gptps_register_constraint(e, fn, ud)` | gate admission (rate limit, quota, time window) |
| `gptps_set_scheduler(e, fn, ud)` | swap the admission *ordering* (deadline-first, fair-share, …) over the fixed mechanism |
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

**8. Run a supervised background service.** Flag the task `GPTPS_TASK_SERVICE`: its
`run()` loops until told to stop and is restarted if it exits — see
[Long-running services](#long-running-services).

**9. Scale beyond one engine.** Shard across engines behind a router, ship work to
worker processes, or swap the scheduler — all opt-in, all on the public API, no core
change — see [Scaling](#scaling-opt-in-by-composition).

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
- **Generic, no glue:** declare your own typed knobs at runtime — `gptps_define_global(e,
  "app.max_upload_mb", GPTPS_SETTING_UINT, "10", "0..4096", 0)` for a global, or
  `gptps_define_task_setting(e, "quality", GPTPS_SETTING_UINT, "75", "0..100", 0)` to
  materialize `tasks.<name>.quality` on every task. A `run()` reads its own value with
  `gptps_task_setting_int(ctx, "quality", &q)`. The engine stores and validates them; both
  round-trip through TOML and appear in the editor.
- **Editor:** the `tui` add-on includes a live **Settings pane** (`s`) to browse/edit/save.

## Manage tasks at runtime (control plane)

The registry is itself live and mutable — enumerate, pause, clone, and remove task types
without recompiling or restarting:

```c
size_t n = gptps_task_count(e);                       /* enumerate for a UI ... */
gptps_task_info ti = { .struct_size = sizeof ti };
gptps_task_get_info(e, 0, &ti);                       /* name, exec, prio, queued/running/dead */

gptps_set_task_enabled(e, "resize", 0);               /* pause: reject new submits, reversibly */
gptps_clone_task(e, "resize", "resize_hi");           /* duplicate, then retune the copy */
gptps_unregister_task(e, "resize", GPTPS_REMOVE_DRAIN);   /* finish in-flight work, then remove */
```

- **Removal policy** (the `flags`): `GPTPS_REMOVE_REJECT_IF_BUSY` (default — refuse with
  `GPTPS_E_BUSY` while work is outstanding), `GPTPS_REMOVE_DRAIN` (stop new submits, let
  queued + in-flight finish, then free), or `GPTPS_REMOVE_CANCEL` (drop queued, cancel
  in-flight, then free). A removed name is free to re-register; its `tasks.<name>.*` settings
  are torn down; retained dead-letter items survive and stay drainable.
- **Behavior still arrives in code.** These calls own *configuration and lifecycle*. New
  in-process logic comes from a `run` fn (code or an add-on); a `GPTPS_EXEC_PROGRAM` task,
  though, is fully creatable at runtime (and from the TUI) since its behavior is an external
  argv. Add-ons get the same control plane via the host-table ABI.

## Long-running services

Some work isn't a one-shot task — it's a resident loop (a poller, a listener, a metrics
collector) that should run until the process stops and come back if it crashes. Flag the
task type `GPTPS_TASK_SERVICE` and submit it like any task; the engine supervises it:

```c
static gptps_status poller(gptps_ctx *ctx, void *ud) {
    (void)ud;
    while (!gptps_is_cancelled(ctx)) { poll_once(); }   /* loop until told to stop */
    return GPTPS_OK;
}

gptps_task_def d = { .struct_size = sizeof d, .name = "metrics",
                     .run = poller, .exec = GPTPS_EXEC_INPROC,
                     .flags = GPTPS_TASK_SERVICE };
d.default_cost.struct_size = sizeof d.default_cost;
d.default_policy.struct_size = sizeof d.default_policy;
d.default_policy.retry_backoff_seconds = 2;             /* restart delay after a crash */
gptps_register_task(e, &d);

gptps_handle h;
gptps_submit(e, "metrics", NULL, 0, &h);   /* start one instance (submit N for a pool) */
```

- **Supervised restart.** When the loop exits, the engine restarts the instance after
  `retry_backoff_seconds` — crash-restart supervision, for free. The failure policy is
  normalized (restart-on-exit, no timeout) so a config or settings edit can't un-service it.
- **Stop it.** The submit handle stays valid across restarts, so `gptps_cancel(e, h)`
  stops that one instance for good; `gptps_unregister_task` stops the whole type; and
  **`gptps_shutdown` stops running services** (raising their cooperative cancel flag) so a
  resident service never hangs teardown. Non-service in-flight work still drains gracefully.
- **Restart-always vs. on-failure.** By default a service is "always up" (even a clean
  `GPTPS_OK` return restarts it). Add `GPTPS_TASK_RETIRE_ON_OK` for the `Restart=on-failure`
  semantic: a clean return retires the instance; only a failure restarts it.
- v1 services are `GPTPS_EXEC_INPROC`, `GPTPS_RUN_THREADED`, and have no timeout.

## Scaling (opt-in, by composition)

The core is a single-writer engine — one lock, one dispatcher — which is simple, correct,
and the per-node throughput ceiling. Rather than complicate the core, GPTPS scales by
**composing modules on top of it**: you pay for scale only when you reach for it, and the
default stays small and portable. None of the below touches the mechanism-only core.

**Scale up — shard across engines (`gptps_pool`).** Run N independent engines (each its own
lock + dispatcher + worker pool) behind a router:

```c
gptps_pool *pool = gptps_pool_open(4, &cfg);          /* 4 independent shards */
gptps_pool_register_task(pool, &def);                 /* same task type on every shard */
gptps_pool_submit(pool, "resize", buf, len, &h);      /* round-robin */
gptps_pool_submit_keyed(pool, tenant_id, "resize", buf, len, &h);  /* per-key affinity */
gptps_pool_cancel(pool, h);
gptps_pool_close(pool);
```

The router's only shared state is a round-robin cursor (keyed routing is lock-free); each
shard is a full engine. [`examples/bench_pool`](examples/bench_pool.c) measures it: on a
32-core box, aggregate tiny-task throughput rose from ~15k/s at 1 shard to ~290k/s at 8
(≈19×) — the single-writer ceiling, then composition breaking past it.

**Scale out — worker processes (`gptps_xport`).** Fork N persistent worker *processes* and
ship each submit to one over IPC, marshalling the result back — work runs in a separate
address space (crash-isolated), and swapping the local socketpair for a TCP socket reaches
another machine (POSIX only, like `EXEC_OOP`):

```c
gptps_xport *xp = gptps_xport_open(4, my_handler, ud);   /* 4 worker processes */
gptps_xport_submit(xp, "resize", buf, len, &res, &rlen, &task_status);
gptps_xport_close(xp);
```

**Swap the scheduling discipline (`gptps_set_scheduler`).** The admission *mechanism*
(skip-to-fit, budget, starvation guard) is fixed; the *ordering* is a hook. Return a score
per item and the dispatcher admits the highest that fits — so deadline-first, per-tenant
fair-share, cost-aware, or aging disciplines compose without a core fork (default is
priority/FIFO):

```c
static int64_t earliest_deadline_first(const gptps_sched_input *in, void *ud) {
    (void)ud; return -(int64_t)in->enqueue_ms;   /* older enqueue = higher score */
}
gptps_set_scheduler(e, earliest_deadline_first, NULL);
```

**Faster locks (`-DGPTPS_HAL_FAST`).** An opt-in build knob swaps in adaptive
(spin-then-block) mutexes on glibc — a latency knob for the engine's contended critical
sections; OFF by default keeps the portable pthread HAL. The HAL is a module boundary, so a
downstream can drop in its own platform-optimized backend.

`gptps_pool` and `gptps_xport` are add-ons ([`addons/`](addons/)) built entirely on the
public API — the proof that scaling here needs no core change.

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

keys: [r] Resize  [t] Thumbnail   ·  ? help  s settings  t tasks  l dead-letter  m kpi  p pause  q quit
```

- **Live metrics:** throughput, an in-flight gauge, cumulative counts, and a per-task
  table (runs / ok / fail / dead / success-rate / average latency).
- **Interactive:** hotkeys submit tasks; `k`/`j` scroll the event log; `m` dials the
  dashboard's own CPU/RAM cost (minimal/normal/full) live; `p` pauses; `s` opens the live
  **settings editor**; `?` shows a help overlay of every key.
- **Task control plane:** `t` opens a **task manager** — list every type with live
  queued/running/dead counts, inspect one to edit its settings inline, pause/resume (`a`),
  clone (`c`), create a `GPTPS_EXEC_PROGRAM` task from a typed name + argv (`n`), or delete
  with a confirm dialog (`d`) that shows the outstanding count and offers drain or
  cancel-force. `l` opens a **dead-letter** view to bulk re-submit or discard retained
  failures.
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
| `GPTPS_EXEC_OOP` | the same C function in a forked child | memory cap + hard-kill on timeout **or cancel** | POSIX only (needs `fork`) |
| `GPTPS_EXEC_PROGRAM` | an external program (`def.argv`); payload→stdin, stdout→result | memory cap + hard-kill on timeout **or cancel** | all (POSIX `fork`+exec; Windows `CreateProcess` + Job Object) |

The out-of-process executors are fully **cancellable**: `gptps_cancel` (and
`gptps_unregister_task(…, CANCEL)`) hard-kill a running child in bounded time — even one
with no timeout, which used to run unstoppably. The POSIX program executor pumps the
payload to stdin and reads stdout *concurrently* (a single `poll` loop), so a large payload
through a streaming child never deadlocks.

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
- **Runtime task management:** enumerate, pause/resume, clone, and unregister task types
  live (`gptps_task_*`, `gptps_unregister_task` with reject-if-busy / drain / cancel) — the
  control plane behind the dashboard's task manager. See
  [Manage tasks at runtime](#manage-tasks-at-runtime-control-plane).
- **Live dashboard (optional):** a portable real-time terminal UI with live metrics, a
  per-task table, a scrollable event log, a settings editor, a **task manager** + dead-letter
  panes, and hotkeys — see [Live terminal dashboard](#live-terminal-dashboard) above.
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
│   ├── engine.c         core: dispatcher, queue, admission, scheduler, failure engine, loader
│   ├── settings.c       typed settings registry;  alloc.c  custom-allocator seam
│   ├── config.c         config model + hardware auto-tune;  config_toml.c  TOML parser
│   ├── hal_posix.c      POSIX backend (threads, clock, dynload, detection);  hal_win.c  Win32 backend
│   └── exec_oop_posix.c out-of-process + external-program executors;  exec_win.c  Win32 executor
├── addons/              ← optional modules on the public API: gptps_pool (scale-up), gptps_xport (scale-out),
│                          gptps_orch (dependencies), durable_queue, gpu_quota, wasm_exec, tui
├── examples/            ← runnable examples (demo, config_file, task_control, external_program, dashboard,
│                          embedded, wasm_program, bench_pool)
├── gptps.example.toml   ← annotated sample config file
├── docs/ARCHITECTURE.md ← how it works inside
├── tests/               ← CTest suite (engine, failure, oop, program, constraint, ...)
├── tools/amalgamate.sh  ← generates the single-file gptps.c + gptps.h
├── CMakeLists.txt
└── .github/workflows/ci.yml   Linux/macOS/Windows (mingw + MSVC), i386 + s390x + freestanding, ASan/UBSan + TSan
```

## Status

Working today (tested + ThreadSanitizer-clean): the engine, all three executors,
result delivery, retries/timeout/dead-letter + dead-letter drain, priority scheduling
with skip-to-fit + reservation, accurate cgroup v2 memory enforcement (with RLIMIT_AS
fallback), the add-on loader + ABI, constraints + observers, TOML config-file loading
(limits + scheduler + per-task overrides + add-on auto-load), the unified settings
registry (typed get/set + validation + round-trip persistence + add-on-extensible),
**single-threaded MANUAL mode** (`gptps_step`) and a **custom-allocator hook** for
embedded / bare-metal hosts, **supervised long-running services** (`GPTPS_TASK_SERVICE`),
the **pluggable scheduler seam** (`gptps_set_scheduler`), **scale-up** (`gptps_pool`
shards) and **scale-out** (`gptps_xport` worker processes) add-ons, the optional
platform-optimized HAL (`-DGPTPS_HAL_FAST`), the **live terminal dashboard** (with the
settings editor), the crash-durable queue, GPU-quota, and WASM-executor add-ons, the
examples + benchmark, CMake + CI + single-file amalgamation.

Platforms (all CI-verified): **Linux** and **macOS** are full. **Windows** (Win32 HAL
via `src/hal_win.c`) runs the engine, scheduler, config, the in-process and
external-program executors (`CreateProcess` + Job Object), and the add-on loader; only
`GPTPS_EXEC_OOP` is POSIX-only, since it forks an in-process function (no `fork()` on
Windows — use `GPTPS_EXEC_PROGRAM` there for isolated, killable, memory-capped work).

Running WebAssembly works today two ways: via `GPTPS_EXEC_PROGRAM` + a wasm runtime CLI
(`examples/wasm_program.c`), or the `wasm_exec` add-on with a pluggable runtime. Optional
future work: a bundled default wasm runtime so neither a CLI nor an adapter is needed.

## Design notes

The core is deliberately small and general: a mechanism-only engine with five add-on seams
(task / constraint / scheduler / transport / observer). Anything specific — GPU quotas, rate
limits, priority, time-of-day windows — is a **constraint add-on**; the admission *order* is
a **scheduler** hook; scale-up (`gptps_pool`) and scale-out (`gptps_xport`) are **transport /
composition** add-ons — so the core stays minimal while the variety, and the scaling, live on
the seams. The novel piece is single-process *self-throttling* admission: "can my own process
afford to start this task right now, given my own remaining budget?"

For the full internals — concurrency model, dispatch loop, scheduler, executors, HAL, and the
add-on ABI — see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## License

MIT — see [LICENSE](LICENSE). No third-party code is vendored; the TOML parser, journal, and
all add-ons are first-party.
