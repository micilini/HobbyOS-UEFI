# Qualified monotonic clock sources

This successor separates production elapsed time, the unchanged 1 ms LAPIC
clock event, and the independently calibrated IRQ observer. It does not change
the 50,000 ns strict budget or any enclosing observer endpoint. The first fast
provider is KVM system time; native TSC as a global clock is not implemented.

## Motivation and reference boundary

The reconciled 917-file laptop source matches the desktop-tested tree
`dddd7be56173c33f4ac5c24dc00d74861d5700a6`. The desktop's CPU0 heap maximum was
an enclosing xHCI interrupt containing an actual DPC allocation. Its label is
not exclusive allocator attribution. The distinct CPU3 metadata operation was
133,630 ns minimum, with software versus host interference unassigned.

The subsequent stack-coverage candidate
`f3fe52dede4a9dcbf49125780fbc247f2d0b2e0391b3e5823eb20e8c4d408829`
exercised all 88 required CPU/path cells. Its CPU0 timer record contains
857,263 cycles inside one HPET counter read: 324,404 ns minimum using the
qualified 2,642,577,475 Hz upper bound. Its distinct CPU7 metadata operation
was 2,998,783 ns minimum. Removing recurring device reads is causally motivated
by the first observation; it is not a diagnosis of the second. This boot still
failed observer qualification on CPUs 6 and 7 and had 43 over-budget cells.

## Provider and lifetime

HPET remains mandatory, available, quiescent as a clock event, and the initial
and qualification-failure provider. KVM system time is selected only at the
boot barrier, before runtime request admission, after every discovered CPU has
registered and qualified its own page. TCG and unsupported KVM use HPET without
executing the KVM MSR. No live provider transition is supported: after a selected
fast provider fails, checked reads fail permanently for this boot. No in-flight
deadline is extended by a fallback or a fresh epoch. Reboot is required to
reselect a provider. HPET fallback is functional, not a timing qualification.

The public KVM ABI is detected through the hypervisor signature, the advertised
CPUID range, system-time bit 3 and stable-time bit 24 of leaf 0x40000001.
Ordered TSC and MSR capability are checked separately. Each CPU registers a
static aligned RAM page through the existing exact-fault-safe MSR interface.
Each 32-byte ABI object occupies a separate 64-byte aligned slot in one retained
page-aligned array; it cannot straddle a 4 KiB page. No full page per CPU is
required by the ABI. The first provider is restricted to Intel ordering support;
other vendors retain HPET until their counter-ordering contract is qualified.
Physical translation is checked. Registration and readback occur on the owning
CPU under exclusion. Pages remain reserved for the kernel lifetime, including
failed or uncertain unregister: no allocator, task or stack owns them.

Readers use at most four even/equal version attempts. They neither wait for a
hypervisor writer nor spin on a guest writer. CPU pinning is the existing IRQ
save boundary, not Linux's preemption-disable assumption. A one-attempt owner
guard rejects a reader interrupting the same CPU's active read. Different CPUs
use different page and history storage. Unknown CPU, invalid scale, unsupported
flags, guest-stop indication, inconsistent version, overflow, backward/stalled
local counter or a changed scale invalidates the read before publication.

## Qualification and ongoing reference work

Each CPU retains four fixed HPET reference points around three nominal 20 ms
intervals. Each point brackets the real HPET sample with two consistent KVM
samples. A point's bracket must be at most 100 us. Reference intervals must be
10..100 ms and their intervals must agree within 10,000 ppm. Every point is
retained; none is selected for a favorable latency. A bounded polling limit
fails qualification if the proposed source cannot advance.

Method version 3 projects each HPET-minus-KVM offset interval to the first BSP
reference's HPET epoch. The projection adds the already admitted rate reserve,
`ceil(abs(point_ns - origin_ns) * 10000 / 1000000)`, to the interval's existing
rounded-up HPET tick uncertainty. All projected intervals must intersect.
The immutable midpoint aligns the origin once before request admission; it is
not an assertion that the two independent clocks have exactly equal rates.
KVM's advertised stable-time capability supplies the cross-vCPU monotonic
contract; the independent points check compatibility rather than establish a
native TSC synchronization guarantee. Per-CPU rate parameters remain fixed. Counter update
anchors must agree with that CPU's frozen interpolation within a documented
rounding reserve; changed scale or an incompatible anchor is not silently
recalibrated. Clock state records source and epoch; HPET raw ticks are never
relabelled TSC ticks.

An independent HPET comparison remains compulsory once one second has elapsed
since the CPU's last reference. Successive same-CPU HPET and KVM elapsed
intervals must agree under the same 10,000-ppm rule. The entire interval of
possible KVM elapsed time, including both endpoint brackets, must fit that
rate range for every HPET elapsed time allowed by two ticks of uncertainty.
An overlap alone is insufficient. This runtime reference uses the measured
uncertainty, not the separate 100-us bootstrap admission limit. It can therefore
validate a slow reference while still recording that reference as an IRQ
budget violation. A more precise later
reference need not contain the chosen origin midpoint. The check never changes
that midpoint or resets an in-flight deadline. The maximum reference retains
its matching previous reference for independent rate reconstruction.
This work remains inside the caller's actual
IRQ boundary when the caller is masked. Its raw counts, failures and maximum
bracket are retained. It can itself fail the latency budget; moving it into a
maintenance label cannot make the observation pass. A fast-source pause or
scale change cannot suppress the check: local counter progression and anchor
consistency are checked first, and a long elapsed gap requires the reference.

