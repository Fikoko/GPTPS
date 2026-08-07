# Changelog

All notable changes to GPTPS are recorded here. Format follows
[Keep a Changelog](https://keepachangelog.com/). As of 1.0.0 the project follows
semantic versioning; the ABI version (`GPTPS_ABI_VERSION_*`) moves independently of
the release version and is documented in `include/gptps.h`.

## [Unreleased]

### Added — `addons/gptps_remote`: the cross-host WIRE PROTOCOL (codec; transport pending)
- `addons/gptps_xport` says the local socketpair "is the only thing standing between
  this and cross-MACHINE execution: swap it for a TCP socket and the same protocol
  reaches another host." That is true of the *shape* and false of the *details*. This
  module makes the details honest.
- **The codec ships and is reviewable before any socket exists, deliberately.** A wire
  format is a second forever-contract standing beside ABI 2.0: once one peer anywhere
  speaks version 1, every future version must interoperate with it — and unlike a C ABI
  there is no compiler to catch a violation, no `struct_size` to check, and no way to
  recall a deployed peer.
- What a network commits to that a socketpair does not, each now handled:
  **byte order** (`xport` writes raw native-endian integers — free on one machine,
  silent corruption between a little-endian client and a big-endian server; everything
  here is explicitly big-endian, byte by byte, never a `memcpy` of an integer and never
  a cast of the buffer to a struct pointer, which would also be an alignment fault on
  strict targets); **`gptps_status` becoming wire-visible** (the enum fixes only
  `GPTPS_OK = 0`, so the wire carries its own stable codes and an unknown one from a
  newer peer degrades to `GPTPS_E_IO` rather than being reinterpreted); **a request id**
  so a transport can multiplex later (`xport` holds a lock across the whole round trip,
  capping a link at one in-flight request — fine at socketpair latency, a hard ceiling
  of a few thousand/sec over a network); and **a length cap that is a defence**
  (`xport`'s 256 MiB is reasonable against your own forked child and is one-packet
  memory exhaustion from an unauthenticated peer — 1 MiB here, per-link configurable,
  checked before the caller is told how many bytes to read).
- The tests assert the **actual bytes**, not a round trip — a round trip passes just as
  happily on a native-endian codec, which is the bug being avoided. Plus every
  rejection path: bad magic, unknown version, unknown kind, over-cap length, truncated
  frames, and a `task_len` that overruns the payload.
- Security, stated in the header rather than implied: the `task` field of a request is a
  dispatch key chosen by the peer, so on a listening socket it is remote code
  *selection* by whoever can connect. No authentication, no encryption, by design — run
  it inside a trusted boundary.

### Added — a plug-in author has documentation, a template, and a way to prove their work
- **`tools/gptps_conformance`** — run it against your own `.so` before you ship it.
  Installed to `bin/`, shipped in releases, no third-party dependencies.
- The check that matters is the **degradation ladder**. The engine always hands a
  plug-in its *full* host table, so the engine structurally cannot discover that a
  plug-in reads past the table size it was given — yet that is the single most likely
  way a plug-in breaks in the field: built against a newer GPTPS, dropped into an older
  host, calls a routine that host's table does not contain, jumps through whatever lies
  past the end. The harness links `libgptps`, so it can *synthesise* the table as each
  released core actually had it and run your `setup()` against every rung. Slots past
  the rung hold **poison stubs, not NULL** — a harness that proves your bug by crashing
  cannot say which routine, cannot continue, and makes the CI leg look broken rather
  than informative. You get the routine's name and the exact guard to add.
- **`tests/addon_greedy.c`** exists so the harness has something it must reject: a
  plug-in calling a v1.4 routine while declaring a v1.0 floor — which works fine against
  a current core and breaks only in an older host. Wired as `WILL_FAIL`, so if the
  harness ever goes soft it turns red instead of green. A conformance harness that
  cannot fail certifies nothing.
- **`templates/plugin/`** — a complete, standalone, copyable plug-in. Nothing in this
  tree builds it; the `package` CI job builds it *out of tree* against a staged install,
  which is the only real proof the install tree works for someone who did not clone.
- **`docs/PLUGINS.md`** — the missing author guide: which tier you want and why a module
  is not the lesser thing, the `struct_size` guard, why you must never call a core
  symbol directly, namespaces, threading and re-entrancy per seam, seam ownership,
  building on three platforms, proving it, shipping it, and the security posture.
- **`docs/PACKAGING.md`** — the consumer side: four acquisition paths, the install tree,
  every build option, and why add-on libraries are static on purpose.

### Added — add-ons are now OBTAINABLE
- Until now an add-on was compiled as an extra *source* into whichever test binary
  referenced it. There was no library target, nothing for `install()` to ship, nothing
  in `find_package`/pkg-config, nothing in the amalgamation, and nothing in any
  release. The only documented way to get one was to clone the repo and vendor the raw
  `.c` at whatever commit you happened to have. **An add-on you cannot obtain is not a
  module, it is a sample.**
- Each add-on is now its own installable library — `gptps::pool`, `gptps::await`, … —
  with an installed header under `include/gptps/`, an exported CMake target and a
  generated `.pc`. Three supported ways to take a **subset**:
  `find_package(gptps COMPONENTS durable_queue pool)`, `pkg-config gptps-durable_queue
  gptps-pool`, or `amalgamate.sh --addons durable_queue,pool`. Asking for an add-on an
  install does not have now says so in a sentence instead of failing three files later.
- **The amalgamation emits one self-contained `.c`/`.h` pair per add-on, never appended
  to `gptps.c`.** `gptps.c` stays byte-identical whatever selection you ask for — its
  SHA256 is the thing a Dockerfile pins, and one release must not have N hashes for one
  filename. Add-ons are not merged with each other either: each has file-`static`
  helpers that could collide in a shared translation unit.
- Releases now attach every add-on as an individual asset (so a Dockerfile can `curl`
  exactly one) plus a tarball, and the MIT-notice check covers every generated file
  rather than only the core two.
- The four unprefixed add-on filenames (`durable_queue`, `gpu_quota`, `wasm_exec`,
  `tui`) gained the `gptps_` prefix the other three already had. Their C symbols were
  always namespaced; only the filenames had drifted. Done now because these have never
  been installed or released, so the cost is exactly zero today and permanent tomorrow.
- The suite links the real add-on libraries instead of compiling their sources in, so a
  broken target fails the existing tests rather than surviving until a stranger tries
  to link it. The downstream guarantee is unchanged: an `add_subdirectory`/`FetchContent`
  consumer still gets zero CTest targets and zero add-on builds; opting in is two lines.

### Fixed — three packaging defects nothing in-tree could have caught
- **`gptps.pc` omitted `-ldl`.** The core calls `dlopen` for the add-on loader, so a
  static `pkg-config --libs gptps` link failed with an undefined reference on
  glibc < 2.34.
- **The `.pc` files were not relocatable.** They baked in the *configure-time* prefix,
  while `cmake --install --prefix` is honoured at *install* time. When those disagree —
  routinely, for distro packagers and DESTDIR staging — every `-I` and `-L` points
  somewhere that does not exist, surfacing as a baffling "gptps.h: No such file". Now
  derived from `${pcfiledir}`, with the depth computed rather than hardcoded, since
  libdir is `lib`, `lib64` or a multiarch triplet depending on the system.
- **A consumer's add-on selection was silently ignored.** `set(GPTPS_ADDONS "pool")`
  before `add_subdirectory` was clobbered by the subdirectory's own
  `set(... CACHE ...)` under CMP0126's OLD behaviour (which supporting CMake 3.13
  inherits): the consumer asked for one add-on and got all eight.
