#ifndef IDT_H
#define IDT_H

#include <stdint.h>
#include <stdbool.h>

#define IDT_ENTRIES 256

#define IDT_TA_INTERRUPT_GATE 0x8E
#define IDT_TA_TRAP_GATE 0x8F

#define INT_VECTOR_HPET_TIMER 32
#define INT_VECTOR_KEYBOARD 33
#define INT_VECTOR_LAPIC_TIMER 34

#define INT_VECTOR_XHCI 64

typedef struct
{
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attributes;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed)) IdtEntry;

typedef struct
{
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) IdtPtr;

typedef uint64_t irq_flags_t;

void init_idt();
void idt_load();
void enable_interrupts();
void disable_interrupts();

irq_flags_t irq_save(void);
void irq_restore(irq_flags_t flags);
bool irq_is_enabled(void);

static inline void irq_disable(void)
{
    disable_interrupts();
}

static inline void irq_enable(void)
{
    enable_interrupts();
}

static inline int irq_are_enabled(void)
{
    irq_flags_t f;
    __asm__ volatile("pushfq; popq %0" : "=r"(f) : : "memory");
    return ((f & (1ULL << 9)) != 0);
}

#endif
