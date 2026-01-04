#include "timer.h"
#include "../timer/hpet.h"
#include "../apic/lapic.h" 
#include "../core/shell.h" 
#include "../graphics/console.h" 


static uint64_t g_ticks = 0;
static const uint64_t TICK_MS = 50; 


static int g_cursor_blink_counter = 0;
static const int CURSOR_BLINK_THRESHOLD = 10; 

void timer_init() {
    g_ticks = 0;
    
    hpet_set_timer(TICK_MS);
}

uint64_t timer_get_uptime_ms() {
    return g_ticks * TICK_MS;
}

void timer_handler() {
    
    g_ticks++;

    
    g_cursor_blink_counter++;
    if (g_cursor_blink_counter >= CURSOR_BLINK_THRESHOLD) {
        shell_on_tick(); 
        g_cursor_blink_counter = 0;
    }

    
    
    
    hpet_set_timer(TICK_MS);

    
    lapic_send_eoi();
}