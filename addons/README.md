# GPTPS add-ons

Optional modules built on the public GPTPS API. They are **not** part of the core
library — link only what you use.

There are two flavours of add-on:

- **dlopen add-ons** — shared objects loaded at runtime over the host-table ABI
  (`gptps_load_addon` / `addons = [...]` in config). They export `gptps_addon_init`
  and call the core only through the passed `gptps_api_routines` table. See
  `tests/addon_demo.c` for the minimal shape.
- **compiled-in modules** — plain source you build into your application, using
  the public API and the observer/constraint seams. The durable queue below is one.

## durable_queue — crash-durable submission

`durable_queue.c` / `durable_queue.h`. Submit through `gptps_dq_submit()` instead
of `gptps_submit()`: the `(task, payload)` is written to an append-only journal and
`fsync`'d **before** the task is enqueued, so a crash afterward is recoverable. The
module registers an observer to mark a record complete once its task finishes or is
dead-lettered; `gptps_dq_recover()` re-submits anything a prior run left unfinished.

- **Guarantee:** at-least-once — task bodies must be idempotent.
- **Caveat:** `on_failure = drop` failures emit no terminal event, so they stay in
  the journal and replay on recover.
- **Ordering:** call `gptps_dq_close()` **after** `gptps_shutdown()` (the engine has
  no unregister-observer call, so the queue must outlive event delivery).
- **Portability:** Linux/macOS/Windows (via the `addon_compat` mutex + fsync shim).

Build:

```sh
cc -std=c99 gptps.c addons/durable_queue.c yourapp.c -lpthread -ldl
```

## gpu_quota — GPU-unit admission quota

`gpu_quota.c` / `gpu_quota.h`. Caps total in-flight GPU usage so the engine never
*starts* more GPU work than your budget allows.

**Read this one first if you are learning the named-resource API.** It is a thin
wrapper over `gptps_define_resource` / `gptps_set_task_resource_cost` /
`gptps_resource_usage` and carries no counter, no lock, and no bookkeeping —
about 90 lines, most of them comment. "GPU units" is not a mechanism, it is a
*name* for one: swap the string and the same code caps licence seats, IO
bandwidth, or a per-tenant quota. That is why the core has no idea what a GPU is.

- **Scope:** admission-level *oversubscription prevention*, not driver-level VRAM
  enforcement (that needs vendor APIs / real hardware). Pair it with an OOP/cgroup
  memory cap for hard limits.
- **Model:** units are fixed per task TYPE, declared after registration.
- **Ordering:** the engine owns the budget, so `gptps_gpu_quota_close()` only
  releases the handle.
- **Portability:** no platform code at all.

```c
gptps_gpu_quota *q = gptps_gpu_quota_install(engine, /*total*/ 8, /*unused*/ 0);
gptps_register_task(engine, &def);
gptps_gpu_quota_set_task_units(q, "infer", 2);   /* each item costs 2 units */
gptps_shutdown(engine);
gptps_gpu_quota_close(q);
```

Before ABI 2.0 this was a constraint + observer pair with its own counter, keyed
off a `gptps_cost.gpu_units` field. It had to infer releases from the event
stream by task name, which leaked the budget on any terminal path the observer
did not see — and there was one. Reserving through the core deletes the failure
mode instead of patching it.

## wasm_exec — run WebAssembly modules as tasks (bring-your-own-runtime)

`wasm_exec.c` / `wasm_exec.h`. A `.wasm` module is portable, sandboxed task code —
"write one task, run it on any hardware." A wasm *interpreter*, though, is a heavy
dependency, so this add-on owns the GPTPS-side integration (module-as-task:
admission, cost/priority, retries/timeout, result delivery, and — in OOP mode —
OS-enforced memory caps + hard-kill) and leaves the **runtime pluggable**: you
supply one `gptps_wasm_run_fn` that drives wasm3 / wasmtime / WAMR / etc.

- **Why pluggable:** keeps the core dependency-free and portable; any runtime drops
  in. (Zero-glue alternative: `GPTPS_EXEC_PROGRAM` with a runtime CLI, e.g.
  `argv = {"wasmtime", "module.wasm", NULL}`.)
- **Modes:** in-process (cooperative cancel; all platforms) or OOP (forked,
  OS-capped, hard-killed; POSIX only — the runtime must be fork-safe).
