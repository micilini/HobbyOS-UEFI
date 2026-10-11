# Request duration telemetry

The `request-local-cycles-v1` successor removes observational HPET samples
from the request callback, local TLB/cache operation and initiating send
sections. It does not change `clock_monotonic_ns()`, admission, stall detection,
the one-second absolute deadline, terminal elapsed nanoseconds or quarantine.
The system clock still masks interrupts internally and remains covered by the
outer SHOOTDOWN observer. Historical HPET records retain their original schema.

Debug and SELFTEST record ordered local TSC endpoints with the existing
`MFENCE; LFENCE; RDTSC; LFENCE` primitive. The claimed callback's CPU/APIC,
generation and physical root bind its four timestamps. The handler bracket
starts after claim and capability preparation, includes the existing callback
observer and local operation, and ends before validation, sample publication,
ACK, EOI and architectural return. The inner bracket begins after CR3 sampling and the physical-root precheck.
It surrounds operation dispatch, CR4/strategy checks, actual operations and
the existing diagnostic hooks. It is
not a complete interrupt-unavailability measurement. No local duration is
reported as nanoseconds in the guest.

Each initiating section independently acquires its actual CPU identity under
its existing IRQ exclusion. Its two endpoints enclose the local call or ICR
submission. They exclude identity/capability preparation and final telemetry
validation, which remain inside the unchanged outer IRQ-off measurement.
Migration between sections is allowed. No timestamps from different CPUs are
subtracted or compared to select a duration maximum.

Capability is checked once on each participating CPU before executing TSC.
The cache contains capability only, not a task identity or clock offset. The
request service does not enable interrupts, block or change context between
these endpoints. The sample retains both actual IF observations. Generation,
root, CPU, APIC, epoch and endpoint order are checked before publication.
Unavailable TSC, invalid context/identity/order and incomplete capture have
distinct states. Both epoch observations are retained; an epoch crossing is
invalid auxiliary timing without failing the real operation. Epoch zero means cycles without latency calibration; it is
not zero elapsed time. Functional success never depends on optional timing.

Targets finish scalar telemetry before the existing release ACK. The
initiator acquires completion before copying it. SELFTEST retains one bounded
entry per existing audit generation and target, outside IRQ exclusion; a
missing ACK leaves an explicitly incomplete entry. Audit exhaustion remains a
failed audit. No task/stack pointer is retained. The reporting consumer copies
one entry at a time; there is no large automatic array or masked history drain.

Production omits optional timestamps. Both production and debug preserve real
INVLPG/CR3/WBINVD operations and state-machine completion ordering. The complete
outer observer remains the sole whole-window timing authority. Any later host
conversion must use that CPU's source-bound pre/post frequency model, checked
upward arithmetic and the actual calibration epoch. A local statistic cannot
establish a passing path or waive observer qualification.

The first after experiment is one ordinary Q35/KVM/SMP8 diagnostic boot, 2 GiB,
default placement and unchanged workload, reference effort, detail policy and
outer primitive. Its before evidence is the reviewed request 2496 capture,
not a newly executed baseline. No preallocation or trace change is combined
with this correction. A second diagnostic requires a different predeclared
discriminator or corrective source change.

The state-machine source is unchanged. Its production object changes because
retired auxiliary fields shrink the sample structure; debug adds typed cycle
fields instead. The request result keeps its 64-byte layout and its elapsed
nanoseconds, with the retired initiator duration replaced by a timing-schema
identifier. No old numeric predicate consumes this identifier. TLB schema 7
requires separately bound raw LOCAL records; schemas 1–6 keep their HPET rules.
The two service units and this header-driven state-object change are declared
semantic production differences, alongside the earlier heap/PMM/bitmap changes.

The debug audit adds 160 bytes per declared generation/target: 41,943,040 bytes
at 8,192 generations and 32 CPUs. This fixed SELFTEST-only storage preserves all
local and initiating endpoints without selecting favorable samples or draining
history in IRQ context. Production has neither this array nor these timestamps.
The full bound, rather than an observed eight-CPU subset, is part of admission.

Conceptual references inspected: [QEMU 8.2.2 HPET device model](https://raw.githubusercontent.com/qemu/qemu/v8.2.2/hw/timer/hpet.c),
[KVM MMIO API](https://docs.kernel.org/virt/kvm/api.html),
[KVM timekeeping](https://docs.kernel.org/virt/kvm/x86/timekeeping.html), and
[Linux v6.12 ordered-read helpers](https://raw.githubusercontent.com/torvalds/linux/v6.12/arch/x86/include/asm/msr.h).
They explain possible emulation and scheduling contributions; they do not assign
an individual captured delay to a unique cause. No implementation was copied.
The existing stronger timestamp sequence and all memory-safety fences remain.
