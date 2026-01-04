#include "hpet.h"
#include "../acpi/acpi.h"
#include "../cpu/mmio.h"
#include "../apic/ioapic.h"


static uint64_t g_hpet_base = 0;
static uint64_t g_clk_period_fs = 0; 


static uint64_t hpet_read(uint64_t offset) {
    return mmio_read64((void*)(g_hpet_base + offset));
}

static void hpet_write(uint64_t offset, uint64_t val) {
    mmio_write64((void*)(g_hpet_base + offset), val);
}

void init_hpet() {
    HpetTable* hpet = (HpetTable*)acpi_find_table(ACPI_SIG_HPET);
    if (!hpet) return;

    g_hpet_base = hpet->address;

    uint64_t caps = hpet_read(HPET_REG_CAPABILITIES);
    g_clk_period_fs = caps >> 32;
    if (g_clk_period_fs == 0) g_clk_period_fs = 10000000; 

    
    
    
    uint64_t config = hpet_read(HPET_REG_CONFIG);
    config |= 3; 
    hpet_write(HPET_REG_CONFIG, config);
}

void hpet_usleep(uint64_t microseconds) {
    if (!g_hpet_base) return;
    uint64_t main_counter = hpet_read(HPET_REG_MAIN_COUNTER);
    uint64_t delta_ticks = (microseconds * 1000000000ULL) / g_clk_period_fs;
    uint64_t target = main_counter + delta_ticks;

    while (hpet_read(HPET_REG_MAIN_COUNTER) < target) {
        __asm__ volatile ("pause");
    }
}

void hpet_set_timer(uint64_t milliseconds) {
    if (!g_hpet_base) return;

    
    uint64_t main_counter = hpet_read(HPET_REG_MAIN_COUNTER);
    uint64_t delta_ticks = (milliseconds * 1000000000000ULL) / g_clk_period_fs;
    uint64_t target = main_counter + delta_ticks;

    
    uint64_t t0_config = hpet_read(HPET_TN_CONFIG_CAP(0));
    t0_config |= (1 << 2);  
    t0_config &= ~(1 << 1); 
    hpet_write(HPET_TN_CONFIG_CAP(0), t0_config);

    
    hpet_write(HPET_TN_COMPARATOR(0), target);

    
    
    
    
    ioapic_map_irq(2, 32, 0); 
}