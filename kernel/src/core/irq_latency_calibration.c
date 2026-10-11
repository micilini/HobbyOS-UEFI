#include "irq_latency_calibration.h"
#include "irq_latency.h"
#include "irq_latency_tsc.h"
#include "idt.h"
#include "../cpu/cpu.h"
#include "../timer/hpet.h"

bool irq_latency_read_capabilities(irq_latency_capabilities_t *out)
{
    if (!out) return false;
    irq_latency_capabilities_t caps = {0};
    uint32_t a, b, c, d;
    if (!cpu_get_cpuid_count(0, 0, &caps.maximum_basic, &b, &c, &d)) return false;
    caps.vendor_b=b;caps.vendor_d=d;caps.vendor_c=c;
    if (caps.maximum_basic >= 1 && cpu_get_cpuid_count(1, 0, &a, &b, &c, &d)) caps.leaf1_edx = d;
    if (cpu_get_cpuid_count(0x80000000, 0, &a, &b, &c, &d) && a >= 0x80000000) {
        caps.maximum_extended = a;
        if (a >= 0x80000001 && cpu_get_cpuid_count(0x80000001, 0, &a, &b, &c, &d))
            caps.extended1_edx = d;
        if (caps.maximum_extended >= 0x80000007 && cpu_get_cpuid_count(0x80000007, 0, &a, &b, &c, &d))
            caps.extended7_edx = d;
    }
    if (caps.maximum_basic >= 0x15)
        (void)cpu_get_cpuid_count(0x15, 0, &caps.denominator, &caps.numerator, &caps.crystal_hz, &d);
    *out = caps;
    return irq_latency_capabilities_valid(out) && irq_latency_tsc_capability(
        caps.leaf1_edx,caps.vendor_b,caps.vendor_d,caps.vendor_c);
}

bool irq_latency_reference_point(irq_latency_reference_point_t *out)
{
    if (!out) return false;
    *out = (irq_latency_reference_point_t){.cpu = UINT32_MAX, .end_cpu = UINT32_MAX};
    out->acquired = true;
    out->period_fs = hpet_period_fs();
    out->initialized = hpet_is_available();
    out->interrupts_enabled = irq_is_enabled();
    if (!out->initialized || !out->interrupts_enabled) return false;
    out->identity_valid = irq_latency_identity_sample(&out->cpu, &out->switch_sequence);
    if (!out->identity_valid) return false;
    hpet_counter_sample_t sample = {0};
    out->tsc_before = irq_latency_read_tsc();
    bool valid = hpet_read_counter_sample(&sample);
    out->tsc_after = irq_latency_read_tsc();
    out->ticks = sample.ticks; out->mode = sample.mode;
    out->retries = sample.retries;
    out->counter_bits = sample.mode == HPET_COUNTER_READ_SPLIT64_STABLE ? 64 :
        sample.mode == HPET_COUNTER_READ_EXTENDED32 ? 32 : 0;
    out->sample_valid = valid && sample.valid;
    out->identity_valid = irq_latency_identity_sample(&out->end_cpu, &out->switch_after);
    valid = out->identity_valid && valid;
    out->period_after = hpet_period_fs();
    out->if_after = irq_is_enabled();
    out->valid = valid && sample.valid && out->counter_bits && out->cpu == out->end_cpu &&
        out->switch_sequence == out->switch_after && out->tsc_after >= out->tsc_before &&
        out->if_after;
    return out->valid;
}

uint32_t irq_latency_reference_acquire(irq_latency_calibration_window_t *out)
{
    if (!out) return 5;
    *out = (irq_latency_calibration_window_t){0};
    bool begin = irq_latency_reference_point(&out->begin);
    out->period_fs = out->begin.period_fs;
    uint32_t outcome = 2;
    uint64_t target;
    if (begin && irq_latency_muldiv(IRQ_LATENCY_REFERENCE_TARGET_NS,UINT64_C(1000000),
                                   out->period_fs,true,&target) && target) {
        outcome = 4;
        for (uint32_t poll=0;poll<IRQ_LATENCY_REFERENCE_POLL_LIMIT;++poll) {
            hpet_counter_sample_t sample;
            if (!hpet_read_counter_sample(&sample) || !sample.valid || sample.ticks<out->begin.ticks) {
                outcome=3; break;
            }
            if (sample.ticks-out->begin.ticks>=target) { outcome=1; break; }
            __asm__ volatile("pause" ::: "memory");
        }
    }
    /* Never replace an uncollected second endpoint with a zero-initialized row. */
    (void)irq_latency_reference_point(&out->end);
    return outcome;
}

bool irq_latency_reference_window(irq_latency_calibration_window_t *out)
{
    if (!out) return false;
    *out = (irq_latency_calibration_window_t){0};
    irq_latency_capabilities_t caps;
    if (!irq_latency_read_capabilities(&caps) || !irq_latency_identity_prepare() ||
        !hpet_is_available() || !irq_is_enabled()) return false;
    out->period_fs = hpet_period_fs();
    uint64_t target;
    if (!irq_latency_muldiv(IRQ_LATENCY_REFERENCE_TARGET_NS, UINT64_C(1000000),
                            out->period_fs, true, &target) || !target ||
        !irq_latency_reference_point(&out->begin)) return false;
    for (uint32_t poll = 0; poll < IRQ_LATENCY_REFERENCE_POLL_LIMIT; ++poll) {
        hpet_counter_sample_t sample;
        if (!hpet_read_counter_sample(&sample) || !sample.valid || sample.ticks < out->begin.ticks)
            return false;
        if (sample.ticks - out->begin.ticks >= target) {
            if (!irq_latency_reference_point(&out->end)) return false;
            irq_latency_frequency_interval_t interval;
            return irq_latency_calibration_interval(out, &interval);
        }
        __asm__ volatile("pause" ::: "memory");
    }
    return false;
}
