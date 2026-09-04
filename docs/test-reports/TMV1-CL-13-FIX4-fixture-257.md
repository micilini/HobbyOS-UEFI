# TMV1-CL-13-FIX4 — real 257-task fixture

Status: `REJECTED_CL13_MATRIX`

The FIX4 fixture and its focused validation passed. The resumed consolidated
CL-13 matrix stopped on a new causal harness contract failure in its first
SMP=4/TCG boot, so quantum, negatives, soaks, final build and commit were not
run.

## 1. Entry and FIX1/FIX2/FIX3 continuity

Date: 2026-08-17. Branch: `feat/taskman`. Base:
`4e700a9b38653fb57ecb0f107a18f87508db0736`. The index was empty and the
legitimate uncommitted CL-13/FIX1/FIX2/FIX3 worktree was preserved. FIX1's
target-specific boundary evidence, FIX2's causal anchor evidence and FIX3's
transactional transport evidence remain approved.

The original `taskmantest setup 257` had a valid CRC and sequence, emitted
`FRAME ACCEPT`, `BEGIN` and `END status=1`, and had zero replay, duplicate
execution, command stall or guest fault. Inspection found `FIXTURE_MAX 129u`;
the entry classification was `REJECTED_CL13_FIXTURE_CAPACITY_MISMATCH`.

## 2. Required growth and memory cost

TASKMAN starts at 128 entries and grows by powers of two up to 4096. A fixture
of 129 crosses 128 to 256; 257 is retained because it crosses 256 to 512.
`task_handle_t` is 16 bytes. The 257-handle BSS array occupies 4112 bytes and
the 257 worker stacks are estimated from `HOBBYOS_KERNEL_STACK_SIZE` as
4210688 bytes.

## 3. Fixture contract and rollback

`TASKMANTEST_FIXTURE_MAX` is defined once as 257. Compile-time assertions bind
it to 257, the second TASKMAN growth boundary and the V1 cap. The handle array
remains in BSS. `taskmantest_fixture_max()` exposes the capacity, while the
coherent `taskmantest_fixture_snapshot()` reports active count, exact handles
present, gate state and cleanup state without holding the scheduler lock.

The setup parser is exact. Zero and values above 257 are range failures;
malformed and overflowing decimal input are parse failures; an existing
fixture is a busy failure with no side effect. Setup stores every valid handle
before incrementing active and waits up to ten guest seconds for every exact
handle to become visible through `scheduler_snapshot_task_by_handle()`.

Creation or visibility failure invokes the shared transactional rollback and
reports both the original cause and rollback result. Cleanup requests
cooperative kill only for exact handles still present, releases the gate,
drives the reaper and waits up to 30 guest seconds for all exact handles to
disappear and `free_inflight` to reach zero. State is cleared only on success;
manual cleanup with no active fixture is an idempotent PASS. The focused run
exercised successful and idempotent cleanup; no forced intermediate creation
failure was injected.

## 4. Capacity, range and busy validation

The pure selftest `ui.taskman.fixture_capacity_257` passed. The capacity
command reported `max=257`, `handle_size=16`, `handle_bytes=4112`,
`stack_bytes_est=4210688`, `taskman_initial=128` and `taskman_cap=4096`.

The SMP=1/TCG range boot rejected `0` and `258` with `reason=range`, and
rejected `abc` and `4294967296` with `reason=parse`; every command returned
framed status 1 and left `active=0`, `present=0`, `free_inflight=0`. The busy
boot created 20 exact handles, rejected a second setup with `reason=busy
active=20`, removed exactly 20, then passed an idempotent cleanup with
`removed=0`.

## 5. Exactly-once 257

On SMP=2/TCG, an intentionally bad CRC for `taskmantest setup 257` was
rejected before dispatch and the empty fixture was verified. The valid frame
then emitted one `ACCEPT`, one `BEGIN`, one setup sentinel with
`created=257 present=257 total=263`, and `END status=0`. Replaying the completed
sequence emitted one `REPLAY`, no second setup sentinel and left exactly 257
active/present handles. Cleanup removed 257 exact handles with
`handles_gone=257 free_inflight=0`; final fixture status was empty.

