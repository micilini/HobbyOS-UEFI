# TMV1-CL-13-FIX6 — SELFTEST record framing

Status: `REJECTED_CL13_FIX6_SELFTEST_SUMMARY`

## Entry and continuity

Date: 2026-08-17. Branch: `feat/taskman`. Base:
`4e700a9b38653fb57ecb0f107a18f87508db0736`. FIX1 boundary, FIX2 anchor,
FIX3 transactional transport, FIX4 fixture257 and FIX5 async state/helpers
remain preserved. The protected source manifest excludes only
`kernel/src/core/selftest.c` and is identical at entry and rejection.

The original FIX5 boot had summary 116/0/0, AUTORUN PASS and SHELL_READY, with
zero frames sent and zero guest faults. Its exact matcher saw
`scheduler.async.completed_before_marker_contract` split by `[HP]`/`[XHCI]`.
The serial driver already locks each `serial_write_all`; the old SELFTEST
emitter lost atomicity by composing one record through multiple calls.

## Bounded record emission

`selftest.c` now uses a 256-byte, non-heap, non-VLA builder with bounded char,
text and uint64 append, newline/NUL finalization and explicit overflow state.
PASS/FAIL/SKIP results and the summary are fully constructed before one final
`selftest_record_emit`. Invalid records emit one bounded `EMIT_ERROR` line.
BEGIN and AUTORUN PASS/FAIL remain single-call strings. No serial-driver,
scheduler, xHCI or async runtime source changed.

`registry.selftest_records_fit` traverses the actual registry, builds PASS,
FAIL with a bounded detail plus two UINT64_MAX values, SKIP and maximum summary
records, checks newline/NUL and exercises the overflow guard. The autorun grew
from 116 to 117 cases. The independent j2/jN SELFTEST builds were identical;
stack-check remained PASS with the historical maximum frame 1984/2048.
Across the 117 registry names, the longest exercised FAIL record is 207 bytes
including its final newline; the maximum-value summary is 185 bytes. Both fit
the 256-byte buffer with a NUL terminator.

## Strict parser and shared gate

`parse-selftest-log.py` retains exact result/summary regexes, duplicate
detection, summary-versus-observed validation and AUTORUN consistency. It adds
`case_count`, `--require-clean-framing` and repeatable `--require-case`.
Historical repair remains available only without the clean option.

`harness-selftest.sh` extracts the first autorun segment, normalizing only the
serial CRLF terminator, produces log/JSON/Markdown artifacts and requires
PASS, zero FAIL/SKIP/severity failures, zero repairs, balanced case_count and
five core cases. All listed CL-13 runners source this helper; redundant exact
greps for the async and fixture cases were removed.

The parser host-unit passed clean input, independent foreign lines and the
historical one-repair case. It rejected a split record, summary mismatch,
duplicate case, missing required case and repair under clean framing.

## Focused results and new causal failure

Static, parser host-unit, UP/TCG 2/2, SMP4/TCG 3/3, SMP4/KVM 3/3 and the first
SMP8/TCG boot passed. Every successful boot reported 117 PASS, zero FAIL/SKIP,
the five required cases, clean framing, shell readiness, balanced framed
transport and clean task diagnostics.

The second SMP8/TCG autorun internally completed with:

```text
[SELFTEST][SUMMARY] pass=117 fail=0 skip=0 p0_fail=0 p1_fail=0 p2_fail=0
[SELFTEST][AUTORUN] PASS
[BOOT][SHELL_READY] PASS
```

The strict parser observed 112 results. Five contiguous atomic records were
not line records because a foreign writer had already left text on the line:

```text
[XHCI] Disabling slot 1... [SELFTEST][PASS] registry.selftest_records_fit severity=P0
(Hardware disable OK) [SELFTEST][PASS] transport.sequence.initial severity=P0
[XHCI-DBG] CMD Ring: enq_idx=5[SELFTEST][PASS] transport.replay.same_payload severity=P0
[XHCI-DBG] EVT Ring: deq_idx=13[SELFTEST][PASS] transport.replay.payload_mismatch severity=P0
[XHCI-DBG] STS=0x[SELFTEST][PASS] transport.recursive_reject severity=P0
```

There was no byte inserted inside those SELFTEST records, no `EMIT_ERROR`,
duplicate, guest failure, panic, exception or frame submission. Nevertheless,
the required summary/case balance failed. Per FIX6 classification rules this
is `REJECTED_CL13_FIX6_SELFTEST_SUMMARY`, not a scheduler or async defect.
The boot was not retried.

## Unreached gates

The remaining SMP8/TCG boots, all SMP8/KVM hotplug boots, successful framing
classification, FIX5 async rerun/cpu-pin/entry-window/negative, consolidated
SMP4+ matrix, quantum variants, complete negatives, both 180-second soaks and
final release build were not executed. No commit or push was created and
TMV1-CL-14 was not started.

Operational estimate remains non-gating: best case a subsequent causal fix
closes CL-13; likely FIX6 or one further focused correction precedes closure.
CPU-pin/entry-window, full negatives and both soaks remain uncertain, so no
exact completion count is promised.

## FIX7 causal continuation

FIX7 reclassified the immediate cause as
`REJECTED_CL13_SELFTEST_LINE_BOUNDARY`. The five missing results were complete
and atomic per call, but began after partial xHCI prefixes because FIX6 did
not fence the beginning of a physical line. The bounded builder, registry
logic, strict summary validation and one-call result/summary guarantees from
FIX6 remain technically approved. See
`TMV1-CL-13-FIX7-selftest-line-boundary.md` for the line-fence correction and
resumed evidence.
