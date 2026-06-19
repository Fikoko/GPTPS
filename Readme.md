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

## API at a glance

| Call | Purpose |
|---|---|
| `gptps_open(path, &e)` / `gptps_open_ex(cfg, &e)` | create an engine (auto-tunes workers + memory budget) |
| `gptps_register_task(e, &def)` | register a task type (in-process fn **or** external program) |
| `gptps_submit(e, name, payload, len, &handle)` | enqueue work |
| `gptps_set_event_cb(e, cb, ud)` | observe lifecycle events (results arrive on `FINISHED`) |
| `gptps_register_constraint(e, fn, ud)` | gate admission (rate limit, quota, time window) |
| `gptps_register_observer(e, cb, ud)` | extra event sink (e.g. analytics) |
| `gptps_load_addon(e, path)` | load a shared-library add-on over the stable ABI |
| `gptps_shutdown(e)` | drain in-flight + queued work, then free |

Inside a task you get a `gptps_ctx *`: `gptps_payload()`, `gptps_is_cancelled()` (poll it
for cooperative timeout), `gptps_result_set()` / `gptps_result_set_nocopy()`.

## Executor kinds (per task, via `def.exec`)

| Kind | Runs as | Enforcement | Use for |
|---|---|---|---|
| `GPTPS_EXEC_INPROC` | your C function, in-process | cooperative cancel (advisory) | fast, trusted work |
| `GPTPS_EXEC_OOP` | the same C function in a forked child | OS memory cap + hard-kill on timeout | isolation / hard limits |
| `GPTPS_EXEC_PROGRAM` | an external program (`def.argv`) | OS cap + whole-group hard-kill | any language / any binary; payload→stdin, stdout→result |

## Resource budgets, failures, add-ons

- **Admission:** each task type declares a rough cost (`mem` / `gpu` / duration). The core
  starts a task only if it fits the live budget — not an all-or-nothing cap. `max_concurrent_tasks=1`
  is strictly sequential; `>1` is concurrent.
- **Failure policy** (per task, overridable): `timeout_seconds`, `max_retries`,
  `retry_backoff_seconds`, `on_failure` = `dead_letter` (default) / `drop` / `requeue`.
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
│   ├── hal_posix.c      POSIX backend (threads, clock, dynload, detection)
│   └── exec_oop_posix.c out-of-process + external-program executors
├── examples/demo.c      ← runnable example (the quick start above)
├── tests/               ← CTest suite (engine, failure, oop, program, constraint, ...)
├── tools/amalgamate.sh  ← generates the single-file gptps.c + gptps.h
├── CMakeLists.txt
└── .github/workflows/ci.yml   Linux + macOS build/test + ThreadSanitizer
```

## Status

Working today (Linux + macOS, tested + ThreadSanitizer-clean): the engine, all three
executors, result delivery, retries/timeout/dead-letter, the add-on loader + ABI,
constraints + observers, the demo, CMake + CI + single-file amalgamation.

In progress: TOML config-file loading (the model + auto-tune work today; file parsing is
being added), richer scheduling (priority / skip-to-fit), cgroups memory enforcement,
durable queue / GPU / WASM add-ons, and a Windows backend.

## Design notes

The core is deliberately small and general: a mechanism-only engine with four add-on seams
(task / constraint / transport / observer). Anything specific — GPU quotas, rate limits,
priority, time-of-day windows — is a **constraint add-on**, so the core stays minimal while
the variety lives in add-ons. The novel piece is single-process *self-throttling* admission:
"can my own process afford to start this task right now, given my own remaining budget?"
