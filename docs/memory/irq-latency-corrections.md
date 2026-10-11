# Measured PMM observation and stack-proof corrections

The continuation preserves the complete prior dirty checkpoint, candidate
`e7385fe7f36c3d6155f3f32e93d982b50b22624b5639c46cd50c10583db350e6`,
and its rejected CPU5 envelope. The latest user request authorizes continuing
repairs. Each further boot needs a changed candidate and a bounded declared
discriminator. No historical failure, numeric tolerance or budget is changed.

## Causal starting evidence

The current independently frequency- and continuity-qualified PMM integrity
interiors are 95,563–122,296 ns. Their source is the full bitmap/count validation
under the PMM lock. The prior qualified whole-window reference independently
supported this same path. The proposed correction addresses the duration of that
coherent observation; it does not sample frames or cache prior success.

The CPU4 held serial-lock body is 22,026,243 cycles, at least 8,340,014 ns with
the fixed upper-frequency divisor. Its source path emits a completed guarded
stack lifetime proof through the polled UART. That witness contains one masked
identity query and no recorder-sequence increment. Host pauses remain possible;
the retained bytes and source show substantial synchronous I/O inside this body.

## Coherent incremental PMM observation

The original bitmap, reservation bitmap and global free count remain the sole
allocation state. A bounded observer slot holds a prefix length and counts of
free bits, invalid reserved/free bits and the two reachability subranges.
Under the PMM lock, it scans at most 4,096 logical frames, then restores the
caller’s enabled IF state before taking the next chunk. It performs no allocation,
wait, retry-until-quiet, sampling or scheduler-policy change.

The invariant at every PMM lock release is that each active observer’s counts
describe the **current** contents of its completed prefix. Before each allocation,
free or reservation write, the writer subtracts the exact old counts on its
intersection with that prefix and adds the exact new counts. Allocation and
reservation make that intersection allocated; a permitted free has already
proved the reservation bit clear. Thus no legitimate writer introduces a new
reserved/free violation. The next chunk extends the same invariant. When the
prefix reaches the entire logical span, its counts and the global free counter
belong to the state under that final lock acquisition: the linearization point.
This is not a mixed-epoch sum compared to a later counter.

Writer audit: `pmm_alloc_frame`, `pmm_alloc_contiguous_frames`, `allocate_range`,
`pmm_free_frame`, `pmm_mark_frame_used` and `pmm_reserve_range` call the accounting
helper before their unchanged logical update. Bootstrap initialization runs before
observers exist. Direct-map activation changes pointers only; each chunk and
writer verifies the captured dimensions, buffer identities, minimum and ceiling.
The PMM buffers retain their permanent boot-owned lifetime. No worker-stack or
task pointer is retained. Reservation, search order, atomic allocation ownership,
high-frame corruption detection and all original bitmap storage remain intact.

There are `2 * HOBBYOS_MAX_CPUS + 1` static observer slots, covering twice the
declared CPU worker population plus its controller. Capacity exhaustion and
IF=0 callers use the original complete locked observation. That fallback preserves
API behavior and may exceed the latency budget; it cannot manufacture success or
enable interrupts that the caller masked. The required enabled-context workloads
must demonstrate completion using the incremental path. Small inputs retain the
original single-lock behavior. Reachability now also validates the entire span.

## Immutable stack lifetime output

Only the existing stack ALLOC/RETIRE proof rows are deferred while a measured
memory scope is active. Formatting and CRC bytes remain identical. A producer
reserves one absolute sequence and copies into its own preallocated slot before
release publication. No allocation, IRQ-disabling lock, printing or retained
worker-stack pointer occurs in publication. Failed reservation, missing publication,
sequence mismatch, overflow or malformed length is a real failure.

The queue holds 1,024 rows of at most 767 bytes. The largest declared single
ordinary lifetime operation creates/retires 258 churn tasks (516 rows); the
257-task setup/cleanup phases are separately completed commands. Calibration
uses at most 32 workers (64 lifetime rows), and stress uses at most 64 workers
(128 rows). The capacity therefore covers one complete declared command plus
bounded adjacent worker cleanup without overwriting an active row. Actual raw
absolute counts and high-water behavior remain checked; this is not permission
for an undeclared larger workload or silent overflow.

