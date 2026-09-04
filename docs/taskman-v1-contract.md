# TASKMAN V1 public contract

Status: CERTIFIED

This document is the normative public contract of TASKMAN V1. “MUST”, “MUST
NOT”, “SHOULD”, and “MAY” have their usual requirements meaning. Architecture
and implementation details are in [TASKMAN V1 architecture](taskman-v1-architecture.md).

## Identity and terminology

| Term | V1 meaning |
|---|---|
| task | A schedulable kernel task; it is not a POSIX process. |
| PID | UX label for a `task_id_t`, not a POSIX PID. |
| task ID | Monotonic `uint64_t`; zero is invalid and IDs are not reused during one boot. |
| task handle | Task ID plus lifecycle generation; the safe identity for delayed work. |
| lifecycle generation | Generation that prevents a stale handle from naming a different lifetime. |
| wait generation | Generation that binds wake/cancel to one prepared wait. |
| CPU slot | Dense kernel index for a logical CPU. |
| APIC ID | Hardware interrupt-controller identity; it need not equal the CPU slot. |
| current task | Task published as executing in one CPU slot. |
| on-CPU | Task physically executing or participating in an exact switch handoff. |
| runqueue membership | Intrusive membership in one scheduler class queue. |
| cancellation point | A cooperative location where accepted kill becomes task exit. |
| EXITING | Derived status: exit was claimed but the task is not yet `ZOMBIE`. |
| ZOMBIE | Published terminal state, still observable until safe reap. |
| reap | Remove from the registry, claim resources, then free outside the scheduler lock. |
| modal owner | Task handle that owns a modal session. |
| route generation | Generation of a router transition used by a modal token. |
| session generation | Generation of one modal session transaction. |

There is no `process_t`, userspace process, POSIX PID, or PID/TID split in V1.

## Task states

The only values of `task_state_t` are `TASK_READY`, `TASK_RUNNING`,
`TASK_BLOCKED`, `TASK_SLEEPING`, and `TASK_ZOMBIE`. `EXITING` MUST NOT be
treated as another enum value; it is derived from lifecycle fields.

```text
READY <-> RUNNING
RUNNING -> BLOCKED -> READY
RUNNING -> SLEEPING -> READY

READY/RUNNING/BLOCKED/SLEEPING
    -> EXITING
    -> ZOMBIE
    -> REAPED/absent
```

Each CPU MUST own exactly one Idle task. Idle is never a normal public
workload and MUST NOT be killed or reaped.

## Classes and quantum

| Class | Default quantum | Policy |
|---|---:|---|
| `INTERACTIVE` | 5 ticks | Preferred by the V1 two-runqueue policy. |
| `NORMAL` | 20 ticks | Runs when no interactive task is ready. |

`HOBBYOS_SCHED_TEST_QUANTUM` MAY replace both defaults in a test build.
Interactive preference is neither CPU affinity nor POSIX priority.

## Flags

| Flag | Contract |
|---|---|
| `IDLE` | Per-CPU Idle identity; reserved to scheduler bootstrap. |
| `SYSTEM` | Kernel service classification. |
| `USER` | V1 presentation classification only; it is not a real user identity. |
| `KILL_PROTECTED` | Kill requests MUST be rejected. |
| `KILLABLE` | Cooperative kill requests MAY be accepted. |
| `MODAL_UI` | Dedicated modal UI worker classification. |

`SYSTEM` and `USER` MUST NOT be combined. `KILL_PROTECTED` and `KILLABLE`
MUST NOT be combined. Absence of `KILLABLE` means non-killable; it does not
imply protection.

## Lifecycle guarantees

- Publication MUST assign the final ID and lifecycle generation before other
  CPUs can use the task. Future callbacks MUST retain a handle, never a raw
  `task_t *`.
- Kill is cooperative. It MUST NOT forcibly interrupt arbitrary code; an
  accepted request completes at a cancellation point or interruptible wait.
- The first valid exit claim freezes the exit reason. A killed task MUST NOT
  later become `NORMAL`; duplicate exit is invalid state.
- Cleanup ownership MUST be claimed under lock and the callback MUST run
  outside the scheduler lock, exactly once.
- Lifecycle notification MUST publish one copy-only event, exactly once.
  Listener retirement waits for in-flight delivery; listeners never receive
  a raw task pointer.
- Timer references MUST prevent reap. Reaper safety MUST NOT depend on the
  configured grace interval.
- A `ZOMBIE` MUST remain visible through snapshots until registry removal.

## Observation and UI

- Scheduler snapshots MUST be copy-only. TASKMAN and PS MUST NOT retain a
  `task_t *` beyond the scheduler lock.
- TASKMAN and PS MUST use the shared snapshot/formatter contract, while each
  view MUST keep an independent CPU sampler and therefore MAY display
  different sampling windows.
- TASKMAN MUST run as a modal UI. ESC is its only normal exit key. Other
  navigation keys MUST NOT close it.
- Modal owner death MUST recover the route and shell. A failed modal end MUST
  keep the shell paused rather than exposing mixed ownership.

See [lifecycle](task-lifecycle.md), [input/modal ownership](input-modal-architecture.md),
and the [command reference](taskman-v1-command-reference.md).
