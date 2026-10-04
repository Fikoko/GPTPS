# Changelog

All notable changes to GPTPS are recorded here. Format follows
[Keep a Changelog](https://keepachangelog.com/). As of 1.0.0 the project follows
semantic versioning; the ABI version (`GPTPS_ABI_VERSION_*`) moves independently of
the release version and is documented in `include/gptps.h`.

## [Unreleased]

### Upgrading from 1.5

Four changes can need a change in a host. The first three are about the config file:

Six changes can need a change in a host. The first three are about the config file,
the last three about the `durable_queue` add-on:

- **A config file with a mistake in it fails `gptps_open`.** 1.5 used what it
  understood and dropped the rest without a word: a misspelt key, a line it could not
  read, a value of the wrong type. Each is now an error. The open returns
  `GPTPS_E_CONFIG`, and the log names the file, the line and the key. Open each file
  you deploy once with this release before you ship it. The file is held to TOML's
  rules where 1.5 bent them: a quoted number, `"4"`, is a string and no longer passes
  for a number; a backslash in a string must be written `\\`; a control character in
  a string must be escaped; a file whose lines end in CR alone is refused, where 1.5
  read it as one line.
- **An add-on in `addons` that does not load fails the open.** 1.5 logged it and opened
  without the add-on.
- **`gptps_settings_save` edits the file instead of rewriting it.** It writes the values
  set live and leaves every other line as it was. Saving to a new path copies the
  loaded file with those changes. A host that saved to dump every setting gets only
  the changed ones; `gptps_settings_count` and `gptps_settings_get_info` still
  enumerate them all.
- **Some out-of-process tasks end with a different status.** Each was a bug, fixed
  below, but a host sees the change:
  - A PROGRAM task whose stdin cannot be written, for a reason other than the program
    closing it, fails with `GPTPS_E_IO`, on POSIX and Windows. 1.5 closed stdin and
    reported what the program made of the part it had, often `GPTPS_OK`.
  - On Windows, a PROGRAM task whose stdout cannot be read to its end fails with
    `GPTPS_E_IO`. 1.5 reported the part it had read, often as `GPTPS_OK`.
  - An OOP task whose child cannot write its whole result fails, with
    `GPTPS_E_TASK` or `GPTPS_E_IO`, where 1.5 could report a `FINISHED` carrying the
    wrong bytes.
  - A PROGRAM result of exactly 16 MiB is returned, where 1.5 failed the task with
    `GPTPS_E_IO`. A byte more still fails.
  - In a host that closed its standard descriptors, as a daemon does, an OOP task that
    prints now returns its result, where 1.5 failed it with `GPTPS_E_IO`. Such a host
    needs two free descriptors above fd 2 for each OOP task it starts: without them
    the attempt fails with `GPTPS_E_IO`, where 1.5 ran a task that printed nothing.

- **A task type is there for other threads once its registration returns.**
  `gptps_register_task`, and `gptps_clone_task`, which calls it, take the name at once:
  another registration of it gets `GPTPS_E_DUP`. Until the call returns, the type's
  settings are still being added, and every call from another thread that names the
  type treats it as absent: a submit, a pause, a change of its priority or costs, a
  clone from it and its unregister return `GPTPS_E_NOTFOUND`; `gptps_task_exists`
  returns 0; `gptps_task_count` and `gptps_task_get_info` leave it out. In 1.5 they
  all found it from the start, and an item could run before its settings existed. A
  host that registers on one thread and uses the type from another should do so once
  the registration has returned. On the registering thread - in an add-on's watcher
  that hears the type's file values - the type is there, but removing it returns
  `GPTPS_E_BUSY`.

- **`gptps_dq_open` returns NULL where 1.5 lost records.** It does so for a journal it
  cannot read - one that exists but will not open for reading, or a read that fails -
  and for a journal cut short by damage when no copy of it can be made. 1.5 opened,
  and the compaction an open runs rewrote the journal without every record it had not
  read. The journal is now left as it was: retry once the cause is gone. A failed
  open now unregisters the observer it registered, so, like
  `gptps_register_observer`, call it before the engine runs work.
- **A failed durable call can break the queue.** A submit, batch or retraction that
  returns `GPTPS_E_IO` now returns only once its write is durably cut back out of the
  journal, so that it cannot come back. If even that fails, the queue writes nothing
  more: every durable call returns `GPTPS_E_IO`, and an error goes to the log sink,
  until `gptps_dq_compact` returns `GPTPS_OK`. A host whose retries on `GPTPS_E_IO`
  keep failing should try a compaction. 1.5 went on writing.
- **`gptps_dq_compact` returns `GPTPS_E_IO` when it cannot sync the journal's
  directory** after putting the new journal in place, and so does the drain's
  `*out_compact`. Until a sync of the directory succeeds, durable calls wait for one,
  and fail with `GPTPS_E_IO` if it fails. 1.5 ignored the failure. A directory that
  cannot be synced at all - one the process may write but not read, or on a file
  system with no fsync for a directory - is not a failure: there the queue carries on
  as 1.5 did.

### Added — bounded mode: no allocation once work starts (ABI 2.4)

- **`gptps_config.max_items`, `max_payload_bytes` and `max_result_bytes`.** With
  `max_items` set, the first submit that names a registered task allocates the
  engine's whole working set, and the work path neither allocates nor frees after it:
  - items come from a fixed pool, each with its own payload slot and named-resource
    snapshot slot;
  - the handle index is made at its final size and never rebuilt;
  - each executing thread owns one result buffer;
  - the callback-thread records are made in advance.

  Every operation's cost is then bounded by the configured maxima. A full engine
  answers `GPTPS_E_FULL`. A payload or a `gptps_result_set` past its maximum is
  `GPTPS_E_INVAL`. Setup that would allocate is `GPTPS_E_BUSY` after the seal, and
  process-based task kinds are refused, since starting a process allocates by nature.
  A submit that names no registered task seals nothing, so a typo in the first one
  does not end setup. For a static arena, the seal took 339 bytes per item at 256
  items (64-byte payloads, 32-byte results, two named resources). Measured against
  the classic mode, with both limited to 4,096 items: latency is the same, one
  producer is 5-10% faster, and four producers are 5-10% slower, because the pool has
  its own lock. `max_items` is a `uint64_t` so that it grows `gptps_config` on every
  ABI: a caller built against an older header never appears to have set it.
  `docs/BOUNDED.md` has the rules, sizing and speed.
- **`tests/test_bounded.c` holds it to that.** A counting allocator drives a sealed
  engine through the work path, in MANUAL and THREADED mode: payloads, results,
  retries, backoff, a deadline, dead letters and their drain, eviction, named
  resources, a constraint that defers, cancels, settings writes, and a full pool and
  its reuse. It fails on any allocation or free after the seal. Removing any one of
  the pool, the result buffer, the snapshot slot, the callback records, the fixed
  index, the drain's skip of name copies, or a setup refusal makes it fail.
  - Writing it found two bugs before they shipped. The dead-letter drain copied every
    item's name. Those copies are needed only so a callback can unregister the task
    type, which a sealed engine refuses, so the drain no longer makes them there.
    And a pooled item went back to the pool without its name copy, which leaked.
  - As an experiment, the whole suite was run with every engine made bounded. Of 74
    tests, 50 passed, and each of the 24 that failed stopped at a documented refusal.

### Added — the dead-letter cap and the shutdown grace at open (ABI 2.4)

- **`gptps_config.max_dead_letters`, `gptps_config.shutdown_grace_ms` and
  `GPTPS_LIMIT_NONE`.** A host that configured through `gptps_open_ex` could not set
  either limit at open. Neither was in `gptps_limits`, where 0 means "not set", while
  for these two 0 already means something - keep every dead letter, wait forever -
  and the defaults are 1024 and 30000 ms. Only a config file or a live
  `gptps_settings_set` reached them. In the report, 5,000 failures overnight left
  1,024 dead letters, and the other 3,976 were evicted, counted only in
  `stats.dead_letters_evicted`; the new test measures the same.
  - The two `uint32_t` fields are appended to `gptps_config`, after the bounded-mode
    ones. 0 means not set: the default, or the config file's value.
    `GPTPS_LIMIT_NONE` (`0xFFFFFFFFu`) means no limit, and becomes the 0 the settings
    and the file use for it. Any other value is the limit. So the struct cannot ask
    for a limit of exactly 4,294,967,295, which no host could reach: that many dead
    letters would need over 890 GB on x86-64, at 208 bytes an item before its payload,
    and that grace is 49.7 days. The setting and the file still take the number.
  - An explicit value wins over the config file, as `cfg.limits` does, and the file's
    value is still checked. A live set or a reload changes either one as before.
  - They are read only when `struct_size` covers them, so a caller built against an
    older header has neither. Two `uint32_t` grow `gptps_config` by 8 bytes on every
    ABI checked (from 72 to 80 bytes on x86-64, from 48 to 56 on i386), and
    `tests/test_abi.c` asserts that each field appended to it starts at or past the
    size of the struct before it.
  - `gptps_limits` could not take them. `gptps_config` embeds it by value with fields
    after it, so a field appended to it would move `mode` on i386 Linux, and on 64-bit
    and the other 32-bit ABIs land in its 4 bytes of tail padding, where
    `limits.struct_size` cannot tell an older caller's leftover bytes from a value. A
    note in `gptps_limits` now says where the two limits are, and why.
  - A bounded engine with no cap keeps every dead letter until the pool is full, and
    `gptps_submit` then returns `GPTPS_E_FULL` until a drain frees them.
    `docs/BOUNDED.md` says so.

  Reported by @nightops00dev in #15.
- **`tests/test_open_limits.c`** checks the defaults, explicit values,
  `GPTPS_LIMIT_NONE`, an older caller whose struct ends at `max_result_bytes` with
  garbage after it, the struct against the file both ways, a live set and a reload
  afterwards, and a bounded engine. It runs the report's night: 5,000 failures keep
  1,024 dead letters and evict 3,976 by default, and keep all 5,000, oldest first, with
  `GPTPS_LIMIT_NONE`. With an explicit 30 ms grace, shutdown cancels a running body;
  with `GPTPS_LIMIT_NONE`, it waits for the body to finish. Each of ten deliberately
  broken builds fails it: one reads the struct without its size check, one stores
  `GPTPS_LIMIT_NONE` as itself, one lets the file win, and so on.

### Added — a conformance test for the HAL, and a HAL that takes every freedom it allows

- **`tests/test_hal_conformance.c` holds a HAL to its contract.** It has 37 checks:
  - the clock never decreases, on any thread, and runs at the rate of real time,
    measured against `time()`;
  - mutual exclusion;
  - waits lose no wakeup, and a broadcast wakes every waiter;
  - timed waits neither oversleep nor spin, for every timeout the core can pass;
  - acquire/release message passing;
  - the fork generation and thread ids;
  - dynamic loading and the atomic replace.

  CTest runs it against the HAL of every build, and a port built with
  `-DGPTPS_HAL_SOURCE` runs the same file. CI's freestanding job runs it against the
  stub in a MANUAL-only profile, where `--freestanding` declares what the stub lacks
  and each skipped check says what stops working without it. Ten deliberately broken
  POSIX HALs each fail it at the check named for what was broken: a clock in ticks
  instead of milliseconds, a broadcast that wakes one waiter, a wait that spins, a
  mutex that does nothing, and six more. A HAL whose acquire/release are plain
  accesses passes on x86, as it must, and fails under ThreadSanitizer, which CI runs
  it under.
- **`tests/hal_chaos.c`: the suite on the weakest HAL the contract allows.** It takes
  every freedom the contract leaves a port, often:
  - waits that return without a signal;
  - timed waits that return early, or a clock step late;
  - signals that wake every waiter;
  - a clock that moves in 16 ms steps;
  - locks that yield first, and threads that start late.

  The suite passes on it, except the three `*_perf` timing gates: they time a
  curve on the HAL's own clock, which this HAL coarsens and perturbs on purpose, and
  one failed that way on a loaded run. CI's new `hal_chaos` job leaves them out and
  runs everything else with a new seed each time. With the conformance test, that
  checks both sides of the contract: a HAL keeps it, and the core needs nothing more.
- **`docs/HAL.md`** gives the contract clause by clause, with why the core needs each
  clause and the check that holds a HAL to it. It also lists what the test cannot
  show: memory ordering on x86, wall-clock steps, a failed thread start.

### Added — a simulation HAL: a schedule is a seed, and a failure replays

- **`tests/hal_sim.c` makes the interleaving of threads a function of a seed.** Every
  engine thread is a real OS thread, but only one runs at a time. At each HAL call, and
  at each pthread, sleep and `clock_gettime` call of the add-ons and tests, a PRNG
  seeded from `GPTPS_SIM_SEED` picks the thread that runs next. Time is virtual: when
  every thread waits, the clock jumps to the earliest deadline, so a 30 s shutdown
  grace costs nothing and a timeout fires at the same point of the schedule every run.
  A deadlock, or a HAL contract broken on the spot, is reported with each thread's
  state and last call instead of hanging. At exit it prints the seed and a hash of
  every decision it made, and the same seed and CPU count print the same hash. A
  forked child runs on real time, so from a run's first fork on the clock keeps to
  real time too, and the exit line says such a run may not replay. It passes the
  conformance test except the check that the clock keeps to real time; with
  `GPTPS_SIM_PACE=1`, which paces a run from the start, it passes that one too.
  `docs/HAL.md` says how to replay a seed and search many, and which tests cannot run
  on it, and why.
- **CI's new `hal_sim` job** builds the suite on it and runs it on three seeds a run,
  from the run number, with the CPU count pinned to 4. The conformance test and the
  tests about child processes run in a second step, paced from the start. The first
  step leaves out the `*_perf` gates and the two benchmarks, which time a clock that
  is virtual here: on a seed that switches threads at every point, `gptps_bench_pool`
  took 71 to 87 s, against its 60 s timeout. It runs `stress_api` for one 1.5 s round,
  with its seed pinned to the simulation's: until a round forks, its 1.5 s are
  virtual, and the default six rounds ran past the test's 120 s timeout on such a
  seed.
- **`orch` failed now and then: the test raced its own worker.**
  `check_unsatisfiable_gate` submits a dependency, then makes a gate on it with
  `gptps_orch_after`. If a worker ran the dependency in between, `gptps_orch_after`
  took its documented fast path: with every dependency already terminal, it submitted
  the gated task, `no_such_task`, at once and returned the engine's
  `GPTPS_E_NOTFOUND`. The engine was right, but the test counted that as a failed
  check without saying which, so all it showed was an occasional
  `1 orch check(s) FAILED`. On a loaded machine that was 12 of 200 runs under
  ThreadSanitizer and 2 of 300 of a RelWithDebInfo build, and every run with a 50 ms
  pause put between the two calls. The simulation HAL lost the race on 4 of seeds
  1-100. The dependency now waits until the gate exists, and the test passed 300 runs,
  50 under ThreadSanitizer, every run with the pause, and seeds 1-200.

### Added — a config file can describe a whole deployment, and says what is wrong with it

- **`[resources]`, `[tasks.<name>.resources]` and `[bounded]` in the file.** Named
  resources and their budgets, what one run of each task costs of them, and bounded
  mode's maxima could be set only from code; a deployment can now carry them in its
  file. Each budget and each cost is also a setting (`resources.<name>`,
  `tasks.<name>.resources.<name>`), so the settings API, the dashboard and save see
  them too.
- **`gptps_config_check(e)` (ABI 2.4).** A key that only a later definition can claim
  waits for it: a `[tasks.<name>]` table until that task registers, a plug-in's or the
  host's setting until it is defined, and each is checked and applied then. Called
  once setup is done, `gptps_config_check` logs every key nothing has used, with a
  suggestion for a near miss (`no task named resise is registered (did you mean
  resize?)`), and returns `GPTPS_E_CONFIG` if there is one. Without the call, the first
  submit logs the same keys once, as warnings.
- **`gptps_settings_set_ex` (ABI 2.4)** is `gptps_settings_set` that says why it refuses
  a value (`70000 must be a whole number between 0 and 65536`), for a UI to show.
- **`docs/CONFIG.md` lists every key**, with its type, range, default, and whether a
  change applies at once or at the next start. `tools/gptps_config_reference.c`
  generates it from the settings registry, by registering a task named `<task>` and a
  resource named `<name>`, so the keys they create come out with their placeholders in
  them. The `config_reference` test fails while the page and the code disagree.
- **The dashboard's settings editor explains itself.** Under the list it shows what
  the selected setting does, what it takes, its default, and whether a change applies
  at once or at the next start. A value it refuses comes with the reason, a byte count
  comes with its size in KiB, MiB or GiB, the key column fits the longest key, and the
  list scrolls to keep the selection on screen. `w` reports where it saved, or why it
  did not.

### Changed — an item's cancel flag lives in the item

- **A submit no longer allocates in the HAL, so every per-item allocation goes through
  `gptps_set_allocator`.** Each item's cancel flag was a separate block that the HAL
  allocated with libc `malloc` on POSIX and Windows. That bypassed the allocator hook,
  so a host with a static arena still touched the libc heap on every submit. The flag
  is now a word inside the item, read and written through the HAL's acquire/release
  pair. The core no longer calls the HAL's four `gptps_flag_*` functions: a new port
  can leave them out, and an existing one still builds. The conformance test checks
  the acquire/release pair on the cancel path instead.
- **The handle index leaves no tombstones.** A deletion closes its gap by moving later
  entries of the probe run back, so the index never has to be rebuilt just to clear
  deleted slots. `tests/test_hidx.c` drives it against a reference model, on small
  tables where probe runs wrap past the end; dropping the shift, an off-by-one in it,
  or mishandling the wrap each fails it.

### Changed — the config file is checked as it is read, and save edits it in place

- **One check for every value, from a file or live.** The parser is strict: a line it
  cannot read fails the open with its file and line, and so does a NUL byte. Every
  value then goes through the check `gptps_settings_set` makes, after one that it is
  the kind of value its key takes. A key is judged by its dotted name, however the file
  spells it, so `"limits.max_memroy_bytes" = 1` is the same typo as the one in a
  `[limits]` table. A key the engine's own tables (`[limits]`, `[scheduler]`,
  `[bounded]`, `[stats]`) do not have is an error with a suggestion, and so is a table
  name a letter or two from one of the engine's, when the key under it is that
  table's: `[limit] max_concurrent_tasks` is a typo, a host's `[status] code` is its
  own table and waits for its definition. A value a plug-in refuses as it loads, inside
  the open, fails the open too. One attempt
  reports every problem, not the first one only: every line that does not parse, or,
  once the file parses, every value and key that is wrong:

  ```
  config gptps.toml:2: limits.max_concurrent_tasks: 70000 must be a whole number between 0 and 65536
  config gptps.toml:9: tasks.resize.on_failure: retry must be one of: dead_letter, requeue, drop
  config gptps.toml:3: limits.max_memroy_bytes: [limits] has no such key (did you mean limits.max_memory_bytes?)
  config gptps.toml:6: schedular.reserve_after_skips: there is no [schedular] table (did you mean [scheduler]?)
  config gptps.toml: 4 errors - the engine was not opened
  ```

  `gptps_settings_reload` makes the same checks: a file that does not parse changes
  nothing; otherwise every valid value is applied, each problem is logged, and the
  result is `GPTPS_E_CONFIG` if there was one.