- All three are now covered by a new **`package` CI job** — installs to a staging
  prefix, asserts the install tree, then builds an out-of-tree consumer against it via
  `find_package COMPONENTS`, via pkg-config, and via the amalgamation. Every other job
  builds *inside* the tree, where every header is one `-I` away, so none of them could
  ever fail on any of this.
- `tools/check_addon_coverage.sh` holds the CMake table, the amalgamation table and
  `addons/README.md` to the directory listing. It failed on the tree that introduced it
  — six add-ons were undocumented, three of them (`pool`, `xport`, `orch`) having never
  been in `addons/README.md` at all.

### Added — ABI 2.1: add-on IDENTITY, so an ecosystem can have more than one plug-in
- **Namespaces.** An add-on may declare `ns` (e.g. `"gpuq"`). Declaring one buys a
  guarantee — the loader **claims** the token, and a second add-on wanting it is
  refused with `GPTPS_E_DUP` — and accepts a rule: every task name, setting key,
  per-task leaf and resource name it registers during `setup()` must be `"<ns>."`
  prefixed. Enforced inside `setup()` only and pinned to that thread: this is
  **attribution, not a sandbox**, consistent with `docs/SECURITY.md`, and a violation
  is logged with the required prefix rather than returning a bare error code.
- The surface where this is load-bearing rather than tidy is `gptps_define_resource`:
  a duplicate name is not an error there, it **silently re-budgets**. Two add-ons both
  defining `"gpu"` each believed they owned the budget. A claimed namespace makes that
  collision impossible instead of undetectable.
