#include "idt.h"
#include "interrupts.h"
#include "../libc/memory.h"


#define INT_VECTOR_TIMER    32 
#define INT_VECTOR_KEYBOARD 33


static IdtEntry g_idt[IDT_ENTRIES];
static IdtPtr   g_idtr;


static void set_idt_gate(int vector, void* handler, uint8_t type) {
    uint64_t offset = (uint64_t)handler;
    
    g_idt[vector].offset_low  = (uint16_t)(offset & 0xFFFF);
    g_idt[vector].selector    = 0x08; 
    g_idt[vector].ist         = 0;
    g_idt[vector].type_attributes = type;
    g_idt[vector].offset_mid  = (uint16_t)((offset >> 16) & 0xFFFF);
    g_idt[vector].offset_high = (uint32_t)((offset >> 32) & 0xFFFFFFFF);
    g_idt[vector].reserved    = 0;
}

void init_idt() {
    
    memset(&g_idt, 0, sizeof(g_idt));
    
    
    set_idt_gate(0,  exc_divide_by_zero,     IDT_TA_INTERRUPT_GATE);
    set_idt_gate(13, exc_general_protection, IDT_TA_INTERRUPT_GATE);
    set_idt_gate(14, exc_page_fault,         IDT_TA_INTERRUPT_GATE);

    
    set_idt_gate(INT_VECTOR_TIMER,    irq_timer_handler,    IDT_TA_INTERRUPT_GATE);
    set_idt_gate(INT_VECTOR_KEYBOARD, irq_keyboard_handler, IDT_TA_INTERRUPT_GATE);
    
    
    g_idtr.limit = (sizeof(IdtEntry) * IDT_ENTRIES) - 1;
    g_idtr.base  = (uint64_t)&g_idt;

    
    __asm__ volatile("lidt %0" : : "m"(g_idtr));
    
    
    
}

void enable_interrupts() {
    __asm__ volatile("sti");
}

void disable_interrupts() {
    __asm__ volatile("cli");
}