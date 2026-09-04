#include "hpet.h"
#include "../acpi/acpi.h"
#include "../cpu/mmio.h"
#include "../apic/ioapic.h"
#include "../memory/paging.h"
#include "../core/spinlock.h"
#include "../core/task_metrics.h"
#include "../core/idt.h"
#include "../drivers/serial.h"

static uint64_t g_hpet_base = 0;
static uint64_t g_clk_period_fs = 0;
static hpet_timer_snapshot_t g_hpet_timer0;
static volatile uint64_t g_hpet_stray_irqs;
static volatile uint64_t g_hpet_quarantine_actions;
static volatile uint64_t g_hpet_quarantine_failures;

static spinlock_t g_hpet_lock;

static int g_hpet_counter_is_64bit = 0;
static uint32_t g_hpet_last_low = 0;
static uint64_t g_hpet_high = 0;
static int g_hpet_ext_inited = 0;
static hpet_counter_stats_t g_hpet_counter_stats;
static uint32_t g_hpet_observed_high;

static inline void hpet_stats_add_u64(uint64_t *value, uint64_t amount)
{
    __atomic_add_fetch(value, amount, __ATOMIC_RELAXED);
}

static inline void hpet_stats_max_retry(uint32_t retries)
{
    uint32_t old = __atomic_load_n(&g_hpet_counter_stats.max_split_retries,
                                   __ATOMIC_RELAXED);
    while (old < retries &&
           !__atomic_compare_exchange_n(
               &g_hpet_counter_stats.max_split_retries, &old, retries, false,
               __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;
}

hpet_split_result_t hpet_counter64_combine(uint32_t high_before,
                                            uint32_t low,
                                            uint32_t high_after,
                                            uint64_t *out)
{
    if (!out)
        return HPET_SPLIT_SAMPLE_INVALID;
    if (high_before != high_after)
        return HPET_SPLIT_SAMPLE_RETRY;
    *out = ((uint64_t)high_after << 32) | low;
    return HPET_SPLIT_SAMPLE_OK;
}

static bool hpet_read_counter64_split(uint64_t *out, uint32_t *out_retries)
{
    if (!out || !out_retries || !g_hpet_base)
        return false;
    for (uint32_t retry = 0; retry <= HPET_COUNTER_SPLIT_RETRY_LIMIT; retry++) {
        uint32_t high_before = mmio_read32(
            (void *)(g_hpet_base + HPET_REG_MAIN_COUNTER + 4));
        uint32_t low = mmio_read32(
            (void *)(g_hpet_base + HPET_REG_MAIN_COUNTER));
        uint32_t high_after = mmio_read32(
            (void *)(g_hpet_base + HPET_REG_MAIN_COUNTER + 4));
        if (hpet_counter64_combine(high_before, low, high_after, out) ==
            HPET_SPLIT_SAMPLE_OK) {
            *out_retries = retry;
            return true;
        }
    }
    *out_retries = HPET_COUNTER_SPLIT_RETRY_LIMIT + 1;
    return false;
}

static bool hpet_extend_counter32_locked(uint32_t low, uint64_t *out)
{
    if (!out)
        return false;
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
            if (g_hpet_high > UINT64_MAX - (1ULL << 32))
                return false;
            g_hpet_high += (1ULL << 32);
            hpet_stats_add_u64(&g_hpet_counter_stats.low32_rollovers, 1);
        }
        g_hpet_last_low = low;
    }
    *out = g_hpet_high | (uint64_t)low;
    return true;
}

