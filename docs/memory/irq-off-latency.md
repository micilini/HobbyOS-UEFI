# Memory-path interrupt masking

The owner revised the performance objective on 2026-10-10. For new declared
reference diagnostics, 50 us is an optimization reference rather than a
universal advancement gate. See [the current policy and Linux comparison](linux-latency-comparison.md).
The contract below remains the exact historical strict role; old evidence and
its verdicts are unchanged. Observer qualification, correctness and complete
coverage have not been relaxed. Comparative acceptance is not established.

## Historical strict acceptance contract

The approved source is commit `d68a3b4a2c97f55dfdcc445bfe42317e09e1c55e`,
tree `3ce6be4fcae7c23fd41adb439e081af72ea4717c`. The initial instrumented
reference adds observation only. Allocator changes require a valid measured
violation, a retained witness and a synchronization proof.

The maximum accepted observed runtime exposure is **strictly less than
50,000 ns**. Equality is a violation. This engineering choice is 5% of the
existing 1-ms LAPIC period, not an architectural guarantee. The independent
verifier contract is `scripts/irq-latency-contract.json`. No average, percentile,
overhead subtraction or snapshot reset can replace the per-boot maximum.

Authority requires three distinct consecutive Q35/KVM/SMP8 boots, host quota
and cpuset capacity for eight vCPUs, default placement, validated local timebases,
and complete required CPU/path coverage. SMP24 is mandatory functional stress
with raw timings retained. TCG is functional evidence with explicitly reported
timebase status. A host pause remains part of the observed maximum.

## Source inventory before instrumentation

| Boundary | Actual source behavior and measurement obligation |
| --- | --- |
| `core/idt.c` | `irq_save` reads exact RFLAGS before CLI; `irq_restore` uses POPFQ. Repeated CLI and restores retaining IF=0 must keep the original outer start. Direct enable/disable wrappers also belong to the boundary inventory. |
| `core/spinlock.c`, `.h` | IRQ-save precedes ticket acquisition, including try-lock failure. Charge acquisition spinning, nested work and unlock. Keep the packed owner/next ticket algorithm unchanged. |
| `memory/heap.c` | First-fit, growth reservation/registration and failure frame cleanup run under the heap lock. Expansion nests PMM locks; unsuccessful growth may print while masked. No change is justified before measurement. |
| `memory/pmem.c` | Single, contiguous and constrained selection; mark/free; integrity and reachability all take the PMM lock. Integrity checks both full bitmaps and the same locked free count. Reachability scans admitted prefixes but retains total represented memory separately. |
| `memory/dma_buffer.c` | Ownership slot reservation and publication are separate locked intervals. PMM selection, translation, full buffer zeroing and frame cleanup occur between them. Attribute their individual masks to the DMA operation without treating the IF-enabled gap as masked. |
| `memory/owned_mapping.c` | Runtime admission is IF-enabled and nonblocking. Metadata locks surround view scans and publication; paging, invalidation and cache writeback occur outside those locks. Bootstrap admission explicitly saves IRQ flags. Retired/quarantined states preserve uncertain ownership. |
| `memory/mapping_core.c`, `memory/paging.c` | Core mapping helpers do not mask directly; their production paging callers hold the paging lock for validation/publication. Include all preflight loops and rollback work actually performed under that lock. |
| `memory/kernel_stack.c` | Backing allocation/zeroing precedes owner reservation. Owner scans use the stack lock. Runtime retirement invalidates and awaits ACK before backing and slot reclamation. Include real reaper traffic. |
| `memory/kernel_tlb.c`, `smp/cpu_request.c` | Topology copying, local callback and each send have distinct masked sections. Admission and ACK waits are IF-enabled. The current HPET body measurements omit outer boundaries and do not satisfy this increment. Production WBINVD must remain visible. |
| `apic/lapic.c`, `lapic_delivery.c` | Checked xAPIC ICR exclusion nests in the request's masked send. Ordered x2APIC writes and status semantics stay unchanged. |
| `core/interrupt_stubs.S`, `interrupts.c` | Hardware entry masks before saved-register dispatch. CPU-request handling has exactly one EOI. The timer dispatch can switch contexts before returning; a callback interior span is not the full hardware masking envelope. |
| `core/switch.S`, `scheduler.c` | Context switching occurs with IF=0. The continuous CPU exposure can cross task identities; saved flags remain owned by the suspended call. New-thread `STI` in `thread_wrapper` terminates that exposure. |
| `core/entry.S`, `smp/trampoline.S`, `smp_boot.c`, `irq_bootstrap.c` | Raw CLI and STI/HLT sequences include initialization, AP release and bootstrap handoff. Before safe topology and calibration, report explicit early/unavailable coverage. No ordinary runtime allocator call gains this exemption. |
| `memory/guard_probe_access.S`, `core/interrupts.c`, `core/panic.c`, `power/power.c` | Destructive probe, fatal halt, panic and power transitions have separate terminal roles. Keep exact fault ownership and emergency deadlines; they do not furnish passing production samples. |

