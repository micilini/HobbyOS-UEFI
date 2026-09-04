#ifndef HOBBYOS_IRQ_BOOTSTRAP_H
#define HOBBYOS_IRQ_BOOTSTRAP_H

#include "../drivers/timer.h"
#include "../smp/smp_topology.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    IRQ_BOOTSTRAP_OFF = 0,
    IRQ_BOOTSTRAP_CONTROLLERS_QUIESCENT,
    IRQ_BOOTSTRAP_ROUTES_PREPARED,
    IRQ_BOOTSTRAP_CPUS_PREPARED,
    IRQ_BOOTSTRAP_BSP_LAPIC_VERIFIED,
    IRQ_BOOTSTRAP_HPET_CLOCKSOURCE_VERIFIED,
    IRQ_BOOTSTRAP_CLOCKEVENT_ACTIVE,
    IRQ_BOOTSTRAP_SERVICES_ACTIVE,
    IRQ_BOOTSTRAP_FAILED
} irq_bootstrap_state_t;

typedef enum
{
    IRQ_CPU_RUNTIME_OFF = 0,
    IRQ_CPU_RUNTIME_WAIT_RELEASE,
    IRQ_CPU_RUNTIME_TIMER_BASELINE,
    IRQ_CPU_RUNTIME_TIMER_WAIT,
    IRQ_CPU_RUNTIME_TIMER_VERIFIED,
    IRQ_CPU_RUNTIME_HANDOFF,
    IRQ_CPU_RUNTIME_PREEMPTION,
    IRQ_CPU_RUNTIME_READY,
    IRQ_CPU_RUNTIME_FAILED
} irq_cpu_runtime_stage_t;

typedef enum
{
    IRQ_CPU_READY_OK = 0,
    IRQ_CPU_READY_INVALID_STATE,
    IRQ_CPU_READY_RELEASE_ABORTED,
    IRQ_CPU_READY_TIMER_STATE,
    IRQ_CPU_READY_TIMER_NO_ENTRY,
    IRQ_CPU_READY_TIMER_NO_RETURN,
    IRQ_CPU_READY_TIMER_JOURNAL,
    IRQ_CPU_READY_HANDOFF,
    IRQ_CPU_READY_PREEMPTION,
    IRQ_CPU_READY_GLOBAL_DEADLINE,
    IRQ_CPU_READY_PUBLICATION
} irq_cpu_ready_result_t;

typedef struct
{
    cpu_slot_t slot;
    uint32_t apic_id;
    uint32_t first_vector;
    irq_cpu_runtime_stage_t stage;
    irq_cpu_runtime_stage_t failure_stage;
    irq_cpu_ready_result_t failure_reason;
    uint32_t timer_mode;
    uint64_t lapic_entered;
    uint64_t lapic_returned;
    uint64_t entered_before;
    uint64_t returned_before;
    uint64_t entered_after;
    uint64_t returned_after;
    uint8_t timer_calibrated;
    uint8_t timer_masked;
    uint8_t interrupt_prepared;
    uint8_t interrupt_verified;
    uint8_t handoff_complete;
    uint8_t preemption_enabled;
    uint8_t runtime_ready;
    uint8_t failed;
} irq_bootstrap_cpu_snapshot_t;

typedef struct
{
    irq_bootstrap_state_t state;
    uint32_t cpus_expected;
    uint32_t cpus_prepared;
    uint32_t cpus_verified;
    uint32_t handoffs_complete;
    uint32_t preemption_enabled;
    uint32_t cpus_runtime_ready;
    uint32_t failed_cpus;
    uint32_t keyboard_gsi;
    uint64_t bsp_lapic_entered;
    uint64_t bsp_lapic_returned;
    uint64_t hpet_counter_before;
    uint64_t hpet_counter_after;
    uint64_t hpet_counter_delta;
    uint64_t global_ticks;
    uint64_t early_tick_attempts;
    uint64_t non_bsp_tick_attempts;
    uint64_t tick_time_regressions;
    uint64_t hpet_stray_irqs;
    uint64_t runtime_ready_deadline_ns;
    uint64_t transition_violations;
    uint32_t bsp_probe_vector;
    uint32_t hpet_probe_samples;
    cpu_slot_t clockevent_bsp_slot;
    uint32_t clockevent_period_us;
    timer_clockevent_source_t clockevent_source;
    uint8_t pcat_compat;
    uint8_t controllers_quiescent;
    uint8_t routes_prepared;
    uint8_t bsp_lapic_probe_passed;
    uint8_t hpet_clocksource_verified;
    uint8_t hpet_timer0_quiescent;
    uint8_t clockevent_active;
    uint8_t cpu_release;
    uint8_t runtime_ready_abort;
    uint8_t services_active;
} irq_bootstrap_snapshot_t;

bool irq_bootstrap_init(void);
bool irq_bootstrap_controllers_quiescent(void);
bool irq_bootstrap_routes_prepared(void);
bool irq_bootstrap_cpu_prepare(cpu_slot_t slot);
irq_cpu_ready_result_t irq_bootstrap_cpu_run_runtime(cpu_slot_t slot);
void irq_bootstrap_log_cpu_runtime_error(cpu_slot_t slot,
                                         irq_cpu_ready_result_t result);
bool irq_bootstrap_verify_bsp_lapic(uint64_t timeout_us);
bool irq_bootstrap_verify_hpet_clocksource(uint32_t max_samples);
bool irq_bootstrap_activate_clockevent(void);
bool irq_bootstrap_release_cpus(void);
bool irq_bootstrap_cpu_complete_runtime(cpu_slot_t slot);
bool irq_bootstrap_wait_all_runtime_ready(uint64_t timeout_us);
bool irq_bootstrap_release_services(void);

irq_bootstrap_state_t irq_bootstrap_state(void);
bool irq_bootstrap_snapshot(irq_bootstrap_snapshot_t *out);
bool irq_bootstrap_cpu_snapshot(cpu_slot_t slot,
                                irq_bootstrap_cpu_snapshot_t *out);
bool irq_bootstrap_cpu_irq_ready(cpu_slot_t slot);
bool irq_bootstrap_services_are_ready(void);
bool irq_bootstrap_validate(void);
bool irq_bootstrap_model_selftest(void);

#endif
