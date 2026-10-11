# Physical serial latency diagnostic

For new declared reference runs, use `irqlatency reference` and explicitly
select `--schema-version 5` in the verifier. Schema 5 implements the owner's revised
optimization policy: 50 us remains a reported reference, while correctness,
transport, clock/observer validity and complete coverage still determine
whether a diagnostic is qualified. It never grants comparative or full
implementation acceptance. See [the policy and Linux comparison](linux-latency-comparison.md).
The schema-4 `irqlatency serial` instructions and strict historical verdicts
below remain available for exact replay. USB deployment is separate from
building and testing a new candidate in QEMU.

This is an owner-requested diagnostic image for the Intel Core i9-12900K,
ASUS PRIME-A Z690, 16 GiB DDR5-5200 and RTX 4070 system. It does not replace
the qualified virtual timing series or the outstanding functional/native gates.
No physical result has been obtained when this image is prepared.

Build with SELFTEST=1 DEBUG_ASSERT=1 IRQ_LATENCY=1 IRQ_ENTRY_DIAGNOSTIC=1,
IRQ_LATENCY_DETAILS=0 IRQ_LOCAL_TSC_ORDER=1 SERIAL_PROOF_DIAGNOSTIC=1 and a
nonzero SERIAL_PROOF_ID. The session candidate receipt binds this ID to the
exact source, flags, ELF, EFI and image hashes. The ordinary production build
compiles out the command, ownership state and pending serial buffer. Use the
canonical production-image target for production candidates, not this role.

On the receiving Linux machine, start picocom before booting HobbyOS:

```bash
sudo picocom --baud 115200 --flow n --parity n --databits 8 --stopbits 1 --logfile "$HOME/hobbyos-physical-$(date +%Y%m%d-%H%M%S).log" /dev/ttyUSB0
```

The boot banner initializes each discovered UART before GOP messages. Subsequent
text and hexadecimal fragments retain that configuration. Reinitializing each
fragment previously reset the transmit FIFO while its final byte could still be
queued. The actual boot writer/GOP formatter controls in
`scripts/test-boot-serial.py` reproduce this loss with a delayed UART model and
verify exact fragment concatenation after the correction. Hardware effects are
substituted; this does not prove the unique cause of an old corrupted capture.
The independent kernel UART initialization and measured workload are unchanged.
An EFI-only successor can retain the exact prior kernel ELF and its build tag;
its new candidate receipt must explicitly bind the new EFI/image and unchanged
kernel to their separate source roles. It cannot reuse the old candidate identity.

Use the actual serial receiver device if it differs. Keep picocom's input
mappings at their defaults; do not enable CR/LF rewriting or paste terminal
text into the evidence file. Capture from power-on, including CPU topology,
physical root and clocksource selection. The established serial card/cable
and motherboard settings remain unchanged. This procedure assumes the owner's
previously working serial connection, not an arbitrary USB port as a guest UART.

Boot the SanDisk via UEFI and type this once on the HobbyOS keyboard:

```text
irqlatency serial
```

Release Enter normally; the existing five-second input condition is unchanged.
Do not run other HobbyOS commands while this command runs. The command prepares
and touches its bounded proof spool, obtains the original calibration, executes
ordinary `mem` plus the unchanged stress/TLB/stack lifecycle work, freezes the
original complete measurements, performs the postcheck and terminal service
audit, and finally exports the sealed TLB records. A rejected postcheck still
exports its raw evidence when the workload/audit are complete.

The raw output ends with `[PHYSICAL_LATENCY][END]`. `status=CAPTURE_COMPLETE`
means the command/export completed, not that the latency bound passed. Version 4
also reports the frozen measurement's missing, invalid, unqualified and
over-budget cells, maximum and actual CPU/path. The command returns failure if
measurement is rejected/unavailable or a checked UART failed, even when the
functional workload and proof export completed. A
`finish=1`, missing footer, failed operation or interrupted capture is retained
as such. `timing_acceptance=NOT_GRANTED` is intentional. Exit picocom using
Ctrl-A followed by Ctrl-X after the footer and return of the HobbyOS prompt;
send the complete log file rather than a screen copy. A panic or failure log is
also useful and must not be discarded or described as a passing run.

At 115200 baud with 8N1, the physical wire carries at most 11520 bytes/s.
Calibration, service and TLB proof can contain many megabytes, particularly on
24 logical CPUs. A 20 MiB log alone requires at least 30.4 minutes of wire time;
measurement and software overhead add to this. Visible calibration lines may
therefore take time to finish. Bulk export time is distinct from the five-second
Enter-release requirement and is not an IRQ-off latency measurement.

## Transport and lifetime

