# Measurements: what a job actually used

Admission works on declarations. A task's `default_cost.mem_bytes` says how much memory
its jobs need at their peak, a named resource says how many GPU slots or CPU seats, and
GPTPS starts a job only when what it declares fits what is left. GPTPS never measures
anything to decide that, and it never will: admission is predictable because it counts
numbers the host chose.

A declaration is only as good as the measurement behind it, though, and until 1.7 the
host had to make that measurement itself. GPTPS starts every job that runs as a separate
process, and it collects that process when it exits - so the operating system hands
GPTPS, and only GPTPS, what the job actually used. Measurements are how GPTPS passes that
on: each attempt of such a job reports its peak memory, its CPU time and its I/O next to
its declaration, so a host can see "declared 24 GB, peaked at 19.3 GB" and tune the
declaration from evidence instead of a sizing worksheet.

This page is the design record. The rules below bind every measurement GPTPS reports
now and every one added later, by the core, an add-on or a plug-in.

## The rules

1. **Report, never act.** No measurement changes admission, a declaration, a retry or a
   timeout. Acting on a number is policy: the host's, or an add-on's. A core that
   adjusted declarations on its own would stop being predictable, and predictable
   admission is the point.
2. **Measured, or absent.** A value is either what the platform reported or it is not
   there. GPTPS never estimates, extrapolates or fills a gap, and `0` is a real value,
   never "unknown". An attempt that could not be measured carries no measurements.
3. **Every value says how it was measured.** Each one carries its method - for example
   `cgroup.job.resident`, `rusage.largest_process.resident` or
   `jobobject.tree.committed` - because the same name can mean different things on
   different platforms. Consumers that aggregate keep values with different methods
   apart.
4. **Fixed units.** A name has one unit everywhere: bytes, milliseconds, a count, or a
   flag. The executors convert from what each platform reports (Linux's `ru_maxrss` is
   in KiB, macOS's in bytes, Windows' CPU times in 100 ns).
5. **Nothing allocated to report.** Measurements are built on the stack of the thread
   that runs the job and handed to observers for the duration of the callback. They
   exist only for jobs that start a process, which bounded mode does not run, so bounded
   and embedded builds pay nothing.
6. **An open vocabulary.** Measurements are named like settings keys: dotted, lower
   case. The core owns the unprefixed families (`mem.`, `cpu.`, `io.`); an add-on or
   plug-in names its own under its namespace (`gpu_quota.mem_peak`). Anything that does
   not know a name passes it through untouched and otherwise ignores it.
7. **What costs is opt-in.** Measuring at exit costs a system call GPTPS makes anyway, so
   it is always on. Sampling a running job costs a read every interval, so it is off
   until the host sets `measure.sample_ms`.

## The shape

```c
typedef struct {
    const char *name;     /* "mem.peak", "cpu.user_ms", ... or "<addon>.<name>" */
    uint64_t    value;
    uint16_t    unit;     /* gptps_measure_unit: GPTPS_UNIT_BYTES, _MS, _COUNT, _FLAG */
    uint16_t    kind;     /* gptps_measure_kind: how values combine - see below */
    uint32_t    flags;    /* 0 today; ignore bits you do not know */
    const char *method;   /* how it was measured, e.g. "cgroup.job.resident" */
} gptps_measure;
```

`gptps_measure` is frozen: it is the element of an array, so it can never grow. New
information arrives as new names, new units, new kinds or new flag bits, never as new
fields. Its kind says how values of a name combine:

| Kind | Means | Combines by |
|---|---|---|
| `GPTPS_MEASURE_PEAK` | the highest value during the attempt | maximum |
| `GPTPS_MEASURE_TOTAL` | accumulated over the attempt | sum |
| `GPTPS_MEASURE_FLAG` | 1 if it happened at least once | or |
| `GPTPS_MEASURE_CURRENT` | the value at one moment (a sample) | the latest |

**Where they appear.** ABI 2.5 appends two fields to `gptps_event`:

```c
const gptps_measure *measures;    /* NULL when nothing was measured */
size_t               n_measures;
```

- On `GPTPS_EV_FINISHED` and `GPTPS_EV_FAILED` of an attempt that ran as a separate
  process (`GPTPS_EXEC_PROGRAM`, `GPTPS_EXEC_OOP`): what that attempt used.
- On `GPTPS_EV_SAMPLE`, a kind only emitted when sampling is on: values of a running
  attempt at that moment.
- Nowhere else. In-process tasks share the host's memory and threads, so they are never
  measured (rule 2). `DEAD_LETTERED`, `DROPPED` and `RETRIED` report on the item, not
  an attempt, and carry none; the measurements of an item's last attempt are on its
  last `FAILED`.

