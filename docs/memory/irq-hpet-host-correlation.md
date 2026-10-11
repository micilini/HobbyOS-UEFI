# HPET counter and userspace read correlation

`qemu-hpet-counter-correlation-v2` is a diagnostic adapter for the existing
`lapic-handler-body-v3` counter witness. It adds no guest timestamp or clock
implementation. The source-bound plan uses ordinary Q35/KVM/SMP8, 2 GiB and
default placement. QEMU's existing read, KVM-run entry and exit events are enabled for
the complete stress command. The 90-second and 512-MiB limits, four-GiB
provision, emergency reserve and owned-VM cleanup remain enforced.

The preceding mandatory-observer contrast retained violations with optional
causal probes compiled out. Its CPU0/CPU1 post envelopes were 20638/23418
cycles against their frozen 4180/3776 allowances. Its service audit passed;
its diagnostic whole-window maximum was 1326993 ns. Those observations do
not identify a unique cause. The earlier frequency-qualified LAPIC witness
contained a 372038-ns minimum counter-read region and an 859-ns conversion
region in the same event. Conversion alone does not explain that event.

The checker independently reconstructs the current guest witness, then joins
its actual HPET value to one successful high/low/high triple on the actual
QEMU CPU/TID. Counter mode, retries, epoch, outer sequence, source/candidate,
QMP VM identity, thread lifetime and trace enable/disable transcripts must
agree. Missing or ambiguous triples are retained as partial/unavailable;
there is no invented host match. All eight CPU inventories are checked. APs
with no memory-participating LAPIC event retain an unavailable witness; they
cannot be relabeled as zero-duration samples. A retained maximum outside the
trace window remains unmatched.

In pinned [QEMU v8.2.2 memory.c](https://raw.githubusercontent.com/qemu/qemu/v8.2.2/system/memory.c),
the event is after the device callback. Its value precedes access-width
masking. The parser therefore applies the requested 32-bit width to the low
read. Event timestamps use QEMU's microsecond wall-clock log, with separate
monotonic/realtime anchors. They are not guest TSC timestamps. Callback gaps
include guest reentry, emulation, logging and possible scheduling or memory
interference; they do not isolate one of those causes. The first callback
does not timestamp entry to the first MMIO access.

The pinned KVM trace marks `kvm_vcpu_ioctl` before its ioctl and
`kvm_run_exit` after the return and architecture post-run handling. For two
adjacent matched reads, the checker reports read-to-next-run,
run-plus-postrun, and exit-to-read separately. The first and last are
userspace execution/interference regions, with logging included. The middle
is not pure guest execution. Missing entries/exits or a non-MMIO exit leave
the subdivision unavailable, while retaining the original read match.

Kernel perf/trace access is unavailable under the recorded permissions.
Userspace tracing needs no privilege change, but perturbs execution and has
no complete loss counter. Exact matches remain diagnostic associations,
never an accepted duration bound or a subtraction from guest elapsed time.
All original pre/post probes, maxima, frequency rules, allowance, work and
terminal service checks remain unchanged. The old trace method and original
HPET telemetry captures retain their original source selector and verdict.

A declared trace-size/time/space stop does not establish a complete capture.
The separate bounded-prefix checker retains that original failure and uses
only complete event triples before the actual limit observation. It verifies
both the limit-disable and final-disable raw QMP transcripts, thread identity,
candidate and clock anchors. Every unqualified or missing guest maximum stays
explicit. Prefix matches cannot repair the full-capture predicate, a timing
streak or the full implementation result.

Stopped traces may be stored losslessly as XZ. The streaming consumer checks
the encoded identity, bounds expansion to the original sealed byte count, and
verifies the original raw SHA-256 after consuming every line. The original
seal and capture failure are unchanged. Compression does not turn a prefix
into a complete capture or require a second expanded trace in replay scratch.