- **`gptps_settings_save` updates the file in place.** It used to regenerate it, which
  dropped every comment, so saving from the dashboard destroyed a hand-written file's
  notes. Save now rewrites only the values set live, each on its own line, keeping that
  line's comment, and adds a changed setting the file lacks next to its siblings. Every
  other line stays as written, so a `0 = auto` stays auto, instead of becoming this
  machine's core count or memory. A file that does not parse is not overwritten: save
  logs why and returns `GPTPS_E_CONFIG`. Saving to a new path writes a copy of the
  loaded file - its add-ons, `[task_defaults]` and comments - with the live changes
  made in it. The `stats.dead_letters_evicted` counter is never written. A host's
  settings watchers hear only `gptps_settings_set`, as before, so one that saves on a
  change never saves over a file being reloaded. On POSIX the file keeps its
  permission bits across a save; it used to take the process umask's, so a config
  kept to its owner came back readable by others, and one read-only to its group could
  come back writable by it. A save while a reload is applying its file returns
  `GPTPS_E_BUSY`, rather than write a live value over the file being read.
- **The core settings' descriptions say what each one does and what 0 means.** The
  dashboard and `docs/CONFIG.md` show them.
- **`gptps_hwinfo.has_gpu` is documented as outside the HAL contract.** The core never
  read it. A GPU, like any device a task holds, is a named resource the host or the
  config file defines.

### Fixed — config values that went in unchecked

Each measured against 1.5.0's code, which this part of the engine had not changed
since.

- **Per-task values from the file were cast without a check.** `max_retries = -1`
  became 4294967295 retries, `timeout_seconds = -1` a timeout of 136 years, and
  `priority = 99999999999` became 1215752191. `on_failure = "retry"` was ignored, and
  the task kept its compiled-in policy. Each now fails the open.
- **A live set past a per-task value's width was truncated.** Setting
  `tasks.<name>.timeout_seconds` to 4294967296 was accepted and stored 0, which means
  no timeout. The per-task keys now declare their ranges, and the set is refused.
- **The parser accepted lines it could not read.** A file with an unclosed `[limits`
  opened; so did `max_concurrent_tasks = = 4`. A `[limits]` value out of range made
  the open fail with no message at all. Both now fail with the file and the line.
- **Quoted keys and table names matched nothing.** The parser kept the quotes as part
  of the name, so the Readme's `"gpuq.units" = 2` example set no setting, and
  `[tasks."resize v2"]`, which is how TOML names a task with a space in it, matched no
  task. Quoted parts of keys and table names are now read as TOML reads them, and a
  dotted key means the same whether the dots are in the table name or the key.
- **A number setting took values no file could hold.** A setting with a range of 0 to
  1 accepted `nan`, because every comparison with NaN is false, and any number setting
  accepted a hex float such as `0x1p-1`, or `inf` where it had no range. Such a value
  then went into a saved file, which no longer opened. A number is now what a config
  file writes: a sign, digits, a fraction and an exponent.
- **A backslash in a string vanished.** `"C:\plugins"` read as `C:plugins`, a path that
  does not exist, without a word. The parser now reads TOML's escapes, `\uXXXX`
  included, and refuses any other.
- **A plug-in could not be configured from the file.** `docs/PLUGINS.md` says one can
  be, but a plug-in's own keys in the file were never applied at open, and a reload
  applied them without telling the plug-in, which learns its values through a
  settings watcher. So `[gpuq] total_units = 4` left the GPU quota plug-in's budget
  unlimited. Now an add-on's keys take the file's values when its setup returns, and
  a watcher an add-on registers hears a config file's values - at open, at a reload,
  and as a task takes its per-task value - while a host's watcher still hears only
  live sets. An add-on whose setup fails has its watchers switched off and the
  settings it registered with `register_setting` or `define_global` removed: their
  accessors and targets are its own, and in 1.5 they stayed, so a later set or
  reload could run its code after the setup that should have readied it had failed.
  (A per-task setting it defined stays; the engine owns its cells.)
  `tests/test_plugin_tier.c` configures the quota from nothing but a file and checks
  that it bites.
- **Under a locale with a decimal comma, numbers were misread.** A host that set
  `LC_NUMERIC` from the environment - as GUI toolkits do - read `max_memory_gb = 4.5`
  as 4 GiB without a word. Numbers are now read and written as the C locale has them,
  whatever the host's locale.
- **An unsigned value past 2^63 could not be written in the file.** A budget or a
  byte count of up to 2^64 - 1 is a valid setting, but the parser refused it as too
  large a number, and the open failed with no message. It now reads.
- **The dashboard printed `(null)` as its title** in the settings, tasks and
  dead-letter panes when `gptps_tui_config.title` was left unset.

### Fixed — the POSIX HAL's timed wait

Both found while writing the conformance test.

- **On a 32-bit `time_t`, a long task timeout made the dispatcher spin.** The
  dispatcher sleeps until the next deadline, and the POSIX HAL added the wait to the
  clock without a clamp. A deadline far enough away overflows a 32-bit `time_t`:
  about 68 years on Linux, which a `timeout_seconds` of `UINT32_MAX` written to mean
  "none" is. `pthread_cond_timedwait` then returned at once, every time. Before the
  fix, the test's wait returned 25,255 times in 200 ms on CI's i386 runner. Waits are
  now clamped to a day, and the core works out its wait again on every wakeup. A
  64-bit `time_t` was not affected.
- **On macOS, timed waits followed the wall clock.** macOS has no
  `pthread_condattr_setclock`, so the dispatcher's sleep ended at an absolute time on
  `CLOCK_REALTIME`. Setting the clock back stretched the sleep by as much as the clock
  moved, so a deadline or a backoff due in a second could fire hours late. The wait is
  now relative (`pthread_cond_timedwait_relative_np`), measured on a clock nothing
  moves.

### Fixed — the out-of-process executors

- **An OOP task failed in a daemonised host if it printed.** A host that has closed
  its stdin and stdout, as a daemon does, gets those numbers back from `pipe()`, so the
  OOP executor's result pipe was fds 0 and 1, and the child's end of it was the task's
  own stdout. Whatever the task printed went down the pipe ahead of its result, the
  parent read that text as the result's header, and the task failed with
  `GPTPS_E_IO`. With fds 1 and 2 free, a task that wrote to stderr failed the same
  way. Both ends of the pipe now move above fd 2, as the PROGRAM executor's child
  already moved its own; if no descriptor is free there, the attempt fails with
  `GPTPS_E_IO`. `tests/test_oop.c` runs both shapes, and `tests/test_exec_faults.c`
  fails the move.
- **A PROGRAM result of exactly 16 MiB failed with `GPTPS_E_IO`,** on POSIX and on
  Windows. 16 MiB is the cap, and the OOP executor accepts a result of that size, but
  the PROGRAM executors refused as soon as their buffer was full, before they knew
  whether more was coming. They now read one byte more: the end of the output there
  is a result of exactly the cap, and a byte is one too many.
  `tests/test_program_helper.c` runs results of 16 MiB less a byte, exactly 16 MiB and
  a byte over, which CI does on Linux, macOS and Windows.
- **On Windows, a PROGRAM task could finish on part of its output.** The executor took
  any error reading the program's stdout for the end of it, so a program that exited 0
  was reported as a success with what had been read. Only the end of the output
  (`ERROR_BROKEN_PIPE`) means that now. Any other error stops the program and fails
  the attempt with `GPTPS_E_IO`, as it does on POSIX. CI compiles the change, but no
  test makes a Windows pipe read fail.

`tests/test_exec_faults.c` runs OOP and PROGRAM tasks through the POSIX executors and
fails, one at a time, each call they make to fork, pipe, pipe2, dup2, poll, read,
write, close, waitpid, kill, setrlimit and execvp, and to fcntl to move a descriptor
above fd 2, then checks what each task reports. It leaves the cgroup path off. It
found two ways a failed write ended as a `FINISHED` with the wrong result:

- **An OOP task could finish with the wrong bytes.** The child sends its result as a
  status, a length and a payload, in three writes, and it went on after one failed.
  Linux's pipe write fails with `ENOMEM` when it cannot get a page for the buffer, and
  nothing makes the next write fail too. The test injects that sequence; it has not
  been seen on a real system. With the length lost, the parent read the payload's
  first 8 bytes as the length, and a payload that starts with a small number parsed as
  a whole record: a `FINISHED` carrying 3 of its 11 bytes, in the test. The child now
  stops at the first write that fails, so the parent sees a torn record and fails the
  attempt.
- **A PROGRAM task could finish on part of its payload.** The executor took any error
  writing the program's stdin to mean the program had closed it: it closed stdin and
  let the program run on what it had, so a program that exits 0 on short input -
  `cat`, in the test - was reported as a success with its output for part of the
  payload, from none of it to 94208 of its 98304 bytes. Only the program closing its
  stdin means that now: `EPIPE` on POSIX, `ERROR_NO_DATA` or `ERROR_BROKEN_PIPE` on
  Windows. Any other error fails the attempt with `GPTPS_E_IO`, and the program is
  killed before it sees the end of its input, as an error reading its stdout is
  handled on both. The Windows change is compiled by CI but not run, since the test
  is Linux-only.

### Added — a randomized stress of the public API

- **`tests/test_stress_api.c` searches where two features meet on two threads.** Eight
  threads draw operations at random, from one logged seed, out of what the threading
  contract lets run at once: submit and cancel; settings get, set, reload, save and
  check, against a config file other threads keep rewriting; pause, clone, unregister
  and register again, in-process and PROGRAM; re-budgeting; the dead-letter drain; an
  add-on load that fails. The callbacks re-enter as the contract invites. Rounds vary
  THREADED and MANUAL, classic and bounded. It checks that every handle gets exactly
  one terminal event; that cancelling closes every open one and leaves nothing queued,
  running or reserved; that no setting outlives its task and no live task lacks one;
  that a saved file reopens; that a drain calls back once for each dead letter it
  takes; that shutdown keeps its grace; and the sanitizers. A hang fails it too: a
  watchdog names what each thread was doing, before CTest's timeout. Each round prints
  its seed: `--seed S --rounds 1` runs it again, and `--replay` runs its operations on
  one thread, in order. The seed comes from the clock unless `GPTPS_STRESS_SEED` or
  `--seed` pins it. By default it runs six rounds of 1.5 s; `GPTPS_STRESS_MS` makes it
  a soak. It found the four defects below, and reviewing their fixes found more of the
  same kind, fixed with them.

### Fixed — defects the stress test found

- **Two saves at once failed, and could put a cut-off file in place.** Every save to a
  path writes `<path>.tmp` and renames it over the file. A second save that started
  between the first's write and its rename truncated that temporary file and wrote into
  it, so the first renamed a half-written file into place, and the second's rename
  found nothing to move: `GPTPS_E_IO`. `tests/test_config_strict.c` has two threads
  save one 33 KB file 300 times each while a third reads it whole. Before the fix, in
  ten runs of a RelWithDebInfo build, 69 to 233 of the 600 saves failed, and the
  reader found the file cut off 48 to 1,770 times and empty or missing 891 to 61,649
  times; in ten runs of a Debug build, 151 to 256 saves failed. The counts follow
  scheduling and load. Saves of one engine now take turns, from reading the file to
  renaming its replacement into place.
