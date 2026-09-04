# Input and modal ownership architecture

Status: CERTIFIED

## Data path

```text
PS/2 IRQ ----+
             +-> keyboard queue (512) -> input thread -> router
xHCI HID ----+                                  |-> default queue (128) -> shell
                                                `-> modal queue (128)   -> worker
```

The keyboard queue decouples hardware producers from the interactive input
thread. The router chooses a destination and enqueues atomically; a consumer
never acquires router/modal locks while holding a queue lock.

## Queue contract

`count` is the source of truth; storage capacity is supplied externally. Each
enqueue advances a monotonic sequence. Consumers use wait-pop or try-pop, and
drain removes residual events. Full queues count drops. There is no parallel
semaphore whose permits can diverge, so an empty queue cannot expose a phantom
permit. Current capacities are 512 keyboard events and 128 events in each
default/modal route queue.

## Router transaction

The router states are `DEFAULT`, `OPENING`, `MODAL`, and `CLOSING`. Begin
drains typed-ahead/default residue before making the modal route active. End
drains the modal tail before returning to default. Route generation changes
bind the transition; route mismatch is a contract violation. The transition
follows `modal -> shell -> router -> queue -> scheduler` where those locks are
needed.

## Modal session token

A token contains session generation, route generation, owner task ID, and
owner lifecycle generation. Begin is transactional. End and input are
token-aware: stale tokens and wrong owners are rejected. Owner close is
idempotent. If route end fails, the shell remains paused rather than sharing
input with a half-closed session.

Owner death invokes lifecycle recovery. Primary UI cleanup closes the session;
the lifecycle listener is the fallback. Both paths identify ownership by the
exact handle, never by task name.

## Modal UI worker

The UI runs in an independent INTERACTIVE worker marked `SYSTEM|KILLABLE` and
`MODAL_UI`. The caller waits on a completion semaphore. Completion publication
and its signal occur once. The worker context remains owned until cleanup;
cleanup precedes modal close. Caller cancellation tears down the worker before
the caller reaches a cancellation point. Quarantine is diagnostic for a
context that cannot yet be safely freed and MUST return to zero in a clean
session.

TASKMAN builds on this contract. Its normal exit is ESC, after which cleanup
restores the console region, closes ownership, signals completion, and resumes
the shell.
