# GPTPS add-ons

Optional modules built on the public GPTPS API. They are **not** part of the core
library — take only what you use.

## Two tiers, and the one question that decides which

> **Does the host have to call *into* the module?**
> **Yes → compiled-in module.** It needs a header, so it needs your build. It links
> core symbols directly and can do anything the public API can — own threads, own a
> whole engine, ship platform code.
> **No → binary plug-in.** It needs only the versioned host table, so it needs only
> your *process*. Named in a TOML `addons = [...]` line, `dlopen`'d at runtime,
> configured by an operator through settings. It links **no** core symbols, which is
> exactly what lets one `.so` work against a static, shared or amalgamated host.

That rule explains every module below without special-casing any of them, and it is
not a ranking: most are compiled-in *by necessity*, because their whole point is an API
the host calls — `gptps_dq_submit()`, `gptps_orch_after()`, `gptps_tui_run()`.
`addons/gptps_gpu_quota_plugin.c` ships the same policy as `gptps_gpu_quota.c` in the
other tier — the diff between those two files is the clearest available description of
the difference.

Two of them are not add-ons in the loader's sense at all: **`pool` and `xport` are
composition libraries**. They register on no seam, use no host table, and sit *above*
the engine rather than inside it (`pool` owns N whole engines). Naming that plainly is
more useful than calling everything an add-on.

| Add-on | Tier | Seam(s) used | Platform | CMake target | pkg-config |
|---|---|---|---|---|---|
| `await` | module | observer | all | `gptps::await` | `gptps-await` |
| `durable_queue` | module | observer | all | `gptps::durable_queue` | `gptps-durable_queue` |
| `gpu_quota` | module | *(named resources)* | all | `gptps::gpu_quota` | `gptps-gpu_quota` |
| `gpu_quota_plugin` | **plug-in** | *(named resources + settings)* | all | *(MODULE, `.so`)* | — |
| `orch` | module | observer | all | `gptps::orch` | `gptps-orch` |
| `pool` | *composition* | none — public API only | all | `gptps::pool` | `gptps-pool` |
| `remote` | module | none — wire codec | all | `gptps::remote` | `gptps-remote` |
| `tui` | module | observer + settings | all | `gptps::tui` | `gptps-tui` |
| `wasm_exec` | module | task | all | `gptps::wasm_exec` | `gptps-wasm_exec` |
| `xport` | *composition* | none — public API only | **POSIX** | `gptps::xport` | `gptps-xport` |

## Getting one

Three supported ways — see [`docs/PACKAGING.md`](../docs/PACKAGING.md) for the detail:

```cmake
# installed package, a subset by name
find_package(gptps 1.0 REQUIRED COMPONENTS durable_queue pool)
target_link_libraries(myapp PRIVATE gptps::durable_queue gptps::pool)
```

```sh
# pkg-config
cc -std=c99 myapp.c $(pkg-config --cflags --libs gptps-durable_queue gptps-pool) -o myapp

# amalgamation: no build system, no clone - one .c/.h pair per add-on
sh tools/amalgamate.sh out --addons durable_queue,pool
cc -std=c99 myapp.c out/gptps.c out/gptps_durable_queue.c out/gptps_pool.c -Iout -lpthread -ldl
```

---

## await — block until a handle finishes

`gptps_await.c` / `gptps_await.h`. The core is event-driven by design and "futures /
promises in the engine" is a stated non-goal, on the grounds that a blocking
`wait(handle)` is a small amount of code on the observer seam. This is that code, so
the claim is backed by something you can link.

- **Install before you submit.** In THREADED mode a task can finish *before*
  `gptps_submit` returns its handle; the observer is registered up front and unclaimed
  completions are retained, which is the only place that race can be closed.
- **Retention is bounded** — a fixed ring that evicts oldest, not the process-lifetime
  growth `orch` documents. Size it with `gptps_await_install_ex`, and always pass a
  timeout you can live with.
- **Two statuses, deliberately:** the return says whether the *wait* succeeded,
  `*out_status` says whether the *task* did. A task that failed is a successful wait.
- **Not for use inside a task body or callback** — blocking a worker on another item's
  completion invites deadlock.
- **Ordering:** `gptps_await_close()` after `gptps_shutdown()`.

```c
gptps_await *aw = gptps_await_install(engine);      /* BEFORE submitting */
gptps_submit(engine, "resize", buf, len, &h);
gptps_await_wait(aw, h, 5000, &res, &rlen, &task_status);
free(res);
gptps_shutdown(engine); gptps_await_close(aw);
```

`gptps_await_quiesce(aw, n, ms)` covers "tell me when N things are done" — it needs no
retention at all, so it is the right call for bulk work and benchmarks.

## durable_queue — crash-durable submission

