# HobbyOS - Kernel Version (x86_64)

> 🚧 **STATUS: ACTIVE DEVELOPMENT** 🚧

This is the active development branch for the new Monolithic Kernel architecture.
If you are looking for the old/stable version (Bootloader/UEFI only), please visit the **[UEFI-Version Branch](https://github.com/micilini/HobbyOS-UEFI/tree/UEFI-Version)**.

## 🗺️ Development Status

### ✅ Implemented (Done)
* **Bootloader:** Custom UEFI Loader (BootInfo & Memory Map).
* **Core:**
    * GDT (Global Descriptor Table).
    * IDT (Interrupt Descriptor Table).
    * Exception Handling (ISR) & Kernel Panic (BSOD).
    * APIC/IOAPIC & HPET Timer support.
* **Memory Management:**
    * PMM (Physical Memory Manager - Bitmap).
    * VMM (Virtual Memory Manager - 4-Level Paging).
    * Heap (Dynamic Allocation - malloc/free) with 32MB initial pool.
* **Graphics & UI:**
    * Framebuffer Support (GOP).
    * Double Buffering (RAM to VRAM) for flicker-free rendering.
    * **Circular Console:** Instant scrolling using a logical Ring Buffer (2000 lines).
    * Splash Screen with Animation & Memory Cleanup.
* **Input & Shell:**
    * **Keyboard Driver:** PS/2 Driver with Scancode translation, modifiers (Shift/Caps), and hardware auto-repeat filtering.
    * **Interactive Shell:** Real-time command processing with cursor handling and atomic execution.
* **LibC:** Basic `string.h` and `memory.h` implementations.

### 🛠️ To-Do
* [ ] **Multitasking:** Scheduler & Basic Processes.
* [ ] **Input:** PS/2 Mouse Driver support.
* [ ] **PCI:** Bus scanning and device discovery.
* [ ] **Filesystem:** Basic FAT32/ISO9660 file reading support.

---
*(c) Portal Micilini - All Rights Reserved.*