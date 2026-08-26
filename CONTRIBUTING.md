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

54 tests, no external dependencies beyond pthreads and `dlopen` on POSIX. A change is
not done until they all pass.

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
or cancels an item, that path owes a terminal event — unless the item's attempt already
ran, in which case `execute()` already emitted one. `tests/test_reconcile.c` enforces it.

**Nothing may hang the host.** `gptps_shutdown` always returns; `gptps_shutdown` and
`gptps_step` refuse re-entrant calls with `GPTPS_E_BUSY` rather than deadlocking.
`tests/test_hang.c` enforces these with hard timeouts.

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

Register a test in `CMakeLists.txt` with a `TIMEOUT`. POSIX-only tests go inside the
`if(UNIX)` block. If you add a test that cannot run under QEMU (fork/exec, `dlopen`,
TTY), add its name to the `cross` job's exclusion list in `.github/workflows/ci.yml` —
the list is anchored and explicit, so a test not named there runs.

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
