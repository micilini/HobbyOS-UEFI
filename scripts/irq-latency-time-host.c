#include "../kernel/src/core/irq_latency_time.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>

static uint64_t state = UINT64_C(0x4952514c);
static uint64_t random_word(void)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

static void arithmetic(uint64_t a, uint64_t b, uint64_t divisor)
{
    for (unsigned up = 0; up != 2; ++up) {
        uint64_t actual = UINT64_C(0x12345678);
        bool ok = irq_latency_muldiv(a, b, divisor, up != 0, &actual);
        if (!divisor) { assert(!ok && actual == UINT64_C(0x12345678)); continue; }
        __uint128_t product = (__uint128_t)a * b;
        __uint128_t expected = product / divisor;
        if (up && product % divisor) ++expected;
        assert(ok == (expected <= UINT64_MAX));
        if (ok) assert(actual == expected);
        else assert(actual == UINT64_C(0x12345678));
    }
}

static irq_latency_calibration_window_t window(unsigned i)
{
    uint64_t t = UINT64_C(1000000000) + i * UINT64_C(100000000);
    uint64_t ticks = UINT64_C(10000000) + i * UINT64_C(1000000);
    return (irq_latency_calibration_window_t){
        .begin = {.tsc_before=t, .ticks=ticks, .tsc_after=t+100,
            .switch_sequence=4, .switch_after=4, .cpu=2, .mode=1, .end_cpu=2,
            .valid=true, .initialized=true, .interrupts_enabled=true, .counter_bits=64},
        .end = {.tsc_before=t+UINT64_C(48000000), .ticks=ticks+200000,
            .tsc_after=t+UINT64_C(48000100), .switch_sequence=8, .switch_after=8,
            .cpu=2, .mode=1, .end_cpu=2, .valid=true, .initialized=true,
            .interrupts_enabled=true, .counter_bits=64},
        .period_fs = UINT64_C(100000000)};
}

