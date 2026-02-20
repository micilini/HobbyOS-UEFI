#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include <stdint.h>

typedef struct
{
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} InterruptFrame;

__attribute__((interrupt)) void exc_divide_by_zero(InterruptFrame *frame);
__attribute__((interrupt)) void exc_page_fault(InterruptFrame *frame, uint64_t error_code);
__attribute__((interrupt)) void exc_general_protection(InterruptFrame *frame, uint64_t error_code);

__attribute__((interrupt)) void exc_isr0(InterruptFrame *frame);
__attribute__((interrupt)) void exc_isr1(InterruptFrame *frame);
__attribute__((interrupt)) void exc_isr2(InterruptFrame *frame);
__attribute__((interrupt)) void exc_isr3(InterruptFrame *frame);
__attribute__((interrupt)) void exc_isr4(InterruptFrame *frame);
__attribute__((interrupt)) void exc_isr5(InterruptFrame *frame);
__attribute__((interrupt)) void exc_isr6(InterruptFrame *frame);
__attribute__((interrupt)) void exc_isr7(InterruptFrame *frame);
__attribute__((interrupt)) void exc_isr8(InterruptFrame *frame, uint64_t error_code);
__attribute__((interrupt)) void exc_isr9(InterruptFrame *frame);
__attribute__((interrupt)) void exc_isr10(InterruptFrame *frame, uint64_t error_code);
__attribute__((interrupt)) void exc_isr11(InterruptFrame *frame, uint64_t error_code);
__attribute__((interrupt)) void exc_isr12(InterruptFrame *frame, uint64_t error_code);
__attribute__((interrupt)) void exc_isr13(InterruptFrame *frame, uint64_t error_code);
__attribute__((interrupt)) void exc_isr14(InterruptFrame *frame, uint64_t error_code);
__attribute__((interrupt)) void exc_isr15(InterruptFrame *frame);
__attribute__((interrupt)) void exc_isr16(InterruptFrame *frame);
__attribute__((interrupt)) void exc_isr17(InterruptFrame *frame, uint64_t error_code);
__attribute__((interrupt)) void exc_isr18(InterruptFrame *frame);
__attribute__((interrupt)) void exc_isr19(InterruptFrame *frame);
__attribute__((interrupt)) void exc_isr20(InterruptFrame *frame);
__attribute__((interrupt)) void exc_isr21(InterruptFrame *frame, uint64_t error_code);
__attribute__((interrupt)) void exc_isr22(InterruptFrame *frame);
__attribute__((interrupt)) void exc_isr23(InterruptFrame *frame);
__attribute__((interrupt)) void exc_isr24(InterruptFrame *frame);
__attribute__((interrupt)) void exc_isr25(InterruptFrame *frame);
__attribute__((interrupt)) void exc_isr26(InterruptFrame *frame);
__attribute__((interrupt)) void exc_isr27(InterruptFrame *frame);
__attribute__((interrupt)) void exc_isr28(InterruptFrame *frame);
__attribute__((interrupt)) void exc_isr29(InterruptFrame *frame, uint64_t error_code);
__attribute__((interrupt)) void exc_isr30(InterruptFrame *frame, uint64_t error_code);
__attribute__((interrupt)) void exc_isr31(InterruptFrame *frame);

__attribute__((interrupt)) void exc_generic_handler(InterruptFrame *frame);
__attribute__((interrupt)) void exc_generic_handler_err(InterruptFrame *frame, uint64_t error_code);

__attribute__((interrupt)) void irq_keyboard_handler(InterruptFrame *frame);
__attribute__((interrupt)) void irq_timer_handler(InterruptFrame *frame);
__attribute__((interrupt)) void irq_xhci_handler(InterruptFrame *frame);

__attribute__((interrupt)) void irq_unhandled_handler(InterruptFrame *frame);
__attribute__((interrupt)) void irq_spurious_handler(InterruptFrame *frame);

__attribute__((interrupt)) void irq_halt_handler(InterruptFrame *frame);

#endif