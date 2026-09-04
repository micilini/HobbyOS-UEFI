# TASKMAN V1 maintainer handoff checklist

Status: READY

- [ ] Install the documented x86_64 GCC/binutils, GNU-EFI, mtools, QEMU, and
  OVMF dependencies; verify `/dev/kvm` access when KVM is desired.
- [ ] Run `make deps-check`, `make kernel-check`, and `make image`.
- [ ] Distinguish release (`SELFTEST=0`) from test builds. Test commands,
  framed execution, and autorun are test-only.
- [ ] Wait for `SHELL_READY`, `RUNTIME_READY`, then `TEST_READY`; inspect
  serial, debugcon, and trace before classifying a timeout.
- [ ] Exercise `ps`, `kill`, `taskman`, and `taskdiag` using the
  [command reference](taskman-v1-command-reference.md).
- [ ] Read the [contract](taskman-v1-contract.md),
  [architecture](taskman-v1-architecture.md),
  [visual contract](taskman-v1-visual-contract.md), and
  [known limits](taskman-v1-known-limitations.md).
- [ ] Run `scripts/test-taskman-visual.sh naming` before accepting active
  interface changes.
- [ ] Use `scripts/test-taskman-visual.sh focused` for the bounded visual smoke;
  do not replace impact-based testing with an automatic long campaign.
- [ ] Inspect `docs/test-reports/TASKMAN_VISUAL_HUMAN_CHECKLIST.md` and leave it
  pending until a person reviews WIDE, COMPACT, selection, pagination, flicker,
  and ESC in QEMU.
- [ ] Keep frozen release bytes and handoff packages unchanged. New evidence
  belongs under `artifacts/build/taskman-visual-final/`.
- [ ] For a failure, preserve logs, identify the impact cone, compare hashes,
  and classify build, transport, shell, guest fault, or structured command
  failure before choosing a test.
- [ ] Treat bare-metal acceptance and future product work as separately
  authorized campaigns.

## Bare-metal acceptance

The current release does not claim bare-metal execution. A separately approved
campaign should capture serial from power-on, confirm all CPUs and runtime
readiness, run diagnostics, exercise PS and TASKMAN, exit with ESC, and add
stress progressively.

## Failure discipline

Do not accept PASS merely because FAIL is absent. Use exact handler status and
machine-readable records. Runtime changes invalidate source continuity;
nomenclature and documentation changes require candidate hash equality.
