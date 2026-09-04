# Bare-metal scheduler handoff trace

## 1. Frozen baseline

- Branch: `feat/taskman`
- Base commit: `7126ef635de448a9040689bd97feb6fd8ed59bab`
- Base tree: `469952e3a2cfce76c4123c1892d8190b0eb537b3`
- Initial worktree: only `ROADMAP_TASKMAN_V1_CLOSURE_HARDENING.md` untracked
- Initial stage: empty

All five required source hashes and all eight protected runtime hashes matched
the frozen baseline before editing.

## 2. Input evidence

The local input archive is named `hobbyos.zip`; its content hash matches the
frozen review source named `hobbyos(20260825-132347).zip`:

```text
3f17409e514bb655232efa2d80497ddbc9eb18a35d1b5b304e6cb18b44195c37  hobbyos.zip
ca630b9f282016ddcfd832c3b130011b41232ed6b7f0c667663f6850e628db8c  input kernel.elf
```

## 3. Frozen 24-CPU hardware observation

The supplied serial log reported all 24 topology-discovered logical CPUs
online and a complete SMP boot. It also reported creation and execution of the
DPC worker, input thread, shell thread, and reaper; task identity validation;
and `[SCHED][BOOT] START_OK`. No PCI, graphics, terminal prompt, or later core
completion record followed.

The stale framebuffer text `UEFI Watchdog disabled` therefore showed only
that `init_system_core()` had not returned. It is not evidence that the
bootloader or watchdog caused the stop.

## 4. Previous causal window

Before this change, the last confirmed interval began after scheduler
publication and after the four runtime tasks began executing, but before the
first PCI message. That interval included the return from `scheduler_start()`,
interrupt enable, the first possible timer preemption, `console_begin_batch()`,
and entry into `pci_init()`. No culprit is inferred from the frozen display or
the old serial tail.

## 5. CPU policy

No CPU cap, APIC whitelist, E-core filter, Hyper-Threading filter, or new boot
argument was added. Runtime topology continues to use the existing
`g_cpu_count`, `cpu_slot_t`, and `smp_bsp_cpu_slot()` contracts. The current
architectural capacity remains `HOBBYOS_MAX_CPUS=32`; this mission neither
reduces it nor claims unbounded scalability.

The physical target is expected to initialize the count discovered from its
topology, including all 24 logical CPUs observed in the supplied i9-12900K log.
No BIOS change is required.

## 6. LAPIC preservation

The BSP and AP paths still call:

```c
lapic_timer_calibrate(1000, ...);
lapic_timer_start_periodic(...);
```

The 1000 microsecond period, periodic mode, vector, divisor, calibration,
accounting, and reschedule behavior were not changed. `lapic.c`, `smp_boot.c`,
scheduler, and clock remain byte-identical to the baseline.

## 7. Autorun audit

Production is built with `SELFTEST=0 SELFTEST_AUTORUN=0`. Its preprocessed
`kernel.c` contains no call to `selftest_autorun_if_enabled()`, its registry has
no `tasktest` entry, and its kernel has no `[SELFTEST][AUTORUN]` marker.

The production registry still contains exactly one entry for each manual
diagnostic: `schedtest`, `synctest`, `accounttest`, `killtest`, `reaptest`,
`inputtest`, `modaltest`, `taskmantest`, and `smpstress`. Neither boot source
invokes any of their handlers.

## 8. Test modes and bounded boot checks

- Production: `SELFTEST=0 SELFTEST_AUTORUN=0`; no `tasktest`, suite call, or
  automatically created test workload.
- Manual selftest: `SELFTEST=1 SELFTEST_AUTORUN=0`; registers `tasktest` but
  waits for an explicit user or harness command.
- CI autorun: `SELFTEST=1 SELFTEST_AUTORUN=1`; invokes the suite only after
  `RUNTIME_READY`, and only for an explicitly selected CI/QEMU build.