- **A failed add-on load could remove what the host registered while it ran, or
  everything.** The unwind of a failed setup took everything ahead of each list's head,
  as the head was when the setup began, for the setup's own: task types, observers and
  constraints. A type, an observer or an admission constraint a host thread registered
  while the setup ran was removed with it, without a word. If that old head was
  removed meanwhile, the unwind never met it again and removed every type, observer or
  constraint in the engine; or, its memory reused by a newer one, it stopped at once
  and left the failed add-on's in place. It also put back the scheduler it had found,
  over one the host set meanwhile. Each now records the load that registered it, and
  the unwind removes exactly those; it puts the scheduler back only if the failed
  setup was the last to set it. `tests/test_config_strict.c`, with `addon_waiter`
  given a cue to fail, and an observer, a constraint and a scheduler of its own.
- **An item could run before its type's settings existed, and an unregister could free
  a type still being registered.** `gptps_register_task` made a type known, then added
  its settings. An item another thread submitted in between read its own per-task
  setting as `GPTPS_E_NOTFOUND`. An unregister in between - a `gptps_clone_task` and a
  `gptps_unregister_task` of one name, both live calls, are enough - freed the type
  while the registration was still writing into it. ASan reported that use-after-free
  in each of 10 runs of the new test without the fix. So could an add-on's watcher
  that removed the type as it heard the type's file values, on the registering
  thread. Until its registration returns, a type is now not there for other threads
  (see "Upgrading from 1.5"), and its own thread cannot remove it; its name is taken
  from the start. `tests/test_taskmgmt.c` parks a registration in that window and
  checks each call, and races a clone against an unregister for a second;
  `tests/test_config_strict.c` has `addon_submit`'s watcher try the removal.
- **A service whose timeout had been set live could not be cloned.** A service's items
  run with no timeout whatever `tasks.<name>.timeout_seconds` says, but the value is
  kept in the type's definition. `gptps_clone_task` copied it, and registration refuses
  a service with a timeout: `GPTPS_E_INVAL`. The copy now takes the policy a service
  runs with. `tests/test_service.c`.

### Fixed — `durable_queue`: I/O errors that lost acknowledged work, or brought back failed work

`tests/test_durable_crash.c` kills a process at each I/O call the queue makes during a
workload, one call per run: before the call, after it, in the middle of a write, or
with a power cut that takes everything not yet fsync'd. Or it makes that call fail -
or that call and the next fsync - and lets the workload go on. Then it recovers the
journal and checks it against `gptps_durable_queue.h`. A sweep is about 11,800 runs:
in 200 sweeps here, on a shared 4-CPU machine, each took 28 to 60 s (19 to 29 s of
CPU). Linux only. No crash broke a promise. Failed calls broke five, all present in
1.5.0:

- **A submit that returned `GPTPS_E_IO` could come back after a power cut.** When an
  fsync failed, the group commit cut the journal back to the last good fsync and failed
  every write still waiting, but never made the cut durable. The bytes it cut could
  already be on disk: an fsync settles only the writes made before it began, yet may
  catch one made while it runs, and the kernel's own writeback can catch one at any
  time. A write whose own flush failed was cut back the same way. The test meets this
  only when one submitter writes while another's fsync runs, which none of 10 runs here
  did. With its fsync model slowed by 3 ms, so that more is written while an fsync
  runs, 1.5.0's queue brought such a submit back in 7 of 1,100 targeted runs. Now a
  failure is reported only once the cut is durable, and a queue that cannot make it
  durable stops writing (see "Upgrading from 1.5"). The same slowed runs bring none
  back.
- **A read error while opening the journal deleted records.** `gptps_dq_open` took a
  journal it could not open for reading (EIO, EACCES, ENFILE) for one that did not
  exist yet. It took a failed read for the end of the file, or for damage. The
  compaction that follows then rewrote the journal without every record it had not
  read. One failed `fopen` lost all eight open records of the test's journal; a
  failed `fread` lost the records from that point on. The open now returns NULL and
  leaves the journal as it was, as the header says it does on an I/O error.
- **A journal cut short by damage was compacted though no copy of it could be made.**
  The header says the records after a cut are "reported and kept in the copy, but not
  loaded". When the copy failed (a full disk, a failed fsync), the compaction went
  ahead, and those records were gone. The open now returns NULL and leaves the journal
  as it was. Other damage is compacted as before when no copy can be made: the copy
  would hold only bytes already lost.
- **A compaction ignored a short write of the journal's header.** It renamed a journal
  with a broken header into place, and from then on every `gptps_dq_open` returned
  NULL, with every record out of reach until someone repaired the file by hand. The
  compaction now fails with `GPTPS_E_IO` and keeps the old journal.
- **A compaction ignored a failed fsync of the journal's directory.** Until that
  fsync succeeds, a power cut can undo the rename. That takes everything written to the
  new journal since: submits acknowledged as durable were lost, and records retracted
  since came back. The compaction now returns `GPTPS_E_IO`. Until a later sync of the
  directory succeeds, the group commit syncs it again before it acknowledges anything.
  `gptps_dq_open` does not fail on it: once its compaction has replaced the journal,
  it returns the queue, with a warning.

Three more, found in review of the same code, each in 1.5.0 as well:

- **An earlier copy that the process could not read was overwritten.** A copy goes to
  the first of `.corrupt` to `.corrupt.9` that does not exist, but any name that would
  not open for reading counted as free: a mode-0200 copy, or another user's, was
  truncated and replaced. On 1.5.0's queue, a 31-byte earlier copy became the new
  63-byte one. A name is now free only when the file system says nothing is there.
- **With all ten names taken, no copy was made.** Together with the fix for cut
  journals above, that would have kept such a queue from opening for good. The oldest
  copy, by modification time, is now replaced; and a copy identical to the journal is
  not made again, so retrying an open that failed does not use up the names.
- **An open that failed could leave an empty journal behind.** When the compaction of
  a new queue could not rename its file into place, it reopened the journal to put back
  an append handle the open never had, which created it.

Each fix was taken out in turn. Without any one but the first, the test failed on its
first run here. Without the first, it passed 10 plain runs, and failed 3 of 3 with
`GPTPS_CRASH_SYNC_DELAY_US=3000`, which slows its fsync model by 3 ms; with it, it
passed 4 of 4 that way.
The test also runs the workload with every fsync of the directory failing with
EINVAL, and checks a directory the process may write but not read (mode 0300).

### Documentation

- **`examples/item_ledger.c`: what a threaded host must add.** In THREADED mode an
  item can finish, and its event reach the observer, before `gptps_submit` returns its
  handle. The example's lookup by handle then finds no row, drops the event as a late
  one, and the row stays open. The example's note told threaded hosts only to put a lock
  around the ledger. It now says to hold that lock from before `gptps_submit` until the
  row has its handle, and to take it in the observer only after the kind check, since
  `QUEUED` is delivered inside `gptps_submit` on the thread holding it. In a threaded
  run of the example's pattern, with a no-op task and 16 workers, the old advice left
  6-10 rows in 100,000 open; the new one left none.

## [1.5.0] - 2026-10-02

### Upgrading from 1.4

No API changed incompatibly, and the ABI is still 2.3. Three changes can need a change
in a host:

- **`gptps_dq_recover` can return fewer records than are pending.** A record that was
  running at two deaths of the process is a suspect, and suspects are recovered one at
  a time: the count includes at most one, and the others are handed to the engine as
  each turn ends. Code that expects every pending record enqueued when the call
  returns, or compares its count with `gptps_dq_pending`, must not.
  `gptps_dq_set_resubmit_cb` reports each record as it goes.
- **`gptps_dq_open` returns NULL if it runs out of memory loading the journal,** where
  1.4 compacted away whatever it had not loaded.
- **A queued item costs 48-80 more bytes at 64-bit:** 16 in the item, and 32-64 in the
  engine's new index by handle, which briefly holds its old and new tables while it
  grows. A host whose allocator is a fixed pool sized for 1.4 needs that headroom; a
  submit that cannot grow the index fails with `GPTPS_E_NOMEM`, as one that cannot
  allocate its item does.

Four more change what a host sees:

- **A record that was running at three deaths of the process is quarantined at open,**
  with a warning, instead of crash-looping the host. Like a dead-lettered record, it
  stays in the journal until `gptps_dq_drain_quarantine` hands it over.
- **Damage in a journal no longer ends the load.** A damaged record that is not a torn
  tail is skipped and the valid records after it are kept; the journal is first copied
  to `<journal>.corrupt`, and a warning goes to the log sink - stderr, unless
  `gptps_set_log_sink` redirects it.
- **On macOS, each durable submit is slower.** The fsync is now `F_FULLFSYNC`, which
  flushes the drive's own cache, where a plain fsync leaves the data for a power cut to
  take. To journal many items, `gptps_dq_submit_batch` pays one fsync for all of them.
- **The journal grows by a 24-byte marker for each attempt,** and another for each
  attempt that fails; a compaction drops them, keeping at most two for a record still
  pending. A journal written by 1.5 still opens in 1.4.0, which skips the markers and
  loses only the crash counts.

The sections below give the details.

### Added — `durable_queue`: batches, the handle a recovered record runs under, and fsync on macOS that reaches the disk

- **`gptps_dq_submit_batch`** journals n items with one fsync, then enqueues each.
  One at a time, durable work costs an fsync per item: 3,000 items took 2.8 s from one
  thread. As one batch they took 3 ms, and 32 ms in batches of 100. A batch is
  journaled whole or not at all, and each item's status says whether the engine took
  it.
- **`gptps_dq_set_resubmit_cb`** reports every record the queue hands back to the
  engine - each one `gptps_dq_recover` re-submits, and a suspect on its later turn -
  with its new handle. Events carry no payload and `gptps_dq_recover` returns a count,
  so a host keeping a ledger of journaled work (an invoice id in the payload, say) had
  no way to tell which handle a recovered record ran under after a restart.
- **On macOS, the queue's fsync now reaches the disk.** `fsync()` there leaves the data
  in the drive's own cache, where a power cut can still take it; the queue now uses
  `F_FULLFSYNC`, and plain `fsync` where a file system refuses it. It is slower, which
  is the price of what "durable" promises.

### Fixed — cancelling from a deep queue walked it

- **`gptps_cancel` is O(1) in queue depth.** It found its item by walking every queue,
  and a cancel from intake wiped the cache that keeps an ordered insert O(1), so the
  next submit walked the whole queue too. Cancelling 40,000 queued items newest-first
  took 3.7-4.5 s, and cancelling and resubmitting at that depth 4.5-5.5 s - each cancel
  holding the engine lock for its walk, stalling the dispatcher and every submitter.
  Now the engine keeps an index by handle and its queues are doubly linked: the same
  runs take 3 ms and 6 ms. `gptps_dq_cancel`, `gptps_orch`, `gptps_balance` and a host
  cancelling what is left at the end of a window all go through it.
  `tests/test_cancel_perf.c` gates the shape of the curve, from intake and from
  backoff, and `tests/test_cancel.c` now cancels after every way an item can end.
  The index and the second link cost 48-80 bytes per queued item at 64-bit. The index
  shrinks again as the queues drain, so a burst does not keep its memory until
  `gptps_shutdown`; `tests/test_alloc.c` counts the bytes.

### Fixed — `durable_queue`: a stalled engine, a damaged journal, a quadratic drain, and crash loops

- **Journaling work no longer stalls the engine.** `gptps_dq_submit` held the queue's
  lock across its fsync, a millisecond or more of disk, and the queue's observer needs
  that lock for the events of every task, journaled or not, on the engine's own worker
  and dispatcher threads. While one thread journaled work back-to-back, the engine all
  but stopped: plain tasks fell from ~420,000/s to 2-13/s, and of 344 queued 20 ms jobs
  only 24-30 finished during a 3,000-item submit loop that took 2.8 s. A host enqueuing
  a batch through the queue froze everything else until it was done. Now the fsync
  holds no lock the engine needs, and the same loop lets all 344 finish. Submits made at
  the same time share one fsync (group commit): 8 threads journal ~4,250 items/s where
  they managed ~1,070, which is what one thread still gets. `gptps_dq_cancel` waits for
  its fsync the same way. Nothing changes in what is durable when: a submit still
  returns only once its record is on disk, and one whose fsync fails returns
  `GPTPS_E_IO` and is not enqueued.
- **One damaged record no longer destroys every record after it.** Replay stopped at
  the first record that did not verify, as if it were the torn tail a crash leaves,
  and the compaction `gptps_dq_open` runs next rewrote the journal without everything
  after that point: fsync'd, acknowledged work, deleted without a word. One flipped
  bit in record 2 of 5 left 1 pending record of 5. Now damage that is not at the end
  of the file is skipped and every valid record after it kept; the original journal
  is first copied to `<journal>.corrupt`, and a warning goes to the log sink. A record
  torn at the end of the file is still dropped silently. A record whose header claims
  more bytes than the file holds looks exactly like a torn write, so anything after it
  is reported and preserved but not loaded. Running out of memory while loading now
  fails the open and leaves the journal alone; it used to compact away whatever had
  not been loaded.
- **Draining a recovered backlog is linear in its size** (it was quadratic). Each
  event searched the whole record table for its handle: 40,000 recovered records took
  0.54 s to drain where the engine alone took 0.004 s, and each doubling took about
  four times longer. An index by handle makes each event constant-time: the same
  backlog now drains in 0.07 s, including the new attempt markers below.
  `tests/test_durable_perf.c` gates the shape of the curve.
- **A task that kills its own process no longer crash-loops the host.** Quarantine
  waited for a `DEAD_LETTERED`, which a process killed by its own task never sends, so
  the record was recovered, and killed the process, on every restart. The journal now
  notes when each attempt starts and ends; a record that was running at three deaths of
  the process is quarantined at open, with a warning. At two it becomes a suspect, and
  suspects are recovered one at a time: recovery replays records in the same order
  every time, so the records running beside a crashing task are there at each of its
  deaths too, and counting alone would quarantine them with it. Any attempt that ends,
  however it ends, clears the count, so a clean shutdown counts as no death at all.
  `gptps_dq_recover` therefore returns at most one suspect in its count; the others
  follow as each turn ends. The new markers are record types an older reader skips:
  a journal written now still opens in 1.4.0, which loses only the crash counts.

### Documentation

- **`durable_queue`: compact when the engine is quiet.** A compaction holds the queue
  while it rewrites the journal and fsyncs it, and the engine's threads wait at their
  next event until it is done. `gptps_dq_compact`'s header now says so.
