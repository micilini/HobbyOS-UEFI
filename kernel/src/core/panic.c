#include "panic.h"
#include "../graphics/console.h"
#include "idt.h"
#include "../timer/hpet.h"
#include "../power/power.h"
#include "io.h"
#include "spinlock.h"
#include "../drivers/serial.h"
#include "../apic/lapic.h"

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

    // Só o primeiro core que entrar no panic vira "owner".
    if (__sync_bool_compare_and_swap(&g_panic_in_progress, 0, 1))
    {
        g_panic_owner_cpu = (int32_t)cpu;
        return;
    }

    // Core secundário: não tenta pegar locks nem desenhar nada.
    serial_write_all("[PANIC] Secondary CPU entered panic. Halting. cpu=0x");
    serial_write_hex64_all((uint64_t)cpu);
    serial_write_all("\n");

    __asm__ volatile("cli");
    while (1) __asm__ volatile("hlt");
}

static void panic_draw_header(void)
{
    // fundo já está azul escuro via console_clear(0x000022)
    console_set_color(0xFF5555, 0x000022);
    console_write("============================================================\n");
    console_write("                     HOBBYOS KERNEL PANIC                    \n");
    console_write("============================================================\n");
    console_set_color(0xFFFFFF, 0x000022);
}

void kpanic(const char *message)
{
    // Define owner ou para o core imediatamente (SMP-safe).
    panic_enter_owner_or_halt();

    // Apenas o owner chega aqui.
    serial_write_all("\n[PANIC] kpanic() owner entered. Stopping other CPUs...\n");

    // "Stop-the-world" cedo para congelar os outros cores antes de tocar em console/graphics.
    lapic_send_broadcast_halt();

    // Evita flicker / buffer swap durante panic
    graphics_enable_buffering(false);

    // Serializa acesso ao console caso algum caminho ainda tente escrever antes do halt pegar.
    irq_flags_t flags = spin_lock_irqsave(&g_panic_lock);

    console_clear(COLOR_BSOD_BG);

    panic_draw_header();

    console_write("KERNEL PANIC\n\n");
    console_write("Message: ");
    console_write(message ? message : "(null)");
    console_write("\n\n");

    console_write("CPU: ");
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
    // Define owner ou para o core imediatamente (SMP-safe).
    panic_enter_owner_or_halt();

    // Apenas o owner chega aqui.
    serial_write_all("\n[PANIC] kpanic_exception_ex() owner entered. Stopping other CPUs...\n");

    // "Stop-the-world" cedo.
    lapic_send_broadcast_halt();

    graphics_enable_buffering(false);

    irq_flags_t flags = spin_lock_irqsave(&g_panic_lock);

    console_clear(COLOR_BSOD_BG);

    panic_draw_header();

    console_write("EXCEPTION PANIC\n\n");

    if (title)
    {
        console_write("Title: ");
        console_write(title);
        console_write("\n");
    }

    console_write("Vector: ");
    console_print_dec((uint64_t)vector);
    console_write("\n");

    if (has_error_code)
    {
        console_write("Error Code: 0x");
        console_print_hex(error_code);
        console_write("\n");
    }

    if (has_cr2)
    {
        console_write("CR2: 0x");
        console_print_hex(cr2);
        console_write("\n");
    }

    console_write("Frame: 0x");
    console_print_hex((uint64_t)frame);
    console_write("\n");

    console_write("CPU: ");
    console_print_dec((uint64_t)lapic_get_id());
    console_write("\n");

    serial_write_all("[PANIC] Owner CPU is drawing BSOD and will execute action.\n");

    spin_unlock_irqrestore(&g_panic_lock, flags);

    panic_countdown_and_act(g_panic_timeout_seconds);
}