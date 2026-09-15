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

Three of them are not add-ons in the loader's sense at all: **`pool`, `balance` and
`xport` are composition libraries**. They use no host table and sit *above* the engine
rather than inside it (`pool` owns N whole engines; `balance` owns the queue in front
of them; `xport` owns N worker processes). Naming that plainly is more useful than
calling everything an add-on.

| Add-on | Tier | Seam(s) used | Platform | CMake target | pkg-config |
|---|---|---|---|---|---|
| `await` | module | observer | all | `gptps::await` | `gptps-await` |
| `balance` | *composition* | observer (on each shard) — routes above `pool` | all | `gptps::balance` | `gptps-balance` |
| `durable_queue` | module | observer | all | `gptps::durable_queue` | `gptps-durable_queue` |
| `gpu_quota` | module | *(named resources)* | all | `gptps::gpu_quota` | `gptps-gpu_quota` |
| `gpu_quota_plugin` | **plug-in** | *(named resources + settings)* | all | *(MODULE, `.so`)* | — |
| `orch` | module | observer | all | `gptps::orch` | `gptps-orch` |
| `pool` | *composition* | none — public API only | all | `gptps::pool` | `gptps-pool` |
| `remote` | module | none — wire codec | all | `gptps::remote` | `gptps-remote` |
| `stats` | module | observer | all | `gptps::stats` | `gptps-stats` |
| `tui` | module | observer + settings | all | `gptps::tui` | `gptps-tui` |
| `wasm_exec` | module | task | all | `gptps::wasm_exec` | `gptps-wasm_exec` |
| `xport` | *composition* | none — public API only (owns an engine per worker) | **POSIX** | `gptps::xport` | `gptps-xport` |

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

## balance — late-binding load balancer above pool

`gptps_balance.c` / `gptps_balance.h`. `pool` routes each submit to a shard as it
arrives — round-robin or by key. Right for uniform work, wrong for mixed sizes: one
shard can hold three long items while its neighbours idle, and nothing can move them,
because an item inside an engine's queue belongs to that engine (the public API has
no "give me back a queued item", and should not — that queue is the engine's admission
ledger). This module keeps the queue where it *can* be re-routed: in the router. Work
waits here, in one priority queue; each shard is handed only what it can run now plus
a bounded depth; when a shard finishes something (observer seam) the next item goes to
whichever shard has the least outstanding. Join-shortest-queue with late binding —
work-stealing in effect, since idle shards pull — and it adapts to any task size
without knowing sizes in advance: a shard running one long item simply stops being the
shortest queue.

```c
p = gptps_pool_open(4, &cfg); gptps_pool_register_task(p, &def);
b = gptps_balance_open(p, NULL);              // BEFORE submitting; NULL cfg => depth 2x workers
gptps_balance_set_event_cb(b, on_event, ud);  // events carry BALANCE handles
gptps_balance_submit(b, "resize", buf, len, &h);
gptps_balance_submit_ex(b, "resize", buf, len, /*priority*/ 5, &h);
... gptps_pool_close(p); gptps_balance_close(b);   // close AFTER the pool
```

- **Events:** the host sees the engine's lifecycle events for balanced work with the
  handle rewritten to the balance handle, plus a `QUEUED` emitted here at submit (the
  item queued *here*) and a terminal `DEAD_LETTERED` / `DROPPED` for an item this module
  could not hand over (shard refused the submit, or close with work still queued).
  Every balance handle reaches exactly one terminal event — the core's guarantee,
  kept. Work submitted straight to a shard is neither seen nor counted.
- **`shard_depth`:** how many items a shard may hold at once (running + waiting in
  *its* queue). Deep enough that the engine's skip-to-fit and starvation guard still
  have something to order; shallow enough that a late-arriving long item cannot bury a
  shard. Set it to `max_concurrent_tasks` for no prefetch at all.
- **Priority** orders the router queue (then FIFO) and is passed to the shard on
  dispatch, so the engine's ordering agrees with the router's.
- **`gptps_balance_queued()`** / **`gptps_balance_shard_load(i)`** are the gauges;
  install `stats` on the shards for everything else.
