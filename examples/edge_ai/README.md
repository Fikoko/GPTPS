# Edge AI: several models on one board's memory

On a Jetson the CPU and the GPU share one pool of DRAM, so a detector, a segmenter, a
small LLM and a few classifiers all draw on the same memory. Start them together and
their peaks add up to more than the board has, and the kernel's OOM killer ends one of
them — not necessarily the one that grew.

This demo puts GPTPS in front of such jobs. Each job declares its peak memory and how
many units it takes of a resource named `gpu`. GPTPS starts a job only when its peak fits
the memory the board can spare and its units fit the `gpu` budget, and queues the rest.
A job that fails is retried, then set aside as a dead letter; one that runs past its
timeout is killed; one that grows past what it declared is capped on its own, and
reported. The same jobs started all at once without GPTPS are the contrast.

It runs on any Linux machine: the jobs are stand-ins that take real memory, and the board
is a container with a memory limit. Nothing here needs a GPU.

## What is here

| File | What it is |
|---|---|
| `edge_admission.c` | The GPTPS host: about 500 lines of C99 on the public API. It reads a jobs file, registers one `GPTPS_EXEC_PROGRAM` task per job, submits them all, waits until every one has ended, and prints what happened. `--naive` forks every job at once instead, without GPTPS. |
| `fake_infer.c` | A stand-in for a model. It takes N MB over the first quarter of its run, writing every page so the memory is resident, and holds it (`--mb N --ms T`). It can fail its first attempt (`--fail-first FILE`) or keep allocating past N (`--runaway`). |
| `jobs.txt` | A synthetic night for a 1 GB board: eight jobs that declare 1920 MB between them, 2.5 times the 768 MB budget. One of them is flaky and one is a runaway. |
| `jobs.jetson.txt` | A template for real jobs (`trtexec`, `python3`), with `mem_mb` left for you to measure. |
| `run_demo.sh`, `Dockerfile` | Build the demo and run both modes in a container whose memory is the board's. |
| `smoke_test.cmake` | The `example_edge_ai` test: a tiny night, in about 0.2 s. |

### The jobs file

One job per line; `#` starts a comment.

```
# name           mem_mb  gpu  timeout_s  retries  command
detect_cam0         300    2         30        1  fake_infer --mb 280 --ms 2000
```

- `mem_mb`: the job's peak. GPTPS admits it against `--budget-mb`
  (`limits.max_memory_bytes`), and caps the job at it (`default_cost.mem_bytes`).
- `gpu`: the job's units of the resource `gpu`, whose budget is `--gpu-slots`
  (`gptps_define_resource`, `gptps_set_task_resource_cost`). What a unit means is yours
  to decide. Without `--gpu-slots` the column is not enforced.
- `timeout_s`: past it the job is killed, with its whole process group; 0 for none.
- `retries`: attempts after a failed first one. After the last, the job is a dead letter
  (`GPTPS_ON_FAILURE_DEAD_LETTER`).
- `command`: run directly, not by a shell. It is split at spaces, with no quoting or
  redirection, and a bare name is looked up on `PATH`. Its stdin is empty, GPTPS collects
  its stdout (this host discards it), and its stderr is the host's.

`edge_admission` exits 0 if every job finished, 1 if any failed for good, and 2 on a setup
error, such as a bad line or a program that is not there.

## Run it

### In Docker: both modes

```sh
examples/edge_ai/run_demo.sh                  # a 1 GB board: budget 768 MB, 4 gpu slots
examples/edge_ai/run_demo.sh --board-mb 2048  # a 2 GB board: budget 1536 MB
```

It builds the image from the repository root: GPTPS goes in as its two-file
amalgamation, compiled with `cc` on `gcc:13-bookworm`, and the demo runs on
`debian:bookworm-slim`. Then it runs `edge_admission` twice, with
`docker run --rm --init --network none --memory=<board> --memory-swap=<board>`, which
gives the container the board's memory and no swap. The naive run is meant to run out of
memory; the kernel then kills processes in the container, not on your machine.
`--budget-mb` defaults to three quarters of the board, `--gpu-slots` to 4, and
`--jobs FILE` runs another jobs file.

### On this machine, without Docker: GPTPS mode only

```sh
examples/edge_ai/run_demo.sh --host
```

or by hand:

```sh
cmake -S . -B build
cmake --build build --target edge_admission fake_infer
PATH="$PWD/build:$PATH" build/edge_admission --budget-mb 768 --gpu-slots 4 examples/edge_ai/jobs.txt
```

