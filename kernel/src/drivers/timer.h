#ifndef TIMER_H
#define TIMER_H

#include <stdbool.h>
#include <stdint.h>
#include "../core/task.h"
#include "../smp/smp_topology.h"

typedef enum
{
    TIMER_CLOCKEVENT_NONE = 0,
    TIMER_CLOCKEVENT_BSP_LAPIC
} timer_clockevent_source_t;

typedef struct
{
    timer_clockevent_source_t source;
    cpu_slot_t bsp_slot;
    uint32_t period_us;
    uint8_t configured;
    uint8_t active;
    uint64_t total_ticks;
    uint64_t early_attempts;
    uint64_t non_bsp_attempts;
    uint64_t timestamp_regressions;
    uint64_t last_tick_ns;
    uint64_t software_timer_polls;
    uint64_t deferred_polls;
} timer_clockevent_snapshot_t;

void timer_init();

uint64_t timer_get_uptime_ms();

task_wait_result_t timer_sleep_interruptible(uint64_t ms);
void timer_sleep(uint64_t ms);

void timer_run_deferred(void);
void timer_run_deferred_at(uint64_t now_ms);

bool timer_clockevent_configure_bsp_lapic(cpu_slot_t bsp_slot,
                                          uint32_t period_us);
bool timer_clockevent_activate(void);
bool timer_clockevent_is_active(void);
void timer_clockevent_on_lapic_tick(cpu_slot_t slot, uint64_t now_ns);
bool timer_clockevent_snapshot(timer_clockevent_snapshot_t *out);
bool timer_clockevent_validate(void);
bool timer_clockevent_model_selftest(void);

#endif