- **`gptps_addon_disable`, and deliberately no `gptps_unload_addon`.** A successful
  `setup()` can leave a settings entry holding a read/write function pair, and there is
  no `gptps_unregister_setting` — so `dlclose` would leave the settings registry
  pointing into an unmapped library and the next `gptps_settings_save` is a wild jump.
  Disable asks an add-on to stop participating; nothing is unmapped, so nothing can
  become a wild pointer. Same principle the loader already applied when refusing to
  `dlclose` after a failed setup.
- **`gptps_addon_count` / `gptps_addon_get_info`** — what is loaded, its namespace, its
  path, what it claims to be, and whether it is still enabled.
- **`gptps_set_scheduler_ex` + `gptps_scheduler_owner` + `GPTPS_SCHED_REPLACE`.** The
  seam is single-slot by design — an ordering key is a total order, and two `int64`
  scorers on no defined scale cannot be composed without silently producing an ordering
  neither author intended. So two definitions is a conflict the core now *reports*: an
  add-on passes `flags == 0` and fails its `setup()` on `GPTPS_E_BUSY`, instead of
  silently replacing the incumbent as it did before. `gptps_set_scheduler` is unchanged.
  The header names the right answer for a second plug-in: the **constraint** seam, whose
  `GPTPS_DEFER` reorders in time, composes by construction, and is many-per-engine.

### Changed — `GPTPS_SEAM_TRANSPORT` is now `GPTPS_SEAM_COMPOSITION`
- Same enumerator value, and `GPTPS_SEAM_TRANSPORT` remains as a `#define`, so every
  add-on already built is binary-identical and existing source still compiles. It is
  renamed to what it actually is: not an interface (it has no struct, typedef or
  register call) but the **composition pattern**, in which a module moves work out of
  the engine entirely. A transport calls *into* the core rather than being called *by*
  it, so an interface for it would be a vtable with no call site.
- `gptps_addon.seam` is documented as **advisory** — the loader does not inspect it and
  never will, because the field is single-valued while useful add-ons routinely span
  seams. It now has its first real consumer instead: `gptps_addon_get_info` reports it.

### Fixed — the add-on setup-failure path leaked its handle wrapper
- Retaining the **mapping** when `setup()` fails is deliberate (a partial setup can
  leave pointers the unwind cannot reach). Retaining the small handle *wrapper* was
  not — nothing references it once the load has failed. New `gptps_dl_release` frees
  the bookkeeping without unloading the library, which makes the deliberate decision
  exact and the path leak-free under LeakSanitizer. Found by the first test to
  exercise a failing `setup()`; every previous rejection failed earlier, at the magic
  gate, which already unloaded.

### Added — ABI 2.1: a binary plug-in can finally write a working task
- **The host table had no `is_cancelled`.** A `dlopen`'d task body therefore could not
  poll for cancellation, which means it could not honour a timeout, `gptps_cancel`,
  `GPTPS_REMOVE_CANCEL` or `limits.shutdown_grace_ms` — it structurally could not meet
  the liveness guarantees this library makes contractual and `tests/test_hang.c`
  enforces. Nor could a plug-in reach the symbol directly: the default build is a
  static library, so core symbols live in the host executable behind the
  `gptps_`/`gptps__` namespacing that exists precisely to prevent add-on capture.
- **This, not packaging, is why the frozen plug-in ABI shipped with zero real
  consumers** and why all seven bundled add-ons are compiled-in. It went unnoticed
  because the only plug-in in the tree, `tests/addon_demo.c`, returns `GPTPS_OK`
  immediately — the one task shape that never has to ask.
