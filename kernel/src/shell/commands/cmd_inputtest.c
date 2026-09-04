#include "cmd_inputtest.h"
#include "../../core/input_queue.h"
#include "../../core/input_router.h"
#include "../../core/modal_session.h"
#include "../../core/scheduler.h"
#include "../../drivers/timer.h"
#include "../../core/clock.h"
#include "../../drivers/keyboard.h"
#include "../../drivers/serial.h"
#include "../../libc/string.h"
#include "../../libc/memory.h"
#include "../shell.h"
#include <stdint.h>

#define TEST_CAPACITY 256u
static input_queue_t g_test_queue;
static input_queue_entry_t g_test_storage[TEST_CAPACITY];
static volatile uint32_t g_wait_stage;
static volatile uint32_t g_wait_done;
static volatile uint32_t g_producer_done;
static volatile uint64_t g_producer_accepted;
static volatile uint64_t g_consumer_count;
static volatile uint64_t g_consumer_chars, g_consumer_specials;
static volatile uint64_t g_sequence_gaps, g_sequence_duplicates;
static volatile uint64_t g_sequence_regressions, g_last_sequence;
static uint32_t g_producer_each;
static volatile uint32_t g_boundary_attempted;
static volatile uint32_t g_boundary_producer_done, g_boundary_transition_done;
static volatile uint32_t g_boundary_mode;
static volatile uint32_t g_drain_waiter_ready, g_drain_waiter_done;
static volatile uint8_t g_drain_waiter_value;
static volatile uint32_t g_negative_waiter_done;
static volatile uint32_t g_negative_waiter_result;
static volatile uint32_t g_shell_pause_worker_done;
static volatile uint32_t g_shell_pause_first_delivered;
static volatile uint32_t g_shell_pause_second_delivered;
static volatile uint32_t g_preblock_started, g_preblock_release, g_preblock_returned;
static volatile uint32_t g_preblock_result;
static task_handle_t g_preblock_handle;
static volatile uint32_t g_preblock_claimed;

typedef struct {
    bool passed;
    task_handle_t handle;
    input_queue_pop_result_t pop_result;
    task_exit_reason_t exit_reason;
    task_snapshot_t final;
    input_queue_stats_t queue;
    uint64_t waiter_residual;
    uint64_t residual;
    bool handle_gone;
    bool free_inflight_zero;
    bool cleanup_ok;
} input_preblock_result_t;

static bool wait_until(volatile uint32_t *value, uint32_t expected,
                       uint32_t timeout_ms);
static bool reset_test_queue(uint32_t capacity);
static void put_u64(uint64_t v);

static void preblock_input_worker(void *arg)
{
    (void)arg;
    __atomic_store_n(&g_preblock_started, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&g_preblock_release, __ATOMIC_ACQUIRE)) spin_cpu_relax();
    input_queue_entry_t entry;
    input_queue_pop_result_t result = input_queue_wait_pop_entry(&g_test_queue, &entry);
    __atomic_store_n(&g_preblock_result, (uint32_t)result, __ATOMIC_RELEASE);
    __atomic_store_n(&g_preblock_returned, 1, __ATOMIC_RELEASE);
    task_cancel_point();
}

static bool input_preblock_waiters_empty(void)
{
    irq_flags_t flags = spin_lock_irqsave(&g_test_queue.lock);
    bool empty = list_empty(&g_test_queue.waiters.head);
    spin_unlock_irqrestore(&g_test_queue.lock, flags);
    return empty;
}

static bool input_preblock_cleanup(input_preblock_result_t *result)
{
    __atomic_store_n(&g_preblock_release, 1, __ATOMIC_RELEASE);
    task_snapshot_t snapshot = {0};
    bool present = result->handle.id != TASK_ID_INVALID &&
        scheduler_snapshot_task_by_handle(result->handle, &snapshot);
    bool kill_ok = true;
    if (present && snapshot.state != TASK_ZOMBIE) {
        task_kill_result_t kill = scheduler_request_kill(result->handle.id);
        if (kill != TASK_KILL_ACCEPTED && kill != TASK_KILL_ALREADY_PENDING &&
            kill != TASK_KILL_ALREADY_EXITING &&
            kill != TASK_KILL_ALREADY_ZOMBIE)
            kill_ok = false;
    }

    uint64_t deadline = clock_monotonic_ns() + 10000000000ULL;
    while (scheduler_snapshot_task_by_handle(result->handle, &snapshot) &&
           snapshot.state != TASK_ZOMBIE && clock_monotonic_ns() < deadline)
        timer_sleep(1);
    bool zombie = scheduler_snapshot_task_by_handle(result->handle,
                                                      &snapshot) &&
                  snapshot.state == TASK_ZOMBIE;
    if (zombie) {
        result->final = snapshot;
        result->exit_reason = snapshot.exit_reason;
    }

    input_queue_validation_t validation = {0};
    (void)input_queue_snapshot(&g_test_queue, &result->queue);
    bool waiters_empty = input_preblock_waiters_empty();
    bool exact_wait_clean = zombie && !snapshot.wait_active &&
        snapshot.wait_kind == TASK_WAIT_NONE &&
        snapshot.queue_membership == TASK_QUEUE_NONE;
    bool queue_clean = result->queue.count == 0 && waiters_empty &&
        !result->queue.phantom_empty_wakes &&
        input_queue_validate(&g_test_queue, &validation);
    result->waiter_residual = waiters_empty ? 0 : 1;

    if (zombie)
        (void)scheduler_test_hold_reap(result->handle.id, false);
    task_reaper_stats_t reaper = {0};
    deadline = clock_monotonic_ns() + 10000000000ULL;
    do {
        (void)scheduler_reap_zombies(0);
        scheduler_reaper_stats_snapshot(&reaper);
        result->handle_gone = !scheduler_snapshot_task_by_handle(
            result->handle, &snapshot);
        result->free_inflight_zero = reaper.free_inflight == 0;
        if (result->handle_gone && result->free_inflight_zero)
            break;
        timer_sleep(1);
    } while (clock_monotonic_ns() < deadline);

    result->residual = result->waiter_residual + result->queue.count +
        result->queue.phantom_empty_wakes + !exact_wait_clean +
        !result->handle_gone + !result->free_inflight_zero;
    result->cleanup_ok = kill_ok && zombie && exact_wait_clean && queue_clean &&
        result->handle_gone && result->free_inflight_zero;
    return result->cleanup_ok;
}

