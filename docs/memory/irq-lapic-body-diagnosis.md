# LAPIC handler body diagnosis

`lapic-handler-body-v3` is an optional entry-diagnostic observation. It does
not replace, adjust or qualify the enclosing interrupt-unavailability witness.
The current whole-window recorder, original probe margins and per-CPU allowance
remain active. Production and ordinary DEBUG contain none of this observation.

The actual `irq_lapic_timer_handler_inner` call site records four ordered local
TSCs: before its clock read, after that read, after the clockevent/statistics
work, and after reschedule notification and the existing EOI. All preparation
precedes the first read; validation/publication follows the last. The three
differences belong to one event. They include their declared timestamp and
scalar-store boundaries, observer work inside callees and possible host delay.
They are not uninstrumented instruction costs. The later scheduler/preemption
epilogue, architectural entry/return and diagnostic publication are outside this
interior but remain inside the mandatory enclosing measurement when masked.

A fixed record per discovered CPU owns a sequence reservation. An active LAPIC
outer record supplies its actual begin sequence; a validated same-mask identity
supplies CPU ownership. Completion verifies the original CPU, epoch, owner and
generation, uninterrupted mask segment, switch/IRQ/fault sequences and nesting.
An intervening restore, context switch, fault, replaced outer record or stale
reservation invalidates the result. No live stack pointer is retained. A
duplicate or recursive writer invalidates evidence without overwriting the
reserved record. Terminal reporting requires every reservation retired.

Every valid handler body is counted. Bodies with no participating memory path
are counted separately. The greatest memory-participating body retains all four
same-event timestamps and continuity fields; it is independent of optional detail
capacity. This is a fixed diagnostic maximum, not a complete chronology or a
claim that it is the event supplying the greatest enclosing window. Matching
outer sequences may establish that association; independent maxima may not.

The checker reconstructs all identities, ordered stages, counts and the maximum
duration. It uses the existing independently qualified pre/post CPU frequency
upper bound with its reserve and downward rounding. A long valid body is minimum
exposure only; a short one grants no below-budget acceptance. Historical records
without this source-bound method are not relabeled. Unknown methods fail.

The first experiment holds the checked-clock candidate's timestamp primitive,
calibration effort, optional-detail policy, 2-GiB RAM, eight CPUs, default host
placement and workload constant. It adds only these four timestamps and bounded
recording. Its discriminator is whether the recurring long LAPIC/heap outer
window also occurs in the handler body, and which of its three broad regions
contains it. It cannot uniquely distinguish software work from interference.

The first diagnostic found the same outer sequence in its largest body witness:
the clock call contributed 1,930,188 of 2,435,562 interior cycles. Version 2
therefore adds only two timestamps immediately around the first actual masked
`hpet_read_clock_sample` in that clock call. CPU identity is already validated
by the clock under its original exclusion. A fixed CPU-local slot is claimed
once; later calls in timer/keyboard work cannot overwrite it. The original four
timestamps remain, yielding five same-event regions: clock preparation, HPET
sampling/conversion, clock publication, timer work and completion. The sample
bracket includes the safe high-low-high reads, conversion and their existing
debug work; it is not a timestamp of one MMIO instruction. No HPET or deadline
semantics change. Version 1 records retain their original frozen checker and
result; they cannot select this current version.

## Counter-read subdivision

`lapic-handler-body-v3` retains every primary and sample endpoint from version
2. That candidate observed a same-event 8,192,337-ns minimum inside the first
HPET sample, versus 1,500 and 1,868 ns in clock preparation and publication.
Neither those data nor a fixed instruction count identifies a unique host or
software cause inside the sample.

One ordered timestamp after the actual safe counter-read function separates
counter access and its existing tracing/statistics from subsequent conversion
and output assignment. It is written only when the existing tick owner supplies
a bounded destination. The same HPET implementation supplies the ordinary API;
production, DEBUG and SELFTEST without entry diagnosis retain their original
compiled implementation. No counter access, fence, retry, extension, arithmetic,
clock deadline or interrupt state changes. The new field is not a clocksource.

The claimed sample occupies one aligned cache line per CPU and retains actual
ticks, mode, retries and validity. Reporting emits a separate bounded scalar row
bound to CPU, epoch, tick sequence and outer sequence. The checker requires its
complete inventory and the same counter domain as calibration. Failed reads,
missing boundaries or a sample outside the demonstrated epoch cannot supply a
minimum. Every counter/read/conversion region still includes its declared clock
boundaries and possible interference; no segment is called pure software time.

Actual HPET/MMIO tests retain all three high/low/high accesses and bounded retry
exhaustion. Controller tests inject delay before sampling, within counter access,
after the new boundary and after publication, preserve the same-event whole
body, and reject reused, missing or contradictory sample identities. One newly
frozen diagnostic will distinguish counter-read from conversion dominance. It
cannot count as an accepted timing boot or reinterpret either earlier method.