- Appended, each guarded by `struct_size` on the callee side: `is_cancelled`,
  `deadline_ms`, `now_ms`, `result_set_nocopy`, `task_setting_int`, `task_setting_str`
  (the ctx surface); `submit`, `submit_ex` (observers run with the lock released and
  *may* re-enter — an add-on had no way to accept the invitation); `settings_get`,
  `settings_set`, `settings_watch`, `set_task_priority`, `strerror`, `version` (what a
  purely config-driven add-on needs to exist at all).
- The header now also records what is **deliberately absent** and why —
  `open`/`shutdown`/`step`, `set_allocator`/`set_log_sink`, `load_addon`,
  `set_event_cb`, `dead_letter_drain` — so the omissions read as decisions.
- `tests/addon_cancel.c` is the regression guard: a plug-in whose task *loops* and can
  only be stopped through the table. If that routine is ever dropped the test hangs and
  CTest reports it, instead of the suite passing on a lie.
- Compatibility verified in **both** directions: an ABI-2.0-built `.so` still loads into
  the 2.1 core unchanged, and a 2.1-built `.so` meeting a 2.0 core is cleanly refused
  with `GPTPS_E_ABI` rather than calling past the end of a shorter table.
  `MINOR` 0 → 1; **`MAJOR` stays 2** — ABI 2.0 remains the last breaking change.

### Added — `addons/gptps_await`: the blocking wait the non-goals promised
- The "futures / promises" non-goal argued a blocking `wait(handle)` is a small amount
  of code on the observer seam and does not belong in the mechanism. That was true, but
  **nobody had written it** — so the row asked readers to take it on faith, and
  `examples/bench_pool` busy-spun on a shared atomic instead. `gptps_await_wait` now
  blocks on a handle and returns the task's result and status; `gptps_await_quiesce`
  covers "tell me when N things are done".
- Two details that make it correct rather than merely present. **The submit/finish race
  is closed**: in THREADED mode an item can finish before `gptps_submit` returns its
  handle, so the observer is installed up front and unclaimed completions are retained.
  **Retention is bounded** — a fixed-size ring that evicts oldest, rather than the
  process-lifetime growth `addons/gptps_orch` documents in its own header. The limit is
  stated in the header instead of being a surprise.
- No core change. The one guarantee the wait needs — every submitted handle reaches
  exactly one terminal event — is already contractual and already tested
  (`tests/test_reconcile`).

### Fixed — `addons/gptps_orch` released a gate while a dependency was still running
- The orchestrator treated `GPTPS_EV_FAILED` as terminal. It is not: `execute()` emits it
  after every failed ATTEMPT, and only then does the dispatcher choose retry / drop /
  dead-letter. So a dependency with `max_retries >= 1` decremented the gate twice by
  itself, and "run C after A and B" ran C **while B was still running** — because A
  retried. With a single dependency it is just as wrong: A fails once, C runs, A retries
  and succeeds, and C ran before its dependency finished.
- The terminal predicate is now `{FINISHED, DROPPED, DEAD_LETTERED}` plus `FAILED`
  carrying `GPTPS_E_CANCELLED` (a cancel is emitted exactly once — by `execute()` if the
  item ran, by the dispatcher if it never started). This is what `tests/test_reconcile.c`
  already uses to assert exactly-one-terminal-event-per-handle, and what
  `addons/durable_queue.c` already did. Gate advancement is also idempotent now, so a
  repeated terminal event could not double-decrement.
- The header documents the two dependency shapes that never reach a terminal state at
  all — `GPTPS_ON_FAILURE_REQUEUE` and `GPTPS_TASK_SERVICE` — because a gate on one waits
  forever, correctly but surprisingly.

### Changed — `examples/bench_pool` no longer busy-spins on a shared counter
- The completion wait was `while (get(&g_done) < N) { }`: a spin, inside a throughput
  benchmark, on a thread competing for CPU with the workers it was timing, incrementing
  one atomic shared by every shard. It now uses `gptps_await` per shard — no shared cache
  line, and the waiter sleeps. The documented ~15k/s → ~290k/s (≈19×) shape reproduces
  unchanged; the mid-range (4 shards) improves, which is the shared counter no longer
  throttling the harness. Note the CI-quick default of 40k items completes in ~20ms and
  is noise-dominated — pass a larger count for a figure worth quoting.

