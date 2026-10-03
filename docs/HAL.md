# The HAL: what a port must do, and how it is checked

The HAL, [`include/gptps_hal.h`](../include/gptps_hal.h), is GPTPS's only platform
seam: 24 functions for the clock, threads, locks, an acquire/release pair, fork
safety, hardware detection, dynamic loading and an atomic file replace. (Four more,
`gptps_flag_*`, are still declared so existing backends build, but the core no longer
calls them: each item's cancel flag is a word in the item, on the acquire/release
pair.)
Everything else in the core is ISO C99 — it compiles under `-std=c99
-pedantic-errors` — and calls nothing else of the platform. Two more files are
platform code without being HAL: the executors for forked and external-program
tasks (`src/exec_oop_posix.c`, `src/exec_win.c`). A target without processes
supplies stubs for them, as [`freestanding/exec_stub.c`](../freestanding/exec_stub.c)
does.

Three backends ship: `src/hal_posix.c`, `src/hal_win.c`, and the freestanding stub
[`freestanding/hal_stub.c`](../freestanding/hal_stub.c). A port is one more file,
built in with `-DGPTPS_HAL_SOURCE=<file>`.

## Two profiles

- **THREADED.** Everything below. The engine runs a dispatcher and a worker pool.
- **MANUAL only.** `gptps_thread_start` returns `NULL`, and the engine runs only in
  MANUAL mode, on the host's thread (`gptps_step`). A single-threaded host then calls
  no wait and starts no thread, so locks, signals and broadcasts may be no-ops, the
  acquire/release pair may use plain accesses, and the thread id may be a constant.
  The freestanding stub is this profile.

## The contract

Each clause names the check in [`tests/test_hal_conformance.c`](../tests/test_hal_conformance.c)
that holds a HAL to it. CTest runs that test against the HAL of every build, on every
CI platform.

