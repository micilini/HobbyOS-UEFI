# Bare-metal LAPIC clockevent operator runbook

Use this runbook only after the candidate metadata reports
`READY_FOR_BARE_METAL_LAPIC_CLOCKEVENT_VALIDATION`.

The candidate may document a raw virtual SMP24 LAPIC-rate failure together
with `ENVIRONMENT_NON_AUTHORITATIVE_OVERSUBSCRIBED`. That classification is
specific to the QEMU host CPU allowance; it does not change the 1000 us LAPIC
period or waive any physical marker below. Do not reproduce host workarounds
on the target.

1. Use exactly
   `artifacts/baremetal/lapic-clockevent-candidate/hobbyos.img`.
2. Verify the image with the adjacent `SHA256SUMS` before writing it to media.
3. Start complete serial capture before power-on: 115200 baud, 8 data bits,
   no parity, 1 stop bit, no flow control.
4. Do not change BIOS settings. Keep every CPU, E-core, and Hyper-Threading
   context available.
5. Boot the target and preserve the complete serial log without editing.
6. Wait for the shell or a terminal stop. Do not retry with fewer CPUs or a
   different LAPIC period.
7. If the shell appears, run only:

   ```text
   version
   irq check
   irq boot
   accounttest check
   taskdiag check
   ```

8. Do not run TASKMAN, `smpstress`, heavy `synctest`, or heavy `reaptest` on
   this first attempt.
9. Return the complete serial log to the project maintainer.

The expected physical sequence is:

```text
[IRQ][BSP_LAPIC_PROBE] PASS vector=34
[CLOCK][HPET_CLOCKSOURCE_PROBE] PASS
[CLOCK][HPET_TIMER0] QUIESCENT
[CLOCKEVENT][RUNTIME] ACTIVE source=BSP_LAPIC bsp_slot=0 period_us=1000
[SCHED][BOOTSTRAP_HANDOFF] PASS slot=0
[BOOTTRACE][BSP] 4 after-irq-enable
[IRQ][CPU_READY] PASS ... 24/24
[IRQ][BOOTSTRAP] CLOCKEVENT_ACTIVE cpus=24
[CORE] System Core Initialization Complete.
[GRAPHICS][SPLASH] BEGIN
[GRAPHICS][SPLASH] COMPLETE
[BOOT][SHELL_READY] PASS
[BOOT][RUNTIME_READY] PASS cpus=24/24
[KERNEL] Entering Main Loop.
```

Per-CPU records may interleave. Stop and return the untouched log if the HPET
counter probe fails, Timer0 is not quiescent, a CPU does not become ready, an
HPET stray or unexpected vector appears, or any panic/fault marker is emitted.
This runbook does not authorize BIOS changes, CPU filtering, a different LAPIC
period, or a second stress boot.