- **Ordering:** `gptps_wasm_close()` after `gptps_shutdown()`.
- **Portability:** Linux/macOS/Windows (the add-on itself has no platform calls).

```c
/* you implement run_wasm3() against your runtime of choice */
gptps_wasm *w = gptps_wasm_install(engine, run_wasm3, NULL);
gptps_wasm_register(w, "resize", "/modules/resize.wasm", /*oop*/ 1, NULL, NULL);
gptps_submit(engine, "resize", jpeg, jpeg_len, &h);   /* runs the module, sandboxed */
gptps_shutdown(engine);
gptps_wasm_close(w);
```

A minimal `gptps_wasm_run_fn` skeleton against wasm3 (illustrative):

```c
static gptps_status run_wasm3(const char *path, const void *in, size_t n,
                              void **out, size_t *out_n, void *ud) {
    /* m3_NewEnvironment / ParseModule(path) / LoadModule / FindFunction("run")
       -> write `in` into wasm memory, call it, copy the result region into a
       malloc'd buffer -> *out/*out_n. Return GPTPS_OK or an error. */
}
```

## tui — real-time terminal dashboard

`tui.c` / `tui.h`. A live, portable terminal dashboard over a running engine,
built on the observer seam — ANSI/VT escapes only (no ncurses; Windows VT enabled
automatically). Panes/metrics:

- header: uptime + **throughput** (done/s);
- counts (queued/started/finished/failed/retried/dead) + an **in-flight gauge bar**;
- per-task table: run / ok / fail / dead, **success rate (ok%)**, and **average
  queue→finish latency (ms)**, plus the task's hotkey;
- a **scrollable** recent-events log (timestamped) — `k`/`j` scroll older/newer
  (keyboard, so it's portable everywhere; mouse reporting isn't universal, so it's
  intentionally not used).

- **Global settings** (`gptps_tui_config`): refresh rate, color, interactivity,
  which panes to show, title, output stream.
- **Local settings** (`gptps_tui_add_task`): per-task label + a **hotkey that
  submits work straight from the dashboard**.
- **Cost is a budgeted knob** — the dashboard runs on the engine's worker threads,
  so its own CPU/RAM is tunable, **at runtime**:
  - **KPI level** (`gptps_tui_set_kpi`, or `m` live): `MINIMAL` = counts only
    (~no per-event work, no latency ring); `NORMAL` = + per-task table + recent
    log; `FULL` = + per-handle latency (allocates a ring; **freed when you drop
    below FULL**). `latency_window` sizes that ring.
  - **Cadence / mode** (`gptps_tui_set_mode`, or `p` live): `CONTINUOUS` (real-time),
    `ON_DEMAND` (repaint only when state changed or a key), `PAUSED` (frozen).
  - `gptps_tui_set_refresh()` changes the redraw interval live.
  - `gptps_tui_snapshot()` renders **one** frame on demand (the "once" / per-ask
    case) — independent of the live loop, handy for logging or a signal handler.
- **Testable split:** `gptps_tui_render()` returns the frame as a string and
  `gptps_tui_press()` applies a key — both unit-tested headlessly; `gptps_tui_run()`
  is the blocking live loop (and self-skips without a TTY).
- **Ordering:** `gptps_tui_close()` after `gptps_shutdown()`. **Portable** (POSIX
  termios + Windows console).

Live keys: hotkeys submit their task · `k`/`j` scroll · `m` cycles KPI level ·
`p` pauses/resumes · `q` quits.

```c
gptps_tui_config cfg = { sizeof cfg };
cfg.title = "image pipeline";
gptps_tui *ui = gptps_tui_install(engine, &cfg);
gptps_tui_add_task(ui, "resize", "Resize", 'r', NULL, 0);  /* [r] submits resize */
gptps_tui_run(ui);          /* live dashboard until 'q' (no-op without a TTY) */
gptps_shutdown(engine);
gptps_tui_close(ui);
```

See [`examples/dashboard.c`](../examples/dashboard.c) for a runnable demo.

```c
gptps_dq *dq = gptps_dq_open(engine, "queue.journal");
gptps_dq_recover(dq);                              /* replay prior-run survivors */
gptps_dq_submit(dq, "resize", buf, len, &handle);  /* durable submit */
/* ... */
gptps_shutdown(engine);
gptps_dq_close(dq);
```