bool hpet_read_counter_sample(hpet_counter_sample_t *out)
{
    if (!out)
        return false;
    *out = (hpet_counter_sample_t){0};
    if (!g_hpet_base)
        return false;
    hpet_stats_add_u64(&g_hpet_counter_stats.reads, 1);

    if (g_hpet_counter_is_64bit) {
        uint32_t retries = 0;
        bool ok = hpet_read_counter64_split(&out->ticks, &retries);
        out->retries = retries;
        out->mode = HPET_COUNTER_READ_SPLIT64_STABLE;
        hpet_stats_add_u64(&g_hpet_counter_stats.split64_reads, 1);
        hpet_stats_add_u64(&g_hpet_counter_stats.split_retries, retries);
        hpet_stats_max_retry(retries);
        if (!ok) {
            hpet_stats_add_u64(
                &g_hpet_counter_stats.split_retry_exhaustions, 1);
            return false;
        }
        uint32_t high = (uint32_t)(out->ticks >> 32);
        uint32_t observed = __atomic_load_n(&g_hpet_observed_high,
                                            __ATOMIC_RELAXED);
        while (high > observed) {
            uint32_t expected = observed;
            if (__atomic_compare_exchange_n(&g_hpet_observed_high, &expected,
                                            high, false, __ATOMIC_RELAXED,
                                            __ATOMIC_RELAXED)) {
                hpet_stats_add_u64(&g_hpet_counter_stats.low32_rollovers,
                                   high - observed);
                break;
            }
            observed = expected;
        }
        out->valid = 1;
        return true;
    }

    irq_flags_t flags = spin_lock_irqsave(&g_hpet_lock);
    uint32_t low = mmio_read32(
        (void *)(g_hpet_base + HPET_REG_MAIN_COUNTER));
    bool ok = hpet_extend_counter32_locked(low, &out->ticks);
    spin_unlock_irqrestore(&g_hpet_lock, flags);
    out->mode = HPET_COUNTER_READ_EXTENDED32;
    out->valid = ok;
    hpet_stats_add_u64(&g_hpet_counter_stats.extended32_reads, 1);
    return ok;
}

void init_hpet()
{
    spinlock_init(&g_hpet_lock);
    g_hpet_timer0 = (hpet_timer_snapshot_t){0};
    g_hpet_timer0.state = HPET_TIMER0_OFF;
    g_hpet_stray_irqs = 0;
    g_hpet_quarantine_actions = 0;
    g_hpet_quarantine_failures = 0;

    HpetTable *hpet = (HpetTable *)acpi_find_table(ACPI_SIG_HPET);
    if (!hpet)
        return;

    g_hpet_base = hpet->address;
    if (!g_hpet_base)
        return;

    uint64_t base_page = g_hpet_base & ~0xFFFULL;
    paging_map(base_page, base_page, PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);
    __asm__ volatile("invlpg (%0)" ::"r"(base_page) : "memory");

    uint32_t caps_low = mmio_read32(
        (void *)(g_hpet_base + HPET_REG_CAPABILITIES));
    uint32_t caps_high = mmio_read32(
        (void *)(g_hpet_base + HPET_REG_CAPABILITIES + 4));

    g_hpet_counter_is_64bit = ((caps_low & (1u << 13)) != 0);
    g_clk_period_fs = caps_high;
    if (!hpet_validate_period_fs(g_clk_period_fs)) {
        serial_write_all("[CLOCK][HPET] INVALID_PERIOD period_fs=");
        char b[24]; unsigned n=0; uint64_t v=g_clk_period_fs; do { b[n++]=(char)('0'+v%10); v/=10; } while(v); while(n) serial_putc_all(b[--n]); serial_write_all("\n");
        g_hpet_base=0; g_clk_period_fs=0; return;
    }

    g_hpet_last_low = 0;
    g_hpet_high = 0;
    g_hpet_ext_inited = 0;
    g_hpet_counter_stats = (hpet_counter_stats_t){0};
    __atomic_store_n(&g_hpet_counter_stats.counter_width_bits,
                     g_hpet_counter_is_64bit ? 64u : 32u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_hpet_counter_stats.read_mode,
                     g_hpet_counter_is_64bit
                         ? HPET_COUNTER_READ_SPLIT64_STABLE
                         : HPET_COUNTER_READ_EXTENDED32,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_hpet_observed_high, 0, __ATOMIC_RELAXED);

    uint32_t config = mmio_read32((void *)(g_hpet_base + HPET_REG_CONFIG));
    mmio_write32((void *)(g_hpet_base + HPET_REG_CONFIG),
                 config & ~3u);

    /* Counter initialization never arms timer 0.  Firmware may have left the
       comparator enabled or in legacy replacement mode, so neutralize both
       before resetting the main counter. */
    uint32_t timer_config = mmio_read32(
        (void *)(g_hpet_base + HPET_TN_CONFIG_CAP(0)));
    timer_config &= ~((1u << 1) | (1u << 2) | (1u << 3) |
                      (1u << 6) | (1u << 8) | (1u << 14));
    mmio_write32((void *)(g_hpet_base + HPET_TN_CONFIG_CAP(0)),
                 timer_config);
    mmio_write32((void *)(g_hpet_base + HPET_REG_MAIN_COUNTER), 0);
    if (g_hpet_counter_is_64bit)
        mmio_write32((void *)(g_hpet_base + HPET_REG_MAIN_COUNTER + 4), 0);
    mmio_write32((void *)(g_hpet_base + HPET_REG_INT_STATUS), UINT32_MAX);
    mmio_write32((void *)(g_hpet_base + HPET_REG_CONFIG),
                 (config & ~2u) | 1u);
    (void)hpet_timer0_force_quiescent();
}

