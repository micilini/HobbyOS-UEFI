#include "kernel_init.h"
#include "panic.h"
#include "../memory/gdt.h"
#include "../memory/pmem.h"
#include "../memory/paging.h"
#include "../memory/heap.h"
#include "../acpi/acpi.h"
#include "../acpi/madt.h"
#include "../apic/lapic.h"
#include "../apic/ioapic.h" 
#include "../timer/hpet.h"
#include "../graphics/console.h"
#include "idt.h"
#include "../drivers/keyboard.h"
#include "../drivers/timer.h" 

void init_system_core(BootInfo* boot_info) {
    
    
    init_gdt();
    init_idt(); 

    
    init_acpi(boot_info->rsdp);
    init_madt();
    init_hpet();  
    init_lapic();
    init_ioapic();

    
    init_pmm(boot_info->memory_map);
    init_paging((uint64_t)boot_info->framebuffer->BaseAddress, boot_info->framebuffer->BufferSize);
    init_heap();

    
    console_init(boot_info);
    console_clear(CONSOLE_COLOR_BLACK); 

    
    void* test_heap = kmalloc(16);
    if (!test_heap) kpanic("CRITICAL: Heap Failed.");
    kfree(test_heap);

    
    

    
    keyboard_init(); 
    timer_init(); 
    ioapic_map_irq(1, 33, 0);

    __asm__ volatile ("sti");
}