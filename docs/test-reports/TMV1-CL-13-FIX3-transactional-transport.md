# TMV1-CL-13-FIX3 — transactional HMP transport

Date: 2026-08-17
Branch: `feat/taskman`
Base: `4e700a9b38653fb57ecb0f107a18f87508db0736`
Status: `REJECTED_CL13_MATRIX`

## 1. Preserved worktree and continuity

The legitimate uncommitted CL-13, FIX1 and FIX2 implementation was present
with an empty index. FIX1's target-specific semaphore matrix and FIX2's causal
anchor proof remain accepted evidence. The protected source manifest is
`artifacts/build/cl13fix3-protected-runtime-before.sha256`; it covers kernel,
bootloader, shared and makefile while excluding only the five authorized
shell/selftest transport files.

## 2. Original transport failure

The SMP=4/TCG FIX2 variant boot completed counts 1, 20, 50 and 129, including
exact cleanup and `taskdiag check`. The next raw HMP line,
`taskmantest setup 257`, produced no serial line. QEMU and its HMP socket stayed
alive, no panic/#PF/#GP/FATAL or guest FAIL appeared, and a later
`inputtest marker transport1` arrived. Serial does not expose an unknown shell
command, so the old artifact cannot prove whether a partial line existed; it
does prove that the target handler never emitted its setup sentinel.

## 3. Failure boundary

The raw path is host HMP `sendkey`, virtual USB keyboard, xHCI/DPC, keyboard
FIFO, input router and shell line buffer. The old protocol had no frame ID,
checksum, dispatch acknowledgement, completion acknowledgement or replay
rule. Retrying a non-idempotent setup command was therefore unsafe.

## 4. Guest frame protocol

SELFTEST builds accept:

```text
tasktest exec <sequence> <crc32hex> <canonical payload>
```

Payloads are 1..96 ASCII bytes, use one space between arguments and fit with
the envelope below the 256-byte shell buffer. CRC is reflected
CRC-32/ISO-HDLC (`0xEDB88320`, initial/final XOR `0xffffffff`); the guest pure
selftest includes `123456789 = cbf43926`. CRC, syntax, payload, recursion and
sequence are rejected before dispatch.

An accepted new sequence emits `FRAME ACCEPT`, then `BEGIN` immediately before
the internal test-only shell dispatch and `END status=N` after the handler
returns. Only then are last sequence/payload/status and next sequence
committed. Repeating that exact completed tuple emits `REPLAY` and returns the
stored status without another dispatch. Stale, gap and mismatched replays are
rejected.

## 5. Shell dispatcher boundary

The regular shell still parses the same way, prints unknown commands to the
console and ignores handler status. Under `HOBBYOS_SELFTEST`,
`shell_execute_command_line_for_selftest()` calls the same dispatcher, returns
the handler status, suppresses console-only unknown UX and does not touch
history, the interactive buffer or the prompt. Recursive `tasktest exec`
payloads are forbidden.

## 6. Host protocol

`scripts/harness-framed.sh` resets host sequence when `.qemu/qemu.pid`
changes, canonicalizes and CRCs each payload with Python `zlib`, and permits at
most three attempts. A later attempt first submits three empty Enters and
waits one second, clearing a partial line without backspace floods. Retry is
allowed only while no `BEGIN`, `END` or valid `REPLAY` has been observed.
`BEGIN` without `END` is `GUEST_COMMAND_STALL`; transport absence, persistent
corruption, unexpected status and missing payload contract have distinct
classifications. Modal navigation keys remain raw.

## 7. Pure transport suite

The `transport` suite registers nine cases: known CRC, canonical join, initial
sequence, gap and stale rejection, exact replay, mismatched replay rejection,
recursive rejection and maximum frame bound. `tasktest transport-status`
reports accepted/completed/replay and reject counters without the stored
payload.

## 8. Focused validation

The focused static gate passed: SELFTEST stack-check, reproducible j2/jN
kernel, autorun image, transport pure suite (9 PASS, zero failure) and an empty
undefined-symbol list. The formal transport boots then passed SMP=1/TCG 3/3,
SMP=2/TCG 3/3 and SMP=2/KVM 3/3. They covered CRC rejection and recovery,
partial-line recovery, recursion rejection, 100 marker frames per boot and
non-idempotent setup/replay/cleanup exactly once. Every completed boot had
monotonic sequences, `accepted == completed`, zero unexpected sequence reject,
zero guest fault and clean input/task diagnostics.

The first SMP=4/TCG boot passed counts 1, 20, 50 and 129 over framed commands.
For sequence 29, the deliberately corrupt CRC was rejected before dispatch;
the correct retry then emitted `FRAME ACCEPT`, `BEGIN` and `END status=1`.
There was no setup sentinel and no command stall. Read-only source inspection
identified the deterministic cause: the preserved CL-12 fixture declares
`FIXTURE_MAX 129u`, and `setup()` returns false before logging when the
requested count exceeds that bound. `cmd_taskmantest.c` is explicitly frozen
outside the FIX3 scope, so the required count-257 scenario cannot be made to
pass in this mission without prohibited scope expansion. The failed serial is
`artifacts/build/cl13fix3-smp4-tcg-run1-failed.log` with SHA-256
`2bf82580557f4ea150544b21abb1d2a4c591cda37fca41eb5dfd30bb88318dfb`.

The protected-runtime manifest remained byte-identical after the stopped
focused run. QEMU was stopped and no later focused boot was attempted.

## 9. Remaining CL-13 certification

Because the required focused SMP=4/TCG scenario failed explicitly, the runner
stopped fail-fast. SMP=4/KVM focused boots, resumed CL-13 matrix, quantum
variants, negatives, both soaks and final release build were not run. No
commit or push was created. Resolving the fixture-capacity/specification
mismatch requires separate authorization because the only direct production
test fixture file is outside FIX3's permitted scope. CL-14 remains reserved.

## 10. FIX4 continuation

TMV1-CL-13-FIX4 authorizes only the test fixture implementation needed to
resolve that deterministic limit. It keeps the FIX3 protocol frozen: the
original frame remains CRC-valid, accepted, begun and completed exactly once
with handler status 1, zero replay and zero guest fault. The FIX4 report is
`TMV1-CL-13-FIX4-fixture-257.md`; it expands the real fixture to 257 and
resumes certification without reopening the passed FIX1, FIX2 or FIX3 causal
contracts.
