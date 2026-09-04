# TMV1-CL-13-FIX8 — runtime readiness and atomic HARNESS records

Status: focused validation approved; consolidated CL-13 rejected in the first
new SMP4/TCG boot by an explicit `accounttest clock` guest failure.

## Entry and continuity

Date: 2026-08-17. Branch: `feat/taskman`. Base:
`4e700a9b38653fb57ecb0f107a18f87508db0736`. FIX1 boundary observation,
FIX2 TASKMAN anchoring, FIX3 transactional transport, FIX4 fixture257, FIX5
schedtest async correlation and FIX6/FIX7 SELFTEST record framing remain
preserved. The FIX8 protected-runtime manifest excludes only the explicitly
authorized boot/readiness, DPC, shell, hotplug, tasktest, synctest and build
files.

## Causal failure

The resumed SMP4/TCG scenario launched `synctest sem 20000`. The guest
accepted frame 18, emitted BEGIN and SEM START, created its workers and
returned from the launch handler. A concurrent task-creation writer split the
multi-call control record into:

```text
[HARNESS][END] seq=18 status=[TASK][CREATE] id=0
23 name=synctest-sem-wait ...
```

The workload subsequently passed with waits=20000, signals=20000, permits=0
and errors=0. There was no guest FAIL, panic, exception, semaphore residual or
lost wake. The frozen entry classification is
`REJECTED_CL13_RUNTIME_BOUNDARY_AND_HARNESS_RECORD`.

Inspection also proved a real boot-order defect: the old core path executed
the full autorun and emitted SHELL_READY before terminal initialization and
`shell_init()`, then used a fixed 500 ms settle after PCI/hotplug activity.
FIX8 treats that defect independently from the corrupted END record.

## Runtime readiness contract

The new `runtime_ready` module publishes a monotonic state machine from
NOT_STARTED through WAITING_SERVICES, WAITING_DRIVERS, READY, TESTING and
TEST_READY, with FAILED terminal. Its copy-only snapshot covers expected and
online CPUs, scheduler, DPC, shell initialization/thread start, input and
modal validators, PCI completion, hotplug activity/quiet time, autorun result
and publication timestamps.

Shell and DPC expose read-only readiness snapshots. USB hotplug maintains a
balanced active-enumeration count, activity generation and last-activity
timestamp without changing its functional state machine. Initial readiness
requires active=0, a stable generation and a bounded 1000 ms quiet interval;
systems without initialized hotplug do not require a USB device.

The boot sequence is now:

```text
[BOOT][SHELL_READY] PASS
[BOOT][RUNTIME_READY] PASS ...
[SELFTEST][AUTORUN] BEGIN
[SELFTEST][SUMMARY] ...
[SELFTEST][AUTORUN] PASS
[BOOT][TEST_READY] PASS autorun=1 ...
```

With autorun disabled, TEST_READY follows RUNTIME_READY with `autorun=0`.
`tasktest exec` rejects frames before TEST_READY without consuming sequence,
and `tasktest readiness-status` reports the same state read-only. The shared
host gate validates marker order, autorun mode, PID continuity and absence of
early BEGIN records before enabling framed commands.

The focused readiness host-unit rejects the historical order, accepts the new
autorun and non-autorun orders, and rejects an early frame. The real matrix
passed UP/TCG 2/2, SMP4/TCG 3/3, SMP4/KVM 3/3, SMP8/TCG 3/3 and SMP8/KVM 5/5.
Every boot reported 132 PASS, zero FAIL/SKIP, zero SELFTEST framing repairs or
line-boundary violations, the required runtime/transport/sync cases, correct
CPU counts, clean validators and post-TEST_READY framed diagnostics.
Classification:

```text
PREMATURE_RUNTIME_TESTING_CONFIRMED
```

## Atomic HARNESS records

The SELFTEST-only tasktest transport now builds every machine control record
in a bounded 256-byte buffer. FRAME ACCEPT/REJECT, BEGIN, END, REPLAY, STATUS,
STATUS_DETAIL and EMIT_ERROR are each emitted as one
`newline + [HARNESS] record + newline` string through one serial call and one
console call. The END status is built from the command return value and cannot
contain payload output.

The host parser searches only complete anchored lines for transport records;
it never salvages a HARNESS substring from a foreign prefix. Host units reject
the historical split END and a prefixed END, while accepting their line-fenced
forms. Kernel selftests cover record fit, fences, maximum uint64/status,
maximum reject reason and maximum status counters.

