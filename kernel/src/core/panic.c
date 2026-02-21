#include "panic.h"
#include "../graphics/console.h"
#include "idt.h"
#include "../timer/hpet.h"
#include "../power/power.h"
#include "io.h"
#include "spinlock.h"
#include "../drivers/serial.h"
#include "../apic/lapic.h"
#include "interrupts.h"

static spinlock_t g_panic_lock = {0};

volatile uint64_t g_last_mmio_addr = 0;
volatile uint64_t g_last_mmio_value = 0;
volatile uint8_t g_last_mmio_size = 0;
volatile uint8_t g_last_mmio_is_write = 0;

#define COLOR_BSOD_BG 0xFF0000AA
#define COLOR_BSOD_FG 0xFFFFFFFF

static PanicAction g_panic_action = PANIC_ACTION_RESTART;
static uint32_t g_panic_timeout_seconds = 8;

static void panic_force_triple_fault(void)
{

    struct
    {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) idtr = {0, 0};

    __asm__ volatile("lidt %0" ::"m"(idtr));
    __asm__ volatile("int $3");
    while (1)
        __asm__ volatile("hlt");
}

static void panic_force_8042_reset(void)
{

    outb(0x64, 0xFE);
    for (int i = 0; i < 200000; i++)
        io_wait();
}

static void panic_sleep_ms_best_effort(uint64_t ms)
{

    uint64_t t0 = hpet_time_ms();
    hpet_usleep(2000);
    uint64_t t1 = hpet_time_ms();

    if (t1 != t0)
    {

        hpet_usleep(ms * 1000ULL);
        return;
    }

    for (uint64_t m = 0; m < ms; m++)
    {
        for (volatile uint64_t i = 0; i < 2000000ULL; i++)
        {
            __asm__ volatile("pause");
        }
    }
}

static void panic_do_action(void)
{

    if (g_panic_action == PANIC_ACTION_SHUTDOWN)
    {
        power_shutdown();
        while (1)
            __asm__ volatile("hlt");
    }

    if (g_panic_action == PANIC_ACTION_RESTART)
    {
        power_restart();

        panic_force_8042_reset();
        panic_force_triple_fault();
    }

    while (1)
        __asm__ volatile("hlt");
}

static void panic_countdown_and_act(uint32_t seconds)
{
    console_write("\n\n");
    console_write("  The system will be restarted in ");

    uint32_t num_x = 0, num_y = 0;
    console_get_cursor(&num_x, &num_y);

    console_print_dec(seconds);
    console_write(" seconds...\n");

    for (uint32_t s = seconds; s > 0; s--)
    {
        panic_sleep_ms_best_effort(1000);

        uint32_t next = s - 1;

        console_set_cursor(num_x, num_y);
        console_write("   ");
        console_set_cursor(num_x, num_y);
        console_print_dec((uint64_t)next);

        console_write(" ");
        if (next == 1)
        {
            console_write("second...");
        }
        else
        {
            console_write("seconds...");
        }
    }

    panic_do_action();
}

void panic_config(PanicAction action, uint32_t timeout_seconds)
{
    g_panic_action = action;
    g_panic_timeout_seconds = timeout_seconds;

    if (g_panic_timeout_seconds == 0)
        g_panic_timeout_seconds = 1;
}

volatile int g_panic_in_progress = 0;
static volatile int32_t g_panic_owner_cpu = -1;

static void panic_enter_owner_or_halt(void)
{
    irq_disable();

    uint32_t cpu = (uint32_t)lapic_get_id();

    if (__sync_bool_compare_and_swap(&g_panic_in_progress, 0, 1))
    {
        g_panic_owner_cpu = (int32_t)cpu;
        return;
    }

    serial_write_all("[PANIC] Secondary CPU entered panic. Halting. cpu=0x");
    serial_write_hex64_all((uint64_t)cpu);
    serial_write_all("\n");

    __asm__ volatile("cli");
    while (1)
        __asm__ volatile("hlt");
}

void kpanic(const char *message)
{

    panic_enter_owner_or_halt();

    serial_write_all("\n[PANIC] kpanic() owner entered. Stopping other CPUs...\n");

    serial_write_all("[PANIC] Message: ");
    serial_write_all(message ? message : "(null)");
    serial_write_all("\n");

    lapic_send_broadcast_halt();

    graphics_enable_buffering(false);

    irq_flags_t flags = spin_lock_irqsave(&g_panic_lock);

    console_clear(COLOR_BSOD_BG);
    console_set_color(COLOR_BSOD_FG, COLOR_BSOD_BG);

    console_write("\n\n");

    console_write("================================================================\n");
    console_write("                     :( HOBBYOS KERNEL PANIC                    \n");
    console_write("================================================================\n");

    console_write("\n\n");

    console_write("  Error: ");
    console_write(message ? message : "(null)");
    console_write("\n\n");

    console_write("  CPU: ");
    console_print_dec((uint64_t)lapic_get_id());
    console_write("\n");

    serial_write_all("[PANIC] Owner CPU is drawing BSOD and will execute action.\n");

    spin_unlock_irqrestore(&g_panic_lock, flags);

    panic_countdown_and_act(g_panic_timeout_seconds);
}

