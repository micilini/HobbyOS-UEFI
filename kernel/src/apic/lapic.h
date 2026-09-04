#ifndef LAPIC_H
#define LAPIC_H

#include <stdbool.h>
#include <stdint.h>

#define IA32_APIC_BASE_MSR 0x1Bu
#define IA32_APIC_BASE_MSR_ENABLE 0x800u

#define LAPIC_ID 0x020u
#define LAPIC_VER 0x030u
#define LAPIC_TPR 0x080u
#define LAPIC_EOI 0x0B0u
#define LAPIC_SPURIOUS 0x0F0u
#define LAPIC_ESR 0x280u
#define LAPIC_ICR0 0x300u
#define LAPIC_ICR1 0x310u
#define LAPIC_LVT_TIMER 0x320u
#define LAPIC_LVT_THERMAL 0x330u
#define LAPIC_LVT_PERF 0x340u
#define LAPIC_LVT_LINT0 0x350u
#define LAPIC_LVT_LINT1 0x360u
#define LAPIC_LVT_ERROR 0x370u
#define LAPIC_TICR 0x380u
#define LAPIC_TCCR 0x390u
#define LAPIC_TDCR 0x3E0u
#define LAPIC_LVT_CMCI 0x2F0u

#define APIC_DM_FIXED 0x00000000u
#define APIC_DM_LOWEST 0x00000100u
#define APIC_DM_SMI 0x00000200u
#define APIC_DM_NMI 0x00000400u
#define APIC_DM_INIT 0x00000500u
#define APIC_DM_SIPI 0x00000600u

#define APIC_DEST_PHYSICAL 0x00000000u
#define APIC_DEST_LOGICAL 0x00000800u
#define APIC_DS_PENDING 0x00001000u
#define APIC_LEVEL_DEASSERT 0x00000000u
#define APIC_LEVEL_ASSERT 0x00004000u
#define APIC_TRIGGER_EDGE 0x00000000u
#define APIC_TRIGGER_LEVEL 0x00008000u
#define APIC_DEST_SHORTHAND_NONE 0x00000000u
#define APIC_DEST_SHORTHAND_SELF 0x00040000u
#define APIC_DEST_SHORTHAND_ALL 0x00080000u
#define APIC_DEST_SHORTHAND_ALL_BUT_SELF 0x000C0000u

#define APIC_TIMER_PERIODIC 0x00020000u
#define APIC_TIMER_ONE_SHOT 0x00000000u
#define APIC_TIMER_MASKED 0x00010000u

typedef enum
{
    LAPIC_TIMER_OFF = 0,
    LAPIC_TIMER_CALIBRATED,
    LAPIC_TIMER_PREPARED_MASKED,
    LAPIC_TIMER_ACTIVE_PERIODIC
} lapic_timer_mode_t;

typedef struct
{
    uint64_t window_ns;
    uint32_t elapsed_ticks;
    uint64_t ticks_per_second;
    uint32_t ticks_per_ms;
    uint32_t periodic_initial_count;
    uint32_t divisor;
    uint8_t valid;
} lapic_timer_calibration_t;

typedef struct
{
    lapic_timer_calibration_t calibration;
    uint32_t desired_period_us;
    uint32_t programmed_vector;
    uint32_t programmed_divisor;
    uint32_t programmed_initial_count;
    uint32_t lvt_value;
    uint32_t tdcr_value;
    uint32_t ticr_value;
    uint64_t irq_count;
    uint64_t last_irq_ns;
    uint64_t max_irq_gap_ns;
    uint64_t start_ns;
    uint64_t programming_generation;
    lapic_timer_mode_t mode;
    uint8_t calibrated;
    uint8_t programmed;
    uint8_t periodic;
    uint8_t masked;
} lapic_timer_state_t;