The first provider candidate (`e64229c3cdd27c36f7391d9abb33e215729ed214178e2720d765a537e7b0fa7e`)
panicked before runtime readiness. Its exact failure reason was not captured.
A separate stopped diagnostic selected HPET fallback and did not reproduce the
panic. An actual-controller regression independently demonstrated a reference
model defect: clocks with a valid 100,000 ns relative origin were rejected when
a later bracket narrowed and excluded the chosen 95,000 ns midpoint. This
proves the defect, not the cause of that particular guest panic. Version 1
raw records keep their original interpretation and failed verdicts.
Version 2 temporarily emitted a failure record after invalidation. The current
provider retains the failure in RAM without formatting or output: an
asynchronous reader must not wait on the serial lock of an interrupted writer.
The existing caller's panic policy remains unchanged.
The version 2 candidate `9c15bac4718923b1da2f15490a488828070004a3b44fb4ae237d70ce1be7fff7`
reported CPU3 reference refusal with a 124,594 ns KVM bracket. This exceeds its
old 100-us point cutoff but does not itself prove rate disagreement. The line's
tail was interrupted by panic output, so it is not a complete clock proof.
Actual-controller tests reproduce this cutoff rejection and require version 3
to admit the full rate-consistent interval. A 20-ms uncertain bracket and an
over-limit rate still reject. No reference duration is subtracted or omitted
from outer IRQ measurements. Neither failed candidate supplied timing success.

## Units, publication and failure

KVM interpolation uses the ABI's shift and multiply, rounded down to integer
nanoseconds. All shifts, products, additions and offset signs are checked.
The common offset changes the origin, not elapsed duration. No steal time or
host descheduling is subtracted. Checked clock APIs leave output untouched on
failure. APIs unable to return a validity bit fail closed after initialization;
they cannot turn a failed source into a usable cached constant. Request start,
wait and final completion still test real elapsed nanoseconds against the same
one-second deadline, and uncertain completion is quarantined.

The global monotonic floor handles a genuinely older concurrent sample, while
local progression is tested independently first. A floor cannot make a stopped
or backward local clock valid. Publication has a fixed attempt bound and fails
closed on exhaustion; it never waits for a lock holder or resets a deadline.
HPET also rejects 64 consecutive equal local samples; this is a bounded
failure detector, never a substitute elapsed-time clock. Before initialization,
the legacy display API retains its explicit zero/unavailable boot value and
the checked API fails. Runtime deadlines are admitted only after initialization.

## Independent tests and retained scope

Actual C tests substitute only CPUID/MSR/IF/TSC/HPET/physical-translation seams.
Arithmetic vectors have independently computed exact results. Tests cover torn
updates, bounded odd versions, migration/wrong slot, interrupted readers,
registration and readback faults, retained pages, source failure during a
request, scale/offset disagreement, jumps/stalls/overflow and failure after an
earlier valid read. Existing HPET safety, 64/65-page invalidation, WBINVD,
completion/quarantine, observer and deferred-proof tests remain required.

The new provider is production work, not optional timing telemetry. Linked
images, native roles and runtime results must be rebuilt. Historical captures
retain their own source and schemas. Source identity, provider selection and
all reference points must accompany a current result. A short local read or a
successful provider boot never establishes complete IRQ timing acceptance.

## Conceptual sources

The Linux v6.12 `kvmclock.c`, `pvclock.c`, `timekeeping.c` and `hpet.c` informed
the separation of clock events and elapsed time, stable per-vCPU interpolation
and reference checking. The independently written HobbyOS implementation does
not adopt Linux's full clocksource framework, unbounded snapshot retry,
watchdog forgiveness after a host pause, preemption-only pinning or allocator.
The KVM shared layout, MSR and feature numbers are interface declarations.

- [KVM system-time ABI](https://docs.kernel.org/virt/kvm/x86/msr.html)
- [Pinned ABI declarations](https://raw.githubusercontent.com/torvalds/linux/v6.12/arch/x86/include/asm/pvclock-abi.h)
- [Pinned KVM clock integration](https://raw.githubusercontent.com/torvalds/linux/v6.12/arch/x86/kernel/kvmclock.c)
- [Linux timekeeping responsibilities](https://docs.kernel.org/timers/timekeeping.html)
- [Sequence counter constraints](https://docs.kernel.org/locking/seqlock.html)
- [KVM timekeeping limitations](https://docs.kernel.org/virt/kvm/x86/timekeeping.html)

The documentation's abbreviated numeric examples are not the oracle for the
CPUID leaf or feature mask. The ABI declarations above resolve them. None of
these references promises a universal 50 us host scheduling bound.
