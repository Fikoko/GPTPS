# GPTPS — General Purpose Task Processing System

**An embeddable, in-process C99 task processor — the "SQLite of task processors."**
Link one library, register a task, submit work. GPTPS runs it on a worker pool under
declared resource budgets, with retries / timeouts / dead-letter, and gives you the
result back. No server, no broker, no mandatory dependency. Runs on Linux and macOS.

---

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

## Build & link

**Option A — single file (easiest).** Generate the amalgamation and drop two files into your project:

```sh
sh tools/amalgamate.sh out          # writes out/gptps.c and out/gptps.h
cc -std=c99 yourapp.c out/gptps.c -Iout -lpthread -ldl   # Linux
cc -std=c99 yourapp.c out/gptps.c -Iout -lpthread        # macOS (dlopen is in libSystem)
```

**Option B — CMake** (builds `libgptps`, the tests, and the demo):

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure   # run the test suite
./build/demo                                  # run the example
```

More runnable examples in [`examples/`](examples/): `demo` (in-process tasks + events),
`config_file` (tuning from a TOML file), `external_program` (run any binary as a task), and
`wasm_program` (run a `.wasm` module via a wasm runtime CLI — see WebAssembly below), and
`dashboard` (a live terminal UI — counts, per-task table, recent log, hotkeys to add tasks).

**Install / consume.** `cmake --install build --prefix <dir>` installs the header, static
library, a CMake package config, and a pkg-config file. Downstream projects then use either
`find_package(gptps)` → link `gptps::gptps`, or `pkg-config --cflags --libs gptps`.

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
| `gptps_load_addon(e, path)` | load a shared-library add-on over the stable ABI |
| `gptps_shutdown(e)` | drain in-flight + queued work, then free |

Inside a task you get a `gptps_ctx *`: `gptps_payload()`, `gptps_is_cancelled()` (poll it
for cooperative timeout), `gptps_result_set()` / `gptps_result_set_nocopy()`.

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
- **Live dashboard (optional):** `addons/tui.c` is a portable real-time terminal UI (counts,
  per-task table, recent log, hotkeys to submit tasks) — global + per-task configurable.
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
├── examples/            ← runnable examples (demo, config_file, external_program)
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
(limits + scheduler + per-task overrides + add-on auto-load), the crash-durable queue,
GPU-quota, and WASM-executor add-ons, the demo, CMake + CI + single-file amalgamation.

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