An event carries at most one value per name. The array and its strings are valid only
during the callback, like `result`: copy what you keep. Read the two fields only when
`ev->struct_size` covers them, or use the lookup, which checks for you:

```c
const gptps_measure *m = gptps_event_measure(ev, GPTPS_M_MEM_PEAK);
if (m) printf("%s: peak %llu bytes (%s)\n", ev->task_name,
              (unsigned long long)m->value, m->method);
```

## The vocabulary

| Name | Unit | Kind | What it is |
|---|---|---|---|
| `mem.peak` | bytes | peak | the attempt's highest memory use |
| `mem.current` | bytes | current | memory in use at the moment of a sample |
| `mem.cap_hit` | flag | flag | 1 if the attempt reached its memory cap (Linux cgroup mode only) |
| `cpu.user_ms` | ms | total | CPU time spent in the job's own code |
| `cpu.sys_ms` | ms | total | CPU time the kernel spent on the job's behalf |
| `io.read_bytes` | bytes | total | bytes the job read |
| `io.write_bytes` | bytes | total | bytes the job wrote |

CPU time divided by run time (`STARTED` to `FINISHED`) is the average number of cores
the attempt kept busy - the check on a declared CPU-seat resource.

### Methods: how each value is measured, by platform

The method names a source, a scope and, for memory, what is counted. The executor picks
the best method the platform offers for each name, and an attempt reports a name only if
some method could measure it.

| Method | Platform | Scope | Measures |
|---|---|---|---|
| `cgroup.job.resident` | Linux, cgroup mode | every process in the job's own cgroup | `mem.peak` (`memory.peak`), `mem.current` (`memory.current`): resident memory charged to the job, page cache included |
| `cgroup.job` | Linux, cgroup mode | the job's cgroup | `cpu.*` (`cpu.stat`), `mem.cap_hit` (`memory.events`: `max` or `oom_kill` above 0) |
| `cgroup.job.block` | Linux, cgroup mode with the io controller | the job's cgroup | `io.*` (`io.stat`): bytes moved to and from block devices |
| `rusage.largest_process.resident` | Linux, macOS, BSD | the job's process and the children it waited for | `mem.peak` (`ru_maxrss`): the resident peak of the largest single process, not their sum |
| `rusage.process` | Linux, macOS, BSD | the job's process and the children it waited for | `cpu.*` (`ru_utime`, `ru_stime`) |
| `rusage.process.block` | Linux | the job's process and the children it waited for | `io.*` (`ru_inblock`, `ru_oublock`, in 512-byte units): block-device I/O |
| `procfs.process.resident` | Linux without a cgroup | the job's main process | `mem.current` (`/proc/<pid>/statm`) |
| `libproc.process.resident` | macOS | the job's main process | `mem.current` (`proc_pidinfo`) |
| `jobobject.tree.committed` | Windows | every process in the job object, which holds the program and everything it starts | `mem.peak` (`PeakJobMemoryUsed`), `mem.current` (summed over the job's processes): **committed** memory, not resident |
| `jobobject.tree` | Windows | the job object | `cpu.*` (basic accounting) |
| `jobobject.tree.all` | Windows | the job object | `io.*` (I/O accounting): every read and write, files, pipes and network alike |
| `process.committed` | Windows, without a job object | the program's own process | `mem.peak` (`PeakPagefileUsage`), `mem.current` (`PrivateUsage`): committed |
| `process` | Windows, without a job object | the program's own process | `cpu.*` (`GetProcessTimes`) |
| `process.all` | Windows, without a job object | the program's own process | `io.*` (`GetProcessIoCounters`) |

Where the cgroup files are missing - an older kernel without `memory.peak`, a parent
without the io controller delegated - the executor falls back to the next method in the
table for that name, and the method says so.

On Windows both the job object and the program's process handle are read, and two facts
that need no guessing decide which is reported: a job contains its program, so a job
figure below the program's own is not a measurement; and every process commits memory,
so a committed peak of 0 is not one either. A figure that fails either check is
replaced by the process's, or left out. (Wine, which does not implement job accounting,
answers the job queries with zeros; this is how that answer stays out of the numbers.)

Notes that matter when reading the numbers:

- **Resident and committed are different numbers.** Linux and macOS report memory
  actually in RAM; Windows reports memory the processes reserved (commit charge), which
  is usually larger. Do not compare them across platforms.
- **The cap and the peak can cover different scopes.** Without a cgroup the cap is
  `RLIMIT_AS` on the job's process (address space, not resident memory), and on Windows
  it is per process; the peak above is per largest process or per tree.
- **`mem.cap_hit` comes from cgroups only.** A cgroup records reaching its cap in
  `memory.events`. `RLIMIT_AS` records nothing, and Windows announces a job's memory
  limit on a completion port whose delivery it documents as not guaranteed - a missed
  message would read as "not hit", so it is not reported (rule 2).
