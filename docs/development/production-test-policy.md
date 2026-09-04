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
| Production | `SELFTEST=0 SELFTEST_AUTORUN=0` | Does not register `tasktest` or call the selftest suite. Manual diagnostics remain registered. |
| Manual selftest | `SELFTEST=1 SELFTEST_AUTORUN=0` | Registers `tasktest`, but runs it only after an explicit shell or harness command. |
| CI autorun | `SELFTEST=1 SELFTEST_AUTORUN=1` | Runs the suite only after `RUNTIME_READY`; this profile is exclusive to an explicit CI/QEMU harness. |

`SELFTEST_AUTORUN=1` without `SELFTEST=1` is an invalid configuration and the
Makefile rejects it immediately. Do not use an autorun image on normal
bare-metal hardware.

The production shell keeps `schedtest`, `synctest`, `accounttest`, `killtest`,
`reaptest`, `inputtest`, `modaltest`, `taskmantest`, and `smpstress` as manual
diagnostics. None is called from the boot path. `tasktest` is development-only
and appears in the registry only with `SELFTEST=1`.

The HPET, clock, task-metrics, and scheduler guard checks executed before
`scheduler_start()` are small, synchronous boot contract checks. They are not
the host-driven SMP matrices, fault injection, TASKMAN sessions, or soak tests,
and this policy does not remove them.

Build the profiles explicitly with:

```bash
make production-image
make kernel.elf SELFTEST=1 SELFTEST_AUTORUN=0
make kernel.elf SELFTEST=1 SELFTEST_AUTORUN=1
```

After a production boot, serial output must contain:

```text
[BOOT][TEST_READY] PASS autorun=0 selftests=0
```

The full static and binary policy check is available as:

```bash
make production-test-policy
```