The original diagnostic-result adapter drains completed immutable rows outside
memory scopes and before the handler result is published. This reporting step
does no allocation, integrity scan, mapping, retirement or cache maintenance.
The polled serial implementation and its atomic lines are unchanged. Both the
original row CRC and a begin/end absolute-count ledger are retained. Work cutoff
requires a fully drained queue; final audit requires the same terminal count.
The independent verifier rejects missing, duplicate, late and incomplete rows.
No ordinary memory work moves outside its measured scope or terminal cutoff.

Production includes the actual PMM/bitmap correction. The proof queue and its
adapter hooks exist only in the instrumented selftest role. Intentional production
differences are source-bound and tested separately from unchanged-unit equality.
The original compiler hashes, historical source selectors, original envelope,
postcheck, frequency rules and strict 50,000-ns budget remain unchanged.


## Current coherent-observation diagnostic

Candidate `dbcdd93be300a87fe1b036c11b7930ca3c58d0de4bfdfb48fc54f386241e9e06`
ran the unchanged calibration, memory command, 16-worker stress, postcheck and
terminal audit. All eight pre/post frequency criteria passed. All 2,512 service
generations and 52 deferred stack lifetime records reconciled. CPU3 rejected
its post envelope: 7,098 cycles against its own 5,000-cycle frozen allowance;
leading/interior/trailing were 792 / 15,086 / 6,306 cycles. This is a new tail
observation, not a resolution of the earlier leading excursions.

PMM integrity held-body diagnostic minima by CPU were 47,189 / 13,166 / 9,495 /
7,467 / 9,466 / 6,948 / 15,326 / 3,113 ns. These shorter subsets do not certify
the whole operation, ticket waiting or any passing upper bound. All maxima,
invalid states, calibration bytes and auxiliary omissions remain retained.

The largest independently qualified interior was CPU0, heap path,
`g_serial_lock`: 22,027,848 cycles, minimum 8,334,077 ns with a frequency upper
limit and downward rounding. Its endpoints have one architectural identity
query and no recorder-sequence increment inside. The raw stream retains the
two mandatory heap overflow/refusal WARN rows; `assertion_warn_report` formats
and synchronously emits them while masking IRQ, including its UART lock. This
source path explains a concrete avoidable reporting cost. It does not uniquely
attribute every cycle or rule out host pauses. Nested stack/paging maxima also
contain 57 architectural identity queries and 72 recorder sequence increments.

## Continuous-mask identity successor

`stack-mask-identity-v2` changes observer identity lookup only. The ordered
boundary primitive, 29 cells, frozen allowance, original four probe endpoints,
reference pairing, tolerances and budgets are unchanged. The actual current
BEGIN and sealed experiment plan must name this method; historical captures
continue using their immutable earlier source and method selectors.

A token is stored by the current physical stack slot. That slot selects
storage, never a CPU. Only an actual architectural lookup seeds the CPU.
Reuse requires IF=0, the same live stack generation, recorder epoch, mask
segment, original CPU's switch and IRQ-entry sequences, and recoverable-fault
sequence, with no exhausted counter. Metadata storage is permanent; no task
pointer is retained. It is read/written only by its on-CPU stack owner. Exact
off-CPU generation retirement invalidates it. A reused/reaped stack fails its
generation comparison before use.

Every IF-enabled save starts with a raw lookup, and its post-CLI hook performs
another raw lookup. Before any instrumented restore can enable IRQ, the token
is cleared and the mask segment advances. After restoration, hardware identity
is always reacquired. The corrected assembly switch hook uses hardware identity and
advances the old CPU's sequence before transferring stacks; a resumed or
migrated task therefore cannot reuse its old token. External entry uses a raw
lookup, advances entry sequence and seeds only the current stack. Return
invalidates it even for IF=0. The two recoverable exception paths advance the
fault sequence before attempting their existing exact fixups in every observer
build. Other exceptions, including NMI, are terminal and cannot resume this
ordinary hook chain. No MSR, GS, AUX, affinity, scheduler or exception-recovery
policy changes. Calibration identity samples always query hardware; they do
not use these tokens.

