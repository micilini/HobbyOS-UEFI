#include "smp_boot.h"
#include "smp_topology.h"
#include "../acpi/madt.h"
#include "../apic/lapic.h"
#include "../drivers/serial.h"
#include "../libc/memory.h"
#include "../libc/string.h"
#include "../memory/gdt.h"
#include "../core/idt.h"
#include "../memory/paging.h"
#include "../memory/pmem.h"
#include "../cpu/cpu.h" 

extern uint64_t smp_trampoline_cr4;
extern uint64_t smp_trampoline_efer;
extern uint32_t smp_trampoline_x2apic_flag;
extern uint32_t smp_trampoline_base;
extern uint8_t smp_trampoline_gdtr;
extern uint8_t smp_trampoline_gdt_start;
extern uint8_t _trampoline_start[];
extern uint8_t _trampoline_end[];
extern uint64_t smp_trampoline_cr3;
extern uint64_t smp_trampoline_stack;
extern uint64_t smp_trampoline_entry_ptr;

extern void gdt_flush(uint64_t gdtr_addr);

#define HHDM_OFFSET 0xFFFFFFFF80000000ULL

volatile bool g_ap_boot_done = false;

static void smp_print_dec(uint32_t n) {
    if (n == 0) { serial_write_all("0"); return; }
    char buffer[32];
    int i = 0;
    while (n > 0) { buffer[i++] = '0' + (n % 10); n /= 10; }
    for (int j = i - 1; j >= 0; j--) { serial_putc_all(buffer[j]); }
}

static void smp_print_hex(uint64_t n) {
    serial_write_all("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t nibble = (n >> i) & 0xF;
        char c = (nibble < 10) ? ('0' + nibble) : ('A' + (nibble - 10));
        serial_putc_all(c);
    }
}

void ap_kernel_entry() {
    serial_write_all("A"); 

    
    extern PageTable *g_kernel_pml4;
    paging_load_map(g_kernel_pml4);
    
    
    
    idt_load(); 
    serial_write_all("I"); 

    uint32_t my_apic_id = lapic_get_id();
    SmpCpuInfo *me = NULL;

    for (uint32_t i = 0; i < g_cpu_count; i++) {
        if (g_cpus[i].apic_id == my_apic_id) {
            me = &g_cpus[i];
            break;
        }
    }

    if (!me) {
        serial_write_all("PANIC-CPU\n");
        while(1) __asm__ volatile("cli; hlt");
    }

    me->state = CPU_STATE_STARTING;

    
    GdtPtr gdtr;
    gdtr.size = sizeof(GdtTable) - 1;
    gdtr.offset = (uint64_t)me->gdt_ptr;
    
    gdt_flush((uint64_t)&gdtr);
    serial_write_all("G"); 

    __asm__ volatile("mov $0x28, %%ax; ltr %%ax" ::: "ax");
    serial_write_all("T"); 

    
    init_lapic_ap();
    serial_write_all("L"); 

    
    
    lapic_timer_set_periodic(32, 10000000);
    serial_write_all("C"); 

    __asm__ volatile("mfence" ::: "memory");
    me->state = CPU_STATE_ONLINE;
    serial_write_all("!"); 

    
    while (1) {
        __asm__ volatile("sti; hlt");
    }
}

static uint64_t smp_find_trampoline_page() {
    for (uint64_t addr = 0x1000; addr < 0xA0000; addr += 0x1000) {
        if (pmm_is_frame_free(addr)) return addr;
    }
    return 0;
}

