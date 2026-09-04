# Interrupt bring-up certification

## Scope and causal input

Baseline branch: `feat/taskman`
Baseline commit: `602532db881f5e328a78efdb52b0c012f35b4065`
Baseline tree: `22a109bf7d2dd761bdcb8cca16b67e1258770872`
Input archive SHA-256:
`c40995690004e5233a32a350fc135d8b7d59dcbc5401cf121028bf3ad11777c3`
Physical trace SHA-256:
`9c2cd9029d5ce23f770da1397eb9070d5db167988f6cd1d0568011cf93383f16`

The supplied physical trace discovers and onlines 24 CPUs, calibrates every
LAPIC timer for 1000 microseconds, creates the kernel workers, returns from
`scheduler_start`, prints the marker immediately before `sti`, and never prints
the following marker. PCI, graphics, the shell, and TASKMAN do not begin. The
input classification is therefore
`FIRST_BSP_MASKABLE_INTERRUPT_DOES_NOT_RETURN`.

The nineteen exact source findings and their functions are recorded in
[`interrupt-controller-bringup.md`](../interrupt-controller-bringup.md). The
implemented correction covers PIC acquisition, bounded MADT/ISO parsing,
multi-IOAPIC GSI routing and quarantine, LAPIC LVT shutdown, separate HPET
clocksource/clockevent state, deliberate BSP probes, AP containment, formal
scheduler handoff, per-CPU preemption gates, service/MSI staging, neutral IRQ
stubs, and a canonical early framebuffer bind.

## Automated evidence

The authoritative machine-readable records are:

- `artifacts/build/interrupt-bringup/evidence.tsv`
- `artifacts/build/interrupt-bringup/evidence.json`
- `artifacts/build/interrupt-bringup/source-before.sha256`
- `artifacts/build/interrupt-bringup/source-after-tests.sha256`
- `artifacts/build/interrupt-bringup/source-after-build.sha256`
- `artifacts/build/interrupt-bringup/taskman-protected-before.sha256`
- `artifacts/build/interrupt-bringup/taskman-protected-after.sha256`
- `artifacts/build/interrupt-bringup/bootloader-before.sha256`
- `artifacts/build/interrupt-bringup/bootloader-after.sha256`

The permanent runner is `scripts/test-interrupt-bringup.sh`; the offline
verifier is `scripts/verify-interrupt-bringup.py`. A PASS is derived from an
affirmative scenario marker and its hashed log, never from the absence of a
panic line.

## Gate results

This table is derived from the final evidence ledger. All QEMU rows below use
the same kernel SHA-256,
`ff4f1e543d4f563e1f06006d5ed0fc0b9b8a196b1dc8f8ff6e6157c617026125`.
No exploratory boot or earlier kernel hash is counted.

| Gate | Result | Evidence |
|---|---|---|
| Static contracts and active naming | PASS | `static.log` |
| Bounded host/C model selftests | PASS | `selftest.log` |
| PIC negative | PASS | `negatives/negative-pic/serial.log` |
| IOAPIC negative | PASS | `negatives/negative-ioapic/serial.log` |
| ISO negative | PASS | `negatives/negative-iso/serial.log` |
| HPET early-arm negative | PASS | `negatives/negative-hpet/serial.log` |
| Early-preemption negative | PASS | `negatives/negative-preemption/serial.log` |
| q35 UP/TCG | PASS, 1/1 CPU | `qemu/up-tcg/serial.log` |
| q35 SMP2/TCG | PASS, 2/2 CPUs | `qemu/smp2-tcg/serial.log` |
| q35 SMP4/KVM | PASS, 4/4 CPUs, TASKMAN | `qemu/smp4-kvm/serial.log` |
| q35 SMP8/KVM | PASS, 8/8 CPUs, stress | `qemu/smp8-kvm/serial.log` |
| q35 SMP24/KVM | PASS, 24/24 CPUs, TASKMAN and stress | `qemu/smp24-kvm/serial.log` |
| pc PCAT/TCG | PASS, 4/4 CPUs | `qemu/pcat/serial.log` |
| Focused SMP24 soak, at least 300000 ms | PASS, 734590 ms | `qemu/soak24/serial.log` |
| Stack, deterministic build, dependencies, production image, undefined symbols | PASS | `final-build.log` |
| Early framebuffer pixel verification | PASS | `framebuffer.log` |
| Offline evidence verification | PASS | `report.log` |