`proof_output=3` identifies the current physical serial role. The TLB fixture
requires its admitted RAM spool and does not probe QEMU port 0x402 in this role.
The capture receipt says `channel=ram export=uart`. Every original TLB record,
CRC, count, stream digest, invocation and terminal service identity is retained.
No allocator, TLB strategy, deadline, clocksource, callback or observer primitive
is changed by this transport adaptation.

Checked diagnostic records and final sealed TLB export claim exclusive UART
ownership. Reservation/release hold the existing serial lock briefly; UART
polling and output execute with IF enabled without that lock. Ordinary serial
diagnostics arriving during ownership are copied into a bounded 64 KiB FIFO.
Consumed bytes are copied to a 128-byte local chunk before their slots are
reused. Each drain has a one-MiB progress bound; a full FIFO, exhausted drain,
invalid context, panic or loss of all live UARTs fails export. A failed UART
does not rejoin midway. Panic bypasses ownership and invalidates the run.
Neither a UART polling loop nor a pending-output drain is unbounded.

The pre-existing ordinary diagnostic output remains measured according to its
existing scope; it has not been moved behind a cutoff or removed. This role
therefore does not promise a small maximum. The HPET/reference/fallback provider
and actual discovered CPU count remain intact. Normal bare metal does not have
KVM pvclock, which is an explicit difference from the virtual provider runs.

## Reconstruction

Use the build ID in the candidate receipt, not an arbitrary value inferred from
a partial log:

```bash
python3 -B scripts/verify-physical-serial.py hobbyos-physical.log --build-id BUILD_ID --output physical-analysis.json
```

The separate current-role parser validates exact raw LF TLB bytes, individual
CRCs, capacity, counts, FNV digest, capture/export order, complete service ledger,
actual TLB A/B/ACK proof, stack lifetimes, local cycle units, original calibration
and maximum witnesses. It invokes the existing semantic decoders in an isolated
process and does not change the historical debugcon selector. Missing endpoints,
CPUs, rows, audit, tail or mismatched build identity cannot become a zero or PASS.
The original 50000-ns strict comparison and observer allowance remain unchanged.
The output is diagnostic reconstruction, never implementation acceptance.

Hosted tests substitute privileged UART/IRQ effects and prove byte preservation,
lock/IF context, bounded overflow, concurrent pending writers, panic/failure and
command chronology. They do not prove hardware serial delivery, invalidation,
cache effects or physical timing. A separate QEMU UART/HPET smoke checks the
actual image without debugcon; it is not a physical run or acceptance-series boot.

## Failed workload proof and checked records

Physical command version 2 uses `immutable-tlb-failure-proof-v1`. Its byte
capacity is the original geometry plus three bounded records per invocation:
one OUTCOME and up to two CYCLE_FAILURE records. The ordinary virtual proof
contract remains `immutable-tlb-proof-v2`.

A failed invocation keeps its mapping/frame quarantine. The physical role waits
for every successfully created initiator to finish and for its normal stack
retirement, including on failure, before permitting a seal. Missing ownership,
unfinished writers, spool overflow or an incomplete service ledger still prevent
export. Completed invocation count and successful invocation count are distinct.
OUTCOME binds the first failed fixture predicate, writer retirement, created
workers and quarantine; optional CYCLE_FAILURE binds the failed cycle phase and
request identity. It does not invent hardware invalidation or load success.

Calibration/maximum/audit records and physical command endpoints now use the
actual checked UART record writer. Each record claims exclusive export ownership
briefly, transmits with IF enabled outside the serial lock, drains the bounded
ordinary-writer queue, and reports any UART/queue failure. It retains the existing
UART polling bound. A failed byte stops the record instead of silently omitting
it and reporting successful output. Normal production serial behavior and panic
output are unchanged. Serial hardware or receiver loss remains detectable by
CRC, complete sequence/count and stream digest, not repaired by a parser.

An exact exported FAIL is `FAILED_WORKLOAD_PROOF_RECONSTRUCTED`, with a nonzero
verifier exit. It is not functional, timing or implementation acceptance.
Historical version 1 requires its original successful command and proof contract.
The current CLI defaults to version 4; `--schema-version 1`, `2` or `3` explicitly
selects an old build identity. No capture selects another schema merely by its
own label.

Run `python3 -B scripts/test-serial-proof.py --output <new-scratch-directory>` and
`python3 -B scripts/physical-serial-fixtures.py` for actual-function and synthetic
transport controls. UART/privileged substitutes do not prove physical timing.

Failed-writer draining and physical initiator retirement use checked safety-clock
reads and voluntary scheduling. Invalid/backward time, the existing absolute
deadline, or 1,024 consecutive identical reads reject progress; the latter is a
fail-closed stall guard, not an elapsed-time estimate. These waits never mask IRQs
or treat a cached clock floor as fresh time. The one-second request deadline is
unchanged.

