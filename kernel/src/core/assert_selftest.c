#include "assert_selftest.h"

#include "dpc.h"
#include "idt.h"
#include "list.h"
#include "panic.h"
#include "scheduler.h"
#include "spinlock.h"
#include "../drivers/serial.h"
#include "../libc/string.h"
#include "../smp/cpu_limits.h"
#include "../smp/smp_topology.h"

#include <stddef.h>
#include <stdint.h>

#if !HOBBYOS_DEBUG_ASSERT
#error "assertion selftest requires HOBBYOS_DEBUG_ASSERT=1"
#endif

typedef struct
{
    struct list_head head_a;
    struct list_head head_b;
    struct list_head nodes[4];
} assertion_list_fixture_t;

assertion_test_state_t g_assertion_test_state;
assertion_list_fixture_t g_assertion_test_fixture;

_Static_assert(offsetof(assertion_test_state_t, lock_addr) == 64,
               "assertion test state lock offset");
_Static_assert(offsetof(assertion_test_state_t, fixture_addr) == 72,
               "assertion test state fixture offset");
_Static_assert(offsetof(assertion_test_state_t, task_id) == 88,
               "assertion test state task offset");
_Static_assert(offsetof(assertion_test_state_t, context_ready) == 104,
               "assertion test state context offset");
_Static_assert(offsetof(assertion_test_state_t, context_task_id) == 128,
               "assertion test state context task offset");
_Static_assert(sizeof(assertion_test_state_t) == 144,
               "assertion test state size");

enum assertion_test_record_kind
{
    ASSERTION_TEST_RECORD_EXECUTE = 1,
    ASSERTION_TEST_RECORD_COMPLETE = 2,
    ASSERTION_TEST_RECORD_PREPARED = 3,
    ASSERTION_TEST_RECORD_CONTEXT_READY = 4
};

#define ASSERTION_TEST_RECORD_CAPACITY 96u

_Static_assert(ASSERTION_TEST_RECORD_CAPACITY >= 80u,
               "assertion test record capacity must cover PREPARED");

static int assertion_test_format_record(
    char *record,
    size_t record_capacity,
    enum assertion_test_record_kind kind,
    uint32_t scenario,
    uint32_t slot,
    uint32_t cpu_id,
    uint32_t value)
{
    switch (kind)
    {
        case ASSERTION_TEST_RECORD_EXECUTE:
            return ksnprintf(
                record, record_capacity,
                "\n[ASSERT_TEST][EXECUTE] scenario=%u\n",
                (unsigned int)scenario);
        case ASSERTION_TEST_RECORD_COMPLETE:
            return ksnprintf(
                record, record_capacity,
                "\n[ASSERT_TEST][COMPLETE] scenario=%u status=%s\n",
                (unsigned int)scenario, value ? "PASS" : "FAIL");
        case ASSERTION_TEST_RECORD_PREPARED:
            return ksnprintf(
                record, record_capacity,
                "\n[ASSERT_TEST][PREPARED] scenario=%u slot=%u cpu_id=%u\n",
                (unsigned int)scenario, (unsigned int)slot,
                (unsigned int)cpu_id);
        case ASSERTION_TEST_RECORD_CONTEXT_READY:
            return ksnprintf(
                record, record_capacity,
                "\n[ASSERT_TEST][CONTEXT_READY] valid_mask=%u\n",
                (unsigned int)value);
        default:
            return -1;
    }
}

static void assertion_test_mark_record_error(void)
{
    __atomic_store_n(&g_assertion_test_state.result, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_assertion_test_state.arm_error, 1,
                     __ATOMIC_RELEASE);
}

static void assertion_test_record_error(void)
{
    assertion_test_mark_record_error();
    serial_write_all("[ASSERT_TEST][RECORD_ERROR] status=FAIL\n");
}

static bool assertion_test_emit_record_with_capacity(
    size_t record_capacity,
    enum assertion_test_record_kind kind,
    uint32_t scenario,
    uint32_t slot,
    uint32_t cpu_id,
    uint32_t value)
{
    char record[ASSERTION_TEST_RECORD_CAPACITY];
    if (record_capacity > sizeof(record))
        record_capacity = sizeof(record);
    int required_length = assertion_test_format_record(
        record, record_capacity, kind, scenario, slot, cpu_id, value);
    if (required_length < 0 ||
        (size_t)required_length >= record_capacity)
    {
        assertion_test_record_error();
        return false;
    }
    serial_write_all(record);
    return true;
}