- **Cost:** one lock and one hash lookup per event; a copy of the payload while the
  item waits here (the engine copies again at dispatch).
- **THREADED shards only**: dispatch is driven from their dispatcher threads.
- **Measured — and where it does NOT help.** `examples/bench_balance.c` runs the same
  heavy-tailed workload (1 in 32 items 100× longer) through round-robin `pool` and
  through `balance` on the same 4 shards. On a 32-thread machine, makespan vs ideal:

  | items | round-robin | late binding | balance / rr |
  |---:|---:|---:|---:|
  | 200 | 1.98× | 1.44× | **0.73** |
  | 1,000 | 1.54× | 1.07× | **0.69** |
  | 20,000 | 1.01× | 0.98× | 0.96 |

  The last row is the honest one: on a long, stationary stream the law of large
  numbers balances round-robin for free, and this module buys nothing. It earns its
  place for **batches and bursts** — "render these 40 thumbnails", "process this
  upload" — where a few long items landing on one shard is the whole makespan, and
  for **per-item wait**, where a short item is no longer stuck behind a long one.
  Task bodies spin, so the bench needs ≥ 4 free cores to mean anything.
- **Not** cross-process (`xport` in engine mode is a fine thing to put behind a
  shard) and **not** a scheduler for the shards' own queues.

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
- **A request id**, so a transport *can* multiplex. `xport` now carries one too (its
  links are multiplexed, `max_in_flight` per worker); the codec here defines the same
  idea with a fixed width and byte order a foreign peer can rely on.
- **A length cap that is a defence.** `xport` allows 256 MiB, reasonable against your
  own forked child; from an unauthenticated peer it is one-packet memory exhaustion.
  Default here is 1 MiB, per-link configurable, and checked *before* the caller is
  told how many bytes to read.

**Security, plainly:** the `task` field of a request is a dispatch key chosen by the
peer. On a listening socket that is remote code *selection* by whoever can connect.
There is no authentication and no encryption here by design — run it inside a trusted
boundary (loopback, WireGuard, a TLS terminator, an SSH tunnel), never on an open port.

## stats — counters, gauges and latency on the observer seam

`gptps_stats.c` / `gptps_stats.h`. The README's non-goals table promises "aggregate in an
observer add-on" and then, until now, no such add-on existed. This is it: one observer
per engine keeps **totals** (queued / started / finished / failed / retried /
dead-lettered / dropped / cancelled), **live gauges** (pending in the queue, in flight),
and **latency** (queue wait, and run time per attempt: sum, max, sample count) — for the
engine and per task type. A snapshot is a plain struct; exporting it to Prometheus,
statsd, OTel or a log line is the host's ten lines, which is exactly what keeps this
module from dating the library to a wire format.

```c
gptps_stats *st = gptps_stats_install(e);          // BEFORE you submit anything
...
gptps_stats_counters c;
gptps_stats_total(st, &c);                          // c.pending, c.in_flight, c.run_ms_sum / c.run_samples ...
gptps_stats_task(st, "resize", &c);                 // one task type
... gptps_shutdown(e); gptps_stats_close(st);       // close AFTER shutdown, like await
```

- **Scaling:** one `gptps_stats` per engine. With `gptps_pool`, install one on each
  `gptps_pool_shard(p, i)` and fold them with `gptps_stats_merge()` — the sum is the pool.
  With `gptps_xport` in engine mode, install it from the `child_init` hook; each worker
  process then has its own.
- **Order-independent:** `QUEUED` is emitted on the submitting thread and everything
  else on the dispatcher, so a fast task can report `STARTED` (or `FINISHED`) before its
  own `QUEUED`. Every transition is keyed on the handle's current state, not on arrival
  order; the one visible effect is that a `STARTED` that outruns its `QUEUED` has no
  queue-wait sample.
- **Cost:** one lock and one hash lookup per event. The handle table is bounded by the
  engine's own queue.
- **Not settings.** The counters are deliberately not registered in the settings
  registry: there is no `gptps_unregister_setting`, so a registered read callback
  would outlive `gptps_stats_close`. If a host wants them in the TUI, that is the
  accessor argument the non-goals table asks for.

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
out: it forks N persistent worker **processes** and ships each request to one over a
socketpair, marshalling the result back. Work runs in a separate address space —
crash-isolated and independently capped. Two modes:

