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
| **Detection returns `GPTPS_OK` and at least one CPU**; RAM may be 0 for unknown; `NULL` is `GPTPS_E_INVAL`. `has_gpu` is not part of the contract: the core never reads it. | Auto-tuning the worker count and the memory budget. A GPU is a named resource the host or the config file defines, not something the HAL detects. | `detect: ...` |
| **Optional — dynamic loading.** Open returns `NULL` on failure, a lookup of a missing symbol returns `NULL`, close unloads, release frees the handle but keeps the mapping. | Binary plug-ins (`addons = [...]` in TOML). Without it, open returns `NULL` and plug-ins are unavailable. | `dl: ...` (CTest `hal_conformance_dl`) |
| **Optional — atomic replace.** The target is replaced in one step, also when it exists; `GPTPS_E_IO` on failure. Recommended, not checked: the replacement keeps the replaced file's permissions, as the bundled POSIX HAL does, because save edits an operator's config file. | `gptps_settings_save` writes a temporary file and replaces the real one. Without a filesystem it returns `GPTPS_E_IO`, and saving is unavailable. | `atomic_replace: ...` (four checks) |

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

## Searching schedules: the simulation HAL

A failure on `hal_chaos` may never come back. [`tests/hal_sim.c`](../tests/hal_sim.c)
makes the interleaving a function of a seed, so a failure is a number that replays:

- **One thread at a time.** Every engine thread is a real OS thread, but they pass a
  baton, and every HAL call - lock, unlock, wait, signal, broadcast, timed wait, the
  acquire load and release store, thread start and join, the clock - is a point where
  a PRNG seeded from `GPTPS_SIM_SEED` picks who runs next.
- **Virtual time.** When every thread waits, the clock jumps to the earliest deadline:
  a 30 s shutdown grace costs nothing, and a timeout fires at the same point of the
  schedule every run. While the threads that can run only poll, it creeps a ms per 32
  polls; and work costs a ms per 1000 scheduling points, so a loop that waits for time
  while it locks and unlocks still sees it pass.
- **Deadlocks are reported, not hung on.** Every thread waiting and none with a timeout
  aborts with each thread's state and its last call, as `file+offset` for
  `addr2line -e`. So does a HAL contract broken on the spot: unlocking a mutex the
  thread does not hold, destroying a mutex or a cond in use.
- **The add-ons' own pthreads too.** The add-ons and some tests lock, wait, start
  threads, sleep and read `clock_gettime` with libc directly. A thread blocked there
  would hold the baton and stall every other, so `hal_sim.c` defines those functions
  as well and runs them through the same scheduler. `time()` stays real.
- **A forked child** steps out of the simulation: there it is the POSIX HAL. It runs
  on real time, so from the parent's first fork on, the parent's clock keeps to real
  time as well, as `GPTPS_SIM_PACE=1` below makes it do from the start. Before that, a
  wait would jump the clock over seconds while the child had yet to be scheduled: a
  shutdown grace ran out on a child about to exit.

It passes the conformance test except for one check, which cannot hold by design:
**the clock runs at the rate of real time**. Virtual time runs ahead while everything
waits and stands still while a thread computes. With `GPTPS_SIM_PACE=1` the clock
keeps to real time - a jump waits for it, and every scheduling point catches the clock
up with it - and then every check passes. Paced, a thread that has held the baton for
10 ms of real time, as one blocked in `poll()` on a child's pipe does, lets another
go first, as the end of a time slice would.

```sh
cmake -S . -B build-sim -DGPTPS_HAL_SOURCE=$PWD/tests/hal_sim.c
cmake --build build-sim -j
```

**Reproduce a seed.** The log names it at start and at exit, with a hash of every
decision the scheduler made. Run the failing test's binary with the seed from the log
(`ctest -V` shows the same lines):

```sh
GPTPS_SIM_SEED=5 GPTPS_SIM_CPUS=4 build-sim/test_orch
# hal_sim: seed 5 (switch 1 in 16, freedoms 1 in 16, 4 cpus)
# all orch checks passed
# hal_sim: seed 5: 3450 steps, 173 switches, 10 ms of virtual time, trace 60a470e157d89776
```

The numbers change with the code, but on one tree the same seed and the same CPU count
give the same run, and the same trace hash. `GPTPS_SIM_CPUS` matters because the worker
pool is sized from the CPU count; CI pins it to 4. If the hash differs, something
outside the simulation decided. The exit line warns of what it can see, with a reason
after `(may not replay: ...)`: the run forked a child process, which runs on real time
(`1 child process ran on real time`); real time moved a paced clock; or the run met a
thread the simulation did not start, or a pthread mutex held outside it. A forked
child is only a warning: a parent that polls its child or holds it to a deadline does
not replay, but one that only waits for it to exit does. It cannot see a test that
reads `time()` and acts on it. `GPTPS_SIM_VERBOSE=1` logs every switch, including the
one a thread makes as it exits, and every time jump. Run the test binary itself under a
debugger the same way: the schedule does not change.

