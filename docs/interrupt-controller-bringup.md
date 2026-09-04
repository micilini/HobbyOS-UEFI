# x86 interrupt-controller bring-up

This document defines the permanent acquisition and release protocol for
maskable interrupts. Firmware controller state is treated as unknown. The
kernel enters this protocol with IF clear and does not expose device services
or hard-IRQ preemption until their consumers and scheduler contexts are ready.

## Why the old boot path could stop at the first interrupt

The physical trace stopped after `before-irq-enable`: the BSP executed `sti`
and the first maskable interrupt did not return. The source audit found the
following concrete contributors in the base tree:

1. `kernel/src/core/kernel_init.c:init_system_core` had no active 8259 driver.
2. `kernel/src/acpi/madt.c:init_madt` did not consume MADT flags or
   `PCAT_COMPAT`.
3. `kernel/src/acpi/madt.h:MadtIsoEntry` existed, but `init_madt` did not parse
   type 2 entries.
4. `kernel/src/apic/ioapic.c:init_ioapic` selected only one IOAPIC.
5. `kernel/src/apic/ioapic.c:init_ioapic` ignored each controller's GSI base.
6. `kernel/src/apic/ioapic.c:init_ioapic` did not quarantine redirection
   entries inherited from firmware.
7. `kernel/src/apic/ioapic.c:ioapic_map_irq` treated an ISA IRQ as a local
   redirection index.
8. `kernel/src/apic/ioapic.c:ioapic_map_irq` always encoded high/edge delivery.
9. `kernel/src/apic/ioapic.c:ioapic_map_irq` did not use a masked-low, upper,
   final-low transaction.
10. `kernel/src/apic/lapic.c:init_lapic` and `init_lapic_ap` did not neutralize
    every supported LVT.
11. The same LAPIC initialization could inherit ExtINT through LINT0.
12. `kernel/src/timer/hpet.c:init_hpet` and
    `kernel/src/drivers/timer.c:timer_init` armed timer 0 long before BSP IF
    release.
13. `kernel/src/apic/lapic.c:lapic_timer_start_periodic` activated local timers
    during per-CPU preparation.
14. `kernel/src/smp/smp_boot.c:ap_kernel_entry` reached `sti; hlt` without an
    interrupt-runtime release barrier.
15. `kernel/src/core/scheduler.c:scheduler_start` published global STARTED
    state while the BSP still executed on its bootstrap path.
16. `kernel/src/core/scheduler.c:scheduler_preempt_from_irq` invoked
    `schedule_impl` without a per-CPU handoff gate.
17. `kernel/src/core/scheduler.c:scheduler_cpu_init_common` installed the idle
    task as current with an unbound `rsp` while the BSP still used its physical
    bootstrap stack.
18. `kernel/src/graphics/graphics.c:init_graphics` and
    `kernel/src/graphics/console.c:console_init` maintained separate
    framebuffer bindings.
19. `kernel/src/graphics/console.c:console_clear` called `clear_screen` before
    the graphics module had a framebuffer.

This is classified as `FIRST_BSP_MASKABLE_INTERRUPT_DOES_NOT_RETURN`. The
watchdog, TASKMAN renderer, PCI scan, GOP selection, CPU count, and 1 ms LAPIC
period are not used as substitute explanations.

## Canonical state machine

`irq_bootstrap_state_t` advances exactly once through:

```text
OFF
  -> CONTROLLERS_QUIESCENT
  -> ROUTES_PREPARED
  -> CPUS_PREPARED
  -> BSP_LAPIC_VERIFIED
  -> HPET_CLOCKSOURCE_VERIFIED
  -> CLOCKEVENT_ACTIVE
  -> SERVICES_ACTIVE
```

An invalid jump, duplicate transition, or regression publishes terminal
`FAILED`. Transition data and per-CPU readiness use storage sized from the
discovered topology. The historical global topology ceiling remains, but the
new registries do not add a CPU-count constant.

## Controller acquisition

`madt.c` performs a checksum-checked bounded walk. It records the table flags,
PCAT declaration, LAPIC address override, enabled Local APIC and x2APIC CPU
entries, all IOAPIC descriptors, all ISA source overrides, and Local APIC NMI
metadata. Reserved polarity or trigger encodings, malformed bounds, duplicate
enabled APIC IDs, conflicting overrides, and conflicting LAPIC address
overrides are rejected.

