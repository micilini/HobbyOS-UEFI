# TMV1-CL-13-FIX15 — Transactional TASKMAN sessions and guest-side batching

Status: APPROVED

Date: 2026-08-19
Branch: `feat/taskman`
Base: `4e700a9b38653fb57ecb0f107a18f87508db0736`

## Preserved evidence and entry classification

FIX1 through FIX14 remain intact. The frozen evidence was accepted without
rerunning its workloads:

| Evidence | Result / SHA-256 |
| --- | --- |
| consolidated matrix | 4/4 PASS, reused |
| quantum variants | 2/2 PASS, reused |
| negative checkpoints | 16/16 PASS, `fa1bd204e63604d42f5e25983f7d2952c53ad734b07d268be69106263bd12cc4` |
| SMP4/TCG soak | PASS, `fe5955f350b12d2b2bd0b1a6c649251a1db4691960a9f17079923ec5b073c0a8` |
| FIX12 partial SMP8 | `e4949f2ad6d71a45bc81d155cc7103bf3d9e7fd19bd72fc260e736d4bce1c6d0` |
| FIX14 partial SMP8 | `84188d1ddf11a2ce9958922eef8adc6ff0f6c194900625ccb8d2404d5b347a9a` |
| frozen runtime manifest | `d8dd183b4b2ef247bd766464e2e09dc88addbeb08c66b373dd6e9fb199b01d09` |

No matrix, quantum, negative, SMP4 soak or earlier focused campaign was
repeated. The entry classification is
`REJECTED_CL13_SOAK_TASKMAN_CONTROL_MARKER_INTERLEAVING`, not a runtime,
TASKMAN, modal or transport defect.

## Causal audit of the rejected run

The preserved FIX14 log has SHA-256
`84188d1ddf11a2ce9958922eef8adc6ff0f6c194900625ccb8d2404d5b347a9a`.
It records frame 924 accepted at line 35833, BEGIN at line 35835, the
`AUTO_EXIT_ARM` text prefixed/interleaved by `smpstress` at line 35836, and
END status 0 at line 35839. There is no sequence 925, no subsequent TASKMAN
auto-exit and no guest fault.

The source snapshot hash
`4f83e637757d67b24f2693056776b5e20163e1f4a3fe620bb008d0809a20a68d`
revalidates strict integer parsing, range 1..100000, direct arm and the
handler's `ok ? 0 : 1` status contract. The old log contains 101 arm
substrings, 100 completed auto-exits, 101 anchor resets, and 924 balanced
ACCEPT/BEGIN/END-status-0 frames. Thus the warmup and 99 stress sessions
completed; stress session 100 was armed successfully but never launched by
the host.

The stage timestamps were boot 13:54:27, warmup 13:57:48, modal 14:04:15,
PS 15:17:46 and TASKMAN failure 16:15:41. Total elapsed time was
8,474.935 s (about 2h21m15s), including 4,410.839 s in PS and 3,475.772 s in
TASKMAN. The offline sentinels are:

```text
[CL13][TASKMAN_OLD_RUN_AUDIT] PASS
arm_status=0 completed_stress_sessions=99 next_session_not_launched=1
[CL13][AUTO_EXIT_ARM_REVALIDATED] PASS seq=924 status=0 armed=1 faults=0
```

## Guest-side transaction contract

`taskmantest auto-session <refresh_ms> <frames>` now performs one complete
session inside a single framed handler: it snapshots TASKMAN stats, clears the
console anchor, arms auto-exit directly, calls `cmd_taskman(2, argv)` directly,
snapshots again, classifies exact deltas, emits one bounded line-fenced
`[TASKMANTEST][AUTO_SESSION]` record, and returns the authoritative status.
It never recurses through the shell parser.

For the soak's target of one full frame, every session observed:

- `taskman_status=0`, sessions delta 1 and auto-exit delta 1;
- full-frame delta 1, fallback/render/shortfall deltas zero;
- WIDE mode, at least one page and a nonzero captured population;
- zero modal scroll, shell-exit scroll and clipped writes;
- no live model, no pending auto-exit, and default controls.

The pure classifier also covers targets 1 and 3 and rejects missing sessions,
wrong full-frame counts, fallbacks, shortfalls, live models, pending controls
and invalid modes. The maximum legal record fits the 512-byte BSS buffer and
is emitted by one `serial_write_all` call. `AUTO_EXIT_ARM` and
`TASKMAN AUTO_EXIT` text remain diagnostic only.

`taskmantest auto-session-loop <count> <frames> <start_index>` executes those
transactions guest-side, stops at the first failure, emits atomic progress
every ten sessions and returns a single final loop result. Its global-index
refresh pattern is unchanged: divisible by 3 uses 50 ms, remainder 1 uses
1,000 ms, remainder 2 uses 2,000 ms. Frame replay was tested on SMP4: the
transport returned `REPLAY`, and the auto-session count did not change.

`tasktest ps-loop <count>` is deliberately narrow. It invokes the real
`cmd_ps(1, {"ps"})` on every iteration, including the scheduler snapshot,
generation retry, sort, sampler, formatting and output paths; it stops on the
first nonzero status and emits bounded atomic progress/final HARNESS records.