## 6. Real growth and heap

The SMP=4/TCG `growth257` boot created and observed all 257 fixture handles.
`ps 1 128`, `ps 2 128` and `ps 3 128` wrote 128, 128 and 9 rows. TASKMAN
rendered three WIDE full frames with `pages=3`, `captured=266`, zero fallback,
truncation, render failure and modal scroll. Cleanup removed all 257 handles.
Heap used bytes were 518032 before and after, blocks were 50 before and after,
and both drifts were zero. Final `taskdiag check` reported
`free_inflight=0` and no invariant or structural failure.

## 7. Focused matrix and classification

All focused gates passed: static; range; busy; exactly-once; growth257;
SMP=4/TCG 3/3; SMP=4/KVM 2/2; SMP=8/TCG smoke; and SMP=8/KVM smoke. The
SMP=4 runs retained counts 1/20/50/129/257, refresh variants and 20 anchored
sessions. No focused log contains a guest fault sentinel. The classification
is:

```text
[CL13][FIXTURE257_CLASSIFICATION]
FIXTURE_CAPACITY_MISMATCH_CONFIRMED
```

## 8. Resumed CL-13 matrix failure

The resumed static gate passed stack/build/image checks, and the autorun boot
reported 110 PASS, zero FAIL/SKIP. Hashed UP TCG, SMP=2/TCG and SMP=2/KVM
matrix evidence was reused. The first new SMP=4/TCG boot passed selftests,
fixture diagnostics and subsystem checks through transport sequence 15.

At sequence 16, `schedtest yield 4 20000` emitted `FRAME ACCEPT`, `BEGIN`,
`YIELD_START`, created all four workers and then emitted `END status=0`. The
framed host helper required the asynchronous `YIELD_COMPLETE` payload marker
to precede `END` and immediately classified the otherwise successful handler
return as `GUEST_COMMAND_CONTRACT_FAILURE`. There was no guest FAIL, panic,
exception, command stall or transport rejection. This is a new causal harness
contract failure; it does not contradict fixture257 or FIX3 exactly-once
transport evidence.

The serial evidence is
`artifacts/build/cl13fix4-cl13-smp4-tcg-yield-contract-failed.log`, SHA-256
`9e61dc3f08beb9d304b81650125160e32e13563814ab58775e3e815c93525c93`.
QEMU was stopped. Per the fail-fast rule, the run was not retried or repaired.

## 9. Quantum, negatives, soaks and final build

Quantum 1/7/default, the causal negative matrix, 180-second SMP4/SMP8 soaks
and the final release build were not executed after the matrix rejection.
Consequently no final kernel/image hashes were produced and no commit was
created.

## 10. Source continuity and Git

The protected-runtime manifests before FIX4 and after focused validation are
byte-identical; both manifest files hash to
`d721ea3eeb56fe446d386c1829a6110514eab136e9cfdf28eb5ecc805dba899a`.
The after-tests and after-build manifests do not exist because those gates
were not reached. No protected runtime source was edited by FIX4.

The branch and base remain unchanged, the index remains empty, no commit or
push was created, and `ROADMAP_TASKMAN_V1_CLOSURE_HARDENING.md` remains
untracked. TMV1-CL-14 was not started.

## 11. FIX5 continuation

TMV1-CL-13-FIX5 keeps fixture257 approved and addresses the causal harness
rejection at `schedtest yield 4 20000`. Frame END proved successful return of
the asynchronous launch handler, not worker completion. FIX5 adds explicit
launch and correlated run-ID completion without changing TASKMAN, scheduler,
CRC/sequence/replay or HMP timing. See
`TMV1-CL-13-FIX5-async-command-contract.md`.