typedef struct
{
    uint32_t apic_id;
    uint32_t version;
    uint32_t max_lvt;
    uint32_t esr_before;
    uint32_t esr_after;
    uint32_t tpr;
    uint32_t svr;
    uint32_t lvt_timer;
    uint32_t lvt_thermal;
    uint32_t lvt_perf;
    uint32_t lvt_lint0;
    uint32_t lvt_lint1;
    uint32_t lvt_error;
    uint32_t lvt_cmci;
    uint32_t masked_count;
    uint8_t x2apic;
    uint8_t valid;
} lapic_quiescent_snapshot_t;

typedef enum
{
    LAPIC_TIMER_VALID_CALIBRATION = 1u << 0,
    LAPIC_TIMER_VALID_PROGRAMMED = 1u << 1,
    LAPIC_TIMER_VALID_VECTOR = 1u << 2,
    LAPIC_TIMER_VALID_DIVISOR = 1u << 3,
    LAPIC_TIMER_VALID_INITIAL_COUNT = 1u << 4,
    LAPIC_TIMER_VALID_PERIODIC = 1u << 5,
    LAPIC_TIMER_VALID_UNMASKED = 1u << 6,
    LAPIC_TIMER_VALID_IRQ_SEEN = 1u << 7,
    LAPIC_TIMER_VALID_LAST_IRQ = 1u << 8
} lapic_timer_validation_bit_t;

#define LAPIC_TIMER_VALID_CONFIG_MASK \
    (LAPIC_TIMER_VALID_CALIBRATION | LAPIC_TIMER_VALID_PROGRAMMED | \
     LAPIC_TIMER_VALID_VECTOR | LAPIC_TIMER_VALID_DIVISOR | \
     LAPIC_TIMER_VALID_INITIAL_COUNT | LAPIC_TIMER_VALID_PERIODIC | \
     LAPIC_TIMER_VALID_UNMASKED)

typedef struct
{
    uint32_t passed_mask;
    uint32_t failed_mask;
} lapic_timer_validation_t;

typedef struct
{
    uint64_t expected_periods;
    uint64_t delivered_irqs;
    uint64_t service_deficit;
    uint64_t longest_gap_ns;
} lapic_timer_rate_sample_t;

bool init_lapic(void);
bool init_lapic_ap(void);
void lapic_write(uint32_t reg, uint32_t value);
uint32_t lapic_read(uint32_t reg);
void lapic_eoi(void);
uint32_t lapic_get_id(void);
bool lapic_is_x2apic(void);
bool lapic_set_normal_delivery(bool enable);
bool lapic_bind_quiescent_slot(uint32_t slot);
bool lapic_quiescent_snapshot(uint32_t slot,
                              lapic_quiescent_snapshot_t *out);

void lapic_send_ipi(uint32_t apic_id, uint8_t vector);
void lapic_send_init(uint32_t apic_id);
void lapic_send_sipi(uint32_t apic_id, uint32_t trampoline_page);
void lapic_send_broadcast_halt(void);

bool lapic_timer_calibrate(uint32_t desired_period_us,
                           lapic_timer_calibration_t *out);
bool lapic_timer_prepare_periodic(
    uint32_t vector, const lapic_timer_calibration_t *calibration);
bool lapic_timer_enable_periodic(void);
bool lapic_timer_disable(void);
bool lapic_timer_is_active(void);
bool lapic_timer_start_periodic(
    uint32_t vector, const lapic_timer_calibration_t *calibration);
bool lapic_timer_calibration_math(uint32_t elapsed_ticks, uint64_t elapsed_ns,
                                  uint32_t desired_period_us,
                                  lapic_timer_calibration_t *out);
bool lapic_timer_get_calibration(uint32_t slot,
                                 lapic_timer_calibration_t *out,
                                 uint64_t *irq_count);
void lapic_timer_record_irq(void);
void lapic_timer_record_irq_at(uint64_t now_ns);
bool lapic_timer_state_snapshot(uint32_t slot, lapic_timer_state_t *out);
bool lapic_timer_validate_configuration(uint32_t slot,
                                        lapic_timer_validation_t *out);
bool lapic_timer_model_selftest(void);

#endif
