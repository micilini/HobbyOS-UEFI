# TASKMAN V1 architecture

Status: CERTIFIED

This page explains how the components satisfy the normative
[V1 contract](taskman-v1-contract.md). Each diagram is followed by a prose
equivalent for non-graphical readers.

## Boot readiness

```text
boot -> CPUs/scheduler -> DPC/input/modal -> PCI/hotplug quiescence
     -> SHELL_READY -> RUNTIME_READY -> optional autorun -> TEST_READY
```

The shell marker means its thread exists. Runtime readiness additionally
requires core services, all expected CPUs, PCI discovery, and initial hotplug
quiescence. `TEST_READY` authorizes a test harness; in release it reports
`autorun=0 selftests=0`.

## Scheduler, runqueues, and handoff

```text
INTERACTIVE queue --+
                    +-> choose next -> per-CPU handoff -> switch.S
NORMAL queue -------+                                 -> finish on new stack
```

The scheduler chooses from the interactive queue first, otherwise normal.
The context-switch sequence is: save IRQ state and pin the CPU; capture CPU
slot, current task, and APIC identity; acquire the scheduler lock; select
`next`; publish a per-CPU sequence/token handoff; let `switch.S` exchange RSP;
finish on the new stack; retire or requeue `prev`; publish current/next; and
unlock. Certified invariants require started equals finished, no pending
handoff, no wrong CPU, no sequence mismatch, no pending overwrite, and no
on-CPU task in a runqueue.

## Wait, wake, and timer reference

```text
object/timer lock -> scheduler lock
prepare wait -> commit block -> wake/cancel by generation -> READY
timer node -> task handle + lifecycle ref -> claim/cancel -> release ref
```

Object state and wait attachment are serialized before scheduler state.
Prepare/commit/abort closes the pre-block lost-wake window. Timer callbacks
hold a generation-bound task reference; publication of a task never grants a
raw pointer lifetime to a future callback.

## Lifecycle and reap

```text
exit claim -> cleanup outside scheduler lock -> copy-only notification
           -> ZOMBIE publication -> safety eligibility -> registry remove
           -> free outside scheduler lock
```

Grace controls policy only. The hard mask described in
[task lifecycle](task-lifecycle.md) controls safety.

## Snapshot, metrics, and views

```text
scheduler registry --copy--> task_snapshot_t[] --+--> shared formatter --> PS
                                                  +--> TASKMAN model/UI
runtime counters ----------> independent sampler-+
```

`scheduler_snapshot_tasks`, `scheduler_snapshot_task_by_id`, and
`scheduler_snapshot_task_by_handle` copy values under synchronization. A
snapshot result includes registry generation and one sample timestamp.
Pagination uses an offset and capacity; callers retry when generation or
capacity changes. Neither UI stores a `task_t *`.

## Input and modal ownership

```text
USB/PS2 -> keyboard queue -> input thread -> router
                                          +-> default queue -> shell
                                          +-> modal queue   -> UI worker
```

Route selection and enqueue are atomic. A modal token binds owner and route
generations. Details are in [input/modal architecture](input-modal-architecture.md).

## Lock order

The validated partial orders are:

```text
object or timer lock -> scheduler lock
modal -> shell -> router -> queue -> scheduler
```

Not every operation acquires every lock, but code MUST NOT reverse an edge.
Input consumers release queue state before acquiring modal/router state.
Cleanup callbacks and resource free run outside the scheduler lock. Per-CPU
handoff state is protected by CPU pinning plus the scheduler lock rather than
being cached across an interrupt-enabled window.
