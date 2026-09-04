# TMV1-CL-13 — TASKMAN V1 certification

Status: APPROVED

This report is completed from the CL-13 ledger after the static, selftest,
QEMU matrix, quantum, negative, soak and final-build gates pass. It records the
2026-08-16 `feat/taskman` baseline
`4e700a9b38653fb57ecb0f107a18f87508db0736`.

The initial audit found existing identity, scheduler, cancellation, clock,
metrics, reaper, input and modal validators; the `schedtest`, `synctest`,
`accounttest`, `killtest`, `reaptest`, `inputtest`, `modaltest`, `taskmantest`
and `taskdiag` stress/diagnostic commands; and causal negative hooks from the
CL-07 through CL-11 campaigns. CL-13 registers bounded wrappers around those
sources and leaves the established stress bodies in their owning commands.

The FIX1 boundary work is documented in
`TMV1-CL-13-FIX1-boundary-harness.md`. Its focused matrix passed SMP=1/TCG
3/3, SMP=2/TCG 3/3, SMP=2/KVM 5/5 and SMP=4/KVM 3/3, including controlled
foreign-semaphore noise and the causal old-gap negative. The resulting
classification is `HARNESS_FALSE_POSITIVE_CONFIRMED`; no exact target lost
wake reproduced.

Consolidated static and selftest gates subsequently passed, with 100 PASS,
zero FAIL/SKIP and zero P0/P1 failures. The forced final matrix passed UP TCG,
SMP=2 TCG and SMP=2 KVM, then stopped fail-fast in SMP=4 TCG: after all prior
scenario subsystems and the count=1 TASKMAN session passed, the count=20
`taskman 1000` session remained ACTIVE without its three-frame auto-exit
sentinel for the 900-second host timeout. There was no panic, exception,
guest-side FAIL or boundary stall. The fault artifacts are preserved under
`artifacts/build/cl13-matrix-smp4-tcg-taskman-timeout-900*`.

FIX2 causally confirmed the missing-anchor diagnosis. Four fixed snapshots
produced `TOO_SHORT`, zero full frames, 22 fallback frames and one shortfall;
ESC recovered the shell and all residual checks passed. With `anchor-reset`,
the identical setup produced three WIDE full frames, zero fallback and
captured 29 tasks. Matrix and soak helpers now anchor every automated session,
and the matrix helper uses a 180-second bound plus stats-based failure
classification. No runtime source changed.

The focused variant run then passed counts 1, 20, 50 and 129 on SMP=4/TCG,
including refreshes 50, 1000 and 2000, but the synthetic keyboard transport
lost the subsequent `taskmantest setup 257` command. The QEMU process remained
alive and the last guest checks were clean. The runner correctly did not retry
the lost command and stopped fail-fast with `REJECTED_CL13_FIX2_FOCUSED`.
SMP=4/KVM variants, soak-smoke, resumed matrix, quantum variants, negatives,
soaks and final build were therefore not executed; no commit was created.
Details are in `TMV1-CL-13-FIX2-taskman-anchor.md`. The visual checklist
remains in `TMV1-CL-13-visual-checklist.md`; TMV1-CL-14 remains reserved.

FIX3 adds a SELFTEST-only transactional shell envelope after the FIX2
`setup 257` delivery loss. CRC validation precedes dispatch, sequence is
monotonic per boot, `BEGIN` prohibits retry, `END` carries the handler status,
and a completed sequence can only return `REPLAY`. All CL-13 shell-line
runners source `scripts/harness-framed.sh`; real modal navigation keys remain
raw HMP. Its static gate and the focused SMP=1/TCG 3/3, SMP=2/TCG 3/3 and
SMP=2/KVM 3/3 boots passed, including CRC/partial recovery and non-idempotent
replay exactly once.

The first framed SMP=4/TCG boot then proved that the prior ambiguity was not a
transport stall: after a deliberate CRC rejection, the valid `taskmantest
setup 257` frame emitted `ACCEPT`, `BEGIN` and `END status=1`. The preserved
fixture has `FIXTURE_MAX 129u` and deterministically rejects larger counts
before printing its setup sentinel. Since `cmd_taskmantest.c` is explicitly
outside FIX3's allowed scope, certification stopped with
`REJECTED_CL13_MATRIX`. The later matrix, variants, negatives, soaks and final
build were not executed, and no commit was created. Full evidence is in
`TMV1-CL-13-FIX3-transactional-transport.md`.

FIX4 continues from that exact handler rejection. The fixture maximum is now a
testable 257-task contract with exact-handle visibility, transactional setup,
rollback and idempotent cleanup. Its focused and resumed evidence is recorded
in `TMV1-CL-13-FIX4-fixture-257.md`; the FIX1 boundary, FIX2 anchor and FIX3
transport results are reused unless a new causal failure contradicts them.

