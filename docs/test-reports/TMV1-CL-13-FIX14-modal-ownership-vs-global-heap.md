# TMV1-CL-13-FIX14 — Modal ownership versus global heap

Status: APPROVED

FIX15: APPROVED

Date: 2026-08-19
Branch: `feat/taskman`
Base: `4e700a9b38653fb57ecb0f107a18f87508db0736`

## Cause and corrected contract

FIX13 established exact warmup/worker handle disappearance, reaper
quiescence and stable heap snapshots. Its two SMP4 focused boots nevertheless
ended with different global heap observations while modal ownership was
clean: run 1 was `ZERO / 0 bytes / 0 blocks`; run 2 was
`DOWN / -96 bytes / -1 block`. The preserved audit confirms zero guest
faults and exact cleanup of 1,000 workers in both runs.

On the current x86-64 ABI, `sizeof(timer_node_t)=96` and `HEAP_ALIGN=16`.
The concurrent `smpstress` workers use `yield_ms=1`, call `timer_sleep(1)`,
and therefore allocate/free timer nodes independently of the modal command.
Stable snapshots cannot assign a global allocation to modal ownership while
those workers remain active. The old internal global-heap equality gate was
therefore a false ownership oracle.

The final split is:

- `modaltest open-close` hard-gates modal-owned lifecycle: exact warmup and
  cycle handles, reaper state, runtime/session/router/shell invariants, and
  exact before/after `modal_ui_stats` deltas.
- Its heap and timer fields remain visible as
  `heap_scope=GLOBAL_DIAGNOSTIC heap_gate=OUTER_SCENARIO`.
- `taskmantest heap-begin`/`heap-end`, after `smpstress` cleanup, is the hard
  global-heap gate for the complete scenario.

No modal runtime, scheduler, reaper, timer, heap allocator or smpstress
production source was changed by FIX14.

## Ownership gates

For 1,000 requested cycles plus one warmup, the required deltas are exactly:

| Field | Required |
| --- | ---: |
| runs/workers/normal | 1,001 |
| cleanup closes/completion signals | 1,001 |
| killed/duplicates | 0 |
| allocation/create/begin/recovery/second-session failures | 0 |
| handles gone | 1,000 |
| contexts live/quarantined | 0 |
| zombies/free inflight | 0 |

Runtime inactive, session inactive, default router, unpaused shell, default
test controls and zero modal/session violations remain mandatory. Timer node
counts and signed global heap direction are diagnostic only.

## FIX14 focused evidence

The offline host audit passed with run 1 `ZERO`, run 2 `DOWN -96/-1`, timer
node size 96, heap alignment 16, `smpstress` sleep of 1 ms, clean ownership
and zero faults. The pure host classifier accepted clean ownership with heap
`ZERO`, `UP` or `DOWN`, rejected dirty ownership, and required an outer heap
gate.

Focused SMP4/KVM passed with ownership deltas 1,001, 1,000 handles gone,
timer nodes 8 to 10 and an internal diagnostic `UP +96/+1`; the outer heap
ended at zero drift. Focused SMP8/KVM passed with the same ownership deltas,
timer nodes 15 to 18, internal diagnostic `ZERO`, outer heap zero and zero
faults. The resulting classification was:

```text
[CL13][MODAL_HEAP_CLASSIFICATION]
GLOBAL_TIMER_NODE_NOISE_FALSE_REJECTION_CONFIRMED.
```

## Continuation through FIX15

The first FIX14 full SMP8 run later stopped in the unrelated TASKMAN stage
because an accepted arm command's human-readable marker was interleaved by
`smpstress`; modal ownership had already passed. FIX15 corrected that host
oracle and completed a new continuous SMP8 soak. The modal checkpoint in that
run passed with warmup ID 30054, `warmup_gone=1`, 1,000/1,000 cycle handles
gone, all 1,001 ownership deltas exact, zero violations, internal heap
`ZERO/0`, timer nodes 17 to 18, and idle reaper. After the stress sweep, the
outer heap passed with 995,280 bytes and 78 blocks before and after.

The frozen production manifest and the special modal/session/scheduler/timer/
heap/smpstress manifest were identical before focused validation, after
focused, after soak and after the final build.

## Evidence

- Old focused audit: `artifacts/build/cl13fix14-old-focused.{json,log,md}`
- FIX14 focused ledger: `artifacts/build/cl13fix14-ledger.tsv`
- FIX14 focused logs: `artifacts/build/cl13fix14-modal-smp4-kvm.log` and
  `artifacts/build/cl13fix14-modal-smp8-kvm.log`
- Classification: `artifacts/build/cl13fix14-modal-classification.log`
- FIX15 final soak: `artifacts/build/cl13-soak-smp8-kvm.log`
- FIX15 stages: `artifacts/build/cl13fix15-soak8-stages.tsv`

TMV1-CL-14 was not started.