void kpanic_exception(const char *title, void *frame,
                      uint64_t error_code, int has_error_code,
                      uint64_t cr2, int has_cr2)
{
    kpanic_exception_ex(title, 0xFF, frame, error_code, has_error_code, cr2, has_cr2);
}

void kpanic_exception_ex(const char *title,
                         uint8_t vector,
                         void *frame,
                         uint64_t error_code,
                         int has_error_code,
                         uint64_t cr2,
                         int has_cr2)
{

    panic_enter_owner_or_halt();

    serial_write_all("\n[PANIC] kpanic_exception_ex() owner entered. Stopping other CPUs...\n");

    serial_write_all("[PANIC] Exception Title: ");
    serial_write_all(title ? title : "(null)");
    serial_write_all(" Vector=0x");
    serial_write_hex64_all((uint64_t)vector);
    serial_write_all("\n");

    lapic_send_broadcast_halt();

    graphics_enable_buffering(false);

    irq_flags_t flags = spin_lock_irqsave(&g_panic_lock);

    console_clear(COLOR_BSOD_BG);
    console_set_color(COLOR_BSOD_FG, COLOR_BSOD_BG);

    console_write("\n============================================================\n");
    console_write("                HOBBYOS - KERNEL PANIC\n");
    console_write("============================================================\n\n");

    console_write("EXCEPTION: ");
    console_write(title ? title : "(null)");
    console_write("\n\n");

    console_write("VECTOR: ");
    console_print_dec((uint64_t)vector);
    console_write(" (0x");
    console_print_hex((uint64_t)vector);
    console_write(")\n\n");

    if (frame)
    {
        InterruptFrame *f = (InterruptFrame *)frame;

        console_write("RIP:    0x");
        console_print_hex(f->rip);
        console_write("\n");
        console_write("CS:     0x");
        console_print_hex(f->cs);
        console_write("\n");
        console_write("RFLAGS: 0x");
        console_print_hex(f->rflags);
        console_write("\n");
        console_write("RSP:    0x");
        console_print_hex(f->rsp);
        console_write("\n");
        console_write("SS:     0x");
        console_print_hex(f->ss);
        console_write("\n");

        serial_write_all("[PANIC] RIP=0x");
        serial_write_hex64_all(f->rip);
        serial_write_all(" RSP=0x");
        serial_write_hex64_all(f->rsp);
        serial_write_all(" RFLAGS=0x");
        serial_write_hex64_all(f->rflags);
        serial_write_all(" CS=0x");
        serial_write_hex64_all(f->cs);
        serial_write_all(" SS=0x");
        serial_write_hex64_all(f->ss);
        serial_write_all("\n");
    }
    else
    {
        console_write("Frame: (null)\n");
    }

    if (has_error_code)
    {
        console_write("\nERROR CODE: 0x");
        console_print_hex(error_code);
        console_write(" (");
        console_print_dec((uint64_t)error_code);
        console_write(")\n");
    }

    console_write("CR2:    0x");
    console_print_hex(cr2);
    console_write("\n");

    if (has_cr2)
    {
        console_write("\n#PF DETAILS:\n");
        console_write("  P   (bit0)  = ");
        console_print_dec((error_code >> 0) & 1);
        console_write("  (0=not-present, 1=protection)\n");
        console_write("  W/R (bit1)  = ");
        console_print_dec((error_code >> 1) & 1);
        console_write("  (0=read, 1=write)\n");
        console_write("  U/S (bit2)  = ");
        console_print_dec((error_code >> 2) & 1);
        console_write("  (0=supervisor, 1=user)\n");
        console_write("  RSVD(bit3)  = ");
        console_print_dec((error_code >> 3) & 1);
        console_write("  (1=reserved bit violation)\n");
        console_write("  I/D (bit4)  = ");
        console_print_dec((error_code >> 4) & 1);
        console_write("  (1=instruction fetch)\n");
    }

    if (g_last_mmio_addr != 0)
    {
        console_write("\nLAST MMIO:\n");
        console_write("  addr:  0x");
        console_print_hex(g_last_mmio_addr);
        console_write("\n");
        console_write("  op:    ");
        console_write(g_last_mmio_is_write ? "WRITE" : "READ");
        console_write("\n");
        console_write("  size:  ");
        console_print_dec((uint64_t)g_last_mmio_size);
        console_write(" bytes\n");
        console_write("  value: 0x");
        console_print_hex(g_last_mmio_value);
        console_write("\n");
    }

    {
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        serial_write_all("[PANIC] CR3=0x");
        serial_write_hex64_all(cr3);
        serial_write_all("\n");
    }

    console_write("\nCPU: ");
    console_print_dec((uint64_t)lapic_get_id());
    console_write("\n");

    serial_write_all("[PANIC] Owner CPU is drawing BSOD and will execute action.\n");

    spin_unlock_irqrestore(&g_panic_lock, flags);

    panic_countdown_and_act(g_panic_timeout_seconds);
}