The translation helpers do not introduce another lock. Their nested PMM/paging
callers and exceptional exits are nevertheless part of the operation exposure.
An inventory row is a design obligation, not proof of implemented coverage.

## Timebase policy fixed before baseline collection

Record raw checked CPUID leaves, including TSC support, extended-leaf availability,
invariant-TSC and optional RDTSCP. CPUID-15 is usable only with all three nonzero
ratio/crystal fields. No CPUID-16, brand-string or model-name frequency fallback
is admitted. RDTSCP/AUX is not used as an assumed CPU identity.

Each CPU uses three independent HPET windows targeting 20 ms, each admitted
only within 10–100 ms. Each HPET endpoint is bracketed by ordered TSC reads.
Use the initialized HPET sample contract, unchanged counter configuration and
the reported femtosecond period. Both endpoints must be valid, IF-enabled,
on the same CPU. Each endpoint bracket must have an unchanged context-switch
sequence; preemption between the two completed endpoint brackets is allowed.
Extended 32-bit
HPET ticks are accepted only through the existing extended-counter reader;
backward/equal ticks are rejected, never repaired by guessed wrap arithmetic.

For each window, the TSC bracket supplies lower/upper elapsed cycles. Add two
HPET ticks to the reference uncertainty, one per quantized endpoint. The
frequency interval must be no wider than 10,000 ppm (1%); all three intervals
must intersect. The windows must be disjoint. A complete CPUID-15 frequency
must lie in the intersection. A pre-stress conversion frequency is the floor
of 99% of the intersection's lower bound. This fixed 1% drift reserve makes
conversion conservative; it never subtracts observer time.

Repeat calibration after the measured workload. Reject more than 1% estimated
rate drift or a post-stress lower bound below the frozen conversion rate. Do
not increase the conversion rate after seeing results. Raw cycles and the
declared/calibrated frequencies remain separate. Unknown frequency produces
cycles-only/unavailable records. Conversion rounds upward with checked integer
arithmetic; overflow is invalid rather than zero.

The intended ordered primitive is `MFENCE; LFENCE; RDTSC; LFENCE`, with a compiler
memory clobber. The leading MFENCE includes prior-store completion and the trailing
LFENCE orders following work. The final emitted boundary sequence, including
CLI/POPFQ/IRET-adjacent observer work, must be audited before any timing authority
is claimed. See Intel's [RDTSC instruction specification](https://cdrdv2-public.intel.com/868137/325462-089-sdm-vol-1-2abcd-3abcd-4.pdf).
The manual establishes ordering semantics, not the actual executor frequency.

## Fixed workload design

Use reproducible seed `0x4952514c`. Each participating worker performs 1,024
small/mixed allocator operations, with sizes 16, 64, 256, 1,024 and 4,096 bytes,
at most 64 simultaneously live allocations, and deterministic alternating-slot
fragmentation. Aligned cases use 64- and 4,096-byte alignment. Require actual
local CPU identities for every participating CPU; task creation is insufficient.

Exercise one 33-MiB heap request with measured region-count growth, after proving
128 MiB of eligible free headroom. If this cannot force genuine growth safely,
the workload is incomplete and must be revised and frozen before collection.
Each worker owns at most 32 PMM frames and one 64-KiB aligned DMA buffer at a
time. Include constrained refusal without accessing inadmissible addresses,
full integrity/reachability, zero verification and permitted DMA retirement.

Perform 192 complete owned map/unmap cycles in aggregate with two eligible
initiators; cover 63-, 64- and 65-page invalidation edges with real target CPU
A/B loads. Keep predecessor task/reaper, keyboard, timer, guard and TLB sampling
cardinalities. Stop workers, join/reap, complete ACKs and cleanup before terminal
service and latency snapshots. Retained boot/controller objects are separate
from the stress ownership balance. No snapshots reset earlier maxima.

## Boundary implementation