void smp_boot_aps() {
    serial_write_all("[SMP] Starting BOOTSTRAP sequence...\n");

    PageTable *boot_pml4 = paging_create_bootstrap_table();
    if (!boot_pml4) return;
    
    uint64_t boot_cr3 = (uint64_t)boot_pml4; 
    
    serial_write_all("[SMP] Bootstrap CR3 created at ");
    smp_print_hex(boot_cr3);
    serial_write_all("\n");

    uint64_t trampoline_addr = smp_find_trampoline_page();
    if (!trampoline_addr) trampoline_addr = 0x10000; 
    
    pmm_mark_frame_used(trampoline_addr);
    
    uint64_t trampoline_len = (uint64_t)_trampoline_end - (uint64_t)_trampoline_start;
    memcpy((void*)trampoline_addr, _trampoline_start, trampoline_len);

    volatile uint64_t *ptr_cr3    = (volatile uint64_t*)(trampoline_addr + ((uint64_t)&smp_trampoline_cr3 - (uint64_t)_trampoline_start));
    volatile uint64_t *ptr_cr4    = (volatile uint64_t*)(trampoline_addr + ((uint64_t)&smp_trampoline_cr4 - (uint64_t)_trampoline_start));
    volatile uint64_t *ptr_efer   = (volatile uint64_t*)(trampoline_addr + ((uint64_t)&smp_trampoline_efer - (uint64_t)_trampoline_start));
    volatile uint64_t *ptr_stack  = (volatile uint64_t*)(trampoline_addr + ((uint64_t)&smp_trampoline_stack - (uint64_t)_trampoline_start));
    volatile uint64_t *ptr_entry  = (volatile uint64_t*)(trampoline_addr + ((uint64_t)&smp_trampoline_entry_ptr - (uint64_t)_trampoline_start));
    volatile uint32_t *ptr_x2flag = (volatile uint32_t*)(trampoline_addr + ((uint64_t)&smp_trampoline_x2apic_flag - (uint64_t)_trampoline_start));
    volatile uint8_t  *ptr_gdtr   = (volatile uint8_t*) (trampoline_addr + ((uint64_t)&smp_trampoline_gdtr - (uint64_t)_trampoline_start));

    uint64_t current_cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(current_cr4));
    uint64_t current_efer = cpu_read_msr(0xC0000080);
    uint64_t apic_msr = cpu_read_msr(0x1B);
    uint32_t x2apic_on = (apic_msr & (1ULL << 10)) ? 1 : 0;

    *ptr_cr3    = boot_cr3;      
    *ptr_cr4    = current_cr4;
    *ptr_efer   = current_efer;
    *ptr_entry  = (uint64_t)&ap_kernel_entry;
    *ptr_x2flag = x2apic_on;

    uint32_t gdt_phys_addr = (uint32_t)(trampoline_addr + ((uint64_t)&smp_trampoline_gdt_start - (uint64_t)_trampoline_start));
    *(volatile uint32_t*)(ptr_gdtr + 2) = gdt_phys_addr;

    __asm__ volatile("mfence; wbinvd" ::: "memory");

    uint32_t sipi_vec = (uint32_t)(trampoline_addr >> 12);

    for (uint32_t i = 0; i < g_cpu_count; i++) {
        if (g_cpus[i].is_bsp) continue;

        SmpCpuInfo *cpu = &g_cpus[i];
        
        serial_write_all("   -> Waking APIC ");
        smp_print_dec(cpu->apic_id);
        serial_write_all("... ");

        cpu->state = CPU_STATE_PREPARE;
        
        
        
        
        
        
        uint64_t stack_phys = cpu->stack_top & ~HHDM_OFFSET;
        *ptr_stack = stack_phys;
        
        serial_write_all("StkPhys: ");
        smp_print_hex(stack_phys);
        serial_write_all(" ... ");

        __asm__ volatile("mfence" ::: "memory");

        lapic_send_init(cpu->apic_id);
        lapic_send_sipi(cpu->apic_id, sipi_vec);

        int timeout = 10000000; 
        bool online = false;
        
        while (timeout > 0) {
            if (cpu->state == CPU_STATE_ONLINE) {
                online = true;
                break;
            }
            __asm__ volatile("pause");
            timeout--;
        }

        if (online) {
            serial_write_all("SUCCESS (Online)\n");
        } else {
            serial_write_all("Retry SIPI... ");
            lapic_send_sipi(cpu->apic_id, sipi_vec);
            
            timeout = 10000000;
            while (timeout > 0) {
                if (cpu->state == CPU_STATE_ONLINE) {
                    online = true;
                    break;
                }
                __asm__ volatile("pause");
                timeout--;
            }
            
            if (online) {
                serial_write_all("SUCCESS\n");
            } else {
                cpu->state = CPU_STATE_FAILED;
                serial_write_all("FAILED\n");
            }
        }
    }
    serial_write_all("[SMP] Boot sequence finished.\n");
}