**Search seeds.** Each seed also picks the style of its schedule - switching at one
point in 1, 4, 16 or 64, with or without the contract's freedoms (spurious wakeups, a
signal that wakes everyone) - so a range of seeds covers more than one kind of run.
CI's two commands, for each seed in turn:

```sh
for s in $(seq 1 500); do
  GPTPS_SIM_SEED=$s GPTPS_SIM_CPUS=4 GPTPS_STRESS_SEED=$s GPTPS_STRESS_MS=1500 \
    ctest --test-dir build-sim -j4 --no-tests=error \
    -E '_perf$|^(hal_conformance|oop|program|program_helper|exec_faults|durable_crash|hang|example_program|example_wasm|xport|xport_engine|gptps_bench_pool|gptps_bench_balance)$' \
    > sim-$s.log 2>&1 || echo "seed $s failed: see sim-$s.log"
  GPTPS_SIM_SEED=$s GPTPS_SIM_CPUS=4 GPTPS_SIM_PACE=1 \
    ctest --test-dir build-sim --no-tests=error \
    -R '^(hal_conformance|oop|program|program_helper|exec_faults|durable_crash|hang|example_program|example_wasm)$' \
    > sim-paced-$s.log 2>&1 || echo "seed $s failed paced: see sim-paced-$s.log"
done
```

The tests the first command leaves out, and why:

| Test | Why |
|---|---|
| `*_perf` | They time a curve on the HAL clock, which is virtual here. |
| `gptps_bench_pool`, `gptps_bench_balance` | They time throughput on the same clock, and assert nothing. On a seed that switches at every point, `gptps_bench_pool` makes 5.4 million real thread switches and took 71 to 87 s, against its 60 s CTest timeout. |
| `hal_conformance` | Its real-time rate check, above. The second command runs it paced. |
| `oop`, `program`, `program_helper`, `exec_faults`, `durable_crash`, `hang`, `example_program`, `example_wasm` | They are about child processes, which live in real time: the executor polls a child in real 200 ms slices and holds it to deadlines and shutdown graces. Every run of them forks, so none replays, and the second command runs them once, paced from the start. Before a run was paced from its first fork they failed unpaced: `exec_faults` with every undisturbed run timed out, `example_program` in 18 of 400 runs under load. The seed still draws every switch, but real time moves the clock. (`example_wasm` runs a child only where a wasm runtime is installed; without one it skips. `durable_crash` forks a child for each of its about 11,800 crash and power-cut runs; it passes unpaced too, in 37-41 s on three seeds that switch at every point, but runs once, paced.) |
| `xport`, `xport_engine` | Not runnable on it. A reader thread blocks in `read()` on a worker's socket while it holds the baton, and the request that reply needs is written by a thread that cannot run until it returns. |

`stress_api` runs in the first command, shortened to one round of 1.5 s by
`GPTPS_STRESS_MS=1500`. Its rounds last 1.5 s of the clock. Once a round runs one of
its PROGRAM tasks the run is paced, and a round takes about 1.5 s; until then the clock
is virtual, work moves it a ms per 1000 scheduling points, and on a seed that switches
at every point a round took 9 to 20 s. Its default of six rounds took 10 to 32 s, but
it ran past its 120 s CTest timeout on seed 19 before runs were paced from their first
fork, and a seed whose rounds fork nothing would still do so. `GPTPS_STRESS_SEED` pins
the round to the simulation's seed, so a re-run draws the same operations. A round
that forks does not replay: to repeat a failing one, use the `reproduce:` line it
prints, with `--replay` to run it on one thread.

Other knobs, read once at start: `GPTPS_SIM_SWITCH` and `GPTPS_SIM_SPURIOUS` override the
style the seed picked (one point in n; 0 turns the freedoms off), `GPTPS_SIM_HORIZON_S`
is how long virtual time may run with no thread waking another before a possible hang is
reported (it is reported once, and the run goes on: a test that naps until `time()` says
so looks the same), and `GPTPS_SIM_STALL_S` is how many real seconds without a
scheduling point before the threads are printed - a thread that computes, blocks in a
call the simulation does not see, or spins on memory with no call in its loop. A test's
own busy-wait needs a call the simulation sees in its loop - `gptps_now_ms(NULL)` will
do - or the spinning thread keeps the baton and every other waits.

CI's `hal_sim` job runs both commands: the first for three seeds a run, from the run
number, and the second for the first of them. A re-run of a job keeps its run number,
so it runs the same seeds again.

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
