# Safety-clock sampling and masked segments

The frozen preceding diagnostic `masked-clock-body-v1` is a diagnostic minimum-exposure observation. It does
not replace the enclosing IRQ observer or qualify a path below 50,000 ns.
The preceding candidate did not isolate a same-event safety-clock body, so
system-clock dominance was not established by that capture.

In an entry-diagnostic build, `clock_monotonic_ns` first executes its original
`irq_save`. A bounded controller helper reserves a CPU-local sequence and
captures the existing continuous-mask, owner, generation, epoch, entry,
switch and fault identities. The first ordered timestamp follows that
helper's return. The second ordered timestamp precedes diagnostic publication
and the original `irq_restore`. Both reads retain
`MFENCE; LFENCE; RDTSC; LFENCE`. The measured body includes the existing clock
validation, safe HPET sample/conversion, classification, global monotonic
publication, statistics and local-history update. Clock decisions and
incoming flags are unchanged.

The first timestamp's remaining boundary instructions, the body, and the
second timestamp's ordering instructions contribute to this interval. The
record does not claim uninstrumented execution time. Host or asynchronous
interference is not removed. The helper's preparation and publication remain
inside the original outer measurement; moving them outside this diagnostic
bracket does not remove them from actual IRQ unavailability.

There is one outstanding clock token per CPU, with monotonically increasing
reservation and retirement sequences. Reentry, duplicate retirement,
exhaustion, changed identity, missing endpoints and unfinished reservations
invalidate diagnostic authority. No callback retains a stack pointer.
Complete scalar maxima are separate from optional detail storage. Publication
copies one fixed witness and never scans history. Faults invalidate the
continuity proof; no fault/NMI reader waits for an interrupted writer.
Reporting reads the record only after that CPU's terminal freeze.

The checker independently reconstructs the pre/post frequency upper bound,
including the existing one-percent reserve, and computes
`floor(cycles * 1000000000 / upper_hz)`. It checks both raw endpoints, every
continuity field, reservation accounting, calibration epoch and all-CPU
inventory. The return address identifies the immediate clock caller in the
frozen ELF; it is resolved against that candidate's disassembly. A valid
minimum of at least 50,000 ns establishes a violation within the clock-body
bracket. It does not distinguish HPET MMIO, conversion, atomic publication,
observer work and interference by itself.

In that preceding candidate, ordinary production, DEBUG and non-entry SELFTEST builds compiled out these
additions. Same-local-compiler comparison against the incoming source must
confirm that property. No existing HPET deadline, request ACK, TLB/cache
operation, allocator algorithm or observer endpoint changes in this step.
The initial experiment keeps ordinary Q35/KVM/SMP8, 2 GiB, default placement,
the current complete workload/calibration and mandatory-maxima detail policy.
Its sole changed feature is this clock-body observation. Its role is
diagnostic; its outcome cannot become a timing-series success.

The architectural ordering audit is separate. Linux v6.12
[`rdtsc_ordered`](https://raw.githubusercontent.com/torvalds/linux/v6.12/arch/x86/include/asm/msr.h)
documents a locally ordered read, whereas HobbyOS currently also drains prior
stores. That distinction warrants an architectural proof before any change;
this experiment retains the existing primitive. Linux's
[`timekeeping`](https://raw.githubusercontent.com/torvalds/linux/v6.12/kernel/time/timekeeping.c)
separates sampling and publication under different synchronization assumptions.
No implementation is copied, and moving a sample outside masking without
revalidating HobbyOS CPU-local history is not justified by that example.

## Revalidated sampling successor

The first fresh body diagnostic qualified frequency and the original observer
on all eight CPUs. It still contained valid clock-body minimum violations on
every CPU (88,245 to 7,576,454 ns). It proves that the safety-clock body is a
masked contributor; it does not uniquely divide that elapsed time among MMIO,
conversion, publication and interference. CPU0 entered with IF=0; the other
retained maxima entered with IF=1 from request deadline checks.

The production correction preserves HPET as the clocksource. Under the first
`irq_save`, a known CPU snapshots its raw-history and saturating read revision.
An originally IF-enabled caller restores its exact flags before the safe HPET
read. A second `irq_save` then obtains the actual CPU and revalidates its local
revision. Only an unchanged CPU/revision consumes the original local baseline.
The global maximum publication remains the linearization point for successful
readout. No pointer into task storage survives, and no owner-dependent wait is
introduced. The known CPU array has static boot lifetime.

On migration or an intervening reader, the pending sample is first classified
against its original baseline. An invalid or already provably backward sample
remains a failure. Otherwise the function takes exactly one fresh sample under
masking, using the current CPU's baseline. This avoids mistaking a delayed older
sample for a local regression, without dropping an observed regression. Only
the current CPU's local history is updated. The returned floor includes its
latest returned value and global publication. A saturated revision disables
the optimistic route. No unbounded resampling, cached-only read or deadline
change is present. An originally IF=0 caller, unknown CPU, or exhausted revision
uses the existing fully masked sampling path. The HPET split/extension reads
and their own exclusion are unchanged and remain measured.

No IRQ/fault reader waits for an interrupted clock writer. Ordinary IRQ reentry
while sampling causes revision mismatch and bounded fallback. Existing fatal
fault/NMI policy is unchanged; this does not introduce a clock read in those
contexts. All request admission, wait/stall and terminal one-second decisions
still read the real clock. A valid ACK is not a substitute for timely completion.

`masked-clock-segment-v2` retires a separate diagnostic token before each actual
restore. Phase 1 is the pre-sample snapshot, phase 2 the revalidated publication,
and phase 3 an IF=0/unknown/exhausted or stale-state masked sample plus publication.
Each retained maximum belongs to one continuous masked segment; the two segments
of an IF-enabled call are never added into a fictitious continuous interval.
All outer measurements, including the HPET extension's own masks, remain active.
The unchanged four-instruction clock primitive is used throughout. Version 1
records retain their original source, interpretation and verdict; current source
cannot select that historical schema.

Actual-function tests substitute HPET, privileged flags and topology only. They
exercise delayed IF-enabled acquisition, retained IF=0 exposure, same-CPU nested
publication, migration, old sample versus local regression, invalid first read,
saturated revision, stalled equal values, cross-CPU lag, reservation exhaustion
and discontinuities. They prove control flow and ownership, not host fault cost
or hardware timing. The clock production object intentionally differs from the
approved and incoming kernels; diagnostic additions still compile out of the
ordinary production role. No allocator or request state-machine behavior changes.
