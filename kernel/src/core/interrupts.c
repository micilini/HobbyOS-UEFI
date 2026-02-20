#include <stdint.h>

#include "idt.h"
#include "irq_stats.h"
#include "interrupts.h"
#include "panic.h"
#include "../graphics/console.h"
#include "../drivers/keyboard.h"
#include "../drivers/timer.h"
#include "../apic/lapic.h"
#include "../drivers/serial.h"

static volatile uint8_t g_need_resched = 0;

void interrupts_request_reschedule(void)
{
    g_need_resched = 1;
}

int interrupts_consume_reschedule(void)
{
    if (g_need_resched)
    {
        g_need_resched = 0;
        return 1;
    }
    return 0;
}

static const char *g_exc_names[32] = {
    "0  #DE Divide Error",
    "1  #DB Debug",
    "2  NMI Non-Maskable Interrupt",
    "3  #BP Breakpoint",
    "4  #OF Overflow",
    "5  #BR BOUND Range Exceeded",
    "6  #UD Invalid Opcode",
    "7  #NM Device Not Available",
    "8  #DF Double Fault",
    "9  Coprocessor Segment Overrun (reserved)",
    "10 #TS Invalid TSS",
    "11 #NP Segment Not Present",
    "12 #SS Stack-Segment Fault",
    "13 #GP General Protection",
    "14 #PF Page Fault",
    "15 Reserved",
    "16 #MF x87 Floating-Point Exception",
    "17 #AC Alignment Check",
    "18 #MC Machine Check",
    "19 #XM SIMD Floating-Point Exception",
    "20 #VE Virtualization Exception",
    "21 #CP Control Protection Exception",
    "22 Reserved",
    "23 Reserved",
    "24 Reserved",
    "25 Reserved",
    "26 Reserved",
    "27 Reserved",
    "28 #HV Hypervisor Injection Exception",
    "29 #VC VMM Communication Exception",
    "30 #SX Security Exception",
    "31 Reserved"};

#define EXC_PANIC_NOERR(vec)                                                                \
    do                                                                                      \
    {                                                                                       \
        kpanic_exception_ex(g_exc_names[(vec)], (uint8_t)(vec), (void *)frame, 0, 0, 0, 0); \
    } while (0)

#define EXC_PANIC_ERR(vec)                                                                           \
    do                                                                                               \
    {                                                                                                \
        kpanic_exception_ex(g_exc_names[(vec)], (uint8_t)(vec), (void *)frame, error_code, 1, 0, 0); \
    } while (0)

extern void xhci_handle_interrupt(void);

__attribute__((interrupt)) void exc_divide_by_zero(InterruptFrame *frame)
{
    extern void kpanic_exception(const char *title, void *frame, uint64_t error_code, int has_error_code, uint64_t cr2, int has_cr2);

    uint64_t cr2 = 0;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    kpanic_exception("Divide by Zero (Division Error)", (void *)frame, 0, 0, cr2, 0);
}

__attribute__((interrupt)) void exc_general_protection(InterruptFrame *frame, uint64_t error_code)
{
    extern void kpanic_exception(const char *title, void *frame, uint64_t error_code, int has_error_code, uint64_t cr2, int has_cr2);

    uint64_t cr2 = 0;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    kpanic_exception("General Protection Fault (#GP)", (void *)frame, error_code, 1, cr2, 0);
}

__attribute__((interrupt)) void exc_page_fault(InterruptFrame *frame, uint64_t error_code)
{
    extern void kpanic_exception(const char *title, void *frame, uint64_t error_code, int has_error_code, uint64_t cr2, int has_cr2);

    uint64_t cr2 = 0;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    kpanic_exception("Page Fault (#PF)", (void *)frame, error_code, 1, cr2, 1);
}

__attribute__((interrupt)) void exc_generic_handler(InterruptFrame *frame)
{
    extern void kpanic_exception(const char *title, void *frame, uint64_t error_code, int has_error_code, uint64_t cr2, int has_cr2);

    uint64_t cr2 = 0;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    kpanic_exception("Unknown Exception", (void *)frame, 0, 0, cr2, 0);
}

__attribute__((interrupt)) void exc_generic_handler_err(InterruptFrame *frame, uint64_t error_code)
{
    (void)frame;
    (void)error_code;
    kpanic("EXCEPTION: Unhandled CPU Exception (with error code)");
}

__attribute__((interrupt)) void irq_unhandled_handler(InterruptFrame *frame)
{
    (void)frame;
    irq_stats_record_unhandled();
    lapic_eoi();
}

__attribute__((interrupt)) void irq_spurious_handler(InterruptFrame *frame)
{
    (void)frame;

    irq_stats_record(0xFF);
}

__attribute__((interrupt)) void irq_keyboard_handler(InterruptFrame *frame)
{
    (void)frame;
    irq_stats_record(INT_VECTOR_KEYBOARD);
    keyboard_handle_interrupt();
    lapic_eoi();
}

