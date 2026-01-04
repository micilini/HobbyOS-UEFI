#include "interrupts.h"
#include "panic.h"
#include "../graphics/console.h" 
#include "../drivers/keyboard.h"
#include "../apic/lapic.h" 
#include "../drivers/timer.h"


__attribute__((interrupt)) void exc_divide_by_zero(InterruptFrame* frame) {
    (void)frame; 
    kpanic("EXCEPTION: Divide by Zero (Division Error)");
}



__attribute__((interrupt)) void exc_general_protection(InterruptFrame* frame, uint64_t error_code) {
    (void)frame;
    (void)error_code;
    kpanic("EXCEPTION: General Protection Fault (#GP)");
}



__attribute__((interrupt)) void exc_page_fault(InterruptFrame* frame, uint64_t error_code) {
    
    
    
    
    (void)frame;
    (void)error_code;
    kpanic("EXCEPTION: Page Fault (Memory Access Violation)");
}


__attribute__((interrupt)) void exc_generic_handler(InterruptFrame* frame) {
    (void)frame;
    kpanic("EXCEPTION: Unknown System Interrupt");
}






extern void lapic_send_eoi(); 

__attribute__((interrupt)) void irq_keyboard_handler(InterruptFrame* frame) {
    (void)frame;

    
    keyboard_handle_interrupt();

    
    
    
    lapic_send_eoi(); 
}



__attribute__((interrupt)) void irq_timer_handler(InterruptFrame* frame) {
    (void)frame;
    
    
    
    
    timer_handler();
}