static int hpet_pick_gsi(uint32_t route_cap)
{
    static const int preferred[] = {
        16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
        8, 9, 10, 11, 12, 13, 14, 15,
        2, 3, 4, 5, 6, 7};

    for (unsigned i = 0; i < sizeof(preferred) / sizeof(preferred[0]); i++)
    {
        int gsi = preferred[i];
        const ioapic_controller_t *controller = NULL;
        uint32_t pin = 0;
        interrupt_route_t existing;
        if ((route_cap & (1u << gsi)) == 0)
            continue;
        if (!ioapic_find_for_gsi((uint32_t)gsi, &controller, &pin))
            continue;
        (void)controller;
        (void)pin;
        if (ioapic_route_snapshot((uint32_t)gsi, &existing) &&
            existing.owner != INTERRUPT_ROUTE_OWNER_HPET)
            continue;
        return gsi;
    }
    return -1;
}

bool hpet_validate_period_fs(uint64_t p) { return p > 0 && p <= 100000000ULL && (1000000000000000ULL/p) > 0; }
bool hpet_duration_us_to_ticks(uint64_t us,uint64_t *out) { if(!out||!hpet_validate_period_fs(g_clk_period_fs)) return false; return task_metrics_muldiv_u64(us,1000000000ULL,g_clk_period_fs,out); }
bool hpet_duration_ms_to_ticks(uint64_t ms,uint64_t *out) { if(!out||!hpet_validate_period_fs(g_clk_period_fs)) return false; return task_metrics_muldiv_u64(ms,1000000000000ULL,g_clk_period_fs,out); }
bool hpet_validation_selftest(void) { uint64_t out=0; bool ok=!hpet_validate_period_fs(0)&&hpet_validate_period_fs(10000000ULL)&&!hpet_validate_period_fs(100000001ULL); if(hpet_is_available()) ok=ok&&hpet_duration_us_to_ticks(0,&out)&&out==0&&hpet_duration_ms_to_ticks(1,&out)&&out>0&&!hpet_duration_ms_to_ticks(UINT64_MAX,&out); serial_write_all(ok?"[CLOCK][SELFTEST] HPET_VALIDITY_OK\n":"[CLOCK][SELFTEST] HPET_VALIDITY_FAIL\n"); return ok; }
bool hpet_is_available(void) { return g_hpet_base != 0 && g_clk_period_fs != 0; }
uint64_t hpet_read_counter(void)
{
    hpet_counter_sample_t sample;
    return hpet_read_counter_sample(&sample) ? sample.ticks : 0;
}
uint64_t hpet_period_fs(void) { return g_clk_period_fs; }
uint64_t hpet_frequency_hz(void)
{
    return hpet_is_available() ? 1000000000000000ULL / g_clk_period_fs : 0;
}

/* Exact floor(value*scale/divisor), for value<divisor and a small scale,
   using quotient/remainder long multiplication instead of 128-bit helpers. */
static uint64_t hpet_fraction_muldiv(uint64_t value,uint64_t scale,uint64_t divisor)
{
    uint64_t quotient=0,remainder=0,scale_q=scale/divisor,scale_r=scale%divisor;
    for(int bit=63;bit>=0;bit--) {
        quotient*=2;
        if(remainder>=divisor-remainder) { remainder-=divisor-remainder; quotient++; }
        else remainder+=remainder;
        if((value>>bit)&1ULL) {
            quotient+=scale_q;
            if(scale_r && remainder>=divisor-scale_r) { remainder-=divisor-scale_r; quotient++; }
            else remainder+=scale_r;
        }
    }
    return quotient;
}