- **`examples/item_ledger.c`: the ledger a host keeps while it re-drives dead letters.**
  One row per business id, found by the item's current handle, since events carry no
  payload. The observer and the dead-letter drain close a row through the same function:
  whichever arrives first closes it, and the other only confirms. The two can come in
  either order, because the drain can hand over an item before its `DEAD_LETTERED` is
  delivered. A re-drive re-submits the business id unchanged after the outage that
  failed it has cleared, moves the row to the new handle, and is bounded per row. So an
  item with bad input is re-driven once, fails again, and the next drain leaves it dead.
  A late event for the superseded handle finds no row and changes nothing. The example
  runs in MANUAL mode, is registered as a test, and fails if any of that breaks. Written
  for the questions in #12; contributed by @nightops00dev in #14.

## [1.4.0] - 2026-09-30

### Upgrading from 1.3

Three changes can need a change in your code:

- **`gptps_open_ex` reads `cfg->config_path` at open,** its `addons` included. A path
  that does not exist yet, or does not parse, now fails the open with
  `GPTPS_E_CONFIG`. If you set it only as a save path, create the file first, or leave
  it NULL and pass the path to `gptps_settings_save` and `_reload` yourself.
- **A task body that returns `GPTPS_E_CANCELLED` is no longer retried.** To ask for a
  retry, return `GPTPS_E_TASK` or your own failure code.
- **A HAL supplied through `GPTPS_HAL_SOURCE` must implement
  `gptps_hal_load_acquire_u32` and `gptps_hal_store_release_u32`,** or the link fails.
  Plain accesses suffice single-threaded, as in `freestanding/hal_stub.c`. The bundled
  POSIX and Windows HALs have them.

Three more change what a running host sees:

- **Past `limits.shutdown_grace_ms`, work still queued is never started.** It ends
  `DEAD_LETTERED`, or `DROPPED` under `on_failure = drop`, with `GPTPS_E_SHUTDOWN` and
  `GPTPS_EV_FLAG_SHUTDOWN`, as work in backoff already did. 1.3.0 still started it
  after the deadline, only to cancel it, so the grace did not bound shutdown.
- **A live write of `limits.max_concurrent_tasks` no longer changes the running
  engine.** It was documented restart-only, and now it is: the value takes effect at
  the next open. To throttle a running engine, re-budget a named resource.
- **`gptps_shutdown` and `gptps_step` return `GPTPS_E_BUSY` from a callback on the
  host's own thread,** such as the dead-letter drain callback or a settings watcher.
  The header always said they refuse callbacks; that held only on the engine's own
  threads.

The sections below give the details.

### Added — the event says who ended an item (ABI 2.3)

- **`gptps_event.flags`, with `GPTPS_EV_FLAG_SHUTDOWN` and
  `GPTPS_EV_FLAG_SELF_CANCELLED`.** Two statuses could not say who ended an item.
  `GPTPS_E_SHUTDOWN` sits on the `DEAD_LETTERED` / `DROPPED` teardown imposes (the grace
  expiring on work still queued or in backoff, the drain refusing a requeue item another
  cycle), but a task body may return it too. `GPTPS_E_CANCELLED` sits on every cancel,
  whether a `gptps_cancel`, a removal or a shutdown stopped the item or its body
  returned it (see Changed, below). The flags say which: `GPTPS_EV_FLAG_SHUTDOWN` on
  teardown's dispositions, `GPTPS_EV_FLAG_SELF_CANCELLED` on a `FAILED` whose
  `GPTPS_E_CANCELLED` the body returned itself. Statuses are unchanged. The field is
  appended; read it only when `struct_size` covers it, since an engine older than 2.3
  hands you a shorter struct (`docs/PLUGINS.md` shows the guard). `gptps_balance` sets
  `GPTPS_EV_FLAG_SHUTDOWN` on its own teardown dispositions too.
  `durable_queue` now decides by the flags. The one case 1.3.0 quarantined by design —
  a requeue item whose body had returned `GPTPS_E_SHUTDOWN` to end a cycle, then
  dead-lettered by teardown — stays pending and is recovered like every other kind of
  work teardown abandons. The per-record inference from the attempt's own `FAILED`
  that the flag replaces is gone.

### Changed — a task body that returns `GPTPS_E_CANCELLED` ends its item

- **It is final: no retry, no dead letter, no requeue.** Its attempt's `FAILED` carries
  `GPTPS_E_CANCELLED`, which observers reconciling handles count as terminal, yet the
  engine retried it. The retries reopened a handle observers had closed: more
  `STARTED`s after the terminal event, `gptps_stats` ending with `pending` 1, and a
  `gptps_await` that had already returned. Now the item ends as a cancel would, and an
  always-up service whose body returns it ends too; the `FAILED` carries
  `GPTPS_EV_FLAG_SELF_CANCELLED`.
  `durable_queue` closes such a record, since the body ended its own work. Kept
  pending, it would re-run on every restart; a cancel from outside still leaves the
  record pending. **A body that returned `GPTPS_E_CANCELLED` to ask for a retry must
  return `GPTPS_E_TASK` (or its own failure code) instead.** `tests/test_reconcile.c`
  pins it: with two retries and no backoff, three attempts and a dead letter before,
  one attempt now. `tests/test_retry_order.c`'s case for that return now expects no
  `RETRIED`, and `tests/test_durable.c` checks the record closes and is never re-run.

### Fixed — shutdown past its grace, callbacks on the host's thread, and four more defects

Found while answering #12, and in review of the fixes; every one has a test that fails
without its fix.

- **A constraint that kept deferring an item hung `gptps_shutdown` forever.** The header
  promises such an item is ended by its policy once `limits.shutdown_grace_ms` expires,
  and the grace did end what it found in the backoff queue. But a dispatcher pass first
  moves every parked item that has fallen due back to intake, and only then applies the
  grace. Once the deadline had passed, the item's own re-check was the only wake left,
  so the item was due at every pass: never in the backoff queue when the grace looked,
  and deferred again by admission. Whether a shutdown hung depended on where a re-check
  fell against the deadline; with a short re-check interval it hung on every run, since
  at least 1.2.0. `tests/test_hang.c` case 8.
- **Past the grace, queued work was still admitted.** The grace cancelled what was
  running, and admission carried on. Each item still queued was started after the
  deadline, only to be cancelled on the next pass. An in-process body that never polls
  `gptps_is_cancelled()` instead ran to completion, one queued item after another, so
  the bound on shutdown was not a bound: four 800ms bodies behind one worker took 3.2s
  with a 200ms grace. Now work still queued when the grace expires is ended by its
  policy like work in backoff, which also closes the hang above: `DEAD_LETTERED`, or
  `DROPPED` under `on_failure = drop`, with `GPTPS_E_SHUTDOWN` and
  `GPTPS_EV_FLAG_SHUTDOWN`. It used to end as `FAILED` / `GPTPS_E_CANCELLED` after being
  started. Nothing more is admitted after that, however long the queue, and a scheduler
  hook is not run over what is left. `durable_queue` keeps both kinds of record pending,
  so recovery is unchanged. `tests/test_hang.c` cases 9 and 9b.
- **`gptps_shutdown` and `gptps_step` from a callback on the host's own thread were not
  refused.** The header says both return `GPTPS_E_BUSY` from a callback. That held on
  the engine's own threads and inside `gptps_step`, but some callbacks run on the
  host's thread: the `QUEUED` that `gptps_submit` emits, the `FAILED` that
  `gptps_cancel`, `gptps_unregister_task` and `gptps_shutdown` emit for what they
  cancel, the dead-letter drain callback, a settings watcher, a setting's write
  accessor during `gptps_settings_set` or `gptps_settings_reload`, an add-on's `setup`,
  `teardown` and `disable`, and an event an add-on emits. A `gptps_shutdown` from one
  of them freed the engine under the call still using it. From `gptps_shutdown`'s own
  events, it ran teardown inside teardown. They are refused now. The header's
  THREADING section lists which callbacks count, and now says what was always assumed:
  a callback must return normally, not longjmp or throw out. The engine keeps a record
  per such thread, found by a hash under the engine lock that all but
  `gptps_settings_set` and `gptps_settings_reload` already hold, and left with no lock.
  Measured at one to eight producers against the engine before these fixes,
  `gptps_submit` stays within about 4% of its old throughput with a counting observer
  and without one — the measurement's own run-to-run noise. A thread that escapes a
  callback anyway is refused `gptps_shutdown` and `gptps_step` from then on, rather
  than leaving the engine a dangling pointer. The records are bounded: past 1024
  threads inside such callbacks at once, or with no memory for one, a callback runs
  unguarded. New HAL entry points: `gptps_hal_load_acquire_u32` and
  `gptps_hal_store_release_u32`, which a HAL supplied through `GPTPS_HAL_SOURCE` must
  now implement (plain accesses suffice single-threaded, as in
  `freestanding/hal_stub.c`). `tests/test_hang.c` case 10, `tests/test_addon.c` and,
  built with a cap of 4, `tests/test_cb_cap.c`.
- **`gptps_set_task_resource_cost` took effect only at the next unrelated wake.** It
  now wakes the dispatcher, so queued items of the type are judged by the new cost at
  once. Before, an item held back only by its cost stayed queued after the cost was
  lowered, on an otherwise idle engine forever. `tests/test_budget_shrink.c`.
- **A service that crashed just before `gptps_shutdown` ended `DEAD_LETTERED`.** A run
  that has returned waits for the dispatcher's next pass. `gptps_shutdown` stopped
  services that were running, ready, queued or in restart backoff, but not one in that
  window. A crash caught there was judged like any failure while stopping, as a requeue
  the drain will not restart, and ended `DEAD_LETTERED` / `GPTPS_E_SHUTDOWN` instead of
  with the `FAILED` / `GPTPS_E_CANCELLED` the header promises a service's shutdown.
  `tests/test_service.c`.
- **`limits.max_concurrent_tasks` is restart-only, as it was documented.** The pool is
  sized once, at open, but a live `gptps_settings_set` still reached the limit admission
  reads. A lower value throttled the running engine. A higher one admitted more items
  than the pool has threads: they waited in the ready queue holding their memory and
  named-resource budget, and went ahead of anything submitted after them. A live write
  is now kept only for `gptps_settings_get` and `gptps_settings_save`, so a saved file
  carries it to the next open. To throttle a running engine, re-budget a named
  resource. `tests/test_settings.c`.
- **Equal scheduler-hook scores came out newest-first.** A new item enters the queue at
  its priority and was placed by it against items the hook had already scored. A hook
  returning scores below the default priority 0, such as `-deadline`, put every newcomer
  ahead of the equal-score items already waiting. The stable re-sort then kept it there,
  although the header says ties resolve FIFO. It happened whenever items waited across
  passes with a slot free, and dated from 1.1.0's ordered intake. Ties now break
  explicitly on the order items entered the queue.
  `tests/test_admission_order.c` gains the case its earlier hook case could not hit,
  since that one queued everything before the first pass.

### Changed — `gptps_open_ex` reads the config file it is given

- **`cfg->config_path` is read at open, exactly as `gptps_open(path)` reads it.** The
  header calls it the "optional TOML path", but `gptps_open_ex` kept it only as the
  default path for `gptps_settings_save` and `_reload`: none of the file's `[limits]`,
  `[scheduler]`, `[tasks.*]` or `addons` applied at open, yet a later
  `gptps_settings_reload` applied part of the same file. Now it all applies, and an
  explicit value in `cfg->limits` still wins over the file's, as the header says. **A
  host that set `config_path` only as a save path now has the file applied at open —
  its `addons = [...]` included, so the file is the same trust boundary `gptps_open`'s
  is (`docs/SECURITY.md`) — and a path that does not exist yet, or does not parse, now
  fails the open with `GPTPS_E_CONFIG`, where it used to open and wait for the first
  save.** Create the file first, or leave `config_path` NULL and pass the path to
  `gptps_settings_save` and `_reload` yourself. `gptps_pool` and `gptps_xport` open
  every shard or worker with `gptps_open_ex`, so each now reads a `config_path` it is
  given. A later `gptps_settings_reload` applies the file as `gptps_settings_set`
  would, so a key it sets overrides a `cfg->limits` value that won at open.
  `gptps_open(path)` is now `gptps_open_ex` with that path. `tests/test_toml.c`.

### Fixed — four smaller defects

- **In a live setting, as in a config file, 0 now means auto.** Reloading a file whose
  `[limits]` say `max_concurrent_tasks = 0`, as the shipped `gptps.example.toml` does,
  failed with `GPTPS_E_CONFIG`: the live setting refused 0. And a reloaded or set
  `max_memory_bytes = 0` became a budget of zero bytes, which dead-letters every queued
  item that declares memory. The pool size now keeps 0 (auto at the next open); the
  memory budget resolves it to ~0.75 of detected RAM, as at open. `tests/test_toml.c`.
- **`GPTPS_SUBMIT_TIMEOUT_MS` did nothing in MANUAL mode.** `gptps_step` computed an
  attempt's deadline from the task type's `timeout_seconds` only, while the threaded
  worker honoured the per-submit `timeout_ms`, so in MANUAL mode a body got the task
  type's deadline, or none, instead of the one its submit asked for. Both now share
  one rule.
  `tests/test_step.c`.
- **`gptps_orch_after` accepted a dependency handle of 0, and held that gate forever.**
  No submit returns 0, but a held gate's `*out` does, which makes it easy to pass on by
  mistake; `gptps_orch_pending` then never reached 0, although its doc said it always
  converges. It is refused with `GPTPS_E_INVAL` now, and the doc names the other ways
  a gate can wait forever (a handle this engine never issued, or one that finished
  before the gate and is no longer remembered). The function's doc also listed a plain
  failed attempt as terminal, which it is not. `tests/test_orch.c`.
- **The dashboard ignored `DROPPED`.** A dropped item (`on_failure = drop`) was in no
  count, missing from `ok%`, and logged as `?`. `gptps_tui` now counts it (`dropped N`,
  a `drop` column), counts it as a failure in `ok%`, and names it in the log.
  `tests/test_tui.c`.

### Fixed — `durable_queue`: recovering under backpressure

- **`gptps_dq_recover` may be called again, and the header says how.** It said "safe to
  call once after open". With `limits.max_intake_depth` set, a call re-submits what
  fits, and the engine refuses the rest with `GPTPS_E_FULL`. Those records stay
  pending, and a later call in the same run offers them again — the supported way to
  recover a backlog larger than the intake. A record whose type is not registered is
  refused every time, so the header also says when to stop: when a call re-submits
  nothing although the engine has drained. `tests/test_durable.c` recovers three
  records through an intake of two. Raised by @kuntakinte7270 in #10.

### Fixed — tests

- **`test_admission_perf` is steadier on shared machines and coarse clocks.** It now
  grows the drain until its first run takes at least 100ms — past 160000 items by
  repeating it, up to 32 times — then takes the best of three runs per size. Before, it
  stopped at 50ms or at 160000 items, whichever came first: about 30ms on a fast
  machine. A single timing on a shared machine could push a linear ratio past its
  bound: it failed once on the macOS runner for the 1.3.0 release commit (3.21 against
  3.0), and under parallel sanitizer builds. Under a ~16ms clock tick, as on Windows, a
  30ms interval is about two ticks, where quantization alone moves a linear ratio a long
  way toward the bound. A quadratic admission path is slow in every run, so the minimum
  still shows it.

