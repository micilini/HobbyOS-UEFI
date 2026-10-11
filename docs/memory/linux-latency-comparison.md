# Linux mechanisms and memory latency

The owner changed the development policy on 2026-10-10: 50,000 ns remains a
reported optimization reference, rather than a universal advancement gate.
Correctness, complete observations, valid clocks, the original observer
qualification, and truthful historical results remain required. No numerical
replacement for a comparable Linux result has been established. This document
does not grant implementation acceptance.

## The reference that actually ran

The retained virtual Linux run used upstream **6.12.112**, built from source
with IRQSOFF tracing, Q35/KVM, eight vCPUs, 2 GiB, and the laptop's ordinary
background load. It was not the downloaded Debian 6.12.111-1 binary; that
binary lacked the required tracer. The tested configuration used queued
spinlocks and voluntary/dynamic preemption, without PREEMPT_RT or paravirtual
spinlocks. Its clocksource was kvm-clock. The single maximum snapshot was
264 us on CPU5, in a mapping/unmapping workload. It does not establish a
universal Linux bound. The full trace history overran; the independently
checked maximum snapshot and the incomplete history retain separate status.

That workload ran sixteen workers with 1,024 iterations each, with mmap,
protection changes, loads/stores, and munmap. It did not reproduce HobbyOS's
explicit all-CPU WBINVD requests, guarded task lifetimes, or first-fit/bitmap
work. Linux's hook-to-hook IRQSOFF measurement and HobbyOS's enclosing observer
also have different boundaries. Cross-host physical/virtual maxima are not a
performance ranking. A new comparative acceptance rule requires matched roles,
work definitions, configuration, exposure boundaries, and retained failures.

Selected relevant source paths were compared between the tested v6.12.112
and v7.2.9, the stable version consulted on 2026-10-10. The session under
`backups/20261010T161738.618351Z-linux-latency-comparison` retains the exact
reference-file hashes, source URLs, configuration evidence, and differences.
This is a directed call-path study, not a claim to have audited every Linux
subsystem. Linux code is studied as a reference, not copied into HobbyOS.

## Owner release and new arrivals

Linux's queued-lock release updates the ownership byte independently of queue
admission (`include/asm-generic/qspinlock.h`, `queued_spin_unlock`). The generic
ticket-lock reference in v7.2.9 likewise separates owner publication from next
ticket allocation. The tested x86 kernel used queued locks, not that generic
ticket implementation. Both illustrate the useful ownership principle.

HobbyOS's incoming `spin_unlock` used a compare/exchange on the entire packed
ticket word. A new arrival changes the next-ticket halfword and can force the
current owner to retry release, even though it does not change that owner's
right to release. The actual incoming function, with one controlled atomic
arrival before each compare, needs 33 attempts for 32 arrivals. This is a
software progress witness, not attribution of a previous elapsed maximum.

The independently written x86-64 correction advances only the low owner
halfword with one locked ADDW. The ticket word remains naturally aligned and
the existing FIFO admission, no-barging trylock, and object layout remain.
The operation cannot carry into the next-ticket halfword on owner wrap. Its
locked memory operation and compiler memory clobber publish protected stores
before the next owner's acquire load. The existing diagnostic hook remains
before actual release, and IRQ restoration remains after release. No fence
or ICR/TLB/cache publication ordering is weakened.

Unlike Linux's release store, this deliberately retains a stronger locked
operation while changing the smallest possible x86 primitive. It removes a
software retry loop; it does not bound cache-coherence, SMI, host scheduling,
or elapsed locked-instruction latency. All such elapsed exposure remains in
the outer observer. `scripts/test-spinlock-release.py` executes the actual
functions in normal, UBSan and ASan modes, including FIFO contention,
160,000 protected acquisitions per mode, payload publication, wrap, trylock,
and exact original IRQ flags. Its old-source control and emitted-instruction
check are separate from guest timing proof.

## Other mechanisms and their applicability

| Linux mechanism inspected | HobbyOS conclusion |
| --- | --- |
| Queue-local spinning in `kernel/locking/qspinlock.c` | Could reduce shared-cache-line traffic at high contention. A replacement needs nested CPU-local node ownership and fairness/lifetime proof. The measured release defect can be removed without replacing the existing FIFO lock; no queue replacement is claimed here. |
| Conditional `CONFIG_GENERIC_LOCKBREAK` path in `kernel/locking/spinlock.c` | Some configurations retry trylock with interrupts restored between attempts. This is not the normal measured x86 queued-lock path. HobbyOS cannot enable interrupts after reserving a FIFO ticket: an interrupt taking the same lock could wait behind its interrupted owner. No generic early-STI substitution is made. |
| PREEMPT_RT lock/IRQ transformations | The reference kernel did not use RT. Sleeping locks and threaded IRQs require different context and scheduler contracts. They are not an explanation for its observed number or a drop-in raw-lock replacement. |
| Per-CPU page lists in `mm/page_alloc.c` and SLUB fast paths in `mm/slub.c` | They depend on local pinning, distinct allocation semantics and slow-path synchronization. HobbyOS already prepares heap growth privately and rechecks first-fit before publication. Repeating that split or importing a new allocator would not fix the demonstrated release retry. |
| Range cache maintenance in `arch/x86/mm/pat/set_memory.c` | Linux can flush known cacheable addresses individually, with feature checks, TLB ordering and barriers. Global WBINVD still exists when required. HobbyOS's current global writeback API has no validated range contract; substituting a range operation would silently weaken its meaning. |
| TLB requests and SMP completion in `kernel/smp.c`, x86 SMP code | Completion follows real work and storage remains live. HobbyOS retains root/target/generation binding, INVLPG for up to 64 pages, full-context invalidation above that, and all ACKs before reuse. Faster notification cannot replace those obligations. |
| `kvmclock.c`, `pvclock.c`, `tsc.c`, `hpet.c`, timekeeping publication | HobbyOS already uses a qualified KVM provider with independent HPET reference/fallback. The selected pvclock source was identical between the two Linux versions. No new clocksource or cached-clock shortcut is justified by the release witness. |
| `trace_irqsoff.c` and IRQ trace hooks | Observer boundaries and disturbed-trace policies differ. HobbyOS keeps every eligible maximum, original allowance, nonreturning publisher handling, and complete path coverage; Linux's filtering is not imported to make a result smaller. |

