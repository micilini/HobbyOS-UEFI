# HPET clocksource and BSP LAPIC clockevent

HobbyOS deliberately separates reading time from generating timer events. The
HPET main counter is the monotonic clocksource. Every CPU retains its calibrated
1000 us periodic LAPIC timer for local accounting and preemption. Only the BSP
LAPIC tick advances the global software-timer queue and service polling.

This design follows the clocksource/clockevent separation used by mature x86
kernels as a principle. It is not a line-by-line port of Linux HPET or APIC
code.

## Runtime ownership

```text
HPET main counter
  -> monotonic time and LAPIC calibration
  -> no required interrupt

LAPIC timer on every discovered CPU, period 1000 us
  -> local scheduler accounting and preemption

LAPIC timer on the BSP
  -> the single global clockevent
  -> software timer polling
  -> shell/xHCI deferred polling after service release
```

The invariant is one active global clockevent with source `BSP_LAPIC`.
Non-BSP CPUs never enter the global polling path. Both the explicit owner
branch in the LAPIC handler and the clockevent API validate this rule.

## HPET Timer0 quiescence

`init_hpet()` validates the advertised period, resets and enables the main
counter, disables legacy replacement, and leaves comparator Timer0
quiescent. `hpet_timer0_force_quiescent()` is idempotent and preserves the
running main counter while it:

- clears interrupt, periodic, set-value, 32-bit, route, and FSB-delivery bits;
- clears Timer0 pending status by write-one-to-clear;
- masks any previously prepared IOAPIC route;
- confirms global, timer-config, and pending-status readback.

The permanent runtime snapshot reports counter enablement, legacy mode,
Timer0 delivery/configuration, route state, pending state, and stray IRQ
quarantine counters. A prepared historical HPET route is acceptable only if
it remains masked; production does not prepare one.

Vector 32 remains installed as a safety boundary. Unexpected HPET delivery is
recorded, Timer0 is forced quiescent, its route is masked when known, pending
status is cleared, and LAPIC EOI is issued. The handler does not read the
clocksource, poll timers, call the scheduler, rearm a comparator, allocate, or
print. Any stray count makes `irq check` fail even when quarantine returned.

## Bounded clocksource proof

The BSP proves the HPET counter with IF clear and APs contained. The proof
first forces Timer0 quiescent, captures the runtime configuration, then makes
at most 4096 counter samples with `pause` between reads. It succeeds only when
ticks and converted nanoseconds increase and Timer0 remains non-pending and
quiescent. Because the loop has a fixed iteration bound, a stalled HPET cannot
turn this check into an unbounded wait and the candidate never depends on an
HPET interrupt.

The interrupt bootstrap state order is:

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

Invalid, duplicate, skipped, or regressing transitions terminate the bootstrap
in `FAILED`.

## BSP clockevent path

With IF clear, the BSP first proves a deliberate LAPIC vector 34 enter/return.
After the bounded HPET counter proof, `timer_clockevent_configure_bsp_lapic()`
records the discovered BSP slot and the fixed 1000 us period.
`timer_clockevent_activate()` requires the scheduler, both proofs, and Timer0
quiescence before publishing the sole global clockevent.

The LAPIC handler reads `clock_monotonic_ns()` once and reuses that timestamp
for accounting and the BSP clockevent. `timers_poll_at(now_ms)` preserves the
existing timer claim, cancellation, wait-generation, task-reference, wake,
callback, and reaper contracts without a second HPET read.
`timer_run_deferred_at(now_ms)` similarly reuses the timestamp and remains
gated until `SERVICES_ACTIVE`.

Clockevent telemetry is copy-only and bounded: configured/active state,
source, owner slot, period, total ticks, last timestamp, software and deferred
poll counts, early attempts, non-BSP attempts, and timestamp regressions. It is
reported by `irq boot`; no per-tick logging occurs.

## CPU and service release

APs retain the interrupt barrier with IF clear. After BSP clockevent
activation and the BSP voluntary scheduler handoff, each AP activates and
proves its own LAPIC timer, attaches its bootstrap context outside hard IRQ,
enables its local preemption gate, and publishes runtime readiness. AP LAPIC
ticks perform local accounting/preemption only.

Software timers can advance once the clockevent is active. Keyboard delivery,
shell cursor work, xHCI polling, and other external services remain unavailable
until all discovered CPUs are runtime-ready and `SERVICES_ACTIVE` is
published.

## Diagnostics and limits

`irq check` proves HPET clocksource availability, Timer0 quiescence, the sole
BSP LAPIC clockevent, a 1000 us period, zero early/non-BSP ticks, zero timestamp
regressions, zero stray HPET IRQs, and the existing controller, route, CPU,
handoff, preemption, and journal contracts. `irq boot`, `irq controllers`, and
`irq routes` expose the corresponding read-only snapshots.

This change does not add TSC timekeeping, TSC deadline mode, tickless
operation, HPET broadcast delivery, CPU hotplug, interrupt remapping, dynamic
topology storage, or a new scheduler policy. The historical CPU ceiling and
physical IOAPIC destination limitation remain documented constraints; no CPU
cap, E-core filter, Hyper-Threading filter, or BIOS workaround is introduced.

## Virtual LAPIC-rate measurement authority

The functional SMP gate and the virtual cadence measurement answer different
questions. SMP24/KVM remains mandatory for CPU readiness, handoff, timer
liveness, software timers, scheduling, and interrupt balance. The historical
`accounttest lapic-rate 2000 5` limit also remains unchanged at
`700..1300` thousandths of wall-clock cadence.

The rate result is authoritative only when the host can schedule at least as
many CPUs as the guest exposes. The harness computes
`host_schedulable_cpus = min(nproc, Cpus_allowed_list count)` from read-only
host metadata. If that count is below the SMP24 guest count, a raw rate FAIL
remains a raw FAIL and is recorded as
`ENVIRONMENT_NON_AUTHORITATIVE_OVERSUBSCRIBED`; it is never relabeled PASS.
The functional scenario may continue only when every vCPU advances and all
configuration, liveness, clock, interrupt, HPET-quiescence, and runtime checks
pass without faults.

Three predetermined SMP4/KVM boots provide the non-oversubscribed cadence
control. Each runs config, liveness, the unchanged rate command exactly once,
accounting validation, and IRQ validation. All three must pass; no fourth boot,
CPU pinning, realtime priority, threshold change, or opportunistic retry is
permitted. This control does not replace the functional SMP24 gate.