FIX4's focused validation passed capacity/range/busy, CRC reject and replay,
exactly-once setup/cleanup of 257 handles, real TASKMAN growth beyond 256 with
three PS pages, zero heap drift, SMP=4 TCG 3/3, SMP=4 KVM 2/2 and both SMP=8
smokes. The resumed static/autorun gates also passed with 110 selftests, and
UP/SMP2 evidence was reused by hash. The first new consolidated SMP=4/TCG boot
then stopped at `schedtest yield 4 20000`: the framed command emitted ACCEPT,
BEGIN, YIELD_START, created four workers and returned END status=0, but the
host incorrectly required the asynchronous YIELD_COMPLETE marker to appear
before END. This new causal `GUEST_COMMAND_CONTRACT_FAILURE` leaves the final
status `REJECTED_CL13_MATRIX`. Per fail-fast policy it was not retried or
fixed; quantum, negatives, soaks, final build and commit were not performed.
The protected runtime remained identical through focused validation and
TMV1-CL-14 was not started.

FIX5 continues from `REJECTED_CL13_ASYNC_COMMAND_CONTRACT`. It preserves the
strict synchronous frame rule while giving scheduler test workloads an
explicit START/END/async-wait contract correlated by monotonic run ID. Its
focused and resumed evidence is recorded in
`TMV1-CL-13-FIX5-async-command-contract.md`.

FIX5 static and host-unit gates passed, and 20 real focused yield boots passed
with exact run correlation and END_BEFORE_COMPLETE. The last required
SMP8/KVM boot reached AUTORUN PASS (116/0/0) and SHELL_READY, but concurrent
hotplug text split the exact async-selftest PASS line used by the focused host
matcher. The harness stopped before sending a frame. It was not retried;
cpu-pin/entry-window, the negative, resumed matrix, variants, soaks and final
build remain unexecuted. The current certification status is therefore
`REJECTED_CL13_FIX5_HARNESS`; no commit or push was created.

FIX6 makes result and summary construction bounded and emits each through one
serial call. Its strict parser host-unit, stack/build reproducibility, UP/TCG
2/2, SMP4/TCG 3/3, SMP4/KVM 3/3 and SMP8/TCG run 1 passed with 117 cases,
zero failures/skips and zero framing repairs. SMP8/TCG run 2 retained summary
117/0/0 but only 112 result records were line-parseable: five complete records
were prefixed by xHCI fragments from an already-open foreign line. No
EMIT_ERROR, guest fault or command frame occurred. The required classification
is `REJECTED_CL13_FIX6_SELFTEST_SUMMARY`; async resume, final matrix, quantum,
negatives, soaks, final build and commit were not executed. Protected runtime
outside `selftest.c` remained unchanged and TMV1-CL-14 was not started.

FIX7 identifies the FIX6 summary mismatch as a line-boundary consequence:
five atomic records began after still-open xHCI prefixes. SELFTEST records are
now emitted as one-call `newline + record + newline` strings and the parser
explicitly rejects markers outside column zero. Host reproductions, static
gates and the focused UP/SMP4/SMP8 TCG/KVM matrix passed with 117 cases, zero
repairs and zero boundary violations; required hotplug noise was observed.
The resumed FIX5 matrix then completed yield, CPU-pin, entry-window and the
causal stale-slot negative with exact run correlation. Details are in
`TMV1-CL-13-FIX7-selftest-line-boundary.md`; the consolidated matrix resumes
at SMP4/TCG before variants, negatives, soaks and final build.

The resumed static and autorun gates passed and the new SMP4/TCG boot validated
the fixed SELFTEST line boundary plus exact-run asynchronous yield. It then
stopped at `synctest sem 20000`: the workload reached its functional PASS, but
task-creation output split `[HARNESS][END] seq=18 status=0`, leaving no
parseable completion record. Retry after BEGIN was correctly forbidden. With
zero guest faults and protected runtime unchanged, this new causal matrix
failure is classified `REJECTED_CL13_MATRIX`. Later SMP4/SMP8 scenarios,
quantum, negatives, soaks, final build and commit were not executed.

