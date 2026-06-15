# General Purpose Task Processing System

## Overview

This project implements a **general purpose task processing system** that listens for incoming tasks from a server/user service,

queues them, and executes them based on performance constraints. The system supports both **sequential** and **non-sequential** task execution,

tracks analytics, recovers from failures, and allows manual or automatic configuration of the environment.

---

## Features

- **Automatic hardware detection**

  Detects CPU, RAM, and GPU to apply pre-defined performance patterns.

- **Flexible task execution**

  - **Sequential execution**: One task at a time (`max_concurrent_tasks = 1`)

  - **Non-sequential execution**: May do multiple tasks (if assigned) concurrently, parallel or non-parallel (`max_concurrent_tasks > 1`)

- **Performance control**  

  Limits that cap the live workload:

  - `max_concurrent_tasks` — maximum tasks running at once

  - `max_memory_gb` — shared memory ceiling

- **Failure handling**  

  Per-task policy (a default for all tasks, overridable per task type) for when a task

  times out, errors, or dies — so a queue-fed TPS never silently loses work:

  - `timeout_seconds` — time limit before a task counts as failed

  - `max_retries` — attempts before giving up

  - `retry_backoff_seconds` — wait between attempts

  - `on_failure` — `requeue`, `drop`, or `dead_letter`

- **Task cost declaration**  

  Each task type declares roughly what it needs (memory, GPU, expected duration), so the

  core can check whether a task fits the remaining budget before starting it — rather than

  treating the memory ceiling as an all-or-nothing cap.

- **Task analytics**  

  Tracks task start and end times, memory usage, and execution statistics via the `Tracker` module.

- **Detector integration**

  - Listens for messages from the server service

  - Filters messages based on worker/group ID

  - Queues relevant tasks for execution

- **Manual or auto configuration**

  - Auto-fill based on detected hardware

  - Manual override for tasks, paths, and IDs

  - Supports defining multiple tasks per hardware pattern

---

## Configuration

Configuration separates worker-wide **limits** from per-task **defaults**, and lets any

task type override those defaults. The keys carry no `task_` prefix — an override block

reuses the exact same keys as `task_defaults`, so overriding a setting is just copying a

key up one level.

Anything more specific is a **constraint add-on** rather than a global setting

(see Getting Started).

---

## Getting Started

The GPTPS is written in C99, mainly for portability across most hardware. It uses a

plugin-like system where add-ons — libraries or applications — provide the logic for

the different tasks it can process. These add-ons do **not** have to be written in C.

An add-on attaches to the core in one of two ways:

- **As a library** — loaded directly by the core and run in-process. This is the fast

  path, usable from any language that can expose a C-compatible interface.

- **As an application** — run as its own process that talks to the core over a message protocol. This path has no language restriction at all, so an add-on

  can be written in anything else.

Each add-on declares what it is and what it does; the core only loads the ones named in

the active configuration, so the same build can carry many add-ons while each environment

enables just the ones it needs.

The global configuration is kept intentionally small. Anything more specific — GPU quotas,

rate limits, task priority, time-of-day windows — is expressed as a **constraint add-on**,

so the core stays minimal while the variety lives in add-ons. (GPUs are picked up during

hardware detection but have no global knob; a GPU constraint is the natural first add-on to write.)