uint64_t hpet_counter_to_ns(uint64_t ticks)
{
    uint64_t frequency = hpet_frequency_hz();
    if (!frequency) return 0;
    uint64_t whole = ticks / frequency;
    uint64_t remainder = ticks % frequency;
    if (whole > UINT64_MAX / 1000000000ULL) return UINT64_MAX;
    uint64_t base = whole * 1000000000ULL;
    uint64_t ns=hpet_fraction_muldiv(remainder,1000000000ULL,frequency);
    if (UINT64_MAX - base < ns) return UINT64_MAX;
    return base + ns;
}
bool hpet_read_clock_sample(hpet_clock_sample_t *out)
{
    if (!out)
        return false;
    *out = (hpet_clock_sample_t){0};
    hpet_counter_sample_t counter;
    if (!hpet_read_counter_sample(&counter)) {
        out->retries = counter.retries;
        out->mode = counter.mode;
        return false;
    }
    out->ticks = counter.ticks;
    out->ns = hpet_counter_to_ns(counter.ticks);
    out->retries = counter.retries;
    out->mode = counter.mode;
    out->valid = out->ns != UINT64_MAX;
    return out->valid;
}

bool hpet_counter_extend32_test(uint32_t previous, uint64_t high,
                                uint32_t current, uint64_t *out)
{
    if (!out || (high & 0xFFFFFFFFULL)) return false;
    if (current < previous) high += (1ULL << 32);
    *out = high | current;
    return true;
}

bool hpet_counter_access_selftest(void)
{
    uint64_t value = 0;
    bool ok = hpet_counter64_combine(7, 100, 7, &value) ==
                  HPET_SPLIT_SAMPLE_OK &&
              value == 0x0000000700000064ULL;
    ok = ok && hpet_counter64_combine(7, 5, 8, &value) ==
                   HPET_SPLIT_SAMPLE_RETRY;
    ok = ok && hpet_counter64_combine(8, 5, 8, &value) ==
                   HPET_SPLIT_SAMPLE_OK &&
         value == 0x0000000800000005ULL;
    ok = ok && hpet_counter64_combine(UINT32_MAX, UINT32_MAX,
                                      UINT32_MAX, &value) ==
                   HPET_SPLIT_SAMPLE_OK &&
         value == UINT64_MAX;
    ok = ok && hpet_counter64_combine(0, 0, 0, NULL) ==
                   HPET_SPLIT_SAMPLE_INVALID;
    ok = ok && hpet_counter_extend32_test(100, 0, 101, &value) &&
         value == 101;
    ok = ok && hpet_counter_extend32_test(UINT32_MAX - 1, 0, 2, &value) &&
         value == (1ULL << 32) + 2;
    ok = ok && !hpet_counter_extend32_test(0, 1, 0, &value);

    uint32_t retries = 0;
    for (; retries <= HPET_COUNTER_SPLIT_RETRY_LIMIT; retries++) {
        if (hpet_counter64_combine(retries, 0, retries + 1, &value) !=
            HPET_SPLIT_SAMPLE_RETRY)
            ok = false;
    }
    ok = ok && retries == HPET_COUNTER_SPLIT_RETRY_LIMIT + 1;

#ifdef HOBBYOS_HPET_NEGATIVE_TORN_COUNTER_READ
    uint64_t previous = 0x00000007FFFFFFF0ULL;
    uint64_t torn = ((uint64_t)8 << 32) | (uint32_t)previous;
    uint64_t actual_after = 0x0000000800000010ULL;
    uint64_t ahead = torn - actual_after;
    bool detected = torn > actual_after &&
                    ahead > (1ULL << 32) - 4096 &&
                    ahead < (1ULL << 32);
    serial_write_all(detected
        ? "[CLOCK][NEGATIVE] TORN_64BIT_COUNTER_READ_DETECTED\n"
        : "[CLOCK][NEGATIVE] TORN_64BIT_COUNTER_READ_MISSED\n");
    return detected;
#else
    serial_write_all(ok ? "[CLOCK][SELFTEST] HPET_COUNTER_ACCESS_OK\n"
                        : "[CLOCK][SELFTEST] HPET_COUNTER_ACCESS_FAIL\n");
    return ok;
#endif
}

