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
| **Modularity** — handle all outcomes | Mechanism-only core + four *called* seams (task / constraint / observer / scheduler) and one *composed* pattern (transport). Domain policy lives in add-ons, not the core. |
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
- `src/settings.c` — the typed settings registry (§10).
- `src/alloc.c` — the allocator seam, `gptps_set_allocator` (§3.2).
- `src/hal_posix.c` / `src/hal_win.c` — the POSIX and Win32 HAL backends.
- `src/exec_oop_posix.c` — the out-of-process and external-program executors;
  `src/exec_win.c` is the external-program executor on Windows.
- `src/gptps_internal.h` — prototypes shared between the core's own files; not
  installed.
- `addons/` — optional modules built on the public API (e.g. the durable queue).

---

## 3. Concurrency model

GPTPS has two **execution modes**, selected at open via `gptps_config.mode`:

- **`GPTPS_RUN_THREADED`** (default) — the dispatcher + worker pool below.
- **`GPTPS_RUN_MANUAL`** — **no threads.** The caller drives the engine via
  `gptps_step()`, which runs the same scheduling pass and then executes the
  admitted tasks inline on the calling thread. This is the portable path for
  single-threaded hosts and bare-metal; it needs only the HAL mutex/clock/flag
  primitives, never `gptps_thread_start`/`cond_wait`. See §3.1.

Steps 1–4 of the loop below are factored into one function, **`engine_pass()`**,
shared verbatim by the threaded dispatcher and the manual pump — so admission,
retry, dead-letter, and starvation semantics are identical across modes; only the
*driver* (a thread that sleeps on a condvar vs. a caller that returns) differs.

### Threaded mode

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
   (ok → free; failed with retries left → a pass-local list, its `RETRIED` buffered;
   failed & exhausted → `on_failure`).
2. **Promote `delayed`** — move backoff-ready items back to `intake`. Then **2b.**
   step 1's retries join `delayed`, only now, so none is promotable in the pass that
   announced it: its `RETRIED` goes out (step 5) before any pass can admit it, even at
   zero backoff. One already due sets `more`, so the next pass runs at once.
3. **Enforce deadlines** — flip the cancel flag on any running task past its
   deadline (cooperative for in-process; the OOP path hard-kills separately).
4. **Admit** — priority-ordered, skip-to-fit, with reservation, holding a slot for
   each due retry from 2b (see §5). Nothing once a shutdown's grace has expired.