Actual-controller tests preserve the allowance/nonreturning-publisher tests
and exercise nested masks, a hidden enable/disable through the real restore
hook, migration with a switch, entry/return, stale owner and epoch, recoverable
fault and overflow, retirement, IF-enabled access and independent calibration
queries. The red test ran the old controller and failed on repeated queries.
Raw minima still report actual architectural query counts; removed operations
are not subtracted from any timestamp.

## Additive return checkpoint

`entry-return-checkpoints-v2` retains the earlier entry marker and adds one
ordered timestamp after the original return-tail END/OBSERVED_END/READY
publication. The C-selected probe pointer owns two adjacent 64-bit cells;
assembly writes the second only when both pending-tail and probe pointers are
present. It never indexes by a speculative CPU or a task's former CPU. All
15 GPRs remain saved; the existing frame offsets, extra 16 bytes, DF clearing,
stack alignment, dispatch, EOI and IRET path are unchanged. Hosted execution
checks register/flags restoration and that the original END equals the first
cell and precedes the second. Production compiles out both checkpoints.

The independent checker reconciles every original probe and both checkpoints,
requires outer_begin <= entry <= inner_begin < inner_end <= return <= outer_end,
and retains both tail-publication and remaining-return durations. The new
instruction perturbs this diagnostic candidate; the original allowance remains
computed from leading plus the entire trailing interval. A residual after this
checkpoint still includes restore/IRET and the outer ordered read; no assumed
entry/exit cost or host time is subtracted.

## Immutable memory-warning output

