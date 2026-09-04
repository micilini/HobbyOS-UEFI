# Diagnostic result output certification

Status: PASS -- ready for bare-metal diagnostic-result validation.

## Scope and cause

The interactive shell invoked diagnostic handlers and discarded the returned
status. Several handlers kept their detailed result exclusively on serial, so
returning to the prompt proved liveness but did not distinguish success from
failure. `synctest timer-cancel` is asynchronous, so its successful start also
could not be treated as a completed PASS.

This change adds a bounded framebuffer mirror without changing any diagnostic
criterion. Synchronous wrappers print the result already returned by the
handler. Asynchronous `synctest` workloads publish RUNNING and replace it with
their existing terminal result. The serial records remain the detailed,
machine-readable authority.

Branch and frozen base:

```text
branch: feat/taskman
base: 380a8c9fe796a7e3714162a14740701a13800652
base tree: dd5e013360ab8811a36480ddbdcf1439c4c76473
planned commit: fix(shell): show diagnostic results on the framebuffer
push: prohibited / not performed
```

## Presentation contract

- Sync success: `[PASS] <canonical command>` followed by newline.
- Sync failure: `[FAIL] <canonical command> - details on serial` followed by
  newline.
- Async start: `[RUNNING] <canonical command>` in the status overlay.
- Async terminal result replaces RUNNING with PASS or FAIL.
- The status overlay is fixed-size, non-heap, non-ANSI, does not enter console
  history, and preserves shell input and cursor state.
- The first subsequent input clears the visible status without invalidating a
  still-running generation.
- Modal entry restores any visible overlay and defers later publication;
  resume flushes only the newest generation.
- A terminal result cannot be replaced by stale RUNNING data.
- `diagnostic_result_complete()` returns the input status exactly, including
  the tested values 0, 1, and 64.

Covered commands:

```text
accounttest check
taskmantest check
inputtest check
modaltest check
synctest check
synctest timer-order
synctest timer-backlog
synctest timer-cancel
reaptest check
reaptest timer-ref
killtest check
killtest smpstress-sweep
schedtest check
```

All existing asynchronous `synctest` kinds use the same generation-safe
RUNNING-to-terminal path. `irq check` and `taskdiag check` retain their own
framebuffer output and are deliberately not wrapped.

## Static and SELFTEST gates

`scripts/test-diagnostic-results.sh all` passed. Static checks proved:

- no forbidden historical label in the new permanent names;
- no heap allocation in the new result/status infrastructure;
- handler return identity is preserved;
- `schedtest check` returns the scheduler validation result rather than zero;
- `timer-cancel` does not publish PASS at start;
- async workers publish through shell status, not direct console writes;
- extracted `serial_write_all(...)` expressions in all eight audited handler
  files are unchanged from the frozen base.

The explicit `diagnosticresulttest all` run passed every required marker:

```text
[SHELLRESULTTEST][SYNC_PASS] PASS
[SHELLRESULTTEST][SYNC_FAIL] PASS
[SHELLRESULTTEST][ASYNC_RUNNING] PASS
[SHELLRESULTTEST][ASYNC_PASS] PASS
[SHELLRESULTTEST][ASYNC_FAIL] PASS
[SHELLRESULTTEST][MODAL_DEFER] PASS
[SHELLRESULTTEST][PROMPT_PRESERVE] PASS
[SHELLRESULTTEST][BOUNDED] PASS
[SHELLRESULTTEST][ALL] PASS
```

The test reads the actual console history and status destination. Its visible
FAIL is synthetic and is counted as a successful negative-path assertion.

## Focused QEMU

Both boots used the same frozen SELFTEST payload:

```text
kernel SHA-256: f2f8bb7c77b3bc859b6838ec9efa08a5c9ff3678af9b7931a9e78ad6e82a99f0
input image SHA-256: 001e2edc8b235318c92e8a0d68c357196aa13cb3a830c85343fca442d1072d4c
BOOTX64.EFI SHA-256: 3f0665e6a85825b4821d36f37c2f3d9f7e1f63c37b6fac726158ee66d7133b66
machine: q35
accelerator: KVM
```

