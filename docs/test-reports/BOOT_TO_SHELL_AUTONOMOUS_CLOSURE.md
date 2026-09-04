# Boot-to-Shell Autonomous Closure

Status: `READY_FOR_BARE_METAL_LAPIC_CLOCKEVENT_VALIDATION`.

Bare metal was not executed. No USB media was written and no push was made.
The next owner is William.

## Frozen entry

- Branch: `feat/taskman`
- Required base: `523f367c9cd8652e709c0ffc7af28f6433d1213c`
- Required base tree: `cc04290b8cfcfcc16de06e65b1a8145f6760d646`
- Entry kernel: `4fe39fea792efd0cc64b05a612cb8850487733428dfbcdc195db2b4559c08fe1`
- SMP24 boot 1 serial: `341d2c114a2cf06d23f026e4eadf71b4111cb72dd003a1d32e4d1929c95352ef`
- SMP24 boot 2 serial: `86a34e7b7d820144a1f8e7d63de6852a6770a0b0842554a133aa5e17cf5c62d4`

Both frozen serial hashes were rechecked after the campaign and remained
unchanged.

## Initial reclassification

The second SMP24 run reached `24/24` CPU readiness, completed PCI/xHCI and had
no guest fault. Its last marker was `[GRAPHICS][SPLASH] BEGIN`. Its correct
entry classification is therefore
`REJECTED_BOOT_TO_SHELL_LIVENESS_UNCLASSIFIED`, not
`REJECTED_IRQ_RUNTIME_READY`.

## Autonomous cycles

Four repair cycles were consumed; the fifth was not needed.

| Cycle | Classification/result | Causal finding and action |
|---:|---|---|
| 1 | `REJECTED_BOOT_TO_SHELL_LIVENESS_SPLASH_BUDGET` | Bounded guest time still allowed one decorative frame to consume excessive host time under 24-on-12 oversubscription. Added cadence-aware fail-open before rendering. |
| 2 | `REJECTED_HOST_ORACLE_FALSE_SPLASH_TIMEOUT` | The guest reached Main Loop, but the host retained the absolute splash deadline after the terminal marker. Disarmed it at `SPLASH_TERMINAL`. |
| 3 | `REJECTED_SHELL_INPUT_TRANSPORT_TIMEOUT` | Boot, Main Loop and `irq check` passed; the focused runner then sent an out-of-contract `basic_commands` sequence. Restricted SMP24 focused validation to `irq check` and `taskdiag`. |
| 4 | `ACCEPTED_BOOT_TO_SHELL_LIVENESS` / `PASS` | Same kernel completed 24/24, splash, shell, runtime ready, Main Loop, IRQ and taskdiag. Splash host duration was 3,096 ms. |

The complete immutable ledger is in
`artifacts/build/boot-to-shell-autonomous/cycles.tsv` and `cycles.json`.
Rejected cycle evidence remains in its original per-cycle directories.

## Progress oracle and stall capture

The QEMU harness now uses stage-specific renewable idle budgets and a 900 s
hard cap for oversubscribed SMP24. It timestamps UEFI, kernel entry, CPU
preparation/release/readiness, PCI/xHCI progress, core, graphics, splash,
terminal, shell, runtime-ready and Main Loop markers. A PCI path with no MCFG
is accepted only when it publishes typed `status=MCFG_MISSING` completion; a
real scan still requires intermediate progress.

On timeout the harness captures HMP status/registers, screendump, serial and
trace tails, stage/elapsed metadata and a clockevent snapshot when shell is
available. Cycle 2 includes a valid 12,582,929-byte PPM capture. The capture
inventory and hashes are embedded in each `stall.json` and `stall.md`.

## Splash closure

The splash is typed, decorative, best-effort and fail-open. It validates the
logo and framebuffer geometry with overflow checks, uses monotonic time plus
BSP clockevent progress, skips late frames instead of replaying backlog, and
has bounded clock-stall/render/iteration/absolute budgets. It no longer calls
`hpet_usleep()`.

Every path converges on one cleanup block that restores console rendering,
repairs the framebuffer presentation and emits `RESUME` plus typed
`COMPLETE`. The null-logo path no longer leaves the console suspended. Eight
model selftests emit `[GRAPHICS][SPLASH_SELFTEST] PASS` during every certified
boot. All final runs completed normally; the abort path was separately
observed during autonomous repair.

## Final QEMU results

Final kernel SHA-256:
`f45d2c0bf00de08c37303e88433d881acc0d6e2ac395d34e331ce72671fa91fe`.

| Gate | Result |
|---|---|
| Focused SMP4 | PASS; timer-order 3, timer-cancel 3 |
| Focused SMP24 | PASS; 24/24, splash, shell, runtime ready, Main Loop, IRQ, taskdiag |
| Three independent SMP24 boots | PASS; splash host durations 2,718 / 3,080 / 3,117 ms; no retry |
| Quick matrix | PASS: q35 TCG 1/2, q35 KVM 4/8/24, pc TCG 4 |
| Runtime SMP24 | PASS: TASKMAN, smpstress/kill sweep, synctest, input, modal, IRQ, taskdiag |
| Timer fairness SMP24 | PASS; timer-order 5, timer-cancel 5, late 0, claimed residual 0 |
| Negative claimed-bypass | PASS; `old_position=2 new_position=1` detected only in the negative build |
| LAPIC rate SMP24 | raw FAIL 497, correctly non-authoritative on host 12 / guest 24 |
| LAPIC rate SMP4 | authoritative PASS 3/3: 997, 996, 995; threshold 700 |
| Soak SMP24 | PASS; 979,860 ms; IRQ 19, taskdiag 19, fairness 5/5, TASKMAN 4, stress/kill 4 |

The soak ended with 24/24 ready, console suspension 0, claimed residual 0,
late callbacks 0, non-BSP global clockevent ticks 0, early ticks 0,
clockevent regressions 0, HPET stray IRQs 0 and faults 0.

## Final build

The mandated clean sequence passed:

1. `make clean`; `make stack-check`
2. `make clean`; `make kernel-check JOBS=2`
3. `make clean`; `make kernel-check JOBS=12`
4. byte comparison, `make deps-check`, and `nm -u kernel.elf`

Maximum automatic frame was 1,968 bytes against the 2,048-byte limit. j2 and
j12 produced the same final kernel hash. Undefined symbols and warnings owned
by changed kernel files were both zero. `make production-image` and its
production-test-policy also passed while reproducing that same kernel hash.

## Continuity

The four-file timer-fairness manifest before/after build is byte-identical.
The 58-file protected-subsystem manifest before/after build is also
byte-identical. Scheduler/switch, LAPIC/IOAPIC, MADT/topology, timer fairness,
TASKMAN, modal/input architecture, bootloader and shared sources were not
changed by the boot-to-shell repair.

The negative build was restored to the same normal kernel hash. Its recreated
FAT container received a different raw hash, as expected for metadata, and
continuity was proven by extracting and comparing kernel, BOOTX64, font, logo
and startup payloads.

## Offline authority

`python3 scripts/verify-timer-clockevent.py autonomous-closure` independently
validated the frozen entry hashes, ledger, exactly three SMP24 final boots,
six-run matrix, runtime/fairness/rate evidence, soak, deterministic build and
continuity manifests. It emitted `[BOOT][AUTONOMOUS_EVIDENCE] PASS`.

Packaging is performed only after the single final commit. The candidate is
never opened in QEMU; its `candidate.txt`, payload manifest and SHA256SUMS bind
the post-commit production image to this report.