## Boot coordination and reusable UART retention

Version 3 waits for boot diagnostic publication to finish before acquiring
checked UART ownership. This is a read-only completion flag, not the snapshot
routine that itself executes and prints task selftests. Waiting yields with
interrupts enabled and rejects invalid/backward/stalled clock reads or its
declared 30-second startup deadline. It does not change the one-second request
deadline. A failed boot remains visible as `boot_ready=0`; reaching a terminal
boot state is not a claim that the boot passed. Startup failures emit an
INCOMPLETE endpoint where UART delivery is possible.

Current candidates also require successful TEST_READY before preparing the
proof spool or starting calibration. A completed but failed boot ends promptly
with `stage=boot_not_ready`; it cannot spend a full run reaching the CPU-control
snapshot that already rejects that state. Diagnostic boot timeouts publish
READY_BLOCKERS and READY_USB from the final actual readiness snapshot. These
records explain a refusal and do not override any readiness predicate.

The previous physical capture ended after the opening marker and exactly
65,536 queued ordinary bytes. Actual-function controls reproduce the old
append-only queue exhausting even when earlier bytes have already left the
UART. The new FIFO reclaims consumed slots; real outstanding-capacity overflow
still fails. Boot coordination prevents the separate high-rate readiness
poller from flooding the opening record.

During the long TLB export, pending ordinary output is drained between complete
TLB records. It uses `[SERIAL_AUX]` hex frames bound to build, global sequence,
byte count and FNV digest. This preserves partial ordinary lines and arbitrary
bytes without interleaving them inside a CRC-protected proof record. Serial
transport version 2 declares `auxiliary=hex-fnv1a-v1`; physical command version 3
requires that transport. The decoder validates and removes only these exact
frames when reconstructing the immutable TLB stream. Original bytes remain
losslessly recoverable from the raw capture and the auxiliary decoder. No
unframed extra text, missing frame, wrong identity/order/digest, shortened proof
or mismatched final count becomes a successful transport result.

The physical END records auxiliary totals through the start of that endpoint.
Auxiliary frames produced while transmitting END are a separately observable
tail, not falsely included in the earlier total. Full TLB byte/count/digest,
worker/service chronology and all original timing checks remain required.

## Unsupported USB attachment correction

A successfully addressed device with a complete configuration descriptor but
no supported interface retains its existing slot and attachment ownership.
Root and hub-child enumeration previously returned failure in this case. The
root entered a retained-error state; a hub failed to publish its child and
repeated enumeration on subsequent polls. The physical capture shows this
repeated hub activity, but its missing early boot prefix cannot identify every
readiness failure.

The corrected parser validates descriptor boundaries before inspecting interface
and endpoint fields. An advertised boot keyboard without its usable endpoint,
malformed descriptor, failed transfer or stale attachment still fails. An
unsupported device remains addressed without endpoint configuration or a claimed
driver. Existing generation checks, disconnect retirement and DMA ownership
remain in force. The CPU-only descriptor copy is released after scalar parsing.
This is an intentional production change in the xHCI enumeration unit, separate
from the diagnostic-only serial and readiness records.

Run `python3 -B scripts/test-usb-enumeration.py --output <new-scratch-directory>`
for actual parser/root/hub functions with explicit device substitutes. The
`--source <incoming-xhci.c> --legacy-witness` role reproduces the old unsupported
device result; it is not a runtime USB test. Retain the ordinary device-ownership
regressions and a real hub-keyboard/unsupported-device virtual smoke.

## Checked UART lifetime and final measurement summary

The receiver's previous log contains a damaged bootloader line and a truncated
post-calibration record. The unchanged whole-log validator rejects it. Its
intact TLB spool and functional families remain separately verifiable; a good
spool does not repair a missing calibration endpoint.

An actual-source witness also demonstrates a distinct transmitter defect: a
UART that times out during one checked record was admitted again at the next
record, permitting a later footer after lost bytes. A checked failure now marks
that port dead until UART initialization, including for ordinary output. Other
working ports continue. The terminal `uart_failed_ports` mask reports failures
through the start of that endpoint; a failure during the endpoint also makes
the command fail. This witness does not establish that a timeout caused the
observed physical corruption. Receiver/cable loss remains detectable only by
the complete received CRC/count/digest/identity checks; zero transmitter errors
cannot prove successful reception.

The version-4 summary reads the existing immutable terminal snapshots with
interrupts enabled, after the real work, audit and proof export. It issues no
new request or hardware operation and changes no maximum or calibration rule.
The host independently compares every summary value with the complete raw
records. Unavailable or rejected measurement cannot become a generic command
PASS. `BELOW_BUDGET_OBSERVED` still describes one diagnostic, not campaign or
implementation acceptance. Historical versions retain their original behavior.