`jobs.txt` names `fake_infer` without a path, hence the `PATH`. GPTPS mode is safe to run
on your machine: what its jobs declare in flight never passes `--budget-mb`, and each job
is capped at its declaration. Where Docker is missing, `run_demo.sh` says so and offers
this instead. Naive mode is not safe, since nothing limits it, and `run_demo.sh` runs it
outside a container only when given `--unsafe-host`.

### On a Jetson

- The synthetic night runs on a Jetson as on any other Linux machine, with the commands
  above; both images exist for arm64. Pass your board's memory as `--board-mb` to make the
  container the same size.
- For your own jobs, start from `jobs.jetson.txt`. Measure each job's peak with
  `tegrastats`, put it in `mem_mb`, set `--budget-mb` to what the board can spare, and read
  the two sections below first: for CUDA jobs, the cgroup mode is not optional.
- To run CUDA jobs in a container, NVIDIA's base image is `nvcr.io/nvidia/l4t-jetpack`,
  with the tag of the board's L4T release. Pass it to `docker build` as both `BUILD_IMAGE`
  and `RUN_IMAGE`, and run with NVIDIA's runtime (`--runtime nvidia`). Inside a default
  container GPTPS has no cgroup it may write to, so it caps with `RLIMIT_AS`, which CUDA
  does not fit (below). We have not tested any of this on a Jetson.

## A real run

One run of each mode, on an x86-64 Linux VM with 4 CPUs and 16 GB (kernel 6.18, cgroup
v1, Docker 29.6.2). The timings change from run to run, and so do the jobs the OOM killer
picks.

### GPTPS mode

On the host, by hand, with the last command above. The `fake_infer:` lines are the jobs'
own stderr.

```
edge_admission: 8 jobs, budget 768 MB, gpu 4 slots, cap: RLIMIT_AS
    time  event   job              in flight (declared)
   0.000  start   detect_cam0       300/768 MB  gpu 2/4
   0.001  start   detect_cam1       600/768 MB  gpu 4/4
   2.015  done    detect_cam0       300/768 MB  gpu 2/4
   2.015  start   segment_yard      660/768 MB  gpu 4/4
   2.015  done    detect_cam1       360/768 MB  gpu 2/4
   2.016  start   classify_plates   480/768 MB  gpu 3/4
   2.018  start   classify_ppe      600/768 MB  gpu 4/4
   3.221  done    classify_plates   480/768 MB  gpu 3/4
   3.221  start   reid_flaky        640/768 MB  gpu 4/4
fake_infer: --fail-first: failing this first attempt on purpose
   3.224  failed  reid_flaky        480/768 MB  gpu 3/4   attempt 1: GPTPS_E_TASK
   3.224  start   track_runaway     580/768 MB  gpu 4/4
   3.228  done    classify_ppe      460/768 MB  gpu 3/4
   3.229  start   reid_flaky        620/768 MB  gpu 4/4   attempt 2
fake_infer: --runaway: allocation refused at 96 MB
   3.793  failed  track_runaway     520/768 MB  gpu 3/4   attempt 1: GPTPS_E_TASK
   3.793  start   track_runaway     620/768 MB  gpu 4/4   attempt 2
   4.237  done    reid_flaky        460/768 MB  gpu 3/4
fake_infer: --runaway: allocation refused at 96 MB
   4.362  failed  track_runaway     360/768 MB  gpu 2/4   attempt 2: GPTPS_E_TASK
   4.362  dead    track_runaway     360/768 MB  gpu 2/4   GPTPS_E_TASK
   4.537  done    segment_yard        0/768 MB  gpu 0/4
   4.537  start   llm_summary       460/768 MB  gpu 3/4
   7.564  done    llm_summary         0/768 MB  gpu 0/4

job                mem MB   gpu  start ms    end ms  tries  result
detect_cam0           300     2         0      2015      1  finished
detect_cam1           300     2         1      2015      1  finished
segment_yard          360     2      2015      4537      1  finished
llm_summary           460     3      4537      7564      1  finished
classify_plates       120     1      2016      3221      1  finished
classify_ppe          120     1      2018      3228      1  finished
reid_flaky            160     1      3221      4237      2  finished after a retry
track_runaway         100     1      3224      4362      2  dead letter: GPTPS_E_TASK

peak declared in flight: 660 MB of a 768 MB budget, gpu 4 of 4
all at once they would declare 1920 MB (2.5x the budget)
7 of 8 jobs finished
```

