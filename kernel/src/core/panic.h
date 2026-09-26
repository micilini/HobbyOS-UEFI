#include <stdint.h>
#include <stdbool.h>

#ifndef PANIC_H
#define PANIC_H

#ifndef HOBBYOS_DEBUG_ASSERT
#define HOBBYOS_DEBUG_ASSERT 0
#endif

typedef struct
{
    const char *file;
    const char *expression;
    uint32_t line;
} assertion_context_t;

void assertion_warn_report(const assertion_context_t *context);
void assertion_bug_report(const assertion_context_t *context)
    __attribute__((noreturn));

#if HOBBYOS_DEBUG_ASSERT
#define KWARN_ON(condition)                                                   \
    do                                                                        \
    {                                                                         \
        if (!!(condition))                                                    \
        {                                                                     \
            const assertion_context_t assertion_context = {                   \
                .file = __FILE__,                                             \
                .expression = #condition,                                     \
                .line = (uint32_t)__LINE__};                                  \
            assertion_warn_report(&assertion_context);                        \
        }                                                                     \
    } while (0)

#define KBUG_ON(condition)                                                    \
    do                                                                        \
    {                                                                         \
        if (!!(condition))                                                    \
        {                                                                     \
            const assertion_context_t assertion_context = {                   \
                .file = __FILE__,                                             \
                .expression = #condition,                                     \
                .line = (uint32_t)__LINE__};                                  \
            assertion_bug_report(&assertion_context);                         \
        }                                                                     \
    } while (0)
#else
#define KWARN_ON(condition) do { } while (0)
#define KBUG_ON(condition) do { } while (0)
#endif

typedef enum
{
    PANIC_ACTION_HALT = 0,
    PANIC_ACTION_RESTART = 1,
    PANIC_ACTION_SHUTDOWN = 2
} PanicAction;

void panic_config(PanicAction action, uint32_t timeout_seconds);

void kpanic(const char *message) __attribute__((noreturn));

void kpanic_exception_ex(const char *title,
                         uint8_t vector,
                         void *frame,
                         uint64_t error_code,
                         int has_error_code,
                         uint64_t cr2,
                         int has_cr2) __attribute__((noreturn));
void kpanic_exception(const char *title, void *frame, uint64_t error_code,
                      int has_error_code, uint64_t cr2,
                      int has_cr2) __attribute__((noreturn));

bool panic_current_cpu_is_owner(void);
void panic_halt_secondary(void) __attribute__((noreturn));

#if defined(HOBBYOS_PANIC_TEST) && !defined(HOBBYOS_SELFTEST)
#error "HOBBYOS_PANIC_TEST requires HOBBYOS_SELFTEST"
#endif

#if defined(HOBBYOS_PANIC_NEGATIVE_LOCK_WAIT) && \
    !defined(HOBBYOS_PANIC_TEST)
#error "panic negative control requires HOBBYOS_PANIC_TEST"
#endif

#ifdef HOBBYOS_PANIC_TEST

enum
{
    PANIC_TEST_SCENARIO_NONE = 0,
    PANIC_TEST_SCENARIO_SIMPLE = 1,
    PANIC_TEST_SCENARIO_EXCEPTION = 2,
    PANIC_TEST_SCENARIO_CONTENTION = 3,
    PANIC_TEST_SCENARIO_REENTRY = 4,
    PANIC_TEST_SCENARIO_SECOND_REENTRY = 5,
    PANIC_TEST_SCENARIO_CLOCK_STALLED = 6,
    PANIC_TEST_SCENARIO_CLOCK_INVALID = 7,
    PANIC_TEST_SCENARIO_UART_UNRESPONSIVE = 8
};

enum
{
    PANIC_TEST_LOCK_CONSOLE = 1u << 0,
    PANIC_TEST_LOCK_CLOCK = 1u << 1
};

enum
{
    PANIC_TEST_CLOCK_NORMAL = 0,
    PANIC_TEST_CLOCK_CONSTANT = 1,
    PANIC_TEST_CLOCK_INVALID = 2,
    PANIC_TEST_CLOCK_REGRESSING = 3,
    PANIC_TEST_CLOCK_INTERMITTENT = 4
};

typedef struct
{
    volatile uint32_t armed;
    volatile uint32_t scenario;
    volatile uint32_t owner_slot;
    volatile uint32_t peer_slot;
    volatile uint32_t lock_mask;
    volatile uint32_t lock_ack_mask;
    volatile uint32_t fired;
    volatile uint32_t arm_error;
    volatile uint32_t action;
    volatile uint32_t timeout_seconds;
    volatile uint32_t clock_mode;
    volatile uint32_t owner_observed_slot;
    volatile uint32_t peer_observed_slot;
    volatile uint32_t owner_cpu_id;
    volatile uint32_t peer_cpu_id;
    volatile uint32_t reentry_faults;
    volatile uint64_t console_lock_addr;
    volatile uint64_t clock_lock_addr;
    volatile uint64_t console_lock_value;
    volatile uint64_t clock_lock_value;
    volatile uint64_t counter_value;
    volatile uint64_t countdown_samples;
} panic_test_state_t;

extern panic_test_state_t g_panic_test_state;
void panic_test_timer_hook(uint32_t slot);

#endif

#endif
