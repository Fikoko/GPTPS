# Writing a GPTPS add-on

Everything you need to build, prove and ship one. Start from
[`templates/plugin/`](../templates/plugin) — it is a complete, buildable plug-in.

## 1. Which tier do you want?

One question decides it:

> **Does the host have to call *into* your module?**

| | **compiled-in module** | **binary plug-in** |
|---|---|---|
| answer | **yes** | **no** |
| ships as | source, or `libgptps_x.a` + a header | your own `.so` / `.dll` / `.dylib` |
| links core symbols | yes, directly | **no — only the passed table** |
| added to a host by | recompiling it | naming a path in config |
| may | expose a C API, own threads, own an entire engine, use its own dependencies, ship platform code | register tasks / constraints / observers / a scheduler; define resources, globals and per-task settings; be configured entirely from TOML |
| cannot | be added without a rebuild | be called by the host, own the engine lifecycle, repoint the allocator or log sink, load other plug-ins |

**A module is not the lesser thing.** Six of the eight bundled add-ons are modules *by
necessity*, because their whole point is an API the host calls — `gptps_dq_submit()`,
`gptps_orch_after()`, `gptps_tui_run()`. `gpu_quota` ships in **both** tiers, and the
diff between `addons/gptps_gpu_quota.c` and `addons/gptps_gpu_quota_plugin.c` is the
shortest honest description of the difference.

Choose **plug-in** when you want an operator to add or change policy without
recompiling the host, when you ship on a different release cadence, or when you are a
third party who cannot patch someone's build.

## 2. The whole plug-in

See [`templates/plugin/myplugin.c`](../templates/plugin/myplugin.c). It is ~90 lines,
half of them comment, and every ceremonial-looking line is load-bearing.

## 3. The contract

- **Exactly one exported symbol**, `gptps_addon_init`, produced by `GPTPS_ADDON_INIT`
  or `GPTPS_ADDON_INIT_NS`. That is the entire ABI surface of a plug-in.
- The core validates **magic**, **`abi_version_major`** and **`struct_size`** *before*
  calling a line of your code. A rejected plug-in never runs.
- `setup(e, api, err_out)` runs once, at load. Register what you need and return
  `GPTPS_OK`. Returning anything else **fails the load**.
- **If `setup()` fails, the core unwinds** what you already registered — observers,
  constraints, tasks, the scheduler — but **deliberately does not `dlclose` you**. A
  partial setup can leave pointers the unwind cannot reach (a settings entry's
  read/write pair, a per-task schema), and unmapping would turn each into a wild jump.
  Your code stays mapped. Know that when you decide what to allocate in `setup()`.
- `teardown(e)` runs at `gptps_shutdown`, **after every worker thread is joined**, so
  no task of yours can still be running. It runs whether or not you were disabled.
- **Your code is mapped for the engine's lifetime.** There is no `gptps_unload_addon`
  and there will not be one — for the same reason the unwind refuses to `dlclose`.
  Offer a `disable` hook instead (see §7).

## 4. The host table — guard before you call

The table only ever grows. `api->struct_size` tells you how much of it *this host*
actually has, and reading past that is undefined behaviour.

```c
if (api->struct_size <= offsetof(gptps_api_routines, is_cancelled) || !api->is_cancelled)
    return GPTPS_E_ABI;      /* refuse cleanly; do not half-install */
```

Check the **last** routine you intend to use. Refusing to load beats registering a
task you would not be able to cancel.

**Never call a core function directly.** In a static or amalgamated host the core's
symbols live in the host executable behind its own `gptps_`/`gptps__` namespacing —
which exists precisely so an add-on cannot capture them. Going through the table is
what lets one `.so` work against a static, shared or amalgamated host alike.

Routines by the version that introduced them: v1.0 `register_task`, `emit_event`,
`log`, `result_set`, `payload` · v1.1 `register_constraint`, `register_observer` ·
v1.4 `register_setting` · v1.8 `unregister_task`, `task_exists`, `define_global`,
`define_task_setting` · v1.9 `cancel`, `unregister_constraint`,
`unregister_observer` · v1.10 `define_resource`, `set_task_resource_cost`,
`resource_usage` · v1.12 `set_scheduler` · **2.1** `is_cancelled`, `deadline_ms`,
`now_ms`, `result_set_nocopy`, `task_setting_int`, `task_setting_str`, `submit`,
`submit_ex`, `settings_get`, `settings_set`, `settings_watch`, `set_task_priority`,
`strerror`, `version`, `set_scheduler_ex`.

Deliberately absent, with reasons in the header: engine lifecycle
(`open`/`shutdown`/`step`), the process-wide `set_allocator`/`set_log_sink`,
`load_addon`, the single-slot `set_event_cb`, and the destructive
`dead_letter_drain`.

