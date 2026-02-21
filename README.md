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
* **IDT** — Interrupt Descriptor Table with full exception handling (ISR 0–31).
* **Kernel Panic (BSOD)** — SMP-safe panic with register dump, `#PF` bit decode, MMIO trace, configurable action (halt/restart/shutdown), countdown timer with in-place update, and `lapic_send_broadcast_halt()` to freeze secondary CPUs.
* **ACPI** — RSDP/XSDT/RSDT table parsing, ACPI mode enable via SMI_CMD, FADT, and S5 sleep type extraction for clean shutdown.
* **Watchdog Disable** — Intel TCO, ACPI WDAT, WDDT, and WDRT watchdog timers are detected and disabled at boot to prevent unwanted reboots.

#### Interrupt Controller & Timers
* **Local APIC** — Per-CPU LAPIC initialization, EOI, IPI (Inter-Processor Interrupt) support, broadcast halt for panic.
* **I/O APIC** — IRQ routing with MADT-based GSI mapping.
* **HPET** — High Precision Event Timer for system tick (1ms), `hpet_usleep()` busy-wait, and `timer_sleep()` with scheduler integration.

#### Memory Management
* **PMM** — Physical Memory Manager using bitmap allocator with contiguous frame allocation support.
* **VMM** — 4-Level Paging (PML4 → PDPT → PD → PT) with identity + higher-half mapping, MMIO mapping (PCD/PWT), and NX bit support.
* **Heap** — Dynamic kernel allocator (`kmalloc`/`kfree`/`kmalloc_aligned`) with 32MB initial pool and auto-expansion via PMM.

#### SMP (Symmetric Multiprocessing)
* **AP Bootstrap** — Real-mode trampoline (`trampoline.S`) boots Application Processors via INIT-SIPI-SIPI sequence.
* **Topology Discovery** — MADT-based CPU enumeration, per-CPU structures, BSP APIC ID detection (works when BSP ≠ APIC ID 0).
* **Per-CPU State** — Each CPU has its own GDT, TSS, IDT, stack, idle task, and `need_resched` flag indexed by APIC ID.

#### Scheduler & Multitasking
* **Preemptive Scheduler** — Timer IRQ-driven preemption with quantum accounting (no voluntary yield required).
* **Priority Classes** — Two-queue design: `INTERACTIVE` (5ms quantum, always scheduled first) and `NORMAL` (20ms quantum). Interactive tasks preempt normal tasks immediately.
* **Context Switching** — Full register save/restore in assembly (`switch.S`), per-task kernel stacks, cooperative (`schedule_voluntary`) and preemptive paths.
* **Thread Lifecycle** — `thread_create`, `thread_create_with_class`, `thread_block`, `thread_wake`, `thread_exit` (ZOMBIE state), and `thread_wrapper` in assembly for clean return.
* **Synchronization Primitives** — Spinlocks (with `irqsave`/`irqrestore` and `trylock` variants), Semaphores (counting, with wait queues), Wait Queues (intrusive linked list).
* **DPC (Deferred Procedure Call)** — Lock-free MPSC queue with dedicated interactive worker thread for bottom-half processing (used by xHCI).
* **Software Timers** — Sorted deadline list, callback-based, driven from timer IRQ. Supports `timer_sleep()` for blocking delays.

#### Graphics & UI
* **Framebuffer** — GOP-based pixel rendering with configurable resolution.
* **Double Buffering** — Full back-buffer with `swap_buffers()` (RAM → VRAM memcpy) for flicker-free rendering. Can be disabled (e.g., during panic).
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
* **Built-in commands:** `help`, `echo`, `clear`, `version`, `mem`, `cpu`, `pci`, `irq`, `acpi`, `power` (shutdown/restart/sleep), `panic`, `smpstress`, `usbdiag`.

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
* Basic `string.h` (`strlen`, `strcmp`, `strncmp`, `strcpy`, `strncpy`, `strcat`, `memcmp`) and `memory.h` (`memset`, `memcpy`, `memmove`) implementations.

---

### 🛠️ To-Do (Planned — in rough priority order)

1. **Fix: xHCI NO-OP Freeze** — The NO-OP command can occasionally freeze the system during hotplug sequences. Needs timeout or abort handling.
2. **USB Hotplug Behind Hubs** — Currently hotplug only works on root ports. Devices connected/disconnected through USB hubs are not detected dynamically.
3. **Task Manager** — Shell command to list running threads with ID, name, state, CPU affinity, class, and quantum stats.
4. **Ring 0 Hardening** — The kernel currently runs everything in Ring 0 with a single address space. Audit and document this as an intentional design choice, enforce stack guards, and add kernel-only memory protections (NX on data, RO on code).
5. **Topology-Aware Scheduling** — Use MADT/SRAT/cache topology to make scheduling decisions (prefer same-package CPUs, NUMA awareness, cache-affinity).
6. **Support for UHD 770 (GPU)** — Create a Driver to support Intel GPU, AMD and for future NVIDIA (model by model).
7. **Per-Process Virtual Memory** — Separate CR3 per task, private address spaces, copy-on-write fork, and kernel/user page table split.
8. **User Space** — Ring 3 execution with syscall interface (SYSCALL/SYSRET), user-mode stacks, and privilege separation.
9. **Filesystem** — FAT12/FAT16/FAT32 and exFAT read/write support with a VFS abstraction layer.
10. **Graphical User Interface** — Windowed desktop environment with mouse support, window manager, and file explorer (inspired by Windows Explorer).

---

*(c) Portal Micilini — All Rights Reserved.*