- **Engine mode** (`gptps_xport_open_ex` with a task table): **every worker runs its own
  GPTPS engine.** The worker pool, memory and named-resource budgets, retries, timeouts,
  dead-letter and all four seams apply per worker process, exactly as in-process. The
  reply carries the item's terminal status: `GPTPS_OK` + result on `FINISHED`, the
  failure status on `DEAD_LETTERED` / `DROPPED`, `E_CANCELLED`. A `child_init` hook runs
  in each worker between open and the first request — define resources, register
  constraints, install `stats` or `durable_queue` there. This is the mode that makes
  scale-out *keep* what GPTPS is for; `pool` and `xport` are now the same pattern at two
  levels (engines behind a router; engines behind a link).
- **Handler mode** (`gptps_xport_open`, unchanged): a bare handler per request, no engine.
  Plain crash-isolated RPC, for hosts that want exactly that.

```c
gptps_xport_config cfg = { .struct_size = sizeof cfg, .nworkers = 4,
                           .engine_cfg = &per_worker_limits, .tasks = table, .ntasks = n,
                           .child_init = install_stats };
gptps_xport *xp = gptps_xport_open_ex(&cfg);
gptps_xport_submit(xp, "resize", buf, len, &res, &rlen, &task_status);          // blocks
gptps_xport_submit_async(xp, "resize", buf, len, on_reply, ud, &request_id);    // returns at once
gptps_xport_close(xp);                                                           // graceful drain
```

- **Multiplexed links.** Every request carries an id; a reader thread per link matches
  replies to waiters; up to `max_in_flight` requests (default 64) may be outstanding per
  worker, beyond which submit returns `GPTPS_E_FULL` — the same backpressure shape as the
  core's `limits.max_intake_depth`. A worker with 8 engine threads therefore receives 8
  concurrent requests, which is what makes engine mode useful. (The old transport held a
  lock across the whole round-trip: one request per link.)
- **Sizing, engine mode:** a `NULL engine_cfg` auto-tunes *each* worker to the whole
  machine, which oversubscribes for N > 1 — set `engine_cfg` so the workers *sum* to
  what the box can bear, exactly as with `pool` shards. The worker's
  `limits.shutdown_grace_ms` bounds how long `gptps_xport_close` waits for its drain.
- **A broken link retires its worker, and the rotation skips it.** Everything
  outstanding on it fails with `GPTPS_E_IO`. It is not respawned — the parent now has
  reader threads, and `fork()` from a multi-threaded process is only safe before those
  exist. `gptps_xport_count()` is the pool size and never changes; `gptps_xport_live()`
  is the remaining capacity and only falls. At 0, every submit returns `GPTPS_E_IO`.
- **Graceful close.** `close()` shuts the request side of each link; a worker sees EOF,
  drains (engine mode: `gptps_shutdown`, bounded by its grace), sends what it still
  owes, and exits. Blocking submits outstanding at close get real answers; async ones
  get their callback. Only then are readers joined and pids reaped.
- **Consumes no seam.** It never calls into the *parent's* engine and does not use the
  add-on ABI; the child's engine is its own, opened fresh after the fork (the supported
  case in [docs/SECURITY.md](../docs/SECURITY.md)). The composition pattern needs
  nothing from the core, which is why the core offers it nothing.
- **POSIX only** (`fork` + `socketpair`), like `GPTPS_EXEC_OOP`.
- **Fork safety:** call `gptps_xport_open*` before you start other threads.
- **Wire format is native-endian and unversioned** — parent and child are the same
  forked binary, so there is no second peer that could disagree. A cross-host transport
  needs `remote`'s codec, and a request id (which this framing now has) is the first
  thing it would carry across.

---

New add-on? `tools/check_addon_coverage.sh` holds this file, `addons/CMakeLists.txt` and
`tools/amalgamate.sh` to the directory listing, so an add-on cannot be added without
becoming buildable, obtainable and documented. It fails if any of the three drifts —
which is how three of them silently went undocumented before it existed.
