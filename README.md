# HobbyOS — x86_64 Monolithic Kernel

> 🚧 **STATUS: ACTIVE DEVELOPMENT** 🚧

A hobby operating system built from scratch in C and x86_64 Assembly, booting via custom UEFI bootloader.
Features a preemptive SMP scheduler with priority classes, xHCI USB 3.0 keyboard support, and a fully interactive shell — all running in Ring 0.

If you are looking for the old/stable version (Bootloader/UEFI only), please visit the **[UEFI-Version Branch](https://github.com/micilini/HobbyOS-UEFI/tree/UEFI-Version)**.

---

## 🗺️ Development Status

### ✅ Implemented

#### Bootloader
* Custom UEFI Loader with BootInfo protocol, memory map, framebuffer, font, logo, and serial port discovery.

#### Core Architecture
* **GDT** — Global Descriptor Table (flat 64-bit segments, per-CPU TSS).
* **IDT** — Interrupt gates are installed for exception vectors 0–31; NMI,
  double fault, and machine check use IST stacks. These entries provide
  terminal diagnostics, not universal recovery from arbitrary corruption.
* **Kernel Panic** — An SMP owner/reentry protocol emits bounded emergency
  diagnostics directly to serial, including the available interrupt frame,
  `#PF` bits, CR3, and a best-effort MMIO trace. Countdown and
  halt/restart/shutdown attempts have finite budgets and documented fallbacks;
  an unavailable UART cannot delay the terminal action. The framebuffer may
  retain its previous contents. Physical validation of these terminal paths is
  deferred to final closure.
* **ACPI** — RSDP/XSDT/RSDT table parsing, ACPI mode enable via SMI_CMD, FADT, and S5 sleep type extraction for clean shutdown.
* **Watchdog Disable** — Intel TCO, ACPI WDAT, WDDT, and WDRT watchdog timers are detected and disabled at boot to prevent unwanted reboots.
* **Freestanding C contract** — Kernel C builds explicitly disable strict
  aliasing assumptions, compiler stack-protector runtime dependencies, and
  deletion of null-pointer checks. The common profile also enables
  `-Wextra`, `-Wundef`, `-Wmissing-prototypes`, `-Wvla`, the 2048-byte frame
  warning and `-Werror`; `graphics.c` alone retains the documented
  protected-file exception for two legacy prototypes. Debug, selftest and
  production kernel objects use the common `-O2` profile; the libc memory
  implementation alone disables loop-pattern recognition so GCC cannot turn
  its own byte loops into recursive libc calls.

#### Interrupt Controller & Timers
* **Safe controller acquisition** — Legacy PIC quarantine, bounded MADT parsing
  with PCAT/ISO metadata, all-IOAPIC GSI registries, masked route preparation,
  neutral LAPIC LVT state, and deliberate enter/return probes before release.
* **Local APIC** — Per-CPU 1000 us timer calibration with bounded,
  HPET-bracketed counter samples, masked preparation, coordinated activation,
  EOI, IPI support, and broadcast halt for panic.
* **I/O APIC** — Multi-controller routing by GSI range with firmware entries
  masked before known routes are installed; the HPET Timer0 route stays absent
  or masked.
* **Clocksource/clockevent split** — HPET is the monotonic clocksource with
  Timer0 quiescent. Per-CPU LAPIC timers stay at 1000 us, and only the BSP
  LAPIC drives global software timers and released deferred services.

#### Memory Management
* **PMM** — Physical Memory Manager with separate allocation/reservation
  bitmaps, word-at-a-time first-fit searches, contiguous frame allocation and
  explicit rejection of invalid frees.
* **VMM** — 4-Level Paging (PML4 → PDPT → PD → PT) with identity + higher-half
  mapping and MMIO mapping (PCD/PWT). An NX flag constant exists, but the
  current mappings do not establish an NX, read-only, or W^X policy.
* **Heap** — Dynamic kernel allocator (`kmalloc`/`kfree`/`kmalloc_aligned`)
  with a 32MB initial pool, PMM-backed expansion, checked size arithmetic and
  locked provenance validation for frees.

#### SMP (Symmetric Multiprocessing)
* **AP Bootstrap** — Real-mode trampoline (`trampoline.S`) boots Application
  Processors via INIT-SIPI-SIPI; a fronteira final limpa DF e entrega a C uma
  stack alinhada segundo a ABI SysV sem alterar o blob de low memory.
* **Topology Discovery** — MADT-based CPU enumeration, per-CPU structures, BSP APIC ID detection (works when BSP ≠ APIC ID 0).
* **Per-CPU State** — Each CPU has its own GDT, TSS, IDT, stack, idle task, and `need_resched` flag indexed by APIC ID.

#### Scheduler & Multitasking
* **Preemptive Scheduler** — Timer IRQ-driven preemption with quantum accounting (no voluntary yield required).
* **Priority Classes** — Two-queue design: `INTERACTIVE` (5ms quantum, always scheduled first) and `NORMAL` (20ms quantum). Interactive tasks preempt normal tasks immediately.
* **Context Switching** — Full register save/restore in assembly (`switch.S`), per-task kernel stacks, cooperative (`schedule_voluntary`) and preemptive paths.
* **Thread Lifecycle** — `thread_create`, `thread_create_with_class`, `thread_block`, `thread_wake`, `thread_exit` (ZOMBIE state), and `thread_wrapper` in assembly for clean return.
* **Synchronization Primitives** — FIFO ticket spinlocks (with
  `irqsave`/`irqrestore`, non-queuing `trylock`, and zero-filled static
  initialization), Semaphores (counting, with wait queues), Wait Queues
  (intrusive linked list).
* **DPC (Deferred Procedure Call)** — Lock-free MPSC queue with dedicated interactive worker thread for bottom-half processing (used by xHCI).
* **Software Timers** — Sorted deadline list, callback-based, driven from timer IRQ. Supports `timer_sleep()` for blocking delays.
* **TASKMAN V1 (complete and certified)** — Paginated `ps`, cooperative `kill`, modal
  `taskman`, read-only `taskdiag`, hardened task identity/lifecycle/reaper,
  windowed CPU accounting, and generation-bound snapshots.
* **Lifecycle & Test Hardening** — Exactly-once cleanup/notification,
  timer-reference-safe reap, modal/input ownership, and bounded selftest/fault
  injection infrastructure for SMP certification.

#### Graphics & UI
* **Framebuffer** — GOP-based pixel rendering with configurable resolution.
* **Double Buffering** — Full back-buffer with `swap_buffers()` (RAM → VRAM memcpy) for flicker-free rendering.
* **Console** — Circular ring-buffer console (2000 lines × 512 cols), per-cell foreground/background colors, batch rendering mode, `render_suspended` flag for splash protection, scroll with `render_suspended` awareness.
* **Splash Screen** — Animated logo fade-in/fade-out using alpha overlay, runs after all init is complete, auto-frees logo memory after playback.

#### Input
* **PS/2 Keyboard** — IRQ-driven driver with scancode-to-ASCII translation, modifier tracking (Shift, Caps Lock), and hardware auto-repeat filtering.
* **USB Keyboard (xHCI)** — Full xHCI host controller driver with:
    * BIOS handoff (Legacy → OS ownership).
    * Command Ring, Event Ring, and Transfer Ring management.
    * Device enumeration with Enable Slot → Address Device → Get Descriptor → Set Configuration → Set Protocol (Boot) pipeline.
    * Multi-slot support (up to 64 devices), multi-keyboard support.
    * HID Boot Protocol parsing with modifier tracking and software key repeat.
    * Keyboard LED control (Caps Lock, Num Lock, Scroll Lock).
    * Endpoint halt recovery and transfer ring reset.
    * Polling-based event processing via DPC (timer IRQ → `xhci_poll_events` → DPC worker).
* **USB Hub Support** — Hub enumeration, port power-on, port reset, device configuration behind hubs (route string, TT for USB2 behind USB3 hubs), hub tree display.
* **USB Hotplug** — State machine for root port connect/disconnect detection with debounce, port reset, and device configuration. *(Hub-level hotplug is a known limitation — see To-Do.)*
* **Unified Input Thread** — Single interactive-priority thread processes both PS/2 IRQs and USB HID reports into a shared key queue consumed by the shell.

#### Shell
* Interactive command-line shell with real-time keystroke processing, cursor display (blinking via `shell_on_tick` with trylock for IRQ safety), command history buffer, and atomic command execution.
* **Built-in commands:** `help`, `echo`, `clear`, `version`, `mem`, `cpu`,
  `pci`, `irq`, `acpi`, `power` (shutdown/restart/sleep), `panic`, `smpstress`,
  `usbdiag`, `ps`, `kill`, `taskman`, and `taskdiag`.
  `mem` and `cpu` also emit bounded, atomic `[MEM][SUMMARY]` and
  `[CPU][SUMMARY]` serial receipts for offline validation; an inconsistent
  census returns a nonzero command status.
* **Test/diagnostic commands:** `schedtest`, `synctest`, `accounttest`,
  `killtest`, `reaptest`, `inputtest`, `modaltest`, and `taskmantest` support
  development builds. `tasktest` and the bounded FIFO contention command
  `locktest`, the allocator hardening command `alloctest`, and the ABI/memory
  command `profiletest` are registered only when `SELFTEST=1`.

#### PCI
* MCFG-based PCIe configuration space scanning, vendor/device/class name lookup, MSI (Message Signaled Interrupts) configuration, Bus Master enable.

#### Power Management
* **Shutdown** — ACPI S5 sleep via PM1a/PM1b control registers.
* **Restart** — ACPI reset register → keyboard controller 8042 reset → triple fault fallback chain.

#### Serial
* Dynamic serial port discovery from BootInfo (bootloader-detected ports).
* Multi-port broadcast output (`serial_write_all`).
* All kernel debug output (`console_write_debug` and friends) is routed exclusively to serial — zero framebuffer pollution.

#### LibC
* Basic `string.h` (`strlen`, `strcmp`, `strncmp`, `strcpy`, `strcat`,
  `itoa`, `k_int_to_hex`, `ksnprintf`, `kvsnprintf`) and `memory.h` (`memset`,
  `memcpy`, `memmove`, `memcmp`) implementations. The copy/set/move paths use
  64-bit general-purpose registers for their aligned body and byte handling at
  unaligned boundaries; `memmove` preserves overlap in both directions.
  Bounded formatting reports
  the required length, always terminates a valid positive-capacity destination,
  and supports the documented integer, string, character, and pointer subset;
  unsupported syntax returns `-1`.

---

## Bare-metal production image

Use `make production-image` as the canonical command for physical media. Task
diagnostics remain available as manual shell commands and are not run at boot.

Foundation Stage 1 is closed for host/QEMU validation. The final status and
the deliberately deferred Stage 2 work are recorded in
[`docs/foundation/reports/E1-FECHAMENTO.md`](docs/foundation/reports/E1-FECHAMENTO.md);
the physical i9-12900K run remains a maintainer action documented in
[`docs/foundation/reports/E1-BM-roteiro.md`](docs/foundation/reports/E1-BM-roteiro.md).

Interrupt bring-up diagnostics are available through `irq check`,
`irq controllers`, `irq routes`, and `irq boot`. The complete host/QEMU gate is
`scripts/test-timer-clockevent.sh all`; architecture and operator guidance are
in [the timer clockevent document](docs/timer-clocksource-clockevent.md). The
controller acquisition protocol remains documented in
[the interrupt bring-up document](docs/interrupt-controller-bringup.md).

---

## TASKMAN V1 documentation

* [Public contract](docs/taskman-v1-contract.md)
* [Architecture](docs/taskman-v1-architecture.md)
* [Task lifecycle](docs/task-lifecycle.md)
* [Input/modal ownership](docs/input-modal-architecture.md)
* [User guide](docs/taskman-v1-user-guide.md)
* [Command reference](docs/taskman-v1-command-reference.md)
* [Known limitations](docs/taskman-v1-known-limitations.md)
* [Test plan](docs/taskman-v1-test-plan.md)
* [Homologation](docs/taskman-v1-homologation.md)
* [File inventory](docs/taskman-v1-file-inventory.md)
* [Maintainer handoff](docs/taskman-v1-handoff-checklist.md)
* [V2 entry criteria](docs/taskman-v2-entry-criteria.md)
* [Release notes](docs/releases/TASKMAN_V1_RELEASE_NOTES.md)
* [Release manifest](docs/releases/TASKMAN_V1_RELEASE_MANIFEST.md)

---

### 🛠️ To-Do (Planned — in rough priority order)

1. **Ring 0 Hardening** — The kernel currently runs everything in Ring 0 with a single address space. Audit and document this as an intentional design choice, enforce stack guards, and add kernel-only memory protections (NX on data, RO on code).
2. **TASKMAN V2** — Add real per-task CPU affinity and memory
   ownership/accounting, then evolve the monitor toward process-aware identity
   and reusable modal system interfaces.
3. **Topology-Aware Scheduling** — Use MADT/SRAT/cache topology to make scheduling decisions (prefer same-package CPUs, NUMA awareness, cache-affinity).
4. **Support for UHD 770 (GPU)** — Create a Driver to support Intel GPU, AMD and for future NVIDIA (model by model).
5. **Filesystem** — FAT12/FAT16/FAT32 and exFAT read/write support with a VFS abstraction layer.
6. **Graphical User Interface** — Windowed desktop environment with mouse support, window manager, and file explorer (inspired by Windows Explorer).

---

*(c) Portal Micilini — All Rights Reserved.*
