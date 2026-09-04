# TMV1-CL-10 — Owned modal sessions and independent UI workers

Date: 2026-08-01
Branch: `feat/taskman`
Base: `76d9a35f045322246fd6997226fcf008d79ed0ea`

## Diagnosis

CL-09 made queueing and focus transitions linearizable, but the modal session
still identified its caller only by a diagnostic name.  End and input APIs had
no unforgeable session identity, TASKMAN executed in the shell task, and owner
exit had no lifecycle fallback.

## Ownership and token

`modal_session_token_t` copies the session generation, router generation,
owner task ID, and owner lifecycle generation.  Zero is invalid.  Begin obtains
the current handle internally with `scheduler_current_task_handle`; names never
participate in identity.  End and modal input validate the complete token and
the current caller.  The last closed token supports an idempotent close, while
tokens from older sessions are rejected once a newer session is active.

The official lock order is:

```text
modal session -> shell -> router -> input queue -> scheduler
```

Begin and end retain the session lock through shell/router transitions.
Router refusal rolls begin back to INACTIVE, and end refusal leaves ACTIVE with
the shell paused.  Recovery validates the owner handle, restores the router
before the shell, records the closed token, and is idempotent.

## Token-aware input

The router now exposes entry-copy pop/wait APIs.  Modal input validates the
token before blocking, verifies destination and route generation in the queue
entry, and revalidates the session after removal.  Wrong tokens do not block;
stale entries are discarded and counted.

## Modal UI runtime

`modal_ui_run_sync` allocates a canary-protected context and creates an
INTERACTIVE `SYSTEM|KILLABLE` worker.  The caller blocks on a semaphore.  The
worker owns the modal session, runs the entry function, and reaches cooperative
cancellation points.  Worker cleanup runs UI-specific cleanup while the shell
is still paused, closes or recovers the session, and signals completion once.

A permanent lifecycle listener compares task ID plus lifecycle generation.  It
is a fallback when cleanup deliberately omits close: it restores the session
and signals completion.  The caller waits until the worker is ZOMBIE or absent
before detaching, poisoning, and freeing the context.  A timeout never frees a
context still reachable by a worker or listener; it is quarantined and fails
validation.

## TASKMAN migration

The existing frame rendering, refresh range, and keys were preserved.  The loop
now runs in `taskman-ui`; the shell handler only parses arguments and calls the
synchronous runtime.  Cleanup clears the panel before modal close.  HMP logs
prove distinct shell/UI IDs for both normal and killed runs.

## Files

- `kernel/src/core/modal_session.[ch]`
- `kernel/src/core/modal_ui.[ch]`
- `kernel/src/core/input_router.[ch]`
- `kernel/src/core/scheduler.[ch]`
- `kernel/src/core/kernel_init.c`
- `kernel/src/shell/commands/cmd_taskman.c`
- `kernel/src/shell/commands/cmd_modaltest.[ch]`
- `kernel/src/shell/commands/cmd_inputtest.c`
- `kernel/src/shell/commands/registry.c`
- `scripts/harness-common.sh`, `scripts/test-cl10.sh`
- `makefile`, `AGENTS.md`, development documentation

## Tests and artifacts

The positive SMP matrix completed at 1/2/4/8 CPUs with open/close volumes
250/500/1000/1000.  It covered false/stale tokens, wrong owner, token-aware
input, caller blocking, second-session preservation, killed owner, lifecycle
fallback, allocation/create/router failures, CL-09 boundaries, completion-once,
and the scheduler/sync/account/kill/reap/input invariant checks.

Artifacts:

- `artifacts/build/cl10-smp1.log`
- `artifacts/build/cl10-smp2.log`
- `artifacts/build/cl10-smp4.log`
- `artifacts/build/cl10-smp8.log`
- `artifacts/build/cl10-taskman-normal.log`
- `artifacts/build/cl10-taskman-killed.log`
- `artifacts/build/cl10-negative-stale-token.log`
- `artifacts/build/cl10-negative-owner-recovery.log`
- `artifacts/build/cl10-soak-smp8.log`

The stale-token negative changed the real end classifier and observed an old
token closing a new session.  The owner-recovery negative disabled the normal
lifecycle action, observed ACTIVE with a dead owner, emitted its sentinel, then
used test recovery to leave QEMU clean.  Neither sentinel is macro-only.

`make stack-check` passed with maximum global frame 1920 bytes and maximum CL-10
frame 496 bytes.  CL-owned misleading-indentation warnings are zero; legacy
HPET/XHCI warnings remain outside this phase.

CL-07 completed with `[CL07][MATRIX] PASS smp=1,2,4,8`.  The CL-09 positive
matrix and leakage scenario passed.  The first full CL-08 run found an existing
test-baseline race after TASKMAN cycles: the create-handle test sampled heap
while the last UI worker was still in the reaper backlog.  The test now requires
reaper idle and waits for exact `used_bytes` equality.  The exact heavy
reproducer (three churn/timer/snapshot batches, ten TASKMAN cycles, then 100000
handle creations) passed with `baseline_used=995280`, `final_used=995280`, and
`faults=0`.  The complete CL-08 matrix was not rerun after that final change.

The CL-10 soak ran for 180000 ms and ended with clean modal/input/scheduler/
clock/reaper checks.  It completed only six aggregate cycles, approximately
1500 normal closes and six executions of killed/recovery/busy/failure paths.
This does not meet the requested minimums of 5000 normal closes, 1000 killed
owners, 100 recoveries, and 1000 busy rejections.  The log sentinel therefore
is evidence of a clean 180-second run, not final CL-10 certification.

## Certification record

```text
stack-check: PASS (max 1920 <= 2048)
kernel-check JOBS=2: PASS during development
kernel-check JOBS=nproc: pending final run
reproducible cmp: pending final run
image/nm: pending final run
CL-07 regression: PASS SMP=1/2/4/8, negatives, soak
CL-08 regression: REJECTED first run; exact fixed reproducer PASS; full rerun pending
CL-09 regression: positive SMP=1/2/4/8 and leakage PASS; full all pending
CL-10 soak >=180000 ms: clean duration PASS, required volumes NOT MET
status: REJECTED — no commit created
```

## Reserved for CL-11

No pagination, final layout, refresh-policy, ESC-only behavior, User/Affinity
columns, selection, or interactive kill functionality was added.

## Correção e certificação posterior

A primeira tentativa implementou ownership e worker independente, mas permaneceu
rejeitada por gates incompletos e janelas de lifetime/reserva do runtime.
A certificação final está em:
TMV1-CL-10-FIX-final-certification.md
