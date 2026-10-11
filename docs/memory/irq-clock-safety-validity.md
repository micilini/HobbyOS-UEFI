# Checked request safety time

The actual clock/request integration test exposed a fail-open boundary: an
invalid HPET sample returned the compatibility API's last monotonic floor, and
a request with all ACKs could report success with zero elapsed time. This is a
hosted control-flow finding, not a newly observed guest failure or a timing cause.

`clock_monotonic_read_ns` shares the real HPET acquisition, CPU/revision
revalidation, sample classification and global publication with the existing
nanosecond API. It succeeds only for an identified CPU and a nonfailed,
nonzero, nonsaturated result. Failure leaves the caller's output unchanged.
The compatibility API retains its monotonic-floor behavior. A floor following
a failed sample is never fresh safety time for a request.

The request dispatcher uses this checked result at its four existing admission,
admission-wait, completion-wait and terminal decisions. Invalid admission starts
no operation. Failure after publication retains the request's storage under the
existing quarantine protocol, even when all ACKs arrived. A nonempty completed
request must demonstrate clock progress. Its terminal value must remain below
the unchanged one-second deadline. No TSC deadline or cached-clock fallback is
introduced. Actual invalidation, writeback, immutable targets, release/acquire
ACK ordering, EOI and resource lifetime are unchanged.

The new status is per invocation and does not race through a global last-error
flag. The existing exact flags, failed-sample retention, cross-CPU lag
classification and bounded stale-history fallback remain. Original IF=0
callers stay masked; optional clock-segment observations still cover that work.
The call-site identity is passed through the shared reader so the diagnostic
continues to identify its real caller. Unavailable elapsed time uses the existing
invalid sentinel and cannot satisfy a timing predicate.

Actual-function tests combine the request dispatcher, state machine, local TLB
code and clock implementation. Only privileged effects and HPET input are
substituted. Valid completion, failed admission/read/terminal, regression,
stall, saturation and late completion are exercised under normal, UBSan and
ASan. Separate controller tests preserve exact flags and all enclosing
observations across valid, unavailable and reentrant clock reads. Hosted
privileged seams establish control flow and ordering, not a hardware flush or
an accepted runtime duration.
