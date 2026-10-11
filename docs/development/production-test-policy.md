# Production test policy

Production images do not run test suites automatically. The canonical build for
physical media is:

```bash
make production-image
```

This target cleans all compiled objects, rebuilds with `SELFTEST=0` and
`SELFTEST_AUTORUN=0`, creates `hobbyos.img`, and runs the production policy
verifier. It does not start QEMU.

## Supported profiles

| Profile | Flags | Behavior |
|---|---|---|
| Production | `SELFTEST=0 SELFTEST_AUTORUN=0` | Registers only the 13 normal commands; laboratory command objects and the selftest suite are excluded. |
| Manual selftest | `SELFTEST=1 SELFTEST_AUTORUN=0` | Registers development diagnostics, but runs them only after an explicit shell or harness command. |
| CI autorun | `SELFTEST=1 SELFTEST_AUTORUN=1` | Runs the suite only after `RUNTIME_READY`; this profile is exclusive to an explicit CI/QEMU harness. |

`SELFTEST_AUTORUN=1` without `SELFTEST=1` is an invalid configuration and the
Makefile rejects it immediately. Do not use an autorun image on normal
bare-metal hardware.

The production shell contains `help`, `clear`, `version`, `mem`, `cpu`, `pci`,
`acpi`, `echo`, `shutdown`, `restart`, `ps`, `kill`, and `taskman`, including
their ordinary aliases. IRQ inspection, deliberate panic, stress and regression
commands require `SELFTEST=1`. `irqlatency` also requires `IRQ_LATENCY=1`.
`DEBUG_ASSERT=1` alone does not register laboratory commands. Runtime safety
checks and emergency panic handling remain available in production.

The HPET, clock, task-metrics, and scheduler guard checks executed before
`scheduler_start()` are small, synchronous boot contract checks. They are not
the host-driven SMP matrices, fault injection, TASKMAN sessions, or soak tests,
and this policy does not remove them.

Build the profiles explicitly with:

```bash
make production-image
make kernel-check SELFTEST=1 SELFTEST_AUTORUN=0
make image SELFTEST=1 SELFTEST_AUTORUN=0
```

Use `SELFTEST_AUTORUN=1` on both development build commands only for the
explicit CI/QEMU role. `kernel-check` cleans the old objects before changing
build roles; keep the same flags for the following image build.

After a production boot, serial output must contain:

```text
[BOOT][TEST_READY] PASS autorun=0 selftests=0
```

The full static and binary policy check is available as:

```bash
make production-test-policy
```