`gptps_durable_queue.c` / `gptps_durable_queue.h`. Submit through `gptps_dq_submit()`
instead of `gptps_submit()`: the `(task, payload)` is written to an append-only journal
and `fsync`'d **before** the task is enqueued, so a crash afterward is recoverable. The
module registers an observer to mark a record complete once its task reaches a terminal
state; `gptps_dq_recover()` re-submits anything a prior run left unfinished.

- **Guarantee:** at-least-once — task bodies must be idempotent.
- **Quarantine drains are at-least-once too.** `gptps_dq_drain_quarantine()` compacts
  the drained records out of the journal afterwards; if that compaction fails they are
  still on disk, so a restart hands them to your callback **again**. Fine for an
  idempotent callback, not fine for one that bills or emails — use
  `gptps_dq_drain_quarantine_ex()`, which reports the compaction status separately from
  the drained count.
- **Ordering:** call `gptps_dq_close()` **after** `gptps_shutdown()`.
- **Portability:** Linux/macOS/Windows (via the `addon_compat` mutex + fsync shim).

```c
gptps_dq *dq = gptps_dq_open(engine, "queue.journal");
gptps_dq_recover(dq);                              /* replay prior-run survivors */
gptps_dq_submit(dq, "resize", buf, len, &handle);  /* durable submit */
gptps_shutdown(engine);
gptps_dq_close(dq);
```

## gpu_quota — admission quota over a named resource

`gptps_gpu_quota.c` / `gptps_gpu_quota.h`. Caps total in-flight GPU usage so the engine
never *starts* more GPU work than your budget allows.

**Read this one first if you are learning the named-resource API.** It is a thin wrapper
over `gptps_define_resource` / `gptps_set_task_resource_cost` / `gptps_resource_usage`
and carries no counter, no lock, and no bookkeeping — about 90 lines, most of them
comment. "GPU units" is not a mechanism, it is a *name* for one: swap the string and the
same code caps licence seats, IO bandwidth, or a per-tenant quota. That is why the core
has no idea what a GPU is.

- **Scope:** admission-level *oversubscription prevention*, not driver-level VRAM
  enforcement (that needs vendor APIs). Pair it with an OOP/cgroup memory cap for hard
  limits.
- **Careful with the budget's meaning:** the engine admits when
  `reserved + cost <= budget`, so a budget of **zero** with a non-zero cost admits
  **nothing** — it is the most restrictive setting, not "unlimited".
- **Ordering:** the engine owns the budget, so `gptps_gpu_quota_close()` only releases
  the handle.

```c
gptps_gpu_quota *q = gptps_gpu_quota_install(engine, /*total*/ 8, /*unused*/ 0);
gptps_register_task(engine, &def);
gptps_gpu_quota_set_task_units(q, "infer", 2);   /* each item costs 2 units */
gptps_shutdown(engine);
gptps_gpu_quota_close(q);
```

Before ABI 2.0 this was a constraint + observer pair with its own counter, keyed off a
`gptps_cost.gpu_units` field. It had to infer releases from the event stream by task
name, which leaked the budget on any terminal path the observer did not see — and there
was one. Reserving through the core deletes the failure mode instead of patching it.

## gpu_quota_plugin — the same policy, as a binary plug-in

`gptps_gpu_quota_plugin.c`. Same behaviour, other tier: the host calls **nothing**. Load
it by path and configure it entirely through settings —

```toml
addons = ["/usr/local/lib/gptps/gpu_quota_plugin.so"]
[tasks.render]
"gpuq.units" = 2
```
```sh
gpuq.total_units = 4     # or set live via gptps_settings_set
```

Namespaced `gpuq`, so it cannot collide with anything else — including with the Tier-A
`gpu_quota` above, whose resource is called `gpu`. Both can be loaded at once.

## orch — run-after / fan-in dependencies

`gptps_orch.c` / `gptps_orch.h`. Gate a submission on a set of handles and release it
once they have all reached a terminal state. Built entirely on the observer seam plus
`gptps_submit`, which is the proof that task dependencies belong on top of GPTPS rather
than in the dispatcher.

- **What counts as terminal:** `FINISHED`, `DROPPED`, `DEAD_LETTERED`, or `FAILED`
  carrying `GPTPS_E_CANCELLED`. A plain `FAILED` is **not** terminal — it is emitted
  after every failed *attempt*, and a dependency that merely retries must not release
  your gate.
- **Two shapes never terminate at all:** a task type with
  `GPTPS_ON_FAILURE_REQUEUE`, and a `GPTPS_TASK_SERVICE` instance. A gate on one waits
  forever — correctly, but surprisingly.
- **Retention is bounded on request.** Completed handles are remembered so a gate
  created *after* a dependency finished still resolves. `gptps_orch_install` keeps them
  for the process lifetime; `gptps_orch_install_ex(e, cap)` drops the set once it
  reaches `cap`, and `gptps_orch_prune(o)` drops it on demand. Dropping is always safe
  — no *unreleased* gate reads the set — and its only cost is that a gate created
  afterwards which names an already-finished handle waits forever, which is the same
  outcome as creating a gate too late.
