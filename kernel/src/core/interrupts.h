#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include <stdint.h>

typedef struct {
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} InterruptFrame;


__attribute__((interrupt)) void exc_divide_by_zero(InterruptFrame* frame);


__attribute__((interrupt)) void exc_page_fault(InterruptFrame* frame, uint64_t error_code);


__attribute__((interrupt)) void exc_general_protection(InterruptFrame* frame, uint64_t error_code);


__attribute__((interrupt)) void exc_generic_handler(InterruptFrame* frame);



__attribute__((interrupt)) void irq_keyboard_handler(InterruptFrame* frame);


__attribute__((interrupt)) void irq_timer_handler(InterruptFrame* frame);

#endif