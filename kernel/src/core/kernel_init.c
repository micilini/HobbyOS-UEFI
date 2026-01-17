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
#include "panic.h"

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

    init_gdt();
    init_idt();

    init_pmm(boot_info->memory_map);

    init_paging((uint64_t)boot_info->framebuffer->BaseAddress,
                (uint64_t)boot_info->framebuffer->BufferSize);

    init_heap();

    console_init(boot_info);
    console_clear(CONSOLE_COLOR_BLACK);

    console_set_debug_enabled(1);

    void *test_heap = kmalloc(16);
    if (!test_heap)
        kpanic("CRITICAL: Heap Failed.");
    kfree(test_heap);

    init_acpi(boot_info->rsdp);

    acpi_list_tables_debug();
    acpi_enable_mode();

    intel_tco_disable();

    acpi_wdat_disable();
    acpi_wddt_disable();
    acpi_wdrt_disable();

    init_madt();

    init_hpet();
    init_lapic();
    init_ioapic();

    keyboard_init();
    timer_init();

    ioapic_map_irq(1, 33, 0);

    __asm__ volatile("sti");

    console_begin_batch();

    pci_init();

    console_end_batch();
}