static bool run_input_preblock_case_result(bool quiet,
                                            input_preblock_result_t *result)
{
    if (!result || __atomic_exchange_n(&g_preblock_claimed, 1,
                                        __ATOMIC_ACQ_REL))
        return false;
    memset(result, 0, sizeof(*result));
    result->pop_result = INPUT_QUEUE_POP_ERROR;
    if (!reset_test_queue(8)) {
        __atomic_store_n(&g_preblock_claimed, 0, __ATOMIC_RELEASE);
        return false;
    }
    __atomic_store_n(&g_preblock_started, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_preblock_release, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_preblock_returned, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_preblock_result, INPUT_QUEUE_POP_ERROR,
                     __ATOMIC_RELEASE);
    scheduler_wait_stats_t before = {0}, after = {0};
    scheduler_wait_stats_snapshot(&before);
    task_create_options_t options = {
        .name = "input-preblock",
        .task_class = TASK_CLASS_NORMAL,
        .flags = TASK_FLAG_SYSTEM | TASK_FLAG_KILLABLE,
        .test_reap_hold = 1
    };
    bool created = thread_create_ex_handle(preblock_input_worker, NULL,
                                            &options, &g_preblock_handle);
    if (!created) {
        __atomic_store_n(&g_preblock_claimed, 0, __ATOMIC_RELEASE);
        return false;
    }
    result->handle = g_preblock_handle;
    bool started = wait_until(&g_preblock_started, 1, 5000);
    task_kill_result_t kill = started ? scheduler_request_kill(
        g_preblock_handle.id) : TASK_KILL_ERR_INVALID;
    bool accepted = kill == TASK_KILL_ACCEPTED;
    __atomic_store_n(&g_preblock_release, 1, __ATOMIC_RELEASE);
    bool returned = wait_until(&g_preblock_returned, 1, 5000);
    if (returned)
        result->pop_result = (input_queue_pop_result_t)__atomic_load_n(
            &g_preblock_result, __ATOMIC_ACQUIRE);
    scheduler_wait_stats_snapshot(&after);

    bool cleaned = input_preblock_cleanup(result);
    bool validated = started && accepted && returned &&
        result->pop_result == INPUT_QUEUE_POP_CANCELLED &&
        after.preblock_cancelled == before.preblock_cancelled + 1 &&
        result->exit_reason == TASK_EXIT_KILLED &&
        result->queue.cancelled_waits == 1;
    result->passed = validated && cleaned;
    if (!quiet) {
        serial_write_all(result->passed ?
            "[INPUTTEST][PREBLOCK_CANCEL] PASS" :
            "[INPUTTEST][PREBLOCK_CANCEL] FAIL");
        serial_write_all(" cancelled=");
        put_u64(result->pop_result == INPUT_QUEUE_POP_CANCELLED);
        serial_write_all(" killed=");
        put_u64(result->exit_reason == TASK_EXIT_KILLED);
        serial_write_all(" waiters="); put_u64(result->waiter_residual);
        serial_write_all(" phantom=");
        put_u64(result->queue.phantom_empty_wakes);
        serial_write_all(" handle_gone="); put_u64(result->handle_gone);
        serial_write_all(" free_inflight=");
        put_u64(result->free_inflight_zero ? 0 : 1);
        serial_write_all(" residual="); put_u64(result->residual);
        serial_write_all("\n");
    }
    if (cleaned)
        __atomic_store_n(&g_preblock_claimed, 0, __ATOMIC_RELEASE);
    return result->passed;
}

static int run_input_preblock_case(bool quiet)
{
    input_preblock_result_t result;
    return run_input_preblock_case_result(quiet, &result) ? 0 : 1;
}

static int test_input_preblock_loop(uint32_t count)
{
    uint32_t passed = 0, cancelled = 0, killed = 0, normal = 0;
    uint32_t handles_gone = 0;
    uint64_t waiters = 0, phantom = 0, residual = 0;
    bool free_inflight_zero = true;
    scheduler_test_set_lifecycle_log_quiet(true);
    while (passed < count) {
        input_preblock_result_t result;
        bool ok = run_input_preblock_case_result(true, &result);
        cancelled += result.pop_result == INPUT_QUEUE_POP_CANCELLED;
        killed += result.exit_reason == TASK_EXIT_KILLED;
        normal += result.exit_reason == TASK_EXIT_NORMAL;
        handles_gone += result.handle_gone;
        free_inflight_zero = free_inflight_zero &&
                             result.free_inflight_zero;
        waiters += result.waiter_residual;
        phantom += result.queue.phantom_empty_wakes;
        residual += result.residual;
        if (!ok)
            break;
        passed++;
    }
    scheduler_test_set_lifecycle_log_quiet(false);
    scheduler_runtime_stats_t stats = {0};
    bool ok = count && passed == count && scheduler_validate_runtime_invariants(&stats) &&
              stats.pending_cancel_wait_violations == 0 &&
              cancelled == count && killed == count && !normal &&
              !waiters && !phantom && handles_gone == count &&
              free_inflight_zero && !residual;
    serial_write_all(ok ? "[INPUTTEST][PREBLOCK_LOOP] PASS count=" :
                          "[INPUTTEST][PREBLOCK_LOOP] FAIL count=");
    put_u64(count); serial_write_all(" cancelled="); put_u64(cancelled);
    serial_write_all(" killed="); put_u64(killed);
    serial_write_all(" normal="); put_u64(normal);
    serial_write_all(" waiters="); put_u64(waiters);
    serial_write_all(" phantom="); put_u64(phantom);
    serial_write_all(" handles_gone="); put_u64(handles_gone);
    serial_write_all(" free_inflight=");
    put_u64(free_inflight_zero ? 0 : 1);
    serial_write_all(" residual="); put_u64(residual);
    serial_write_all("\n");
    return ok ? 0 : 1;
}