### Documentation

- **The auto pool size is one worker per online logical CPU, not per core.** That is
  twice as many with SMT; `gptps.example.toml`, `gptps.h`, the Readme,
  `docs/SECURITY.md` and `docs/ARCHITECTURE.md` now say so. `docs/ARCHITECTURE.md`'s
  admission rule now includes named resources, the dashboard's docs its dropped count,
  and `gptps_tui.h` no longer gives a reason for closing after shutdown that stopped
  being true when observers became unregistrable.
- **Safety artifacts.** `docs/SAFETY.md` and a Readme section describe the planned GPTPS
  Safety Artifacts Package. Each package will hold the GPTPS-level evidence a product
  team typically needs when certifying a product built on one specific GPTPS release.
  It comes under a separate
  commercial license that never restricts GPTPS itself. GPTPS stays MIT, and no
  release is certified today.
- **The Readme's `earliest_deadline_first` was oldest-first.** It scored by
  `enqueue_ms`. The example now takes the deadline from the payload, and says what the
  hook can see: no deadline of its own.
- **Re-budgeting a named resource is live-safe, and the header now says so.** It called
  every `gptps_define_*` setup-time. Re-budgeting an existing resource has woken the
  dispatcher since 1.2.0, and `tests/test_budget_shrink.c` covers a cut under queued
  work. The header now names that exception. It also says what a cut does: it cancels
  nothing, and it dead-letters queued items that can no longer fit once an admission
  scan reaches them, so a budget of 0 is not a pause. `gptps_set_task_resource_cost`,
  which `gpu_quota`'s plug-in already calls at runtime from a settings watcher, is
  declared live-safe too.
- **`examples/success_gate.c`: a success-only dependency lives in the host.**
  `gptps_orch_after` releases its gate on any terminal outcome, a dead letter or a drop
  included, so it cannot express "run this only if that succeeded". The example shows
  the host-side rule, in MANUAL mode with the core API only. The host opens its gate on
  the prerequisite's own `FINISHED` and submits the dependent from its loop, outside the
  callback. It runs both branches and exits non-zero if the gate misbehaves. When the
  prerequisite succeeds on its retry, the dependent runs once. When it exhausts its
  retry, the dependent is never submitted, and independent work still completes. It is
  registered as a test, and `addons/README.md` points to it from orch's definition of
  terminal. Contributed by @kuntakinte7270 in #11.
- **`gptps.example.toml`'s `[tasks.report]` no longer requeues forever.** It paired a
  300 s timeout with `on_failure = "requeue"`, which is unbounded: once the retries run
  out, the item starts a new cycle at attempt 1 and never dead-letters, and its handle
  stays open until a cancel or shutdown closes it. Copied as is, the sample killed any
  report that ran past 5 minutes and then retried it all night. It now uses a 900 s
  timeout and the default `dead_letter` policy, so a report that keeps failing lands on
  the dead-letter list after its 3 retries, and a comment says why `requeue` is not a
  safe default. Fixed by @nightops00dev in #13.

## [1.3.0] - 2026-09-28

### Fixed — a retry could start before its `RETRIED` was delivered

- **A bounded retry's `RETRIED` now reaches every callback and observer before the
  retry can be admitted, even with `retry_backoff_seconds = 0`.** The dispatcher parked
  a retry in `delayed` in step 1 of its pass, and step 2 of the same pass promoted
  whatever was due — so a zero-backoff retry went straight back to `intake`, was
  admitted, and a worker could report its next attempt's `STARTED`, even its
  `FINISHED`, while the `RETRIED` announcing it was still waiting to be emitted after
  the pass. Counted through `gptps_stats` as it was before its late-`RETRIED` fix
  (under "Fixed — add-ons" below): with near-empty bodies almost every run of 20,000
  once-failing tasks hit it; with real work stats alone saw none, but a 20 µs observer
  registered ahead of it brought it back at 3–4 per 5,000 retries (200 µs bodies, 16
  workers). A retry
  now joins `delayed` only after that pass's promotion scan, and both pumps finish
  emitting before they run another pass, so it cannot be admitted until its `RETRIED`
  is out; one already due makes the next pass run at once, so `gptps_step` admits it
  in the same step it always did, and a MANUAL host whose callbacks do not submit or
  cancel in reaction to `RETRIED` sees the same event order as before.
  Only the retry waits, not the pass: every other slot the pass freed is refilled as
  before. So that lower-priority work cannot take the place the retry would have had,
  admission keeps back a slot and the retry's declared memory for each due retry from
  work it outranks; work at or above its priority goes first, as it was queued ahead
  of it anyway. MANUAL mode admits nothing the retry outranks until the next pass, so
  a priority-10 retry with three priority-0 tasks behind it still runs ahead of every
  one it can: `hi#1 hi#2 lo lo lo` on one slot, `hi#1 lo hi#2 lo lo` on two. Named
  resource budgets are not held. Measured (Release,
  medians of 7 interleaved runs, 8 slots, an observer that sleeps 5 ms on `RETRIED`):
  2,000 5 ms tasks, a quarter of them failing once, take 3,092 ms (3,117 ms before;
  holding the whole pass, as this change first did, took 4,105 ms, +32%); arriving
  one every 2 ms, the tasks that never fail start after a median 1.8 ms (1.7 ms
  before, 53 ms holding the pass). With no observer, 200,000 no-op tasks that all
  fail once on 4 slots take 599 ms against 582 ms, inside that row's −3% to +9%
  run-to-run spread. `tests/test_retry_order.c` holds the first callback on `RETRIED`
  until the next attempt has started (or finished) — against the old engine both
  THREADED scenarios fail on every run — and checks THREADED admission while it is
  delivered: equal- or higher-priority work runs, lower-priority work cannot take the
  retry's slot or its memory but can use room beside it. It also pins both orders,
  and runs 300 retries through the 256-event buffer in one step. Fixed by @kuntakinte7270 in #10, who also showed what the
  inversion cost `gptps_stats`.

- **A cancel can overtake a `RETRIED` — now also at zero backoff — and `gptps.h` names
  that inversion in place of the one above.** The retry waits in `delayed` while its
  `RETRIED` is delivered, and a `gptps_cancel` — or a `REMOVE_CANCEL` with nothing else
  of the type in flight — ends a parked handle at once, on the calling thread, ahead of
  the `RETRIED` for every observer still to see it. From a callback reacting to that
  `RETRIED` it happens every time, MANUAL mode included, and the EVENT ORDER block now
  says so. Because the retry is parked rather than admitted while its `RETRIED` is
  delivered, a callback that re-enters the engine at that moment sees four things
  change, in both modes: work it submits at or above the retry's priority now runs
  before the retry, which is not queued yet; a `gptps_cancel` of the retried handle at
  zero backoff now emits `FAILED`/`GPTPS_E_CANCELLED` at once, ahead of the `RETRIED`,
  where it used to follow it; a THREADED `REMOVE_CANCEL` of its own type now completes
  at zero backoff — there is nothing admitted to wait for — where it returned
  `GPTPS_E_BUSY`; and a MANUAL `REMOVE_CANCEL` no longer emits a second
  `FAILED`/`GPTPS_E_CANCELLED` for a handle whose body returned `GPTPS_E_CANCELLED`
  itself — the parked retry is freed without one, as it already was with a backoff.
  That last shape is still not clean: the `RETRIED` announcing attempt 2 follows the
  body's own terminal event and nothing closes it, so `gptps_stats` ends such a handle
  with `pending` at 1 at any backoff; the engine retrying an attempt whose body
  reported itself cancelled is the open question behind it. `tests/test_retry_order.c`
  counts terminal events itself for the MANUAL removal — a body that failed and one
  that returned `GPTPS_E_CANCELLED`, with and without backoff — and for the THREADED
  one.

### Fixed — routing a service through a balancer was a use-after-free

- **`gptps_balance` now refuses a `GPTPS_TASK_SERVICE` at submit (`GPTPS_E_INVAL`).**
  A service handle emits one terminal event per run, and the router took the first of
  them for the item finishing: it dropped the shard mapping, decremented that shard's
  load and freed the item while the instance was still on the shard and about to
  restart. The visible half was a permanent phantom free slot — measured, shard loads
  `[0,0]` with the instance still running. The dangerous half is that losing the item
  also loses the only handle `gptps_balance_close` had on it, so close freed the
  balancer while the instance was still running with the module's observer registered:
  ASan reports a heap-use-after-free in `observe()`, read on a worker thread. There is
  no bookkeeping fix for a lifetime the module was never told about, so it is refused
  at the boundary. `tests/test_balance.c` pins it — without the refusal that test does
  not merely fail, it crashes. Start services on the shards directly; a balancer is
  for work items. The check asks `gptps_task_flags` (below). As first written (never
  released) it walked `gptps_task_get_info` and compared against the returned `name`,
  which is borrowed only until the registry next changes, so an unregister on another
  thread could free it mid-compare; `tests/test_balance.c` reproduces that under ASan
  in every run of the old code.

- **ABI 2.2: `gptps_task_info` gained `flags`, and `gptps_task_flags` reads them by
  name.** Nothing could introspect whether a registered type was a
  `GPTPS_TASK_SERVICE` — a caller could see its executor, policy
  and counters but not the one property that decides whether its handle ends once or
  once per run, which is exactly what `gptps_balance` needed. Additive, and the loader
  compares ABI MAJOR only, so no add-on is refused. `gptps_task_get_info` now validates
  against a frozen 1.0.0 floor instead of `sizeof`, and writes the new field only when
  the caller's `struct_size` covers it — without that, appending would have started
  returning `GPTPS_E_INVAL` to every already-compiled caller, which is the trap
  `src/gptps_internal.h` exists to prevent. Verified: a caller passing the pre-2.2
  `struct_size` still gets `GPTPS_OK`. `gptps_task_flags(e, name, &flags)` answers the
  same question by name, copying the flags under the engine lock: safe while another
  thread unregisters a type, which matching a name by walking `gptps_task_get_info` is
  not, and without the scan of every item the engine holds that each call in that walk
  pays to fill its counters.

### Fixed — a handle that closed in silence, and the guarantee that oversold itself

- **A `GPTPS_ON_FAILURE_REQUEUE` item reached shutdown without a terminal event.** The
  drain refuses it a new cycle — an always-failing requeue would hang shutdown forever —
  and dead-letters it instead, but it never emitted the `DEAD_LETTERED` that says so. It
  was one shape that could reach shutdown and close in silence (the parked-item entry
  below closes the others): `gptps_await` on such a handle never returned, and every
  add-on that reconciles terminal events leaked a slot per item. Measured before: 8 of 8
  handles ended with zero terminal events after `gptps_shutdown` returned; after: 8 of 8
  close with `DEAD_LETTERED`. `tests/test_reconcile.c` now pins it — the file the Readme
  cites as proof of the invariant previously had no case for this path, and the new one
  fails without the fix. The event carries `GPTPS_E_SHUTDOWN`, as the grace expiry's do,
  and so does the retained dead letter `gptps_dead_letter_drain` returns, where 1.2.x
  kept the attempt's status, which its `FAILED` still reports. What ends the item is
  teardown refusing another cycle, and an observer that keeps work for the next run
  (see `durable_queue` below) has to be able to tell that from the task failing.

- **An item parked between attempts could be cancelled, removed or torn down without a
  terminal event.** The one path that ends queued or parked work without running it —
  `REMOVE_CANCEL`, the stop of a service at shutdown, and a MANUAL host's teardown —
  freed an item in silence whenever its `started` flag was set, reading it as
  "`execute()` already reported this handle closed". But `started` records that an
  attempt ran, not how it ended, and an item waiting in backoff still carries it from
  the attempt before. So a bounded retry, a `REQUEUE` item or a restarting service
  parked there vanished with only a per-attempt `FAILED` (or, for a service, one run's
  `FINISHED`) to its name, and `gptps_await` on it never returned; the same happened to
  an attempt that had merely failed and was awaiting its retry decision when a MANUAL
  removal took it. That path now asks the real question — did this attempt run *and* end
  in a terminal event: a `FAILED` stamped `GPTPS_E_CANCELLED`, or a `FINISHED` for
  anything but an always-up service, whose instance `gptps.h` says is closed by the
  `FAILED`/`GPTPS_E_CANCELLED` its stop produces — and emits `FAILED`/`GPTPS_E_CANCELLED`
  otherwise. So a `REQUEUE` item taken by `REMOVE_CANCEL` or a MANUAL teardown now
  closes with that event; a THREADED shutdown still dead-letters it. `CONTRIBUTING.md`'s
  rule now spells out both halves, and eight new cases in `tests/test_reconcile.c` cover
  each path: six ended with a terminal event missing before the fix, and two pin that
  an attempt already cancelled, or already finished, is not reported twice.

- **A stop that landed as an attempt ended closed the handle in silence.** A
  `gptps_cancel` or `GPTPS_REMOVE_CANCEL` can reach an item whose attempt has already
  reported its own outcome - after an in-process body returned, or an external child
  exited, but before the dispatcher has accounted for the item. That outcome was a
  per-attempt `FAILED` for a failing one-shot, or one run's `FINISHED` for an always-up
  service, and neither is terminal. The accounting then freed the item with no
  terminal event - for the failed attempt it read `started` alone as "closed", and for
  the service's run it did not check at all - so there was no retry, no dead letter,
  and nothing for `gptps_await` or any observer that reconciles. A service shut down
  while a finished run waited in `done` went the same way. The entry above fixed the
  same test in the path that ends work without running it; the three places in this
  one now use it too, so such a handle ends with `FAILED` / `GPTPS_E_CANCELLED` after
  the attempt's own events. The window is narrow but ordinary: a cancel from the
  attempt's own `FAILED` callback hits it every time, and a stress harness cancelling
  through `gptps_dq_cancel` hit it in every run that cancelled failing work.
  `include/gptps.h`'s `gptps_cancel` now says what a late cancel does.
  Four new cases in `tests/test_reconcile.c` — a cancel from the attempt's own `FAILED`
  callback (MANUAL with and without a retry left, and THREADED), a `REMOVE_CANCEL` while
  that `FAILED` is being delivered, and a service cancelled from its first run's
  `FINISHED` — all ended with no terminal event before, and each of the three places
  fails its own case when reverted.

- **"Every submitted handle reaches exactly one terminal event" was stated
  unconditionally in eleven places, and is false for two opt-in shapes.** A REQUEUE item
  stays open for as long as its body keeps failing — that is the policy working as
  designed, and shutdown now closes it. A `GPTPS_TASK_SERVICE` handle is a supervised
  *lifetime*, not a completion: under the default always-up policy every clean exit emits
  a `FINISHED` before the restart, so one handle up for 1.5 seconds emitted 15 of them.
  `GPTPS_TASK_RETIRE_ON_OK` is the one service shape that emits exactly one. `Readme.md`,
  `CONTRIBUTING.md`, `include/gptps.h`'s SERVICE block, `tests/test_reconcile.c`,
  `src/engine.c` and the `orch` / `await` / `stats` / `balance` add-on docs now state the
  real contract and name both exceptions. Two of those sites were wrong in the opposite
  direction — `gptps_orch.h` and `addons/README.md` claimed a service NEVER terminates,
  when it terminates too often, which is the more dangerous error for a dependency gate.
  `gptps_balance` turns out to enforce the guarantee itself rather than inherit it (it
  drops its handle mapping on the first terminal event), which also means it stops
  counting a service against its shard's load while the instance is still up.