- "In flight" is what the running jobs declare, as the host counts it from GPTPS's
  events. It never passed 660 of the 768 MB, or 4 gpu units.
- `llm_summary` (460 MB, 3 units) did not fit until 4.5 s, when the segmenter ended.
  Smaller jobs went ahead of it into the room there was, rather than leave it unused.
- `reid_flaky` failed its first attempt on purpose, and finished on its second.
- `track_runaway` declared 100 MB and kept allocating past its 64. GPTPS capped it at
  its 100 MB with `RLIMIT_AS`, as no `GPTPS_CGROUP_PARENT` was set: its allocation was
  refused at 96 MB, it exited, failed its retry the same way, and ended as a dead letter
  with `GPTPS_E_TASK`. No other job was touched. In the cgroup mode the kernel would have
  OOM-killed it inside its own cgroup, and GPTPS would report `GPTPS_E_NOMEM`.
- The exit status was 1: one job failed for good.

### Naive mode

`run_demo.sh`'s naive run, in a container with 1024 MB and no swap.

```
edge_admission --naive: 8 jobs, all at once, no admission
    time  event   job
   0.000  start   detect_cam0
   0.000  start   detect_cam1
   0.000  start   segment_yard
   0.000  start   llm_summary
   0.004  start   classify_plates
   0.004  start   classify_ppe
   0.004  start   reid_flaky
   0.004  start   track_runaway
fake_infer: --fail-first: failing this first attempt on purpose
   0.011  ended   reid_flaky         exit 1
   1.213  done    classify_plates
   1.215  done    classify_ppe
   1.240  ended   segment_yard       killed by SIGKILL
   1.442  ended   llm_summary        killed by SIGKILL
   2.013  done    detect_cam0
   2.016  done    detect_cam1
   6.169  ended   track_runaway      killed by SIGKILL
the kernel OOM-killed 3 processes in this memory cgroup during the run

job                mem MB   gpu  start ms    end ms  tries  result
detect_cam0           300     2         0      2013      1  finished
detect_cam1           300     2         0      2016      1  finished
segment_yard          360     2         0      1240      1  killed by SIGKILL
llm_summary           460     3         0      1442      1  killed by SIGKILL
classify_plates       120     1         4      1213      1  finished
classify_ppe          120     1         4      1215      1  finished
reid_flaky            160     1         4        11      1  exit 1
track_runaway         100     1         4      6169      1  killed by SIGKILL

all at once they declared 1920 MB (2.5x a 768 MB budget) and 13 gpu units
4 of 8 jobs finished
```

- All eight started at once, declaring 1920 MB on a 1024 MB board. Within 1.5 s the
  kernel's OOM killer had ended `segment_yard` and `llm_summary`, neither of which had done
  anything wrong. The runaway went on until it had the board to itself, and then took all
  of it. The count of OOM kills is the container's own (`memory.oom_control` on cgroup v1,
  `memory.events` on v2), read before and after the run.
- `reid_flaky` failed its first attempt, and with no retry it stayed failed.
- 4 of the 8 jobs finished. In GPTPS mode, 7 did: every job except the runaway.

## What GPTPS does not fix

- **A single model too big for the board.** GPTPS decides when a job starts, not how much
  memory it needs. A job that declares more than `--budget-mb` is refused at submit with
  `GPTPS_E_BUDGET` rather than queued forever, but nothing here makes it fit. That takes a
  smaller or quantised model, a smaller batch or input, or a board with more memory.
- **Fragmented GPU memory.** A common report on the NVIDIA forums: tegrastats shows free
  RAM, yet CUDA cannot get a contiguous block large enough. GPTPS counts declared
  megabytes. It does not see how the free memory is laid out, and it cannot put it back
  together. Running fewer jobs at once is not a fix for this, and the demo does not
  claim one.
- **A wrong declaration.** Admission is only as good as `mem_mb`. A job declared below its
  real peak lets too much in at once; the cap then stops that job, and that job alone,
  when it outgrows its declaration, if the cap can see its memory (next section).
- **Memory GPTPS is not told about.** It counts declarations and does not watch free
  memory, so `--budget-mb` must leave out whatever else runs on the board.
- **GPU time.** `gpu` units are a count you choose. GPTPS does not schedule work on the
  GPU, or share it fairly.

