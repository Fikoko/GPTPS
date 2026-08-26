# Security posture

GPTPS runs **inside your process**. That single fact sets the whole trust model, so
this document says plainly what is and is not a boundary. It is short on purpose:
an honest small list beats an impressive long one.

## Reporting a vulnerability

Open a [GitHub issue](https://github.com/Fikoko/GPTPS/issues) for anything that is
already public or low-impact. For something exploitable, use GitHub's **private
vulnerability reporting** on the repository's Security tab so a fix can land before
the details do.

## The trust boundary

**Task bodies are trusted.** A `GPTPS_EXEC_INPROC` task is a C function pointer
called on a worker thread in your address space. It can read every byte your process
holds, corrupt the engine, and crash the host. This is not a weakness in the design —
it is what "in-process" means, and it is why INPROC is fast. Do not run code you
distrust this way.

**Config files and add-on paths are code.** `gptps_open(path)` parses TOML, and a
top-level `addons = ["libfoo.so"]` array causes each entry to be `dlopen`ed. A
writable config file is therefore equivalent to arbitrary code execution in your
process. Treat the config path with exactly the care you would treat a shared
library search path: if an attacker can write it, they own the process.

**The engine does not sandbox.** There is no seccomp filter, no namespace, no
capability drop, and no privilege separation anywhere in the core.

## What the out-of-process executors actually buy you

`GPTPS_EXEC_OOP` and `GPTPS_EXEC_PROGRAM` give you **resource isolation and a
guaranteed kill**. That is genuinely valuable and most task systems do not offer it:

- an OS-enforced memory cap (cgroup v2 `memory.max` + `memory.swap.max=0` on Linux,
  `RLIMIT_AS` elsewhere), with an OOM-kill surfaced as `GPTPS_E_NOMEM`;
- a hard `SIGKILL` on the deadline **or** on `gptps_cancel`, bounded even when the
  task declared no timeout;
- a crash that takes down only the child, reported as `GPTPS_E_TASK`, leaving the
  host and every other in-flight item untouched.

They do **not** give you privilege isolation, and this is the part that matters:

- `GPTPS_EXEC_OOP` **forks**. The child inherits a full copy of the host address
  space — every secret, key, and token the parent held — plus its file descriptors
  and its entire environment.
- `GPTPS_EXEC_PROGRAM` forks and execs, so the child does not inherit memory, but it
  **does** inherit the environment and any descriptor not marked close-on-exec. A
  bare `argv[0]` is resolved through the inherited `PATH`.
- Neither drops privileges. A child of a root process is a root process.

**`child_setup` is the seam where you fix that.** It runs in the child after `fork`
and before the work (or the `exec`), so it is where `chdir`, `setuid`/`setgid`,
`setrlimit`, `unsetenv`, closing inherited descriptors, and installing a seccomp
filter belong. It must be async-signal-safe. If you are running code you actually
distrust, `child_setup` is not optional — and you should still put the whole process
inside a container or VM, because a seccomp filter you wrote is not a security
boundary a determined attacker respects.

## Fork safety

An engine created **before** a `fork()` must not be used in the child: its mutex may
be held by a thread that did not survive, so the child would deadlock. GPTPS detects
this and returns `GPTPS_E_SHUTDOWN` from every entry point rather than hanging — and
the check runs *before* the lock is taken, because taking it is itself the hang.
`gptps_shutdown` is included, which matters most: it would otherwise go on to join
dispatcher and worker `pthread_t`s that do not exist in the child.
[`tests/test_fork.c`](../tests/test_fork.c) forks a live threaded engine and asserts
the refusal across mutating, read-only, settings and teardown entry points.

A child that opens its **own** engine after forking is fully supported. The POSIX rule
stands regardless: a forked child of a threaded process should `exec()` or `_exit()`.
(`addons/gptps_xport` forks worker processes, but they run the executor directly and
never open an engine of their own, so it is not an example of this.)

## Signal dispositions

GPTPS never changes a process-wide signal disposition — the host owns those. The
POSIX program executor suppresses `SIGPIPE` per-fd (`F_SETNOSIGPIPE`) or by masking
it on its own thread, never with `signal(SIGPIPE, SIG_IGN)`.

The converse is a requirement on the host: **do not set `SIGCHLD` to `SIG_IGN`, and
do not run a wait-any reaper**, if you use `GPTPS_EXEC_PROGRAM`. Either auto-reaps
the executor's children, so its `waitpid()` returns `ECHILD` and the exit status is
unrecoverable. GPTPS reports `GPTPS_E_TASK` rather than assume success — assuming
success would silently convert every failing program into a passing one. On such a
host every `GPTPS_EXEC_PROGRAM` task therefore fails immediately.
`GPTPS_EXEC_OOP` is unaffected; it carries its status over its own result pipe.

## Denial of service

The engine is bounded by construction, and the bounds are the defence:

| Bound | Setting | Default |
|---|---|---|
| Queued (un-admitted) items | `limits.max_intake_depth` | unbounded — **set this** if submitters are untrusted |
| Concurrently running items | `limits.max_concurrent_tasks` | one per detected core |
| Memory admitted at once | `limits.max_memory_bytes` | ~0.75 × detected RAM |
| Retained dead letters | `limits.max_dead_letters` | 1024, oldest evicted |
| Shutdown drain | `limits.shutdown_grace_ms` | 30s, then in-flight work is cancelled |
| Bytes buffered from a child | *(compile-time)* | 16 MiB per result |

The one default that is deliberately permissive is `max_intake_depth`: unbounded
intake is right for a host that submits its own work and wrong for one that accepts
work from elsewhere. If a submitter can outrun your workers, set it and handle
`GPTPS_E_FULL`.

## Non-guarantees

Stated so nobody infers them:

- **No authentication, no encryption, and nothing GPTPS ships opens a socket.** The
  core has no network code at all. `addons/gptps_xport` uses a `socketpair` to its own
  forked children, and its framing is **not** authenticated; do not put it on a socket
  you do not control.
- `addons/gptps_remote` defines a cross-host **wire format** — and only that. It is a
  codec: it encodes and decodes bytes and opens nothing. It is deliberately **not**
  published as a release artifact, because shipping it is what would create a
  version-1 peer in the world and make the format permanent.
  If you build a transport on it, understand what you are taking on: the `task` field
  of a request is a **dispatch key chosen by the peer**, so on a listening socket that
  is remote code *selection* by anyone who can connect. Run it inside a trusted
  boundary — loopback, WireGuard, a TLS terminator, an SSH tunnel — never on an open
  port. GPTPS will not grow authentication or encryption of its own; that is a
  different product with a different failure model.
- Payloads and results are opaque bytes. Nothing is validated, sanitized, or
  size-checked beyond the caps above.
- The dead-letter list holds original payloads in memory. If those are sensitive,
  drain it.
- Settings persistence (`gptps_settings_save`) writes a world-readable file with the
  process umask.
