#include "gdt.h"


static GdtTable g_gdt;
static GdtPtr g_gdtr;


static void gdt_set_gate(GdtEntry* entry, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    entry->base_low    = (base & 0xFFFF);
    entry->base_middle = (base >> 16) & 0xFF;
    entry->base_high   = (base >> 24) & 0xFF;

    entry->limit_low   = (limit & 0xFFFF);
    entry->granularity = (limit >> 16) & 0x0F;
    
    entry->granularity |= gran & 0xF0;
    entry->access      = access;
}



void gdt_flush(uint64_t gdtr_addr) {
    __asm__ volatile (
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
        : "r" (gdtr_addr) 
        : "rax", "memory"
    );
}

void init_gdt() {
    
    
    
    gdt_set_gate(&g_gdt.null, 0, 0, 0, 0);

    
    
    
    gdt_set_gate(&g_gdt.kernel_code, 0, 0, 0x9A, 0xA0);

    
    
    
    gdt_set_gate(&g_gdt.kernel_data, 0, 0, 0x92, 0xA0);

    
    
    gdt_set_gate(&g_gdt.user_code, 0, 0, 0xFA, 0xA0);

    
    
    gdt_set_gate(&g_gdt.user_data, 0, 0, 0xF2, 0xA0);

    
    g_gdtr.size = sizeof(GdtTable) - 1;
    g_gdtr.offset = (uint64_t)&g_gdt;

    
    gdt_flush((uint64_t)&g_gdtr);
}