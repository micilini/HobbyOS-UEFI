#include "irq_latency_time.h"
#include <stddef.h>

bool irq_latency_muldiv(uint64_t a, uint64_t b, uint64_t divisor,
                        bool round_up, uint64_t *out)
{
    if (!out || !divisor) return false;
    __uint128_t product = (__uint128_t)a * b;
    uint64_t high = (uint64_t)(product >> 64), low = (uint64_t)product;
    if (high >= divisor) return false;
    uint64_t quotient = 0, remainder = high;
    /* The high half is already a proper remainder. Divide the low half with
     * explicit carry; no freestanding 128-bit division helper is required. */
    for (unsigned bit = 64; bit != 0; --bit) {
        bool carry = (remainder >> 63) != 0;
        remainder = (remainder << 1) | ((low >> (bit - 1)) & 1);
        if (carry || remainder >= divisor) {
            remainder -= divisor;
            quotient |= UINT64_C(1) << (bit - 1);
        }
    }
    if (round_up && remainder) {
        if (quotient == UINT64_MAX) return false;
        ++quotient;
    }
    *out = quotient;
    return true;
}

bool irq_latency_cycles_ns(uint64_t cycles, uint64_t frequency, uint64_t *out)
{
    return irq_latency_muldiv(cycles, UINT64_C(1000000000), frequency, true, out);
}

bool irq_latency_capabilities_valid(const irq_latency_capabilities_t *raw)
{
    return raw && raw->maximum_basic >= 1 && (raw->leaf1_edx & (1u << 4));
}

bool irq_latency_cpuid_frequency(const irq_latency_capabilities_t *raw, uint64_t *out)
{
    uint64_t hz;
    if (!out || !irq_latency_capabilities_valid(raw) || raw->maximum_basic < 0x15 ||
        !raw->denominator || !raw->numerator || !raw->crystal_hz ||
        !irq_latency_muldiv(raw->crystal_hz, raw->numerator, raw->denominator, false, &hz) ||
        !hz) return false;
    *out = hz;
    return true;
}

bool irq_latency_calibration_interval(const irq_latency_calibration_window_t *w,
                                      irq_latency_frequency_interval_t *out)
{
    if (!w || !out) return false;
    const irq_latency_reference_point_t *a = &w->begin, *b = &w->end;
    if (!a->valid || !b->valid || !a->initialized || !b->initialized ||
        !a->interrupts_enabled || !b->interrupts_enabled || a->cpu != b->cpu ||
        a->cpu != a->end_cpu || b->cpu != b->end_cpu ||
        a->switch_sequence != a->switch_after || b->switch_sequence != b->switch_after ||
        (a->mode != 1 && a->mode != 2) || a->mode != b->mode ||
        !w->period_fs || w->period_fs > UINT64_C(100000000) ||
        a->tsc_after < a->tsc_before || b->tsc_after < b->tsc_before ||
        b->tsc_before <= a->tsc_after || b->ticks <= a->ticks) return false;
    uint64_t ticks = b->ticks - a->ticks;
    if (ticks <= 2 || ticks > UINT64_MAX / w->period_fs - 2) return false;
    uint64_t elapsed_fs = ticks * w->period_fs;
    uint64_t duration_ns = elapsed_fs / UINT64_C(1000000);
    if (duration_ns < IRQ_LATENCY_CALIBRATION_MIN_NS ||
        duration_ns > IRQ_LATENCY_CALIBRATION_MAX_NS) return false;
    uint64_t low_cycles = b->tsc_before - a->tsc_after;
    uint64_t high_cycles = b->tsc_after - a->tsc_before;
    uint64_t lower, upper, estimate, width_ppm;
    if (!irq_latency_muldiv(low_cycles, UINT64_C(1000000000000000),
            elapsed_fs + 2 * w->period_fs, false, &lower) || !lower ||
        !irq_latency_muldiv(high_cycles, UINT64_C(1000000000000000),
            elapsed_fs - 2 * w->period_fs, true, &upper) || upper < lower ||
        !irq_latency_muldiv(upper - lower, UINT64_C(1000000), lower, true, &width_ppm) ||
        width_ppm > IRQ_LATENCY_CALIBRATION_PPM ||
        !irq_latency_muldiv(low_cycles + (high_cycles - low_cycles) / 2,
            UINT64_C(1000000000000000), elapsed_fs, false, &estimate)) return false;
    *out = (irq_latency_frequency_interval_t){lower, upper, estimate, duration_ns};
    return true;
}

bool irq_latency_calibration_select(const irq_latency_calibration_window_t *windows,
                                    const irq_latency_capabilities_t *raw,
                                    irq_latency_timebase_t *out)
{
    if (!windows || !out || !irq_latency_capabilities_valid(raw)) return false;
    uint64_t lower = 0, upper = UINT64_MAX;
    for (unsigned i = 0; i < IRQ_LATENCY_CALIBRATION_WINDOWS; ++i) {
        irq_latency_frequency_interval_t interval;
        if (!irq_latency_calibration_interval(&windows[i], &interval) ||
            windows[i].begin.cpu != windows[0].begin.cpu ||
            windows[i].period_fs != windows[0].period_fs ||
            (i && windows[i].begin.tsc_before <= windows[i-1].end.tsc_after)) return false;
        if (interval.lower_hz > lower) lower = interval.lower_hz;
        if (interval.upper_hz < upper) upper = interval.upper_hz;
    }
    if (lower > upper) return false;
    uint64_t declared = 0, conversion;
    bool cpuid = irq_latency_cpuid_frequency(raw, &declared);
    if (cpuid && (declared < lower || declared > upper)) return false;
    /* Freeze a conservative rate with a one-percent drift reserve. A later
     * check may invalidate this rate, but may never raise it to shrink times. */
    if (!irq_latency_muldiv(lower, UINT64_C(1000000) - IRQ_LATENCY_CALIBRATION_PPM,
                            UINT64_C(1000000), false, &conversion) || !conversion) return false;
    *out = (irq_latency_timebase_t){lower, upper, lower + (upper-lower)/2,
                                    conversion, cpuid ? declared : 0, windows[0].begin.cpu};
    return true;
}

bool irq_latency_calibration_recheck(const irq_latency_timebase_t *before,
                                     const irq_latency_timebase_t *after)
{
    if (!before || !after || !before->conversion_hz || !after->conversion_hz ||
        before->cpu != after->cpu || before->declared_hz != after->declared_hz ||
        before->lower_hz > before->upper_hz || after->lower_hz > after->upper_hz ||
        after->lower_hz < before->conversion_hz || !before->estimate_hz) return false;
    uint64_t delta = before->estimate_hz > after->estimate_hz ?
        before->estimate_hz - after->estimate_hz : after->estimate_hz - before->estimate_hz;
    uint64_t ppm;
    return irq_latency_muldiv(delta, UINT64_C(1000000), before->estimate_hz, true, &ppm) &&
        ppm <= IRQ_LATENCY_CALIBRATION_PPM;
}