### Fixed — an exec kind this core does not know is now refused at registration
- `gptps_register_task` never range-checked `def->exec`. A value outside
  `{INPROC, OOP, PROGRAM}` registered cleanly, and `execute()` then treated
  "not INPROC and not OOP" as PROGRAM — so every item of that type ran with `argv`
  NULL, failed, burned its whole retry budget and dead-lettered. A setup mistake was
  diagnosed as a task failure. Both ends are now explicit: registration rejects the
  value with `GPTPS_E_INVAL`, and `execute()`'s final branch is a hard stop rather
  than a fallthrough. That second half is the forward-compatibility guard — if a
  future ABI MINOR ever appends a fourth kind, an older core meeting a newer add-on
  must REFUSE work it cannot run, never silently run it as something else.
  (`test_engine`.)

### Corrected — `GPTPS_SEAM_TRANSPORT` never had a consumer, or an interface
- The 1.12 entry below calls `addons/gptps_xport` "the reference consumer of
  `GPTPS_SEAM_TRANSPORT`", and `addons/gptps_xport.h` said the same. **Both were
  false in code.** `gptps_xport` consumes no seam: it never calls into an engine,
  registers nothing, and does not use the add-on host-table ABI at all. Nor is there
  anything to consume — `GPTPS_SEAM_TRANSPORT` has no struct, no function-pointer
  typedef and no register call anywhere in the library; it is an enumerator and
  nothing else.
- The history above is left as written; this entry is the correction. The distinction
  the docs now draw: **four CALLED seams** (task / constraint / observer / scheduler —
  real because the core invokes them) **and one COMPOSED pattern** (a transport sits on
  the other side of the engine and calls IN, so an interface for it would be a vtable
  with no call site). That the pattern needs nothing from the core is the strongest
  evidence for the project's own thesis, not a gap in it.

## [1.0.0] - 2026-08-06

**First stable release.** The API and the add-on ABI are now under semantic
versioning: structs grow by appending, never by reshaping, and a breaking change
bumps MAJOR. Everything below this heading shipped in it.

### Removed — the last breaking change (ABI 2.0)

Two fields deleted from `gptps_cost`, in the only window the append-only rule
leaves open — the one before 1.0:

- **`gpu_units`** — a domain-specific field in a self-described mechanism-only core,
  and one the core never enforced (its own comment said "via add-on"). ABI 1.10's
  generic named-resource budgets subsume it exactly. `addons/gpu_quota` is now a thin
  wrapper over `gptps_define_resource` / `gptps_set_task_resource_cost` — 159 lines
  to 97, with no counter, no lock and no bookkeeping of its own, and it doubles as
  the worked example for the named-resource API. Declare units with the new
  `gptps_gpu_quota_set_task_units()`.
  *Note its lifetime changed:* the quota is now a view onto the engine's ledger, so
  every accessor except `_close()` requires a live engine.
- **`est_duration_ms`** — declared a "scheduling hint" and read by no code anywhere.
  Duration-aware ordering belongs in the scheduler seam (`gptps_set_scheduler`).

### Added — a written NON-GOALS list

"Mechanism-only" is not a constraint unless it can reject something. The README now
states what the core will not grow into (distributed scheduling, queue persistence, a
metrics format, futures in the engine, DAG semantics, a logging framework, more
executor kinds, convenience wrappers) and where each belongs instead — plus the
tie-break when nothing else decides it: *does a user with a name want this?*

### Added — `docs/SECURITY.md`, and an honest trust boundary

Three places told readers to route "**untrusted**" work to `GPTPS_EXEC_OOP`. What
that actually does is fork a full copy of the host address space — every secret it
holds — with descriptors and environment inherited. That is **resource** isolation
and a guaranteed kill, not **privilege** isolation, and "untrusted" is the word that
gets someone hurt. Reworded to "unbounded or crash-prone", with `SECURITY.md` naming
the boundary, pointing at `child_setup` as the seam that fixes it, and listing the
DoS bounds and the explicit non-guarantees.