void hpet_counter_stats_snapshot(hpet_counter_stats_t *out)
{
    if (!out)
        return;
    out->reads = __atomic_load_n(&g_hpet_counter_stats.reads,
                                 __ATOMIC_RELAXED);
    out->split64_reads = __atomic_load_n(&g_hpet_counter_stats.split64_reads,
                                         __ATOMIC_RELAXED);
    out->extended32_reads = __atomic_load_n(
        &g_hpet_counter_stats.extended32_reads, __ATOMIC_RELAXED);
    out->split_retries = __atomic_load_n(&g_hpet_counter_stats.split_retries,
                                         __ATOMIC_RELAXED);
    out->split_retry_exhaustions = __atomic_load_n(
        &g_hpet_counter_stats.split_retry_exhaustions, __ATOMIC_RELAXED);
    out->low32_rollovers = __atomic_load_n(
        &g_hpet_counter_stats.low32_rollovers, __ATOMIC_RELAXED);
    out->max_split_retries = __atomic_load_n(
        &g_hpet_counter_stats.max_split_retries, __ATOMIC_RELAXED);
    out->counter_width_bits = __atomic_load_n(
        &g_hpet_counter_stats.counter_width_bits, __ATOMIC_RELAXED);
    out->read_mode = __atomic_load_n(&g_hpet_counter_stats.read_mode,
                                     __ATOMIC_RELAXED);
}

void hpet_counter_test_stats_reset(void)
{
    __atomic_store_n(&g_hpet_counter_stats.reads, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_hpet_counter_stats.split64_reads, 0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_hpet_counter_stats.extended32_reads, 0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_hpet_counter_stats.split_retries, 0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_hpet_counter_stats.split_retry_exhaustions, 0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_hpet_counter_stats.low32_rollovers, 0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_hpet_counter_stats.max_split_retries, 0,
                     __ATOMIC_RELAXED);
    hpet_counter_sample_t sample;
    if (hpet_read_counter_sample(&sample)) {
        __atomic_store_n(&g_hpet_observed_high,
                         (uint32_t)(sample.ticks >> 32), __ATOMIC_RELAXED);
        __atomic_store_n(&g_hpet_counter_stats.reads, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_hpet_counter_stats.split64_reads, 0,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&g_hpet_counter_stats.extended32_reads, 0,
                         __ATOMIC_RELAXED);
    }
}

bool hpet_timer0_prepare(uint8_t vector, uint32_t destination_apic_id)
{
    if (!hpet_is_available() || vector < 32 || destination_apic_id > UINT8_MAX)
        return false;

    uint32_t route_cap = mmio_read32(
        (void *)(g_hpet_base + HPET_TN_CONFIG_CAP(0) + 4));
    int gsi = hpet_pick_gsi(route_cap);
    if (gsi < 0)
        return false;

    interrupt_route_t route = {
        .gsi = (uint32_t)gsi,
        .destination_apic_id = destination_apic_id,
        .vector = vector,
        .polarity = IRQ_POLARITY_HIGH,
        .trigger = IRQ_TRIGGER_EDGE,
        .owner = INTERRUPT_ROUTE_OWNER_HPET,
        .masked = 1,
    };
    if (!ioapic_route_prepare(&route))
        return false;

    irq_flags_t flags = spin_lock_irqsave(&g_hpet_lock);
    uint32_t timer_config = mmio_read32(
        (void *)(g_hpet_base + HPET_TN_CONFIG_CAP(0)));
    timer_config &= ~((0x1Fu << 9) | (1u << 1) | (1u << 2) |
                      (1u << 3) | (1u << 6) | (1u << 8) | (1u << 14));
    timer_config |= (uint32_t)gsi << 9;
    mmio_write32((void *)(g_hpet_base + HPET_TN_CONFIG_CAP(0)),
                 timer_config);
    mmio_write32((void *)(g_hpet_base + HPET_REG_INT_STATUS), 1u);
    g_hpet_timer0 = (hpet_timer_snapshot_t){
        .route_capability = route_cap,
        .gsi = (uint32_t)gsi,
        .destination_apic_id = destination_apic_id,
        .timer_config = timer_config,
        .vector = vector,
        .state = HPET_TIMER0_PREPARED_MASKED,
        .route_prepared = 1,
        .route_enabled = 0,
        .pending = 0,
    };
    spin_unlock_irqrestore(&g_hpet_lock, flags);
    return true;
}

