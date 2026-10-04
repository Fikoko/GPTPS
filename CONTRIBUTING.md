# Contributing to GPTPS

GPTPS is an embeddable C99 task-processing engine that runs **inside** a host process.
That single fact drives most of what follows: a bug here is a bug in someone else's
program, and anything that can hang the engine hangs their exit path.

## Build and test

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

No external dependencies beyond pthreads and `dlopen` on POSIX. A change is not done
until every test passes. The suite's size is deliberately not quoted here: `ctest`
runs a different number on each platform and add-on selection (fewer with
`GPTPS_ADDONS` narrowed), so a figure in prose only ever drifts. `ctest --test-dir
build -N` prints the real one.

Before opening a pull request, run what CI runs — these catch most of what review
would otherwise have to:

```sh
# warnings are errors on the CI `werror` leg
cmake -S . -B build-w -DCMAKE_C_FLAGS="-std=c99 -O2 -Wall -Wextra -Werror -Wno-unused-parameter"
cmake --build build-w -j

# ASan + UBSan
cmake -S . -B build-asan -DCMAKE_C_FLAGS="-fsanitize=address,undefined -g" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure

# ThreadSanitizer (add `setarch -R` if TSan reports "unexpected memory mapping")
cmake -S . -B build-tsan -DCMAKE_C_FLAGS="-fsanitize=thread -g" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan -j && ctest --test-dir build-tsan --output-on-failure

# the core must still compile with no OS at all
sh freestanding/build.sh /tmp/gptps_freestanding && /tmp/gptps_freestanding
```

CI additionally builds on macOS, Windows (mingw-w64 and MSVC), 32-bit i386, and
big-endian s390x under QEMU; it builds the single-file amalgamation and a plain
`cc -std=c99`, and it installs the package and builds an out-of-tree plug-in against
it. Assume your change will meet all of those. In particular: **do not assume 64-bit,
little-endian, or POSIX** — anything platform-specific belongs behind the HAL
(`include/gptps_hal.h`), which is the only platform seam.

## The rules that are not negotiable

**The ABI is append-only.** `include/gptps.h` is a forever-contract. You may append a
field to the end of a versioned struct and append an enumerator to the end of an enum.
You may not reorder, resize, remove, or renumber anything, and you may not change a
public function's signature. Structs are versioned by a leading `struct_size`; if you
append a field, the engine must check `struct_size` before touching it. ABI 2.0 was the
first and, by design, the last breaking change.

**Every submitted handle reaches exactly one terminal event.** The observer seam and
every add-on built on it depend on this. If you add a path that removes, drops, frees
or cancels an item, that path owes a terminal event — unless `execute()` already emitted
a **terminal** one for it: a `FINISHED`, or a `FAILED` it stamped `GPTPS_E_CANCELLED`.
A plain `FAILED` is per-**attempt** and does NOT close the handle, so `it->started`
alone is not the test — an item whose attempt merely failed is still owed one, and so
is one parked in `delayed`, whose `started` describes the attempt before. For an
always-up `GPTPS_TASK_SERVICE` a `FINISHED` closes one run, not the instance, which is
still owed the `FAILED`/`GPTPS_E_CANCELLED` its stop produces. `terminal_reported()` in
`src/engine.c` is that rule in code; `tests/test_reconcile.c` enforces it.

The two documented exceptions are opt-in policies, not licence to add more: a
`GPTPS_ON_FAILURE_REQUEUE` item stays open while it requeues (a THREADED shutdown closes
it with `DEAD_LETTERED`; a `REMOVE_CANCEL` or a MANUAL teardown with `FAILED`/
`GPTPS_E_CANCELLED`), and a `GPTPS_TASK_SERVICE` handle emits one terminal event per run.
If you are adding a third, it belongs in the Readme's guarantee list and in this rule
before it belongs in the engine.

**Nothing may hang the host.** `gptps_shutdown` always returns; `gptps_shutdown` and
`gptps_step` refuse re-entrant calls with `GPTPS_E_BUSY` rather than deadlocking, and so
does a `gptps_unregister_task` from an engine thread that would have to wait. An engine
thread never waits on the engine. `tests/test_hang.c` and
`tests/test_unregister_reentry.c` enforce these with hard timeouts.

**Lock order is `settings->m` → `e->m`.** Never take them the other way. Event
callbacks and observers run with `e->m` released and may re-enter the engine.

**No unbounded growth without a documented reason.** `limits.max_intake_depth` is the
one deliberate exception, and `docs/SECURITY.md` explains it.

## Code style

There is no `.clang-format`, on purpose — the code is hand-aligned in ways a formatter
cannot reproduce (every configuration tried rewrites more than half the tree). Match
the file you are editing:

- C99. No compiler extensions, no VLAs, `/* */` comments in `.c` files.
- Declarations at the top of a block.
- 4-space indent, no tabs. A function's opening brace goes on its own line; a control
  statement's brace attaches.
- Trailing comments and related declarations/assignments are aligned by hand. Keep the
  alignment of the block you touch.
