# TMV1-CL-13-FIX5 — asynchronous command contract

Status: `REJECTED_CL13_FIX5_HARNESS`

## 1. Entry and continuity

Date: 2026-08-17. Branch: `feat/taskman`. Base:
`4e700a9b38653fb57ecb0f107a18f87508db0736`. FIX1 boundary observation, FIX2
TASKMAN anchoring, FIX3 transactional framing and FIX4 fixture257 remain
technically approved and are reused by artifact hash.

The original resumed command was `schedtest yield 4 20000`. It emitted FRAME
ACCEPT, BEGIN, YIELD_START, created four workers and emitted END status 0.
YIELD_COMPLETE is emitted by the last worker, but the synchronous wrapper
rejected the frame immediately because that asynchronous marker had not yet
appeared. QEMU was then stopped before completion. There was no guest failure,
panic, exception, transport rejection, command stall or scheduler violation.
Entry classification: `REJECTED_CL13_ASYNC_COMMAND_CONTRACT`.

## 2. Explicit command contracts

| Command | Contract | Validation |
|---|---|---|
| `schedtest yield` | ASYNC | `async-wait` for exact run |
| `schedtest cpu-pin` | ASYNC | `async-wait` for exact run |
| `schedtest entry-window` | ASYNC | `async-wait` or negative marker |
| `smpstress` | LAUNCH_ONLY | spawn marker, then kill sweep |
| `taskmantest churn-start` | LAUNCH_ONLY | churn-status and churn-stop |
| `taskman` | MODAL | framed start, raw keys, framed finish |
| remaining CL-13 commands | SYNC | functional marker before END |

Scripts select these contracts explicitly; runtime command names do not infer
them and the strict synchronous helper is not relaxed.

## 3. Scheduler async state

Every accepted workload obtains a nonzero monotonic run ID. Worker contexts
carry that run, and stale contexts cannot complete a later run. Copy-only
snapshots expose kind, state, workers, iterations, remaining workers,
migrations, mismatches and scheduler violations.

The last worker publishes PASS/FAIL, completed run, remaining zero and
`g_running=0` before emitting its completion marker. Partial creation enters
FAIL, clears running, accounts only actually created residual workers and
prevents context reuse until they drain. `schedtest async-status [run]` is
read-only. `schedtest async-wait <run> [timeout_ms]` synchronously waits for
the exact run and emits PASS before its own frame END.

## 4. Host helpers and marker order

`framed_send_complete` stays SYNC-strict. `framed_send_launch` validates
ACCEPT/BEGIN/START/END without assuming completion.
`framed_schedtest_async` extracts the START run, submits exact `async-wait`,
checks completion for that run and records COMPLETE_BEFORE_END or
END_BEFORE_COMPLETE. The negative helper waits for its causal marker without
resending after BEGIN. Missing completion is
`GUEST_ASYNC_COMPLETION_TIMEOUT`; guest faults remain distinct.

## 5. Static and host-unit validation

The static gate passed stack-check, SELFTEST j2/jN reproducibility, image and
undefined-symbol checks. The largest kernel frame was 1984 bytes, within the
2048-byte limit, and the FIX5-owned units emitted no warning. Autorun contains
116 tests, including all six scheduler async cases, with zero FAIL/SKIP.

The host-only unit passed all five synthetic order cases: correct SYNC,
late-marker SYNC rejection, ASYNC completion before END, ASYNC completion
after END, and missing ASYNC completion classified as
`GUEST_ASYNC_COMPLETION_TIMEOUT`. Thus COMPLETE_BEFORE_END is covered by the
pure order parser and END_BEFORE_COMPLETE by real guests.

## 6. Focused yield matrix

The real yield boots passed SMP1/TCG 3/3, SMP2/TCG 3/3, SMP2/KVM 3/3,
SMP4/TCG 5/5, SMP4/KVM 3/3 and SMP8/TCG 2/2. SMP8/KVM passed its first boot.
All 20 completed real boots observed END_BEFORE_COMPLETE, exact nonzero runs,
launch status 0, async-wait status 0, remaining zero, balanced transport and
clean scheduler/task diagnostics. The SMP1 boots also proved monotonic runs 1
then 2 in one guest.

The second SMP8/KVM boot completed autorun with 116 PASS, zero FAIL/SKIP and
reached SHELL_READY, but hotplug/xHCI output interleaved the exact selftest
line used as a redundant host gate. Instead of one contiguous line ending in
`severity=P0`, the log contained:

```text
[SELFTEST][PASS] scheduler.async.completed_before_marker_contract severity=[HP] Port Enabled! Configuring device...
P0[XHCI] Disabling slot 1...
```

The overall `[SELFTEST][SUMMARY] pass=116 fail=0` and `[SELFTEST][AUTORUN]
PASS` remained intact. The harness nevertheless aborted before sending any
frame, recorded the boot as FAIL and stopped QEMU. This is a focused harness
matcher failure, not an async state, scheduler, transport or guest failure.
Per fail-fast policy it was not corrected or retried. Classification:
`REJECTED_CL13_FIX5_HARNESS`.

## 7. Unreached gates and continuity

The cpu-pin/entry-window focused boots, stale-slot negative, successful
focused classification, resumed CL-13 matrix, quantum, full negatives, both
soaks and final build were not executed. No commit or push was created and
TMV1-CL-14 was not started.

The protected-runtime state still compares byte-for-byte with the FIX5 before
manifest when excluding only `cmd_schedtest.c`, `cmd_schedtest.h` and
`selftest.c`. The formal after-focused/after-tests/after-build manifests were
not generated because their gates were not reached.

Operational estimate: best case FIX5 closes CL-13; likely outcome is FIX5 and
at most one additional causal fix in negatives or soak. Quantum, the full
negative matrix and both soaks remain the principal uncertainty. No exact
completion count is promised. TMV1-CL-14 remains out of scope.

## 8. FIX6 continuation

FIX6 replaced the redundant exact SELFTEST grep with the shared strict JSON
gate. The previously failing SMP8/KVM boot is therefore no longer classified
from a fragmented individual line. Before the async matrix could resume,
however, the dedicated record-framing focused matrix found a distinct
SMP8/TCG summary/case mismatch documented in
`TMV1-CL-13-FIX6-selftest-record-framing.md`. FIX5 async state and its 20
completed yield boots remain technically approved and were not reopened.