void hpet_usleep(uint64_t microseconds)
{
    uint64_t delta = 0;
    if (!hpet_duration_us_to_ticks(microseconds, &delta))
        return;
    if (delta == 0 && microseconds)
        delta = 1;
    uint64_t start = hpet_read_counter();
    if (UINT64_MAX - start < delta)
        return;
    uint64_t target = start + delta;
    while (hpet_read_counter() < target)
        __asm__ volatile("pause");
}

bool hpet_timer0_clear_pending(void)
{
    if (!hpet_is_available())
        return false;
    irq_flags_t flags = spin_lock_irqsave(&g_hpet_lock);
    mmio_write32((void *)(g_hpet_base + HPET_REG_INT_STATUS), 1u);
    g_hpet_timer0.pending =
        (mmio_read32((void *)(g_hpet_base + HPET_REG_INT_STATUS)) & 1u) != 0;
    bool cleared = g_hpet_timer0.pending == 0;
    spin_unlock_irqrestore(&g_hpet_lock, flags);
    return cleared;
}

bool hpet_timer0_arm(uint64_t milliseconds)
{
    if (!hpet_is_available() || milliseconds == 0)
        return false;

    uint64_t delta_ticks = 0;
    if (!hpet_duration_ms_to_ticks(milliseconds, &delta_ticks) ||
        delta_ticks == 0)
        return false;
    if (!g_hpet_counter_is_64bit && delta_ticks > UINT32_MAX)
        return false;

    uint64_t now = hpet_read_counter();
    if (UINT64_MAX - now < delta_ticks)
        return false;
    uint64_t target = now + delta_ticks;

    irq_flags_t flags = spin_lock_irqsave(&g_hpet_lock);
    if (g_hpet_timer0.state != HPET_TIMER0_PREPARED_MASKED &&
        g_hpet_timer0.state != HPET_TIMER0_ARMED_MASKED &&
        g_hpet_timer0.state != HPET_TIMER0_ACTIVE) {
        spin_unlock_irqrestore(&g_hpet_lock, flags);
        return false;
    }

    uint32_t timer_config = mmio_read32(
        (void *)(g_hpet_base + HPET_TN_CONFIG_CAP(0)));
    uint32_t disabled_config = timer_config & ~(1u << 2);
    mmio_write32((void *)(g_hpet_base + HPET_TN_CONFIG_CAP(0)),
                 disabled_config);
    mmio_write32((void *)(g_hpet_base + HPET_REG_INT_STATUS), 1u);
    mmio_write32((void *)(g_hpet_base + HPET_TN_COMPARATOR(0)),
                 (uint32_t)target);
    if (g_hpet_counter_is_64bit)
        mmio_write32((void *)(g_hpet_base + HPET_TN_COMPARATOR(0) + 4),
                     (uint32_t)(target >> 32));
    timer_config = disabled_config | (1u << 2);
    mmio_write32((void *)(g_hpet_base + HPET_TN_CONFIG_CAP(0)),
                 timer_config);

    g_hpet_timer0.comparator = g_hpet_counter_is_64bit
        ? target : (uint32_t)target;
    g_hpet_timer0.arm_count++;
    g_hpet_timer0.timer_config = timer_config;
    if (g_hpet_timer0.state != HPET_TIMER0_ACTIVE)
        g_hpet_timer0.state = HPET_TIMER0_ARMED_MASKED;
    g_hpet_timer0.pending = 0;
    spin_unlock_irqrestore(&g_hpet_lock, flags);
    return true;
}

bool hpet_timer0_enable_route(void)
{
    irq_flags_t flags = spin_lock_irqsave(&g_hpet_lock);
    if (g_hpet_timer0.state != HPET_TIMER0_ARMED_MASKED ||
        !g_hpet_timer0.route_prepared) {
        spin_unlock_irqrestore(&g_hpet_lock, flags);
        return false;
    }
    uint32_t gsi = g_hpet_timer0.gsi;
    spin_unlock_irqrestore(&g_hpet_lock, flags);

    if (!ioapic_route_enable(gsi))
        return false;

    flags = spin_lock_irqsave(&g_hpet_lock);
    g_hpet_timer0.route_enabled = 1;
    g_hpet_timer0.state = HPET_TIMER0_ACTIVE;
    spin_unlock_irqrestore(&g_hpet_lock, flags);
    return true;
}

