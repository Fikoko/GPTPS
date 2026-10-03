# Bounded mode: no allocation once work starts

By default GPTPS allocates as it goes. Every submit allocates an item and a copy of its
payload, every admission that uses named resources allocates a snapshot, every result
is copied into a new block, and the handle index grows and shrinks with the queues.
`gptps_set_allocator` decides *where* that memory comes from, but not *how much* or
*when*.

Bounded mode decides both. The host declares its maxima up front. When work starts,
the engine allocates everything it will ever need, and from then on the work path
neither allocates nor frees. Every operation's cost is bounded by those maxima, with
no amortized rebuild that makes one unlucky call slow. This is the configuration a
memory-constrained or safety-critical host needs: MISRA C:2012 Directive 4.12 asks
for no dynamic allocation at run time, and ISO 26262 / IEC 61508 practice asks for
memory that can be accounted for before the system runs. No GPTPS release is
certified; see [SAFETY.md](SAFETY.md).

## Turning it on

Three fields at the end of `gptps_config` (ABI 2.4):

```c
gptps_config cfg;
memset(&cfg, 0, sizeof cfg);
cfg.struct_size        = sizeof cfg;
cfg.limits.struct_size = sizeof cfg.limits;
cfg.max_items          = 256;   /* items alive at once: queued, retrying, running, dead-lettered */
cfg.max_payload_bytes  = 64;    /* each item's payload slot */
cfg.max_result_bytes   = 32;    /* each executing thread's result buffer */
gptps_open_ex(&cfg, &e);
```

`max_items == 0`, the default, is the classic allocating mode, which is unchanged.
`max_items` is a `uint64_t` for an ABI reason, not because anyone needs 2^64 items.
As the first field appended to `gptps_config`, it must grow the struct on every ABI,
so a caller built against an older header never appears to have set it.

## The lifecycle

1. **Open and set up as usual.** Register tasks, define named resources, add observers
   and constraints. These calls allocate, as they always have.
2. **The first submit that names a registered task seals the engine.** A submit whose
   task is unknown seals nothing, so a typo in it does not end setup. The seal
   allocates, in one go:
   - `max_items` items, each with its cancel flag inside it;
   - a payload slot of `max_payload_bytes` for each item, 16-byte aligned, so a task
     can read its payload as a struct;
   - a named-resource snapshot slot for each item, for the resources defined so far;
   - the handle index at its final size (the next power of two at or above twice
     `max_items`, and at least 64 slots). Deletion leaves no tombstones, so the index
     is never rebuilt;
   - one result buffer of `max_result_bytes` for each worker, or one in MANUAL mode;
   - the records that guard callbacks against re-entry: one for each worker, the
     dispatcher, and 32 for host threads.

   If that allocation fails, the submit returns `GPTPS_E_NOMEM`, and nothing is sealed.
3. **From then on, the work path neither allocates nor frees.** That covers submit,
   `submit_ex`, cancel, the dispatcher, the workers and `gptps_step`, retries and
   backoff, deadlines, events, results, dead letters and their drain, and settings
   reads and writes.

## What changes when an engine is bounded

| Situation | Bounded mode |
|---|---|
| All `max_items` items are alive | `gptps_submit` returns `GPTPS_E_FULL`. Dead letters hold their items until they are drained or evicted (`limits.max_dead_letters`). |
| A payload longer than `max_payload_bytes` | `gptps_submit` returns `GPTPS_E_INVAL`. |
| `gptps_result_set` with more than `max_result_bytes` | Returns `GPTPS_E_INVAL`, and the attempt keeps no result. `gptps_result_set_nocopy` copies nothing and works as before. |
| Setup after the seal | `register_task`, `unregister_task`, a new name in `define_resource`, `define_global`, `define_task_setting`, `register_setting`, `register_observer`, `register_constraint` and `load_addon` return `GPTPS_E_BUSY`. Re-budgeting an existing resource, `set_task_resource_cost` and `set_scheduler` still work, because they allocate nothing. |
| Forked and external-program tasks | `register_task` refuses them (`GPTPS_E_INVAL`). Starting a process allocates by nature, so bounded mode runs in-process tasks only. |
| A dead-letter drain that re-submits | The re-submit needs a free item, and the item being drained is freed only after its callback returns. On a full pool, the first re-submit of a drain returns `GPTPS_E_FULL`. Keep headroom in `max_items`. |

## Sizing

What the seal allocates, measured with a counting allocator (`gptps_set_allocator`) on
a 64-bit build. The configurations use 64-byte payloads, 32-byte results and two named
resources:

| Configuration | The seal allocates | Per item |
|---|---|---|
| MANUAL, 256 items | 86,704 bytes | 339 bytes |
| THREADED, 4 workers, 256 items | 87,408 bytes | 341 bytes |
| THREADED, 4 workers, 4,096 items | 1,316,208 bytes | 321 bytes |

Per item that is roughly the item itself (208 bytes), its payload slot, 8 bytes per
named resource, and two to four 16-byte index slots. The workers' result buffers and
the callback records are a fixed cost on top. Open and setup allocate a few kilobytes
before the seal. To size a static arena exactly, measure your own configuration the
same way; `tests/test_bounded.c` shows how.

## Speed

On the machine these docs were written on (i9-14900K), with both engines limited to
4,096 items (the classic one through `limits.max_intake_depth`):

| | Classic | Bounded |
|---|---|---|
| Idle round trip, submit to `FINISHED`, p50 | 6.2–8.4 µs | 5.8–8.5 µs |
| One producer, 4 workers | 485–524k tasks/s | 530–556k tasks/s |
| Four producers, 4 workers | 455–497k tasks/s | 439–447k tasks/s |

Latency is the same. One producer is faster without a malloc and a free per task. Four
producers are slower by a few percent because the pool has its own lock. The
alternative to that lock is copying each payload under the engine's one lock, which
would cost every producer more as payloads grow.

## What the guarantee does not cover

- **Settings save, reload and watchers.** They read files and parse TOML. They are
  administration, not the work path, and they allocate.
- **Add-ons.** Each add-on manages its own memory. `durable_queue`, for one, allocates
  per record.
- **The HAL's own create calls**, which run at open: the mutexes, condition
  variables and threads. After open, the HAL allocates nothing the core asks for.
- **More than 32 host threads inside engine callbacks at once.** The extra threads'
  callbacks run unguarded: a `gptps_shutdown` or `gptps_step` called from inside one
  of them is not refused. The classic mode has the same rule past 1024 threads.

## How it is held to that

- **[`tests/test_bounded.c`](../tests/test_bounded.c)** installs a counting allocator
  and drives a sealed engine, in MANUAL and THREADED mode, through the work path:
  payloads, results, retries, backoff, a deadline, dead letters and their drain,
  eviction, named resources, a constraint that defers, a scheduler hook, cancels,
  settings writes, a full pool and its reuse. It fails on any allocation *or free*
  after the seal. It also covers the edges in the table above. Removing any one of
  the pool, the result buffer, the snapshot slot, the callback records, the fixed
  index, the drain's skip of name copies, or a setup refusal makes it fail.
- **The whole suite with bounded mode forced on.** As an experiment, every engine the
  suite opens without its own `max_items` was made bounded (4096 items, 4 KiB payloads
  and results). Of 74 tests, 50 passed. Each of the 24 that failed did so at a refusal
  this document lists: setup after the first submit, a forked or program task, a
  queue deeper than 4096, or an arena too small for that pool. None failed without
  one.
