# Acknowledged kernel invalidation

The rolling contract starts at commit `18e64098519e3787e5f5ced2328d60ef8084964c`
and tree `e07550842e79512a0dc770728bb0e2b919c71ad6`. Existing control-policy,
trampoline, allocator and runtime timing contracts remain fixed. This document
describes the implementation contract; successful collection is recorded separately.

The external interrupt catalog reserves exceptions 0–31, HPET 32, keyboard 33,
LAPIC timer 34, runtime rendezvous and the read-only observer 35, xHCI 64,
acknowledged requests 225, legacy halt 253, masked LAPIC error 254, and spurious
255. Other external vectors remain unallocated. Vector 225 has priority class E;
normal timer/device priorities, TPR and routing are unchanged. All external
vectors use the existing stub table and interrupt-gate ABI. Request dispatch
records entry/exit and emits exactly one EOI. Spurious dispatch emits none.

The checked sender distinguishes validation failure, unavailable controller,
unsupported physical destination, pre-send busy timeout, detectable write failure
and indeterminate post-send completion. It preserves the 100000-poll bound.
xAPIC excludes local maskable IRQs throughout idle checking, destination/high
write, triggering/low write and completion polling. x2APIC uses one 64-bit write
to MSR 0x830 after MFENCE; LFENCE. Broadcast/reserved IDs cannot enter the
single-target path. INIT/SIPI retain their boot-specific delays. The existing
nonreturning emergency NMI broadcast remains separate: panic ownership abandons
normal work, and its delivery cannot produce a coherence acknowledgement.
An indeterminate boot delivery enters the existing nonreturning panic path.
Merely marking the AP failed is insufficient: a late SIPI could otherwise reach
the pre-existing unconditional ONLINE publication and erase that failure.

