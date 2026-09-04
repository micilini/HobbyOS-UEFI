# TASKMAN V1 command reference

Status: RELEASED

## Release commands and aliases

| Command | Aliases | Usage |
|---|---|---|
| `ps` | `tasks`, `tasklist` | `ps [page] [page_size]` |
| `kill` | `terminate`, `taskkill` | `kill <pid>` |
| `taskman` | `tm`, `top` | `taskman [refresh_ms]` |
| `taskdiag` | `td`, `tdiag` | `taskdiag [summary|task <pid>|scheduler|reaper|modal|input|accounting|all|check|selftest|trace <pid>|trace off|trace-status]` |

Aliases and usage strings above are derived from `registry.c`.

## `ps`

Page numbers are one-based. Default page is 1, default page size is 32, and
maximum page size is 128. The command retries a changing registry generation
up to four times. The first invocation shows `--` CPU samples; later calls use
the delta since the prior PS invocation. Pagination prints next/previous hints.

Return 0 means success. Return 1 covers usage, snapshot, sampler, or formatter
failure.

## `kill`

Kill is cooperative, not SIGKILL. It records a request and completes at a
cancellation point or interruptible wait.

| Code | Result |
|---:|---|
| 0 | `ACCEPTED` |
| 2 | `ALREADY_PENDING` |
| 3 | `ALREADY_ZOMBIE` |
| 4 | `ALREADY_EXITING` |
| 64 | `USAGE` |
| 65 | `INVALID_PID` |
| 66 | `PROTECTED` |
| 67 | `NOT_KILLABLE` |
| 68 | `NOT_FOUND` |
| 69 | `INTERNAL_ERROR` |

## `taskman`

Default refresh is 1000 ms; the accepted range is 50 through 2000 ms.
TASKMAN is modal, blocks the shell, and exits only with ESC. Return 0 means the
modal session completed; return 1 means usage/refresh or modal UI failure.
There is no V1 page or kill CLI inside TASKMAN.

## `taskdiag`

Subcommands are `summary`, `task <pid>`, `scheduler`, `reaper`, `modal`,
`input`, `accounting`, `all`, `check`, `selftest`, `trace <pid>`, `trace off`,
and `trace-status`. Inspection is read-only and copy-only. Trace is opt-in and
there is no continuous debug spam by default.

## Metric semantics

`runtime_ns_total` accumulates per task. A view computes `%CPU` from runtime
delta divided by wall delta and stores tenths of a percent (`cpu_x10`). The
first sample and a new task display `--`; ZOMBIE displays `0.0%`; anomaly uses
`!`. One task should not exceed 100%, while an aggregate can reach
CPU-count times 100%. It is a sampling window, not a lifetime average, and PS
and TASKMAN maintain independent windows.

`MEM~ = sizeof(task_t) + kernel_stack_bytes + kernel_ctx_bytes_est`. It is an
estimate of known kernel footprint, not RSS, heap ownership, shared memory, or
device buffers. ZOMBIE retains the estimate until reap.

Canonical KILL values are `PROT`, `NO`, `-`, `PEND`, `EXIT`, and `ZOMB`.
Scheduler state, kill status, and exit reason are separate fields.

## Development-only commands

Development diagnostics include `schedtest`, `synctest`, `accounttest`,
`killtest`, `reaptest`, `inputtest`, `modaltest`, and `taskmantest`.
`tasktest` exists only in a `SELFTEST=1` build and is neither registered nor
accepted by a release build. These commands are test infrastructure, not the
public TASKMAN user interface.