## 5. Namespaces

Declare `ns` (via `GPTPS_ADDON_INIT_NS`) and you get a guarantee and a rule:

- **Guarantee** — the loader *claims* your token. A second add-on declaring it is
  refused `GPTPS_E_DUP`. Nothing can shadow you.
- **Rule** — every task name, setting key, per-task leaf and resource name you
  register during `setup()` must begin `"<ns>."`, or the registration is refused and
  the reason logged.

Enforced inside `setup()` only, pinned to that thread. It is **attribution, not a
sandbox** — a loaded add-on is code you already trust ([SECURITY.md](SECURITY.md)).

Worth knowing: a duplicate **resource** name is not an error at all, it silently
re-budgets. Two add-ons both defining `"gpu"` would each believe they owned it. A
claimed namespace makes that impossible rather than undetectable.

## 6. Threading, per seam

| Seam | Runs on | Engine lock | May re-enter the engine? |
|---|---|---|---|
| task `run` (INPROC) | a worker thread | **released** | yes |
| constraint | the dispatcher | **HELD** | **no — and must not block** |
| scheduler score | the dispatcher | **HELD** | **no — and must not block** |
| observer | worker / dispatcher / submitter | **released** | **yes** — this is how `orch` builds DAGs |

An in-process task is **cooperative**: if you loop, you must poll
`api->is_cancelled(ctx)`. Without it your task cannot honour a timeout,
`gptps_cancel`, `GPTPS_REMOVE_CANCEL` or `shutdown_grace_ms` — and the engine's
guarantee that `gptps_shutdown` always returns becomes your bug.

Your `malloc` is **not** the engine's allocator seam. `gptps_set_allocator` is
process-wide and the host's; allocate normally and free what you allocate.

## 7. Ownership and disable

The **scheduler seam is single-slot** — an ordering key is a total order, and a total
order has one definition. An add-on must therefore take it *politely*:

```c
st = api->set_scheduler_ex(e, my_score, ud, MYNS, 0);   /* flags 0 = only if free */
if (st != GPTPS_OK) return st;                          /* E_BUSY -> fail setup()  */
```

Failing `setup()` on `GPTPS_E_BUSY` gets the operator a diagnosable "seam already
owned" instead of a policy that silently vanished. If you want to *influence* order
without *owning* it, use the **constraint** seam: `GPTPS_DEFER` with `retry_after_ms`
reorders in time, composes by construction, and is many-per-engine.

A `disable` hook lets an operator turn you off without unloading: hand back the seam,
unregister your tasks and hooks. Nothing is unmapped, so nothing can dangle. Note you
*cannot* unregister settings — there is no such call, which is the same absence that
makes unload impossible. Say plainly what "disabled" means for your module.

## 8. Building

```sh
# POSIX
cc -std=c99 -fPIC -shared -fvisibility=hidden myplugin.c -o myplugin.so \
   $(pkg-config --cflags gptps)          # NOTE: cflags only - never --libs
```

You link **no** core symbols; that is the point. In CMake, take the include path from
the imported target and deliberately do not link it — see the template.

macOS: `-bundle` (or `-dynamiclib`), extension `.dylib` or `.so`, either loads.
Windows: build a `.dll`; `GPTPS_ADDON_EXPORT` already carries `__declspec(dllexport)`.

## 9. Proving it

```sh
gptps_conformance build/myplugin.so
```

Ships in the release and installs to `bin/`. The interesting check is the
**degradation ladder**: it synthesises the host table as each released core actually
had it and runs your `setup()` against every rung, so a routine you called without a
`struct_size` guard is reported by name, with the guard to add — instead of
segfaulting in a user's process a year from now. Then run it under ASan.

## 10. Shipping

Version against `GPTPS_ABI_VERSION_MAJOR`. Within one MAJOR your `.so` keeps working:
the table only grows, `GPTPS_ADDON_MIN_SIZE` never moves, and ABI 2.0 is by design the
last breaking change. A MAJOR bump would require a rebuild — the loader refuses a
mismatch rather than risking it.

Install to `$(pkg-config --variable=plugindir gptps)`.

## 11. Security — read this before you ship

Your plug-in is **code in the host's address space**, with no sandbox: no seccomp, no
namespace, no capability drop. The config file naming it is therefore a capability —
whoever can write it can execute code in that process.

**There is no plug-in search path, and that is deliberate.** GPTPS makes a host name
the exact file rather than scanning a directory, because scanning would widen that
trust boundary quietly. Do not build one on top. See [SECURITY.md](SECURITY.md).