static bool assertion_test_emit_record(
    enum assertion_test_record_kind kind,
    uint32_t scenario,
    uint32_t slot,
    uint32_t cpu_id,
    uint32_t value)
{
    return assertion_test_emit_record_with_capacity(
        ASSERTION_TEST_RECORD_CAPACITY, kind, scenario, slot, cpu_id, value);
}

#if defined(HOBBYOS_ASSERT_RECORD_HOST_TEST)
int assertion_test_format_record_for_test(
    char *record,
    size_t record_capacity,
    uint32_t kind,
    uint32_t scenario,
    uint32_t slot,
    uint32_t cpu_id,
    uint32_t value)
{
    return assertion_test_format_record(
        record, record_capacity, (enum assertion_test_record_kind)kind,
        scenario, slot, cpu_id, value);
}

bool assertion_test_emit_record_for_test(
    size_t record_capacity,
    uint32_t kind,
    uint32_t scenario,
    uint32_t slot,
    uint32_t cpu_id,
    uint32_t value)
{
    return assertion_test_emit_record_with_capacity(
        record_capacity, (enum assertion_test_record_kind)kind,
        scenario, slot, cpu_id, value);
}
#endif

static void assertion_test_fixture_init(void)
{
    list_init(&g_assertion_test_fixture.head_a);
    list_init(&g_assertion_test_fixture.head_b);
    for (uint32_t index = 0; index < 4u; index++)
        list_init(&g_assertion_test_fixture.nodes[index]);
}

bool assertion_selftest_early(void)
{
    uint32_t evaluations = 0;
    uint32_t if_before = irq_are_enabled() ? 1u : 0u;
    serial_write_all("[ASSERT_TEST][EARLY_BEGIN] debug=1 stage=post-idt\n");
    KWARN_ON(++evaluations == 0u);
    KBUG_ON(++evaluations == 0u);
    KWARN_ON(++evaluations == 3u);
    uint32_t if_after = irq_are_enabled() ? 1u : 0u;
    bool pass = evaluations == 3u && if_before == 0u && if_after == 0u;
    serial_write_all(pass
        ? "[ASSERT_TEST][EARLY_END] status=PASS evaluations=3 continuation=1 if_before=0 if_after=0 task_expected=unknown\n"
        : "[ASSERT_TEST][EARLY_END] status=FAIL\n");
    return pass;
}

static bool assertion_test_lock_address_valid(uint64_t address)
{
    return address >= 0x1000ULL &&
           (address & (_Alignof(spinlock_t) - 1u)) == 0;
}

static void assertion_test_mark_observed(void)
{
    cpu_slot_t slot = CPU_SLOT_INVALID;
    if (smp_current_cpu_slot(&slot) && slot < HOBBYOS_MAX_CPUS)
    {
        __atomic_store_n(&g_assertion_test_state.observed_slot, slot,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&g_assertion_test_state.observed_cpu_id,
                         g_cpus[slot].apic_id, __ATOMIC_RELAXED);
    }
    else
    {
        __atomic_store_n(&g_assertion_test_state.observed_slot,
                         CPU_SLOT_INVALID, __ATOMIC_RELAXED);
        __atomic_store_n(&g_assertion_test_state.observed_cpu_id,
                         UINT32_MAX, __ATOMIC_RELAXED);
    }
}

static void assertion_test_prepare_fixture(uint32_t scenario)
{
    assertion_test_fixture_init();
    if (scenario == ASSERTION_TEST_SCENARIO_DOUBLE_INSERT)
    {
        list_add_tail(&g_assertion_test_fixture.nodes[0],
                      &g_assertion_test_fixture.head_a);
    }
    else if (scenario == ASSERTION_TEST_SCENARIO_INSERT_RECIPROCITY)
    {
        struct list_head *prev = &g_assertion_test_fixture.nodes[0];
        struct list_head *next = &g_assertion_test_fixture.nodes[1];
        prev->next = &g_assertion_test_fixture.head_a;
        prev->prev = &g_assertion_test_fixture.head_a;
        next->prev = prev;
        next->next = &g_assertion_test_fixture.head_a;
    }
    else if (scenario == ASSERTION_TEST_SCENARIO_REMOVE_RECIPROCITY)
    {
        list_add_tail(&g_assertion_test_fixture.nodes[0],
                      &g_assertion_test_fixture.head_a);
        g_assertion_test_fixture.head_a.next =
            &g_assertion_test_fixture.head_a;
    }
}