- **An OOP job starts as a copy of the host.** Its process maps the host's memory from
  the fork on, so its `rusage.largest_process.resident` includes host pages it touched.
  In cgroup mode only the pages charged to the job's cgroup count.
- **GPU memory is not measured yet.** On a Jetson the GPU shares DRAM, and whether the
  driver charges a job's GPU allocations to its cgroup is not known (see
  `examples/edge_ai/README.md`). A GPU measurement will be its own name, from the add-on
  that can read it, once it can be read reliably.

## Sampling

Set `measure.sample_ms` (config file `[measure] sample_ms`, or `gptps_settings_set`) to
receive `GPTPS_EV_SAMPLE` events from running process jobs at that interval. `0`, the
default, turns sampling off. A value below 10 samples every 10 ms; the executors also
wake every 200 ms anyway, so intervals up to that cost one extra read per interval per
running job. A change applies to attempts that start after it.

A sample carries `mem.current` (kind `current`) with the method the platform offers: the
job's cgroup, its main process (`/proc` on Linux, `proc_pidinfo` on macOS), or on
Windows the job object's processes summed, else the program's own. A process that has
exited but not been collected yet has nothing to sample, and is not sampled.
Samples are emitted by the thread running the job, between that attempt's `STARTED` and
its `FINISHED` or `FAILED`, so they arrive in order with them; an observer that blocks
delays the job's executor, as with every other event. Observers that do not know
`GPTPS_EV_SAMPLE` must ignore it: an engine never emits it unless the host turned
sampling on.

## Aggregation: gptps_stats

`gptps_stats` folds measurements into rows per task type and for the engine as a whole,
keyed by name **and method** (rule 3). Each row keeps the number of attempts that
reported it, sum, minimum, maximum, the last value and when it arrived:

```c
gptps_stats_measure m;
m.struct_size = sizeof m;
if (gptps_stats_measure_get(s, "mrp", "mem.peak", "cgroup.job.resident", &m) == GPTPS_OK)
    printf("mrp: %llu attempts, peak %llu, mean %llu\n", (unsigned long long)m.count,
           (unsigned long long)m.max, (unsigned long long)(m.sum / m.count));
```

Memory is fixed by the vocabulary, not by the number of jobs: no individual value is
kept. A task type holds at most `GPTPS_STATS_MEASURE_ROWS` rows; a value that would need
another - or whose name or method is too long to key, or that comes with a different
unit or kind than its row - is counted in the counters' `measures_dropped`, never
dropped silently. `gptps_stats_measure_count` and `gptps_stats_measure_at` enumerate
the rows. `gptps_stats_measure_merge` folds rows from several engines - counts and sums
add, minimum and maximum take the extreme, the last value is the later one - so shards
of a `gptps_pool` merge in any order.

Measurements that arrive other than as an engine's events - a `gptps_xport` reply's -
fold in with `gptps_stats_measure_fold`, and a process that keeps no engine of its own
gets a stats object for them from `gptps_stats_open()`.

The dashboard (`gptps_tui`) shows each task type's highest measured peak in its tasks
table, as "peak MB".

Comparing nights, spotting a peak that creeps up 5% a night, alerting: all of that needs
history, and history belongs to the host, which keeps its own ledger.

## Across shards and processes

- **`gptps_pool`:** each shard is an engine with its own events; install `gptps_stats`
  on each shard and merge.
- **`gptps_balance`:** forwards each shard's events with their measurements.
- **`gptps_xport`:** a worker process runs its own engine, so its measurements are taken
  there. The reply carries the measurements of the item's last attempt back to the
  parent - the FINISHED attempt's, or the last FAILED one's before a dead letter -
  unknown names included: `gptps_xport_submit_ex` (free with
  `gptps_xport_result_free`) and `gptps_xport_submit_async_ex`. At most
  `GPTPS_XPORT_MAX_MEASURES` travel with one reply, and `measures_cut` says when an item
  reported more.
- **`gptps_remote`:** the version 1 wire format's reply has no extension slot, by
  design (see `gptps_remote.h`). Measurements will travel in version 2's tagged payload,
  which arrives with the first network transport - not before, as that header asks.

## Adding a measurement

1. Pick the name. A core measurement extends a family (`mem.`, `cpu.`, `io.`) or starts
   one; an add-on's goes under its own namespace and needs no change to the core.
2. Fix its unit and kind; they never change afterwards.
3. Name the method for every platform that produces it: source, scope and, for memory,
   what is counted. A platform that cannot measure it reports nothing.
4. Add its row to the vocabulary table and its methods to the method table here.
5. Test it where it is produced: a job of known size or known work, checked for the
   value's range and its method, on each platform in CI that produces it.

No step touches `gptps_measure`, `gptps_event`, the stats add-on or the transports: they
carry any name.