`irq_latency_arch.S` saves RFLAGS before any flag-changing observer instruction
and clears DF while running C. The software beginning is the ordered TSC read
immediately preceding CLI. A saved IF=0 joins the existing outer exposure.
Closing metadata is prepared before POPFQ. The ending TSC read follows POPFQ,
so all pre-restore recorder work is included. Final POPFQ reestablishes the exact
requested flags after the IF-enabled publication code; it does not invent a
different IF value. Hosted assembly tests substitute only privileged IF effects
and check the arithmetic and direction flags with actual PUSHFQ/POPFQ.

Endpoints include CPU identity, IRQ-entry and context-switch sequence checks.
If an IRQ interrupts the pre-CLI envelope, the last local IRQ-exit timestamp
supplies a conservative beginning only when its entry and switch sequences
still match. If an IRQ interrupts post-POPFQ publication, its first entry with
saved IF=1 supplies a conservative same-CPU ending. Both original observations
and the selected boundary kinds are retained. A missing local boundary or an
unordered clock remains invalid; different CPUs' timestamps are never subtracted.
These rules are fixed before runtime collection and do not remove pauses while
IF was actually clear. Publication uses generation-bearing pending slots; an interrupted
completion cannot be reused or replaced by a stale ticket. A snapshot performs
one atomic-cell copy attempt and fails on an active writer. Full buffers,
recursion, generation exhaustion and unresolved intervals remain visible.

Operation tags are indexed by the currently executing guarded stack slot and
owner generation. They contain scalar masks and a bounded nesting depth, with
no retained task or stack pointers. IRQ exclusion serializes changes on the
running stack; the scheduler's existing on-CPU ownership prevents simultaneous
execution of that stack on another CPU. Migration resumes the same slot's tags.
Cleanup attributes handle every ordinary return. Post-ACK retirement checks
that no scope remains active before reuse. The CPU-local recorder only copies
the current scalar path mask at actual masking boundaries. A stack-owned IRQ
barrier clears interrupted thread tags while the handler executes, then restores
them on that frame's return. Its scalar token occupies existing assembly padding
and survives a scheduler switch without changing the saved-register geometry.

Calibration collection admits at most 64 windows per discovered CPU in aggregate,
with four empty IRQ probes per attempted window and one ordinary worker per CPU.
All workers use normal placement and owned handles. Selection takes the earliest
three disjoint valid windows per CPU; every other valid window is cross-checked,
and all invalid attempts remain in the output. At least 32 valid IRQ probes per
CPU are required. Collection has a 120-second completion deadline. A missing
worker/reaper completion prevents publication of a completed series.
Clock and observer decisions are reported independently for every CPU. Clock
error bits are capability coverage (1), disjoint-window coverage (2), frequency
selection (4), interval consistency (8), and drift (16). Observer error bits are
probe coverage (1), arithmetic range (2), and post-check drift (4). A failure on
one CPU does not stop inspection of the others. A raw nanosecond conversion can
remain available while the observation is explicitly unqualified; it cannot
establish budget compliance without both pre-check and post-check acceptance.
The initial observer bring-up used 16 attempts and two workers per CPU. Of 128
windows, 94 migrated and only 29 were valid; CPUs 2 and 5 each had two valid
windows, so activation was refused. The revised finite attempt budget addresses
that observed acquisition failure without changing timebase tolerances. Empty
IRQ overhead probes additionally require exactly one intervening IRQ entry;
interrupted probes retain their raw endpoints and cannot calibrate an empty
entry/return boundary. Host pauses in otherwise eligible probes remain included.

After the post-stress calibration workers have retired, a freeze request is
observed at each CPU's next eligible IF-enabled LAPIC entry, with no active
memory tag, open interval or pending publication. The recorder is copied into
immutable terminal storage once. Local cutoffs and snapshot-copy cycles are
reported separately; this read-only snapshot adds no CPU service request.

Hardware entry has no software instruction before IF is cleared. The entry
timestamp therefore follows the existing register save. The IRQ ending
timestamp is emitted in assembly after all C observation and publication
preparation, immediately before scalar endpoint/ready stores and the existing
restore/IRET tail. The same assembly publication macro executes in hosted tests.
These raw adjacent-boundary spans are explicitly interior observations. Before
activation, measure 32 valid empty interrupt envelopes per CPU through the
existing spurious-vector entry/dispatch/return (no EOI in that handler). Retain
both outer and inner endpoints and every invalid probe. Freeze the additive
allowance at twice the largest observed outer-minus-inner cycle difference,
including all valid probes collected for that CPU. The factor of two is a
pre-measurement observer margin, not an architectural worst-case guarantee.
Repeat after stress; any new boundary cost above the frozen allowance invalidates
that series. Never raise the allowance or frequency to salvage a failed series.

