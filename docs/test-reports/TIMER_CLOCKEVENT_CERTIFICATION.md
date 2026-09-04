# Timer clockevent certification

## Causal input and disposition

The frozen baseline is branch `feat/taskman`, commit
`523f367c9cd8652e709c0ffc7af28f6433d1213c`, tree
`cc04290b8cfcfcc16de06e65b1a8145f6760d646`, with parent
`602532db881f5e328a78efdb52b0c012f35b4065`. The supplied project archive has
SHA-256 `79501959173d699d76182659e7921f563a24669bf83f8380311f491c9fbadc58`.

The i9-12900K serial trace reaches
`[IRQ][BSP_LAPIC_PROBE] PASS vector=34 entered=1 returned=1 period_us=1000`
and emits no later marker. This proves the BSP LAPIC vector, external stub,
dispatcher, journal, EOI, and return path. It localizes the remaining physical
failure to the subsequent HPET Timer0 IRQ path without proving whether the
stop occurred during comparator setup, IOAPIC release, handler entry,
acknowledge/rearm, return, or an immediate storm. The input classification is
`BAREMETAL_HPET_TIMER0_IRQ_PATH_NONRETURN`.

Production therefore no longer requires HPET Timer0 delivery. HPET remains the
monotonic clocksource, Timer0 remains quiescent, all LAPIC timers remain
periodic at 1000 us, and the discovered BSP LAPIC is the single global
clockevent. This is a conceptual clocksource/clockevent separation, not a
line-by-line Linux port.

## Evidence contract

The authoritative records are generated under
`artifacts/build/lapic-clockevent/`:

- `source-before.sha256` freezes the audited input files;
- `protected-before.sha256`, `protected-after-tests.sha256`, and
  `protected-after-build.sha256` prove protected-source continuity;
- `evidence.tsv` and `evidence.json` bind each affirmative marker to a hashed
  log and exact machine/SMP/acceleration tuple;
- `qemu/*/serial.log` contains the six matrix boots and focused soak;
- `final-build.log` records stack, deterministic build, dependency, undefined
  symbol, and production-image gates;
- `report.log` records offline verification.
- `host-environment.txt` records host CPU allowance, load, pressure,
  QEMU/KVM availability, requested acceleration, and guest SMP;
- `source-worktree-before-closure.sha256` and
  `source-worktree-after-closure.sha256` prove certification-only closure;
- `qemu/smp24-kvm-failed/serial.log` remains immutable evidence of the raw
  `worst_median_x1000=672` result (SHA-256
  `75fc283521f23bae6916493982ddb188768412d0e9b177aa2a9a2a55f6c39d46`).

`scripts/test-timer-clockevent.sh` owns QEMU through the existing agent HMP
helpers. `scripts/verify-timer-clockevent.py` uses only the Python standard
library. Neither tool awards PASS merely because a fault string is absent.

The ledger stores `raw_test_result`, `measurement_authority`, and
`scenario_disposition` independently. A raw failure is never rewritten as a
raw pass and the threshold remains 700. An SMP24 exception is valid only when
the host is oversubscribed and the same VM proves all CPU deltas,
configuration/liveness, runtime readiness, IRQ balance, clockevent ownership,
HPET quiescence, and zero faults. Three fixed SMP4/KVM boots with host
headroom remain the authoritative cadence control.

## Implemented architecture

The bootstrap state now advances through `HPET_CLOCKSOURCE_VERIFIED` and
`CLOCKEVENT_ACTIVE`. The HPET probe is a fixed-sample counter-progress proof
with IF clear and no comparator or GSI manipulation. Timer0 force-quiescence
preserves the main counter, clears interrupt/periodic/FSB/legacy/pending state,
and masks any known route.

The LAPIC handler obtains one monotonic timestamp. Only its explicit BSP-owner
branch calls the global clockevent, which forwards the derived milliseconds to
`timers_poll_at()` and, after service release, `timer_run_deferred_at()`.
An unexpected HPET vector is counted and quarantined without rearm, time read,
timer polling, scheduler activity, allocation, or event logging.

The scheduler, switch assembly, clock implementation, LAPIC, IOAPIC, MADT,
graphics implementation, TASKMAN implementation, bootloader, and shared
sources remain byte-identical to the protected baseline.

## Required gates

The completed evidence ledger must contain PASS rows for the five causal
negative builds, UP/q35/TCG, SMP2/q35/TCG, SMP4/q35/KVM, SMP8/q35/KVM,
SMP24/q35/KVM, PCAT/pc/TCG, focused software timers, accounting, TASKMAN,
SMP24 stress, the at-least-300000 ms SMP24 soak, the final reproducible build,
report, and candidate.

Every guest boot must affirm the BSP LAPIC probe, bounded HPET clocksource
probe, Timer0 quiescence, sole BSP LAPIC clockevent, all discovered CPU
handoffs/readiness, service release, splash begin/complete, shell readiness,
runtime readiness, production test policy, and entry into the main loop. The
terminal snapshots must report zero early/non-BSP clockevent attempts,
timestamp regressions, HPET IRQs/strays, unexpected vectors, and IRQ imbalance.