`legacy_pic.c` writes `0xff` to both IMRs and verifies readback. A PCAT
declaration makes this mandatory; without PCAT the same operation is recorded
as a defensive mask attempt. The PIC is never selected as a normal source and
LINT0 remains masked.

`ioapic.c` maps every MADT controller, reads IOAPICVER, derives its GSI range,
and rejects overlap. Every redirection entry is masked and read back before a
known route is prepared. Route preparation resolves controller and pin from
the GSI and writes masked lower, upper destination, then masked final lower.
Only a separate release writes the unmasked lower word. Destinations above 255
are rejected until a reviewed interrupt-remapping or compatible destination
mode exists.

`lapic.c` clears ESR, installs a known spurious vector, raises TPR during
quarantine, and masks Timer, Thermal, Performance, LINT0, LINT1, Error and CMCI
when the reported LAPIC version supports them. MADT NMI entries are retained as
metadata; there is no implicit LINT1 NMI policy in this release.

## Clocksource and clockevents

HPET main-counter setup is a clocksource operation. `init_hpet` validates the
period, disables legacy replacement and timer delivery, clears pending status,
starts the main counter, and does not arm comparator 0.

HPET timer 0 is not a production clockevent. It remains interrupt-disabled,
non-periodic, non-FSB, non-pending, outside legacy-replacement mode, and without
an active IOAPIC route. A bounded counter-progress probe verifies the HPET
clocksource without enabling IF or programming a comparator.

Each CPU calibrates its LAPIC timer independently for 1000 microseconds.
Calibration and periodic preparation leave the LVT masked and the initial
count at zero. Activation reloads the calibrated count immediately before
unmasking.

## Probes, journal, and release

The BSP deliberately enables only its prepared LAPIC timer, with APs contained
and IRQ preemption disabled. The lockless per-CPU journal must observe vector
34 enter and return in balance. The HPET proof reads only the running main
counter and confirms Timer0 quiescence; vector 32 is not a boot prerequisite.

The common external-vector stubs preserve vectors 32 through 255 without
changing exception stubs 0 through 31. Entry and return counters, first and
last vector, depth, mismatch, underflow, and unexpected delivery require no
heap, clock, console, or global handler lock. The LAPIC timer completes handler
work and EOI, closes journal accounting, and publishes a one-use preemption
epilogue token. The scheduler consumes that token before it may switch. This
prevents a newly selected task from inheriting the interrupted task's journal
depth.

After the LAPIC IRQ proof and HPET clocksource proof pass, the BSP installs its
LAPIC as the single global clockevent, attaches the real bootstrap stack to its
idle task and performs a voluntary switch outside interrupt context. It then
enables its per-CPU IRQ-preemption gate. Only after this does the BSP release
the AP barrier. Each AP activates its local timer, proves one enter/return,
performs the same voluntary handoff, enables preemption, and publishes runtime
readiness.

## Service staging and MSI

`CLOCKEVENT_ACTIVE` permits scheduler timing and wake timers driven by the BSP
LAPIC. HPET Timer0 is never rearmed.
Device-facing deferred work returns early until `SERVICES_ACTIVE`. The keyboard
route is prepared from MADT source IRQ 1 and remains masked until service
release. PCI writes xHCI MSI address/data with the enable bit clear; xHCI rings,
event state, and handler readiness are established first. MSI enable occurs
only after the service state is published.

## Early framebuffer ownership

`graphics_bind_framebuffer` is the single canonical framebuffer bind.
`console_init` calls it before the first clear, and later `init_graphics` is
idempotent. The first clear verifies a framebuffer pixel and emits the early
bind marker. Pixel-only host verification compares the UEFI, early-clear, and
final-shell PPM captures without OCR.

## Diagnostics and limitations

The permanent `irq` command provides `check`, `controllers`, `routes`, and
`boot` snapshots. `irq check` validates PIC masks, MADT/ISO resolution,
IOAPIC quarantine, known routes, LAPIC LINT policy and 1 ms timers, HPET state,
all CPU handoffs, preemption gates, journal balance, probes, and zero unexpected
vectors.

Known limitations remain explicit: the topology ceiling is still
`HOBBYOS_MAX_CPUS=32`; IOAPIC physical destination mode cannot represent APIC
IDs above 255; parsed MADT NMI metadata is not yet activated; CPU hotplug,
interrupt remapping, NUMA policy, and topology-aware scheduling remain outside
this change.