The same allowance conservatively covers an IRQ-origin interval that ends in
a new thread's STI/POPFQ after a context switch. Raw endpoints are never edited:
the adjustment is a separate witness field, added with overflow checking.
Normal software CLI/POPFQ intervals use zero adjustment. Host descheduling after
an adjacent IRQ timestamp remains a limitation of the characterized boundary,
not proof of a mathematical bound. Report this boundary class with each maximum.

The actual identity reader uses a checked CPUID topology leaf and bounded slot
lookup. It does not call LAPIC MMIO: the existing debug MMIO tracer itself calls
`irq_save` and would recurse into this observer. Lock components are recorded
only within participating memory exposures; they never replace the outer span.

## Implementation and evidence status

Development is in progress. Conversion, calibration decisions, bounded recording,
scope state and the assembly flag boundary have actual hosted tests. Hardware
adapters and production-path scope integration are being completed. No runtime masking result, instrumented-reference
acceptance, allocator correction, production acceptance, complete replay or
hardware result is claimed by this document.

The second observer bring-up activated all eight CPU timebases and closed all
terminal intervals without missing starts, recursion or attribution errors.
Its post-check rejected CPU 3's observer overhead (18820 cycles); the original
timestamp preceded C completion publication. That failed series motivated moving
the ending timestamp after C into the assembly return tail, without altering
the frozen margin factor or the 50000-ns budget. Its largest failure stream
contained 1222 ordinary violations on one CPU, overflowing the initial 128-entry
buffer. The bounded failure capacity is now 2048 per recorder, the next power of
two above that observed demand. Overflow remains an explicit rejection. With
23 scalar witness cells this adds 22609920 bytes across the 64 normal/calibration
recorders at the supported 32-CPU capacity; the four-GiB host provision is retained.

The assembly-tail diagnostic retained all 11760 CRC-checked records and froze
eight complete CPU snapshots without invalid boundaries or failure overflow.
All pre/post clock checks passed, but CPU 1's post observer cost was 18138 cycles
against its frozen 4668-cycle allowance. This diagnostic remains rejected. Its
source, launch, raw logs, all failure witnesses and classification are retained;
neither the margin factor nor the strict budget was changed.

The initial controlled stress implementation uses two ordinary workers per CPU,
each with 1024 mixed heap/PMM operations, 64 live heap slots, 32 live frame slots,
and one 64-KiB DMA cycle every 64 operations. Each DMA cycle also acquires and
retires a real managed view through a SELFTEST wrapper that validates the owned
DMA handle and uses the production acquire/release transaction. It performs no
post-unmap access and needs no global fault-probe reservation. Thus workers can
exercise actual concurrent mapping transactions while the coordinator performs
its 192 separately counted cycles. The total is `192 + 16 * 2 * discovered_cpus`.
Every 128 operations each worker performs current full PMM/heap integrity and
PMM reachability checks. Local payload accesses under brief
identity exclusion prove actual CPU participation independently of task creation.
API elapsed records use the monotonic clock and explicitly include IF-enabled
time. The coordinator requests genuine 33-MiB heap growth only with 128 MiB of
eligible headroom, exercises alternating free/held frames in an owned 32-frame
extent, and completes 192 managed view retirements. It calls the existing TLB
fixture, preserving 256 sequential rounds, two concurrent initiators with 64
rounds each, real A/B loads and the 63/64/65-page cases. Worker completion is
published before inspection; task disappearance and physical stack retirement
are both required. This workload is under development and has no runtime
acceptance yet.

The first stress bring-up completed real growth, controlled refusals, 192
coordinator view retirements and the inherited TLB fixture, but all 16 workers
stopped at their first managed-view acquisition. The original harness retained
the failure but did not record the exact API status; it is not retrospectively
relabeled as captured BUSY. All published workers retired and their owned
allocation counters balanced. The corrected SELFTEST caller records exact
acquire/release statuses and BUSY counts. Only BUSY receives bounded IF-enabled
1-ms sleeps, limited to 1024 attempts and one second per API operation. This
does not change the production admission algorithm or request timeout. Failed
quarantine retains its backing instead of freeing potentially visible storage.
The coordinator also uses ordinary owned views with no fault-probe reservation.
Actual compiled admission and retirement decisions have hosted normal/UBSan/ASan
tests, including permanent BUSY, stop, cleanup after stop, and quarantine.