The focused record stress passed one boot each on SMP1/TCG, SMP4/TCG,
SMP4/KVM, SMP8/TCG and SMP8/KVM. Each boot issued 100 marker frames, 20 real
fixture setup/cleanup cycles and 20 asynchronous SEM launches before checking
transport balance and all HARNESS line boundaries. All five logs have complete
anchored FRAME/BEGIN/END/STATUS records, accepted=completed, zero prefix or
fragment violations, both valid async marker orders and zero guest faults.
Classification:

```text
MULTI_CALL_HARNESS_INTERLEAVING_CONFIRMED
```

## Synctest asynchronous contract

The worker-launch commands `sem`, `boundary`, `boundary-noise`, `sleep`,
`cancel`, `timer-cancel` and `race` use an explicit per-boot monotonic run ID.
Their START/final markers and async status/wait records are bounded,
line-fenced, single-call records. Controllers publish counters, terminal
PASS/FAIL state and completed run, then clear `g_any_test_running` before the
final marker. Partial controller/worker creation publishes FAIL instead of
leaving RUNNING indefinitely.

`synctest async-status [run]` is read-only;
`synctest async-wait <run> [timeout_ms]` correlates exactly one known run and
polls at 10 ms with a bounded timeout. The host `framed_synctest_async` helper
launches, extracts the nonzero run, accepts END before or after completion,
waits by exact run, verifies the matching final marker and never resends after
BEGIN. Historical synchronous commands remain SYNC.

The causal SMP4/TCG `sem 20000` reproduction now has an intact END, exact run
1, END_BEFORE_COMPLETE, async-wait PASS, waits/signals 20000, permits/errors
zero and no guest fault. The complete focused synctest matrix passed
SMP1/TCG 3/3, SMP2/TCG 3/3, SMP2/KVM 3/3, SMP4/TCG 5/5, SMP4/KVM 3/3,
SMP8/TCG 2/2 and SMP8/KVM 2/2: 21 boots, 147 exact runs, zero timeout, run
mismatch, residual RUNNING state or guest fault. Classification:

```text
SYNC_WRAPPER_FALSE_REJECTION_CONFIRMED
PRE_READY_AUTORUN_AND_FRAGMENTED_HARNESS_CONFIRMED
```

The focused stack/build gates passed, selftest j2 and jN ELF files were
byte-identical, image creation and undefined-symbol checks passed, and the
protected before/after-focused manifests were identical across 192 files.

## Continuation

After the three focused classifications, CL-13 resumes at its static/selftest
build and the remaining SMP4 matrix, followed by SMP8, quantum variants, the
complete causal negative matrix, SMP4/SMP8 soaks, the deterministic final
release build, reports and the single required commit. No CL-14 work is in
scope.

The non-gating operational estimate remains: FIX8 may close CL-13, while
negative/soak execution can still expose one new causal correction. No exact
number of corrections is promised.

## Consolidated CL-13 result

The post-FIX8 static gate passed for release, SELFTEST and autorun builds. The
SELFTEST stack maximum was 1984 bytes, j2/jN binaries were byte-identical,
`nm -u` was empty, and the post-RUNTIME_READY autorun reported 132 PASS with
zero FAIL/SKIP, framing repair or line-boundary violation. UP/TCG, SMP2/TCG
and SMP2/KVM workload evidence was reused by the certified hashes.

The first new SMP4/TCG boot then passed runtime readiness, autorun, transport,
subsystem checks, exact-run scheduler yield and the migrated asynchronous
synctest workloads through timer-cancel. Frame 33 for `accounttest clock` was
accepted and began normally, but the guest emitted:

```text
[ACCOUNT][CLOCK] FAIL reads=1000000 d10=10601330 d100=100612000 d1000=1001070360
[HARNESS][END] seq=33 status=1
```

The HARNESS record was intact and line-fenced; this is neither a readiness,
record-framing nor asynchronous-contract failure. There was no panic,
exception or transport ambiguity. Per the CL-13 fail-fast rule the boot was
not retried, and SMP4/KVM, SMP8, quantum, negatives, soaks, final release build
and commit were not executed. The serial, debugcon and trace evidence is
preserved as `artifacts/build/cl13fix8-matrix-smp4-tcg-account-clock-failed*`.
The consolidated classification is `REJECTED_CL13_MATRIX`. TMV1-CL-14 was not
started.