- **A gate that never gets submitted is visible.** A gate whose deps are all satisfied
  is submitted at once, and that submit can be refused (type paused, intake full, name
  never registered, cost that can never fit). The orchestrator retries the transient
  cases a bounded number of times and then gives up, so `gptps_orch_pending()` always
  converges to 0 — which is what makes it a usable drain predicate, and also what would
  otherwise hide the failure. `gptps_orch_stalled()` counts the gates it gave up on,
  `gptps_orch_stalled_at()` names one and reports the status it was refused with, and
  `gptps_orch_retry()` re-submits them all once you have fixed the cause.

```c
gptps_orch *o = gptps_orch_install(engine);
gptps_submit(engine, "A", ..., &hA);
gptps_submit(engine, "B", ..., &hB);
gptps_handle deps[2] = { hA, hB };
gptps_orch_after(o, "C", ..., deps, 2, NULL);   /* C runs after A and B */

/* did anything fail to launch? */
if (gptps_orch_stalled(o)) {
    char task[64]; gptps_status why;
    gptps_orch_stalled_at(o, 0, task, sizeof task, &why);
    fprintf(stderr, "gate on '%s' never ran: %s\n", task, gptps_strerror(why));
    /* ... fix it (resume the type, register the name), then: */
    gptps_orch_retry(o);
}
gptps_shutdown(engine); gptps_orch_close(o);
```

## pool — scale UP by composition

`gptps_pool.c` / `gptps_pool.h`. The core is a single-writer engine — one lock, one
dispatcher — which is simple, correct, and the single-node throughput ceiling. Rather
than complicate the core, this runs **N independent engine shards** (each its own lock,
dispatcher and worker pool) and routes each submit to one. It needs **no engine change
at all**, which is the whole point: that file is the proof that scale-by-composition
works.

- **Sizing:** with N shards you get N independent budgets, so set `cfg->limits` so the
  shards *sum* to what the machine can bear. A NULL cfg auto-tunes each shard to the
  whole machine, which oversubscribes for N > 1.
- **Routing:** `gptps_pool_submit` is round-robin; `gptps_pool_submit_keyed` pins a key
  to a shard (per-tenant locality, or order among items sharing a key).
- Register the **same** task types on every shard — a submit may land on any.

Measured on a 32-core box: aggregate tiny-task throughput ~15k/s at 1 shard to ~290k/s
at 8 (see `examples/bench_pool.c`).

## remote — the cross-host wire protocol (codec; transport pending)

`gptps_remote.c` / `gptps_remote.h`. `xport` says the local socketpair "is the only
thing standing between this and cross-MACHINE execution: swap it for a TCP socket and
the same protocol reaches another host." That is true of the *shape* and false of the
*details*, and this module exists to make the details honest.

**Today it is the codec only, deliberately.** A wire format is a second
forever-contract standing beside ABI 2.0 — once one peer anywhere speaks version 1,
every future version must interoperate with it, and unlike a C ABI there is no
compiler to catch a violation and no way to recall a deployed peer. So the bytes are
defined, exhaustively tested and reviewable on their own, before a socket exists to
hide bugs behind.

What a network commits to that a socketpair does not, and what this fixes:

- **Byte order.** `xport` writes raw native-endian integers — free on one machine,
  silent corruption between a little-endian client and a big-endian server. Everything
  here is explicitly big-endian, written byte by byte: never a `memcpy` of an integer,
  never a cast of the buffer to a struct pointer (which is also an alignment fault on
  strict targets). The tests assert the **actual bytes**, not a round trip — a round
  trip passes just as happily on a native-endian codec.
- **`gptps_status` values become wire-visible.** The enum fixes only `GPTPS_OK = 0`;
  the rest are positional. The wire carries its own stable codes, and one from a newer
  peer degrades to `GPTPS_E_IO` rather than being reinterpreted.
- **A request id**, so a transport *can* multiplex. `xport` holds a lock across the
  whole round trip, which caps a link at one in-flight request — fine at socketpair
  latency, a hard ceiling of a few thousand/sec over a real network.
- **A length cap that is a defence.** `xport` allows 256 MiB, reasonable against your
  own forked child; from an unauthenticated peer it is one-packet memory exhaustion.
  Default here is 1 MiB, per-link configurable, and checked *before* the caller is
  told how many bytes to read.

**Security, plainly:** the `task` field of a request is a dispatch key chosen by the
peer. On a listening socket that is remote code *selection* by whoever can connect.
There is no authentication and no encryption here by design — run it inside a trusted
boundary (loopback, WireGuard, a TLS terminator, an SSH tunnel), never on an open port.

