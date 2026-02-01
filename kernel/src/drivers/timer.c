#include "timer.h"
#include "../timer/hpet.h"
#include "../apic/lapic.h"
#include "../shell/shell.h"
#include "../graphics/console.h"


extern void xhci_poll_events(void);


static volatile uint64_t g_ticks = 0;
static const uint64_t TICK_MS = 1;


static uint64_t g_last_processed_tick = 0;


static int g_xhci_poll_counter = 0;
static const int XHCI_POLL_THRESHOLD = 1; 

static int g_shell_tick_counter = 0;
static const int SHELL_TICK_DIVIDER = 10; 



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
    uint64_t start_ticks = g_ticks;
    uint64_t ticks_to_wait = (ms + TICK_MS - 1) / TICK_MS;
    
    if (ticks_to_wait == 0 && ms > 0)
        ticks_to_wait = 1;

    uint64_t target_ticks = start_ticks + ticks_to_wait;

    
    while (g_ticks < target_ticks)
    {
        __asm__ volatile("hlt");
    }
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
        g_xhci_poll_counter = 0;
    }
}