Only the instrumented build defers already formatted WARN text while a real
memory scope is active. Its original CPU/task generation, expression, line,
IF state and bytes are copied before restoring flags; ordinary builds and
fatal panic output remain unchanged. A separate bounded queue holds 64 rows
of at most 1,023 bytes (the original formatter's 1,024-byte bound). This adds
about 65 KiB, without changing the stack lifetime queue. The declared diagnostic
has two intentional heap refusal warnings; queue exhaustion is a sticky failure,
never omission or a reason to increase capacity after measurement.

Publication uses unique absolute sequences and release/acquire ownership,
with no allocation, lock, UART or retained stack pointer. Command completion
and finish drain outside memory scope with IF enabled, before successful handler
completion and work cutoff. Each original WARN is preceded by a CRC-bound
sequence/length/payload-CRC row; independent replay verifies exact original
bytes and required context fields. Work/final warning cutoffs must agree. Gaps,
wrong order, lost text, altered context, overflow, incomplete publication or
late ordinary evidence reject. This moves only proof emission; all rejection
checks, counters, allocation/integrity/retirement/cache work remain measured.


## Publication contention correction before the next boot

The current production semantic comparison exposed an actual eight-producer
failure in the stack-proof queue's 65-attempt CAS reservation loop, despite
available capacity. The failed run and exact binary/source remain retained;
this was not accepted as a transient test failure. Both specialized queues now
reserve one absolute sequence with a single atomic fetch-add. The capacity and
ready checks occur before slot access, so an over-capacity caller never touches
an active slot. There is no retry loop, blocking lock or capacity increase.

Reservation refuses at 2^63; even every bounded live stack owner simultaneously
past the precheck cannot approach 64-bit wrap. Exhaustion or malformed order is
sticky and prevents successful cutoff. The headroom is an arithmetic guard,
not an enlarged queue or discarded required record. Actual concurrent tests
still require every declared record exactly once, plus explicit capacity,
publication-gap, stale-sequence and counter-exhaustion failures.


## Switch publication correction

The first cached-identity candidate,
`f9b1ba6913f828532514c0e7e334c1386e1dcb035334036733874e01e6d2def9`,
completed frequency collection and its original all-CPU envelope postcheck,
but retained four invalid recorder boundaries (sites 3 and 22). No interior
minimum from that boot is authoritative and no functional or timing acceptance
is claimed. Its CPU5 pre-envelope maximum of 20,990 cycles remains retained.

The earlier description of switch ordering was incorrect: the original observer
hook ran after `scheduler_finish_switch`, not before stack transfer. That
function can release the outgoing stack for another CPU before the switch
sequence changes. A cache token from that stack could therefore remain valid
in the release window. An actual-assembly hosted test reproduces the violated
invariant at the finish callback, using the actual controller and aliased saved
RSP cells (hosted execution, not a guest scheduler test).

Version two moves only the observer call before saving/transferring RSP. The
six callee-saved registers are already saved; 40 temporary bytes preserve all
four switch arguments and the SysV call alignment. The temporary area is
removed before the original saved-RSP store. The original handoff arguments,
callee-saved frame and finish/return sequence are unchanged. Production emits
the original switch bytes. A release switch increment precedes any runnable
publication, and cache validation acquires that counter. This fixes a concrete
lifetime invariant; it does not prove that every historical invalid transition
had this cause. The old boot and old method remain rejected.

The current analyzer isolates entry/interior diagnostic failures so that raw
clock, envelope, workload and service predicates can still be sealed with their
real statuses. Recorder failure still rejects functional and timing acceptance.
A successful service audit is not overwritten by a separate recorder failure.


## Frequency-qualified heap expansion witness

The switch-publication candidate
`7e341745f192183b4b2cd000d1cd0c3e53f29cbc6f84f6335bcb8b826bd093ed`
completed all declared workers and terminal service proof with zero invalid
recorder boundaries, no missing CPU/path and no invalid CPU state. All eight
pre/post frequency predicates pass. Whole-window observer qualification still
fails on CPU0: post envelope 21,666 versus its own 6,544-cycle allowance, split
796 / 12,726 / 20,870. Of the trailing part, 176 cycles precede the added exit
checkpoint and 20,694 follow it. A separate CPU0 leading residual is 20,116
cycles. Their unique cause is unproved; the original postcheck remains active.

CPU1's independently checked continuous-mask heap-lock body spans ordered
TSC 92,430,399,762 through 92,431,310,570 (910,808 cycles). Upper frequency
2,641,429,551 Hz gives a downward-rounded minimum of 344,816 ns. Both points
have the same CPU, epoch, owner 131/generation 2, mask segment 454, switch
sequence 642, IRQ sequence 26,266 and fault sequence zero; both have IF clear.
The record has zero invalid interiors and lies between that CPU's complete
pre/post references. It proves an observed violation, never a passing upper
bound. It includes one identity query and 18 recorder sequence increments.

The independently retained CPU1 heap/PMM maximum and first failure enclose
those exact endpoints, with paths mask 3 and completion site `kmalloc+380` in
that ELF. The heap lock contains a nested PMM scope only in expansion; the
source performs contiguous backing allocation and physical translation while
holding it. The raw growth row records 34,603,008 requested bytes and heap
growth from 33,554,432 to 68,227,072 bytes before worker release. This is
specific source/work evidence for private expansion preparation. It does not
attempt to assign every cycle to the allocator or exclude a host pause. Other
heap minima without nested observer work remain separately retained.

## Private heap preparation and publication

Only expansion preparation moves: checked byte/page sizing, private PMM
allocation, RAM translation and initialization of the unpublished block. The
original heap list, first-fit scan, split threshold, block layout, coalescing,
region registration, counters and pointer validation remain unchanged. The
caller restores exactly its original IF state when releasing the heap lock;
an originally masked caller stays masked. No IRQ is enabled under a held lock.

After preparation, `kmalloc` reacquires the heap lock and scans the existing
list again in its original first-fit order. A concurrent free or published
expansion can satisfy the request; unused private backing is returned only
after releasing the heap lock. Otherwise registration and tail insertion or
coalescing transfer ownership once under the lock, with fresh capacity and
overflow checks. Split/state/accounting under that same lock are the allocation
linearization point. A failed allocation still performs the first-fit recheck;
a concurrent free can legitimately satisfy it. Preparation is bounded to one
attempt. No BUSY result, retry-until-success loop or alternate allocator is added.

Initialization similarly rechecks whether another initializer published the
first region before transferring its private backing. Every unpublished region
is either transferred once or released page by page outside the heap lock.
Failed translation, exhausted region capacity and total-byte overflow publish
no block or accounting increment. Existing PMM ownership and first-fit rules
apply to each private allocation and release. PMM time remains measured in its
own scope; moving it does not certify that operation below budget.

The old actual C heap fails the new lock-context assertion at PMM acquisition.
The corrected code passes normal, UBSan and ASan cases for original first-fit,
aligned/interior/double free, overflow, caller IF=0, concurrent free during
preparation, allocation/translation failure, failed publication and two actual
threads preparing expansions together. The two-thread case publishes one
region, satisfies both requests in order and releases the unused private region
without leaking or freeing a published page. Production now deliberately
changes heap as well as PMM/bitmap; its object must differ and its semantic
subject must execute. Unchanged units retain exact paired-object equality.

## Private fixed-window table preparation

The diagnostic candidate `0bfd18fb93dc888d7fd52718d025e9fb7bd12fbb5b9ebd88cdac233e92989d45`
passed every CPU's frequency and original post-envelope check. Its complete
budget still failed. The independently reconstructed CPU0/CPU6 paging-lock
interiors were 140,810/205,496 cycles and at least 53,289/77,709 ns respectively.
Both enclosing endpoint sites resolve to `paging_tlb_fixture_prepare`; the
interiors contain four/three identity queries and 72/54 recorder transitions.
These counts and the original source implicate nested allocation/zeroing under
the paging lock, without identifying every cycle as allocator CPU time.

The fixed 65-page test window crosses two PTs in one PD. A missing window needs
at most four structural tables: one shared PDPT, one PD and two PTs. Compile-time
checks retain the fixed root and alignment. A read-only locked preflight counts
unique missing tables and validates existing entries. Preparation allocates and
zeros exactly that bounded private set outside the paging lock. The original
locked walk revalidates root, flags, registered physical origins and all leaves,
registers each consumed table immediately before linking it, and publishes all
65 data leaves only after the complete validation. Registered tables remain
owned for the boot, including an original-style partially installed failure.
Concurrent preparation can only reduce the number of missing structural tables.
A competing leaf publication is rejected; unused private frames are released
only after the original flags are restored. There is no change to shootdown,
ACK-before-data-reuse, leaf attributes, operation counts or production code:
these fixed fixture helpers remain SELFTEST-only.

The actual-C hosted test failed on the preceding implementation's PMM call under
the paging lock. Normal, UBSan and ASan now cover allocation and translation
failure at every private table, capacity/root changes before publication,
existing leaf/pointer corruption, NX absence, exact IF preservation and two
concurrent preparers. Exactly one competing preparation publishes; every unused
frame is released once, and no registered table is freed. A small after maximum
cannot establish a passing whole-window bound.

## Entry admission across activation

The table-preparation candidate `f4e11499b54900069c4430fd5e361108d8e17e7a1d3f38bcabff4705eb1b5264`
completed the declared stress and final service audit, but its controller had
one missing begin on CPU4 at exit site 22. Its independent CPU1 post envelope
was 6,216 cycles against a frozen 4,200-cycle allowance (leading 5,566,
interior 14,132, trailing 650). The earliest residual was 5,408 cycles. Both
failures remain failures; the current interior checker correctly withheld all
minimum-exposure authority for the incomplete controller transition.

An actual-controller regression reproduced a source race: an interrupt enters
while the global observer is inactive, then another CPU activates it before
that interrupt returns. The old exit inferred admission from the current global
state and attempted to close an interval that had never been opened. The old
capture lacks a timestamped admission cookie, so this reproduction does not
uniquely attribute that captured event.

The versioned `entry-admission-cookie-v1` uses the existing assembly frame cell
to preserve independent admitted/scoped bits. No frame layout or timestamp moves.
An unadmitted return with no active interval emits no invented closure. If an
observed mask opened an interval after activation, that interval still closes
at the original real return endpoint. An admitted missing begin, failed scope,
unknown cookie, lost allowance or publisher failure remains invalid. Tests run
the actual controller for the inactive-to-active race, a later early interval,
an admitted erased begin, malformed cookies and failed scope acquisition. The
current raw BEGIN and sealed plan must name this admission method; historical
checkers retain their original method and verdict.
