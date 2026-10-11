# Heap operation boundaries and guest RAM backing

The existing `continuous-mask-heap-stages-v1` records are observer-inclusive.
A marker's timestamp precedes its validation and publication. Its remaining
work therefore belongs to the next reported interval. A hosted actual-controller
regression reproduces a 500,000-cycle synthetic delay there without any heap
operation. This establishes attribution ambiguity, not the cause of a guest delay.

`heap-metadata-callsite-v1` adds a separate diagnostic record. Under the existing
heap lock, a scalar token captures CPU, epoch, mask segment, stack owner and
generation, switch/IRQ/fault sequences, lock and depth. After the preceding marker
returns, two inline ordered clocks enclose the original split, new header,
list linkage, allocation state and used-byte accounting. The compiler schedules
the pure return-pointer address arithmetic outside this interval; it is not
claimed as part of the emitted operation bracket.
The controller validates and publishes after the second clock. Its open slot is
owned only by that continuously masked CPU; no stack pointer is retained. Nested
preparation, missing publication, reused sequence or changed context invalidates
proof. Completion retains parent endpoints and an independent operation maximum.
Metadata-event counts from allocation/recheck records must equal operation counts.
All storage is fixed and debug-only. Reporting requires frozen CPUs and no open
operation. The whole IRQ-off recorder still encloses observer preparation and
publication; its endpoints, maxima, allowance and lifetime protocol are unchanged.

The observation includes its declared clock boundaries. Both C and assembly retain
`MFENCE; LFENCE; RDTSC; LFENCE`: local ordering, ordering of preceding loads, and
waiting for visibility of preceding stores. The latter can include work initiated
before the operation. The record is not pure uninstrumented instruction time.
The 32-byte field counts only a newly created header, not every store. Host pauses
inside the bracket remain included. The minimum uses the existing pre/post upper
frequency bound with reserve and downward integer rounding. A short operation
never establishes a passing whole-window bound.

The finite RAM comparison uses one identified diagnostic candidate twice. Both
runs declare anonymous `memory-backend-ram`, 2 GiB, Q35/KVM, eight vCPUs, default
placement, identical firmware, ordered clock, optional-detail policy and command
sequence. Only `prealloc=off` versus `prealloc=on` changes. Installed QEMU help,
actual argv and QMP properties verify the backend; current documentation alone
is insufficient. Startup work is recorded separately. Admission reserves 2 GiB
for guest RAM, 1 GiB for QEMU/device/translation overhead and 4 GiB host memory
headroom, independently of existing disk provisions. There are no unchanged
retries, host tuning, privilege changes or additional acceptance successes.

Process/thread minor and major faults, RSS, cgroup memory/scheduling state and
monotonic collection intervals are retained at start, guest readiness, activation,
stress completion and terminal collection. Missing observations are unavailable.
These counters support controlled association only: they neither timestamp one
guest operation nor enumerate every KVM/EPT event. Zero faults do not exclude
interference. No host time is subtracted. Historical rejected envelopes remain
rejected, and preallocation cannot silently change the final timing profile.

Conceptual references, with no implementation copied:
[QEMU invocation](https://www.qemu.org/docs/master/system/invocation.html),
[KVM timekeeping](https://docs.kernel.org/virt/kvm/x86/timekeeping.html), and
[Linux v6.12 ordered TSC helper](https://raw.githubusercontent.com/torvalds/linux/v6.12/arch/x86/include/asm/msr.h).
