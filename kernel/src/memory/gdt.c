#include "gdt.h"
#include "../cpu/tss.h"
#include "../graphics/console.h"
#include "../libc/memory.h"
#include "../memory/heap.h"
#include "../drivers/serial.h" 

GdtTable g_gdt;
static GdtPtr g_gdtr;

#define HHDM_OFFSET 0xFFFFFFFF80000000ULL

static void* to_higher_half(void* ptr) {
    uint64_t addr = (uint64_t)ptr;
    
    if (addr < 0x100000000ULL && addr > 0) {
        return (void*)(addr | HHDM_OFFSET);
    }
    return ptr;
}

static void serial_print_hex(uint64_t n) {
    serial_write_all("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t nibble = (n >> i) & 0xF;
        char c = (nibble < 10) ? ('0' + nibble) : ('A' + (nibble - 10));
        serial_putc_all(c);
    }
}

static void gdt_set_gate(GdtEntry *entry, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
    entry->base_low = (base & 0xFFFF);
    entry->base_middle = (base >> 16) & 0xFF;
    entry->base_high = (base >> 24) & 0xFF;

    entry->limit_low = (limit & 0xFFFF);
    entry->granularity = (limit >> 16) & 0x0F;

    entry->granularity |= gran & 0xF0;
    entry->access = access;
}

static void gdt_set_tss(TssDesc64 *desc, uint64_t base, uint32_t limit)
{
    desc->limit_low = (uint16_t)(limit & 0xFFFF);
    desc->base_low = (uint16_t)(base & 0xFFFF);
    desc->base_mid1 = (uint8_t)((base >> 16) & 0xFF);
    desc->access = 0x89; 
    desc->gran = (uint8_t)((limit >> 16) & 0x0F);
    desc->base_mid2 = (uint8_t)((base >> 24) & 0xFF);
    desc->base_high = (uint32_t)((base >> 32) & 0xFFFFFFFF);
    desc->reserved = 0;
}

static void tss_load(void)
{
    __asm__ volatile(
        "movw %[sel], %%ax\n\t"
        "ltr %%ax\n\t"
        :
        : [sel] "i"(GDT_TSS_SEL)
        : "rax", "memory");
}

void gdt_flush(uint64_t gdtr_addr)
{
    __asm__ volatile(
        "lgdt (%0)\n\t"
        "pushq $0x08\n\t"
        "leaq reload_cs(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "reload_cs:\n\t"
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        :
        : "r"(gdtr_addr)
        : "rax", "memory");
}

void init_gdt()
{
    gdt_set_gate(&g_gdt.null, 0, 0, 0, 0);
    gdt_set_gate(&g_gdt.kernel_code, 0, 0, 0x9A, 0xA0);
    gdt_set_gate(&g_gdt.kernel_data, 0, 0, 0x92, 0xA0);
    gdt_set_gate(&g_gdt.user_code, 0, 0, 0xFA, 0xA0);
    gdt_set_gate(&g_gdt.user_data, 0, 0, 0xF2, 0xA0);

    g_gdtr.size = sizeof(GdtTable) - 1;
    g_gdtr.offset = (uint64_t)&g_gdt;

    tss_init();
    uint64_t tss_base = (uint64_t)tss_get();
    uint32_t tss_limit = (uint32_t)(sizeof(Tss64) - 1);
    gdt_set_tss(&g_gdt.tss, tss_base, tss_limit);

    gdt_flush((uint64_t)&g_gdtr);
    tss_load();
}

GdtTable* gdt_create_per_cpu(void *tss_ptr)
{
    serial_write_all("      [GDT] Allocating... ");
    
    
    void *phys_ptr = kmalloc(sizeof(GdtTable));
    if (!phys_ptr) { 
        serial_write_all("FAIL (kmalloc)\n"); 
        return NULL; 
    }

    
    GdtTable *new_gdt = (GdtTable *)to_higher_half(phys_ptr);
    
    serial_write_all("OK. Copying from global... ");
    
    
    memcpy(new_gdt, &g_gdt, sizeof(GdtTable));
    
    serial_write_all("OK. Setting TSS... ");

    uint64_t tss_base = (uint64_t)tss_ptr;
    uint32_t tss_limit = (uint32_t)(sizeof(Tss64) - 1);
    
    gdt_set_tss(&new_gdt->tss, tss_base, tss_limit);
    
    serial_write_all("Done.\n");

    return new_gdt;
}