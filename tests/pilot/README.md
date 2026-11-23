# Night-window pilot harnesses

Regression tests for the nightly ERP batch-window contract discussed in
[Fikoko/GPTPS#12](https://github.com/Fikoko/GPTPS/issues/12). Passes one to three
are adapted from **nightops00dev**'s night-window harnesses
(<https://github.com/nightops00dev/gptps-night-window-harness>), which a pilot user
wrote against GPTPS 1.9.0 - 1.13.0 and offered under the MIT licence: "MIT, (c)
nightops00dev. Use, copy and fold into your own test suite freely." Thank you.
Pass four is new, written in the same style for 1.14 (ABI 2.11).

Every harness drives the engine through the public API only, prints one
`... : HOLDS` / `... : FAILS` line per finding, and exits nonzero on the first
failure, so CTest runs them unattended.

| CTest name | file | helpers | what it holds |
|---|---|---|---|
| `pilot_pass1` | `pilot_pass1_edf.c` | - | `gptps_sched_edf` starts work in deadline order, not submit order (the big MRP job, submitted first, runs last); `gptps_sched_shed_late` refuses a job already past due at admission (one dead letter, `GPTPS_E_DENIED`, never started); every on-time job finishes. MANUAL, one slot, in-process jobs. |
| `pilot_pass2` | `pilot_pass2_retry.c` | `erp_job.c` | `gptps_retry` rules by exit code: exit 2 -> NEVER (dead-lettered at once, `GPTPS_E_DENIED`, its retries unspent), exit 3 -> a retry at least 300 ms later, exit 1 -> the policy's retries; every failure and every dead letter carries the LAST `stderr_tail_bytes` (TAIL-EVIDENCE); `attempts_run` counts the attempts that ran. |
| `pilot_pass3` (Windows only) | `pilot_pass3_grace_win.c` | `erp_stop_job.c`, `erp_redrive_job.c` | `stop_grace_ms`: a job cancelled mid-run gets Ctrl+Break, rolls back (ROLLBACK-EVIDENCE in its tail), and ends `GPTPS_E_CANCELLED` inside its 2000 ms grace; a licence dead letter re-driven after the licence is renewed finishes `GPTPS_OK`. |
| `pilot_pass4` | `pilot_pass4_shutdown.c` | `erp_job.c`, `erp_redrive_job.c` | The night ends in `gptps_shutdown_ex` with work still queued on a MANUAL one-slot engine: the callback receives the night's own failure (exit 3, its stderr tail, `attempts_run` 1) and every job the shutdown cut off (`GPTPS_E_SHUTDOWN`, `attempts_run` 0, payload and submit options intact) - including the one the last step had already admitted. In the morning, after the licence is renewed, each copied dead letter re-submitted with its own options finishes `GPTPS_OK`. |

The helpers stand in for the ERP's batch executables. `erp_job` reads
`"<ms> <exit_code> <marker>"` from stdin, writes a chatty stderr log ending in a
TAIL-EVIDENCE line, sleeps, and exits with the code (0 ok, 1 runtime/lock, 2 bad
input, 3 licence). `erp_redrive_job` exits 3 until `licence.renewed` exists in its
working directory, then 0. `erp_stop_job` (Windows) works in ticks until Ctrl+Break,
then rolls back and exits.

## How they differ from the originals

- A stderr tail is not NUL-terminated (see `gptps.h`): the originals searched it with
  `strstr()`, which reads past its end; these search within `stderr_tail_len`.
- The helpers are found by the absolute path CMake passes (`ERP_JOB_PATH`, ...), not
  next to the harness. The licence marker lives in a directory of each test's own
  (`PILOT_WORK_DIR`, the licence jobs' `work_dir`), not in the current directory, so
  parallel `ctest` runs never share it. The helpers are built without sanitizers: they
  run under the 64 MB address-space cap their jobs declare.
- Pass one spins on `gptps_now_ms()` (the clock its deadlines are kept on, and a call
  `tests/hal_sim.c` sees) instead of `clock()`, and checks what it prints: every
  on-time job FINISHED, the refusal, the dead letter.
- Pass two's dead-letter check required only a non-empty tail; it now requires the
  dead letter's own tail to carry the evidence, and adds `attempts_run` (ABI 2.11).
- Pass three's waits are bounded (a lost event fails the test instead of hanging it),
  and the re-drive submits the dead letter's own payload and options.
- No `%zu` (older Windows C runtimes), C99, clean under `-Wall -Wextra -Werror`.

## Running one by hand

```sh
cmake -S . -B build && cmake --build build
ctest --test-dir build -R '^pilot_' --output-on-failure
```

CI runs them in every leg; the ones with PROGRAM children (`pilot_pass2`,
`pilot_pass4`) are left out under QEMU (s390x) and run paced on the simulation HAL,
like the other process tests (see `docs/HAL.md`).

## Licence

The adapted files keep nightops00dev's copyright and the MIT licence they were given
under; `pilot_pass4_shutdown.c` is Copyright (c) 2026 Fikoko, MIT, like the rest of
GPTPS.
