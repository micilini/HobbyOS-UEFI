# Bare-metal interrupt bring-up operator runbook

Use this runbook only after the certification report says
`READY_FOR_BARE_METAL_INTERRUPT_VALIDATION` and the candidate directory is
complete.

1. Use exactly
   `artifacts/baremetal/interrupt-bringup-candidate/hobbyos.img`.
2. Verify it against `SHA256SUMS` before writing physical media.
3. Start serial capture at 115200 baud, 8 data bits, no parity, 1 stop bit, and
   no flow control before power-on.
4. Do not change BIOS settings. Keep all discovered CPUs, E-cores, and
   Hyper-Threading available.
5. Power on the target and preserve the complete serial stream without edits.
6. Wait for either the shell or a terminal stop. Do not add a delay and do not
   retry with a reduced CPU count.
7. On the first boot, if the shell appears, run only:

   ```text
   version
   irq check
   irq boot
   taskdiag check
   ```

8. Do not run TASKMAN or stress on the first physical attempt.
9. Return the full serial log and, if possible, a photograph of the screen.

The expected logical marker sequence is:

```text
[IRQ][PIC] QUIESCENT
[IRQ][LAPIC] QUIESCENT slot=0
[IRQ][IOAPIC] QUIESCENT
[IRQ][BOOTSTRAP] CONTROLLERS_QUIESCENT
[IRQ][BOOTSTRAP] ROUTES_PREPARED
[IRQ][BOOTSTRAP] CPUS_PREPARED cpus=24
[SCHED][BOOT] START_OK
[IRQ][BSP_LAPIC_PROBE] PASS
[IRQ][HPET_PROBE] PASS
[SCHED][BOOTSTRAP_HANDOFF] PASS slot=0
[IRQ][CPU_READY] PASS
[IRQ][BOOTSTRAP] TIMERS_ACTIVE cpus=24
[GRAPHICS][EARLY_BIND] PASS
[CORE] System Core Initialization Complete.
[IRQ][BOOTSTRAP] SERVICES_ACTIVE
[BOOT][SHELL_READY] PASS
[BOOT][RUNTIME_READY] PASS cpus=24/24
[BOOT][TEST_READY] PASS autorun=0 selftests=0
[KERNEL] Entering Main Loop.
```

Per-CPU lines can interleave. The BSP probe and state-transition records are
single serial writes. Stop and return the original log if any probe reports
failure, a CPU does not become ready, an unexpected vector appears, or a fault
record is emitted. A second TASKMAN/stress boot requires separate operator
authorization after this basic boot passes.
