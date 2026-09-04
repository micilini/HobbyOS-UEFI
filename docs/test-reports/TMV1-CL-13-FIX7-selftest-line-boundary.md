# TMV1-CL-13-FIX7 — SELFTEST line boundary

Status: `REJECTED_CL13_MATRIX` after focused FIX7 approval.

## Entry and causal failure

Date: 2026-08-17. Branch: `feat/taskman`. Base:
`4e700a9b38653fb57ecb0f107a18f87508db0736`. FIX1 boundary, FIX2 anchor,
FIX3 transport, FIX4 fixture257, FIX5 async state and FIX6 bounded record
emission remain preserved. The protected manifest excludes only
`kernel/src/core/selftest.c`.

The failed SMP8/TCG boot declared 117 PASS, zero FAIL/SKIP, AUTORUN PASS and
SHELL_READY, with zero frames and guest faults. The parser observed 112 cases.
Five complete records followed still-open xHCI prefixes (`Disabling slot`,
`Hardware disable OK`, `CMD Ring`, `EVT Ring` and `STS`). Atomicity per serial
call prevented insertion inside a record, but did not place its prefix at
column zero. The causal classification is
`REJECTED_CL13_SELFTEST_LINE_BOUNDARY`, not a scheduler or async defect.

## Line-fenced protocol and parser

The bounded 256-byte builder now begins results and summaries with a newline
and ends them with a newline. AUTORUN BEGIN/PASS/FAIL and EMIT_ERROR use the
same `newline + record + newline` contract. Each fence and record is emitted
by one `serial_write_all`; there is no isolated fence call. Record validation
checks the leading newline, full SELFTEST prefix, trailing newline, NUL and
overflow state. The maximum exercised result grows from 207 to 208 bytes and
the maximum summary from 185 to 186, both below 256. No serial or xHCI source
changed.

The strict parser now raises `prefixed SELFTEST record` when a marker occurs
after column zero, never salvages the substring, and emits
`line_boundary_violations=0` for valid JSON. Clean framing and the shared boot
helper require both repairs and boundary violations to be zero.

## Host units and focused matrix

The host units reject a prefixed record, accept a fenced record surrounded by
foreign text, reject the five-pattern FIX6 reproduction without fences and
accept all five cases with fences. Static stack, j2/jN reproducibility, image
and undefined-symbol gates passed.

The real focused matrix passed UP/TCG 2/2, SMP4/TCG 3/3, SMP4/KVM 3/3,
SMP8/TCG 5/5 and SMP8/KVM 5/5. Every boot reported 117 PASS, zero FAIL/SKIP,
zero repairs, zero boundary violations, all five required core cases,
SHELL_READY, clean diagnostics and balanced framed transport. Concurrent
xHCI/hotplug noise was observed in two SMP8/TCG and three SMP8/KVM boots.
The protected before/after-focused manifests are identical. Classification:

```text
OPEN_FOREIGN_LINE_PREFIX_CONFIRMED
```

## Resumed async focused

The previously incomplete SMP8/KVM yield boot passed with an exact run ID and
END_BEFORE_COMPLETE. CPU-pin and entry-window passed SMP4/TCG 3/3,
SMP4/KVM 3/3, SMP8/TCG 1/1 and SMP8/KVM 1/1. The isolated stale-slot negative
was detected, followed by a normal rebuild and positive CPU-pin canary. There
were no async timeouts, run mismatches, stale completions or guest faults.
Classification:

```text
SYNC_WRAPPER_FALSE_REJECTION_CONFIRMED
```

## Consolidated continuation and new failure

The consolidated static and UP/TCG autorun gates passed with 117 cases and
clean line framing. UP/TCG, SMP2/TCG and SMP2/KVM matrix evidence was reused by
validated artifact hash. The new SMP4/TCG boot passed its autorun, subsystem
checks and the exact-run asynchronous yield that had originally exposed FIX5.

The next command, `synctest sem 20000`, emitted FRAME ACCEPT, BEGIN, START,
created all workers and later emitted its functional PASS. While the handler
returned, task-creation output split the control record into:

```text
[HARNESS][END] seq=18 status=[TASK][CREATE] id=0
23 name=synctest-sem-wait ...
```

Consequently no machine-parseable `END seq=18 status=0` existed. The framed
harness correctly did not resend after BEGIN and timed out as
`GUEST_COMMAND_STALL`. There was no guest FAIL, panic or exception, and the
QEMU process was stopped. This is a new matrix/control-record failure outside
FIX7's sole authorized runtime file, so certification stops fail-fast as
`REJECTED_CL13_MATRIX` without modifying the framed protocol.

SMP4/KVM, SMP8/TCG, SMP8/KVM, quantum variants, the complete causal negatives,
both soaks and the final release build were not executed. The protected
before/after-tests manifests remain identical. No commit or push was created,
and TMV1-CL-14 was not started.

Operational estimate was non-gating: the remaining matrix exposed one further
causal control-record boundary issue before negatives and soaks; no exact
number of further corrections is promised.

FIX8 continues from that exact failure in
`TMV1-CL-13-FIX8-runtime-ready-harness-records.md`. It preserves the FIX7
SELFTEST line fence while moving full autorun behind explicit runtime/test
readiness and applying the same bounded, single-call line contract to HARNESS
control records.
