#include "idt.h"
#include "interrupts.h"
#include "../libc/memory.h"

static IdtEntry g_idt[IDT_ENTRIES];
static IdtPtr g_idtr;

static void set_idt_gate_ex(int vector, void *handler, uint8_t type, uint8_t ist_index)
{
    uint64_t offset = (uint64_t)handler;

    g_idt[vector].offset_low = (uint16_t)(offset & 0xFFFF);
    g_idt[vector].selector = 0x08;
    g_idt[vector].ist = (ist_index & 0x7);
    g_idt[vector].type_attributes = type;
    g_idt[vector].offset_mid = (uint16_t)((offset >> 16) & 0xFFFF);
    g_idt[vector].offset_high = (uint32_t)((offset >> 32) & 0xFFFFFFFF);
    g_idt[vector].reserved = 0;
}

static void set_idt_gate(int vector, void *handler, uint8_t type)
{
    uint64_t offset = (uint64_t)handler;

    g_idt[vector].offset_low = (uint16_t)(offset & 0xFFFF);
    g_idt[vector].selector = 0x08;
    g_idt[vector].ist = 0;
    g_idt[vector].type_attributes = type;
    g_idt[vector].offset_mid = (uint16_t)((offset >> 16) & 0xFFFF);
    g_idt[vector].offset_high = (uint32_t)((offset >> 32) & 0xFFFFFFFF);
    g_idt[vector].reserved = 0;
}

void init_idt()
{
    memset(&g_idt, 0, sizeof(g_idt));

    set_idt_gate_ex(0, exc_isr0, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(1, exc_isr1, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(2, exc_isr2, IDT_TA_INTERRUPT_GATE, 2);
    set_idt_gate_ex(3, exc_isr3, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(4, exc_isr4, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(5, exc_isr5, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(6, exc_isr6, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(7, exc_isr7, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(8, exc_isr8, IDT_TA_INTERRUPT_GATE, 1);
    set_idt_gate_ex(9, exc_isr9, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(10, exc_isr10, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(11, exc_isr11, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(12, exc_isr12, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(13, exc_isr13, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(14, exc_isr14, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(15, exc_isr15, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(16, exc_isr16, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(17, exc_isr17, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(18, exc_isr18, IDT_TA_INTERRUPT_GATE, 3);
    set_idt_gate_ex(19, exc_isr19, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(20, exc_isr20, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(21, exc_isr21, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(22, exc_isr22, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(23, exc_isr23, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(24, exc_isr24, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(25, exc_isr25, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(26, exc_isr26, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(27, exc_isr27, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(28, exc_isr28, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(29, exc_isr29, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(30, exc_isr30, IDT_TA_INTERRUPT_GATE, 0);
    set_idt_gate_ex(31, exc_isr31, IDT_TA_INTERRUPT_GATE, 0);

    for (int v = 32; v < IDT_ENTRIES; v++)
    {
        set_idt_gate(v, irq_unhandled_handler, IDT_TA_INTERRUPT_GATE);
    }

    set_idt_gate(INT_VECTOR_TIMER, irq_timer_entry, IDT_TA_INTERRUPT_GATE);
    set_idt_gate(INT_VECTOR_KEYBOARD, irq_keyboard_handler, IDT_TA_INTERRUPT_GATE);
    set_idt_gate(INT_VECTOR_XHCI, irq_xhci_handler, IDT_TA_INTERRUPT_GATE);

    set_idt_gate(0xFF, irq_spurious_handler, IDT_TA_INTERRUPT_GATE);

    // Vetor 0xFD reservado para SMP Halt (Pânico)
    set_idt_gate(0xFD, irq_halt_handler, IDT_TA_INTERRUPT_GATE);

    g_idtr.limit = (sizeof(IdtEntry) * IDT_ENTRIES) - 1;
    g_idtr.base = (uint64_t)&g_idt;

    __asm__ volatile("lidt %0" : : "m"(g_idtr));
}

void idt_load()
{
    __asm__ volatile("lidt %0" : : "m"(g_idtr));
}

void enable_interrupts()
{
    __asm__ volatile("sti\n\t" ::: "memory");
}

void disable_interrupts()
{
    __asm__ volatile("cli\n\t" ::: "memory");
}

irq_flags_t irq_save(void)
{
    irq_flags_t flags;
    __asm__ volatile(
        "pushfq\n\t"
        "popq %0\n\t"
        "cli\n\t"
        : "=r"(flags)
        :
        : "memory");
    return flags;
}

void irq_restore(irq_flags_t flags)
{
    __asm__ volatile(
        "pushq %0\n\t"
        "popfq\n\t"
        :
        : "r"(flags)
        : "memory", "cc");
}