The HPET, clock, task-metrics, and scheduler guard checks before
`scheduler_start()` are small, synchronous, and bounded. They passed on the
real target. They do not create 100 TASKMAN sessions, run an SMP matrix, inject
faults, or perform a soak. They remain because they are outside the final old
causal window and removing them would alter protected runtime files.

## 9. Dead development helpers

`serial_write_dec_all`, `test_smp_task`, `[SMPTEST]`, `busy_hlt_delay`, and
`delay_seconds_for_reading` were removed from `kernel_init.c`.
`serial_debug_char` was removed from `kernel.c`. The existing
`g_kernel_stack` object was retained with the compiler `used` attribute so the
touched file has no unused warning without changing its storage contract.

The final source and production kernel contain none of the removed helper
symbols or strings. The build emitted no warning for `kernel.c` or
`kernel_init.c`.

## 10. Permanent breadcrumbs

Exactly five direct, static, newline-terminated `serial_write_all()` calls now
run only in the BSP `init_system_core()` path:

1. `[BOOTTRACE][BSP] 1 before-scheduler-start` after successful identity
   validation and before `scheduler_start()`.
2. `[BOOTTRACE][BSP] 2 after-scheduler-start` after its successful return.
3. `[BOOTTRACE][BSP] 3 before-irq-enable` immediately before `irq_enable()`.
4. `[BOOTTRACE][BSP] 4 after-irq-enable` immediately after `irq_enable()`.
5. `[BOOTTRACE][BSP] 5 before-pci-init` after `console_begin_batch()` and
   immediately before `pci_init()`.

No breadcrumb uses console, heap, clock, timer, formatter, scheduler lock,
task creation, sleep, retry, or CPU-count data. The source-order gate and the
production binary count both passed with exactly five markers.

## 11. Interpretation of the next physical serial log

| Last marker observed | Causal window |
|---|---|
| no marker 1 | failure before the `scheduler_start()` call |
| marker 1, no marker 2 | inside `scheduler_start()` |
| marker 2, no marker 3 | scheduler return or the immediately following instructions |
| marker 3, no marker 4 | interrupt activation or the first IRQ/preemption |
| marker 4, no marker 5 | `console_begin_batch()` or concurrency immediately after `sti` |
| marker 5, no first PCI log | entry into `pci_init()`, console debug, or the first PCI access |
| marker 5 plus PCI logs | later inside PCI/xHCI enumeration |
| `Core Initialization Complete` | later in graphics, terminal, or shell startup |

These windows prepare the statuses
`BAREMETAL_STOP_BEFORE_SCHEDULER_START`,
`BAREMETAL_STOP_IN_SCHEDULER_START`, `BAREMETAL_STOP_ON_IRQ_ENABLE`,
`BAREMETAL_STOP_AFTER_IRQ_BEFORE_PCI`, `BAREMETAL_STOP_AT_PCI_ENTRY`,
`BAREMETAL_STOP_DURING_PCI`, `BAREMETAL_REACHED_CORE_COMPLETE`, and
`BAREMETAL_REACHED_SHELL`. The next complete serial log, not the frozen
framebuffer, selects a status.

## 12. Protected source continuity

The before, after-source, and after-build manifests have the same SHA-256:

```text
a6ca21c8938fb8ded1a278c673968c44047d5764389c4e1bdad9e615af87b7cc
```

Protected file hashes remain:

```text
8bce6412010c639b007d94815cb35960cd81fb36223a910c40854062177549a3  kernel/src/core/scheduler.c
d5b98fe3a496c11cbb2e622ce3e6997a17ad9cd9b800882d6f1c34a9de511c9c  kernel/src/core/clock.c
c21e884b4d9b5a1fdfd3577823a3c79da1f0bde44315f540c8c06f931795b906  kernel/src/apic/lapic.c
4ed5e0124f7d5e1e18b459c9938a97420e582d94fd6b39cdb509ab4edd5c0505  kernel/src/smp/smp_boot.c
06969e56b74bb4922def7e912175d996fe7aad991e0855f7c61ce15fe9bb2f51  kernel/src/core/semaphore.c
dd5a53461b79507ed40badd5af298fea840f710799950229b1651e68b649ae26  kernel/src/core/timers.c
1ceb620b004f6331a954eae2a87e31d81be48ab2e5214e5a190b6be5425c5223  kernel/src/drivers/pci.c
8cc987ee30f2a97c12d3a650e1e3eec7b70a70cdf45a64021254cc8cd8c6dba1  kernel/src/graphics/console.c
```