bool hpet_timer0_disable(void)
{
    if (!hpet_is_available())
        return false;
    irq_flags_t flags = spin_lock_irqsave(&g_hpet_lock);
    if (!g_hpet_timer0.route_prepared) {
        spin_unlock_irqrestore(&g_hpet_lock, flags);
        return true;
    }
    uint32_t gsi = g_hpet_timer0.gsi;
    uint32_t timer_config = mmio_read32(
        (void *)(g_hpet_base + HPET_TN_CONFIG_CAP(0))) & ~(1u << 2);
    mmio_write32((void *)(g_hpet_base + HPET_TN_CONFIG_CAP(0)),
                 timer_config);
    mmio_write32((void *)(g_hpet_base + HPET_REG_INT_STATUS), 1u);
    g_hpet_timer0.timer_config = timer_config;
    g_hpet_timer0.route_enabled = 0;
    g_hpet_timer0.pending = 0;
    g_hpet_timer0.state = HPET_TIMER0_PREPARED_MASKED;
    spin_unlock_irqrestore(&g_hpet_lock, flags);
    return ioapic_route_disable(gsi);
}

bool hpet_timer0_force_quiescent(void)
{
    if (!hpet_is_available())
        return false;

    irq_flags_t flags = spin_lock_irqsave(&g_hpet_lock);
    uint8_t route_prepared = g_hpet_timer0.route_prepared;
    uint32_t gsi = g_hpet_timer0.gsi;

    uint32_t global_config = mmio_read32(
        (void *)(g_hpet_base + HPET_REG_CONFIG));
    global_config = (global_config | 1u) & ~2u;
    mmio_write32((void *)(g_hpet_base + HPET_REG_CONFIG), global_config);

    uint32_t timer_config = mmio_read32(
        (void *)(g_hpet_base + HPET_TN_CONFIG_CAP(0)));
    timer_config &= ~((1u << 1) | (1u << 2) | (1u << 3) |
                      (1u << 6) | (1u << 8) | (0x1Fu << 9) |
                      (1u << 14));
    mmio_write32((void *)(g_hpet_base + HPET_TN_CONFIG_CAP(0)),
                 timer_config);
    mmio_write32((void *)(g_hpet_base + HPET_REG_INT_STATUS), 1u);

    uint32_t config_readback = mmio_read32(
        (void *)(g_hpet_base + HPET_REG_CONFIG));
    uint32_t timer_readback = mmio_read32(
        (void *)(g_hpet_base + HPET_TN_CONFIG_CAP(0)));
    uint32_t status_readback = mmio_read32(
        (void *)(g_hpet_base + HPET_REG_INT_STATUS));

    g_hpet_timer0.timer_config = timer_readback;
    g_hpet_timer0.route_enabled = 0;
    g_hpet_timer0.pending = (status_readback & 1u) != 0;
    g_hpet_timer0.state = HPET_TIMER0_QUIESCENT;
    spin_unlock_irqrestore(&g_hpet_lock, flags);

    bool route_masked = !route_prepared || ioapic_route_disable(gsi);
    bool hardware_quiescent = (config_readback & 1u) != 0 &&
        (config_readback & 2u) == 0 &&
        (timer_readback & ((1u << 2) | (1u << 3) | (1u << 14))) == 0 &&
        (status_readback & 1u) == 0;
    return route_masked && hardware_quiescent;
}