### Added — `GPTPS_BUILD_TESTS` / `GPTPS_BUILD_EXAMPLES`

Default ON at top level, OFF when GPTPS is `add_subdirectory`'d or FetchContent'd. A
consumer previously inherited all 43 CTest tests and had to build every test and
example binary to get `libgptps.a`. The two generic target names `demo` and
`bench_pool` are now `gptps_demo` and `gptps_bench_pool`, which would otherwise
collide in any parent project.

### Fixed — teardown always terminates

Four separate ways `gptps_shutdown` could stop returning. All are reachable from the
`memset`-zeroed `gptps_task_def` the quick start teaches, and because GPTPS is an
in-process library, a hung shutdown hangs the **host's** exit path — the supervisor
SIGKILLs the process and any external children survive as orphans.

- **The shutdown drain is now bounded.** In-flight work gets `limits.shutdown_grace_ms`
  (default 30000, live-settable, `0` = the old wait-forever) to finish, after which
  every running item's cancel flag is raised; the enforced executors then hard-kill
  their child within ~200ms. Previously `stop_services` raised the flag *only* for
  service items, so a `GPTPS_EXEC_PROGRAM` task with the default `timeout_seconds == 0`
  whose child never exited hung teardown forever.
- **`gptps_shutdown` and `gptps_step` are no longer re-entrant.** Called from a task
  body or an event callback they now return `GPTPS_E_BUSY` instead of joining the very
  thread making the call (a deadlock in THREADED mode) or freeing the engine that
  `gptps_step` is standing on (a use-after-free in MANUAL mode). The header explicitly
  promised callbacks may re-enter the engine; these two are now documented exceptions.
- **The external-program executor no longer blocks forever in `waitpid`.** Its pump
  breaks on *stdout EOF*, which says the child closed its output — not that it exited.
  A child that closes stdout and keeps running pinned the worker with no deadline to
  rescue it. Reaping is now bounded (`WNOHANG` + a grace period, then `SIGKILL`).
- **A zero-backoff service no longer spins a core.** The `REQUEUE` / service-restart
  paths reset `attempt`, so unlike a bounded retry nothing stops them; with
  `retry_backoff_seconds` at its zero default an immediately-failing body was
  re-admitted as fast as the dispatcher could loop. Re-admission is now floored at
  ~100ms. Bounded retries are deliberately unchanged.

### Fixed — unbounded growth

- **The dead-letter list is capped** at `limits.max_dead_letters` (default 1024,
  live-settable, `0` = unbounded), evicting oldest-first. It is the only queue a host
  is not required to drain and `DEAD_LETTER` is the default `on_failure`, so an
  undrained one grew forever, each entry pinning its original payload — an unbounded
  queue inside an engine whose entire contract is bounded admission. The truncation is
  never silent: `stats.dead_letters_evicted` counts what was dropped.
- **The OOP executor caps the result it will buffer** at 16 MiB, matching the sibling
  PROGRAM executor. The parent previously allocated whatever length the child declared,
  and on a 32-bit host `(size_t)len64` truncated — allocating a short buffer and then
  reading the rest of the record as if it were the next one.

### Fixed — a failed add-on load left dangling pointers

`gptps_load_addon` called `dlclose` when `setup()` failed, without unwinding anything
that partial setup had already registered — so an observer or constraint function
pointer into the now-unmapped library stayed on a list the engine walks on the next
event. `GPTPS_E_DUP` (a name collision) and `GPTPS_E_NOMEM` are exactly the statuses a
host logs and continues past, which turned a soft, recoverable failure into memory
corruption. A failed `setup()` is now unwound (observers, constraints, tasks, the
scheduler hook restored to their pre-`setup` state) and the mapping is deliberately
**not** unloaded, since a partial setup can leave pointers the unwind cannot reach.
Relatedly, config-file `addons = [...]` auto-load failures are now reported through the
log sink instead of being discarded.

### Fixed — the terminal-event contract observers depend on

Observers are the only completion channel in this design (the core never aggregates),
so an item that vanishes with no terminal event makes every add-on built on that seam
quietly wrong — `gpu_quota` releases its reservation only when it sees one, so a
silently-freed item leaked its GPU budget permanently.