static void assertion_test_complete(uint32_t pass)
{
    uint32_t scenario = __atomic_load_n(&g_assertion_test_state.scenario,
                                        __ATOMIC_RELAXED);
    char record[ASSERTION_TEST_RECORD_CAPACITY];
    int required_length = assertion_test_format_record(
        record, sizeof(record), ASSERTION_TEST_RECORD_COMPLETE,
        scenario, 0, 0, pass);
    bool record_ready = required_length >= 0 &&
                        (size_t)required_length < sizeof(record);
    if (!record_ready)
    {
        assertion_test_mark_record_error();
        pass = 0;
    }
    __atomic_store_n(&g_assertion_test_state.result, pass,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_assertion_test_state.completed, 1,
                     __ATOMIC_RELEASE);
    if (record_ready)
        serial_write_all(record);
    else
        serial_write_all("[ASSERT_TEST][RECORD_ERROR] status=FAIL\n");
}

void assertion_test_context_observe(uint32_t cpu_id, uint32_t slot,
                                    uint32_t valid_mask, uint64_t task_id,
                                    uint64_t task_generation)
{
    if (!__atomic_load_n(&g_assertion_test_state.armed, __ATOMIC_ACQUIRE))
        return;
    __atomic_store_n(&g_assertion_test_state.context_cpu_id, cpu_id,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_assertion_test_state.context_slot, slot,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_assertion_test_state.context_valid_mask, valid_mask,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_assertion_test_state.context_task_id, task_id,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_assertion_test_state.context_task_generation,
                     task_generation, __ATOMIC_RELAXED);
    __atomic_store_n(&g_assertion_test_state.context_ready, 1,
                     __ATOMIC_RELEASE);
    if (!assertion_test_emit_record(
            ASSERTION_TEST_RECORD_CONTEXT_READY, 0, 0, 0, valid_mask))
        return;
    while (!__atomic_load_n(&g_assertion_test_state.context_release,
                            __ATOMIC_ACQUIRE))
        __asm__ volatile("pause" ::: "memory");
}

