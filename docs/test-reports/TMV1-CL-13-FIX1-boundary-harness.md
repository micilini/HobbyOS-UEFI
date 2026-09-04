# TMV1-CL-13-FIX1 — target-specific semaphore boundary harness

Date: 2026-08-16
Branch: `feat/taskman`
Base/HEAD: `4e700a9b38653fb57ecb0f107a18f87508db0736`
Status: `REJECTED_CL13_MATRIX`

## 1. Worktree continuity

The legitimate, uncommitted CL-13 implementation present at entry was
preserved. The index remained empty, the roadmap remained untracked, no push
was performed, and no CL-14 work was started. The FIX1 changes are still in
the worktree because the consolidated CL-13 matrix did not complete.

## 2. Original fault

The prior SMP=2/KVM run stopped in `synctest boundary 5000` with 59 completed
waits, 60 successful boolean signals and `lost=1`. It reported no semaphore
error, panic, exception, structural fault or scheduler invariant failure. The
original serial evidence is preserved as
`artifacts/build/cl13-matrix-smp2-kvm-failed.log` (SHA-256
`cbfe895e2ad2389de8809831f312ed314ed4529c5c198d22456a7feff59a69d6`).

## 3. Baseline hashes and historical evidence

At base, `semaphore.c` hashes to
`bdbfef811471586a0a728d4f0fe7b98dd513aa0b5f56bddc9bb72949e67e2f77`
and `cmd_synctest.c` hashes to
`aa5917d60a4a574cc6fa239c2296188341b1ac358fb91b869ef351ccc71ac2ed`.
Historical certification had already passed boundary workloads at 10,000
rounds on SMP=2 and SMP=4, and 20,000 rounds on SMP=8; CL-13 SMP=2/TCG also
passed 5,000 rounds before the ambiguous KVM result.

## 4. Legacy global hook defect

The old callback was global and did not filter by semaphore, task identity,
lifecycle or wait generation. The callback and context were published and
cleared separately, and a single shared phase value had no round identity.
Consequently a foreign semaphore wait could publish progress for the target
test.

## 5. DPC and HMP/xHCI cross-talk

`dpc-worker` is a permanent interactive task which waits on its own semaphore.
HMP `sendkey` reaches the xHCI keyboard path and schedules DPC work. Under KVM
timing, that foreign wait could invoke the former global hook while the
boundary test was armed. This explains why the old boolean signal count did
not prove that an exact target wake was lost.

## 6. Callback/context pair race

Observer state is now published under a dedicated spinlock. Arm publishes the
complete target/hook/context tuple before setting it active. Clear first
deactivates it, advances its generation, then waits boundedly for
`callback_inflight == 0` before the context may be reused.

## 7. Target-specific observer

The observer matches both the semaphore address and the exact task handle
(task ID plus lifecycle generation). The positive event is produced only
after `scheduler_prepare_block_interruptible()` returns prepared and includes
the token's wait generation and wait kind. The old-gap negative emits a
target-filtered pre-prepare event with wait generation zero. Callbacks run
outside the observer lock; target, foreign, total and in-flight calls remain
observable through a copy-only snapshot.

## 8. Signal disposition

`sem_signal_detailed()` distinguishes `WOKE_WAITER`, `ADDED_PERMIT`, overflow,
fatal and invalid outcomes without changing the production semantics of the
existing `sem_signal()` wrapper. Boundary success requires an exact
`WOKE_WAITER`; an added permit is a distinct harness failure rather than a
lost-wake claim. `semaphore_debug_snapshot()` reports count and waiter count
under the semaphore lock without exposing queue pointers.

## 9. Monotonic boundary protocol

The shared 0/1/2/3 phase was replaced with 1-based monotonic waiter, prepared,
signaled, completed and released rounds. The hook validates the exact handle,
semaphore, wait kind, strictly advancing wait generation and expected round.
The controller releases each round only after observing completion. The
waiter is created by exact handle with a reap hold and cannot enter its first
wait until the observer is armed and the startup gate is released.

## 10. Watchdogs and diagnostics

The harness distinguishes a 5,000 ms no-progress prepare/completion stall from
the 300,000 ms total throughput limit. A frozen failure observation records
round counters, signal disposition, observer counters, semaphore count and
waiters, exact task state/CPU/queue/wait generation and elapsed/stalled time.
Timeout is no longer printed as `lost=1`; a confirmed lost wake requires the
exact target and wait-generation evidence specified by FIX1.

## 11. Exact cleanup

All success and failure paths publish stop/start/release, wake or cancel the
exact waiter as needed, clear and drain the observer, wait for the held zombie
or absence, release the reap hold, drive the reaper, require the handle gone
and `free_inflight == 0`, and finally require zero semaphore waiters plus an
inactive observer with zero in-flight callbacks before releasing the global
synctest workspace.

## 12. Foreign semaphore noise

`synctest boundary-noise <rounds> <foreign_rounds>` runs real waits and signals
on a second semaphore concurrently with the target. Every focused run
observed foreign hits while retaining exact target hit counts, zero target
permits and clean residual state. This controlled canary reproduces the class
of interference caused by the DPC semaphore without changing the DPC
subsystem.

## 13. Observer selftest

The bounded `sync.semaphore.observer.atomic_config` selftest validates coherent
arm, snapshot and clear behavior, monotonic observer generation, inactive
post-clear state and zero in-flight callbacks. It passed in the 100-case
selftest suite.

## 14. Old-gap negative