## tui — real-time terminal dashboard

`gptps_tui.c` / `gptps_tui.h`. A live, portable terminal dashboard over a running
engine, built on the observer seam — ANSI/VT escapes only (no ncurses; Windows VT
enabled automatically). Panes/metrics:

- header: uptime + **throughput** (done/s);
- counts (queued/started/finished/failed/retried/dead) + an **in-flight gauge bar**;
- per-task table: run / ok / fail / dead, **success rate (ok%)**, and **average
  queue→finish latency (ms)**, plus the task's hotkey;
- a **scrollable** recent-events log (timestamped) — `k`/`j` scroll older/newer.

- **Cost is a budgeted knob** — the dashboard runs on the engine's worker threads, so
  its own CPU/RAM is tunable **at runtime**:
  - **KPI level** (`gptps_tui_set_kpi`, or `m` live): `MINIMAL` = counts only;
    `NORMAL` = + per-task table + recent log; `FULL` = + per-handle latency (allocates
    a ring; **freed when you drop below FULL**).
  - **Cadence** (`gptps_tui_set_mode`, or `p` live): `CONTINUOUS`, `ON_DEMAND`,
    `PAUSED`. `gptps_tui_snapshot()` renders one frame on demand.
- **Testable split:** `gptps_tui_render()` returns the frame as a string and
  `gptps_tui_press()` applies a key — both unit-tested headlessly; `gptps_tui_run()` is
  the blocking live loop (and self-skips without a TTY).
- **Ordering:** `gptps_tui_close()` after `gptps_shutdown()`.

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

## wasm_exec — run WebAssembly modules as tasks (bring-your-own-runtime)

`gptps_wasm_exec.c` / `gptps_wasm_exec.h`. A `.wasm` module is portable, sandboxed task
code — "write one task, run it on any hardware." A wasm *interpreter*, though, is a heavy
dependency, so this add-on owns the GPTPS-side integration (admission, cost/priority,
retries/timeout, result delivery, and — in OOP mode — OS-enforced memory caps +
hard-kill) and leaves the **runtime pluggable**: you supply one `gptps_wasm_run_fn` that
drives wasm3 / wasmtime / WAMR / etc.

- **Why pluggable:** keeps the core dependency-free and portable. (Zero-glue
  alternative: `GPTPS_EXEC_PROGRAM` with a runtime CLI, e.g.
  `argv = {"wasmtime", "module.wasm", NULL}`.)
- **Modes:** in-process (cooperative cancel; all platforms) or OOP (forked, OS-capped,
  hard-killed; POSIX only — the runtime must be fork-safe).
- **Ordering:** `gptps_wasm_close()` after `gptps_shutdown()`.

```c
gptps_wasm *w = gptps_wasm_install(engine, run_wasm3, NULL);
gptps_wasm_register(w, "resize", "/modules/resize.wasm", /*oop*/ 1, NULL, NULL);
gptps_submit(engine, "resize", jpeg, jpeg_len, &h);   /* runs the module, sandboxed */
gptps_shutdown(engine);
gptps_wasm_close(w);
```

## xport — scale OUT by composition (POSIX)

`gptps_xport.c` / `gptps_xport.h`. Where `pool` scales up inside one process, this scales
out: it forks N persistent worker **processes** and ships each submit to one over a
socketpair, marshalling the result back. Work therefore runs in a separate address space
— crash-isolated and independently capped.

- **A broken link retires its worker, and the rotation skips it.** The wire protocol
  cannot be resynchronised mid-frame — the next submit would read this frame's
  leftovers as its own reply — so a worker whose link breaks is retired permanently and
  is not respawned. `gptps_xport_count()` is the pool size and never changes;
  `gptps_xport_live()` is the remaining capacity and only falls. At 0, every submit
  returns `GPTPS_E_IO`.
- **Consumes no seam.** It never calls into an engine and does not use the add-on ABI:
  the core is not on its path. That is the point, not a gap — the composition pattern
  needs nothing from the core, which is why the core offers it nothing.
- **POSIX only** (`fork` + `socketpair`), like `GPTPS_EXEC_OOP`. Windows has no `fork`,
  and faking it would make the module dictate your program's startup.
- **Fork safety:** the handler runs in a forked child, so call `gptps_xport_open`
  before you start other threads.
- **Wire format is native-endian** — it is a same-machine socketpair. A cross-host
  transport would need a fixed byte order, a versioned header and a request id; the
  header says so rather than implying TCP is a drop-in swap.

---

New add-on? `tools/check_addon_coverage.sh` holds this file, `addons/CMakeLists.txt` and
`tools/amalgamate.sh` to the directory listing, so an add-on cannot be added without
becoming buildable, obtainable and documented. It fails if any of the three drifts —
which is how three of them silently went undocumented before it existed.