5. **Emit** buffered events with the lock released, then `continue` (re-runs the
   loop so a signal arriving during the emit window can't be lost).
6. **Shutdown** check — when fully drained, wake workers to exit and break. The
   drain is BOUNDED: `gptps_shutdown` arms a deadline (`limits.shutdown_grace_ms`,
   default 30s, `0` = wait forever), and once it passes, step 3b raises every
   in-flight item's cancel flag so one stuck child cannot hang the host's exit, and
   ends everything still queued or in backoff by its policy (`DEAD_LETTERED`, or
   `DROPPED` under drop, with `GPTPS_E_SHUTDOWN` and `GPTPS_EV_FLAG_SHUTDOWN`).
7. **Sleep** — `cond_timedwait` until the nearest deadline/backoff, or `cond_wait`
   until signalled. (Steps 1–7 hold the lock continuously when no events were
   emitted, so no wakeup can be lost.)

> A subtle lost-wakeup bug lived in the emit window (step 5): releasing the lock
> to emit could drop a `cv_disp` signal. The fix is the `continue` — re-running
> the loop re-drains `done` and re-admits `intake` rather than risking a sleep.

### 3.1 Manual mode (`gptps_step`)

In MANUAL mode no dispatcher or worker threads are created. `gptps_step()` is the
whole engine on one thread:

1. `engine_pass()` — complete prior work, promote backoff-ready retries, admit
   within budget (steps 1–4 above), then emit buffered events with the lock released.
2. **Run inline** — pop each admitted item from `ready`, run it to completion on
   the calling thread (the work a worker would do), and post it to `done`.
3. `engine_pass()` again — account what just ran (release budget, schedule retries
   / dead-letter) and emit.

`*out_ran` reports how many task attempts executed; the caller loops
`while (gptps_step(e,&n)==GPTPS_OK && n);` to drain, or ticks it from its own main
loop. `max_concurrent_tasks` still bounds how many items one step admits at once
(the memory budget is honored identically). The one semantic difference from
threaded mode: a task runs to completion on the caller's thread, so a wall-clock
deadline can't *preempt* it — cooperative tasks poll `gptps_is_cancelled()` /
`gptps_deadline_ms()`; hard timeout/kill needs an out-of-process executor. Threaded
engines reject `gptps_step` with `GPTPS_E_INVAL`; MANUAL `gptps_shutdown` skips the
joins and drains the in-flight queues directly.

### 3.2 The allocator seam (`gptps_set_allocator`)

All **core** allocation (engine, settings, config, executors) routes through
`gptps_malloc/calloc/realloc/free` in `src/alloc.c`, which default to the C library
and can be redirected process-wide to a custom `malloc`/`realloc`/`free`
(SQLite-style) — e.g. a static pool on a host with no libc heap. Install it once
before the first `gptps_open` (the override is configuration, not runtime state).
The **HAL deliberately uses libc directly**: it is the platform seam you replace
wholesale on an exotic target, and it owns its thread/sync structs there. It
allocates only in its create calls, at open; nothing per item (each item's cancel
flag lives inside the item). MANUAL mode + a custom allocator is the bare-metal
shape; see `examples/embedded.c`.

A hook that returns NULL surfaces as `GPTPS_E_NOMEM` from the call that needed the
memory, and that call, made again, returns what it would have. A setup call that
makes several things - `gptps_register_task` a task's settings,
`gptps_define_resource` and `gptps_define_task_setting` a setting for every task -
makes all of them first, then publishes them together, under the locks, after its last
allocation; refused, it has published nothing, so another thread never sees half of it.
The one trace is the name a registration holds while it runs. Memory that runs out
reading or applying a config file - to open, reload or save - is `GPTPS_E_NOMEM`, not a
mistake in the file.

Three places absorb a failure, by design: the dead-letter drain, which returns a
count, stops short and leaves the rest retained; `gptps_step` leaves an admission it
cannot allocate for its next pass; and the per-thread record that lets the engine
refuse a `gptps_shutdown` from inside its own callback is made as it is needed, so a
callback that runs without memory for it runs without that check.
`tests/test_oom.c` fails each allocation of six scenarios in turn and holds them to
all of this: the work path, a config file, the settings API, a bounded engine's
seal, the durable queue, and definitions and registrations racing on two threads.
A seventh, a THREADED engine failing allocations at random, checks only that
nothing crashes, leaks, hangs or loses an event.

### 3.3 Bounded mode (`gptps_config.max_items`)

The allocator seam decides where memory comes from, not how much or when. Bounded
mode decides both. With `max_items` set, the first submit that names a registered task
allocates the engine's whole working set, sized by the config and by what setup
defined, and the work path neither allocates nor frees after it:

- items come from a fixed pool, each owning a payload slot and a named-resource
  snapshot slot at its own index;
- the handle index is made at its final size, and deletion leaves no tombstones
  ("finding an item" in `src/engine.c`), so it is never rebuilt;
- each executing thread owns one result buffer;
- the callback-thread records are made in advance.

Setup that would allocate is refused after that seal (`GPTPS_E_BUSY`), and so are
process-based task kinds, which allocate by nature. Every operation's cost is then
bounded by the configured maxima, with no amortized rebuild on any path.
[`docs/BOUNDED.md`](BOUNDED.md) gives the rules, sizing and speed;
`tests/test_bounded.c` holds the engine to it with a counting allocator, in MANUAL and
THREADED mode.

---

## 4. Admission: self-throttling budget

Each task type declares a rough **cost**: `mem_bytes`, plus any number of generic
**named resources** (`gptps_define_resource` / `gptps_set_task_resource_cost`).
`gpu_units` and `est_duration_ms` were removed in ABI 2.0 — the first *is* a named
resource under a specific name, and the second was read by no code anywhere. The
engine resolves a budget at open time
(`max_concurrent_tasks`, `max_memory_bytes`) — explicit values win, else
hardware auto-tune (online logical CPUs; ~0.75× RAM).

Admission is **declared-cost-fits-live-budget**, *not* an all-or-nothing cap:

```
admit X  ⟺  running < max_concurrent  ∧  reserved_mem + cost(X) ≤ max_memory
                                       ∧  ∀ resource r: reserved_r + cost_r(X) ≤ budget_r
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
  `reserve_after_skips` backfills. Over-budget submits are rejected up front, and
  a queued item that a live budget cut or cost raise leaves unable ever to fit is
  dead-lettered with `GPTPS_E_BUDGET` when the scan reaches it, so a reserved task
  is always eventually admitted or ended: the reservation cannot wait forever.
- **A due retry announced this pass** (step 2b, zero backoff) is not in `intake` yet.
  Work at or above its priority is admitted as usual; lower work only while it still
  fits beside every due retry, a slot and its declared memory each. MANUAL mode admits
  nothing it outranks until the next pass, since nothing runs before the step's passes
  end. Named resource budgets are not held.

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
  re-admitted after `retry_backoff_seconds`; its `RETRIED` is delivered before it can
  be, even at zero backoff (step 2b above).
- **`on_failure`** when retries are exhausted:
  - `dead_letter` (default, safe) — retained in the in-memory dead-letter list,
    emits `DEAD_LETTERED`. The list is CAPPED (`limits.max_dead_letters`, default
    1024, `0` = unbounded), evicting oldest-first and counting evictions in
    `stats.dead_letters_evicted` — it is the only queue the host need not drain,
    so an unbounded one was a leak in an engine built on bounded admission;
  - `drop` — discarded, emits `DROPPED` (v1.9) so an observer can still reconcile
    the handle;
  - `requeue` — re-enqueued for another cycle (bodies MUST be idempotent; never
    re-admitted during shutdown, to avoid an always-failing task hanging the drain).
- **Dead-letter drain** — `gptps_dead_letter_drain()` takes the retained list under
  the lock, each item with its own copy of its task's name (the callback may remove
  the type), then hands each item to a callback with the lock released (so it may
  re-submit). A name that cannot be copied stops it there: that item and the rest
  stay retained for the next drain. A constraint `DENY` is also retained, with status
  `GPTPS_E_DENIED`.

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
so OOP tasks must be fork-safe / self-contained (CPU/memory-bound work, or code
you want isolated and killable because it may hang, leak, or crash).

This is **resource** isolation, not **privilege** isolation: the child inherits a
full copy of the host address space, its file descriptors, and its environment.
See [SECURITY.md](SECURITY.md) for the trust boundary and what `child_setup` is
for.

---

## 8. The HAL (`gptps_hal.h`)

The only platform-specific seam. Pure C99 interface; each backend uses the best
primitive its platform offers, and **all atomics are confined to the backend**
so the core never includes an `_Atomic` type.

Surface: hardware detection (CPU/RAM/GPU hint), monotonic clock, an acquire/release
`u32` pair (each item's cancel flag is a word in the item, read and written through
it), mutex / condvar / thread, dynamic loading, and atomic
file replace (`gptps_hal_atomic_replace`, for the settings save). Its contract —
clause by clause, with what the core needs each clause for — is in
[`docs/HAL.md`](HAL.md), and `tests/test_hal_conformance.c` holds every backend to
it. Two full backends implement it: `hal_posix.c` (pthreads, `clock_gettime`,
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

**Porting to a new target (RTOS / bare-metal).** Write one backend implementing
this interface, build it in with `-DGPTPS_HAL_SOURCE=<file>`, and run
`ctest -R hal_conformance`: [`docs/HAL.md`](HAL.md) is the checklist. In MANUAL
mode (§3.1) the required subset is small, because a single-threaded host calls no
wait and starts no thread: mutex and condvar create/destroy/lock/unlock/signal/
broadcast (no-ops will do), the monotonic clock, the thread id and the
acquire/release `u32` pair (a constant and plain accesses will do), the
fork-guard pair (a constant), and hardware detection (return `cpu_count = 1`).
`dlopen` and `atomic_replace` can be stubbed (`NULL` / `GPTPS_E_IO`) if you don't use
dynamic add-ons or settings persistence. `freestanding/hal_stub.c` is exactly that,
and CI runs the conformance test against it in that profile. Combined with
`gptps_set_allocator` (§3.2) for a static memory pool, that is the whole bare-metal
dependency surface.

---

## 9. Add-ons and the host-table ABI

The core is mechanism-only; variety lives in add-ons. There are **four called seams**
— real extension interfaces, so called because the core *invokes* them:

- **task** — register task types (frozen v1.0);
- **constraint** — an admission hook (`ADMIT` / `DENY` / `DEFER`), consulted in
  the dispatcher so it MUST be non-blocking; used for rate limits, quotas,
  time-of-day windows, GPU caps;
- **observer** — an extra event sink (analytics, durable queue, dead-letter sink),
  run with the engine lock RELEASED so it MAY call back into the engine;
- **scheduler** — the admission *ordering* key (frozen v1.12), a single-slot hook
  scored under the dispatcher lock.

…and **one composed pattern**, which is not an interface at all:

- **transport** (`GPTPS_SEAM_TRANSPORT`) — a module that carries work OUT of this
  engine entirely. It has **no struct, no function-pointer typedef and no register
  call**: the core never invokes a transport, because a transport sits on the *other
  side* of the engine and calls *in*. `addons/gptps_xport` is the reference case and
  it consumes no seam — it is built on POSIX IPC plus a handler you supply. That the
  pattern needs nothing from the core is the point: an interface here would be a
  vtable with no call site, and it would make the admission budget a lie, since the
  dispatcher reserves `mem_bytes` for work that will not run in this address space.

Which side you are writing on is the first thing to know as an add-on author, which
is why the distinction is drawn here rather than smoothed over as "five seams".

A dlopen'd add-on calls the core **only** through a passed, version-stamped
`gptps_api_routines` table and links **no** core symbols (this also avoids symbol
capture in the amalgamated build, where core symbols are namespaced `gptps_`/
`gptps__`). It exports exactly one symbol, `gptps_addon_init`, conventionally via
the `GPTPS_ADDON_INIT(...)` macro. The loader validates `magic` /
`abi_version_major` / `struct_size` **before** using the add-on.

Not every add-on must be a shared object — a module that only uses the public API
and the observer/constraint seams can simply be compiled into the host. **Eleven** ship
in `addons/`: `durable_queue.c` (observer seam → crash-durable journal),
`gpu_quota.c` (a thin wrapper over the core's named-resource budgets),
`wasm_exec.c` (module-as-task with a pluggable wasm runtime), `tui.c` (observer +
settings → live dashboard), `gptps_orch.c` (observer + submit → run-after/fan-in
dependencies), `gptps_await.c` (observer seam → block until a handle is terminal),
`gptps_stats.c` (observer seam → counters, gauges and latency, bound to no wire
format), `gptps_remote.c` (the cross-host wire codec `gptps_xport` is specified
against), and the three composition libraries `gptps_pool.c` (N engines in one
process, scale-up), `gptps_balance.c` (one queue in front of a pool, routed late to
the least-loaded shard) and `gptps_xport.c` (N worker processes, scale-out).
`gpu_quota_plugin.c` builds separately as the example *binary* plug-in rather than a
linkable module. See [`addons/README.md`](../addons/README.md).

Note the last three are *composition libraries* rather than add-ons in the loader's
sense: they use no host table and sit above the engine instead of inside it
(`gptps_balance` watches each shard's events, but from outside, as any host could).
`gptps_pool` in fact owns N whole engines. Calling them add-ons is what makes the
loader look unused; naming them accurately is more honest and more useful.

---

## 10. Configuration

`gptps_open(path)` — or `gptps_open_ex` with `cfg->config_path`, where an explicit
`cfg->limits` value wins over the file's — parses a config file (a TOML *subset* —
tables, dotted tables, int/float/bool/string scalars, single-line string arrays,
quoted parts in keys and table names, `#` comments — parsed by `config_toml.c`, no
external dependency). It maps
to (every key: [CONFIG.md](CONFIG.md), generated from the registry):

