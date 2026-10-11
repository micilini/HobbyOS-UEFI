# Immutable TLB proof capture and deferred export

The current method is `immutable-tlb-proof-v2`. It changes SELFTEST transport,
not the TLB operation, safety clock, allocator, IRQ primitive or budget. The
incoming source is tree `e460e849e806d5417359c564d28d8f113a255332`, on approved
commit `d68a3b4a2c97f55dfdcc445bfe42317e09e1c55e`. Existing immediate captures
keep their bytes, schema 7 functional/local-cycle interpretation and verdicts.

`tlb_fixture.c:record` formats the original text and CRC before `emit_line`.
The current explicit session copies whole records into one owned byte buffer
under the existing trace lock. Length, count and incremental FNV digest commit
after the bounded copy. Neither a device output nor allocation occurs in the
deferred branch. This lock and all copying during work remain visible to the
unchanged outer IRQ recorder. There is no subtraction of observer cost.
The emitter explicitly enters the existing shootdown scope before locking, so
standalone proof copies contribute to that path's mandatory maximum as well as
copies nested in an existing memory interval. The actual emitter/controller
test retains injected wait and copy delays in both modes, including IF=0 entry.

The immediate diagnostic control uses the same buffer, setup and capture,
and additionally performs the original synchronous byte output. Thus the
deliberate variable is output timing; its extra I/O remains measured. The
unconfigured standalone fixture retains its original immediate receipt.
Current latency plans require explicit preparation and a source-bound policy.

## Capacity and lifetime

Preparation is an IF-enabled command before calibration/activation. It performs
a read-only image inventory, allocates and touches the buffer, and retains it
until VM termination. It is present in baseline memory accounting and remains
allocated across the genuine 33-MiB stress expansion. Both comparison modes
do exactly this setup. No private allocation is released or reused at export.

For N actual CPUs and I declared fixture invocations (1..3), there are
257 cycles on one CPU or 385 on SMP; each emits `5+8*N` records. Cleanup for
each of one/two windows emits `2+2*N` records. Two/three image scans each emit
at most `rights_bound+STACK_OWNER_COUNT+1` records. Twelve additional records
cover BEGIN, END, TABLES, AUDIT, DEFERRED, three OBSERVERs, two INITIATORs and
two WORKER_REAPs. The complete spool additionally admits all 8,192 service
audit slots exactly once across invocations. Every record is at most 766 bytes
including its newline; a formatting failure is an error.

`rights_bound` is the setup inventory's run count plus all 1,665 stack owners,
twice the 512 contiguous device views (including withdrawal splits), and all
130 scratch pages individually. Image/direct-map permissions are unchanged.
Every later scan enforces this admitted bound; unexpected geometry fails the
run rather than silently dropping records. The host independently repeats the
capacity arithmetic using actual CPU count and invocation count. This is not
a universal four-MB constant or a CPU-count reduction.

The fixture owner prevents sealing during an invocation; all declared
invocations must have completed without quarantine. The trace lock serializes
all byte writers. No automatic buffer or worker pointer survives a record
call. Overflow is sticky, publishes no partial record, and prohibits sealing
and acceptance. There is no wrap, fallback output, background consumer or
NMI/fault reader. Panic diagnostics do not acquire spool ownership.

## Cutoffs and reconstruction

The order is support preparation, calibration/activation, all ordinary work,
stress (including the full concurrent TLB fixture and reaps), finish (work
cutoff, terminal snapshots, postcalibration), final service audit, and export.
Only already captured test bytes move after these cutoffs. Export requires
eligible IF-enabled thread context, no fixture writer, and the exact completed
service generation from that audit. It seals/claims briefly under the trace
lock, restores flags, then performs OUTB without that lock or an IRQ mask.
It verifies the unchanged admitted/completed service boundary again afterward.
No workload, page-table scan, cache operation, worker cleanup or request is
performed by export. An IF=0 caller is rejected before any output.

PREPARED, per-invocation CAPTURED, SEALED and EXPORTED are distinct receipts.
The host binds them through actual framed keyboard commands, candidate/VM and
session nonce; independently checks capacity, every record CRC, invocation
order, exact bytes/count/digest, full TLB semantics and terminal service
continuity. A handler success or received prefix alone never completes proof.
The Enter-release deadline remains five seconds; bulk handler completion has
its separate transport timeout. Export interruption or duplication fails.

## Mechanism and limits

The independently read [QEMU v8.2.2 debug-console callback](https://raw.githubusercontent.com/qemu/qemu/v8.2.2/hw/char/debugcon.c)
passes individual bytes to a blocking character-write function. Its distribution
build need not be byte-identical to that upstream source. Removing millions of
these test output operations from the measured workload tests a demonstrated
dependency; it does not assign every historic HPET/return spike to logging.
[KVM timekeeping documentation](https://docs.kernel.org/virt/kvm/x86/timekeeping.html)
also distinguishes guest IRQ masking from host execution scheduling. No host
time is subtracted. The two new conditions are diagnostic, not retrospective
timing successes. All original frequency, observer, maximum, coverage and
strict whole-window acceptance predicates remain mandatory.

The bounded activation line declares `proof_output=2`; the prepared/sealed/exported records and source-bound plan name `immutable-tlb-proof-v2`. Unknown values are rejected. The actual formatter is exercised at the maximum CPU count and longest refusal status. A refused activation record now fails the command.

## Export receipt successor

Version one correctly moved the bulk bytes but still emitted SEALED/EXPORTED
through serial_write_all, which internally masks IRQ while holding the UART
lock. Its hosted serial substitute only checked caller flags. The permanent
regression now rejects any serial call from export. Historical version-one
bytes and timing results remain unchanged; they do not satisfy version two.

Version two writes both bounded receipt lines through the same unlocked
SELFTEST debug console as the immutable bytes. No serial driver or global
logging policy changes. The collector captures the exact debug-console prefix
after terminal audit and before export input, with command/VM/candidate and
monotonic identity. Replay verifies that prefix, exactly one seal and complete
export tail, and raw TLB bytes between them for deferred mode (before the seal
for the immediate control). Other proof families remain separately decoded
before this export. The original per-record CRC/FNV and workload semantics are
unchanged. Receipt bytes are counted separately from captured TLB data.