static irq_flags_t assertion_test_wait_for_observer(bool *record_emitted)
{
    irq_flags_t flags = irq_save();
    assertion_test_mark_observed();
    task_t *task = get_current_task();
    if (task && task->id != TASK_ID_INVALID &&
        task->lifecycle_generation != 0)
    {
        __atomic_store_n(&g_assertion_test_state.task_id, task->id,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&g_assertion_test_state.task_generation,
                         task->lifecycle_generation, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&g_assertion_test_state.prepared, 1,
                     __ATOMIC_RELEASE);
    *record_emitted = assertion_test_emit_record(
        ASSERTION_TEST_RECORD_PREPARED,
        __atomic_load_n(&g_assertion_test_state.scenario, __ATOMIC_RELAXED),
        __atomic_load_n(&g_assertion_test_state.observed_slot,
                        __ATOMIC_RELAXED),
        __atomic_load_n(&g_assertion_test_state.observed_cpu_id,
                        __ATOMIC_RELAXED),
        0);
    if (!*record_emitted)
        return flags;
    while (!__atomic_load_n(&g_assertion_test_state.release,
                            __ATOMIC_ACQUIRE))
        __asm__ volatile("pause" ::: "memory");
    return flags;
}

static void assertion_test_runtime(void *unused)
{
    (void)unused;
    uint32_t scenario = __atomic_load_n(&g_assertion_test_state.scenario,
                                        __ATOMIC_ACQUIRE);
    assertion_test_prepare_fixture(scenario);
    __atomic_store_n(&g_assertion_test_state.fixture_addr,
                     (uint64_t)(uintptr_t)&g_assertion_test_fixture,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_assertion_test_state.fixture_size,
                     sizeof(g_assertion_test_fixture), __ATOMIC_RELAXED);
    bool prepared_record_emitted = false;
    irq_flags_t observer_flags =
        assertion_test_wait_for_observer(&prepared_record_emitted);
    irq_restore(observer_flags);
    if (!prepared_record_emitted)
    {
        assertion_test_complete(0);
        return;
    }
    if (!assertion_test_emit_record(
            ASSERTION_TEST_RECORD_EXECUTE, scenario, 0, 0, 0))
    {
        assertion_test_complete(0);
        return;
    }

    if (scenario == ASSERTION_TEST_SCENARIO_WARN)
    {
        uint32_t evaluations = 0;
        uint32_t before = irq_are_enabled() ? 1u : 0u;
        KWARN_ON(++evaluations == 1u);
        uint32_t after = irq_are_enabled() ? 1u : 0u;
        __atomic_store_n(&g_assertion_test_state.evaluations, evaluations,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&g_assertion_test_state.continuation, 1,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&g_assertion_test_state.if_before, before,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&g_assertion_test_state.if_after, after,
                         __ATOMIC_RELAXED);
        assertion_test_complete(evaluations == 1u && before == after);
        return;
    }
    if (scenario == ASSERTION_TEST_SCENARIO_FALSE)
    {
        uint32_t evaluations = 0;
        KWARN_ON(++evaluations == 0u);
        KBUG_ON(++evaluations == 0u);
        __atomic_store_n(&g_assertion_test_state.evaluations, evaluations,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&g_assertion_test_state.continuation, 1,
                         __ATOMIC_RELAXED);
        assertion_test_complete(evaluations == 2u);
        return;
    }
    if (scenario == ASSERTION_TEST_SCENARIO_VALID_LIST)
    {
        struct list_head *head_a = &g_assertion_test_fixture.head_a;
        struct list_head *head_b = &g_assertion_test_fixture.head_b;
        struct list_head *first = &g_assertion_test_fixture.nodes[0];
        struct list_head *second = &g_assertion_test_fixture.nodes[1];
        list_add(first, head_a);
        list_add_tail(second, head_a);
        list_del(first);
        list_add_tail(first, head_b);
        bool pass = head_a->next == second && head_a->prev == second &&
                    head_b->next == first && head_b->prev == first &&
                    first->next == head_b && first->prev == head_b;
        assertion_test_complete(pass ? 1u : 0u);
        return;
    }

    panic_config(PANIC_ACTION_HALT, 1u);
    if (scenario == ASSERTION_TEST_SCENARIO_DOUBLE_INSERT)
        list_add_tail(&g_assertion_test_fixture.nodes[0],
                      &g_assertion_test_fixture.head_a);
    else if (scenario == ASSERTION_TEST_SCENARIO_INSERT_RECIPROCITY)
        __list_add(&g_assertion_test_fixture.nodes[2],
                   &g_assertion_test_fixture.nodes[0],
                   &g_assertion_test_fixture.nodes[1]);
    else if (scenario == ASSERTION_TEST_SCENARIO_REMOVE_RECIPROCITY)
        list_del(&g_assertion_test_fixture.nodes[0]);
    else if (scenario == ASSERTION_TEST_SCENARIO_LOCKED_BUG)
    {
        uint64_t address = __atomic_load_n(&g_assertion_test_state.lock_addr,
                                           __ATOMIC_RELAXED);
        if (!assertion_test_lock_address_valid(address))
        {
            __atomic_store_n(&g_assertion_test_state.arm_error, 1,
                             __ATOMIC_RELEASE);
            assertion_test_complete(0);
            return;
        }
        spinlock_t *lock = (spinlock_t *)(uintptr_t)address;
        (void)spin_lock_irqsave(lock);
        __atomic_store_n(&g_assertion_test_state.lock_acquired,
                         __atomic_load_n(&lock->locked, __ATOMIC_RELAXED),
                         __ATOMIC_RELEASE);
        serial_write_all("[ASSERT_TEST][LOCK_HELD] acquired=1\n");
        KBUG_ON(g_assertion_test_state.lock_acquired == 1u);
    }
    else if (scenario == ASSERTION_TEST_SCENARIO_BUG ||
             scenario == ASSERTION_TEST_SCENARIO_REENTRY ||
             scenario == ASSERTION_TEST_SCENARIO_SECOND_REENTRY)
        KBUG_ON(scenario != ASSERTION_TEST_SCENARIO_NONE);
    else
    {
        __atomic_store_n(&g_assertion_test_state.arm_error, 1,
                         __ATOMIC_RELEASE);
        assertion_test_complete(0);
        return;
    }

    assertion_test_complete(0);
}

bool assertion_test_timer_hook(uint32_t slot)
{
    if (!__atomic_load_n(&g_assertion_test_state.armed, __ATOMIC_ACQUIRE))
        return false;
    uint32_t trigger = __atomic_load_n(&g_assertion_test_state.trigger_slot,
                                       __ATOMIC_RELAXED);
    if (trigger >= HOBBYOS_MAX_CPUS)
    {
        __atomic_store_n(&g_assertion_test_state.arm_error, 1,
                         __ATOMIC_RELEASE);
        return true;
    }
    if (slot != trigger)
        return true;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&g_assertion_test_state.queued,
                                     &expected, 1, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return true;
    if (dpc_enqueue(assertion_test_runtime, NULL) != 0)
    {
        __atomic_store_n(&g_assertion_test_state.arm_error, 1,
                         __ATOMIC_RELEASE);
        assertion_test_complete(0);
    }
    return true;
}
