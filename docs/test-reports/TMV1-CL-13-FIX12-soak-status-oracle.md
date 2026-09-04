# TMV1-CL-13-FIX12 — Soak status oracle

Status: REJECTED_CL13_SOAK

Date: 2026-08-19
Branch: `feat/taskman`
Base: `4e700a9b38653fb57ecb0f107a18f87508db0736`

## Reused evidence and cost boundary

FIX11 had already completed the four matrix scenarios, both quantum variants,
all 16 negative checkpoints and the SMP4/TCG soak. FIX12 verifies those
artifacts and their runtime manifest rather than executing them again. The
SMP4 log is frozen at
`fe5955f350b12d2b2bd0b1a6c649251a1db4691960a9f17079923ec5b073c0a8`;
it records 1,000,000 switches, 10,000 create/reap operations, 5,000 kills,
100,000 input events, 500 modal cycles, 501 PS snapshots, 101 TASKMAN sessions,
zero heap drift, zero invariant failures and zero faults.

The FIX11 checkpoint is frozen at
`fa1bd204e63604d42f5e25983f7d2952c53ad734b07d268be69106263bd12cc4`.
It contains matrix 4/4, quantum 2/2, negatives 16/16, positive resets and the
HPET BOOT_NEGATIVE revalidation. The protected runtime manifest remains
`d8dd183b4b2ef247bd766464e2e09dc88addbeb08c66b373dd6e9fb199b01d09`.

## Original seq24/seq25 failure

`cl13fix11-soak-smp8-kvm-failed.log` contains exact framed ACCEPT, BEGIN and
END status 0 records for sequences 24 and 25. Each handler produced accepted
100000, consumed 100000, chars 50000, specials 50000, last_sequence 100000,
and zero gaps, duplicates and regressions. Sequence 25's diagnostic line was
physically interleaved with `smpstress`; it was not a handler, queue, transport
or guest fault. The frozen `cmd_inputtest.c` hash is
`264e398d77f9fe1522ebf9722e16bf1e2e8ecf37f91304553a6370fe2ef5cb13`.

The offline checkpoint certifies only the two completed producer frames
(200,000 events). It does not splice the interrupted head into a later boot and
does not claim the old run completed modal, PS, TASKMAN, cleanup, heap or final
diagnostics.

## Command contracts and SYNC_STATUS

The framed harness now names five distinct contracts. SYNC_RECORD retains the
strict functional-record gate. SYNC_STATUS requires FRAME ACCEPT, BEGIN,
END/REPLAY, an allowed handler status, a live QEMU and no guest fault; its human
diagnostic marker is optional telemetry. ASYNC uses an exact run ID and
async-wait, LAUNCH_ONLY proves launch, and MODAL keeps the handler open around
raw keys.

`framed_send_status_complete` shares the FIX3 state machine: CRC and sequence
are unchanged, retry is allowed only before BEGIN, no payload is resent after
BEGIN, and REPLAY has no side effect. It emits
`STATUS_AUTHORITY_PASS` with `diagnostic_marker=0|1`. The strict
`framed_send_complete` behavior is unchanged.

The host unit covers a present, interleaved and absent marker; status 1; BEGIN
without END; END without BEGIN; and replay status 0. It passed. The historical
revalidator passed with producer_runs=2, input_events=200000 and faults=0.

## Focused collision and telemetry period

The SMP8/KVM collision canary kept `smpstress` at a 1000 ms period. Both
producer handlers returned status 0 exactly once; the first diagnostic marker
was intact and the second was interleaved (`diagnostic_marker=0`). Input check,
smpstress sweep, fixture status, taskdiag and transport balance passed with
zero faults. This causally confirms
`DIAGNOSTIC_MARKER_INTERLEAVING_CONFIRMED`.

The long soak uses `SMPSTRESS_PERIOD_MS=10000`. This changes only log
frequency, not workers, stress jobs, CPU load, duration, kill/reap behavior or
acceptance volumes. Host counters advance only after a status-authoritative
command completes. Each macro-stage is written to
`cl13fix12-soak8-stages.tsv`; those checkpoints are diagnostic and never join
different boots.

## SMP8 final soak

The new continuous SMP8/KVM run passed boot, warmup, exact-run yield (2,000,000
switches), create-handle-race 20000, timeout-race 10000 and both producer runs
for 200,000 input events. Sequences 24 and 25 each emitted ACCEPT, BEGIN,
the full correct counter set and END status 0. Thus the FIX12 status oracle
worked in the long soak as designed.

The next macro-stage, `modaltest open-close 1000`, completed all 1,000 cycles
and all 1,000 cleanups but reported `heap_drift=16640`. It emitted an intact
END for sequence 26 with status 1. This is a real handler failure, not a
missing/interleaved marker: SYNC_STATUS stopped immediately with
`GUEST_COMMAND_FAILURE`. No panic, exception, structural fault or transport
ambiguity occurred.

The stage checkpoint records PASS through `input` and FAIL at `modal`, all in
the same boot and runtime manifest. The run did not execute PS 500, TASKMAN
100, duration completion, smpstress sweep, heap-end or final clock/taskdiag
gates; no complete soak summary or global heap certification is claimed. The
preserved failed serial log hashes to
`e4949f2ad6d71a45bc81d155cc7103bf3d9e7fd19bd72fc260e736d4bce1c6d0`.

## Build and Git disposition

The pre-focused smoke passed stack-check, the 138-case SELFTEST/autorun build
and an empty `nm -u`. Runtime continuity before, after focused and after the
failed soak remained byte-identical, with manifest hash `d8dd183b...`.

Per fail-fast policy, the final release j2/jN build, deps/image hashes and
report gate were not executed. No files were staged, no commit was created,
and no push occurred. The next causal mission may resume using the FIX12 stage
checkpoint but must not represent the interrupted run as a complete soak.
TMV1-CL-14 was not started.
