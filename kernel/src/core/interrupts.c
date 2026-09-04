#include <stdint.h>

#include "idt.h"
#include "irq_stats.h"
#include "interrupt_context.h"
#include "interrupts.h"
#include "panic.h"
#include "../graphics/console.h"
#include "../drivers/keyboard.h"
#include "../apic/lapic.h"
#include "../apic/ioapic.h"
#include "../drivers/serial.h"
#include "../smp/smp_topology.h"
#include "../drivers/timer.h"
#include "../core/timers.h"
#include "../core/scheduler.h"
#include "../core/clock.h"
#include "../timer/hpet.h"

static volatile uint8_t g_need_resched[HOBBYOS_MAX_CPUS] = {0};

void interrupts_request_reschedule(void)
{
    cpu_slot_t slot = CPU_SLOT_INVALID;
    if (!scheduler_is_started())
        return;
    if (smp_current_cpu_slot(&slot) && slot < HOBBYOS_MAX_CPUS)
        g_need_resched[slot] = 1;
}

int interrupts_consume_reschedule(void)
{
    cpu_slot_t slot = CPU_SLOT_INVALID;
    if (scheduler_is_started() && smp_current_cpu_slot(&slot) &&
        slot < HOBBYOS_MAX_CPUS && g_need_resched[slot])
    {
        g_need_resched[slot] = 0;
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

static void irq_keyboard_handler_inner(void)
{
    irq_stats_record(INT_VECTOR_KEYBOARD);
    keyboard_handle_interrupt();
    lapic_eoi();
}

void irq_hpet_timer_handler_inner(void)
{
    irq_stats_record(INT_VECTOR_HPET_TIMER);
    interrupt_context_mark_unexpected(INT_VECTOR_HPET_TIMER);
    (void)hpet_timer0_quarantine_stray();
    lapic_eoi();
}

void irq_lapic_timer_handler_inner(void)
{
    extern volatile int g_panic_in_progress;
    if (g_panic_in_progress)
    {
        lapic_eoi();
        __asm__ volatile("cli; hlt");
        return;
    }
    uint64_t now_ns = clock_monotonic_ns();
    cpu_slot_t slot = CPU_SLOT_INVALID;
    irq_stats_record(INT_VECTOR_LAPIC_TIMER);
    lapic_timer_record_irq_at(now_ns);
    scheduler_account_time(now_ns);
    if (smp_current_cpu_slot(&slot) && slot == smp_bsp_cpu_slot())
        timer_clockevent_on_lapic_tick(slot, now_ns);
    interrupts_request_reschedule();
    lapic_eoi();
}

static void irq_xhci_handler_inner(void)
{
    irq_stats_record(INT_VECTOR_XHCI);
    xhci_handle_interrupt();
    lapic_eoi();
}

static void irq_runtime_rendezvous_wake_handler_inner(void)
{
    irq_stats_record(INT_VECTOR_RUNTIME_RENDEZVOUS_WAKE);
    lapic_eoi();
}

void irq_external_dispatch(uint64_t vector_value)
{
    uint8_t vector = (uint8_t)vector_value;
    interrupt_context_enter(vector);

    if (vector == INT_VECTOR_HPET_TIMER)
    {
        irq_hpet_timer_handler_inner();
    }
    else if (vector == INT_VECTOR_KEYBOARD)
    {
        irq_keyboard_handler_inner();
    }
    else if (vector == INT_VECTOR_LAPIC_TIMER)
    {
        irq_lapic_timer_handler_inner();
        bool preempt_epilogue = interrupt_context_exit_to_preempt(vector);
        if (preempt_epilogue)
            scheduler_preempt_from_irq();
        return;
    }
    else if (vector == INT_VECTOR_RUNTIME_RENDEZVOUS_WAKE)
    {
        irq_runtime_rendezvous_wake_handler_inner();
    }
    else if (vector == INT_VECTOR_XHCI)
    {
        irq_xhci_handler_inner();
    }
    else if (vector == 0xFFu)
    {
        irq_stats_record(0xFFu);
    }
    else if (vector == 0xFDu)
    {
        irq_stats_record(vector);
        lapic_eoi();
        interrupt_context_exit(vector);
        __asm__ volatile("cli");
        while (1)
            __asm__ volatile("hlt");
    }
    else
    {
        irq_stats_record(vector);
        irq_stats_record_unhandled();
        interrupt_context_mark_unexpected(vector);
        (void)ioapic_disable_vector(vector);
        lapic_eoi();
    }

    interrupt_context_exit(vector);
}

__attribute__((interrupt)) void exc_isr0(InterruptFrame *frame) { EXC_PANIC_NOERR(0); }
__attribute__((interrupt)) void exc_isr1(InterruptFrame *frame) { EXC_PANIC_NOERR(1); }

__attribute__((interrupt)) void exc_isr2(InterruptFrame *frame)
{
    (void)frame;

    extern volatile int g_panic_in_progress;

    if (g_panic_in_progress)
    {
        __asm__ volatile("cli");
        while (1)
            __asm__ volatile("hlt");
    }

    EXC_PANIC_NOERR(2);
}

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