FIX8 continues from that exact causal split. It introduces explicit
SHELL_READY/RUNTIME_READY/TEST_READY ordering, defers full autorun until core
services and initial hotplug are quiescent, gates framed commands on
TEST_READY, emits bounded line-fenced HARNESS records atomically, and gives
the worker-launch synctest family exact run-ID async status/wait semantics.
Focused and resumed results are recorded in
`TMV1-CL-13-FIX8-runtime-ready-harness-records.md`. All three FIX8 focused
classifications passed, as did the consolidated static gate and 132-case
post-RUNTIME_READY autorun. The first new SMP4/TCG boot then received an
explicit `[ACCOUNT][CLOCK] FAIL` for frame 33 and returned an intact
`[HARNESS][END] seq=33 status=1`. The serial record reports reads=1000000,
d10=10601330, d100=100612000 and d1000=1001070360. This is a real guest test
failure rather than readiness, record or transport corruption, so the
certification remains `REJECTED_CL13_MATRIX`. It was not retried; subsequent
matrix scenarios, quantum, negatives, soaks, final release build and commit
were not executed. TMV1-CL-14 was not started.

FIX9 replaced the invalid exact-millisecond wrapper equality with the bracket
contract and complete clock diagnostics. FIX10 separated monotonic/deadline
correctness from sleep service QoS. Their focused TCG/KVM validation passed;
the consolidated SMP4/SMP8 matrix and quantum variants subsequently passed.
FIX11 formalized BOOT/COMMAND/ASYNC negatives, revalidated HPET torn-read at
boot and checkpointed all 16 negatives. The SMP4/TCG soak passed and is frozen
by hash.

FIX12 addresses only the SMP8 soak's interleaved input diagnostic marker.
Offline seq24/seq25 revalidation and a focused SMP8/KVM collision canary proved
that both input handlers returned status 0 with correct counters. SYNC_STATUS
now uses that exact handler return as its oracle while SYNC_RECORD stays
strict. Matrix, quantum, negatives and soak4 were reused without rerun.

The new full SMP8/KVM attempt passed 2,000,000 switches, 20,000 create/reap,
10,000 kill races and 200,000 input events. It then stopped at the next stage:
`modaltest open-close 1000` returned status 1 after all cycles/cleanups, with
`heap_drift=16640`. The failure record and END were intact and no guest fault
occurred. Certification therefore stops at `REJECTED_CL13_SOAK`; PS/TASKMAN,
duration, cleanup, final diagnostics, final build and the single commit were
not executed. Runtime continuity remained exact and TMV1-CL-14 was not
started. Details are in `TMV1-CL-13-FIX12-soak-status-oracle.md`.

FIX13 replaced the contaminated modal warmup baseline with exact warmup and
worker-handle disappearance, real reaper quiescence, stable heap snapshots
and signed heap direction. FIX14 then separated modal ownership from the
global heap: exact modal stats/lifecycle deltas are the command's hard gate,
while concurrent timer-node movement remains diagnostic and the outer
`taskmantest heap-begin`/`heap-end` pair owns the scenario-level heap gate.
Its focused SMP4/SMP8 validation passed with clean ownership, 1,000 handles
gone, zero violations/faults and zero outer drift. Details are in
`TMV1-CL-13-FIX14-modal-ownership-vs-global-heap.md`.

TMV1-CL-13-FIX15: APPROVED

The rejected FIX14 soak was revalidated offline. Frame 924 was accepted and
began, its `AUTO_EXIT_ARM` text was interleaved by smpstress, and its atomic
END returned status 0. With 101 arm attempts, 100 completed sessions and no
sequence 925, the old host rejected a successful control command before it
launched the final stress session. The audited run lasted about 2h21m15s; it
was not a four-hour run and did not certify the unfinished tail.

FIX15 makes each TASKMAN soak session one guest-side transaction classified
against before/after TASKMAN stats. The composite loop preserves the exact
50/1000/2000 refresh pattern and external frame exactly-once behavior.
`tasktest ps-loop` repeatedly invokes the real PS implementation. Human arm,
auto-exit, stats and heap text remains diagnostic; intact handler status and
the corresponding state/delta contracts are authoritative.

Host/static validation and focused SMP4/KVM plus two SMP8/KVM boots passed
under smpstress. The new continuous SMP8/KVM soak passed 2,000,000 switches,
20,000 create/reap, 10,000 kills, 200,000 input events, 1,000 modal cycles,
500 PS snapshots and 100 TASKMAN sessions. Refresh coverage was 33/34/33,
the modal ownership gate passed, the post-sweep global heap drift was zero,
all final invariants passed and guest faults were zero. Guest duration was
2,855,046 ms.

The final clean build passed the 2,048-byte stack gate (maximum 1,984),
release j2/jN reproducibility, SELFTEST build, release restoration,
deps-check, image and empty undefined-symbol scan. Production and special
frozen manifests remained identical through focused, soak and build. The
complete evidence and final hashes are recorded in
`TMV1-CL-13-FIX15-taskman-transaction-batching.md`.

The CL-13 consolidated V1 certification is approved. No CL-14 work was
started.