- `[limits]` and `[bounded]` → the keys that size the engine, read before it exists;
- `[scheduler]`, `[stats]` → settings, applied through the registry;
- `[resources]` → named resources, defined (or re-budgeted) at open;
- `[task_defaults]` then `[tasks.<name>]` → per-task policy / cost / priority,
  applied at registration (def < `task_defaults` < `tasks.<name>`);
  `[tasks.<name>.resources]` → what one run costs of each named resource;
- top-level `addons = [...]` → shared libraries loaded at open.

`gptps_open_ex(cfg, ...)` with `cfg->config_path` NULL is the explicit, file-free path.

**One validation path.** The parser is strict: a line it cannot read fails the open
with the file and line. Every value then goes through the check a live
`gptps_settings_set` makes (`valid_value` in `settings.c`): the sizing keys before the
engine exists, the per-task keys when the file is read (and applied when the task
registers), everything else through the registry itself. An unknown key in one of
the engine's own tables is an error, with a suggestion by edit distance; so is a
table name a letter or two from one of them, when the key under it is that table's. Each entry the parser keeps has a *claimed*
flag; a key nothing claims at open waits — a `[tasks.<name>]` table for a task not yet
registered, a setting a plug-in or the host defines later — and `cfg_apply_pending`
applies it at that definition (an add-on's own keys, when its setup returns, so the
watcher it registered there hears them; a host's watchers hear only live sets). Keys
are judged by their dotted form, never by how the
file split them into table and key, and a value must be the TOML kind its setting
takes (a number, `true`/`false`, a `"string"`). A table name a letter or two from one of
the engine's is an error only when the key under it is that table's; otherwise it may
be the host's, and waits. The registry, the define-time check and the parser share one
number grammar (`gptps_settings_plain_number`), so no value a setting takes can make a
saved file unreadable. What is still unclaimed after setup is what
`gptps_config_check` reports, and what the first submit logs once. One attempt reports
every problem, not the first one only. The parser goes on past a line it cannot read
(and passes over the keys under a `[table]` line it cannot read, whose table is
unknown); a file that parses has every value checked, and a sizing key the open
refuses is left out so the engine can open to check the rest, then the open is
refused.

### Settings registry (`src/settings.c`)

The same knobs are also a unified, typed **settings registry** for runtime
introspection, validation, get/set, and persistence. Each entry is **schema +
accessor binding** — `{key, type, hot, range/choices, target, read_fn, write_fn}` —
so the live engine/add-on state stays the single source of truth (reads never
drift). The generic registry never sees `struct gptps` or add-on layouts; it only
calls `entry->read/write(target, …)`. Bindings live where the target is visible:
core/per-task in `engine.c` (target = the engine or a `gptps_reg`), add-on settings
in the add-on. Validation (range / enum / parseable) runs in the generic layer
*before* `write_fn` — the check the raw TOML path lacked.

- **Lock order is invariant:** `settings->m → e->m` (and `→` add-on mutex). Per-task
  settings register *after* `gptps_register_task` releases `e->m`, against the
  stable heap `gptps_reg` (append-only; freed in bulk at shutdown).
- **Hot vs restart:** `write_fn` pushes to live state and signals the dispatcher
  where applicable; `max_concurrent_tasks` is restart-only (pool fixed at open): a live
  write is kept for reads and `gptps_settings_save` (a saved file carries it to the next
  open), and admission never sees it.
- **Persistence:** `gptps_settings_save` updates the file in place, through
  `gptps_hal_atomic_replace` (temp + rename/MoveFileEx). Each entry records whether a
  live set or a file last wrote it; save rewrites the lines of the live-set ones (the
  parser's line numbers say which line holds which key), adds those the file lacks next
  to their siblings, and copies every other line byte for byte. A file the parser
  refuses is never overwritten. `gptps_settings_reload` re-applies a file through the
  same checks as the open and swaps `e->toml`.
- **Extensible:** add-ons register settings via the public `gptps_register_setting`
  or the host-table `register_setting` routine; the `tui` Settings pane (`s`) is a
  thin editor over the registry.

### Generic settings (define without per-key glue)

`gptps_register_setting` binds to *your* state; the convenience helpers let a host
declare typed knobs the engine stores and validates for it:

- `gptps_define_global` allocates an engine-owned value cell, parses the
  `"min..max"` / `"a|b|c"` constraint, validates the default, and registers a normal
  registry entry whose `read/write` target the cell. The cell needs no lock of its
  own — every read/write runs under `settings->m`. Cells are freed at shutdown.
- `gptps_define_task_setting` records a **schema** (`leaf`, type, default, range/
  choices) and *materializes* it as `tasks.<name>.<leaf>` on every registered task
  (snapshotting the live regs under `e->m`, then adding the entries with `e->m`
  released to honor the lock order) and on every task registered later (via
  `register_task_local_settings` in `gptps_register_task`). Each (task, schema) pair
  owns a value cell hung off the `gptps_reg`. `gptps_task_setting_int/str` reads the
  running task's resolved value from `ctx->reg->locals` — INPROC only, since an
  OOP/PROGRAM body runs in a child with no live engine handle (`ctx->reg == NULL`).

### Task management (runtime control plane)

The registry is itself mutable at runtime. The hazard: a `gptps_item` points at
`&reg->def`, so a `gptps_reg` must not be freed while any live item references it
(handles are opaque IDs never resolved back to a task, so no generation counter is
needed). Removal is therefore **tombstone + drain**:

- `gptps_unregister_task` marks `reg->removed` (so `registry_find` hides it from new
  submits and `engine_pass` stops retrying its items — a bounded drain). REJECT
  fails `E_BUSY` if any item is live; CANCEL drops the queued backlog, sets the
  cancel flag on in-flight items, and marks `cancelling` so they are *discarded* (not
  dead-lettered) when they finish; DRAIN lets queued+in-flight finish. In THREADED
  mode the caller then blocks on `cv_drain` (broadcast by the dispatcher after each
  pass) until no live item references the reg - unless the caller is one of the
  engine's own threads (`engine_is_reentrant`), which gets `E_BUSY` instead whenever
  there is anything to wait for, since the drain may need that very thread; MANUAL
  mode never has in-flight work between steps. Its `tasks.<name>.*` settings go right
  after the tombstone, before the drain (with `e->m` released — lock order): the ones
  the engine created by owner, not by prefix, so a live `<name>.<more>` sibling keeps
  its own, and host-registered keys under the prefix except a live sibling's. A second
  owned sweep runs once in-flight `gptps_define_task_setting` calls finish. Teardown
  then detaches retained dead-letter items (they take an owned name copy and sever the
  reg pointer), unlinks the reg, and frees it.
- `gptps_clone_task` deep-copies a reg's def under a new name (sharing the run fn);
  `gptps_set_task_enabled` flips a reversible `enabled` flag; `gptps_task_count/
  get_info` enumerate (including draining types, with live queue counts), while
  `gptps_task_exists` and `gptps_task_flags` look a name up and skip them.
- The `tui` add-on's **tasks** and **dead-letter** panes are thin views over these
  calls; deleting from the TUI uses DRAIN (cancel-force on demand).

---

## 11. ABI versioning & stability

- Every caller-extensible struct's first field is `size_t struct_size`; the core
  rejects an undersized struct and reads appended fields conditionally.
- Structs may **only** grow by appending fields. Never reorder/remove/retype.
- `GPTPS_ABI_VERSION_MINOR` bumps on additive change, `MAJOR` on an incompatible
  one; the add-on loader refuses a `MAJOR` mismatch.

New capabilities have so far been added without breaking the ABI: result
delivery, the external-program executor, constraints/observers, task priority
(`gptps_set_task_priority`), the dead-letter drain, the settings registry
(`register_setting` appended to the host table, minor 3 → 4), and runtime task
management + generic settings (new symbols, an appended `GPTPS_E_BUSY` status, a
`struct_size`-fronted `gptps_task_info`, and four more host-table routines, minor
7 → 8) — each a new symbol or an appended field, never a reshape.

---

## 12. Verification discipline

Every increment ships with tests and is held to: CTest green on Linux + macOS,
**AddressSanitizer + UBSan**-clean across the whole suite and **ThreadSanitizer**-
clean on the concurrent paths (ASLR disabled in CI), stress loops on timing-
sensitive tests, coverage-guided fuzzing of the two hand-rolled parsers (TOML +
journal) and of the in-place save (`tests/fuzz/`) - the suite replays the inputs
kept from it, and the `asan` job builds the fuzzers and runs each briefly - system-call
fault injection into the out-of-process executors (`tests/test_exec_faults.c`), and a
check that all three build paths work (CMake, the single-file amalgamation, and a
plain `cc -std=c99`). Platform-specific tests (OOP memory caps, cgroup enforcement)
**self-skip** where the facility is absent rather than failing (cgroup delegation, a
wasm runtime CLI). CI runs thirteen jobs: `build-test` (Linux + macOS, a 2-way matrix),
`werror` (`-O2 -Wall -Wextra -Werror`), `package` (installs, then builds a plug-in
out-of-tree against the installed package), `windows` (mingw-w64), `msvc` (cl.exe),
`amalgamation` (+ a licence-notice assertion), `asan` (+ UBSan/LSan, and the
fuzzers), `tsan`, `hal_fast`, `hal_chaos` (the whole suite on the weakest HAL the
contract allows), `hal_sim` (the suite on a simulation HAL where a seed picks every
thread switch, so a failure replays), `cross` (i386 + big-endian s390x under QEMU),
and `freestanding`.
The `tsan`, `hal_chaos`, `hal_sim` and `cross` jobs select tests with an EXCLUDE
list, so a newly added test is covered by default rather than silently skipped.

---

## 13. Future work (intentionally not built yet)

Some planned work is partial or unbuilt, because this project only ships code it
can verify, and these could not be fully tested in the environment they were
developed in:

- **WASM — works today; only a *bundled default* runtime is unbuilt.** Two ready
  paths: (1) `GPTPS_EXEC_PROGRAM` + a wasm runtime CLI — `argv = ["wasmtime",
  "run", "module.wasm"]`, argv[0] PATH-resolved — shown in
  `examples/wasm_program.c` (which embeds a hand-assembled, validated `.wasm` and
  self-skips when no runtime is on PATH); (2) `addons/gptps_wasm_exec.c`, an in-process
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