- `gptps_unregister_task(…, GPTPS_REMOVE_CANCEL)` destroyed its queued backlog with **no
  event at all**. Every cancelled item now emits `GPTPS_EV_FAILED` / `GPTPS_E_CANCELLED`.
- An item cancelled while *running* is not double-reported: it already got its terminal
  event from the executor.
- **`gptps_cancel` no longer reports `GPTPS_E_TIMEOUT`.** A cancelled in-flight task now
  ends with `GPTPS_E_CANCELLED`, so an operator's cancel is distinguishable from a
  deadline breach. A real deadline still reports `GPTPS_E_TIMEOUT`. The same distinction
  is now made by all three executors (in-process, POSIX OOP/PROGRAM, Win32 PROGRAM),
  which additionally report a pump I/O failure as `GPTPS_E_IO` rather than a timeout.

### Fixed — fork and allocator safety

- **The OOP child no longer calls the allocator.** `gptps_run_capture` duplicated the
  task's result with `gptps_malloc` inside the forked child; if the host installed a
  lock-guarded allocator via `gptps_set_allocator`, that lock could have been held by a
  thread that did not survive the fork. It now hands out the buffer directly (the child
  `_exit`s straight after writing, so nothing leaks).
- **A host `fork()` is detected.** An engine created *before* a fork now returns
  `GPTPS_E_SHUTDOWN` from every entry point in the child rather than deadlocking on a
  mutex a vanished thread may hold. An engine opened fresh in the child is unaffected —
  the fork-a-worker-process pattern (`gptps_xport`) keeps working. New HAL entry points:
  `gptps_hal_thread_id`, `gptps_hal_fork_guard_install`, `gptps_hal_fork_generation`.

### Fixed — durable_queue survived one full disk and then bricked

A short write left a **partial record in the middle of the journal**, and replay stops
at the first record it cannot verify — so every valid record after it was silently
discarded. stdio also latches its error flag, so the queue refused every subsequent
write for the life of the process even after space was freed. Failed appends now roll
the journal back to its previous length and clear the error.

### Changed — CI can now actually fail

- The **ThreadSanitizer job** hand-listed ten test binaries and six `.c` files, so every
  test and add-on added after it was written was silently not covered — including
  `test_stress`, written specifically for TSan, and all seven add-ons. It now builds
  with CMake and runs the whole suite (excluding only `bench_pool`, for runtime).
- The **s390x big-endian job** used an include-list that skipped `abi` — the
  struct-layout gate, which is precisely what a big-endian job is for — while its own
  comment claimed serialization was covered. Both selectors are now EXCLUDE lists, so a
  new test is covered by default: 24 tests there now, up from 17.
- The **freestanding job's** `ldd | grep pthread` assertion was vacuous: glibc ≥ 2.34
  merges libpthread and libdl into `libc.so.6`, so it passed even for a program calling
  `pthread_create`. It now asserts on undefined symbols (`nm -u`), which is real evidence.
- Added a **weekly scheduled run**, so a dormant repo's green badge stays a statement
  about today.
- Two data races fixed in test/example code that the hand-rolled TSan job never
  compiled: `examples/task_control.c` published a result across threads with a plain
  store, and it is a file people copy.

### Added — regression tests for all of the above

- `tests/test_hang.c` — re-entrant shutdown/step, a no-timeout external child that never
  exits (both the never-writes and the closes-stdout-then-lives-on shapes), zero-backoff
  service restart, and the dead-letter cap. The failure mode of every check is a hang or
  unbounded growth, so the CTest `TIMEOUT` is part of the assertion. Verified to **hang**
  against the pre-fix tree.
- `tests/test_reconcile.c` — every submitted handle reaches exactly one terminal event
  across `REMOVE_CANCEL`, the `DROP` policy, and cancel-while-running, plus the
  cancel-vs-timeout distinction. Verified to **fail 4 checks** against the pre-fix tree.
- `tests/prog_helper.c` gained an `eofhang` mode (write, close stdout, keep running).
- `tests/test_settings.c` no longer pins an absolute setting count — it asserts the
  documented keys plus the per-task delta, so a new core knob is not a false regression.

### Added — the licence travels with the code