## Focused validation

The host audit and synthetic status/delta/partial-loop cases passed. The
focused build passed stack-check, reproducible j2/jN SELFTEST builds, image,
empty `nm -u`, all 151 autorun selftests and zero warnings in FIX15-owned C
files.

| Scenario | PS | TASKMAN | Refresh 50/1000/2000 | Outer heap | Transport/faults |
| --- | ---: | ---: | --- | --- | --- |
| SMP4/KVM | 20/20 | 20/20 | 6/7/7 | drift 0 | 11/11, replay exactly once, 0 faults |
| SMP8/KVM run 1 | 100/100 | 50/50 | 16/17/17 | drift 0 | 12/12, 0 faults |
| SMP8/KVM run 2 | 100/100 | 50/50 | 16/17/17 | drift 0 | 12/12, 0 faults |

All boots ran with active `smpstress`. The classification is:

```text
[CL13][TASKMAN_TRANSACTION_CLASSIFICATION]
AUTO_EXIT_ARM_DIAGNOSTIC_MARKER_FALSE_REJECTION_CONFIRMED.
```

One preliminary focused boot was stopped before frame 1 because the new host
runner lacked its local HMP command array. No guest workload or scenario was
executed in that attempt. The host-only setup was corrected, syntax-checked,
and the required focused boots above were then executed once; no frozen
campaign was rerun.

## Full SMP8 soak

One continuous SMP8/KVM soak with `SMPSTRESS_PERIOD_MS=10000` passed. Guest
duration was 2,855,046 ms (47m35.046s), and every preserved volume completed:

| Workload | Completed |
| --- | ---: |
| scheduler switches | 2,000,000 |
| create/reap | 20,000 / 20,000 |
| kill races | 10,000 |
| input events | 200,000 |
| modal open/close | 1,000 |
| PS under stress | 500 |
| TASKMAN under stress | 100 |

PS used five guest batches of 100. TASKMAN used four batches of 25 with
start indices 1, 26, 51 and 76, preserving refresh coverage 33/34/33; the
separate warmup used one 50 ms session and was not counted in the stress
volume. All 101 auto-session records passed. The host frame count fell from
500 to 5 for PS and from approximately 300 to 4 for TASKMAN, saving 791
round trips without reducing work.

The modal ownership checkpoint passed its exact 1,001 deltas and 1,000
handles, with zero internal signed heap delta. After the smpstress sweep, the
hard global heap gate passed at 995,280 bytes and 78 blocks before and after.
Final scheduler, sync, accounting, kill, reaper, input, modal, TASKMAN,
fixture, clock and taskdiag invariants passed; faults were zero. The stage
ledger contains boot, warmup, yield, create-reap, kill-race, input, modal,
five PS batches, four TASKMAN batches, duration, cleanup, final diagnostics
and summary, all PASS.

The raw soak summary omitted the value after `modal_timer_nodes_after`
because of a host-side interpolation typo; the raw modal record and stage
ledger both preserve the value 18 (before 17). The script was corrected to
emit `modal_timer_nodes_after=$modal_timer_nodes_after`; this field is
diagnostic and did not participate in any gate.

## Source continuity and final build

Production-runtime manifests matched before focused, after focused, after
soak and after build (`db0e1c114a9ba5b8ddbde5f8310448de9fb43007e8e976e39646d268241bd50a`).
The special frozen TASKMAN/PS/modal/scheduler/timer/heap/smpstress manifests
also matched throughout
(`c39379719eb54d0463dd03831411952ab21db58f9ed98dfdf15fdc2c65bcbf12`).

The final clean build passed stack-check with maximum frame 1,984 bytes,
release j2/jN equality, the SELFTEST build, a rebuilt SELFTEST=0 release,
deps-check, image creation and empty `nm -u`. There were no warnings from
FIX15-owned files; existing warnings outside the change scope remain
non-blocking. Final hashes are:

- `kernel.elf`: `118284945ca3b469cba586cf5334ddedd28fb73baec61f130784a5b9d719bc1e`
- `hobbyos.img`: `46c36ec6298c533d82ddc122494dcb10953c4d60b08fa45ae85fe59036605105`

## Git and artifacts

The pre-stage audit is clean, the branch/base are exact, generated artifacts
and binaries remain untracked/ignored, and the roadmap stays outside the
stage. Finalization creates exactly one commit with message
`test(taskman): add automated V1 SMP, lifecycle, input and UI validation` and
does not push.

Primary artifacts:

- `artifacts/build/cl13fix15-old-taskman.{json,log,md}`
- `artifacts/build/cl13fix15-taskman-transaction-focused.log`
- `artifacts/build/cl13fix15-ledger.tsv`
- `artifacts/build/cl13-soak-smp8-kvm.log`
- `artifacts/build/cl13fix15-soak8-stages.tsv`
- `artifacts/build/cl13fix15-final-build.log`
- `artifacts/build/cl13fix15-final-{release,image}.sha256`
- `artifacts/build/cl13fix15-production-runtime-*.sha256`
- `artifacts/build/cl13fix15-special-*.sha256`

TMV1-CL-14 was not started.
