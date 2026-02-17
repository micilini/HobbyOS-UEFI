#include "kernel_init.h"
#include "../memory/gdt.h"
#include "idt.h"
#include "../graphics/console.h"
#include "../memory/pmem.h"
#include "../memory/paging.h"
#include "../memory/heap.h"
#include "../acpi/acpi.h"
#include "../acpi/madt.h"
#include "../apic/lapic.h"
#include "../apic/ioapic.h"
#include "../timer/hpet.h"
#include "../drivers/keyboard.h"
#include "../drivers/timer.h"
#include "../drivers/pci.h"
#include "../drivers/watchdog/intel_tco.h"
#include "../drivers/watchdog/acpi_wdat.h"
#include "../drivers/watchdog/acpi_wddt.h"
#include "../drivers/watchdog/acpi_wdrt.h"
#include "../drivers/serial.h"
#include "panic.h"
#include "timers.h"
#include "dpc.h"
#include "scheduler.h"
#include "semaphore.h"

#include "list.h"
#include "queue.h"

#include <stdint.h>
#include <stddef.h>






static void kinit_debug(const char *msg)
{
    serial_write_all(msg);
}

static void busy_hlt_delay(uint64_t loops)
{
    for (uint64_t i = 0; i < loops; i++)
    {
        __asm__ volatile("hlt");
    }
}

static void delay_seconds_for_reading(uint32_t seconds)
{
    for (uint32_t s = 0; s < seconds; s++)
    {
        for (uint64_t i = 0; i < 250000ULL; i++)
        {
            __asm__ volatile("hlt");
        }
    }
}

void init_system_core(BootInfo *boot_info)
{
    
    
    serial_init_from_bootinfo(boot_info);
    kinit_debug("[CORE] Serial init OK (dynamic ports from BootInfo)\n");

    
    kinit_debug("[CORE] Init GDT...\n");
    init_gdt();

    kinit_debug("[CORE] Init IDT...\n");
    init_idt();

    kinit_debug("[CORE] Init PMM...\n");
    init_pmm(boot_info->memory_map);

    kinit_debug("[CORE] Init Paging...\n");
    
    init_paging((uint64_t)boot_info->framebuffer->BaseAddress,
                (uint64_t)boot_info->framebuffer->BufferSize);

    
    kinit_debug("[CORE] Paging OK! Switched CR3.\n");

    kinit_debug("[CORE] Init Heap...\n");
    init_heap();

    kinit_debug("[CORE] Init Console...\n");
    console_init(boot_info);
    console_clear(CONSOLE_COLOR_BLACK);
    console_set_debug_enabled(1);

    
    void *test_heap = kmalloc(16);
    if (!test_heap)
        kpanic("CRITICAL: Heap Failed.");
    kfree(test_heap);

    kinit_debug("[CORE] Init ACPI...\n");
    init_acpi(boot_info->rsdp);

    acpi_list_tables_debug();
    acpi_enable_mode();

    
    intel_tco_disable();
    acpi_wdat_disable();
    acpi_wddt_disable();
    acpi_wdrt_disable();

    kinit_debug("[CORE] Init Interrupt Controller (MADT/APIC)...\n");
    init_madt();
    init_hpet();
    init_lapic();
    init_ioapic();

    kinit_debug("[CORE] Init Drivers & Scheduler...\n");
    keyboard_init();
    timer_init();
    timers_init();

    scheduler_init();

    dpc_init();

    thread_create(input_thread_entry, NULL);

    
    ioapic_map_irq(1, 33, 0);

    irq_enable();

    console_begin_batch();

    pci_init();

    console_end_batch();

    kinit_debug("[CORE] System Core Initialization Complete.\n");
    console_write_debug("[INIT] Waiting for devices to settle...\n");
    timer_sleep(500);
}