- Long lines are fine when they keep an aligned column readable.

**Comments explain why, not what.** This codebase is unusually densely commented and
that is deliberate: nearly every non-obvious line records the failure it prevents or
the alternative that was rejected. A patch that fixes a real bug and does not say what
would go wrong without it is incomplete. Do not leave `TODO` or `FIXME` — there are
currently zero in the tree, and that is worth keeping.

## Tests

New behaviour needs a test; a bug fix needs a test that fails before it and passes
after. Prefer a test that is deterministic: MANUAL mode (`gptps_step`) runs no threads,
so queue state is exactly what you set up.

Two kinds of test are especially welcome, because they catch what unit tests do not:

- **Invariant oracles** — e.g. `tests/test_admission_order.c` pins the exact admission
  order, so a change to the queue representation cannot silently reorder work.
- **Complexity gates** — e.g. `tests/test_admission_perf.c` asserts on the *shape* of a
  curve (does doubling `n` double the time?) rather than an absolute rate, so it means
  the same thing on a laptop and a loaded runner.

**Does the test notice when the code is wrong?** A test that passes against a broken
build checks nothing. `tools/mutate.py` finds out: it breaks the code you changed, one
small change at a time - `==` for `!=`, `&&` for `||`, a negated `if` - and runs the
tests that should notice:

```sh
python3 tools/mutate.py --since origin/main --test 'config_strict|toml'
python3 tools/mutate.py --file src/settings.c --lines 600-780 --test config_strict \
        --cmake-args "-DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS=-fsanitize=address,undefined"
python3 tools/mutate.py --file src/engine.c --sample 200 --test . --exclude '_perf$|bench'
```

It works on a copy of your tree and lists each change that no test caught. Read every
survivor: it is either a weak test, or a change that makes no difference (`<` for `<=`
on a value that is never equal). With a sanitizer build it also catches what only shows
as memory damage, such as an off-by-one copy. A file like `engine.c` has over a thousand
such changes; `--sample` tests a random few hundred and estimates the score from them.

Register a test in `CMakeLists.txt` with a `TIMEOUT`. POSIX-only tests go inside the
`if(UNIX)` block. If you add a test that cannot run under QEMU (fork/exec, `dlopen`,
TTY), add its name to the `cross` job's exclusion list in `.github/workflows/ci.yml` —
the list is anchored and explicit, so a test not named there runs.

The `hal_sim` job has two such lists, anchored the same way. It runs the suite on
`tests/hal_sim.c`, where one thread runs at a time and the clock is virtual, so check
a new test against it ([`docs/HAL.md`](docs/HAL.md) has the commands):
- A test about child processes, every run of which forks, as `exec_faults` does, goes
  in the paced step's `-R` list, and in the main step's `-E` list so it runs only
  there: its runs cannot replay, and the paced step keeps its clock to real time from
  the start. So does a test that needs real time before it forks. A test that forks
  now and then can stay in the main step: a run is paced from its first fork.
- A test that blocks in a call the simulation cannot see, such as a `read()` on a
  socket another thread of the process writes, or that only times throughput on the
  clock, goes in the `-E` list alone.
- A busy-wait in a test needs a call the simulation sees in its loop -
  `gptps_now_ms(NULL)` will do, as in `test_taskmgmt.c`'s `rw_wr` - or the spinning
  thread keeps the baton, every other thread waits, and the job stalls on every seed.
- A test that keeps busy for a stretch of the clock, as a soak or a benchmark does,
  pays for each virtual millisecond in real thread switches. Time it on a seed that
  switches at every point, such as 6, against its `TIMEOUT`.

docs/HAL.md's table gives the reason for each test on a list: add yours there too. A
test that fails on a seed may have found a race - the job found one in `test_orch.c` -
so read the seed's log before you exclude it.

## Writing an add-on or a binary plug-in

See [`docs/PLUGINS.md`](docs/PLUGINS.md). Prove it before shipping:

```sh
gptps_conformance build/myplugin.so
```

It synthesises the host table as each released core actually had it and runs your
`setup()` against every rung, so a routine you called without a `struct_size` guard is
reported by name rather than segfaulting in a user's process later.

## Commits and pull requests

Conventional-commit subjects (`fix(engine):`, `feat(addons):`, `docs:`), written in the
imperative and saying what changed and why. Keep unrelated changes in separate commits.
Update `CHANGELOG.md` under `[Unreleased]` for anything user-visible.

The release version lives in both `project(VERSION ...)` in `CMakeLists.txt` and
`GPTPS_VERSION_STRING` in `include/gptps.h`; CMake fails the configure step if they
disagree, so change both together.

## Security

Do not open a public issue for a vulnerability. See
[`docs/SECURITY.md`](docs/SECURITY.md), which also states the trust boundary plainly:
the engine does **not** sandbox, and a writable config file is arbitrary code execution.

## Licence

MIT. By contributing you agree your work ships under it; keep the SPDX header on new
files.
