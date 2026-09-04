#ifndef HPET_H
#define HPET_H

#include "../acpi/acpi.h"
#include <stdbool.h>
#include <stdint.h>

#define HPET_REG_CAPABILITIES 0x000
#define HPET_REG_CONFIG 0x010
#define HPET_REG_INT_STATUS 0x020
#define HPET_REG_MAIN_COUNTER 0x0F0

#define HPET_TN_CONFIG_CAP(n) (0x100 + (0x20 * n))
#define HPET_TN_COMPARATOR(n) (0x108 + (0x20 * n))
#define HPET_TN_FSB_ROUTE(n) (0x110 + (0x20 * n))

#define HPET_COUNTER_SPLIT_RETRY_LIMIT 4u

typedef enum
{
    HPET_COUNTER_READ_NONE = 0,
    HPET_COUNTER_READ_SPLIT64_STABLE,
    HPET_COUNTER_READ_EXTENDED32
} hpet_counter_read_mode_t;

typedef enum
{
    HPET_SPLIT_SAMPLE_OK = 0,
    HPET_SPLIT_SAMPLE_RETRY,
    HPET_SPLIT_SAMPLE_INVALID
} hpet_split_result_t;

typedef struct
{
    uint64_t ticks;
    uint32_t retries;
    hpet_counter_read_mode_t mode;
    uint8_t valid;
} hpet_counter_sample_t;

typedef struct
{
    uint64_t reads;
    uint64_t split64_reads;
    uint64_t extended32_reads;
    uint64_t split_retries;
    uint64_t split_retry_exhaustions;
    uint64_t low32_rollovers;
    uint32_t max_split_retries;
    uint32_t counter_width_bits;
    hpet_counter_read_mode_t read_mode;
} hpet_counter_stats_t;

typedef struct
{
    AcpiSdtHeader header;
    uint8_t hardware_rev_id;
    uint8_t comparator_count : 5;
    uint8_t counter_size : 1;
    uint8_t reserved : 1;
    uint8_t legacy_replacement : 1;
    uint16_t pci_vendor_id;

    uint8_t address_space_id;
    uint8_t register_bit_width;
    uint8_t register_bit_offset;
    uint8_t reserved2;
    uint64_t address;

    uint8_t hpet_number;
    uint16_t minimum_tick;
    uint8_t page_protection;
} __attribute__((packed)) HpetTable;

typedef enum
{
    HPET_TIMER0_OFF = 0,
    HPET_TIMER0_QUIESCENT,
    HPET_TIMER0_PREPARED_MASKED,
    HPET_TIMER0_ARMED_MASKED,
    HPET_TIMER0_ACTIVE
} hpet_timer0_state_t;

typedef struct
{
    uint64_t comparator;
    uint64_t arm_count;
    uint32_t route_capability;
    uint32_t gsi;
    uint32_t destination_apic_id;
    uint32_t timer_config;
    uint8_t vector;
    hpet_timer0_state_t state;
    uint8_t route_prepared;
    uint8_t route_enabled;
    uint8_t pending;
} hpet_timer_snapshot_t;

typedef struct
{
    uint8_t available;
    uint8_t main_counter_enabled;
    uint8_t legacy_replacement_enabled;
    uint8_t timer0_interrupt_enabled;
    uint8_t timer0_periodic_enabled;
    uint8_t timer0_fsb_enabled;
    uint8_t timer0_route_prepared;
    uint8_t timer0_route_enabled;
    uint8_t timer0_pending;
    uint32_t timer0_route;
    uint32_t timer0_config;
    uint64_t counter;
    uint64_t stray_irqs;
    uint64_t quarantine_actions;
    uint64_t quarantine_failures;
} hpet_runtime_snapshot_t;

void init_hpet();

void hpet_usleep(uint64_t microseconds);

bool hpet_timer0_prepare(uint8_t vector, uint32_t destination_apic_id);
bool hpet_timer0_arm(uint64_t milliseconds);
bool hpet_timer0_enable_route(void);
bool hpet_timer0_disable(void);
bool hpet_timer0_clear_pending(void);
bool hpet_timer0_snapshot(hpet_timer_snapshot_t *out);
bool hpet_timer0_force_quiescent(void);
bool hpet_timer0_is_quiescent(void);
bool hpet_timer0_quarantine_stray(void);
bool hpet_runtime_snapshot(hpet_runtime_snapshot_t *out);
bool hpet_timer0_model_selftest(void);
bool hpet_is_available(void);
bool hpet_validate_period_fs(uint64_t period_fs);
bool hpet_duration_us_to_ticks(uint64_t us, uint64_t *out);
bool hpet_duration_ms_to_ticks(uint64_t ms, uint64_t *out);
bool hpet_validation_selftest(void);
bool hpet_counter_access_selftest(void);
hpet_split_result_t hpet_counter64_combine(uint32_t high_before,
                                            uint32_t low,
                                            uint32_t high_after,
                                            uint64_t *out);
bool hpet_read_counter_sample(hpet_counter_sample_t *out);
void hpet_counter_stats_snapshot(hpet_counter_stats_t *out);
void hpet_counter_test_stats_reset(void);
uint64_t hpet_read_counter(void);
uint64_t hpet_frequency_hz(void);
uint64_t hpet_period_fs(void);
uint64_t hpet_counter_to_ns(uint64_t ticks);
bool hpet_counter_extend32_test(uint32_t previous, uint64_t high,
                                uint32_t current, uint64_t *out);
typedef struct
{
    uint64_t ticks;
    uint64_t ns;
    uint32_t retries;
    hpet_counter_read_mode_t mode;
    uint8_t valid;
} hpet_clock_sample_t;
bool hpet_read_clock_sample(hpet_clock_sample_t*out);

#endif
