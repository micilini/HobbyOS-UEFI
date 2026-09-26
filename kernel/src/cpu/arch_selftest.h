#ifndef ARCH_SELFTEST_H
#define ARCH_SELFTEST_H

#include <stdbool.h>
#include <stdint.h>

#include "../smp/cpu_limits.h"

#if defined(HOBBYOS_ARCH_TEST) || defined(HOBBYOS_ARCH_HOST_TEST)
typedef bool (*arch_test_poll_condition_t)(void *context);

static inline bool arch_test_poll_budget(arch_test_poll_condition_t condition,
                                         void *context, uint32_t budget)
{
    if (!condition)
        return false;
    for (;;)
    {
        if (condition(context))
            return true;
        if (budget == 0)
            return false;
        budget--;
        __asm__ volatile("pause");
    }
}
#endif

#if defined(HOBBYOS_ARCH_TEST) && !defined(HOBBYOS_SELFTEST)
#error "architecture selftest requires HOBBYOS_SELFTEST"
#endif

#if defined(HOBBYOS_ARCH_TEST) && !HOBBYOS_DEBUG_ASSERT
#error "architecture selftest requires HOBBYOS_DEBUG_ASSERT=1"
#endif

#if defined(HOBBYOS_ARCH_NEGATIVE_GP) && !defined(HOBBYOS_ARCH_TEST)
#error "architecture #GP negative requires HOBBYOS_ARCH_TEST"
#endif

#ifdef HOBBYOS_ARCH_TEST
typedef struct
{
    volatile uint32_t early_done;
    volatile uint32_t early_pass;
    volatile uint32_t safe_read_failure;
    volatile uint32_t safe_write_failure;
    volatile uint32_t runtime_started;
    volatile uint32_t target_count;
    volatile uint32_t joined_mask;
    volatile uint32_t active_mask;
    volatile uint32_t complete_mask;
    volatile uint32_t overlap_mask;
    volatile uint32_t snapshot_mask;
    volatile uint32_t error;
    volatile uint32_t iterations[HOBBYOS_MAX_CPUS];
    volatile uint32_t observed_cpu_id[HOBBYOS_MAX_CPUS];
    volatile uint32_t observed_slot[HOBBYOS_MAX_CPUS];
    volatile uint64_t observed_address[HOBBYOS_MAX_CPUS];
    volatile uint64_t observed_value[HOBBYOS_MAX_CPUS];
    volatile uint32_t observed_meta[HOBBYOS_MAX_CPUS];
    volatile uint32_t observer_ready;
    volatile uint32_t observer_release;
    volatile uint32_t publication_ready_mask;
    volatile uint32_t sampling_release;
    volatile uint32_t writers_started_mask;
    volatile uint32_t writers_finished_mask;
    volatile uint32_t concurrent_samples;
    volatile uint32_t concurrent_unavailable;
    volatile uint32_t concurrent_active_samples;
    volatile uint32_t concurrent_progress_mask;
    volatile uint32_t observer_budget_pass;
    volatile uint32_t pin_task_started;
    volatile uint32_t pin_task_done;
    volatile uint32_t pin_task_pass;
    volatile uint32_t pin_if_before;
    volatile uint32_t pin_if_during;
    volatile uint32_t pin_if_after;
    volatile uint32_t pin_cpu_before;
    volatile uint32_t pin_cpu_during;
    volatile uint32_t pin_cpu_after;
    volatile uint32_t pin_slot;
    volatile uint32_t pin_snapshot_if1;
    volatile uint32_t pin_snapshot_if0;
    volatile uint32_t budget_checks;
    volatile uint32_t concurrent_first_sequence[HOBBYOS_MAX_CPUS];
    volatile uint32_t concurrent_last_sequence[HOBBYOS_MAX_CPUS];
} arch_test_state_t;

extern arch_test_state_t g_arch_test_state;

bool arch_selftest_early(void);
bool arch_selftest_start_runtime_task(void);
void arch_selftest_gdb_done(void);
void arch_test_runtime_complete_point(void);
void arch_test_timer_hook(uint32_t slot);

typedef struct
{
    volatile uint32_t count;
    volatile uint32_t context_valid;
    volatile uint64_t rip;
    volatile uint64_t error_code;
    volatile uint64_t cs;
} arch_test_unhandled_gp_observation_t;

extern arch_test_unhandled_gp_observation_t
    g_arch_test_unhandled_gp_observation;

void arch_selftest_record_unhandled_gp(uint64_t rip, uint64_t error_code,
                                       uint64_t cs);
#endif

#endif