__attribute__((interrupt)) void irq_timer_handler(InterruptFrame *frame)
{
    (void)frame;

    extern volatile int g_panic_in_progress;
    if (g_panic_in_progress)
    {
        lapic_eoi();
        __asm__ volatile("cli; hlt");
        return;
    }

    uint32_t id = lapic_get_id();
    irq_stats_record(INT_VECTOR_TIMER);

    // EOI cedo
    lapic_eoi();

    // O "timer_handler()" você ainda pode manter só no BSP, como já faz.
    if (id == 0)
        timer_handler();

    // CRÍTICO: NUNCA chame schedule() aqui.
    // Apenas sinalize que precisa de reschedule.
    interrupts_request_reschedule();
}

__attribute__((interrupt)) void irq_xhci_handler(InterruptFrame *frame)
{
    (void)frame;
    irq_stats_record(INT_VECTOR_XHCI);
    xhci_handle_interrupt();
    lapic_eoi();
}

__attribute__((interrupt)) void exc_isr0(InterruptFrame *frame) { EXC_PANIC_NOERR(0); }
__attribute__((interrupt)) void exc_isr1(InterruptFrame *frame) { EXC_PANIC_NOERR(1); }
__attribute__((interrupt)) void exc_isr2(InterruptFrame *frame) { EXC_PANIC_NOERR(2); }
__attribute__((interrupt)) void exc_isr3(InterruptFrame *frame) { EXC_PANIC_NOERR(3); }
__attribute__((interrupt)) void exc_isr4(InterruptFrame *frame) { EXC_PANIC_NOERR(4); }
__attribute__((interrupt)) void exc_isr5(InterruptFrame *frame) { EXC_PANIC_NOERR(5); }
__attribute__((interrupt)) void exc_isr6(InterruptFrame *frame) { EXC_PANIC_NOERR(6); }
__attribute__((interrupt)) void exc_isr7(InterruptFrame *frame) { EXC_PANIC_NOERR(7); }

__attribute__((interrupt)) void exc_isr8(InterruptFrame *frame, uint64_t error_code) { EXC_PANIC_ERR(8); }

__attribute__((interrupt)) void exc_isr9(InterruptFrame *frame) { EXC_PANIC_NOERR(9); }

__attribute__((interrupt)) void exc_isr10(InterruptFrame *frame, uint64_t error_code) { EXC_PANIC_ERR(10); }
__attribute__((interrupt)) void exc_isr11(InterruptFrame *frame, uint64_t error_code) { EXC_PANIC_ERR(11); }
__attribute__((interrupt)) void exc_isr12(InterruptFrame *frame, uint64_t error_code) { EXC_PANIC_ERR(12); }
__attribute__((interrupt)) void exc_isr13(InterruptFrame *frame, uint64_t error_code) { EXC_PANIC_ERR(13); }

__attribute__((interrupt)) void exc_isr14(InterruptFrame *frame, uint64_t error_code)
{
    uint64_t cr2 = 0;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    kpanic_exception_ex(g_exc_names[14], 14, (void *)frame, error_code, 1, cr2, 1);
}

__attribute__((interrupt)) void exc_isr15(InterruptFrame *frame) { EXC_PANIC_NOERR(15); }
__attribute__((interrupt)) void exc_isr16(InterruptFrame *frame) { EXC_PANIC_NOERR(16); }

__attribute__((interrupt)) void exc_isr17(InterruptFrame *frame, uint64_t error_code) { EXC_PANIC_ERR(17); }

__attribute__((interrupt)) void exc_isr18(InterruptFrame *frame) { EXC_PANIC_NOERR(18); }
__attribute__((interrupt)) void exc_isr19(InterruptFrame *frame) { EXC_PANIC_NOERR(19); }
__attribute__((interrupt)) void exc_isr20(InterruptFrame *frame) { EXC_PANIC_NOERR(20); }

__attribute__((interrupt)) void exc_isr21(InterruptFrame *frame, uint64_t error_code) { EXC_PANIC_ERR(21); }

__attribute__((interrupt)) void exc_isr22(InterruptFrame *frame) { EXC_PANIC_NOERR(22); }
__attribute__((interrupt)) void exc_isr23(InterruptFrame *frame) { EXC_PANIC_NOERR(23); }
__attribute__((interrupt)) void exc_isr24(InterruptFrame *frame) { EXC_PANIC_NOERR(24); }
__attribute__((interrupt)) void exc_isr25(InterruptFrame *frame) { EXC_PANIC_NOERR(25); }
__attribute__((interrupt)) void exc_isr26(InterruptFrame *frame) { EXC_PANIC_NOERR(26); }
__attribute__((interrupt)) void exc_isr27(InterruptFrame *frame) { EXC_PANIC_NOERR(27); }
__attribute__((interrupt)) void exc_isr28(InterruptFrame *frame) { EXC_PANIC_NOERR(28); }

__attribute__((interrupt)) void exc_isr29(InterruptFrame *frame, uint64_t error_code) { EXC_PANIC_ERR(29); }
__attribute__((interrupt)) void exc_isr30(InterruptFrame *frame, uint64_t error_code) { EXC_PANIC_ERR(30); }

__attribute__((interrupt)) void exc_isr31(InterruptFrame *frame) { EXC_PANIC_NOERR(31); }

__attribute__((interrupt)) void irq_halt_handler(InterruptFrame *frame) {
    (void)frame;
    lapic_eoi();
    __asm__ volatile("cli");
    while(1) __asm__ volatile("hlt"); // Para o core para sempre
}
