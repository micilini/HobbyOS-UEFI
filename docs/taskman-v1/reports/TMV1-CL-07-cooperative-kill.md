# TMV1-CL-07 — Cooperative cancellation and killability

- Data: 2026-07-31
- Branch: `feat/taskman`
- Base: `1d6b81e7d2b8eca9ef0f4090ecf1414bb4c9fab3`
- Status: implementation complete; validation limitations are recorded below.

## 1–4. Diagnosis and contract

The old request only set `kill_pending`, accepted ordinary non-killable threads and had no exit reason or cleanup ownership. CL-07 makes classification immutable by flags: protected requests are refused, legacy kernel-thread wrappers remain non-killable, and only explicitly killable tasks can accept cooperative cancellation. No forced RIP/stack manipulation exists.

## 5–7. Flags, results and request flow

`TASK_FLAG_KILL_PROTECTED` and `TASK_FLAG_KILLABLE` are mutually exclusive at creation. The result enum distinguishes ACCEPTED, ALREADY_PENDING, ALREADY_ZOMBIE, NOT_FOUND, PROTECTED, NOT_KILLABLE and INVALID. `scheduler_request_kill` reads the monotonic timestamp before locking, classifies under the scheduler lock, stores the first timestamp exactly once and wakes an active wait with CANCELLED. Cleanup and ZOMBIE transition never run in the requester.

## 8–11. Cancellation, exit and ownership

`task_cancel_requested` is a side-effect-free query of the current task. `task_cancel_point` exits only the calling killable task with KILLED. `sem_wait` and `timer_sleep` turn CANCELLED into a safe cancellation point; interruptible variants retain observable CANCELLED results. Exit is claimed under the scheduler lock, cleanup runs once in task context outside the lock, and only then is the task transitioned to ZOMBIE. A successful creation transfers `cleanup_ctx` ownership to the task; a failed creation leaves it with the caller.

## 12–14. Production migration and regression adaptation

`smpstress` uses `thread_create_ex` with SYSTEM|KILLABLE and a cleanup callback; manual free/exit was removed and safe checkpoints were added. `synctest cancel` records CANCELLED before cancellation exit. `synctest race` now uses identity/lifecycle/wait-generation wake injection instead of persistent kill, preserving TIMEOUT×CANCELLED arbitration. Idle, input, shell, reaper and DPC remain protected; legacy/accounting/scheduler finite workers remain non-killable unless explicitly opted in.

## 15. Snapshot and UI

Snapshots include `kill_requested_ns`, `exit_started`, `cleanup_done` and `exit_reason`. Shared state formatting emits PROT, NO, -, PEND, EXIT or ZOMB and is used by both `ps` and TASKMAN without layout redesign.

## 16. Files

Core task/scheduler, semaphore and timer wrappers, switch wrapper, kernel init, kill/smpstress/synctest/ps/TASKMAN commands, command registry, new `cmd_killtest.[ch]`, Makefile, AGENTS.md and this report.

## Correção e homologação posterior

A primeira entrega foi rejeitada porque a matriz pós-correção não foi executada e o harness possuía falsos positivos. A certificação posterior está em `TMV1-CL-07-FIX-kill-validation.md`.

## 17–18. Builds and image

- kernel-check JOBS=2: PASS
- kernel-check JOBS=12: PASS
- reproducibility cmp: identical
- kernel SHA-256: `3ce4010654822b5d3db54ded03cd58d34d4d7d07cac71d72933aaaaadc4f746d`
- final image SHA-256: `1b5fd866a1fa94787805374e955cf430868d0c34b99e5cdaf197fd989c081782`
- deps-check: PASS
- unresolved runtime helpers: none
- `thread_wrapper`: calls `thread_exit_normal`; finish-switch/`sti` sequence preserved.

## 19–33. Runtime validation

SMP=1 TCG exercised READY, RUNNING, SLEEPING, BLOCKED, duplicate, non-killable, NORMAL and ZOMBIE with PASS sentinels; cleanup counters remained balanced and duplicate cleanup/exit counters were zero. SMP=4 KVM exposed two harness races (READY stopped checking and ZOMBIE was queried before its locked transition); both were corrected and the final kernel rebuilt reproducibly. The full final SMP=1/2/4/8 matrix, 120-second soak, timeout-race thousands-of-rounds workload, interactive smpstress kill sweep and negative build were not rerun after that final harness-only correction. No panic, #PF, #GP or FATAL appeared in the executed runs.

Parser pure coverage includes empty, sign, overflow and UINT64_MAX. Rename coverage proves an ordinary killable task named `reaper` remains killable; the scheduler no longer derives protection from names.

## 34–37. TASKMAN/ps, soak, negative and gates

UI consumers compile against the shared state helper. Static gates confirm the deprecated API remains only as a compatibility wrapper, smpstress has no manual free/exit pair, detailed results and cleanup fields are present, `cmd_kill` requires exact argc, and negative behavior is compile-guarded. The mandatory final soak and negative artifact are not claimed as executed. The SMP=4 diagnostic serial is retained as `artifacts/build/cl07-smp4-serial.log`.

## 38–41. Regressions, warnings, later phases and final status

CL-01–CL-06-FIX build interfaces and scheduler/accounting code compile unchanged except for the narrow wait-test adaptation. Existing compiler warnings remain non-fatal. CL-08 reference-safe reap, lifecycle notifications and reaper redesign were not started.

Final status: APPROVED WITH VALIDATION NOTES for implementation; formal full-matrix certification remains pending because the complete post-fix QEMU matrix, negative build and soak were not executed in this session.