The isolated `HOBBYOS_SYNC_NEGATIVE_OLD_SEM_GAP` build observed the exact
target hook before prepare, `ADDED_PERMIT` with no attached waiter, emitted
`[SYNC][NEGATIVE] OLD_SEM_GAP_DETECTED`, cleaned up, rebuilt normal, and passed
both `boundary 1000` and `boundary-noise 1000 1000` canaries.

## 15. Focused static/build gate

The focused static gate passed release j2/jN reproducibility, selftest autorun
build, image, undefined-symbol and source searches. Stack usage passed with a
maximum frame of 1,984 bytes (limit 2,048). The current semaphore and
synctest source hashes are respectively
`06969e56b74bb4922def7e912175d996fe7aad991e0855f7c61ce15fe9bb2f51`
and `07ce46a3d257700cefdc7cf9e8e7789cdda17bf1a736307ef5f7ecd81a31ab83`.

## 16. Focused matrix

| Scenario | Result |
|---|---|
| SMP=1 TCG, three boots | 3/3 PASS |
| SMP=2 TCG, three boots | 3/3 PASS |
| SMP=2 KVM, five boots | 5/5 PASS |
| SMP=4 KVM, three boots | 3/3 PASS |
| Extra boundary-noise canary | PASS |
| Isolated old-gap negative and positive rebuild canaries | PASS |

Every boot passed target boundary, boundary-noise, `synctest check` and
`taskdiag check`; target signals added no permits and observer cleanup left no
residual. No exact-target stall reproduced.

## 17. Classification

The focused ledger records:

```text
[CL13][BOUNDARY_CLASSIFICATION] HARNESS_FALSE_POSITIVE_CONFIRMED
```

Foreign hits were demonstrated while callbacks remained isolated to the exact
target, the causal old-gap negative was detected, and SMP=2/KVM passed 5/5.
The original `lost=1` therefore did not establish a production lost wake.

## 18. CL-13 resumption

After the focused classification, consolidated static and selftest gates
passed. Autorun and explicit `tasktest all` each reported 100 PASS, zero FAIL,
zero SKIP and zero P0/P1 failures. The resumed forced matrix passed UP TCG,
SMP=2 TCG and SMP=2 KVM. SMP=4 TCG then timed out after 900 host seconds in
the count=20 `taskman 1000` auto-exit session, after the count=1 session had
passed and after scheduler, sync, accounting, kill, reaper, input and modal
scenario checks had passed.

The failure observation ended with the modal session ACTIVE and the shell
BLOCKED. It contained no `AUTO_EXIT` sentinel, panic, #PF, #GP, fatal marker or
guest-side FAIL. The harness classified it as
`[HMP][TIMEOUT] SHELL_INPUT_STALL command=taskman 1000`, stopped QEMU and
preserved:

- `artifacts/build/cl13-matrix-smp4-tcg-taskman-timeout-900.log`
  (`5d6e309a590ff6b6e9c578455c0edcbe9de64df349ed850151f274adce195ec6`)
- `artifacts/build/cl13-matrix-smp4-tcg-taskman-timeout-900-debugcon.log`
  (`e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`)
- `artifacts/build/cl13-matrix-smp4-tcg-taskman-timeout-900-trace.log`
  (`74403ad1102f48b5ca67ed86de59c52f9f510845455e463efb2fe6d07cc800`)

This is a CL-13 matrix failure outside the corrected boundary harness. It was
not retried, no runtime correction was attempted, and the later matrix,
quantum, negative, soak and final-build stages were not run.

## 19. Additional test-wrapper observation

An earlier SMP=4/TCG attempt exposed an accounting lifecycle test observation
race: the test printed removal failure immediately before the exact task was
freed. The CL-13 test wrapper now tracks the exact handle and actively drives
the already-certified reaper until the handle is absent and
`free_inflight == 0`. A clean canary passed three times. No reaper or scheduler
production policy was changed.

## 20. Toolchain and warnings

The run used GCC 13.3.0 and QEMU 8.2.2. Existing non-CL-owned linker warnings
about GNU-stack/RWX and existing xHCI warnings remained visible; the owned
sources built without a new warning. The final release build and hashes were
not generated because fail-fast stopped at the matrix.

## 21. Git

HEAD remains the base commit, with zero commits after it. The index is empty,
there is no push, and no CL-13 commit was created. The uncommitted CL-13/FIX1
implementation and artifacts remain available for diagnosis. No QEMU process
remains active.

## 22. Final status

`REJECTED_CL13_MATRIX`

The FIX1 boundary goal itself is causally validated and classified as a
harness false positive. The overall CL-13 certification remains rejected by
the later SMP=4/TCG TASKMAN matrix timeout. TMV1-CL-14 remains reserved.

## 23. FIX2 addendum

TMV1-CL-13-FIX2 subsequently proved that the 900-second TASKMAN timeout was
caused by an unreset console anchor: four fixed `ps 1 128` snapshots produced
`TOO_SHORT`, zero full frames, 22 fallback frames and one shortfall, while the
same degradation followed by `taskmantest anchor-reset` produced three WIDE
full frames and zero fallback. Runtime remained byte-identical. The FIX2
focused variants then stopped fail-fast on an HMP/xHCI transport loss before
`taskmantest setup 257`; see `TMV1-CL-13-FIX2-taskman-anchor.md`.

## 24. FIX3 continuity addendum

FIX3 does not reopen the target-specific semaphore observer or boundary
protocol. Its protected-runtime manifest excludes only the SELFTEST shell
dispatcher, `cmd_tasktest` transport and selftest registry files; scheduler,
semaphore and TASKMAN runtime remain protected. Transactional HMP evidence is
documented in `TMV1-CL-13-FIX3-transactional-transport.md`.
