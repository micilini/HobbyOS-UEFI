# Bounded heap operation attribution

The starting candidate is `dd5f2782f8ca7a87b6e54b03e0916d0dcd7a1bf369a329ff6b5a6b94c3264ac7`.
Its CPU3 heap-lock interior is a valid 217,077-ns minimum, but the record has no
allocation/free/statistics operation tag. No exact old operation is inferred.
Growth already prepares private backing outside the heap lock and rechecks
first-fit before publication. This diagnostic does not rewrite that algorithm.

`continuous-mask-heap-stages / 1` adds a fixed operation tag and at most eight
ordered checkpoints to each observed heap lock holding interval. The operations
are initial allocation, allocation recheck, free, statistics, integrity,
initialization and initial-region publication (IDs 1 through 7). Stage IDs are
region validation (1), first-fit (2), containment validation (3), pointer and
accounting validation (4), coalescence (5), allocation metadata (6), region
publication (7), statistics (8), full integrity (9), and initialization (10).
Counters report visited nodes/regions and initialized metadata bytes, not a
timestamp per iteration. Backing preparation occurs between separate held
intervals; the recheck records its requested pages and outcome. It is not
silently counted as work under the heap lock.

One preallocated active event per actual CPU is owned only while the existing
heap lock is held with IF=0. Begin/end carry the existing interior identity:
CPU, stack owner/generation, epoch, continuous mask segment, switch/IRQ/fault
sequences, depth, lock, observer queries and recorder sequence. Every checkpoint
is between those endpoints. Unexpected nesting, missing end, owner/CPU change,
overflow, unordered stages or publication after unlock invalidates the
diagnostic. No pointer to a worker or its stack is retained. A completed maximum
is copied only into fixed per-CPU/per-operation storage, with exact counts and
invalid counts; the frozen existing reporting consumer reads it. Original
enclosing timestamps, counts/maxima, allowance and postcheck remain unchanged.

The current lock's wait-start/acquired timestamps accompany that same event as
unqualified component information. They are not added to a held-body minimum.
The independent checker reconstructs the held interval and each stage using
the unchanged pre/post upper-frequency bound and downward rounding. A stage
minimum at least 50,000 ns can identify a violating subset. Smaller subsets
never certify an accepted upper bound. Observer metadata/copy work and host
interference remain part of elapsed execution; none is subtracted.

The first finite role observes these stages with optional recorder details
unchanged. A second role requires either an identified local correction or the
separately declared optional-detail contrast. Every role freezes its complete
candidate, fixed calibration/stress geometry and stop condition before boot.
No unchanged failure is retried to obtain a smaller maximum.

The audited old assembly tail contains no callee or history scan. Between its
inner-end and exit-checkpoint TSC reads it publishes two pending timestamps,
changes one claimed pending state, publishes the probe timestamp and executes
the original ordered-read fences. IF remains clear. Its elapsed excursion is
not evidence that the C pending-slot scan occurred between these timestamps.
That bounded sequence is preserved while operation-level evidence is collected.

The conceptual references are Linux **v6.12**, not copied implementation:
[IRQ flags](https://raw.githubusercontent.com/torvalds/linux/v6.12/include/linux/irqflags.h)
and [transition tracing](https://raw.githubusercontent.com/torvalds/linux/v6.12/kernel/trace/trace_preemptirq.c)
motivate preserving exact flags and suppressing only redundant transitions;
[irqsoff tracing](https://raw.githubusercontent.com/torvalds/linux/v6.12/kernel/trace/trace_irqsoff.c)
separates optional function tracing from latency accounting;
[mempool resizing](https://raw.githubusercontent.com/torvalds/linux/v6.12/mm/mempool.c)
illustrates private preparation followed by locked revalidation.
[Ring-buffer ownership](https://docs.kernel.org/trace/ring-buffer-design.html)
does not make an interruptible per-CPU writer automatically single-producer.
[KVM timekeeping](https://docs.kernel.org/virt/kvm/x86/timekeeping.html) and
[OS noise observation](https://docs.kernel.org/trace/osnoise-tracer.html) motivate
keeping interference hypotheses separate from demonstrated operation cost.
No Linux sample rejection, allocator, tracing service or host campaign is adopted.

## Optional-detail contrast

The first frozen stage candidate (`82e02c8e0a14d328158dced59db968b842569bc0133b26cbd4a01b353d62f705`)
qualified frequency on all CPUs, completed sixteen workers and the terminal
service audit, and rejected the CPU3/CPU7 post envelopes. The largest heap stage
was CPU6 allocation metadata: 623,464 cycles, minimum 235,974 ns, with one
32-byte header initialized. First-fit immediately before it visited 764 nodes
in 21,914 cycles (8,294-ns minimum). CPU0 containment visited 374 nodes in
528,186 cycles (199,931-ns minimum). CPU4's largest free event included a
201,139-cycle observer-return residual (76,058-ns minimum), after a 741-node
containment search of 16,198 cycles. These are different actual events, not
summable component maxima. No identity query or recorder-sequence change
occurred inside those held event boundaries.

The data locates excessive elapsed time in both allocator and observer regions.
The very large fixed-size metadata and return intervals prevent assigning all
excess to list traversal. Host interference remains possible; available trace
interfaces return permission denied and the host perf policy is 4. No host
settings are changed and no delay is subtracted. No production correction is
justified solely by choosing a convenient one of these stages.

The second diagnostic uses `IRQ_LATENCY_DETAILS=0`, permitted only with the
entry-diagnostic selftest build. The sole recording change skips the optional
29-cell failure-history copy in `irq_latency_record_drain`. Each skipped
record increments the existing omission counter. Invalid states, all path
counts/violations, mandatory maximum witnesses, enclosing timestamps, closing
allowance and generation-bound retirement remain identical. Storage layout and
the original assembly tail are unchanged; no work moves past an endpoint.
`mandatory-maxima-only-v1` is explicit in the build, plan, BEGIN record and
current checker. The default `bounded-details-v1` remains the acceptance policy.
Unknown or missing policy, historical promotion and a timing role using the
contrast are rejected. This diagnostic deliberately has no complete optional
failure chronology and cannot pass the complete timing gate.

The contrast tests whether excessive mandatory observations still occur when
optional detail copying is absent. A remaining excursion disproves the claim
that optional copying is necessary for that new excursion; it does not uniquely
explain the old CPU5 event. A smaller maximum alone is not causal proof or an
accepted bound. Original details and maxima stay in the first capture, and the
second capture retains its own exact compulsory observations and omissions.
