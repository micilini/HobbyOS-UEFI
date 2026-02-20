#include "timer.h"
#include "../timer/hpet.h"
#include "../apic/lapic.h"
#include "../shell/shell.h"
#include "../graphics/console.h"
#include "../drivers/usb/xhci/xhci.h"
#include "../core/scheduler.h"
#include "../core/timers.h"
#include "../core/task.h"

extern void xhci_poll_events(void);

static volatile uint64_t g_ticks = 0;
static const uint64_t TICK_MS = 1;

static uint64_t g_last_processed_tick = 0;

static int g_xhci_poll_counter = 0;
static const int XHCI_POLL_THRESHOLD = 1;

static int g_shell_tick_counter = 0;
static const int SHELL_TICK_DIVIDER = 10;

static void timer_wakeup_cb(void *ctx)
{
    task_t *t = (task_t *)ctx;
    thread_wake(t);
}

void timer_init(void)
{
    g_ticks = 0;
    g_last_processed_tick = 0;
    g_xhci_poll_counter = 0;
    g_shell_tick_counter = 0;

    uint8_t apic_id = (uint8_t)lapic_get_id();
    hpet_configure_timer0_irq(32, apic_id);
    hpet_set_timer(TICK_MS);
}

uint64_t timer_get_uptime_ms(void)
{

    return g_ticks * TICK_MS;
}

void timer_sleep(uint64_t ms)
{

    if (ms == 0)
    {
        schedule_voluntary();
        return;
    }

    task_t *current = get_current_task();

    if (!current)
    {

        uint64_t start = timer_get_uptime_ms();
        while (timer_get_uptime_ms() < start + ms)
        {
            asm volatile("hlt");
        }
        return;
    }

    timers_add(ms, timer_wakeup_cb, current);

    thread_block(NULL, TASK_SLEEPING);
}

void timer_handler(void)
{
    g_ticks++;

    hpet_set_timer(TICK_MS);
}

void timer_run_deferred(void)
{

    if (g_ticks <= g_last_processed_tick)
    {
        return;
    }

    uint64_t delta = g_ticks - g_last_processed_tick;
    g_last_processed_tick = g_ticks;

    g_shell_tick_counter += delta;
    if (g_shell_tick_counter >= SHELL_TICK_DIVIDER)
    {
        shell_on_tick();
        g_shell_tick_counter = 0;
    }

    g_xhci_poll_counter += delta;
    if (g_xhci_poll_counter >= XHCI_POLL_THRESHOLD)
    {

        xhci_poll_events();

        xhci_kbd_repeat_poll();

        g_xhci_poll_counter = 0;
    }
}