Intel documents the triggering low ICR write, relaxed x2APIC WRMSR ordering,
and invalidation on every participating CPU before unsafe frame reuse in
[SDM Volume 3A, revision 090, §§5.10.4–5.10.5, 13.6.1, 13.12.3](https://cdrdv2-public.intel.com/874249/253668-090-sdm-vol-3a.pdf).
These architectural rules justify the ordering and lifetime boundaries; the
repository's tests must independently establish its actual emitted operations.

One statically owned request is admitted at a time. Admission is an atomic
thread-context owner; handlers never acquire it. Immutable operands contain a
generation, root, normalized interval, operation and complete slot/APIC set.
Publication is release, the target claim is acquire, and completion is release
after actual local execution. A completion permits no subsequent access by that
handler to request storage. The initiator acquires every required completion.
Delayed/coalesced vectors acquire the currently published request; the interrupt
itself supplies no generation. An old unclaimed generation cannot claim newer
storage. A claimed generation prevents successful reuse until completion.

The one-second deadline covers admission, notifications and completion. A zero,
regressing or exhausted clock sample also fails; one million unchanged samples
is a finite stalled-source safeguard, not an asserted duration. Any unresolved
request quarantines the single record for the remainder of the boot. This
intentionally sacrifices availability rather than reclaiming uncertain storage.
All affected frames and virtual addresses remain owned after failure.

The normal kernel API targets every online CPU of the static shared-root boot.
It never removes a failed responder. Short local IRQ exclusions protect current
slot/APIC identity and sending, and end before waiting. Migration between those
sections cannot remove a CPU from the immutable set. The context guard requires
IF=1, a running non-idle thread, ready interrupts/clock, no interrupt nesting or
preemption epilogue, and no panic ownership. The existing paging lock always
disables IRQs, so holding it fails the guard. This does not detect arbitrary plain
spinlocks: callers must not hold any target-needed resource. Current synchronous
callsites are the memory worker and isolated owned fixture, both without such
locks. No scheduler pin spans a wait.

Intervals use start and byte length, checked canonical arithmetic and outward
page rounding. The stored last page is inclusive so the topmost canonical page
does not require a wrapping exclusive endpoint. Empty ranges execute no local
invalidation. Up to 64 pages use INVLPG; larger ranges reload actual current CR3
once. Each CPU checks the shared physical root, legal PWT/PCD flags and the
accepted PAE-only CR4 before execution. Notification is one round per interval.
The local bootstrap entry is permanently closed before starting any AP.

Eight preallocated deferred slots copy intervals without allocation or waiting
for acknowledgements. Generation/slot handles distinguish queued, pending,
complete and retained failure. Only successful completion can release a slot.
An existing semaphore wakes a memory-owned worker; its queue claim ends before
synchronous invalidation. Semaphore wake uses the existing object-to-scheduler
lock order and contains no heap allocation. Request IRQ handlers never use it.
No production object lifecycle is migrated to this interface in this delivery.
One atomic pending-wake flag bounds the semaphore count. The worker clears it
before scanning the queue: a publication after a slot has been passed schedules
another sweep, while an earlier publication is visible in the current sweep.
Completion claims the running token before reading its operands. A stale finish
cannot inspect or overwrite a newly reused slot. Polling also claims completed
storage while copying it, excluding concurrent release.

The SELFTEST fixture owns two 65-page windows beginning at
`0xffffc000001ff000`, separated by 1 GiB. They cross a page-table boundary.
The tested interval lengths repeat 1, 2, 63, 64 and 65 pages. Each invocation
performs 256 sequential A/B replacements, two concurrent 64-replacement thread
sequences on SMP, and one deferred replacement. Every replacement has separate
preparation flush, A loads, replacement flush and B loads on every target.
Sampling executes volatile loads and performs no invalidation. Markers encode
invocation/window/round; the old frame retains A until completion and retirement.
Each functional boot invokes the fixture twice. UP has 257 replacements and
1029 requests per invocation; SMP has 385 and 1542. Retirement adds one final
flush per window. Three independent vector-35 collections accompany each
invocation, and concurrent initiators must show overlapping intervals and real
admission contention. Four scratch table frames remain owned on UP and seven on
SMP; their count must not grow on the second invocation. Data frames and complete
deferred handles are released only after their final coherent completion.

Trace emission happens after each completed four-operation cycle in thread
context. Schema 5 retains every request field in a fixed positional `R` record,
each PTE transition in `X`, and each CPU result in its own short `C` record.
The decoder checks the column inventory and expands these fields for semantic
validation. Every trace record carries CRC32 computed by the existing command
transport implementation; replay verifies it before interpreting values.

Bulk trace uses the already configured QEMU debug console at port 0x402. The
SELFTEST fixture requires its 0xe9 readback signature before allocating resources.
A fixture-owned lock serializes complete records from concurrent initiators;
no callback emits records. The serial command retains its original framing,
status and deadline and binds the trace with invocation, exact record/byte counts
and a rolling FNV-1a digest. FNV is a transport consistency check, not an
authentication claim. Every command must have one matching complete trace block;
missing, reordered, truncated or corrupted proof fails replay. Production has
neither this port access nor the diagnostic command. QEMU documents the debug
console's byte write and signature behavior in its
[device source](https://github.com/qemu/qemu/blob/master/hw/char/debugcon.c).

Earlier UART captures are preserved as failures: long diagnostic output lost
individual characters despite line pacing, and the complete-record verifier
rejected them. The protected UART driver and its poll limits remain unchanged.
The debug console uses the harness's existing raw artifact and is separate from
fault tracing; no debugger stops execution between prime and post-flush loads.
The current collector selects the existing file-backed serial launch mode.
It records a monotonic host timestamp after each incremental 64-byte read and
polls EOF at 0.5 ms. The chosen mode and exact launch path are replayed; a socket
receipt cannot be relabeled as a file receipt. File visibility, buffering and host
scheduling leave unquantified timing uncertainty. Earlier socket observations
retain their original method and numbers. JSON formatting happens after VM
shutdown. Neither collector repairs malformed bytes. A preserved stopped capture
of an ordinary `cpustate` failure shows one UART, `dead[0]=1`, `fail[0]=4`, and
the CPU in its idle loop after both TLB workloads completed; this is an output
failure, not evidence of a stale translation or a triple fault.

The runtime service measures HPET intervals for requests, local operations,
handler bodies and explicit initiator IRQ exclusion. Handler-body measurements
exclude interrupt entry, the final completion store, EOI and interrupt exit.
They include sampling overhead and are observations rather than worst-case
bounds. They do not establish a system-wide IRQ-off budget.

The approved CR0, CR4, EFER, PAT, PMM span, allocation order and cache selectors
are unchanged. Effective MTRR/PAT type is not computed. Hardware and human visual
acceptance remain separate; historical unavailable demonstrations retain their
original status and current substitutes.
