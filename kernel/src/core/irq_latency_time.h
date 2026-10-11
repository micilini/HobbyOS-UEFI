#ifndef HOBBYOS_IRQ_LATENCY_TIME_H
#define HOBBYOS_IRQ_LATENCY_TIME_H

#include <stdbool.h>
#include <stdint.h>

#define IRQ_LATENCY_BUDGET_NS UINT64_C(50000)
#define IRQ_LATENCY_CALIBRATION_WINDOWS 3u
#define IRQ_LATENCY_CALIBRATION_MIN_NS UINT64_C(10000000)
#define IRQ_LATENCY_CALIBRATION_MAX_NS UINT64_C(100000000)
#define IRQ_LATENCY_CALIBRATION_PPM UINT64_C(10000)

typedef struct {
    uint32_t maximum_basic, maximum_extended;
    uint32_t leaf1_edx, extended1_edx, extended7_edx;
    uint32_t denominator, numerator, crystal_hz;
    uint32_t vendor_b, vendor_d, vendor_c;
} irq_latency_capabilities_t;

typedef struct {
    uint64_t tsc_before, ticks, tsc_after, switch_sequence, switch_after;
    uint32_t cpu, mode, end_cpu;
    bool valid, initialized, interrupts_enabled;
    uint32_t retries, counter_bits;
    /* Acquisition facts for the independently versioned local-pair ledger. */
    uint64_t period_fs, period_after;
    bool acquired, sample_valid, identity_valid, if_after;
} irq_latency_reference_point_t;

typedef struct {
    irq_latency_reference_point_t begin, end;
    uint64_t period_fs;
} irq_latency_calibration_window_t;

typedef struct {
    uint64_t lower_hz, upper_hz, estimate_hz, duration_ns;
} irq_latency_frequency_interval_t;

typedef struct {
    uint64_t lower_hz, upper_hz, estimate_hz, conversion_hz, declared_hz;
    uint32_t cpu;
} irq_latency_timebase_t;

bool irq_latency_muldiv(uint64_t a, uint64_t b, uint64_t divisor,
                        bool round_up, uint64_t *out);
bool irq_latency_cycles_ns(uint64_t cycles, uint64_t frequency, uint64_t *out);
bool irq_latency_capabilities_valid(const irq_latency_capabilities_t *raw);
bool irq_latency_cpuid_frequency(const irq_latency_capabilities_t *raw, uint64_t *out);
bool irq_latency_calibration_interval(const irq_latency_calibration_window_t *window,
                                      irq_latency_frequency_interval_t *out);
bool irq_latency_calibration_select(const irq_latency_calibration_window_t *windows,
                                    const irq_latency_capabilities_t *raw,
                                    irq_latency_timebase_t *out);
bool irq_latency_calibration_recheck(const irq_latency_timebase_t *before,
                                     const irq_latency_timebase_t *after);

#endif