| Clause | Why the core needs it | Checked by |
|---|---|---|
| **The clock never decreases**, on any thread. | Deadlines, backoff and the shutdown grace are differences of readings; a reading that goes back fires a deadline late or waits a backoff twice. | `clock: never decreases`, and `... across threads` |
| **The clock runs at the rate of real time, in milliseconds.** Any step is fine (Win32's is about 16 ms). | Task timeouts, retry backoff and the grace are configured in real units. An RTOS tick count passed off as milliseconds is the classic port bug. | `clock: runs at the rate of real time` (2 s of `time()`, within 25%) |
| **Acquire/release.** A store-release is seen, with everything before it, by a load-acquire that reads it, and a polling load sees a store promptly. | Each item's cancel word: a cancel, a deadline or a removal raises it, and the task body polls it through `gptps_is_cancelled`. And the per-thread callback-depth records, which pass between threads without a lock. | `acquire/release: a released write is seen ...` (see the limits below), `... seen by a polling load on another (the cancel path)` |
| **Mutual exclusion**, with unlock-to-lock ordering. The core never locks a mutex it holds, so a recursive mutex (Win32's `CRITICAL_SECTION`) conforms as well as a non-recursive one. | Every queue and counter in the engine is under one mutex. | `mutex: mutual exclusion` |
| **A wait releases the mutex while it blocks and holds it again when it returns.** A signal wakes at least one waiter; a broadcast wakes every current waiter. | The worker pool's idle wait and the dispatcher's sleep. | `cond: wait/signal hand-off ...`, `cond: one broadcast wakes every waiter` |
| **A wait may return spuriously, but not as a rule.** | The core re-checks every predicate in a loop. A wait that returns at once nearly every time turns the worker pool and the dispatcher into spinning loops. | `cond_wait: blocks until a signal, and does not spin`; the `cond_timedwait` checks count calls |
| **A timed wait returns after about `ms` without a signal, and early when signalled — for any `ms` up to `UINT64_MAX`.** Clamp what the platform cannot express. | The dispatcher's one timed wait sleeps until the next deadline or backoff, and a deadline can be `timeout_seconds * 1000` = 4294967295000 ms away. It works out its wait again each time it wakes, so a clamp costs nothing. | `cond_timedwait(N ms): times out ...` for 0, 1, 25 and 150 ms; `cond_timedwait(...): a signal wakes it` for 60 s, 4294967295000 ms and `UINT64_MAX` |
| **A thread runs `fn(arg)`; join waits for it, frees it, and makes its writes visible.** Start returns `NULL` when it cannot. | The dispatcher and the workers. | `thread: start runs fn(arg) ...` |
| **A thread id is stable within a thread and distinct among live threads.** It may be reused after a thread exits. | Re-entrancy detection: `gptps_shutdown`, `gptps_step` and an unregister that waits refuse a call from inside a callback. | `thread_id: stable ...`, `thread_id: distinct among live threads ...` |
| **On a platform with `fork`, the generation changes in the child, and only there.** Install is idempotent. Without `fork`, a constant. | An engine created before a fork is refused in the child instead of deadlocking on a mutex a vanished thread held. | `fork: ...` (three checks) |
| **Detection returns `GPTPS_OK` and at least one CPU**; RAM may be 0 for unknown; `NULL` is `GPTPS_E_INVAL`. | Auto-tuning the worker count and the memory budget. | `detect: ...` |
| **Optional — dynamic loading.** Open returns `NULL` on failure, a lookup of a missing symbol returns `NULL`, close unloads, release frees the handle but keeps the mapping. | Binary plug-ins (`addons = [...]` in TOML). Without it, open returns `NULL` and plug-ins are unavailable. | `dl: ...` (CTest `hal_conformance_dl`) |
| **Optional — atomic replace.** The target is replaced in one step, also when it exists; `GPTPS_E_IO` on failure. | `gptps_settings_save` writes a temporary file and replaces the real one. Without a filesystem it returns `GPTPS_E_IO`, and saving is unavailable. | `atomic_replace: ...` (four checks) |

## What a port may do

The contract leaves these free, and the core must cope with every one:

- a wait that returns without a signal;
- a timed wait that returns early, or up to a clock step late;
- a signal that wakes more than one waiter;
- a clock of any step size;
- a recursive or a non-recursive mutex, and an unfair one;
- a thread id reused once its thread has exited;
- a thread that starts late.

[`tests/hal_chaos.c`](../tests/hal_chaos.c) is a HAL that takes every one of these
freedoms, often (one call in four by default). It passes the conformance test, and CI's
`hal_chaos` job builds the whole suite on it and runs it with a new seed each time. The
exceptions are the three `*_perf` gates: they time a curve on the HAL's own clock, which
this HAL coarsens and perturbs on purpose. That is the other half of the contract: the
conformance test shows a HAL keeps it, and the chaos run shows the core needs nothing
more.

## What the test cannot show

- **Memory ordering on x86.** The hardware orders plain accesses itself, so a HAL that
  implements acquire/release with plain loads and stores passes there. The check is a
  real test on a weak-memory CPU (CI's arm64 macOS runner) and under ThreadSanitizer
  (CI's `tsan` job), which reports such a HAL as a data race.
- **Wall-clock steps.** A timed wait measured on the time of day stretches or shrinks
  when the clock is set, and setting it needs privileges. On Linux the POSIX backend
  waits on `CLOCK_MONOTONIC`, and on macOS it uses a relative wait; other POSIX
  targets fall back to `CLOCK_REALTIME` until someone wires up and tests the
  monotonic path there.
- **`gptps_thread_start` returning `NULL`.** Provoking it takes resource exhaustion.
- **That `dl_release` keeps the mapping.** The check only shows it returns, and
  LeakSanitizer shows the handle was freed.

## Running it

- **The stock backends.** CTest runs `hal_conformance` and `hal_conformance_dl` in
  every build.
- **A port.** Build with `-DGPTPS_HAL_SOURCE=/path/to/hal_mytarget.c`, then
  `ctest -R hal_conformance`. The same file runs against your HAL.
- **A target with no OS.** The test needs only the HAL and hosted C99, so it can be
  built against a HAL alone. CI checks the stub this way:

  ```sh
  cc -std=c99 -I include -I src tests/test_hal_conformance.c freestanding/hal_stub.c -o conf
  ./conf --freestanding
  ```

  `--freestanding` declares no real-time clock, no `fork`, no filesystem and no
  dynamic loading. Each check it skips says what stops working without that facility.

## Notes on the shipped backends

- **Win32.** The clock steps about every 16 ms. `CRITICAL_SECTION` is recursive. Timed
  waits clamp to `INFINITE - 1` ms (about 49.7 days).
- **POSIX.** Timed waits clamp to a day. Linux waits on `CLOCK_MONOTONIC`, macOS waits
  are relative, and other POSIX targets wait on `CLOCK_REALTIME`.
- **Freestanding stub.** MANUAL only. Its clock counts reads instead of milliseconds,
  so timeouts and backoff are not in real time on it.
