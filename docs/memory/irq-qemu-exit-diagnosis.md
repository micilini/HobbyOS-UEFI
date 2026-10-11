# HPET value correlation with QEMU userspace events

The incoming isolated metadata bracket and RAM-backend comparison did not
establish a unique software cause. Both workloads completed, but enclosing
shootdown maxima remained above the strict 50,000-ns budget. In the ordinary
backend diagnostic an independently checked, continuously masked HPET call
already reached a minimum observed 59,664 ns. This is a reason to inspect the
clock call's execution, not permission to remove it or its validation.

`qemu-hpet-read-correlation-v1` adds a host-only discriminator to the existing
diagnostic driver. The kernel, candidate, 2-GiB memory, eight vCPUs, default
placement, command order, workload, clock fences and calibration remain fixed.
One finite diagnostic traces the final part of the stress command, after its existing CRC-checked
192-mapping completion marker with QEMU's installed log
backend. Pre/post calibration runs with these events disabled. No result from
this role supplies a timing success.

The sole event is `memory_region_ops_read`. QMP reads back its state and the actual VM UUID and
vCPU/TID mapping. Existing strict keyboard transport still supplies commands.
An independent watcher disables tracing at 90 seconds, 512 MiB of trace, or
the four-GiB plus 64-MiB filesystem reserve. A reached limit or control error
means unavailable trace coverage. It never shortens the guest workload.

The parser joins each of the existing 24 clock-call records by actual CPU and
exact HPET value to the successful 32-bit high/low/high MMIO sequence. It does
not interpolate guest TSC across CPUs or extrapolate a frequency over a long
host timeline. Retries require a separate proof and are unavailable under this
first correlation method. Missing, ambiguous or reused values cannot become a
complete match. Every matched read retains its original trace line and the two gaps between those three read callbacks.

The first three-event diagnostic hit the 512-MiB limit before its target
clock calls and three workers exhausted their unchanged mapping-acquisition
bound. Its failure is preserved. The successor removes the two high-volume
ioctl/exit events and arms at an existing guest marker, instead of enlarging a
buffer or weakening the workload. The read marker follows the device callback;
its two gaps include guest execution, KVM transitions, QEMU/device work,
logging and possible host delays. They do not isolate host scheduling or
hardware VM-entry/exit itself.
[MMIO dispatch](https://raw.githubusercontent.com/qemu/qemu/v8.2.2/system/memory.c).

The log backend uses realtime microseconds and a native thread ID. Phase and
watcher samples retain bracketed realtime/monotonic observations. Reversed
per-thread timestamps reject correlation. Anchor variation is reported, not
subtracted. Anchors cannot exclude every clock adjustment between samples.
The log backend exposes no loss counter; complete global event capture is
unavailable even when all selected guest values match. No host interval is
treated as a formally qualified minimum duration or subtracted from the guest.
[Log backend](https://raw.githubusercontent.com/qemu/qemu/v8.2.2/scripts/tracetool/backend/log.py).

This experiment can establish that the same guest HPET call traversed particular
QEMU userspace regions and report their observed durations. It cannot alone
separate emulation, locks, logging, page faults, scheduling and other interference,
or explain an earlier candidate's heap/return event. Tracing itself changes the
environment. Production algorithms and the accepted untraced timing profile
are unchanged. Final acceptance still requires the full qualified enclosing
measurement, functional/native coverage and three consecutive timing boots.

The captured read trace also exercised an existing activation case: an IRQ
entered before global activation, a software save first observed masking inside
that handler, and its IRQ tail closed the excluded EARLY record. The actual
controller already tests this entry-admission behavior. The host endpoint checker
had required an IRQ-origin begin for every IRQ tail, incorrectly rejecting this
excluded mixed-boundary record. The correction admits only a current 29-cell,
first-sequence, direct-begin EARLY record with no participating memory path or
publisher owner, an external IRQ tail, and the unchanged CPU/allowance binding.
It cannot enter the eleven runtime-path maxima. Actual C normal/UBSan/ASan
outputs feed the host checker; promotion, allowance loss, historical downgrade,
wrong CPU/owner/sequence and invented endpoint remain negative controls.
Original captured analyses stay immutable. Corrected host replay has its own
harness identity and does not turn either diagnostic into timing acceptance.

QEMU's MMIO trace records the device callback return before truncation to the
access width. The decoder applies the recorded 32-bit width to the low word
before combining it with the two agreeing high words. A dedicated synthetic
control covers a 64-bit callback return whose hidden high bits differ. This is
a host decoding correction; the guest's validated high/low/high read is unchanged.
