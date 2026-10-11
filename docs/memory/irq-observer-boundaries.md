# Interrupt observer boundary qualification

The observer corrections remain unaccepted runtime instrumentation. The strict
50,000 ns budget, the factor-two pre-stress allowance, and the original clock
tolerances are unchanged. The accepted source remains commit
`d68a3b4a2c97f55dfdcc445bfe42317e09e1c55e`. Historical 23/25-cell captures retain
their diagnostic role. Schema two uses 29 cells; it cannot be inferred from a
historical record merely because a newer checker can read that record.

## Emitted boundary audit

The first diagnostic correction was built as ELF
`e77f1357944910f6e80e96d7dd7f715c6f9e7ddc85fb0ecd39a80f4fe40a5f31`.
Its immutable experiment plan retains before/after disassembly for 13 symbols.
The following table describes actual instruction order. Addresses are anchors
for that ELF only; subsequent record-layout corrections require fresh codegen.

| Path | Instruction boundary and IF | Accounting |
| --- | --- | --- |
| Software save | PUSHFQ captures incoming flags before CLD and allocation of the 96-byte boundary object. `before_mask` reads architectural identity and entry/switch sequences. MFENCE/LFENCE/RDTSC/LFENCE precedes CLI. | The timestamp encloses CLI. Before-mask work is outside a newly started mask, but remains inside an already active outer IF=0 interval. |
| After CLI | Store the timestamp, call `after_mask`; independently sample CPU, validate the pre-CLI identity or a real previous local IRQ exit, drain pending records, begin/tag the outer interval. | All work after CLI is included by the saved outer start, including nested drains. It is not subtracted. |
| Software restore | `before_restore` validates IF and owner generation, closes into permanent pending storage; POPFQ restores saved IF; ordered RDTSC captures the raw end; `after_restore` checks identity and publishes completion; final POPFQ preserves the exact caller flags. | Pre-POPFQ work is included. Post-POPFQ publication is outside the completed mask. An intervening IRQ provides a genuine same-CPU fallback ending. |
| External entry | Vector push, 15 register pushes, 8-byte pad, readiness test, ordered TSC at `ffffffff820c5cc2`; C entry call at `ffffffff820c5ce9`. | Architectural entry and stub prefix precede the inner timestamp and require the frozen enclosing allowance. CPUID in the C hook follows this timestamp. |
| External interior | C entry identity, scope barrier and drain; dispatch, callback/EOI and potential scheduler handoff; C exit identity, scope restoration and pending preparation. | Included in the inner span. A switch does not terminate continuous CPU-local masking; a new task's first enable closes it. |
| External tail | Ordered RDTSC at `ffffffff820c5d31`, scalar stores and READY publication, register restoration, IRET. | Stores and IRET follow the inner end and require the frozen allowance. |
| Empty probe | IF restored before outer read at `ffffffff82081e4a`; arithmetic/store then INT FF at `ffffffff82081e5b`; after IRET, fences and outer read at `ffffffff82081e63`. | Leading/trailing enclosure contains IF-enabled instructions as well as the missing masked margins. It is conservative observation, not exact architectural entry cost. |
| Ticket acquisition | Wait hook reads CPU once and retains a local token; ticket hook retains its independent lookup; acquired hook validates token, IF, outer sequence, epoch, entry/switch counts, nesting depth and lock. | One CPUID removed from acquired; no timestamp moved inward, no lock algorithm changed. The outer span still includes all lookup, spinning and observer work. |
| Snapshot | Eligible LAPIC entry drains completed storage; only with no active scope, open mask, pending publication or lock does one snapshot copy freeze that CPU. | Snapshot copy duration is reported separately. No assertion that a snapshot contains its own unfinished request is permitted. |

No identity cache crosses an IF-enabled return. The acquisition token is an
automatic value in the continuously masked call chain, and is discarded before
restore. Existing NMI handling in `exc_isr2` enters panic and does not resume
ordinary acquisition. This correction adds no NMI-return protocol, GS/AUX/MSR
programming or CPU pinning. Hosted tests reject simulated entry/switch changes,
IF enable, stale outer sequence and stale epoch. Existing one-past-stack address
and terminal task handoff behavior are retained.

`record_drain` remains a bounded scan of 64 pending slots, with at most 11 path
updates per completed interval and at most one optional failure copy. It does
not format, allocate or wait for the host. This is bounded work, not zero cost.
The optional failure capacity remains 2,048 per CPU; saturation does not stop
counts or maxima. Saturation prevents a complete failure chronology claim.

## Distinct defects and observations