### Fixed — the observer contract, and what it cost two add-ons

- **`include/gptps.h` never said event order is not guaranteed.** `QUEUED` is emitted by
  the submitting thread after the engine lock is dropped, and the dispatcher was signalled
  while it was still held, so a task that runs in under a microsecond reports `STARTED` —
  or `FINISHED` — before its own `QUEUED` callback. Measured on `gptps_demo` through a
  terminal: 73 inversions per 1,000 items; pinned to one CPU, whole runs invert. A second
  inversion was found and documented too: with `retry_backoff_seconds = 0` the next
  attempt's `STARTED` could precede the `RETRIED` that announced it, on the dispatcher's
  own thread — fixed since, by "a retry could start before its `RETRIED` was delivered"
  above. The EVENTS block now states the `QUEUED` inversion and the one that remains on
  the retry side (a cancel that overtakes a `RETRIED`), says what *is* ordered, and
  points at `gptps_stats` as the worked example. The `QUEUED` emit order itself is
  deliberately unchanged —
  moving the dispatcher signal only halves the window (73 → 52 per 1,000, measured), and
  the one reorder that closes it is the under-lock emit that was removed for stalling all
  admission behind a slow observer.

- **`gptps_tui` and `gptps_stats` both dropped the queue-wait sample on that inversion.**
  A terminal event that outran its `QUEUED` found nothing to resolve against, so its
  latency was silently discarded — and never neutrally: those are by construction the
  items that waited *least*, so the reported average was pulled **up**. Both add-ons now
  recover the sample with a tombstone the late `QUEUED` clears. Measured on a 5,000-item
  instant burst at `latency_window` 65536: 10–13% of samples lost before, 0% after;
  `gptps_stats` now reports `wait_samples == started` exactly. `tests/test_stats.c` had
  the loss written into it as a tolerated range and now asserts the exact count, because a
  range cannot fail when the fix regresses.

### Fixed — an unregister that waited for itself

- **`gptps_unregister_task` called from a task body or an event callback no longer
  hangs the engine.** In THREADED mode a removal marks the type and then waits for its
  live work to drain. That drain needs the engine's own threads: every completion is
  accounted by a dispatcher pass, and every admitted item runs on a worker. Called from
  the dispatcher — a `RETRIED`, `DEAD_LETTERED` or `DROPPED` callback, the natural place
  for a circuit breaker — it waited for a pass it was itself holding up whenever any
  item of the type was still live: queued, waiting out a retry backoff, ready, running
  or awaiting accounting. That includes the commonest shape of all, a `RETRIED` callback
  draining its own type with nothing else in flight, because the item just retried is
  live. Called from a worker — a task body, or the `STARTED` / `FINISHED` / `FAILED`
  callback it emits — it waited for the very item that worker was running whenever
  that item was of the type being removed. Neither returned, and the engine stopped
  with them; the header only ever named `gptps_shutdown` and `gptps_step` as calls to
  keep out of callbacks. A removal that would have to wait now returns `GPTPS_E_BUSY`
  when called from one of the engine's own threads, before anything is changed — the
  refusal `gptps_shutdown` already gives, and the one MANUAL mode already gave for a
  running instance. A removal with nothing to wait for (an idle type, or a CANCEL of
  work that is only queued) still completes there.
  **This refuses some calls that used to succeed.** A task body that removed a
  *different* type whose work ran on other workers used to wait and return
  `GPTPS_OK`; it now gets `GPTPS_E_BUSY` and the type stays registered. The rule is that
  an engine thread never waits on the engine — two workers each removing the other's
  type would wait on each other, and nothing could tell that call from the safe one.
  Make such removals from a thread of your own, and check the result: a caller that
  ignores it now keeps the type. `tests/test_unregister_reentry.c` puts the type's work
  in each place a drain waits on, from the dispatcher and from a worker; before the fix
  it did not fail, it hung, and each of six mutants of the new check makes it fail or
  hang again.

### Fixed — settings

- **An engine-owned setting could accept a value and store a different one.** Every
  engine-owned value — a `gptps_define_global` or `gptps_define_task_setting` knob, of any
  type — lives in one fixed `GPTPS_SETTINGS_VALUE_MAX` cell, but only STRING values were
  length-checked. An INT, UINT, DOUBLE or ENUM value too long for the cell parsed,
  validated, and was then cut short on store: 255 zeros followed by `1` was accepted and
  kept without its last digit. And a config reload copied STRING/ENUM text into a
  256-byte buffer *before* validating it, so an over-long string passed as its 255-byte
  prefix, and an invalid enum became a valid choice whenever that prefix happened to
  match one. Over-long input is now rejected with `GPTPS_E_CONFIG` before it is stored
  (per key: a reload still applies its other keys), a rejected update leaves the current
  value in place, and reload validates the parser's original text.
  `tests/test_settings_length.c` covers INT, UINT, DOUBLE and ENUM defaults and updates,
  and STRING and ENUM reloads, at the 255/256-byte boundary; 48 of its assertions fail
  against the code before the fix. Found and fixed by @kuntakinte7270 in #7.
- **The string getters' buffer contract is written down.** `gptps_task_setting_str` and
  `gptps_settings_get` return `GPTPS_OK` whether or not the value fit the caller's buffer.
  One of `GPTPS_SETTINGS_VALUE_MAX` bytes can never truncate, and the header now says so;
  before, neither getter mentioned the cap, buffers or truncation. A smaller buffer still
  truncates and still returns `GPTPS_OK` — documented, not changed. Reported by
  @kuntakinte7270, who added the boundary tests that hold both getters to it in #6.
- **Unregistering a task type took a sibling's settings.** Removal deleted every setting
  under `tasks.<name>.`, and a type named `<name>.<more>` keeps its keys under the same
  prefix, so unregistering `resize` left a live `resize.big` with none of its per-task
  settings. A key's shape cannot settle it either: a namespaced add-on leaf such as
  `gpuq.units` also adds a dot. Each per-task setting now records the type that owns it,
  and removal is by owner; a key a host registered under the prefix is still removed
  with its type, unless it lies under a live sibling's own prefix. Every release since
  1.0.0 had the bug. `tests/test_taskmgmt.c` removes `x` next to a live `x.y` and checks
  that the sibling's built-in, defined and host-registered settings survive; before the
  fix all were gone. A `gptps_define_task_setting` that had already taken its list of
  types when the removal began could still add a setting to the departing type after
  its settings were torn down, bound to memory the removal then freed; the owned
  settings are now swept once more after in-flight defines finish.

### Fixed — add-ons

- **`gptps_balance`: submit and cancel read an item another thread could free.** Once
  the router's lock is released, a queued item is reachable from its heap: a shard
  worker finishing earlier work pops it, submits it, and frees it when it ends — or at
  once, if it was cancelled. `gptps_balance_submit_ex` still read the item after
  unlocking, for the caller's handle and the `QUEUED` event, and `gptps_balance_cancel`
  read a still-queued item for its `FAILED` / `GPTPS_E_CANCELLED` event. Both were
  heap-use-after-frees on the hot path, needing nothing more than concurrent submits or
  cancels against work that completes; both shipped in 1.2.0 and 1.2.1. They now take
  what they need under the lock. When the late `QUEUED` arrives is unchanged — as in
  1.2.x, and as the engine's own off-lock `QUEUED` may, it can follow the item's
  `STARTED` or even its `FINISHED` — but it now carries the real handle and name.
  An out-of-memory path in dispatch could also deadlock: it cancelled an item it had
  failed to track while holding the router's lock, and the cancel's own event re-entered
  that lock; it now cancels after unlocking. `tests/test_balance.c`
  submits and cancels top-priority items while the shard keeps dispatching; under ASan
  the old cancel path failed in most runs and the old submit path in some.

- **`gptps_xport`: a blocking submit could return `GPTPS_E_IO` for a retired worker
  while `gptps_xport_live()` still counted it.** The reader published the failure —
  `done` set, waiters woken — and only then took the worker out of the rotation, so a
  submitter woken in that gap could return and read the old count; a concurrent submit
  that found the link `dead` got the same stale answer. `fail_all()` now retires the
  worker inside the `pmu` critical section that publishes the failure. That nests
  `cursor_lock` under `pmu` for the first time, which is safe because nothing is ever
  acquired while `cursor_lock` is held. The `gptps_xport_live(xd) == 3` check in
  `tests/test_xport.c` caught it intermittently under load: 0.4-1.9% of runs with the
  test pinned to two CPUs, every run with a sleep forced into the gap, and none of
  either with the fix. Found and fixed by @kuntakinte7270 in #9.

- **`gptps_xport`: an async submit whose frame write failed as its worker died could be
  freed twice.** Since engine mode (1.2.0), `submit_async` registered its record, then
  wrote the frame. A worker dying under that write woke two threads at once: the
  submitter, whose `send()` failed, and the reader, which saw EOF and ran `fail_all()` —
  unlinking the record, reporting `GPTPS_E_IO` through the callback and freeing it. When
  the reader got there first, the submitter read the record's `done` flag anyway,
  returned `GPTPS_E_IO`, and `submit_async` freed it again: one failure reported twice,
  and a use-after-free plus a double free in whichever order the two threads ran. A
  Release build aborted on heap corruption; ASan reported the heap-use-after-free in
  `send_request` or in `fail_all`'s callback loop. The frame header was also written
  from the record itself, one more read of memory the reader may already have freed.
  The submitter now copies what it needs before registering and settles ownership by
  id: a record it takes back fails the submit with `GPTPS_E_IO`; one the reader
  claimed is reported by the callback alone, so `submit_async` returns `GPTPS_OK`.
  `gptps_xport.h` now states the contract that was always implied — exactly one
  outcome per call — and that a `GPTPS_E_IO` return means the request never reached a
  worker. `tests/test_xport.c` races the two threads at least 40 times per run; against
  the old code every run failed.

- **`gptps_stats`: a `RETRIED` that arrived late left the gauges stuck.** The RETRIED arm
  set a handle back to `PENDING` whatever state it was in. But `RETRIED` comes from the
  dispatcher, and the attempt it announces could start, finish, fail — or be cancelled —
  before it arrived: a zero-backoff retry re-admitted in the same pass (the engine no
  longer does that; see the `RETRIED` entry above), or a `gptps_cancel` landing while
  the `RETRIED` was still being delivered, which still happens. The late event
  then reopened a running handle as pending (`pending` and `in_flight` both stuck at 1,
  its run sample lost), or, after the handle's terminal event, a fresh slot that never
  closed. Measured on the old code: 20,000 near-empty tasks that each fail once left
  `pending` anywhere up to several hundred after shutdown, on 8 workers, in almost every
  run. Each slot now tracks the attempts it has seen run and the attempts a `RETRIED` has
  announced: a `RETRIED` for an attempt that already started or ended moves nothing, and a
  handle that ends before the `RETRIED` owed to it waits for it as a tombstone — the
  shape the late `QUEUED` already had. The wait sample such an attempt carried off is
  taken from the late `RETRIED`, a `QUEUED` that outlives a whole attempt no longer
  loses attempt 1's, and an event that arrives in order but stamped earlier than the
  one before it (two threads' clocks) gives a wait of 0 instead of none.
  `addons/README.md` called stats order-independent; now it is. `tests/test_stats_order.c`
  feeds the observer every order 15 handle lifecycles can arrive in — 372 in all, 273 of
  which broke the old code — and `tests/test_stats.c` reproduces the cancel case on a
  real engine: the old code ended with `pending == 1`.

- **`gptps_xport`: a blocking submit from a reply callback waited for itself.** A reply
  callback runs on its link's reader thread, and only that thread completes the link's
  requests. `gptps_xport.h` said the callback "may call `gptps_xport_submit_async` (or
  `_submit`)", but a blocking `_submit` that round-robins onto the callback's own link —
  every time, with one worker — waited for a reply only the waiting thread could
  deliver, and never returned; `gptps_xport_close` then hung joining it. Two links whose
  callbacks submit to each other could wait on each other the same way. From one of that
  transport's live readers the blocking submit now returns `GPTPS_E_BUSY` before sending
  anything. **This refuses some calls that used to succeed:** on a pool of more than one
  worker, a blocking submit from a callback that landed on another live link used to
  return its reply. An `io == GPTPS_E_IO` callback is exempt — its link is already dead,
  so nothing can be waiting on it, and a synchronous retry from there still works.
  `submit_async` is allowed from a callback, and the header now says what it costs: it
  writes the request on the reader thread, so a request larger than the socket buffer
  can block while that link's worker is itself blocked sending a reply only this thread
  would read. `tests/test_xport.c` makes both calls from a callback on a one-worker pool,
  and retries synchronously from an E_IO callback; before the fix it hung.

### Fixed — `durable_queue`: what a cancel or a shutdown leaves in the journal

- **`gptps_dq_cancel()` retracts a durable submit; `gptps_cancel` still does not.**
  `gptps_cancel` on a handle from `gptps_dq_submit` stopped that execution and left its
  journal record pending, so the next process's `gptps_dq_recover` ran the work anyway,
  and `gptps_durable_queue.h` never mentioned cancellation. That behaviour is kept, and
  now documented: the engine reports an operator's cancel with the same `FAILED` /
  `GPTPS_E_CANCELLED` as the running and unstepped work a teardown cancels, so an
  observer that closed the record on it would also discard that work.
  `gptps_dq_cancel(dq, handle)` is the explicit form. It fsyncs a done marker for the
  record before telling the engine, so the retraction survives a crash, then cancels the
  execution. It returns `GPTPS_OK` once the record is retracted, including for work an
  earlier `gptps_cancel` already stopped; `GPTPS_E_SHUTDOWN` when the engine is tearing
  down, which retracts the record but cannot stop the execution; `GPTPS_E_IO`, leaving
  the record open and the execution untouched, if the marker cannot be made durable; and
  `GPTPS_E_NOTFOUND` when the queue holds no open record for the handle. Raised by
  @kuntakinte7270 in #10, with a MANUAL-mode reproduction under Windows GCC and MSVC.

- **Work that shutdown gave up on was quarantined, or lost.** When the grace expires the
  engine ends everything still waiting in backoff — a retry, an item between requeue
  cycles, one a constraint deferred — by its policy, with status `GPTPS_E_SHUTDOWN`:
  `DEAD_LETTERED`, or `DROPPED` under `on_failure = drop`. The queue's observer took the
  first for poison and quarantined it, which `gptps_dq_recover` skips, and the second
  for done, so the work was gone. Both now stay pending, like the running work the grace
  cancels, and the next run recovers them. So does a requeue item the drain refuses,
  whose new `DEAD_LETTERED` (above) carries the same status. The engine does not reserve
  that status, though: a body can return it too, forwarding a remote worker's shutdown
  for instance. Under `dead_letter` or `drop` that is the task's own verdict, and kept
  pending it would re-run on every restart; the observer tells it apart by the attempt's
  own `FAILED`, which precedes the terminal event and carries the body's status. That
  test cannot see a policy, so a requeue item that teardown dead-letters is quarantined
  if the latest attempt to return `GPTPS_E_SHUTDOWN` ended a cycle rather than being
  retried: retained, not lost. `gptps_shutdown`'s contract in `include/gptps.h` now
  states the engine's side of this, including that the status is not reserved.

