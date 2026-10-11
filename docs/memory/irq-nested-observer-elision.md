# Nested observer updates

`nested-observer-noop-elision-v1` retains the same ordered timestamp primitive,
29-cell witness, CPU-local calibration, frozen allowance and postcheck. The
change concerns two recorder calls from `irq_latency_after_mask`, after the
existing before/after CPU and switch checks and actual CLI boundary.

The preceding clock-call diagnostic observed three nested MMIO trace saves per
HPET split read. Each drained an empty pending ledger and began an already open
mask without adding a path. The actual C HPET/MMIO/controller test reproduced
twelve recorder-sequence increments. Those historical clock-call minima remain
unavailable under their captured method.

An already masked, same-CPU call may omit those two updates only when the
recorder is initialized, active, idle and in a nonexhausted even publication
sequence; its pending count is zero; and the caller adds no new path. Sequence
headroom must cover both original calls. Identity, stack generation, transition
bookkeeping, saved flags and timestamps still execute. No timestamp moves,
latency subtraction, IRQ enable or production lock change is involved.

Only the owning CPU opens/closes an interval or changes its pending count, while
masked. A remote publisher or owner retirement can complete an existing pending
slot but cannot create one or increment that count. Therefore a zero count
cannot hide such a completion. A nonzero count takes the original path even if
no slot is ready yet. New paths, uncertain identity, inactive state, recursion,
exhaustion and unsupported path bits also retain the original path.

With these conditions the original calls changed only the recorder's update
sequence, busy flag and a store of the same path mask. All maximum/count,
pending, lifetime, allowance and attribution data are byte-identical. Actual C
tests compare those helpers with the successor, including completed and
nonreturning publishers, new paths, busy/odd/overflow states and original
controller regressions. The skipped-update counter is separate diagnostic
metadata; it is neither a latency adjustment nor timing authority.
Each CPU's counter occupies a separate 64-byte aligned slot, so the new
diagnostic writes do not share a cache line with another CPU's counter.

The renewal permits a new finite causal experiment after tests and an identified
build. One fresh diagnostic boot will hold the original calibration, workload,
probe and host placement constant and change only this no-op update policy.
The predicted discriminator is zero recorder-sequence change in a nested clock
call with no pending work, while actual nonempty publication still changes the
sequence and remains unavailable under the clock-call rule. Neither a small
interior minimum nor this counter qualifies the whole-window observer. All
original pre/post probes and every maximum remain authoritative inputs to their
unchanged predicates. Any follow-up requires a distinct evidenced correction
or observation, not an identical retry.

## Precheck successor

`nested-observer-precheck-v2` makes the same no-op decision before the redundant
nested ordered read and second C hook. It does not alter the timestamp primitive
or either outer endpoint. The prior method above remains the interpretation of
its captured boots. A current BEGIN, source contract and experiment must name the
successor; historical replay continues using its own source export.

The existing physical-stack token must already be valid before this call. A new
architectural lookup or a token reseeded during this call does not admit the
precheck. The token checks the actual CPU, live stack generation, recorder epoch,
switch/IRQ/fault sequences and continuous mask segment. Saved and observed flags
must agree, the CPU must already have begun tracking, and all original no-op
conditions still apply. No pending publisher, new path, busy/odd/exhausted
recorder or exhausted accounting counter takes this path. No identity lifetime
is extended. The original post-CLI no-op check remains the fallback.

On admission the assembly's existing false-return branch still executes CLI and
returns the exact caller flags with IF clear. It omits only the nested timestamp,
its scalar store, and the second observer call. That timestamp was never used as
an endpoint: the active outer begin, attribution and closing endpoint remain
unchanged. All work that actually executes with IF clear remains inside that
outer interval, including the precheck. There is no latency adjustment or store
fence removal at a retained timestamp. This is a source-proved redundant observer
operation, not an explanation of a historical host pause or return excursion.

A separate per-CPU `prechecked` count reconciles with the existing total elisions;
it cannot exceed that total. Both counters share their original CPU's aligned
64-byte object, with no additional cross-CPU writer. Counter exhaustion retains
the original observer path and rejects the terminal ledger. Tests execute the
actual controller and assembly with privileged IF effects substituted, check
exact RFLAGS/DF and outer witnesses, and refuse stale generation, migration,
switch/IRQ/fault boundaries, pending publication, new paths and invalid recorder
states. The pre-edit normal/UBSan/ASan regression failed at the redundant hook.

A new untraced diagnostic candidate will retain the ordinary RAM backend and
unchanged fixed calibration/stress/postcheck. Its discriminator is actual
precheck coverage with unchanged mandatory accounting and original envelope
checks; a lower maximum alone does not explain past excursions. No traced or
preallocated run is promoted to ordinary timing acceptance, and no unchanged
retry is authorized by this correction.

## Scoped identity reuse

The first precheck candidate executed on all eight CPUs with exact mandatory
accounting and 2,512 complete service records, but its original post-envelope
checks rejected CPUs 2, 4 and 6. The raw data remain unchanged. Three ordered
clock calls from the same writeback request, with identical continuous-mask,
CPU, epoch, owner, generation, IRQ/switch/fault identities, supplied disjoint
observed minima totaling 112–131 us on four CPUs. This is a minimum over actual
same-event intervals, never a sum of independent maxima or an accepted upper
bound. The original safe HPET reads, MMIO recording, fences and any interference
are all included; their unique cost is not identified.

A second source inspection found architectural queries repeated inside the
scope-entry, callback begin/end and request-result hooks even when their own
IRQ save or external entry had already seeded the validated mask token. Those
four lookups now use the existing `masked_slot_now` helper. Its complete token
validation and raw architectural fallback are unchanged. Outer save/restore,
hardware entry, context switch, reference points and independent interior
endpoint queries retain their original hardware identity acquisition. No cache
survives an IF-enabled, owner, switch, entry or fault boundary by this change.

The actual controller regression failed on the incoming scope-entry query in
normal, UBSan and ASan. Its successor verifies no repeated raw identity query
inside a valid mask, the two genuine outer-save queries and post-restore query,
and architectural fallback after each invalidation. Component endpoints,
callback operation/root/generation, mandatory counts/maxima and all original
HPET calls remain. Production is unaffected. A separately frozen after-candidate
may exercise this concrete observer correction once; it cannot turn the first
candidate's failed postcheck into success.