Every file under `src/`, `include/`, `addons/`, `freestanding/`, `examples/` and
`tests/` now carries an `SPDX-License-Identifier: MIT` header, and
`tools/amalgamate.sh` emits the full MIT text into **both** generated files. The
single-file drop-in is the distributed form for anyone who vendors GPTPS, and `LICENSE`
requires its notice "in all copies or substantial portions of the Software" — so the
artifact the architecture exists to enable was shipping without it. A CI step now
asserts the notice is present.

### Changed — append-safe ABI guards for all input structs
- The remaining caller-supplied input structs — `gptps_config`, `gptps_submit_options`,
  `gptps_allocator`, `gptps_addon` — now validate `struct_size` against a **frozen
  minimum** (the end of the last current field) instead of the live `sizeof`. Freezing
  the floor means appending a field to any of them later will not reject a caller
  compiled against today's header — finishing the append-only ABI discipline the header
  promises (previously only `gptps_task_def` was append-safe). No behavior change for a
  normal caller (which passes `struct_size == sizeof`); `test_abi` checks the floor is
  accepted and one byte below it rejected.

### Changed — shorter submit critical section (contention relief)
- `gptps_submit` / `gptps_submit_ex` now copy the payload, allocate the work item, and
  create its cancel flag **before** taking the engine lock, instead of inside it. None
  of that needs engine state, so moving it off-lock shortens the critical section every
  producer contends on — a measurable win for large payloads and many concurrent
  submitters (and for each `gptps_pool` shard). Strictly a default improvement: no API
  change, no behavior change, and no second concurrency model — the engine keeps its one
  simple, correct lock. (A rejected submit now does a wasted copy, but reject is the rare
  path.) New `test_stress`: 8 producer threads × 400 submits with checksummed payloads,
  all delivered intact; TSan- and ASan-clean.

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

### Added — scale-OUT by composition: the worker-process transport add-on
- **`addons/gptps_xport`** forks N persistent worker **processes** and ships each submit
  to one over IPC (a socketpair), marshalling the result back — so work runs in a
  SEPARATE address space (crash-isolated, independently capped), the reference consumer
  of `GPTPS_SEAM_TRANSPORT`. The local socketpair is the only thing between this and
  cross-MACHINE execution: swap it for a TCP socket and the same length-prefixed protocol
  reaches another host. Engine-agnostic (you supply a handler; inside it you may drive a
  gptps engine or anything), round-robin routed, thread-safe, **no core change**. POSIX
  only (fork), like `EXEC_OOP`. Two adversarial reviews (fork/fd lifecycle + protocol/
  concurrency) found it correct; the fixes from them (SO_NOSIGPIPE on the worker socket
  for macOS, a message-length cap) are included. (`test_xport`: proves out-of-process
  execution via the worker pid, fan-out across workers, error/empty round-trips.)

### Added — scale-up by composition: the shard/router add-on
- **`addons/gptps_pool`** runs N independent engine shards (each its own lock +
  dispatcher + worker pool) and routes each submit to one of them, scaling past the
  single-node single-writer ceiling **without any core change** — the proof that the
  engine scales the modular way. `gptps_pool_submit` spreads load round-robin;
  `gptps_pool_submit_keyed` pins a key to a fixed shard (per-tenant affinity / per-key
  order); a returned `gptps_pool_handle` tags the shard so `gptps_pool_cancel` routes
  back. Built entirely on the public API. (`test_pool`: even spread, affinity,
  handle-routed cancel, cross-shard dead-letter aggregation.) `examples/bench_pool`
  is a reproducible proof: on a 32-core box, aggregate tiny-task throughput rose from
  ~15k items/sec at 1 shard to ~290k at 8 (≈19x) — the single-writer ceiling, then
  composition breaking past it.

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
- **Program executor no longer mutates the host's SIGPIPE disposition.** The POSIX
  external-program executor suppressed SIGPIPE for its stdin writes with a process-wide
  `signal(SIGPIPE, SIG_IGN)` — a side effect on the embedding application. It now
  suppresses SIGPIPE without touching global state: per-fd (`F_SETNOSIGPIPE`) on
  macOS/BSD, and per-thread (`pthread_sigmask` block, with a `sigtimedwait` drain of any
  pending signal before restoring the mask) on Linux. `test_program` now asserts the
  host's SIGPIPE disposition is unchanged after a program task.
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