- The header also claimed `on_failure = drop` failures are unobservable and stay in the
  journal. That was true through 0.2.0, whose engine emitted no event for a drop, but
  `GPTPS_EV_DROPPED` and the observer's handling of it both landed before 1.0.0, so the
  claim was wrong in every 1.x release. It now also says that a `GPTPS_TASK_SERVICE`
  without `GPTPS_TASK_RETIRE_ON_OK` whose `run()` returns `GPTPS_OK` is not a fit: the
  `FINISHED` of its first clean exit closes the record while the service keeps
  restarting. `tests/test_durable.c` covers:
  - both kinds of cancel, a retraction surviving a reopen, the `GPTPS_E_IO` path (POSIX)
    and a `gptps_dq_cancel` made during teardown;
  - both policies at grace expiry, and a requeue refused by the drain: kept pending when
    its body failed with `GPTPS_E_TASK`, quarantined when it returned
    `GPTPS_E_SHUTDOWN`;
  - a body that returns `GPTPS_E_SHUTDOWN`, on a live engine and with its retry parked
    at teardown;
  - a MANUAL engine shut down without ever being stepped.

  Against the old observer the grace cases fail on both policies, and so does the
  requeue case whose body fails with `GPTPS_E_TASK`; against the old engine that
  requeue case fails too.

- **`gptps_dq_submit` could return `GPTPS_E_NOMEM` for work it had already made
  durable.** The in-memory record table grew after the `'P'` record was fsync'd, so when
  that allocation failed the caller was told the work was not submitted while the
  journal held it, and - unless a compaction ran first - the next run's
  `gptps_dq_recover` ran it anyway. The table now
  grows first, so a failed allocation writes nothing. `tests/test_durable_oom.c`
  compiles the add-on with `realloc` redirected to fail once; before the fix the
  refused submit was recovered and ran in the next run.

### Added — a tier that costs nothing

- **`GPTPS_TUI_KPI_OFF`.** `MINIMAL` was documented as "~no per-event work", but it still
  takes one global mutex per event, on every worker, the dispatcher and every submitting
  thread. `OFF` returns before that lock and frees both rings, so an event costs one
  predictable branch — for leaving the dashboard installed after you have stopped looking
  at it, with the handle, settings and per-task labels all still valid. Counters do not
  advance while off. The KPI enum is renumbered to keep it a monotonic scale (`OFF` = 1);
  every in-tree reference is symbolic, and `tui.kpi` takes `"off"` as a fourth choice. The
  `m` hotkey cycles `MINIMAL`→`NORMAL`→`FULL` and never *into* `OFF`.

- **`gptps_tui_set_latency_window()` / `tui.latency_window`.** The latency ring was
  install-time only. It is the lever for the distortion the tombstone fix does *not* cure:
  an entry lives from `QUEUED` to `FINISHED`, so a burst deeper than the window overwrites
  live entries before they resolve, with the same upward skew — measured at +44% on a
  workload backing up past the default 1024. Sizing that is the caller's call, so it is
  now a runtime knob rather than a number fixed at install.

## [1.2.1] - 2026-09-24

A release-metadata correction. No API, ABI or behaviour change to the core; ABI stays 2.1.

### Fixed — release metadata

- **Release versions could drift between build metadata and the public API.** CMake,
  numeric/string macros, release tags and changelog sections are now checked together.
  v1.2.0 shipped a tree whose numeric macros still read 1.0.0 while `project(VERSION)` and
  `GPTPS_VERSION_STRING` read 1.1.0, because the configure guard compared only the string -
  so anything reading `GPTPS_VERSION_MAJOR/MINOR/PATCH` got a two-release-old answer from a
  current library. Configure now compares all three; `tests/test_version.c` compares them
  again from the compiled side, including `gptps_version()`, which CMake cannot see; the
  amalgamation job runs that same test against the GENERATED header, since the drop-in is
  the form most people consume and CMake never runs for it; and the release workflow refuses
  a tag that disagrees with CMake, either macro set, or this file. Found and fixed by
  @kuntakinte7270 in #3.

### Fixed — add-ons

- **`gptps_tui` showed 0% for a task whose every item had succeeded.** The TASKS table
  computed `ok%` as `finished * 100u / terminal` in 32-bit arithmetic, so a task crossing
  42,949,673 finished items - about twelve hours at a thousand a second - wrapped past 2^32
  and flipped the column from 100 to 0, then stayed wrong for the life of the process. The
  multiply is now 64-bit and the quotient clamped to 0..100, which also makes the width
  invariant the `%3u` and the `%5s` column both assume true by construction instead of by
  hope: GCC 16's `-Wformat-truncation` was right to refuse it, and refusing it broke
  `-Werror` builds on that compiler. `addons/gptps_tui.c`.

## [1.2.0] - 2026-09-15

### Added — scaling by composition, made real

- **`gptps_balance`.** A late-binding router above `gptps_pool`: work waits in one
  priority queue here and each shard is handed only what it can run plus a bounded
  `shard_depth`; a terminal event on any shard (observer seam) dispatches the next
  item to the least-loaded shard. Join-shortest-queue, work-stealing in effect,
  adaptive to any task size. Events are forwarded with balance handles and every
  handle reaches exactly one terminal event. Measured (`examples/bench_balance.c`,
  4 shards, heavy tail): 27–31% shorter makespan for 200–1,000-item batches; no
  difference on a 20,000-item stream, where round-robin is already balanced. It is a
  batch-and-burst tool, not a throughput tool. No core change. `tests/test_balance.c`.

- **`gptps_xport` engine mode.** Every worker process now runs its own GPTPS engine:
  `gptps_xport_open_ex` takes an `engine_cfg`, a task table and an optional `child_init`
  hook, and the worker's pool, budgets, retries, timeouts, dead-letter and seams all
  apply per worker process. The reply carries the item's terminal status. The link is
  multiplexed (request ids, a reader thread per link, `max_in_flight` per worker with
  `GPTPS_E_FULL` backpressure), `gptps_xport_submit_async` delivers replies on a
  callback, `gptps_xport_in_flight` reports outstanding requests, and `gptps_xport_close`
  is a graceful drain. `gptps_xport_open(n, handler, ud)` and every existing signature
  are unchanged; `tests/test_xport.c` passes untouched. `tests/test_xport_engine.c`
  covers concurrency over one link, in-worker retries, timeout → dead-letter, unknown
  task, backpressure, `child_init`, link death and graceful close.
- **`gptps_stats`.** The observer-seam aggregation the non-goals table promised:
  totals, live gauges (pending, in flight) and latency (queue wait, run time) per engine
  and per task type, order-independent across the core's threads, `gptps_stats_merge`
  for folding `gptps_pool` shards. No wire format. `tests/test_stats.c`.

### Fixed — liveness