SMP4 used one boot and passed `accounttest check`, `taskmantest check`,
`inputtest check`, `modaltest check`, `synctest check`, `timer-order`,
`timer-backlog`, real asynchronous `timer-cancel`, `reaptest timer-ref`,
`reaptest check`, `killtest check`, and corrected `schedtest check`.
Two bounded `synctest sleep` runs supplied an observable RUNNING window and a
runtime prompt-safety probe: `echo abc` remained byte-for-byte in the input
buffer while the terminal PASS overlay arrived.

SMP24 used one boot and reached:

```text
[IRQ][CPU_READY_SUMMARY] expected=24 ready=24 failed=0 waiting=0
[BOOT][RUNTIME_READY] PASS cpus=24/24
```

The synchronous set passed again. A SELFTEST-only bounded arm worker waited
for the one TASKMAN session to enter modal state and then invoked the real
`synctest timer-cancel`. Its serial PASS arrived while the modal session was
active. After ESC, the framebuffer showed the deferred
`[PASS] synctest timer-cancel`. Post-modal checks reported zero TASKMAN clipped
writes and zero scroll delta, inactive/clean modal state, clean input routing,
`irq check` PASS, `taskdiag check` PASS, `synctest check` PASS,
`reaptest check` PASS, and `killtest check` PASS.

Campaign limits actually used:

```text
normal QEMU boots: 2/2
SMP configurations: 4 and 24
TASKMAN sessions: 1/1
smpstress runs: 0
long soak: 0
historical regressions: 0
negative runtime builds: 0
screendumps: 4/4
```

All four PPMs are 2048x2048, non-uniform, and hashed. Their scenario metadata
is in `artifacts/build/diagnostic-results/screendumps.tsv`.

## Fault and continuity gates

No real occurrence of PANIC, FATAL, #PF, #GP, DOUBLE FAULT, TRIPLE FAULT,
STRUCTURAL_FAULT, FINISH_FAULT, deadlock, lost wake, prompt corruption, modal
overwrite, generation regression, or buffer overflow was found.

Protected manifests before edits, after runtime tests, and after final build
are byte-identical. Each manifest has SHA-256:

```text
48b1ee97ee58e3d33337f5902bf97a8b512a731528f08bf4ea5ab70831a917e8
```

This covers scheduler/switch, task lifecycle, timers/clock, HPET, LAPIC,
IOAPIC, MADT/SMP/IRQ bootstrap, TASKMAN renderer/model/view, task formatter,
PCI/xHCI, bootloader, and shared ABI. No protected source changed.

## Final build

```text
stack-check: PASS
maximum frame: 1968 bytes (limit 2048)
kernel-check JOBS=2: PASS
kernel-check JOBS=12: PASS
j2/j12 cmp: 0 (byte-identical)
kernel SHA-256: 67a71b188206bc660516b3b434867124b2d1aeaac0c3e0ebd3ee3874b9944107
deps-check: PASS
nm -u kernel.elf: empty
new/owned warnings: 0
```

The only warning in an edited source path is the pre-existing unused
`shell_delete` function, present in the frozen base. Production generation
passed with `SELFTEST=0`, `SELFTEST_AUTORUN=0`, five byte-identical FAT
payloads, a clean read-only FAT check, and:

```text
[BOOT][PRODUCTION_TEST_POLICY] PASS autorun=0 tasktest=0 manual_diagnostics=1 breadcrumbs=5
production image SHA-256: 9be4c6f0de6d26e145f402c9998603622c33b918d80baac8e459e3b44808fba3
BOOTX64.EFI SHA-256: 3f0665e6a85825b4821d36f37c2f3d9f7e1f63c37b6fac726158ee66d7133b66
```

The production image was generated after the final QEMU boot and was not
opened in a VM.

## Evidence and handoff

Primary evidence lives under:

```text
artifacts/build/diagnostic-results/
```

The candidate and source-plus-`.git` review ZIP are produced only after the
single local commit, so their commit/tree identifiers and hashes live in those
package manifests. Bare metal was not executed by CODEX. The next owner is
William.
