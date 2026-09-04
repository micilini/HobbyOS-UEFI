# TASKMAN V1 homologation

Status: TASKMAN V1 APPROVED

- Date: 2026-08-19
- Branch: `feat/taskman`
- CL13 commit: `f9d47db7e4332fc2d88dce2842f997a9b3f82e57`
CL14 release commit: the commit containing this document.

## Formal decision

TASKMAN V1 APPROVED

- Known open P0: 0
- Known open P1: 0

| Roadmap gate | Status | Evidence |
|---|---|---|
| Build green | PASS | reproducible j2/jN build, stack 1984, empty `nm -u` |
| SMP boot | PASS | matrix plus SMP4/TCG and SMP8/KVM soaks |
| Unique IDs/handles | PASS | identity selftests and lifecycle snapshots |
| Physical handoff | PASS | cpu-pin/entry-window and switch invariants |
| No lost wake | PASS | prepare/commit/abort and negative detection |
| Timer refs | PASS | timer-ref lifecycle/reaper cases |
| Accounting | PASS | clock, sampler, CPU-window cases |
| Kill | PASS | all result classes and cancellation races |
| Protected/non-killable | PASS | positive and negative kill coverage |
| Reaper safety | PASS | hard eligibility mask, churn, timer/on-CPU cases |
| Modal owner death | PASS | cleanup and lifecycle recovery paths |
| UI/layout | PASS | real 257 fixture, layouts, pagination, ESC-only exit |
| Test infrastructure | PASS | framed exactly-once and atomic machine records |
| Known P0/P1 | PASS | zero open P0 and P1 in consolidated report |

## Evidence set

The decision uses CL-01 through CL-12 reports under `docs/taskman-v1/reports`,
the [CL-13 consolidated report](test-reports/TMV1-CL-13-taskman-v1-certification.md),
the [FIX15 report](test-reports/TMV1-CL-13-FIX15-taskman-transaction-batching.md),
the 4-case matrix, 2 quantum variants, 16 negative checkpoints, SMP4/TCG and
SMP8/KVM soak logs, final kernel/image hashes, and four-way runtime/special
source manifests. The [test plan](taskman-v1-test-plan.md) maps evidence to
guarantees so readers need not reconstruct fifteen corrective reports.

The CL-14 release build, after text-only banner cleanup, is reproducible at
`9f8fcc9417e68118ef8fcde4c2af8f9dadd5eb138397b7e4ef15585e9463b973`
for `kernel.elf` and
`424b2b6656d9d8cf772bb173f4f99ccb347d1e005ef3cb1e0db4cb0d80c27b48`
for `hobbyos.img`; these are distinct from the frozen CL-13 binary hashes.

## Non-blocking notes

- Historical xHCI unused/packed-member/switch warnings and linker
  `.note.GNU-stack`/RWX warnings predate this documentation closeout.
- A preliminary HMP boot without workload is retained as historical evidence,
  not as the final certification.
- One preliminary CL-14 smoke attempt reached help and the first PS command,
  then the host runner stopped QEMU on an unbound local timeout variable. The
  corrected runner produced the single continuous qualifying SMP4/KVM smoke;
  the aborted log is retained and is not counted as certification.
- The frozen raw SMP8 summary omitted one timer value; the atomic modal record
  and stage ledger preserve `timer_nodes_after=18`, and the script is corrected.
- Review ZIPs do not contain `.git`; repository identity was verified from the
  live worktree.
- Bare-metal was not executed by CL-14. It is a separate follow-up campaign;
  the V1 roadmap certifies QEMU TCG/KVM.

## V2 gate

TASKMAN V2 ENTRY: AUTHORIZED_NOT_STARTED

This authorizes planning or execution of a separately reviewed V2 roadmap.
No V2 feature is implemented by CL-14. See
[V2 entry criteria](taskman-v2-entry-criteria.md).
