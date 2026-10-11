# Additive entry and continuous-mask diagnostics

This successor preserves CPU-local reference pairing version 1, all 29-cell
whole-window witnesses, the two-times pre-envelope allowance and the original
postcheck. It changes no allocator algorithm. `IRQ_ENTRY_DIAGNOSTIC=1` requires
SELFTEST and IRQ_LATENCY. Its candidate is diagnostic; the complete timing gate
cannot promote that role to acceptance.

## Entry checkpoint

`entry-after-scratch-save-v1` reserves 16 bytes before the original fifteen
register saves. RAX and RDX are pushed before the ordered timestamp, then restored.
The checkpoint occupies the first reserved cell. After the original register
saves and eight-byte scope cell, offsets are: checkpoint 128, vector 144,
hardware RIP 152, CS 160, RFLAGS 168, interrupted RSP 176, SS 184. The extra 16
bytes preserve alignment. Return discards the scope cell, restores every original
GPR, discards the two checkpoint cells and vector, then executes the original
IRETQ. Saved hardware RFLAGS and DF are unchanged; CLD precedes C calls.

The first marker follows vector dispatch, stack reservation, two register saves,
readiness check and the original MFENCE/LFENCE/RDTSC/LFENCE sequence. The residual
from outer begin to this marker includes hardware entry and these instructions.
It is not subdivided further. Marker-to-original-inner includes remaining saves,
readiness and the original timestamp sequence. The wrapper's identity lookup is
after the original inner timestamp. It increases interior observer work; it
cannot explain a preceding timestamp difference without separate evidence.
Original leading, interior, trailing and envelope remain independently reported.
Every original probe has a separate checkpoint record, including ineligible
probes; only original eligibility admits a decomposition.

The hosted assembly execution substitutes only the privileged return, and tests
both readiness states, all fifteen GPRs, exact stack position, frame offsets,
vector, scope token and DF. It does not claim physical IRET validation.

## Interior method

`continuous-mask-lock-body-v2` has separate records and grants only
`DIAGNOSTIC_MINIMUM_EXPOSURE_ONLY`. The begin is after successful ticket acquisition
and observer validation. The end is the first ordered read in the hook called
before the actual `spin_unlock` changes lock state. The legacy released hook,
which remains after unlock, is not used. Independent wait/held maxima are never
summed. This version collects held bodies only.

Each endpoint retains actual CPU, epoch, IF flags, enable-segment counter,
switch sequence, external-entry sequence, returning-fault sequence, stack slot
and lifecycle generation, lock/depth, identity-query count and recorder sequence.
A successful endpoint pair requires unchanged identities and transition counters.
A restore from IF=0 to IF=1 increments the segment before POPFQ, including a restore
whose publisher cannot return. Nested restores of IF=0 and already-enabled restores do not increment it.
Version 1 lacked the already-enabled guard and is not current continuity authority.
Current reconstruction also rejects any recorded untracked controller transition.
Context switches and external entries already increment their respective counters.
The only returning exception handlers, #GP and #PF, increment a separate global
fault counter before any fixup. Saturation/overflow is unavailable. Other exception
handlers panic without a returning endpoint. Bootstrap idle STI sites are outside
live lock bodies; the task-wrapper enable uses the observed restore adapter.
These source boundaries and emitted instruction sites are part of the frozen
candidate contract. Two clear endpoint IF bits alone do not establish continuity.

All records use bounded per-CPU/per-nesting storage. A closed endpoint is consumed
once. Per-path valid/invalid counts and maximum witnesses remain until the CPU's
existing terminal freeze; reporting runs afterward in thread context. Invalid
continuity makes that CPU/path's diagnostic authority unavailable. Owner metadata
is read through the existing copy-only stack-owner reader, without retaining a
worker stack pointer. The hook allocates nothing, prints nothing, takes no lock
and reads no HPET. Identity-query counts include only IF=0 queries, so a migrating IF-enabled caller
cannot update another CPU's counter. Query and recorder-sequence deltas expose nested
observer work; elapsed cycles may also include host/asynchronous delay and nested
memory work. A maximum alone does not identify an allocator's unique cause.