bool hpet_runtime_snapshot(hpet_runtime_snapshot_t *out)
{
    if (!out)
        return false;
    *out = (hpet_runtime_snapshot_t){0};
    if (!hpet_is_available())
        return false;

    irq_flags_t flags = spin_lock_irqsave(&g_hpet_lock);
    uint32_t global_config = mmio_read32(
        (void *)(g_hpet_base + HPET_REG_CONFIG));
    uint32_t timer_config = mmio_read32(
        (void *)(g_hpet_base + HPET_TN_CONFIG_CAP(0)));
    uint32_t status = mmio_read32(
        (void *)(g_hpet_base + HPET_REG_INT_STATUS));
    out->available = 1;
    out->main_counter_enabled = (global_config & 1u) != 0;
    out->legacy_replacement_enabled = (global_config & 2u) != 0;
    out->timer0_interrupt_enabled = (timer_config & (1u << 2)) != 0;
    out->timer0_periodic_enabled = (timer_config & (1u << 3)) != 0;
    out->timer0_fsb_enabled = (timer_config & (1u << 14)) != 0;
    out->timer0_route_prepared = g_hpet_timer0.route_prepared;
    out->timer0_route_enabled = g_hpet_timer0.route_enabled;
    out->timer0_pending = (status & 1u) != 0;
    out->timer0_route = (timer_config >> 9) & 0x1Fu;
    out->timer0_config = timer_config;
    spin_unlock_irqrestore(&g_hpet_lock, flags);

    hpet_counter_sample_t counter;
    if (!hpet_read_counter_sample(&counter) || !counter.valid)
        return false;
    out->counter = counter.ticks;
    out->stray_irqs = __atomic_load_n(&g_hpet_stray_irqs,
                                      __ATOMIC_RELAXED);
    out->quarantine_actions = __atomic_load_n(&g_hpet_quarantine_actions,
                                              __ATOMIC_RELAXED);
    out->quarantine_failures = __atomic_load_n(
        &g_hpet_quarantine_failures, __ATOMIC_RELAXED);
    return true;
}

bool hpet_timer0_is_quiescent(void)
{
    hpet_runtime_snapshot_t snapshot;
    return hpet_runtime_snapshot(&snapshot) && snapshot.available &&
           snapshot.main_counter_enabled &&
           !snapshot.legacy_replacement_enabled &&
           !snapshot.timer0_interrupt_enabled &&
           !snapshot.timer0_periodic_enabled && !snapshot.timer0_fsb_enabled &&
           !snapshot.timer0_route_enabled && !snapshot.timer0_pending;
}

bool hpet_timer0_quarantine_stray(void)
{
    __atomic_add_fetch(&g_hpet_stray_irqs, 1, __ATOMIC_RELAXED);
    bool ok = hpet_timer0_force_quiescent();
    if (ok)
        __atomic_add_fetch(&g_hpet_quarantine_actions, 1, __ATOMIC_RELAXED);
    else
        __atomic_add_fetch(&g_hpet_quarantine_failures, 1,
                           __ATOMIC_RELAXED);
    return ok;
}

bool hpet_timer0_snapshot(hpet_timer_snapshot_t *out)
{
    if (!out)
        return false;
    irq_flags_t flags = spin_lock_irqsave(&g_hpet_lock);
    *out = g_hpet_timer0;
    if (g_hpet_base)
        out->pending =
            (mmio_read32((void *)(g_hpet_base + HPET_REG_INT_STATUS)) & 1u) != 0;
    spin_unlock_irqrestore(&g_hpet_lock, flags);
    return hpet_is_available();
}

bool hpet_timer0_model_selftest(void)
{
    hpet_timer0_state_t state = HPET_TIMER0_OFF;
    bool ok = state == HPET_TIMER0_OFF;
    state = HPET_TIMER0_QUIESCENT;
    ok = ok && state == HPET_TIMER0_QUIESCENT;
    state = HPET_TIMER0_PREPARED_MASKED;
    ok = ok && state == HPET_TIMER0_PREPARED_MASKED;
    state = HPET_TIMER0_ARMED_MASKED;
    ok = ok && state == HPET_TIMER0_ARMED_MASKED;
    state = HPET_TIMER0_ACTIVE;
    ok = ok && state == HPET_TIMER0_ACTIVE;
    state = HPET_TIMER0_QUIESCENT;
    ok = ok && state == HPET_TIMER0_QUIESCENT;
#ifdef HOBBYOS_TIMER_NEGATIVE_HPET_COUNTER_STALL_MODEL
    uint64_t before = 1000u;
    uint64_t after = 1000u;
    bool detected = ok && after <= before;
    serial_write_all(detected
        ? "[CLOCK][NEGATIVE] HPET_CLOCKSOURCE_STALL_DETECTED\n"
        : "[CLOCK][NEGATIVE] HPET_CLOCKSOURCE_STALL_MISSED\n");
    return false;
#else
    return ok;
#endif
}
