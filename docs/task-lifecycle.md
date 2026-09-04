# Task lifecycle in TASKMAN V1

Status: CERTIFIED

This is the operational companion to the [public contract](taskman-v1-contract.md).

## Creation and publication

Creation validates class/flag combinations, allocates the task, stack, and
kernel context, installs stack guards, and initializes accounting. Publication
assigns the monotonic ID and lifecycle generation, inserts the task in the
registry/runqueue, and makes a handle available. Once published, code that
outlives the creator MUST use `task_handle_t`, not `task_t *`.

## Ready, running, and wait

READY tasks belong to exactly one class runqueue. RUNNING tasks are current on
one CPU and are absent from runqueues. A blocking object uses prepare, commit,
and abort under `object lock -> scheduler lock`; its wait kind and generation
bind the matching wake. Timer sleep additionally owns a timer-node reference.

## Cancellation and exit claim

Kill records a cooperative request. Interruptible waits can return cancelled;
ordinary execution exits only at a cancellation point. The first valid exit
claim freezes one reason:

| Exit reason | Meaning |
|---|---|
| `NONE` | No exit has been claimed. |
| `NORMAL` | Entry returned or the task exited normally. |
| `KILLED` | A cooperative kill request won. |
| `INIT_FAILURE` | Creation/start initialization could not complete. |
| `INTERNAL_ERROR` | Kernel lifecycle failure path claimed exit. |

`KILLED` cannot be overwritten by `NORMAL`. A duplicate exit claim is invalid
state. `EXITING` is the derived interval after claim and before ZOMBIE; it is
not a `task_state_t` value.

## Cleanup and notification

Cleanup transfers ownership exactly once. A claim occurs under scheduler
synchronization; the callback runs without the scheduler lock so it can
release its owned context safely. Completion precedes lifecycle publication.

Lifecycle delivery is a copy-only event. Each listener has a generation,
in-flight count, and retiring state; unregister waits for in-flight delivery.
Notification occurs exactly once before ZOMBIE publication. A listener never
receives a raw task pointer.

## ZOMBIE, timer references, and reap

ZOMBIE is observable until registry removal. Timer nodes retain the exact
handle/lifecycle reference, and a task is not reaped while a node or reference
can still wake it. Reap eligibility requires all of the following:

- state is ZOMBIE and the task is not Idle;
- exit started, cleanup finished, valid frozen reason, notification finished;
- task is not on a CPU, not current, and absent from every CPU slot;
- no runqueue, wait queue, active wait, or deferred-ready membership;
- no timer references and no earlier reap claim;
- valid lifecycle/accounting timestamps, stack guards, and saved RSP bounds.

The reaper removes the task from the registry and claims resources under the
scheduler lock, then frees them outside it. `scheduler_reap_zombies()` returns
the number claimed in that call; zero does not prove quiescence. Grace time is
policy, never a substitute for the hard safety mask.

## Investigation guidance

Use `taskdiag task <pid>`, `taskdiag scheduler`, and `taskdiag reaper` for
copy-only inspection. A handle still present as ZOMBIE is not itself a leak;
inspect the eligibility mask, on-CPU/queue state, wait state, timer refs,
cleanup, and notification fields.