The final candidate is created only after the last QEMU process stops and the
production image is rebuilt. QEMU scenarios use private copies of the earlier
test image; the final candidate image is never opened by QEMU.

## Result publication

Concrete scenario durations, final hashes, commit/tree identifiers, stack
maximum, and candidate paths are published by the final evidence verifier and
`candidate.txt`. This report is valid only together with an affirmative
`[TIMER][EVIDENCE] PASS scope=all` record and a candidate whose `SHA256SUMS`
verifies. Bare metal is not executed by the certification runner. The maximum
software disposition is
`READY_FOR_BARE_METAL_LAPIC_CLOCKEVENT_VALIDATION`; William owns the first
physical boot.

The populated result must state independently: implementation PASS; QEMU
functional matrix PASS; the real SMP24 raw rate result and its authority;
`SMP4_KVM_3_OF_3_PASS`; SMP24 soak PASS; disposition
`PASS_WITH_DOCUMENTED_ENVIRONMENT_RATE_EXCEPTION` when host CPUs are fewer
than 24; bare metal `NOT_EXECUTED_BY_CODEX`; and next owner William.

## Environment-aware closure result (2026-08-25)

The implementation and the completed functional scenarios remained intact,
but this closure is rejected by a real runtime gate:

```text
Implementation: PASS
Host schedulable CPUs: 12
SMP24 guest vCPUs: 24
Original SMP24 raw LAPIC-rate: FAIL, worst_median_x1000=672
Closure SMP24 raw LAPIC-rate: FAIL, worst_median_x1000=593
SMP24 rate authority: ENVIRONMENT_NON_AUTHORITATIVE_OVERSUBSCRIBED
SMP4/KVM rate control: 3/3 PASS (996, 997, 997)
SMP24 functional boot: PASS_WITH_ENVIRONMENT_RATE_EXCEPTION
PCAT/pc/TCG: PASS, pcat=1, PIC=ff/ff
SMP24 soak: FAIL before certification completion
Disposition: REJECTED_TIMER_RUNTIME_CORRECTNESS
Bare metal: NOT_EXECUTED_BY_CODEX
Next owner: William
```

The first soak fault was the second predetermined timer-cancel workload:

```text
[SYNC][TIMER_CANCEL] FAIL pending=0 claimed=1 callbacks=0
[SYNC][TIMER_CANCEL] FAIL run=4 requested=3 completed=0 errors=1
[SYNC][ASYNC_WAIT] FAIL reason=workload run=4 kind=TIMER_CANCEL state=FAIL requested=3 completed=0 errors=1
```

This is not a LAPIC-rate precision result and therefore cannot use the
oversubscription exception. The VM was stopped without retry. The frozen log
is `artifacts/build/lapic-clockevent/qemu/soak24-failed/serial.log`, SHA-256
`29ace19ec5bd95ef29f8d962186dcd88d5ec159e636427e708b54ea869dc8e70`.
No final build, commit, candidate, review ZIP, USB write, or physical boot was
performed.

The closure source manifest remained byte-identical before/after with SHA-256
`3965649d96d99ce62a6cdd61f8ddd525e4b7b354ff5b34814b8b5b5e0739015b`;
the broader protected manifest remained byte-identical with SHA-256
`021bc863f03555b1df1dcfa3a04e6afed626981b6fdacb71f14683b42eecdefe`.

## Superseding liveness closure (2026-08-26)

The rejection above remains immutable input evidence. The cumulative timer
fair-dispatch repair now preserves older CLAIMED work ahead of newly due work,
and the negative claimed-bypass build proves that the gate detects the former
ordering. The final kernel passed timer-order/timer-cancel 3/3 on SMP4 and 5/5
on SMP24, with late callbacks and claimed residuals both zero.

The subsequent boot-to-shell campaign also closed the independent splash
liveness stop. `splash.c/.h` are therefore an explicitly allowed delta from
the earlier graphics-continuity statement; the rest of the protected graphics
and TASKMAN implementation remained byte-identical. Full causal details are
in `BOOT_TO_SHELL_AUTONOMOUS_CLOSURE.md`.

```text
Implementation: PASS
Final kernel: f45d2c0bf00de08c37303e88433d881acc0d6e2ac395d34e331ce72671fa91fe
SMP24 boot-to-shell: 3/3 PASS, no retry
Quick matrix: 6/6 PASS
Runtime SMP24: PASS
SMP24 timer fairness: order=5 cancel=5 late=0 claimed_residual=0
SMP24 raw LAPIC-rate: FAIL, worst_median_x1000=497
SMP24 rate authority: ENVIRONMENT_NON_AUTHORITATIVE_OVERSUBSCRIBED
SMP4/KVM rate control: 3/3 PASS (997, 996, 995), threshold=700
SMP24 soak: PASS, duration_ms=979860
Final build: PASS, stack_max=1968, j2/j12 identical, undefined=0
Disposition: READY_FOR_BARE_METAL_LAPIC_CLOCKEVENT_VALIDATION
Bare metal: NOT_EXECUTED_BY_CODEX
Next owner: William
```