Both independently reconstructed pre/post frequency contracts must pass. The
fixed upper divisor is ceil(max(pre.upper_hz, post.upper_hz) * 1010000 / 1000000).
This uses the existing one-percent conversion reserve in the conservative upper
frequency direction. The diagnostic duration is floor(cycles * 1000000000 /
upper_divisor), with checked arithmetic, and no allowance subtraction. Witnesses
must fall after the last eligible local pre point and no later than the last
eligible local post point. Unknown bounds or missing epoch coverage are unavailable.
An admitted value >=50000 ns demonstrates an observed minimum exposure under this
clock model. Any smaller value establishes no passing upper bound. Whole-series
observer, coverage, strict upper budget, native/production and complete campaign
predicates remain mandatory for acceptance.

## Terminal and role contracts

Ordinary work and joined post-calibration workers precede WORK_CUTOFF. Existing
per-CPU recorder snapshots precede SNAPSHOT_CUTOFF. The read-only `irqlatency audit`
waits boundedly for all admitted service generations, emits every absolute raw
service record and rejects a missing record, overflow or a changing final tail.
It performs no new allocation/invalidation workload. Final replay verifies the
same VM's earlier TLB workload and exact shared acknowledged-service semantics,
then requires the full final generation ledger. No generation is reset.

Strict keyboard transport retains raw QMP input/memory observations, unique
CRC-framed commands, ordered ACCEPT/BEGIN/END and matching REJECT/REPLAY rejection.
The guest Enter state must be released within the unchanged five seconds, measured
separately from command completion. QMP physical reads do not stop the guest.

Functional SMP24 validates all 24 actual vCPU threads and retains oversubscription.
Only a raw, precisely identified timing-only finish refusal may coexist with
functional success; ordinary nonzero results or missing terminal state fail.
SMP8 timing still requires adequate capacity and every complete timing predicate.

## Measured complete-word bitmap correction

The first entry diagnostic (candidate 9cfdf75e8385d7de01925603597deba8ac12bb83d838219f5091b195a6d33406)
passed all eight pre/post frequency and original observer-envelope predicates.
Its complete maximum records retain above-budget PMM-integrity observations.
The version-1 interior result is not the authority for this correction; its
already-enabled counter guard was subsequently found incomplete. The accepted
whole-window primitive and its own pre/post envelope remained intact.

`pmm_validate_integrity` holds `g_pmm_lock` across the entire bitmap validation and
free-count comparison. Every complete bitmap word previously required up to eight
byte loads, shifts and ORs in `bitmap_load_word`. The local correction uses a
compiler copy of exactly eight bytes only after proving eight bytes remain.
The target's little-endian requirement is asserted. Unaligned byte buffers remain
valid; partial final words retain the original byte loop and zero extension.
No bitmap extent, high frame, tail mask, reservation, corruption check or count
is omitted. All callers keep the original lock and linearization point. The
reserved bitmap and free count remain from the same locked logical state.
First-fit traversal order and atomic commit are unchanged. No new shared state,
retry, allocation, optional ISA, alignment assumption or sleeping lock is added.

This is an intentional production change to `utils/bitmap.c`; it does not claim
object equality. The fifteen previously compared instrumentation-only units keep
their own equality contract. Actual word/tail, unaligned, high corruption and
selection tests plus the inherited allocator/integrity suite check semantics.
The before/after candidate and emitted-code receipts distinguish this change
from the separately repaired diagnostic segment counter. In a valid continuously
masked PMM body, the already-enabled-restore branch does not execute. Host pauses
remain included and a smaller observed maximum alone is not a unique causal
attribution or a final passing upper bound. Required UART proof emission under
the separate serial lock remains measured and is not removed from path maxima.

The subsequent coherent PMM observation correction is documented in
`irq-latency-corrections.md`. It intentionally changes `memory/pmem.c` in production;
the current explicit change contract supersedes equality only for that measured
unit. The fifteen equal pairs above describe the earlier candidate, not that
successor. Its specialized stack-proof drain also supersedes synchronous UART
output during memory scope while retaining the original immutable proof bytes.