## Real CUDA jobs need the cgroup mode

How GPTPS caps a job depends on `GPTPS_CGROUP_PARENT`, as the main
[Readme](../../Readme.md) and `include/gptps.h` describe:

- **Without it**, each PROGRAM job's virtual address space is capped at its `mem_bytes`:
  the child calls `setrlimit(RLIMIT_AS)` before `exec` (`apply_as_cap` in
  [`src/exec_oop_posix.c`](../../src/exec_oop_posix.c); a job that declares less than
  16 MiB gets no cap). That is the cap that stopped `track_runaway` above. It needs no
  privileges, but it counts address space, not memory.
- **CUDA reserves a large range of virtual address space when it initialises**, for
  Unified Virtual Addressing: far more than any job's real memory. A cap on address
  space sized to a job's real peak is below that reservation, so a CUDA job under it fails
  as CUDA starts. Real CUDA jobs need the other mode.
- **With it**, GPTPS gives each job its own cgroup v2 under that directory, with
  `memory.max` set to its `mem_bytes` and `memory.swap.max` to 0, and the job moves itself
  in before `exec`. That caps resident memory, not address space. A job over its cap is
  OOM-killed by the kernel inside its own cgroup, and GPTPS reports `GPTPS_E_NOMEM`. Each
  step is best effort: a job GPTPS cannot place in a cgroup runs under `RLIMIT_AS`.

To set it up:

1. Check that the board runs cgroup v2. Ask the board rather than go by its JetPack
   version:

   ```sh
   stat -fc %T /sys/fs/cgroup             # cgroup2fs: v2. tmpfs: v1 or hybrid, and no cgroup mode
   cat /sys/fs/cgroup/cgroup.controllers  # must list memory
   ```

2. Make a cgroup for the jobs, with the memory controller enabled for its children. It
   must hold no process of its own: the kernel enables a controller for a cgroup's
   children only while it has none (the root cgroup is the exception). As root:

   ```sh
   sudo mkdir /sys/fs/cgroup/gptps
   echo +memory | sudo tee /sys/fs/cgroup/cgroup.subtree_control        # usually on already
   echo +memory | sudo tee /sys/fs/cgroup/gptps/cgroup.subtree_control
   ```

3. Point GPTPS at it:

   ```sh
   sudo GPTPS_CGROUP_PARENT=/sys/fs/cgroup/gptps build/edge_admission --budget-mb <spare> --gpu-slots 2 my_jobs.txt
   ```

   The first line of the output then says `cap: cgroup v2 under /sys/fs/cgroup/gptps`. A
   job GPTPS cannot place falls back without a word, so check while one runs:
   `ls /sys/fs/cgroup/gptps` lists a `gptps.<pid>.<n>` cgroup for each running job.

Without root, the cgroup has to be in a subtree delegated to your user (systemd delegates
`user@<uid>.service`), and the host has to run inside that subtree as well: see
"Delegation" in the kernel's cgroup v2 documentation. `tests/test_cgroup.c` sets up its
parent that way.

One thing we could not check, and you should on your board: whether the memory a CUDA
job's GPU allocations take is charged to the job's cgroup. On a Jetson that memory comes
from the same DRAM, through NVIDIA's driver. If the driver does not charge it to the
cgroup, `memory.max` caps only the job's CPU-side memory, and a job that outgrows its
declaration on the GPU side is not stopped by it. Admission is not affected: it counts
what you declare, and the tegrastats figure you declare covers both sides. To find out,
run one GPU job in the cgroup mode with `mem_mb` below its tegrastats peak but above its
CPU-side peak (`VmHWM` in `/proc/<pid>/status`). If GPTPS reports `GPTPS_E_NOMEM`, the cap
sees the GPU side; if the job finishes, it does not.

## What was tested, and what was not

Tested, on the machine above:

- both modes of the synthetic night in Docker through `run_demo.sh`, and GPTPS mode on the
  host by hand: the runs above;
- `run_demo.sh --host`, and what it says where Docker is missing or not running;
- the `example_edge_ai` test with `-Werror`, under AddressSanitizer with UBSan and under
  ThreadSanitizer, on the simulation and chaos HALs, and 20 times in a row.

Not tested: anything on a Jetson or another arm64 board; real CUDA or TensorRT jobs; the
cgroup mode with these jobs, since that machine has no cgroup v2 memory controller; the
`l4t-jetpack` images.