## 13. Production build

- `make stack-check`: PASS, 86 stack-usage files, maximum 1968/2048 bytes.
- `make production-image`: PASS after a full clean rebuild.
- `make deps-check`: PASS, including KVM and OVMF availability.
- `nm -u kernel.elf`: empty.
- Production policy: `[BOOT][PRODUCTION_TEST_POLICY] PASS autorun=0 tasktest=0 manual_diagnostics=1 breadcrumbs=5`.
- Embedded `::/kernel.elf`: byte-identical to the root `kernel.elf`.

## 14. Build decontamination proof

The only selftest build used `SELFTEST=1 SELFTEST_AUTORUN=1` after `make clean`.
`strings kernel.elf` found `[SELFTEST][AUTORUN] BEGIN`; that kernel was not
booted. `make production-image` then performed another full clean rebuild.
The final kernel has no autorun marker and passed the production policy gate.

## 15. Single QEMU smoke

Exactly one VM was started, with `SMP=4`, `ACCEL=KVM`, and the production
image. No selftest, TASKMAN session, stress worker, negative scenario, matrix,
or soak ran.

The raw serial evidence records, in order:

```text
line 227  marker 1
line 233  marker 2
line 234  marker 3
line 235  marker 4
line 236  marker 5
line 300  [CORE] System Core Initialization Complete.
line 334  [BOOT][SHELL_READY] PASS
line 344  [BOOT][RUNTIME_READY] PASS cpus=4/4 ...
line 346  [BOOT][TEST_READY] PASS autorun=0 selftests=0
line 347  [KERNEL] Entering Main Loop.
line 348  [TASKDIAG][CHECK] PASS ... structural_faults=0 ...
```

After shell readiness, the host sent only `version` and `taskdiag check`.
`version` writes `HobbyOS 0.2` to the console; `taskdiag check` produced the
machine-readable PASS above. The copied raw serial contains no
`PANIC`, `#PF`, `#GP`, `FATAL`, `STRUCTURAL_FAULT`, or `FINISH_FAULT`. QEMU was
stopped and left no PID or HMP socket.

## 16. Bare-metal candidate hashes

```text
1761b700ed4fb229b295e8d6ff5d26f4aaa79fa4f19b07094c0af35a8e404e53  kernel.elf
c94bfd15dd720f87630b6e18e0c49d12b3413665b759dc69afacb42e9a6fa704  hobbyos.img
3f0665e6a85825b4821d36f37c2f3d9f7e1f63c37b6fac726158ee66d7133b66  BOOTX64.EFI
```

The raw FAT hash identifies this concrete candidate; filesystem timestamps and
volume metadata are not treated as a reproducibility oracle.

## 17. Git disposition

The resulting commit has parent
`7126ef635de448a9040689bd97feb6fd8ed59bab` and subject
`diagnostic(boot): trace scheduler handoff and enforce manual test policy`.
Because a commit cannot embed its own content-addressed hash, resolve the exact
commit containing this report with `git rev-parse HEAD`; it is also recorded in
the external `candidate.txt`. No push, PR, merge, tag, rebase, stash, amend, or
new branch is part of this mission.

## 18. Status and physical handoff

Status: `READY_FOR_BARE_METAL_BOOT_TRACE`.

For the next physical attempt, write exactly the candidate `hobbyos.img`, start
serial capture before powering on the target, leave BIOS unchanged, keep every
CPU available, wait until the boot stops or reaches the shell, and preserve the
complete unedited serial log. If the shell is reached, run only `version` and
`taskdiag check`; do not run TASKMAN or stress diagnostics yet.