The original CPU0 heap witness used 22,720,418 raw cycles plus 3,852 allowance.
Removing that allowance and consistently updating arithmetic and CRC formerly
passed both real checkers. The corrected checker derives allowance from CPU,
pre-calibration and primitive endpoints. Nine historical mutations now reject,
with restored originals, without promoting the original failed series.

A new actual-controller test also reproduced lost allowance in a nonreturning
IRQ-origin publisher: `before_restore` selected 40 injected cycles, but retirement
retained zero. The correction stores the allowance in permanent pending storage
before POPFQ. Retirement preserves it and records the exact owner slot,
generation and pending sequence observed by the retirement call. Schema two
checks these against the close witness, and checks the close allowance against
the independently derived per-CPU rule. Hosted generation-reuse tests retain
the first captured IRQ fallback rather than reading time from a reaped stack.

The old CPU3 probe still has leading/interior/trailing durations
18,162 / 13,024 / 508 cycles. Its 18,670-cycle enclosing cost exceeds the frozen
3,596-cycle allowance. No cause has been established for that event.

The first finite corrected-observer experiment consumed one Q35/KVM/SMP8 boot.
It stopped before activation: CPUs 2 and 7 supplied only one and two eligible
disjoint clock windows. The fixed requirement is three. No stress or post-stress
sample was collected. Pre-calibration also retained large tails on CPUs 5 and 7
(33,626 and 31,270 cycles). These observations show that enclosure spikes can
occur before the memory stress; they do not identify host preemption or any
other sole cause. The ticket-token reduction is inactive during this admission
phase, so that run did not measure its performance effect.

Independent rejection recount found 396 windows with different begin/end CPU
identities, 60 rejected for frequency interval dispersion, and two with an
ineligible reference endpoint. The recorded 512 attempts remain intact. These
facts explain this boot's admission refusal; they do not turn failed windows
into calibration samples or identify an individual host interruption.

The corrected 29-cell ELF is
`889f3f4677e3e393ba1c28b118ee6132e499e8b3cf80a517e61881a70b2a2cb9`.
It passed the canonical kernel and image build, the 2,048-byte frame limit,
24 observer hosted executions, and 15 local paired production-unit comparisons.
The inherited 111 hosted executions also passed on the current source.
No VM ran this later ELF. Three additional raw mutations reject erased maxima
with nonzero valid counts, nonempty maxima with zero counts, and invalid counts
larger than total counts.

The explicit `HOBBYOS_MEMORY_OBSERVER_ROLE=irq-off-latency` adapter requires the
inherited guarded-stack role and anchors its scope at the approved commit.
Without that selector, historical source and build predicates are unchanged.
Six actual selector/source mutation tests passed. This adapter is a prerequisite
for the current canonical gate; it does not establish the unfinished full
latency campaign or its aggregate. No production optimization is justified by
the rejected series, and no completion commit has been made.

The host collector now retains collection bounds, process/thread start times,
ancestor CPU pressure and explicit unavailable facts. Eleven synthetic tests
exercise counter reset/wrap, identity reuse, ordering and missing interfaces.
These counters have aggregate authority only. Tracefs access was denied; no
privileged trace, affinity, quota or host configuration was changed. The first
experiment lacked a terminal host snapshot after activation failure, which is
explicitly unavailable rather than a zero delta.

Intel documents CPUID-induced VM exits in VMX non-root operation in
[FlexMigration section 2.2](https://www.intel.com/content/dam/www/public/us/en/documents/application-notes/virtualization-technology-flexmigration-application-note.pdf).
The [KVM timekeeping discussion](https://docs.kernel.org/virt/kvm/x86/timekeeping.html#virtualization-problems)
explains why guest interrupt masking does not prevent host scheduling delays.
These references establish perturbation risks, not an attribution of either
captured spike. No host delay or minimum instruction cost is subtracted.

## Portable comparison

`irq_latency_production.py` validates the immutable recorded command/source
facts separately from reviewer-local paired compilation. It reconstructs the
759-file approved baseline tree, compiles all 15 claimed units on both sides
with the same local compiler and production options, and writes only disposable
object/dependency paths outside source. Both hashes and compiler components
are recorded. A supplied complete original toolchain fingerprint is required
for exact reproduction; the older receipt does not provide one.

The successor replay completed the original diagnostic subset with all 1,736
payloads unchanged: 21 hosted executions, 29 raw, 24 workload and 15 host-capacity
negatives, and 15 equal local production pairs. This leaves the reviewer's
original nonzero integral result intact. Neither paired equality nor diagnostic
replay establishes a complete production image or full current acceptance.
