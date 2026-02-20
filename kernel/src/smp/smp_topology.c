#include "smp_topology.h"
#include "smp_boot.h"
#include "../acpi/madt.h"
#include "../drivers/serial.h" 
#include "../apic/lapic.h"
#include "../memory/heap.h"
#include "../cpu/tss.h"
#include "../memory/gdt.h"
#include "../libc/string.h"
#include "../memory/paging.h"


#define HHDM_OFFSET 0xFFFFFFFF80000000ULL
#define PAGE_SIZE 4096

SmpCpuInfo g_cpus[MAX_CPUS];
uint32_t g_cpu_count = 0;
uint8_t g_bsp_apic_id = 0;


static uint64_t align_down(uint64_t addr) {
    return addr & ~(PAGE_SIZE - 1);
}


static uint64_t align_up(uint64_t addr) {
    return (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}


static void* map_and_convert(void* phys_ptr, size_t size) {
    uint64_t phys_addr = (uint64_t)phys_ptr;
    
    
    if (phys_addr >= HHDM_OFFSET) return phys_ptr;

    uint64_t virt_addr = phys_addr | HHDM_OFFSET;

    
    uint64_t start_page = align_down(virt_addr);
    uint64_t end_page   = align_up(virt_addr + size);
    uint64_t pages_count = (end_page - start_page) / PAGE_SIZE;

    
    for (uint64_t i = 0; i < pages_count; i++) {
        uint64_t v = start_page + (i * PAGE_SIZE);
        uint64_t p = v & ~HHDM_OFFSET; 
        
        
        paging_map(v, p, 0x03); 
    }

    
    __asm__ volatile("invlpg (%0)" :: "r"(virt_addr) : "memory");

    return (void*)virt_addr;
}

static void serial_print_hex(uint64_t n) {
    serial_write_all("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t nibble = (n >> i) & 0xF;
        char c = (nibble < 10) ? ('0' + nibble) : ('A' + (nibble - 10));
        serial_putc_all(c);
    }
}

static void serial_print_dec(uint32_t n) {
    if (n == 0) { serial_write_all("0"); return; }
    char buffer[32];
    int i = 0;
    while (n > 0) { buffer[i++] = '0' + (n % 10); n /= 10; }
    for (int j = i - 1; j >= 0; j--) { serial_putc_all(buffer[j]); }
}

void smp_topology_init() {
    serial_write_all("[SMP] Init Topology...\n");
    g_bsp_apic_id = (uint8_t)lapic_get_id(); 

    serial_write_all("[SMP] BSP APIC ID = ");
    serial_print_dec((uint32_t)g_bsp_apic_id);
    serial_write_all("\n");
    
    uint32_t madt_count = madt_get_cpu_count();
    if (madt_count > MAX_CPUS) {
        serial_write_all("[SMP] Limiting CPU count to MAX_CPUS\n");
        madt_count = MAX_CPUS;
    }

    uint8_t apic_ids[MAX_CPUS];
    g_cpu_count = madt_get_cpu_apic_ids(apic_ids, MAX_CPUS);
    
    serial_write_all("[SMP] Scanning CPUs...\n");
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        g_cpus[i].apic_id = apic_ids[i];
        
        
        if (g_cpus[i].apic_id == g_bsp_apic_id) {
            g_cpus[i].is_bsp = true;
            g_cpus[i].state = CPU_STATE_ONLINE; 
        } else {
            g_cpus[i].is_bsp = false;
            g_cpus[i].state = CPU_STATE_DEAD;   
        }
    }
    serial_write_all("[SMP] Topology Done.\n");
}

void smp_prepare_cpu_structures() {
    serial_write_all("[SMP] Allocating CPU Structs (w/ Mapping)...\n");
    uint32_t stack_size = 16 * 1024; 

    for (uint32_t i = 0; i < g_cpu_count; i++) {
        serial_write_all("   -> Index ");
        serial_print_dec(i);
        
        if (g_cpus[i].is_bsp) {
            serial_write_all(" [BSP] SKIP\n");
            continue; 
        }
        serial_write_all(" [AP] Allocating...\n");
        
        
        void *stack_phys = kmalloc(stack_size);
        if (!stack_phys) { serial_write_all("FAIL (kmalloc)\n"); continue; }
        
        void *stack_virt_base = map_and_convert(stack_phys, stack_size);
        g_cpus[i].stack_top = (uint64_t)stack_virt_base + stack_size;
        
        serial_write_all("OK (Virt: ");
        serial_print_hex(g_cpus[i].stack_top);
        serial_write_all(")\n");

        
        serial_write_all("      [2] TSS... ");
        void *tss_phys = kmalloc(sizeof(Tss64)); 
        if (!tss_phys) { serial_write_all("FAIL (kmalloc)\n"); continue; }
        
        Tss64 *tss_virt = (Tss64*)map_and_convert(tss_phys, sizeof(Tss64));
        
        serial_write_all("OK (Virt: ");
        serial_print_hex((uint64_t)tss_virt);
        serial_write_all(")\n");

        serial_write_all("      [2.1] Memset TSS... ");
        memset(tss_virt, 0, sizeof(Tss64)); 
        serial_write_all("OK\n");
        
        tss_virt->rsp0 = g_cpus[i].stack_top;
        g_cpus[i].tss_ptr = tss_virt;

        
        serial_write_all("      [3] GDT... ");
        void *gdt_phys = kmalloc(sizeof(GdtTable));
        if (!gdt_phys) { serial_write_all("FAIL (kmalloc)\n"); continue; }

        GdtTable *gdt_virt = (GdtTable*)map_and_convert(gdt_phys, sizeof(GdtTable));
        
        extern GdtTable g_gdt;
        memcpy(gdt_virt, &g_gdt, sizeof(GdtTable));

        
        uint64_t tss_base = (uint64_t)tss_virt;
        uint32_t tss_limit = sizeof(Tss64) - 1;
        
        TssDesc64 *tss_desc = &gdt_virt->tss;
        tss_desc->limit_low = (uint16_t)(tss_limit & 0xFFFF);
        tss_desc->base_low = (uint16_t)(tss_base & 0xFFFF);
        tss_desc->base_mid1 = (uint8_t)((tss_base >> 16) & 0xFF);
        tss_desc->access = 0x89; 
        tss_desc->gran = (uint8_t)((tss_limit >> 16) & 0x0F);
        tss_desc->base_mid2 = (uint8_t)((tss_base >> 24) & 0xFF);
        tss_desc->base_high = (uint32_t)((tss_base >> 32) & 0xFFFFFFFF);
        tss_desc->reserved = 0;

        g_cpus[i].gdt_ptr = gdt_virt;
        
        serial_write_all("OK (Virt: ");
        serial_print_hex((uint64_t)gdt_virt);
        serial_write_all(")\n");
    }
    serial_write_all("[SMP] All structs allocated & Mapped.\n");
}