static void put_u64(uint64_t v)
{
    char b[21]; uint32_t n = 0;
    do { b[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n) serial_putc_all(b[--n]);
}

static uint32_t parse_u32(const char *s, uint32_t fallback)
{
    uint32_t v = 0;
    if (!s || !*s) return fallback;
    while (*s) {
        uint32_t d;
        if (*s < '0' || *s > '9') return fallback;
        d = (uint32_t)(*s++ - '0');
        if (v > (UINT32_MAX - d) / 10u) return fallback;
        v = v * 10u + d;
    }
    return v;
}

static bool reset_test_queue(uint32_t capacity)
{
    if (!capacity || capacity > TEST_CAPACITY) return false;
    return input_queue_init(&g_test_queue, g_test_storage, capacity, "inputtest");
}

static int test_queue(void)
{
    input_queue_entry_t e;
    bool ok = reset_test_queue(8);
    for (uint32_t i = 0; ok && i < 8; i++)
        ok = input_queue_push(&g_test_queue, input_event_char((char)('a' + i)), 1,
                              INPUT_DEST_TEST, &e) == INPUT_QUEUE_PUSH_OK &&
             e.queue_sequence == i + 1u;
    ok = ok && input_queue_push(&g_test_queue, input_event_char('x'), 1,
                                 INPUT_DEST_TEST, NULL) == INPUT_QUEUE_PUSH_FULL;
    for (uint32_t i = 0; ok && i < 4; i++)
        ok = input_queue_try_pop_entry(&g_test_queue, &e) == INPUT_QUEUE_POP_OK &&
             e.event.value == (uint8_t)('a' + i);
    for (uint32_t i = 0; ok && i < 4; i++)
        ok = input_queue_push(&g_test_queue, input_event_char((char)('i' + i)), 1,
                              INPUT_DEST_TEST, NULL) == INPUT_QUEUE_PUSH_OK;
    const char expected[] = "efghijkl";
    for (uint32_t i = 0; ok && i < 8; i++)
        ok = input_queue_try_pop_entry(&g_test_queue, &e) == INPUT_QUEUE_POP_OK &&
             e.event.value == (uint8_t)expected[i];
    input_queue_validation_t validation;
    ok = ok && input_queue_validate(&g_test_queue, &validation);
    serial_write_all(ok ? "[INPUTTEST][QUEUE] PASS capacity=8 wrap=1 drops=1 order=1\n" :
                          "[INPUTTEST][QUEUE] FAIL\n");
    return ok ? 0 : 1;
}

static void drain_waiter_worker(void *arg)
{
    (void)arg;
    __atomic_store_n(&g_drain_waiter_ready, 1, __ATOMIC_RELEASE);
    input_event_t event;
    if (input_queue_wait_pop(&g_test_queue, &event) == INPUT_QUEUE_POP_OK)
        __atomic_store_n(&g_drain_waiter_value, event.value, __ATOMIC_RELEASE);
    __atomic_store_n(&g_drain_waiter_done, 1, __ATOMIC_RELEASE);
}

static int test_drain(void)
{
    bool ok = reset_test_queue(256); input_event_t event;
    for (uint32_t i = 0; ok && i < 256; i++)
        ok = input_queue_push(&g_test_queue, input_event_char((char)(i | 1u)), 1,
                              INPUT_DEST_TEST, NULL) == INPUT_QUEUE_PUSH_OK;
    for (uint32_t i = 0; ok && i < 73; i++)
        ok = input_queue_try_pop(&g_test_queue, &event) == INPUT_QUEUE_POP_OK;
    uint32_t drained = input_queue_drain(&g_test_queue);
    input_queue_stats_t s; input_queue_snapshot(&g_test_queue, &s);
    ok = ok && drained == 183 && s.count == 0 &&
         input_queue_try_pop(&g_test_queue, &event) == INPUT_QUEUE_POP_EMPTY;
    __atomic_store_n(&g_drain_waiter_ready, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_drain_waiter_done, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_drain_waiter_value, 0, __ATOMIC_RELEASE);
    task_handle_t handle;
    ok = ok && thread_create_named_handle(drain_waiter_worker, NULL,
                                           "input-drain-waiter", &handle) &&
         wait_until(&g_drain_waiter_ready, 1, 1000);
    task_snapshot_t waiter = {0};
    uint64_t deadline = clock_monotonic_ns() + 1000000000ULL;
    bool blocked = false;
    while (!blocked && clock_monotonic_ns() < deadline) {
        blocked = scheduler_snapshot_task_by_id(handle.id, &waiter) &&
                  waiter.state == TASK_BLOCKED && waiter.wait_active &&
                  waiter.wait_kind == TASK_WAIT_INPUT_QUEUE &&
                  waiter.queue_membership == TASK_QUEUE_WAIT;
        if (!blocked) timer_sleep(1);
    }
    timer_sleep(20);
    ok = ok && blocked && !__atomic_load_n(&g_drain_waiter_done,
                                           __ATOMIC_ACQUIRE);
    ok = ok && input_queue_push(&g_test_queue, input_event_char('D'), 1,
                                 INPUT_DEST_TEST, NULL) == INPUT_QUEUE_PUSH_OK &&
         wait_until(&g_drain_waiter_done, 1, 1000) &&
         __atomic_load_n(&g_drain_waiter_value, __ATOMIC_ACQUIRE) == 'D';
    input_queue_snapshot(&g_test_queue, &s);
    ok = ok && s.count == 0 && s.phantom_empty_wakes == 0;
    serial_write_all(ok ? "[INPUTTEST][DRAIN] PASS popped=73 drained=183 phantom=0 real_wakes=1\n" :
                          "[INPUTTEST][DRAIN] FAIL\n");
    return ok ? 0 : 1;
}

static void wait_test_worker(void *arg)
{
    (void)arg; input_event_t e;
    __atomic_store_n(&g_wait_stage, 1, __ATOMIC_RELEASE);
    if (input_queue_wait_pop(&g_test_queue, &e) == INPUT_QUEUE_POP_OK && e.value == 'R')
        __atomic_store_n(&g_wait_stage, 2, __ATOMIC_RELEASE);
    if (input_queue_wait_pop(&g_test_queue, &e) == INPUT_QUEUE_POP_OK && e.value == 'S')
        __atomic_store_n(&g_wait_stage, 3, __ATOMIC_RELEASE);
    __atomic_store_n(&g_wait_done, 1, __ATOMIC_RELEASE);
}

static bool wait_until(volatile uint32_t *value, uint32_t expected, uint32_t timeout_ms)
{
    uint64_t deadline = clock_monotonic_ns() + (uint64_t)timeout_ms * 1000000ULL;
    while (__atomic_load_n(value, __ATOMIC_ACQUIRE) != expected) {
        if (clock_monotonic_ns() >= deadline) return false;
        timer_sleep(1);
    }
    return true;
}

static int test_try_wait(void)
{
    bool ok = reset_test_queue(128); input_event_t e;
    for (uint32_t i = 0; ok && i < 100; i++)
        ok = input_queue_push(&g_test_queue, input_event_char((char)(i | 1u)), 1, INPUT_DEST_TEST, NULL) == INPUT_QUEUE_PUSH_OK;
    for (uint32_t i = 0; ok && i < 100; i++) ok = input_queue_try_pop(&g_test_queue, &e) == INPUT_QUEUE_POP_OK;
    __atomic_store_n(&g_wait_stage, 0, __ATOMIC_RELEASE); __atomic_store_n(&g_wait_done, 0, __ATOMIC_RELEASE);
    task_handle_t handle;
    ok = ok && thread_create_named_handle(wait_test_worker, NULL, "input-wait-test", &handle) && wait_until(&g_wait_stage, 1, 1000);
    timer_sleep(20); ok = ok && __atomic_load_n(&g_wait_stage, __ATOMIC_ACQUIRE) == 1;
    ok = ok && input_queue_push(&g_test_queue, input_event_char('R'), 1, INPUT_DEST_TEST, NULL) == INPUT_QUEUE_PUSH_OK && wait_until(&g_wait_stage, 2, 1000);
    timer_sleep(20); ok = ok && __atomic_load_n(&g_wait_stage, __ATOMIC_ACQUIRE) == 2;
    ok = ok && input_queue_push(&g_test_queue, input_event_char('S'), 1, INPUT_DEST_TEST, NULL) == INPUT_QUEUE_PUSH_OK && wait_until(&g_wait_done, 1, 1000);
    input_queue_stats_t stats; input_queue_snapshot(&g_test_queue, &stats);
    ok = ok && stats.count == 0 && stats.spurious_wakes == 0;
    serial_write_all(ok ? "[INPUTTEST][TRY_WAIT] PASS try_popped=100 phantom_wakes=0 real_wakes=1\n" : "[INPUTTEST][TRY_WAIT] FAIL\n");
    return ok ? 0 : 1;
}

static void producer_worker(void *arg)
{
    uintptr_t producer = (uintptr_t)arg;
    for (uint32_t i = 0; i < g_producer_each; i++) {
        input_event_t e = producer ? input_event_special((uint8_t)i) : input_event_char((char)(i | 1u));
        while (input_queue_push(&g_test_queue, e, 1, INPUT_DEST_TEST, NULL) == INPUT_QUEUE_PUSH_FULL) schedule_voluntary();
        __atomic_add_fetch(&g_producer_accepted, 1, __ATOMIC_RELAXED);
    }
    __atomic_add_fetch(&g_producer_done, 1, __ATOMIC_RELEASE);
}

static void consumer_worker(void *arg)
{
    (void)arg; input_queue_entry_t entry;
    uint64_t target = (uint64_t)g_producer_each * 2u;
    while (__atomic_load_n(&g_consumer_count, __ATOMIC_ACQUIRE) < target) {
        if (input_queue_wait_pop_entry(&g_test_queue, &entry) != INPUT_QUEUE_POP_OK) break;
        uint64_t last = __atomic_load_n(&g_last_sequence, __ATOMIC_ACQUIRE);
        if (entry.queue_sequence == last)
            __atomic_add_fetch(&g_sequence_duplicates, 1, __ATOMIC_RELAXED);
        else if (entry.queue_sequence < last)
            __atomic_add_fetch(&g_sequence_regressions, 1, __ATOMIC_RELAXED);
        else if (entry.queue_sequence > last + 1u)
            __atomic_add_fetch(&g_sequence_gaps,
                               entry.queue_sequence - last - 1u,
                               __ATOMIC_RELAXED);
        __atomic_store_n(&g_last_sequence, entry.queue_sequence,
                         __ATOMIC_RELEASE);
        if (entry.event.type == INPUT_EVENT_CHAR)
            __atomic_add_fetch(&g_consumer_chars, 1, __ATOMIC_RELAXED);
        else if (entry.event.type == INPUT_EVENT_SPECIAL)
            __atomic_add_fetch(&g_consumer_specials, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_consumer_count, 1, __ATOMIC_RELEASE);
    }
}

static int test_producers(void)
{
    bool ok = reset_test_queue(256); g_producer_each = 50000;
    __atomic_store_n(&g_producer_done, 0, __ATOMIC_RELEASE); __atomic_store_n(&g_producer_accepted, 0, __ATOMIC_RELEASE); __atomic_store_n(&g_consumer_count, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_consumer_chars, 0, __ATOMIC_RELEASE); __atomic_store_n(&g_consumer_specials, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_sequence_gaps, 0, __ATOMIC_RELEASE); __atomic_store_n(&g_sequence_duplicates, 0, __ATOMIC_RELEASE); __atomic_store_n(&g_sequence_regressions, 0, __ATOMIC_RELEASE); __atomic_store_n(&g_last_sequence, 0, __ATOMIC_RELEASE);
    task_handle_t a, b, c;
    ok = ok && thread_create_named_handle(consumer_worker, NULL, "input-consumer-test", &c) &&
         thread_create_named_handle(producer_worker, (void *)(uintptr_t)0, "input-producer-a", &a) &&
         thread_create_named_handle(producer_worker, (void *)(uintptr_t)1, "input-producer-b", &b);
    uint64_t deadline = clock_monotonic_ns() + 120000000000ULL;
    while (ok && (__atomic_load_n(&g_producer_done, __ATOMIC_ACQUIRE) != 2 || __atomic_load_n(&g_consumer_count, __ATOMIC_ACQUIRE) != 100000)) {
        if (clock_monotonic_ns() >= deadline) { ok = false; break; }
        timer_sleep(1);
    }
    input_queue_stats_t s; input_queue_snapshot(&g_test_queue, &s);
    uint64_t accepted=__atomic_load_n(&g_producer_accepted,__ATOMIC_ACQUIRE),consumed=__atomic_load_n(&g_consumer_count,__ATOMIC_ACQUIRE),chars=__atomic_load_n(&g_consumer_chars,__ATOMIC_ACQUIRE),specials=__atomic_load_n(&g_consumer_specials,__ATOMIC_ACQUIRE),gaps=__atomic_load_n(&g_sequence_gaps,__ATOMIC_ACQUIRE),duplicates=__atomic_load_n(&g_sequence_duplicates,__ATOMIC_ACQUIRE),regressions=__atomic_load_n(&g_sequence_regressions,__ATOMIC_ACQUIRE),last=__atomic_load_n(&g_last_sequence,__ATOMIC_ACQUIRE);
    ok = ok && accepted == 100000 && consumed == 100000 && chars == 50000 && specials == 50000 && last == 100000 && !gaps && !duplicates && !regressions && s.count == 0;
    serial_write_all(ok ? "[INPUTTEST][PRODUCERS] PASS" : "[INPUTTEST][PRODUCERS] FAIL"); serial_write_all(" accepted="); put_u64(accepted); serial_write_all(" consumed="); put_u64(consumed); serial_write_all(" chars=");put_u64(chars);serial_write_all(" specials=");put_u64(specials);serial_write_all(" last_sequence=");put_u64(last);serial_write_all(" sequence_gaps=");put_u64(gaps);serial_write_all(" duplicates=");put_u64(duplicates);serial_write_all(" regressions=");put_u64(regressions);serial_write_all("\n");
    return ok ? 0 : 1;
}

static int test_full(void)
{
    bool ok = reset_test_queue(8); uint64_t accepted = 0, dropped = 0;
    for (uint32_t i = 0; i < 10000; i++) {
        input_queue_push_result_t r = input_queue_push(&g_test_queue,
            input_event_char((char)((i % 95u) + 32u)), 1, INPUT_DEST_TEST, NULL);
        if (r == INPUT_QUEUE_PUSH_OK) accepted++; else if (r == INPUT_QUEUE_PUSH_FULL) dropped++; else ok = false;
    }
    input_queue_stats_t s; input_queue_snapshot(&g_test_queue, &s);
    ok = ok && accepted == 8 && dropped == 9992 && s.count == 8;
    serial_write_all(ok ? "[INPUTTEST][FULL] PASS attempted=10000 accepted=" : "[INPUTTEST][FULL] FAIL accepted=");
    put_u64(accepted); serial_write_all(" dropped="); put_u64(dropped); serial_write_all(" count="); put_u64(s.count); serial_write_all("\n");
    return ok ? 0 : 1;
}

static int test_mixed(void)
{
    bool ok = reset_test_queue(256); input_queue_entry_t e; uint64_t chars = 0, specials = 0;
    for (uint32_t batch = 0; ok && batch < 16; batch++) {
        for (uint32_t i = 0; i < 128; i++) {
            input_event_t event = (i & 1u) ? input_event_special((uint8_t)i) : input_event_char((char)(i | 1u));
            ok = input_queue_push(&g_test_queue, event, 1, INPUT_DEST_TEST, NULL) == INPUT_QUEUE_PUSH_OK;
        }
        for (uint32_t i = 0; ok && i < 128; i++) {
            ok = input_queue_try_pop_entry(&g_test_queue, &e) == INPUT_QUEUE_POP_OK && e.queue_sequence == batch * 128u + i + 1u && e.event.type == (uint8_t)(i & 1u);
            if (e.event.type == INPUT_EVENT_CHAR) chars++; else specials++;
        }
    }
    serial_write_all(ok ? "[INPUTTEST][MIXED] PASS chars=" : "[INPUTTEST][MIXED] FAIL chars="); put_u64(chars);
    serial_write_all(" specials="); put_u64(specials); serial_write_all(" order="); put_u64(ok); serial_write_all("\n");
    return ok ? 0 : 1;
}

static int test_transitions(uint32_t cycles)
{
    bool ok = cycles != 0; input_event_t e;
    modal_session_test_set_quiet(true);
    for (uint32_t i = 0; ok && i < cycles; i++) {
        ok = modal_session_test_begin("inputtest");
        ok = ok && !modal_session_test_begin("inputtest-second");
        input_router_dispatch_event(input_event_char('m'));
        input_router_dispatch_event(input_event_special(1));
        ok = ok && input_router_modal_try_pop(&e) && e.type == INPUT_EVENT_CHAR;
        if ((i & 15u) != 0) ok = ok && input_router_modal_try_pop(&e) && e.type == INPUT_EVENT_SPECIAL;
        modal_session_test_end();
        ok = ok && !modal_session_is_active();
    }
    modal_session_test_set_quiet(false);
    uint64_t violations = 0;
    ok = ok && !modal_session_is_active() && !shell_is_input_paused_for_modal_ui() && input_router_validate(&violations);
    serial_write_all(ok ? "[INPUTTEST][TRANSITIONS] PASS cycles=" : "[INPUTTEST][TRANSITIONS] FAIL cycles="); put_u64(cycles); serial_write_all("\n");
    return ok ? 0 : 1;
}

static void boundary_producer_worker(void *arg)
{
    (void)arg;
    __atomic_store_n(&g_boundary_attempted, 1, __ATOMIC_RELEASE);
    input_router_dispatch_event(input_event_char(g_boundary_mode == 1 ? 'm' : 'd'));
    __atomic_store_n(&g_boundary_producer_done, 1, __ATOMIC_RELEASE);
}

static void boundary_transition_worker(void *arg)
{
    (void)arg;
    if (g_boundary_mode == 1) (void)modal_session_test_begin("inputtest-boundary");
    else modal_session_test_end();
    __atomic_store_n(&g_boundary_transition_done, 1, __ATOMIC_RELEASE);
}

static bool boundary_wait_flag(volatile uint32_t *flag, uint32_t timeout_ms)
{
    return wait_until(flag, 1, timeout_ms);
}

static bool boundary_wait_worker_exit(task_handle_t handle, uint32_t timeout_ms)
{
    uint64_t deadline = clock_monotonic_ns() + (uint64_t)timeout_ms * 1000000ULL;
    for (;;) {
        task_snapshot_t snapshot;
        if (!scheduler_snapshot_task_by_id(handle.id, &snapshot) ||
            snapshot.state == TASK_ZOMBIE)
            return true;
        if (clock_monotonic_ns() >= deadline) return false;
        timer_sleep(1);
    }
}

static int test_begin_boundary(void)
{
    input_event_t e; scheduler_runtime_stats_t runtime; input_router_dispatch_event(input_event_char('t'));
    bool ok = scheduler_validate_runtime_invariants(&runtime);
    if (ok && runtime.cpus >= 3) {
        g_boundary_mode = 1; g_boundary_attempted = 0;
        g_boundary_producer_done = g_boundary_transition_done = 0;
        task_handle_t producer, transition;
        input_router_test_hook_arm(INPUT_TEST_HOOK_BEGIN_BEFORE_ACTIVE);
        ok = ok && thread_create_named_with_class_flags_handle(
            boundary_transition_worker, NULL, TASK_CLASS_INTERACTIVE,
            "input-boundary-begin", TASK_FLAG_SYSTEM, &transition);
        uint64_t deadline = clock_monotonic_ns() + 1000000000ULL;
        while (ok && !input_router_test_hook_entered()) {
            if (clock_monotonic_ns() >= deadline) ok = false;
            else __asm__ volatile("pause");
        }
        ok = ok && thread_create_named_with_class_flags_handle(
            boundary_producer_worker, NULL, TASK_CLASS_INTERACTIVE,
            "input-boundary-producer", TASK_FLAG_SYSTEM, &producer);
        deadline=clock_monotonic_ns()+1000000000ULL;
        while(!__atomic_load_n(&g_boundary_attempted,__ATOMIC_ACQUIRE)&&clock_monotonic_ns()<deadline)__asm__ volatile("pause");
        ok = ok && __atomic_load_n(&g_boundary_attempted,__ATOMIC_ACQUIRE) && !__atomic_load_n(&g_boundary_producer_done, __ATOMIC_ACQUIRE);
        input_router_test_hook_release();
        ok = ok && boundary_wait_flag(&g_boundary_transition_done, 1000) && boundary_wait_flag(&g_boundary_producer_done, 1000);
        ok = ok && boundary_wait_worker_exit(transition, 1000) &&
             boundary_wait_worker_exit(producer, 1000);
    } else if (ok) {
        serial_write_all("[INPUTTEST][BEGIN_BOUNDARY] SKIP cpus=");put_u64(runtime.cpus);serial_write_all(" requires=3\n");return 0;
    }
    ok = ok && !input_router_default_try_pop(&e) && input_router_modal_try_pop(&e) && e.value == 'm';
    modal_session_test_end();
    serial_write_all(ok ? "[INPUTTEST][BEGIN_BOUNDARY] PASS typed_ahead_drained=1 producer_modal=1 default_leak=0\n" : "[INPUTTEST][BEGIN_BOUNDARY] FAIL\n");
    return ok ? 0 : 1;
}

static int test_end_boundary(void)
{
    input_event_t e; scheduler_runtime_stats_t runtime; bool ok = modal_session_test_begin("inputtest-boundary") && scheduler_validate_runtime_invariants(&runtime);
    input_router_dispatch_event(input_event_char('x'));
    if (ok && runtime.cpus >= 3) {
        g_boundary_mode = 2; g_boundary_attempted = 0;
        g_boundary_producer_done = g_boundary_transition_done = 0;
        task_handle_t producer, transition;
        input_router_test_hook_arm(INPUT_TEST_HOOK_END_BEFORE_DEFAULT);
        ok = ok && thread_create_named_with_class_flags_handle(
            boundary_transition_worker, NULL, TASK_CLASS_INTERACTIVE,
            "input-boundary-end", TASK_FLAG_SYSTEM, &transition);
        uint64_t deadline = clock_monotonic_ns() + 1000000000ULL;
        while (ok && !input_router_test_hook_entered()) {
            if (clock_monotonic_ns() >= deadline) ok = false;
            else __asm__ volatile("pause");
        }
        ok = ok && thread_create_named_with_class_flags_handle(
            boundary_producer_worker, NULL, TASK_CLASS_INTERACTIVE,
            "input-boundary-producer", TASK_FLAG_SYSTEM, &producer);
        deadline=clock_monotonic_ns()+1000000000ULL;
        while(!__atomic_load_n(&g_boundary_attempted,__ATOMIC_ACQUIRE)&&clock_monotonic_ns()<deadline)__asm__ volatile("pause");
        ok = ok && __atomic_load_n(&g_boundary_attempted,__ATOMIC_ACQUIRE) && !__atomic_load_n(&g_boundary_producer_done, __ATOMIC_ACQUIRE);
        input_router_test_hook_release();
        ok = ok && boundary_wait_flag(&g_boundary_transition_done, 1000) && boundary_wait_flag(&g_boundary_producer_done, 1000);
        ok = ok && boundary_wait_worker_exit(transition, 1000) &&
             boundary_wait_worker_exit(producer, 1000);
    } else if (ok) { (void)modal_session_test_end();serial_write_all("[INPUTTEST][END_BOUNDARY] SKIP cpus=");put_u64(runtime.cpus);serial_write_all(" requires=3\n");return 0; }
    ok = ok && !input_router_modal_try_pop(&e) && input_router_default_try_pop(&e) && e.value == 'd';
    serial_write_all(ok ? "[INPUTTEST][END_BOUNDARY] PASS modal_tail_drained=1 producer_default=1 modal_leak=0\n" : "[INPUTTEST][END_BOUNDARY] FAIL\n");
    return ok ? 0 : 1;
}

static int test_end_rollback(void)
{
    modal_session_test_set_quiet(true);
    bool ok = modal_session_test_begin("inputtest-rollback");
    input_router_test_reject_next_end(true);
    modal_session_snapshot_t before;modal_session_snapshot(&before);
    bool rejected = modal_session_end((modal_session_token_t){before.active_generation,before.route_generation,before.owner.id,before.owner.lifecycle_generation}) == MODAL_END_ROUTER_REFUSED;
    bool active = modal_session_state() == MODAL_SESSION_ACTIVE;
    bool router_modal = input_router_route_state() == INPUT_ROUTE_MODAL;
    bool shell_paused = shell_is_input_paused_for_modal_ui();
    bool recovered = modal_session_test_end();
    modal_session_test_set_quiet(false);
    ok = ok && rejected && active && router_modal && shell_paused && recovered &&
         !modal_session_is_active() && !shell_is_input_paused_for_modal_ui();
    serial_write_all(ok ? "[INPUTTEST][END_ROLLBACK] PASS" :
                          "[INPUTTEST][END_ROLLBACK] FAIL");
    serial_write_all(" active=");put_u64(active);
    serial_write_all(" router_modal=");put_u64(router_modal);
    serial_write_all(" shell_paused=");put_u64(shell_paused);
    serial_write_all(" recovered=");put_u64(recovered);
    serial_write_all("\n");
    return ok ? 0 : 1;
}

static void shell_pause_boundary_worker(void *arg)
{
    (void)arg;
    bool first = shell_test_consume_default_event(false);
    __atomic_store_n(&g_shell_pause_first_delivered, first, __ATOMIC_RELEASE);
    bool second = shell_test_consume_default_event(false);
    __atomic_store_n(&g_shell_pause_second_delivered, second, __ATOMIC_RELEASE);
    __atomic_store_n(&g_shell_pause_worker_done, 1, __ATOMIC_RELEASE);
}

static int test_shell_pause_boundary(void)
{
    scheduler_runtime_stats_t runtime;
    if (!scheduler_validate_runtime_invariants(&runtime) || runtime.cpus < 2) {
        serial_write_all("[INPUTTEST][SHELL_PAUSE_BOUNDARY] SKIP cpus=");
        put_u64(runtime.cpus);serial_write_all(" requires=2\n");return 0;
    }
    __atomic_store_n(&g_shell_pause_worker_done,0,__ATOMIC_RELEASE);
    __atomic_store_n(&g_shell_pause_first_delivered,0,__ATOMIC_RELEASE);
    __atomic_store_n(&g_shell_pause_second_delivered,0,__ATOMIC_RELEASE);
    uint64_t drops_before=shell_modal_pause_drop_count();
    shell_test_pause_boundary_arm();
    task_handle_t worker;
    bool ok=thread_create_named_handle(shell_pause_boundary_worker,NULL,
                                        "shell-pause-boundary",&worker);
    input_router_dispatch_event(input_event_char('P'));
    uint64_t deadline=clock_monotonic_ns()+1000000000ULL;
    while(ok&&!shell_test_pause_boundary_entered()&&clock_monotonic_ns()<deadline)timer_sleep(1);
    ok=ok&&shell_test_pause_boundary_entered();
    shell_pause_input_for_modal_ui();
    shell_test_pause_boundary_release();
    deadline=clock_monotonic_ns()+1000000000ULL;
    while(ok&&shell_modal_pause_drop_count()==drops_before&&clock_monotonic_ns()<deadline)timer_sleep(1);
    bool dropped=shell_modal_pause_drop_count()==drops_before+1&&!__atomic_load_n(&g_shell_pause_first_delivered,__ATOMIC_ACQUIRE);
    shell_resume_input_from_modal_ui();
    input_router_dispatch_event(input_event_char('A'));
    ok=ok&&wait_until(&g_shell_pause_worker_done,1,1000);
    bool delivered=__atomic_load_n(&g_shell_pause_second_delivered,__ATOMIC_ACQUIRE)!=0;
    ok=ok&&dropped&&delivered;
    serial_write_all(ok?"[INPUTTEST][SHELL_PAUSE_BOUNDARY] PASS":"[INPUTTEST][SHELL_PAUSE_BOUNDARY] FAIL");
    serial_write_all(" pre_pause_dropped=");put_u64(dropped);
    serial_write_all(" post_resume_delivered=");put_u64(delivered);
    serial_write_all("\n");return ok?0:1;
}

static int test_keyboard(void)
{
    bool ok = modal_session_test_begin("inputtest-keyboard"); uint64_t accepted = 0, consumed = 0; input_event_t out;
    for (uint32_t i = 0; ok && i < 10000; i++) {
        input_event_t e = (i & 1u) ? input_event_special((uint8_t)i) : input_event_char((char)(i | 1u));
        while (!keyboard_test_enqueue_event(e)) timer_sleep(1);
        accepted++;
        uint64_t deadline = clock_monotonic_ns() + 1000000000ULL;
        while (!input_router_modal_try_pop(&out)) {
            if (clock_monotonic_ns() >= deadline) { ok = false; break; }
            timer_sleep(0);
        }
        if (ok && out.type == e.type && out.value == e.value) consumed++; else ok = false;
    }
    modal_session_test_end(); input_queue_stats_t s; keyboard_input_queue_snapshot(&s);
    ok = ok && accepted == 10000 && consumed == accepted && s.count == 0;
    serial_write_all(ok ? "[INPUTTEST][KEYBOARD] PASS" : "[INPUTTEST][KEYBOARD] FAIL"); serial_write_all(" injected=10000 accepted="); put_u64(accepted); serial_write_all(" routed="); put_u64(consumed); serial_write_all(" consumed="); put_u64(consumed); serial_write_all(" phantom="); put_u64(s.spurious_wakes); serial_write_all("\n");
    return ok ? 0 : 1;
}

static int test_boundary_loop(uint32_t cycles)
{
    bool ok = cycles != 0;
    uint32_t completed = 0;
    modal_session_test_set_quiet(true);
    scheduler_test_set_lifecycle_log_quiet(true);
    for (; ok && completed < cycles; completed++) {
        ok = test_begin_boundary() == 0 && test_end_boundary() == 0;
    }
    for (uint32_t attempt = 0; attempt < 10000; attempt++) {
        task_reaper_stats_t reaper;
        scheduler_reaper_stats_snapshot(&reaper);
        if (!reaper.current_zombies && !reaper.free_inflight)
            break;
        (void)scheduler_reap_zombies(0);
        timer_sleep(1);
    }
    scheduler_test_set_lifecycle_log_quiet(false);
    modal_session_test_set_quiet(false);
    if (ok && completed == 1000) {
        serial_write_all("[INPUTTEST][BOUNDARY_LOOP] PASS cycles=1000 begin=1000 end=1000 causal=1\n");
        return 0;
    }
    serial_write_all(ok ? "[INPUTTEST][BOUNDARY_LOOP] PASS cycles=" :
                          "[INPUTTEST][BOUNDARY_LOOP] FAIL cycles=");
    put_u64(completed);
    serial_write_all(" begin="); put_u64(completed);
    serial_write_all(" end="); put_u64(completed);
    serial_write_all(" causal=1\n");
    return ok ? 0 : 1;
}

static int test_check(void)
{
    input_router_diagnostics_t d={0}; input_queue_validation_t kv={0}; input_queue_stats_t ks={0};
    uint64_t violations = 0;
    bool snapshots_ok = input_router_diagnostics_snapshot(&d);
    uint64_t expected_dispatch=d.dispatched+d.invalid+d.default_drops+d.modal_drops;
    uint64_t route_mismatch=snapshots_ok&&d.dispatch_sequence==expected_dispatch?0:1;
    uint64_t phantom=d.default_queue.phantom_empty_wakes+d.modal_queue.phantom_empty_wakes;
    bool modal_shell_ok=(d.state == INPUT_ROUTE_DEFAULT && !modal_session_is_active() && !shell_is_input_paused_for_modal_ui()) ||
                        (d.state == INPUT_ROUTE_MODAL && modal_session_is_active() && shell_is_input_paused_for_modal_ui());
    bool ok = snapshots_ok && input_router_validate(&violations) &&
              keyboard_input_queue_validate(&kv) && keyboard_input_queue_snapshot(&ks) &&
              modal_shell_ok;
    phantom+=ks.phantom_empty_wakes;
    violations+=route_mismatch+(modal_shell_ok?0:1);
    ok=ok&&!route_mismatch&&!phantom&&!violations;
    serial_write_all(ok ? "[INPUTTEST][CHECK] PASS" : "[INPUTTEST][CHECK] FAIL");
    serial_write_all(" default_count="); put_u64(d.default_queue.count); serial_write_all(" modal_count="); put_u64(d.modal_queue.count);
    serial_write_all(" keyboard_count="); put_u64(ks.count); serial_write_all(" phantom="); put_u64(phantom);
    serial_write_all(" route_mismatch=");put_u64(route_mismatch);serial_write_all(" violations="); put_u64(violations + kv.violations); serial_write_all("\n");
    return ok ? 0 : 1;
}

static int print_stats(void)
{
    input_router_diagnostics_t d; input_queue_stats_t k;
    if (!input_router_diagnostics_snapshot(&d) || !keyboard_input_queue_snapshot(&k)) return 1;
    serial_write_all("[INPUTTEST][STATS] state="); put_u64(d.state); serial_write_all(" generation="); put_u64(d.route_generation);
    serial_write_all(" dispatched="); put_u64(d.dispatched); serial_write_all(" default_count="); put_u64(d.default_queue.count);
    serial_write_all(" modal_count="); put_u64(d.modal_queue.count); serial_write_all(" keyboard_count="); put_u64(k.count);
    serial_write_all(" trace="); put_u64(input_debug_get_trace_flags()); serial_write_all("\n"); return 0;
}

#ifdef HOBBYOS_INPUT_NEGATIVE_PHANTOM_PERMIT
static void negative_phantom_waiter(void *arg)
{
    (void)arg;input_queue_entry_t entry;
    input_queue_pop_result_t result=input_queue_wait_pop_entry(&g_test_queue,&entry);
    __atomic_store_n(&g_negative_waiter_result,(uint32_t)result,__ATOMIC_RELEASE);
    __atomic_store_n(&g_negative_waiter_done,1,__ATOMIC_RELEASE);
}
#endif

int cmd_inputtest(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "check";
    if (strcmp(sub, "check") == 0) return test_check();
    if (strcmp(sub, "stats") == 0) return print_stats();
    if (strcmp(sub, "queue") == 0) return test_queue();
    if (strcmp(sub, "try-wait") == 0) return test_try_wait();
    if (strcmp(sub, "drain") == 0) return test_drain();
    if (strcmp(sub, "preblock-cancel") == 0) return run_input_preblock_case(false);
    if (strcmp(sub, "preblock-loop") == 0) return test_input_preblock_loop(
        argc > 2 ? parse_u32(argv[2], 1000) : 1000);
    if (strcmp(sub, "full") == 0) return test_full();
    if (strcmp(sub, "mixed") == 0) return test_mixed();
    if (strcmp(sub, "producers") == 0) return test_producers();
    if (strcmp(sub, "begin-boundary") == 0) return test_begin_boundary();
    if (strcmp(sub, "end-boundary") == 0) return test_end_boundary();
    if (strcmp(sub, "end-rollback") == 0) return test_end_rollback();
    if (strcmp(sub, "shell-pause-boundary") == 0) return test_shell_pause_boundary();
    if (strcmp(sub, "keyboard") == 0) return test_keyboard();
    if (strcmp(sub, "boundary-loop") == 0) return test_boundary_loop(argc > 2 ? parse_u32(argv[2], 1000) : 1000);
    if (strcmp(sub, "leakage") == 0) { serial_write_all("[INPUTTEST][LEAKAGE] READY host_hmp_required=1\n"); return 0; }
    if (strcmp(sub, "negative-phantom") == 0) {
#ifdef HOBBYOS_INPUT_NEGATIVE_PHANTOM_PERMIT
        bool detected = reset_test_queue(8); input_event_t e;
        for (uint32_t i = 0; detected && i < 8; i++) detected = input_queue_push(&g_test_queue, input_event_char('p'), 1, INPUT_DEST_TEST, NULL) == INPUT_QUEUE_PUSH_OK;
        for (uint32_t i = 0; detected && i < 8; i++) detected = input_queue_try_pop(&g_test_queue, &e) == INPUT_QUEUE_POP_OK;
        __atomic_store_n(&g_negative_waiter_done,0,__ATOMIC_RELEASE);__atomic_store_n(&g_negative_waiter_result,INPUT_QUEUE_POP_OK,__ATOMIC_RELEASE);
        task_handle_t waiter;detected=detected&&thread_create_named_handle(negative_phantom_waiter,NULL,"negative-phantom-waiter",&waiter)&&wait_until(&g_negative_waiter_done,1,1000);
        input_queue_stats_t negative_stats;input_queue_snapshot(&g_test_queue,&negative_stats);
        detected=detected&&__atomic_load_n(&g_negative_waiter_result,__ATOMIC_ACQUIRE)==INPUT_QUEUE_POP_ERROR&&negative_stats.phantom_empty_wakes==1;
        serial_write_all(detected ? "[INPUTTEST][NEGATIVE] PHANTOM_PERMIT_DETECTED\n" : "[INPUTTEST][NEGATIVE] PHANTOM_PERMIT_MISSED\n"); return detected ? 0 : 1;
#else
        return 1;
#endif
    }
    if (strcmp(sub, "negative-route") == 0) {
#ifdef HOBBYOS_INPUT_NEGATIVE_ROUTE_AFTER_UNLOCK
        modal_session_test_set_quiet(true);
        bool detected = modal_session_test_begin("input-negative") && input_router_test_nonatomic_route_negative();
        serial_write_all(detected ? "[INPUTTEST][NEGATIVE] NONATOMIC_ROUTE_DETECTED\n" : "[INPUTTEST][NEGATIVE] NONATOMIC_ROUTE_MISSED\n"); return detected ? 0 : 1;
#else
        return 1;
#endif
    }
    if (strcmp(sub, "transitions") == 0) return test_transitions(argc > 2 ? parse_u32(argv[2], 10000) : 10000);
    if (strcmp(sub, "marker") == 0) { serial_write_all("[INPUTTEST][MARKER] name="); serial_write_all(argc > 2 ? argv[2] : "missing"); serial_write_all("\n"); return argc > 2 ? 0 : 1; }
    if (strcmp(sub, "trace") == 0) {
        uint32_t flags = INPUT_TRACE_NONE;
        if (argc > 2 && strcmp(argv[2], "keyboard") == 0) flags = INPUT_TRACE_KEYBOARD;
        else if (argc > 2 && strcmp(argv[2], "router") == 0) flags = INPUT_TRACE_ROUTER;
        else if (argc > 2 && strcmp(argv[2], "all") == 0) flags = INPUT_TRACE_ALL;
        else if (argc <= 2 || strcmp(argv[2], "off") != 0) return 1;
        input_debug_set_trace_flags(flags); serial_write_all("[INPUTTEST][TRACE] PASS flags="); put_u64(flags); serial_write_all("\n"); return 0;
    }
    if (strcmp(sub, "all") == 0) return test_queue() || test_try_wait() || test_drain() || test_full() || test_mixed() || test_producers() || test_begin_boundary() || test_end_boundary() || test_end_rollback() || test_shell_pause_boundary() || test_keyboard() || test_transitions(1000) || test_check();
    serial_write_all("inputtest: check|stats|trace|queue|try-wait|drain|preblock-cancel|full|mixed|producers|begin-boundary|end-boundary|end-rollback|shell-pause-boundary|transitions|keyboard|leakage|marker|all\n"); return 1;
}