int main(void)
{
    const uint64_t values[] = {0, 1, 2, 3, 49999, 50000, 50001,
        UINT32_MAX, UINT64_C(1) << 32, UINT64_C(1) << 63, UINT64_MAX-1, UINT64_MAX};
    for (unsigned a = 0; a < sizeof(values)/sizeof(values[0]); ++a)
        for (unsigned b = 0; b < sizeof(values)/sizeof(values[0]); ++b)
            for (unsigned d = 0; d < sizeof(values)/sizeof(values[0]); ++d)
                arithmetic(values[a], values[b], values[d]);
    for (unsigned i = 0; i < 100000; ++i) {
        uint64_t a = random_word(), b = random_word(), d = random_word();
        arithmetic(a, b, d);
    }
    uint64_t ns = 17;
    assert(!irq_latency_cycles_ns(100, 0, &ns) && ns == 17);
    assert(!irq_latency_cycles_ns(UINT64_MAX, 1, &ns) && ns == 17);
    assert(irq_latency_cycles_ns(1, 3000000000, &ns) && ns == 1);
    for (uint64_t n = 49999; n <= 50001; ++n) {
        assert(irq_latency_cycles_ns(n*3, 3000000000, &ns) && ns == n);
        assert((ns < IRQ_LATENCY_BUDGET_NS) == (n == 49999));
    }
    irq_latency_capabilities_t caps = {.maximum_basic=0x15, .maximum_extended=0x80000007,
        .leaf1_edx=1u<<4, .extended1_edx=1u<<27, .extended7_edx=1u<<8,
        .denominator=1, .numerator=100, .crystal_hz=24000000};
    uint64_t hz = 19;
    assert(irq_latency_cpuid_frequency(&caps, &hz) && hz == 2400000000);
    irq_latency_capabilities_t bad = caps;
    bad.denominator = 0;
    assert(!irq_latency_cpuid_frequency(&bad, &hz));
    bad = caps; bad.numerator = 0;
    assert(!irq_latency_cpuid_frequency(&bad, &hz));
    bad = caps; bad.crystal_hz = 0;
    assert(!irq_latency_cpuid_frequency(&bad, &hz));
    bad = caps; bad.maximum_basic = 0x14;
    assert(!irq_latency_cpuid_frequency(&bad, &hz));
    bad = caps; bad.leaf1_edx = 0;
    assert(!irq_latency_cpuid_frequency(&bad, &hz));
    assert(!irq_latency_cpuid_frequency(NULL, &hz));
    assert(!irq_latency_muldiv(1, 1, 1, false, NULL));
    irq_latency_calibration_window_t windows[IRQ_LATENCY_CALIBRATION_WINDOWS];
    for (unsigned i = 0; i < IRQ_LATENCY_CALIBRATION_WINDOWS; ++i) windows[i] = window(i);
    irq_latency_timebase_t before, after;
    assert(irq_latency_calibration_select(windows, &caps, &before));
    assert(before.conversion_hz < before.lower_hz && before.declared_hz == 2400000000);
    assert(irq_latency_calibration_recheck(&before, &before));
    bad = caps; bad.crystal_hz = 0;
    assert(irq_latency_calibration_select(windows, &bad, &after) && !after.declared_hz);
    assert(!irq_latency_calibration_recheck(&before, &after));
    bad = caps; bad.crystal_hz = 25000000;
    assert(!irq_latency_calibration_select(windows, &bad, &after));
    irq_latency_frequency_interval_t interval;
    for (unsigned mutation = 0; mutation < 14; ++mutation) {
        irq_latency_calibration_window_t w = window(0);
        switch (mutation) {
        case 0: w.begin.valid = false; break;
        case 1: w.end.valid = false; break;
        case 2: w.begin.initialized = false; break;
        case 3: w.end.interrupts_enabled = false; break;
        case 4: ++w.end.cpu; break;
        case 5: ++w.end.switch_sequence; break;
        case 6: ++w.end.mode; break;
        case 7: w.period_fs = 0; break;
        case 8: w.end.ticks = w.begin.ticks; break;
        case 9: w.end.ticks = w.begin.ticks - 1; break;
        case 10: w.begin.tsc_after = w.begin.tsc_before - 1; break;
        case 11: w.end.tsc_before = w.begin.tsc_after; break;
        case 12: w.end.tsc_after += 48000000; break;
        case 13: w.end.ticks = UINT64_MAX; break;
        }
        assert(!irq_latency_calibration_interval(&w, &interval));
        assert(irq_latency_calibration_select(windows, &caps, &after));
    }
    windows[1].end.tsc_before += 2000000;
    windows[1].end.tsc_after += 2000000;
    assert(!irq_latency_calibration_select(windows, &caps, &after));
    windows[1] = window(1);
    windows[2] = windows[1];
    assert(!irq_latency_calibration_select(windows, &caps, &after));
    windows[2] = window(2);
    assert(irq_latency_calibration_select(windows, &caps, &after));
    after.lower_hz = before.conversion_hz - 1;
    assert(!irq_latency_calibration_recheck(&before, &after));
    after = before; after.estimate_hz += before.estimate_hz / 50;
    assert(!irq_latency_calibration_recheck(&before, &after));
    after = before; ++after.cpu;
    assert(!irq_latency_calibration_recheck(&before, &after));
    /* Independently evaluated exact duration and dispersion boundaries. */
    const uint64_t duration_ticks[] = {99999,100000,199999,200000,1000000,1000010};
    for (unsigned i=0;i<sizeof(duration_ticks)/sizeof(duration_ticks[0]);++i) {
        irq_latency_calibration_window_t w=window(0);
        w.end.ticks=w.begin.ticks+duration_ticks[i];
        w.end.tsc_before=w.begin.tsc_before+duration_ticks[i]*240;
        w.end.tsc_after=w.end.tsc_before+100;
        bool expected=duration_ticks[i]>=100000 && duration_ticks[i]<=1000000;
        assert(irq_latency_calibration_interval(&w,&interval)==expected);
    }
    for (uint64_t width=479600;width<480100;++width) {
        irq_latency_calibration_window_t w=window(0);w.end.tsc_after=w.end.tsc_before+width;
        uint64_t low_cycles=w.end.tsc_before-w.begin.tsc_after;
        uint64_t high_cycles=w.end.tsc_after-w.begin.tsc_before;
        uint64_t lo=(__uint128_t)low_cycles*1000000000000000ULL/(200002ULL*w.period_fs);
        __uint128_t upper_product=(__uint128_t)high_cycles*1000000000000000ULL;
        uint64_t divisor=199998ULL*w.period_fs;
        uint64_t hi=(upper_product+divisor-1)/divisor;
        uint64_t ppm=((hi-lo)*1000000ULL+lo-1)/lo;
        assert(irq_latency_calibration_interval(&w,&interval)==(ppm<=10000));
    }
    puts("IRQ latency actual-C arithmetic and calibration decisions: PASS");
    return 0;
}