That first stress capture also rejected CPU 5's post observer cost (7962 cycles
against 2764 frozen cycles) and exhausted the auxiliary failure-witness arrays.
Per-path counts and maximum witnesses continued updating, but the auxiliary
failure chronology is explicitly incomplete. No acceptance or production
optimization is inferred from that failed measurement window.

The admission correction completed 16 workers with 1024 heap and 1024 PMM
operations each, 256 DMA lifetimes, 448 owned-view lifetimes and actual local
payload accesses on all eight CPUs. The independent workload comparator checks
every worker's resource balance, operation counts, identity, retirement and
API endpoint arithmetic. Its 24 isolated negative cases have restored positives.
This functional evidence does not supply a timing PASS: CPU 1 exceeded its frozen
observer allowance and CPU 4 did not produce a terminal snapshot.

A subsequent freeze diagnostic retained the last local eligibility state and
per-site invalid counts. It found unfinished publication slots and orphaned
interrupt barriers. A separate read-only stopped debugger experiment showed an
open publisher from `thread_exit_with_reason` and an empty interrupt barrier on
a stack whose production owner had already reached FREE. That experiment's
pauses disqualify timing authority; its raw recorder/scope/owner bytes diagnose
the observer lifetime error. The scheduler may legitimately switch a terminal
task away after POPFQ without ever resuming its publication continuation.

Software-close records now carry scalar stack slot/generation. The existing
post-ACK, off-CPU stack retirement hook can finalize an abandoned publisher only
for that exact identity, using the first already-captured same-CPU IRQ entry
whose saved IF was enabled. The unavailable post-POPFQ observation stays absent;
the witness uses the distinct RETIRED_PUBLISHER boundary class. Without the
independent IRQ endpoint it becomes invalid. Only empty interrupt barriers may
be discarded at retirement; an outstanding ordinary memory scope still fails.
Normal, UBSan and ASan tests exercise stale identities, missing endpoints,
duplicate completion and nonempty scope rejection with the actual C routines.

The fixed workload now completes and reaps its concurrent allocator workers
before entering the inherited TLB fixture. That fixture takes whole owner/table
snapshots, which reject concurrent changes to the owner inventory; one recorded
attempt failed its initial scan while the allocator workers were retiring.
The allocator workers and mapping coordinator remain concurrent, and the TLB
fixture retains its two concurrent initiators and every original round. Its
snapshot contract and failure predicates are unchanged.

Each semantic path additionally records all violating outcomes as a saturating
counter and observes actual ticket distance at the lock's atomic ticket fetch.
The largest queued distance includes the lock address, outer sequence and path
mask. Worker lifetime overlap alone cannot satisfy this contention predicate.
Required sample/invalid/violation counts and maxima update after the optional
2048-entry failure-detail trace fills. Trace exhaustion fails complete failure
chronology; required counter, pending publication or maximum-witness exhaustion
fails measurement coverage. Neither condition is hidden or resets a maximum.
The 25-cell witness schema includes owner identity; earlier 23-cell captures
remain explicitly historical. No old capture gains missing violation counters.

The first direct enable in a new thread starts with the caller's RSP exactly at
the payload's exclusive end. The boundary adapter now captures the live return
address slot inside the payload. The hosted assembly test checks that this slot
contains the same caller address recorded as the boundary site. Eight-CPU runtime
diagnostics confirmed that this removes all 26 previously missing owner reads;
their original records remain retained.

Callback components use ordered local TSC reads around the actual
`kernel_tlb_execute_local` invocation and its existing HPET body sample update.
They include that callback's instrumentation but do not purport to include the
entire hardware IRQ. The outer masking recorder still supplies the budget
maximum. Each component retains its operation, generation, root, CPU, context,
count, failures and exact maximum endpoints. Sample/protection operations remain
separate from ordinary invalidation and cache maintenance.

Request elapsed components use the existing monotonic `started`/`ended` values
from admission through completion collection. They include enabled remote waits;
they exclude initial argument/context checks and final retirement bookkeeping.
Their nanoseconds must never be substituted for masked cycles. Completion is
attributed to the CPU observed under a short IRQ save, allowing legitimate
migration during the enabled wait.

Host capture retains process and native-thread affinity, every cgroup ancestor,
controller availability, quotas and effective cpusets. The verifier follows the
parent's `cgroup.subtree_control` when checking controller interface availability,
as specified by the [Linux cgroup v2 documentation](https://www.kernel.org/doc/html/v6.8/admin-guide/cgroup-v2.html).
QMP `query-cpus-fast` binds actual vCPU indices to native thread IDs; thread names
are not an identity oracle. These host checks do not qualify a failed observer
calibration or erase an elapsed scheduling delay.
