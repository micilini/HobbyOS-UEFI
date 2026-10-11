# Continuous-mask cache writeback diagnosis

`continuous-mask-wbinvd-v1` is a separate diagnostic minimum, enabled only by
`IRQ_ENTRY_DIAGNOSTIC=1`. It never replaces the 29-cell whole-window witness,
CPU-local reference pairs, original four probe timestamps, frozen allowance,
post-envelope check, or strict 50,000-ns acceptance predicate.

The preceding candidate `85296e40d7ca9e65cec9aef0c127a09c1d12d7a519aac401fb1ecb0bfae187d5`
had valid frequency collection on all eight CPUs and zero invalid controller
boundaries. Its CPU7 post envelope was 22,378 cycles against its own 6,124-cycle
allowance: leading 650, internal 9,624, trailing 21,728. The additive tail marker
localized 21,234 cycles to publication and its ordered read; the final residual
was 494 cycles. This is not a unique cache, instruction, or host attribution.
The contiguous per-CPU probe cells share storage lines, but no causal experiment
has established that sharing caused this maximum. Their layout is unchanged.

That boot also executed one measured cache-writeback callback per CPU, generation
2496. The enclosing callback observations ranged from 68,112 to 188,166 ns. Those
callbacks include validation, HPET reads, bookkeeping and the actual WBINVD.
They do not isolate the instruction or supply the new minimum witness. The new
candidate's purpose is to discriminate mandatory writeback execution from those
surrounding operations. No production operation is removed or reordered.

The diagnostic branch at the original instruction calls a specialized assembly
wrapper exactly once. It saves actual flags, performs the existing ordered
MFENCE/LFENCE/RDTSC/LFENCE read, keeps the combined timestamp in a caller-saved
register, executes WBINVD, performs the same ordered read and captures flags
again. Only register operations, WBINVD and ordered timestamp instructions lie
between the sampled timestamps. No CPUID, allocation, lock, HPET access, record
copy, formatting or call is in that interval. Stores to the output structure
occur after the second timestamp. This is an instrumented elapsed interval,
including asynchronous/host delay; it is not uninstrumented instruction cost.

Two independent architectural CPU lookups surround that assembly call. They are
outside its timestamps. The raw query counter must increase by exactly one
between metadata endpoints (the second lookup); the recorder sequence must be
unchanged. These counts are retained, not subtracted from elapsed cycles.
Both actual flags and surrounding metadata must have IF clear. CPU, calibration
epoch, mask segment, switch/IRQ/fault sequences, stack owner/generation and
nesting depth must agree. Unlike a lock-body witness, lock identity is zero and
depth may be zero. The existing transition hooks invalidate continuity across
restore/enable, switch, external entry or returning fault. Missing or invalid
metadata, saturation, duplicate request generation and an untracked controller
transition make this diagnostic unavailable.

Bounded per-CPU storage retains the first and maximum instruction witnesses,
valid/invalid counts and first/last service generations. It never overwrites a
larger maximum. The independent checker reconciles counts with actual callback
records and the same VM's complete final acknowledged-service ledger, including
operation 3, root, target and ACK identities. A one-call CPU must have identical
first/maximum records. This representation is not a claim of complete intermediate
instruction history when there are multiple calls; all original whole-window
maxima and auxiliary-overflow limitations remain separate.

Both pre/post frequency contracts must pass. The divisor is fixed before
collection as ceil(max(pre.upper_hz, post.upper_hz) * 1010000 / 1000000), and the
minimum is floor(raw_cycles * 1000000000 / divisor), with checked arithmetic and
no allowance subtraction. The interval must lie between the completed local
pre-calibration endpoint ledger and the local post-calibration ledger. A minimum
at least 50,000 ns demonstrates a violation within that clock/continuity model.
A smaller minimum never establishes an accepted upper bound. A failure in the
original post-envelope still rejects overall timing authority.

The hosted assembly substitutes NOP for the privileged instruction and tests
only ABI and timestamp ordering. Actual C normal/UBSan/ASan executions cover
continuity, owner/CPU/epoch changes, hidden enable, invalid flags, omitted or
duplicate endpoints/generations, observer work, arithmetic and strict threshold
boundaries. Fresh kernel disassembly must independently show exactly one WBINVD
between the two ordered reads and no hidden call/store in that interval.
Production retains the original inline WBINVD and excludes the wrapper, arrays,
reporting and test records. Same-local-compiler production comparison remains
mandatory; the three already intentional memory changes retain their separate
semantic proof. No hardware or host scheduling change is part of this experiment.
