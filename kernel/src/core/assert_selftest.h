#ifndef ASSERT_SELFTEST_H
#define ASSERT_SELFTEST_H

#include <stdbool.h>
#include <stdint.h>

bool assertion_selftest_early(void);

#if defined(HOBBYOS_ASSERT_TEST)
enum
{
    ASSERTION_TEST_SCENARIO_NONE = 0,
    ASSERTION_TEST_SCENARIO_WARN = 1,
    ASSERTION_TEST_SCENARIO_FALSE = 2,
    ASSERTION_TEST_SCENARIO_VALID_LIST = 3,
    ASSERTION_TEST_SCENARIO_DOUBLE_INSERT = 4,
    ASSERTION_TEST_SCENARIO_INSERT_RECIPROCITY = 5,
    ASSERTION_TEST_SCENARIO_REMOVE_RECIPROCITY = 6,
    ASSERTION_TEST_SCENARIO_BUG = 7,
    ASSERTION_TEST_SCENARIO_LOCKED_BUG = 8,
    ASSERTION_TEST_SCENARIO_REENTRY = 9,
    ASSERTION_TEST_SCENARIO_SECOND_REENTRY = 10
};

typedef struct
{
    volatile uint32_t armed;
    volatile uint32_t scenario;
    volatile uint32_t trigger_slot;
    volatile uint32_t queued;
    volatile uint32_t prepared;
    volatile uint32_t release;
    volatile uint32_t completed;
    volatile uint32_t result;
    volatile uint32_t observed_slot;
    volatile uint32_t observed_cpu_id;
    volatile uint32_t if_before;
    volatile uint32_t if_after;
    volatile uint32_t evaluations;
    volatile uint32_t continuation;
    volatile uint32_t lock_acquired;
    volatile uint32_t arm_error;
    volatile uint64_t lock_addr;
    volatile uint64_t fixture_addr;
    volatile uint64_t fixture_size;
    volatile uint64_t task_id;
    volatile uint64_t task_generation;
    volatile uint32_t context_ready;
    volatile uint32_t context_release;
    volatile uint32_t context_cpu_id;
    volatile uint32_t context_slot;
    volatile uint32_t context_valid_mask;
    volatile uint32_t context_reserved;
    volatile uint64_t context_task_id;
    volatile uint64_t context_task_generation;
} assertion_test_state_t;

extern assertion_test_state_t g_assertion_test_state;
bool assertion_test_timer_hook(uint32_t slot);
void assertion_test_context_observe(uint32_t cpu_id, uint32_t slot,
                                    uint32_t valid_mask, uint64_t task_id,
                                    uint64_t task_generation);
#endif

#endif
