#include "hpet.h"
#include "../acpi/acpi.h"
#include "../cpu/mmio.h"
#include "../apic/ioapic.h"
#include "../memory/paging.h"
#include "../memory/paging.h"
#include "../core/spinlock.h"

static uint64_t g_hpet_base = 0;
static uint64_t g_clk_period_fs = 0;
static int g_hpet_timer0_irq = -1;

static spinlock_t g_hpet_lock;

static int g_hpet_counter_is_64bit = 0;
static uint32_t g_hpet_last_low = 0;
static uint64_t g_hpet_high = 0;
static int g_hpet_ext_inited = 0;

static inline uint64_t hpet_read_main_counter_64(void)
{
    if (!g_hpet_base)
        return 0;

    if (g_hpet_counter_is_64bit)
    {
        return mmio_read64((void *)(g_hpet_base + HPET_REG_MAIN_COUNTER));
    }

    irq_flags_t flags = spin_lock_irqsave(&g_hpet_lock);

    uint32_t low = mmio_read32((void *)(g_hpet_base + HPET_REG_MAIN_COUNTER));

    if (!g_hpet_ext_inited)
    {
        g_hpet_last_low = low;
        g_hpet_high = 0;
        g_hpet_ext_inited = 1;
    }
    else
    {
        if (low < g_hpet_last_low)
        {
            g_hpet_high += (1ULL << 32);
        }
        g_hpet_last_low = low;
    }

    uint64_t ret = g_hpet_high | (uint64_t)low;

    spin_unlock_irqrestore(&g_hpet_lock, flags);
    return ret;
}

static uint64_t hpet_read(uint64_t offset)
{
    return mmio_read64((void *)(g_hpet_base + offset));
}

static void hpet_write(uint64_t offset, uint64_t val)
{
    mmio_write64((void *)(g_hpet_base + offset), val);
}

void init_hpet()
{
    spinlock_init(&g_hpet_lock);

    HpetTable *hpet = (HpetTable *)acpi_find_table(ACPI_SIG_HPET);
    if (!hpet)
        return;

    g_hpet_base = hpet->address;
    if (!g_hpet_base)
        return;

    uint64_t base_page = g_hpet_base & ~0xFFFULL;
    paging_map(base_page, base_page, PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);
    __asm__ volatile("invlpg (%0)" ::"r"(base_page) : "memory");

    uint64_t caps = mmio_read64((void *)(g_hpet_base + HPET_REG_CAPABILITIES));

    g_hpet_counter_is_64bit = ((caps & (1ULL << 13)) != 0);

    g_clk_period_fs = caps >> 32;
    if (g_clk_period_fs == 0)
        g_clk_period_fs = 10000000;

    g_hpet_last_low = 0;
    g_hpet_high = 0;
    g_hpet_ext_inited = 0;

    uint64_t config = mmio_read64((void *)(g_hpet_base + HPET_REG_CONFIG));
    config |= 1;
    mmio_write64((void *)(g_hpet_base + HPET_REG_CONFIG), config);

    if (g_hpet_counter_is_64bit)
    {
        mmio_write64((void *)(g_hpet_base + HPET_REG_MAIN_COUNTER), 0);
    }
    else
    {
        mmio_write32((void *)(g_hpet_base + HPET_REG_MAIN_COUNTER), 0);
    }
}

static int hpet_pick_irq(uint32_t route_cap)
{
    static const int preferred[] = {
        16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
        8, 9, 10, 11, 12, 13, 14, 15,
        2, 3, 4, 5, 6, 7};

    for (unsigned i = 0; i < sizeof(preferred) / sizeof(preferred[0]); i++)
    {
        int irq = preferred[i];
        if (route_cap & (1u << irq))
            return irq;
    }
    return -1;
}

uint64_t hpet_time_ms(void)
{
    if (!g_hpet_base || g_clk_period_fs == 0)
        return 0;

    uint64_t ticks = hpet_read_main_counter_64();

    uint64_t freq_hz = 1000000000000000ULL / g_clk_period_fs;
    if (freq_hz == 0)
        freq_hz = 1;

    return (ticks * 1000ULL) / freq_hz;
}

void hpet_configure_timer0_irq(uint8_t vector, uint8_t apic_id)
{
    if (!g_hpet_base)
        return;

    uint64_t t0 = hpet_read(HPET_TN_CONFIG_CAP(0));
    uint32_t route_cap = (uint32_t)(t0 >> 32);

    int irq = hpet_pick_irq(route_cap);
    if (irq < 0)
    {
        g_hpet_timer0_irq = -1;
        return;
    }

    {
        irq_flags_t flags = spin_lock_irqsave(&g_hpet_lock);
        g_hpet_timer0_irq = irq;
        spin_unlock_irqrestore(&g_hpet_lock, flags);
    }

    t0 &= ~(0x1FULL << 9);
    t0 |= ((uint64_t)(irq & 0x1F) << 9);

    t0 &= ~(1ULL << 1);

    t0 |= (1ULL << 2);

    t0 &= ~(1ULL << 3);

    hpet_write(HPET_TN_CONFIG_CAP(0), t0);

    ioapic_map_irq((uint8_t)irq, vector, apic_id);

    hpet_write(HPET_REG_INT_STATUS, 1ULL << 0);
}

void hpet_usleep(uint64_t microseconds)
{
    if (!g_hpet_base || g_clk_period_fs == 0)
        return;

    uint64_t start = hpet_read_main_counter_64();
    uint64_t delta_ticks = (microseconds * 1000000000ULL) / g_clk_period_fs;
    if (delta_ticks == 0 && microseconds > 0)
        delta_ticks = 1;

    uint64_t target = start + delta_ticks;

    while (hpet_read_main_counter_64() < target)
    {
        __asm__ volatile("pause");
    }
}

void hpet_set_timer(uint64_t milliseconds)
{
    if (!g_hpet_base || g_clk_period_fs == 0)
        return;

    int need_config = 0;

    {
        irq_flags_t flags = spin_lock_irqsave(&g_hpet_lock);
        need_config = (g_hpet_timer0_irq < 0);
        spin_unlock_irqrestore(&g_hpet_lock, flags);
    }

    if (need_config)
    {
        hpet_configure_timer0_irq(32, 0);
    }

    uint64_t delta_ticks = (milliseconds * 1000000000000ULL) / g_clk_period_fs;
    if (delta_ticks == 0 && milliseconds > 0)
        delta_ticks = 1;

    mmio_write64((void *)(g_hpet_base + HPET_REG_INT_STATUS), 1ULL << 0);

    if (g_hpet_counter_is_64bit)
    {
        uint64_t now = hpet_read_main_counter_64();
        uint64_t target = now + delta_ticks;
        if (target <= now)
            target = now + 1;
        mmio_write64((void *)(g_hpet_base + HPET_TN_COMPARATOR(0)), target);
    }
    else
    {

        uint32_t now_low = mmio_read32((void *)(g_hpet_base + HPET_REG_MAIN_COUNTER));
        uint32_t add = (uint32_t)delta_ticks;
        if (add == 0 && milliseconds > 0)
            add = 1;
        uint32_t target_low = now_low + add;
        mmio_write32((void *)(g_hpet_base + HPET_TN_COMPARATOR(0)), target_low);
    }
}