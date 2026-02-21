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
#include "../smp/smp_topology.h"
#include "../smp/smp_boot.h"
#include "list.h"
#include "queue.h"
#include "task.h"

#include <stdint.h>
#include <stddef.h>

static void serial_write_dec_all(uint64_t v)
{
    char buf[32];
    int i = 0;

    if (v == 0)
    {
        serial_putc_all('0');
        return;
    }

    while (v && i < (int)(sizeof(buf) - 1))
    {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }

    while (i--)
        serial_putc_all(buf[i]);
}

static void test_smp_task(void *arg)
{
    uint64_t id = (uint64_t)arg;

    while (1)
    {
        // Vai pro SERIAL (não suja framebuffer/splash/shell)
        serial_write_all("[SMPTEST] Task ");
        serial_write_dec_all(id);
        serial_write_all(" on CPU ");
        serial_write_dec_all((uint64_t)lapic_get_id());
        serial_write_all("\n");

        timer_sleep(1000);
    }
}

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

    
    
    kinit_debug("[CORE] Init SMP Topoly...\n");
    smp_topology_init();
    smp_prepare_cpu_structures();
    
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        if (!g_cpus[i].is_bsp) {
            
            
            
            lapic_send_ipi(g_cpus[i].apic_id, 0xFE);
            
            kinit_debug("[SMP] Test IPI sent to AP (If system runs, Phase 3 is OK)\n");
            break; 
        }
    }

    smp_boot_aps();

    

    

    kinit_debug("[CORE] Init Drivers & Scheduler...\n");
    keyboard_init();
    timer_init();
    timers_init();

    scheduler_init();

    //SMP TESTS
    
    //thread_create(test_smp_task, (void*)1);
    //thread_create(test_smp_task, (void*)2);
    //thread_create(test_smp_task, (void*)3);

    dpc_init();

    /* Input thread é INTERATIVO — prioridade alta para baixa latência */
    thread_create_with_class(input_thread_entry, NULL, TASK_CLASS_INTERACTIVE);

    ioapic_map_irq(1, 33, 0);

    extern volatile int g_system_ready_for_scheduling;
    g_system_ready_for_scheduling = 1;

    irq_enable();

    console_begin_batch();

    pci_init();

    console_end_batch();

    kinit_debug("[CORE] System Core Initialization Complete.\n");
    console_write_debug("[INIT] Waiting for devices to settle...\n");
    timer_sleep(500);
}
