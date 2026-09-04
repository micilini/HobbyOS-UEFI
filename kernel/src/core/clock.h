#ifndef CLOCK_H
#define CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#define CLOCK_SAMPLE_RETRY_LIMIT 4u

typedef enum {
    CLOCK_SAMPLE_OK = 0,
    CLOCK_SAMPLE_CROSS_CPU_LAG,
    CLOCK_SAMPLE_LOCAL_REGRESSION,
    CLOCK_SAMPLE_RETRY_EXHAUSTED,
    CLOCK_SAMPLE_INVALID
} clock_sample_class_t;

typedef struct {
    clock_sample_class_t classification;
    uint64_t returned_ns;
    uint64_t local_backward_ns;
    uint64_t cross_cpu_lag_ns;
    uint8_t update_local_raw;
    uint8_t update_global;
    uint8_t hard_failure;
} clock_sample_decision_t;

typedef struct {
    uint64_t reads;
    uint64_t api_regressions;
    uint64_t source_local_regressions;
    uint64_t cross_cpu_lag_clamps;
    uint64_t source_retry_exhaustions;
    uint64_t saturations;
    uint64_t not_ready;
    uint64_t unknown_cpu_reads;
    uint64_t state_corruptions;
    uint64_t max_local_backward_ns;
    uint64_t max_cross_cpu_lag_ns;
    uint64_t max_retries;
} clock_stats_t;

typedef struct {
    uint64_t sequence;
    uint32_t cpu_slot;
    uint32_t apic_id;
    uint64_t raw_ticks;
    uint64_t raw_ns;
    uint64_t cpu_last_raw_ns;
    uint64_t global_last_ns;
    uint64_t returned_ns;
    uint64_t delta_ns;
    uint32_t retries;
    uint32_t raw_high;
    uint32_t raw_low;
    uint8_t read_mode;
    uint8_t kind;
} clock_anomaly_event_t;

bool clock_classify_sample(uint64_t cpu_last_raw_ns,
                           uint8_t cpu_initialized,
                           uint64_t global_last_ns,
                           uint64_t sample_ns,
                           uint32_t retries,
                           uint8_t sample_valid,
                           clock_sample_decision_t *out);
bool clock_sample_classifier_selftest(void);
uint32_t clock_sample_classifier_case_count(void);
void clock_state_layout(uint64_t *begin_offset, uint64_t *end_offset,
                        uint64_t *protected_bytes);
bool clock_monotonic_init(void);
bool clock_monotonic_is_ready(void);
uint64_t clock_monotonic_ns(void);
uint64_t clock_monotonic_us(void);
uint64_t clock_monotonic_ms(void);
void clock_stats_snapshot(clock_stats_t *out);
void clock_test_stats_reset(void);
uint32_t clock_events_snapshot(clock_anomaly_event_t *out, uint32_t cap);
bool clock_monotonic_selftest(void);

#endif
