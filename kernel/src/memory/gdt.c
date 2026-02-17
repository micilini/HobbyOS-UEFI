#include "gdt.h"
#include "../cpu/tss.h"
#include "../graphics/console.h"
#include "../libc/memory.h"
#include "../memory/heap.h"

static GdtTable g_gdt;
static GdtPtr g_gdtr;

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
    GdtTable *new_gdt = (GdtTable *)kmalloc(sizeof(GdtTable));
    if (!new_gdt) return NULL;

    
    
    memcpy(new_gdt, &g_gdt, sizeof(GdtTable));

    
    uint64_t tss_base = (uint64_t)tss_ptr;
    uint32_t tss_limit = (uint32_t)(sizeof(Tss64) - 1);
    
    gdt_set_tss(&new_gdt->tss, tss_base, tss_limit);

    return new_gdt;
}