## The current physical evidence

The 2026-10-10 11:58:21 physical capture has 264/264 CPU/path cells and no
invalid or missing cells. All 24 CPU frequencies qualified, but CPU5's post
observer envelope was 317,054 cycles against its frozen 307,154-cycle allowance.
That qualification failure remains. There were 248 cells at or above 50 us
under the original strict rule; it is not an accepted old run.

The CPU1 maximum is a 4,742,262-ns enclosing shootdown/writeback observation.
Its same-event WBINVD bracket has 13,750,693 cycles and a conservative minimum
of 4,265,679 ns using the validated upper frequency. Request 3712 has all
24 ACKs, with no failed/missing target. This locates a long cache-maintenance
bracket; it does not separate instruction work from fences or asynchronous
interference. The Linux reference did not request this operation, so its
264-us maximum is not an appropriate replacement threshold for this event.

Ten malformed bootloader bytes precede kernel entry in that raw serial file.
The kernel suffix reconstructs with 222,538 valid CRC records, but this does
not repair the original full transport. The separate UART correction stops
reinitializing/clearing an already initialized transmitter between fragments.
Its before/after device-model tests and new QEMU boot are distinct from the
ticket-lock change. No physical success is inferred from hosted UART stubs.

## Current output policy

`irqlatency serial` remains schema 4 and retains its strict historical
semantics. `irqlatency reference` is an explicit schema-5 successor for the
owner's new policy. It performs exactly the same workload, measurements,
calibration, terminal audit and deferred proof export. It reports the 50-us
comparison as `BELOW`, `EXCEEDED`, or `UNAVAILABLE`, independently of measurement
qualification. Invalid clocks, observer failure, missing cells, invalid
records, functional failure and transport failure still reject the command.
Equality remains an exceeded reference, not below it.

A qualified reference command may complete successfully above 50 us. Its
footer still says `timing_acceptance=NOT_GRANTED`. This denotes a complete
qualified diagnostic, not comparative superiority or completion of the full
functional/native campaign. Old captures cannot select the new parser; the
expected version and policy must be declared. Raw cycles, maxima, losses,
allowances and previous verdicts are never rewritten.

## Successor diagnostic and preserved guarantees

The first successor VM was a declared Q35/KVM/SMP8, 2-GiB diagnostic with
ordinary placement, the unchanged workload, timebase and outer primitive,
and no host trace or application suspension. The real lock progress command
completed 160,000 acquisitions on eight CPUs with no exclusion or IRQ-flag
violation, no barging and all eight workers retired. The UART repair, owner
release change and output-policy successor are separately listed candidate
deltas. A single lower maximum cannot uniquely attribute improvement to one
of them. Failures remain in the attempt history; there is no lucky-retry rule.

The complete schema-5 reference command reconstructed 88/88 qualified cells,
16 retired stress workers, 29,703 retained TLB records (3,999,338 bytes), and
2,560 terminal service generations without transport loss. It retained 49
cells at or above the 50-us reference. The largest enclosing estimate was
1,547,664 ns on CPU1 in the heap path. This is a qualified diagnostic under the
new reference policy, not a strict historical timing success or comparative
acceptance. The original complete functional/native campaign is still pending.

The same event contains metadata operation sequence 140, owner 134/generation
4, continuous mask segment 5824, with 3,954,188 raw cycles and a conservative
minimum of 1,496,169 ns. Its 914-cycle acquisition wait is separate. The first-fit
stage visited 1,004 blocks before the isolated operation began. The operation
ends before telemetry publication and before the corrected owner-release
instruction. This locates the remaining elapsed exposure; it does not assign
the duration uniquely to guest instruction work or host interference.

This boot selected the HPET fallback. CPU2's fourth independent qualification
bracket was 150,923 ns, above the unchanged 100,000-ns qualification limit;
the rejected local page was unregistered. The other seven CPUs qualified,
which is insufficient to select the all-CPU KVM provider. The fallback passed
its functional checks. Consequently this run is not a controlled speed
comparison against an earlier boot that selected the KVM provider.

The one-second elapsed request deadline, exact IRQ flags, all-CPU readiness,
ACK after actual invalidation/writeback, quarantine, guards, W^X, full PMM
representation, stack/frame limits and first-fit allocation stay intact.
The selected boot included no USB deployment. Required functional/native
coverage and a defined comparative acceptance methodology remain outstanding.

Primary references: [Linux v6.12.112 source](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tree/?h=v6.12.112),
[Linux v7.2.9 source](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tree/?h=v7.2.9),
[lock types](https://docs.kernel.org/locking/locktypes.html),
[ftrace](https://docs.kernel.org/trace/ftrace.html), and
[Intel architecture manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).