## Controller and probe observations

The q35 SMP24 boot reports PCAT compatibility, PIC IMRs `ff/ff`, one IOAPIC
with 24 redirection entries, and `masked=24` at acquisition. The two prepared
routes are ISA keyboard GSI 1/vector 33 and HPET GSI 16/vector 32. The `pc`
scenario independently reports PIC `ff/ff`, a discovered IOAPIC, and a valid
ISA route; no exception vector is used as a legacy IRQ.

The BSP's first deliberate maskable vector is 34. Its LAPIC probe reports
`entered=1 returned=1 period_us=1000`; the HPET probe reports
`entered=1 returned=1`. In SMP24, 24 quiescent LAPIC records precede release,
24 bootstrap handoffs report a valid idle stack, and 24 `CPU_READY` records
report `timer_us=1000 handoff=1 preempt=1`. The terminal IRQ snapshots report
`unexpected=0 imbalance=0`.

The implemented order intentionally completes the BSP voluntary handoff before
publishing the AP release flag. This is the one ordering refinement relative
to the illustrative sequence in the mission: it keeps every AP contained while
the BSP binds its real bootstrap stack, so no newly released AP can interact
with a still-unattached BSP scheduler context. Each AP still activates and
proves its own timer, performs its own voluntary handoff, and enables local
hard-IRQ preemption only after release.

The focused soak performed 19 IRQ checks, 18 task diagnostics, four eight-
worker stress sweeps, and four three-frame TASKMAN sessions. Its final clock
check observed advancing LAPIC counts on every CPU and a valid monotonic HPET;
the final task, input, modal, kill, TASKMAN, accounting, and IRQ checks all
reported PASS.

## Build and negative isolation

All five negative builds emitted only their required causal detector. Each was
followed by a clean normal rebuild and boot containing `[IRQ][SELFTEST] PASS`;
the final kernel contains no negative marker.

The final stack scan covered 89 stack-usage files, found zero violations, and
reported a maximum automatic frame of 1968 bytes against the 2048-byte limit.
The JOBS=2 and native-parallel builds are byte-identical at the kernel hash
above. Dependency checks, production-image policy, FAT creation, and
`nm -u kernel.elf` passed; `SELFTEST=0`, `SELFTEST_AUTORUN=0`, and production
`tasktest=0` were preserved. No warning originates in the new interrupt
controller, bootstrap, journal, scheduler-gate, routing, or framebuffer code.
Warnings visible in the aggregate log are pre-existing diagnostics at
unchanged lines in legacy xHCI, memory, keyboard, shell, bootloader, and linker
configuration; warnings owned by this change are zero.

## Scope and TASKMAN continuity

The additional edits outside the preferred creation list are causal:

- `kernel/src/core/irq_stats.c:irq_stats_record_unhandled` no longer increments
  the total a second time or substitutes vector `0xfe`; the vector-preserving
  journal/common ISR path is now the single causal record.
- `kernel/src/core/switch.S` drops the two specialized timer entries after
  `interrupt_stubs.S` assumes the common external-vector contract.
- `scripts/verify-production-test-policy.sh` validates the deliberate BSP
  probe and runtime-ready wait between the retained production breadcrumbs.

The TASKMAN protected manifest and the complete `bootloader/**` manifest are
byte-identical before and after. Runtime TASKMAN smokes passed under SMP4 and
SMP24, and all four soak sessions reported three full frames, zero fallback
frames, zero stable-frame full clears, and a clean return to the shell. Frozen
visual-release artifacts were not replaced or relabeled: their separate human
review remains outside this interrupt-controller certification.

## Continuity and disposition

TASKMAN protected source and `bootloader/**` compare byte-for-byte against
their before manifests. The raw FAT hash identifies the exact packaged
candidate but is not used as a cross-format reproducibility oracle; the kernel
and all five extracted payloads are compared instead. `SHA256SUMS`, FAT
metadata, the boot signature, commit/tree metadata, and the project review ZIP
are generated by the post-commit `candidate` gate.

Bare metal was not executed. After the exact one-commit package passes the
offline `all` verifier, the disposition is
`READY_FOR_BARE_METAL_INTERRUPT_VALIDATION`; the next owner is William and the
only next action is the first physical boot defined by the operator runbook.
