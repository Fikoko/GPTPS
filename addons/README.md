# GPTPS add-ons

Optional modules built on the public GPTPS API. They are **not** part of the core
library — link only what you use.

There are two flavours of add-on:

- **dlopen add-ons** — shared objects loaded at runtime over the host-table ABI
  (`gptps_load_addon` / `addons = [...]` in config). They export `gptps_addon_init`
  and call the core only through the passed `gptps_api_routines` table. See
  `tests/addon_demo.c` for the minimal shape.
- **compiled-in modules** — plain source you build into your application, using
  the public API and the observer/constraint seams. The durable queue below is one.

## durable_queue — crash-durable submission

`durable_queue.c` / `durable_queue.h`. Submit through `gptps_dq_submit()` instead
of `gptps_submit()`: the `(task, payload)` is written to an append-only journal and
`fsync`'d **before** the task is enqueued, so a crash afterward is recoverable. The
module registers an observer to mark a record complete once its task finishes or is
dead-lettered; `gptps_dq_recover()` re-submits anything a prior run left unfinished.

- **Guarantee:** at-least-once — task bodies must be idempotent.
- **Caveat:** `on_failure = drop` failures emit no terminal event, so they stay in
  the journal and replay on recover.
- **Ordering:** call `gptps_dq_close()` **after** `gptps_shutdown()` (the engine has
  no unregister-observer call, so the queue must outlive event delivery).
- **Portability:** POSIX (uses `fsync` + pthreads).

Build:

```sh
cc -std=c99 gptps.c addons/durable_queue.c yourapp.c -lpthread -ldl
```

```c
gptps_dq *dq = gptps_dq_open(engine, "queue.journal");
gptps_dq_recover(dq);                              /* replay prior-run survivors */
gptps_dq_submit(dq, "resize", buf, len, &handle);  /* durable submit */
/* ... */
gptps_shutdown(engine);
gptps_dq_close(dq);
```