- **Runtime budget shrink stranded queued work and hung `gptps_shutdown`.** The
  never-fits check (`GPTPS_E_BUDGET`) ran only at submit. Lowering
  `limits.max_memory_bytes` or re-budgeting a named resource below the declared cost
  of an already-queued item left it in intake with no terminal event; the
  reserve-for-`top` starvation guard then admitted nothing behind it, and the
  dispatcher, which exits only on an empty intake, held `gptps_shutdown` past the
  grace bound (`tests/test_hang.c`'s guarantee, broken from a settings write). The
  admission scan now dead-letters a never-fits item in place with `E_BUDGET`, and
  `gptps_define_resource` wakes the dispatcher on a re-budget. `tests/test_budget_shrink.c`.

## [1.1.0] - 2026-08-26

A correctness and hardening release. No breaking change: ABI stays 2.1, append-only,
and every public signature is unchanged. Everything below was found by auditing the
1.0.0 tree against its own documented guarantees, and each fix ships with the
reproduction that demonstrated it.

### Fixed — memory safety

- **Use-after-free: `gptps_unregister_task(GPTPS_REMOVE_CANCEL)` in MANUAL mode.**
  The MANUAL branch detached only `intake` and `delayed` on the premise that "nothing
  is in-flight between `gptps_step` calls". That premise was false: `gptps_step`'s
  second pass ADMITS work at the end of the step, so `ready` (and `done`) routinely
  still held items of the type being removed — items that keep both `it->reg` and
  `it->def`, which is interior to the same allocation. The registry slot was freed
  under them and the next `gptps_step` read it. THREADED was never affected because it
  blocks on `reg_live_refs`, which counts those queues. Now the MANUAL path detaches
  `ready`/`done` too (releasing the admission budget they hold, which was also being
  leaked), and refuses with `GPTPS_E_BUSY` when called re-entrantly from a task body —
  the same answer a re-entrant `gptps_shutdown` already gives.
- **Use-after-free: `gptps_dead_letter_drain()`.** The drain detaches the whole list
  and then walks it with the lock released, which the header explicitly invites a
  callback to re-enter the engine from. `detach_dead_letter` — the function that gives
  a retained item an owned name copy before its task type dies — only scans
  `e->dead_letter`, which the drain had just emptied, so unregistering that type from
  the callback freed the `gptps_reg` that `item_name()` was about to read. Every item
  is now self-owned under the lock, with **both** `it->reg` and `it->def` severed.
- **`gptps_shutdown` on an engine inherited across `fork()`.** It took `e->m` — which
  may be held by a thread that did not survive — and then joined dispatcher and worker
  `pthread_t`s that do not exist in the child. Observed: `SIGSEGV` in
  `__pthread_clockjoin_ex`.

### Fixed — the fork contract, now actually kept

`include/gptps.h` and `docs/SECURITY.md` both said an engine created before a `fork()`
returns `GPTPS_E_SHUTDOWN` from **every** entry point. Five of thirty-four
lock-taking entry points checked, and two of those checked only *after* taking the
lock — which is the hang, not the guard. All of them now check before locking, via a
single greppable `GPTPS_REFUSE_AFTER_FORK`. The `gptps_settings_*` forwarders check
too: the settings registry carries its own mutex, equally inherited.
[`tests/test_fork.c`](tests/test_fork.c) forks a live threaded engine and asserts the
refusal across mutating, read-only, settings and teardown entry points.

### Fixed — liveness: `limits.shutdown_grace_ms` is now a bound

- **The grace-cancel was not terminal, so the cancelled attempt was RETRIED.** Every
  other cancel site pairs `it->cancelled = 1` with the flag; this one raised only the
  flag, so the done-drain took the ordinary retry branch and re-admitted the item with
  a freshly cleared flag. With `max_retries = 3` a 200 ms grace made shutdown take
  **3.90 s and run the task body 4 times** — 4× *longer* than having no grace at all,
  and it discarded a result the body had already produced. Now **0.90 s, body runs
  once** (the residual is the body's own sleep; nothing can preempt an in-process
  function).
- **The grace ignored the backoff queue entirely.** The dispatcher refuses to exit
  while `delayed` is non-empty, and only promotes an item once its backoff elapses, so
  a task with `retry_backoff_seconds = 8` held teardown regardless of the grace.
  Measured **23.70 s with a 0.2 s grace**; now **0.20 s**. Past the deadline the queue
  is terminated by policy, and every item gets the terminal event it still owed.
- **`gptps_cancel` on a queued item did not wake the drain waiter.** Cancelling the
  last live item of a type left a blocked `gptps_unregister_task(DRAIN)` asleep until
  some unrelated event happened to wake the dispatcher — indefinitely on an idle
  engine. It now broadcasts `cv_drain` and signals `cv_disp` (the cancelled item may
  also have been the reserved `top` holding back skip-to-fit backfill).

### Fixed — the exactly-one-terminal-event invariant

The observer seam's whole reconciliation contract, and every add-on built on it,
depends on every submitted handle reaching exactly one terminal event. Four holes:

- **A constraint hook returning `GPTPS_DENY` could exceed the 256-entry event buffer.**
  `DENY` does not raise `e->running`, so the admission loop can deny an entire intake
  queue in one pass — and intake is unbounded by default. Measured: **600 submitted,
  600 `QUEUED`, 256 terminal** — 344 handles silently lost. The buffer's own comment
  claimed truncation was "observability only" and bounded by `max_concurrent_tasks`;
  both were wrong. A full buffer now defers the remaining work to the next pass
  instead of dropping the event, and both pumps re-run immediately while more is owed.
  Now **600/600**.
- **`item->started` was never reset per attempt**, so the done-drain's `!it->started`
  test read the *previous* attempt's state and a retried item cancelled while sitting
  in `ready` was freed with no event at all. Cleared at the single choke point every
  re-admission passes through.
- **Service instances queued or in restart backoff at `gptps_shutdown` were freed
  outright.** Measured: `queued=1 terminal=0`. They are now detached and reported.
- **Work left in the queues when the pumps stopped got no event.** It is now reported
  — and reported *before* the add-on teardown loop, because that loop calls
  `gptps_dl_close()` and an observer registered by an add-on lives in the `.so` being
  unmapped.

### Changed — admission is now O(1) in queue depth, not O(n²) overall

`engine_pass` scanned the whole intake queue **twice per admitted item** — once for the
best-scoring item that fits, once to unlink it. `limits.max_intake_depth` defaults to
0 (unbounded, deliberately), so a producer that outran the dispatcher grew the queue to
O(n) and made draining n items O(n²). It was invisible to the whole suite because it
only appears once the queue is deep:

| queued items | before | after |
|---|---|---|
| 20,000 | 0.183 s (110k/s) | 0.071 s (282k/s) |
| 40,000 | 1.231 s (32k/s) | 0.105 s (381k/s) |
| 80,000 | 8.619 s (9.3k/s) | 0.221 s (362k/s) |
| 160,000 | 36.708 s (4.4k/s) | 0.379 s (422k/s) |

Intake is now held in **admission order** (`sched_score` descending, ties oldest-first)
rather than submission order, so `top` is the head and the first item that fits is by
construction the one the old scan chose. Ordering an insert would just relocate the
quadratic, so an index of each equal-score run's tail keeps it O(1) — real workloads
use a handful of distinct priorities. The index is strictly advisory; the invariant is
that no cached tail may dangle.

**No policy changed.** Priority order, FIFO within a priority, skip-to-fit backfill and
the starvation reserve behave exactly as before —
[`tests/test_admission_order.c`](tests/test_admission_order.c) pins the exact admission
sequence, including with a scheduler hook installed, and it was written against 1.0.0
first so it could prove the order did not move.

### Fixed — executors

- **The OOP child pinned every other executor's pipe descriptors.** `O_CLOEXEC` only
  fires at `exec()`, and the OOP child never execs — it runs the task function
  in-process — so it inherited and held open a concurrent PROGRAM executor's stdin
  write end for the whole OOP task. `cat` never saw EOF and that task ran to its
  deadline, returning `GPTPS_E_TIMEOUT` with an empty result. Each executor now
  publishes the ends it owns and the child closes only those (a blanket
  close-everything is not available: `docs/SECURITY.md` promises a forked child may
  keep using host-opened descriptors).
- **A failed `waitpid()` was reported as "child exited 0".** A host that runs
  `signal(SIGCHLD, SIG_IGN)` or a wait-any reaper auto-reaps our children, so `waitpid`
  fails with `ECHILD`, `wstatus` keeps its initialiser, and `WIFEXITED(0)` is true with
  status 0 — a **failing program returned `GPTPS_OK`**. An unknowable exit status is
  now `GPTPS_E_TASK`, which keeps retries and dead-lettering working.
- **The PROGRAM child's `dup2` + blind close corrupted stdio when fd 0 or 1 was free.**
  A host that daemonised (closing stdin — a standard step) leaves fd 0 free, so
  `pipe()` hands back `inp[0] == 0`; `dup2(inp[0], 0)` is then a no-op and the
  following `close()` shuts fd 0 outright, so the program execs with no stdin and the
  payload is silently dropped. Every pipe end is now hoisted above fd 2 first.
- **Windows: unbounded `WaitForSingleObject(INFINITE)` on the helper threads.** An
  anonymous pipe reports EOF only when the *last* write handle closes, so a grandchild
  that inherited stdout kept the reader blocked long after the direct child exited —
  wedging the worker and the `gptps_shutdown` that joins it. The job is now torn down
  on every path before the joins, with a grace period and `CancelSynchronousIo` as a
  last resort when no job object is available.
- **Windows: the 16 MiB stdout cap stopped reading without killing the child**, so the
  task could only end at its deadline and reported `GPTPS_E_TIMEOUT` instead of the
  real cause. POSIX already killed at the cap; the two backends now agree.

### Fixed — configuration and settings

- **`[limits]` values from a config file were cast, not checked.**
  `max_concurrent_tasks = -1` became 4294967295 and the engine tried to start that many
  OS threads (observed: spawns until `RLIMIT_NPROC`, then hangs); `max_memory_bytes = -1`
  silently turned the operator's memory limit into no limit. A sign test alone is not
  enough — a positive value wider than the destination truncates — so each key is now
  range-checked against its field and a violation is `GPTPS_E_CONFIG`.
- **A settings value that `strtoll`/`strtoull` had saturated was accepted as valid**,
  silently applying a limit nobody asked for. `gptps_task_setting_int` likewise
  reported `LONG_MAX` as a successful parse — and `long` is 32-bit on Windows and on
  the i386 CI leg, so an ordinary value like `3000000000` clamped there while working
  on 64-bit Linux. The same gap existed in the engine's independent copy of that
  grammar (which validates a `gptps_define_global` default) and in the TOML scanner
  (so `max_memory_bytes = 99999999999999999999999` in a *file* installed `LLONG_MAX`
  — a limit that reads as no limit). All now report the range error.
- **A value that fit `unsigned long long` but not its `uint32_t` target was accepted
  and then truncated by the write callback.** `limits.max_intake_depth = 4294967296`
  validated fine and became **0**, i.e. the bound the operator had just set silently
  became *unbounded*. The four `uint32_t`-backed core settings now declare their real
  ceiling, so the value is refused instead.
- **A `NULL` from `dupn()` mid-parse published a TOML row with a `NULL` section/key**,
  which the very next `find()` `strcmp`'d — a crash on the following lookup. The TOML
  array grower also did `p = realloc(p, ...)`, leaking the old block and every string
  in it and publishing `(array = NULL, count = n)`. An incomplete parse is now a failed
  parse rather than a partially-populated table.
- **`gptps_toml_parse_file` trusted `fseek`/`ftell` unchecked**, so passing a directory
  as the config path requested an ~8 EiB allocation. The size check alone fixes that
  only on glibc, where `ftell` on a directory reports `LLONG_MAX`; on macOS/BSD it
  returns a plausible size, the allocation succeeds, and only the *read* fails with
  `EISDIR` — so the parse quietly produced an empty table and the engine started on
  compiled-in defaults having been handed a path it could not read. A `ferror()` check
  after the read is what makes the rejection portable (a short read is still fine — it
  is the truncate-and-rewrite case the buffer terminator exists for).
- **`strip_comment()` ignored backslash escapes**, truncating any string value
  containing an escaped quote before a `#` on every save→reload round trip.

### Fixed — add-ons

- **`durable_queue`: `gptps_dq_recover()` re-submitted quarantined records**, so a
  poison payload re-ran on every restart, forever. It also accepted payloads the
  replayer would always reject (destroying that record *and* every record appended
  after it), left `dq->fp == NULL` after a failed rewrite (the next submit
  dereferenced it), trusted `ftell()` on an append stream (a failed first append
  truncated the journal on Windows), and replayed in O(n²) while holding every
  completed payload in RAM.
- **`xport`: a half-read reply frame was abandoned without tearing down the link**,
  desynchronising the worker channel so the *next* submit returned a bogus `GPTPS_OK`
  carrying the wrong reply. A link that breaks mid-frame is now dead permanently.
  `submit` also never validated its own arguments against `GPTPS_XPORT_MAX_MSG`.
- **`remote`: the status codec had no wire code for `E_TASK`/`E_DUP`/`E_ABI`/
  `E_CONFIG`/`E_BUSY`**, so a remote task's ordinary application failure arrived as
  `GPTPS_E_IO` — "the link died". `encode_request` also bounded `task` and `item`
  separately against `UINT32_MAX` but not their sum.
- **`orch`: installing it made every completed task cost O(tasks completed so far)** —
  a linear scan of a set that grows for the process lifetime, paid on every terminal
  event even with zero gates. Now an open-addressed set at load factor ½. A gate whose
  submission is rejected is also no longer dropped silently; it is retried a **bounded**
  number of times and then abandoned, so `gptps_orch_pending()` still converges to 0
  (it is a documented drain predicate, and an unbounded retry would re-copy the gate's
  payload on every terminal event in the engine).
- **`tui`: the in-flight gauge underflowed to 4294967295** on a queued-item cancel and
  permanently poisoned `peak`. `gptps_tui_install` also leaked the latency ring when
  observer registration failed.
- **`gpu_quota` plug-in kept the engine in a file-static**, so loading it into a second
  engine misapplied every quota write and use-after-freed a shut-down engine.

### Fixed — other engine defects

- A failed add-on `setup()` left task types registered: the unwind was capped at 16
  names, so an add-on that registered 20 before failing kept 4 live and submittable
  while the host was told the load failed. Measured 4 → 0. The cap is gone.
- A task name longer than 312 bytes registered fine but could **never** be
  unregistered, and lost five of its six per-task settings to silent key truncation.
  `GPTPS_TASK_NAME_MAX` (127) is now documented and enforced at registration.
- Re-registering a task name while an unregister was blocked draining left the new
  task with **no settings at all**. The predecessor's settings are now torn down when
  its name becomes re-registrable, not after the drain.
- A failed per-item resource snapshot silently skipped the named-resource accounting
  *and* admitted the item anyway, un-enforcing the budget under memory pressure. It
  now fails closed.
- `e->config_path` was leaked on every `gptps_open_ex` failure path.
- **The allocator seam forwarded `ptr == NULL` to a host's `realloc_fn`.** The header
  explicitly exempts `free_fn` from ever seeing `NULL`, so a host writing a pool
  allocator reasonably infers the same of `realloc_fn` — and the core uses
  realloc-as-malloc for every growable buffer, so the *first* growth always passed
  `NULL`. A pool `realloc_fn` that trusted the docs dereferenced `NULL - HDR` and
  crashed inside the host's own code. `gptps_realloc` now routes a `NULL` to
  `malloc_fn`, `gptps.h` states the guarantee instead of leaving it implicit, and
  `tests/test_alloc.c` asserts it (its counting hooks now refuse `NULL` rather than
  handling it, and the test drives `gptps_define_resource` so the realloc-from-nothing
  path is actually exercised).

### Added

- [`tests/test_admission_order.c`](tests/test_admission_order.c) — the admission-order
  contract, plus the three ways the new queue index could dangle (cache overflow, a
  cancelled run tail, a bulk unregister). Mutation-tested: removing a cache
  invalidation makes it a hard ASan use-after-free.
- [`tests/test_admission_perf.c`](tests/test_admission_perf.c) — a complexity **gate**,
  not a benchmark. It asserts on the shape of the curve (doubling n must roughly double
  the time), so it means the same thing on a laptop and a loaded runner. It fails the
  1.0.0 engine (ratio 4.34) and passes this one (1.96).
- [`tests/test_fork.c`](tests/test_fork.c) — the fork refusal, across entry points.
- A regression test for an unsatisfiable `orch` gate.
- `CONTRIBUTING.md` and `.editorconfig`. There is deliberately **no** `.clang-format`:
  the code is hand-aligned, and every configuration tried rewrote 57–67% of the tree.
- **Regression tests for the rest of this release.** Every fix above was verified with
  a reproduction while it was being made; these promote them into the suite so they
  cannot come back. Each one was **mutation-tested** — the fix was reverted and the
  test watched to go red — because a test that cannot fail reads as coverage without
  being any: the MANUAL-mode unregister use-after-free, the add-on unwind cap (with a
  new `tests/addon_unwind.c` fixture that registers 20 types and then fails; 4 survived
  on the pre-fix build), the task-name bound, both shutdown-grace holes, the
  terminal-event buffer overflow, a service queued at shutdown, all three executor
  fixes, and the six config/TOML/settings fixes.

### Added — add-on API (all additive; no existing signature changed)

The 1.1.0 fixes left five gaps that could not be closed without new public functions.

- `gptps_orch_install_ex(e, done_cap)` and `gptps_orch_prune(o)` — **bounded retention**.
  The orchestrator remembers completed handles so a gate created *after* a dependency
  finished still resolves, and that set grew for the process lifetime. It can be
  dropped wholesale at any time, which is not obvious: no *unreleased* gate reads it (a
  dependency present at gate creation is resolved immediately, and one that terminates
  later is resolved in the same call that records it). The only cost is that a gate
  created afterwards naming an already-finished handle waits forever — the same outcome
  this header already documents for a gate created too late.
- `gptps_orch_stalled(o)`, `gptps_orch_stalled_at(o, i, buf, cap, &last)` and
  `gptps_orch_retry(o)` — **a stalled gate is now visible**. A gate the orchestrator
  gives up submitting stops being pending, and its task never ran and emitted no event;
  without these that is indistinguishable from success. `stalled_at` copies the task
  name (never hands out an interior pointer) and reports the status the engine refused
  it with. `retry` re-submits them all immediately rather than waiting for a terminal
  event that an idle engine may never produce.
- `gptps_xport_live(xp)` — and the rotation now **skips retired workers**. A worker whose
  link breaks mid-frame is retired permanently (a stream protocol cannot be
  resynchronised), but the round-robin kept handing it every Nth submit: measured, one
  dead worker in four turned **10 of 40** submits into `GPTPS_E_IO` while three healthy
  workers sat idle. Now 0. The liveness bit is mirrored under the cursor lock rather
  than read off the worker's own `dead` field, which is written under a different mutex
  — the data race that made this a deferral rather than a fix.
- `gptps_dq_drain_quarantine_ex(dq, cb, ud, &compact_status)` — the plain drain returns
  how many records the callback saw, which is true whether or not the journal was
  compacted afterwards, so a compaction failure had no channel. It matters: uncompacted
  records are replayed to the callback again after a restart. Fine for an idempotent
  callback, not for one that bills or emails.

### Fixed — Windows (compile-verified only; no Windows runtime here)

- The reader thread's join is now bounded by **closing the pipe handle before joining**
  rather than after. An anonymous pipe signals EOF only when the last write handle
  closes, so a grandchild that inherited the child's stdout kept the reader parked in
  `ReadFile` with nothing left to end it — and the `INFINITE` join then wedged the
  worker and the `gptps_shutdown` that joins it. `CancelSynchronousIo` is kept as a
  first attempt (it is the documented mechanism) but is unreliable on anonymous pipes,
  so the close is what the bound actually rests on. The writer keeps ownership of its
  own handle — it closes it to give the child EOF — so it is unblocked by the job
  teardown plus the grace, and that residual is documented rather than papered over.
  Verified by compiling `src/exec_win.c` clean under `-Wall -Wextra -Werror` against a
  Win32 shim at `_WIN32_WINNT` 0x0501, 0x0600 and 0x0601; CI compiles it for real on
  MSVC and mingw. Nothing here executes it.

### Changed — build, packaging and CI

- `bin/gptps_conformance` was built only when `GPTPS_BUILD_TESTS=ON`, so a packager's
  default build installed **no conformance harness at all** while `docs/PACKAGING.md`
  listed it in the install tree and `docs/PLUGINS.md` told plug-in authors to run it.
  It now has its own option (`GPTPS_BUILD_CONFORMANCE`, default ON) outside the test
  block.
- `gptps.pc` listed `-lpthread`/`-ldl` in `Libs.private` only, but `libgptps` is
  unconditionally STATIC — so a plain `pkg-config --libs gptps` under-linked and failed
  on `pthread_create`. CI only ever tested `--static`.
- The release version lives in `project(VERSION)` **and** `GPTPS_VERSION_STRING`;
  nothing asserted they agree. Drift is now a configure-time error.
- `-DGPTPS_HAL_SOURCE=<file>` was documented as a supported downstream knob and never
  read — setting it silently built the stock HAL. It works now.
- The s390x CI leg excluded tests by substring, and `demo` also matched
  `conformance_demo` — the only **positive** control in the conformance set. The three
  that remained are all `WILL_FAIL`, which passes on any non-zero exit, so the leg
  would have gone green with a harness that could not `dlopen` anything at all. The
  exclusion list is now anchored and explicit.
- `ci.yml` declared no `permissions:` block (inheriting repository defaults, which can
  be read/write); it is now `contents: read`. Third-party actions are pinned to commit
  SHAs rather than mutable tags — `action-gh-release` is the one step that runs
  third-party code with a `contents: write` token.

### Changed — documentation

Every claim that contradicted the code:

- The README's "Liveness guarantees" listed the intake queue among things that "cannot
  grow without bound" — while `docs/SECURITY.md` correctly said it is unbounded by
  default. The README now states the exception and why, and notes that admission is
  O(1) in depth either way.
- `docs/SECURITY.md` cited `gptps_xport` as proof a forked child may open its own
  engine; xport's children never open an engine.
- The README documented a task cost of `mem` / `gpu` / duration; `gptps_cost` has
  carried only `mem_bytes` since ABI 2.0.
- `docs/ARCHITECTURE.md` said seven add-ons ship (nine do) and that CI runs nine jobs
  (eleven, and the two omitted were `werror` and `package`).
- `docs/PLUGINS.md` said the conformance harness ships in the GitHub release; the
  release publishes amalgamation sources only.

### Removed

- `CLAUDE.md` and `.claude/` — a repo-level mandate that any AI assistant install a
  specific third-party tool, enforced by a `PreToolUse` hook that ran on a
  contributor's machine. Nothing in the build, the tests or the library referenced it.


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
