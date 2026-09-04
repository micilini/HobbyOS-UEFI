#include "cmd_synctest.h"
#include "../diagnostic_result.h"
#include "../../core/semaphore.h"
#include "../../core/scheduler.h"
#include "../../core/timers.h"
#include "../../drivers/timer.h"
#include "../../drivers/serial.h"
#include "../../graphics/console.h"
#include "../../libc/string.h"
#include "../../libc/memory.h"
#include <stdint.h>
#include <stdbool.h>

#define SYNC_MAX_WORKERS 64u
#define SYNC_WATCHDOG_MS 120000u
#define BOUNDARY_STALL_WATCHDOG_MS 5000u
#define BOUNDARY_TOTAL_WATCHDOG_MS 300000u
#define BOUNDARY_CLEANUP_WATCHDOG_MS 10000u

typedef struct {
    uint32_t index;
    uint32_t iterations;
    uint64_t delay_ms;
} sync_worker_ctx_t;

typedef struct {
    semaphore_t sem;
    sync_worker_ctx_t workers[SYNC_MAX_WORKERS];
    volatile uint32_t running;
    volatile uint32_t start;
    volatile uint32_t abort;
    volatile uint32_t remaining;
    volatile uint32_t next_wait;
    volatile uint32_t next_signal;
    volatile uint64_t waits_ok;
    volatile uint64_t signals_ok;
    volatile uint64_t cancelled;
    volatile uint64_t errors;
    uint32_t iterations;
    uint32_t waiters;
    uint32_t signalers;
} sync_sem_test_t;

typedef enum
{
    BOUNDARY_FAILURE_NONE = 0,
    BOUNDARY_FAILURE_CREATE,
    BOUNDARY_FAILURE_ARM,
    BOUNDARY_FAILURE_PREPARE_TIMEOUT,
    BOUNDARY_FAILURE_HOOK_CROSSTALK,
    BOUNDARY_FAILURE_HOOK_ROUND,
    BOUNDARY_FAILURE_SIGNAL_PERMIT,
    BOUNDARY_FAILURE_SIGNAL_ERROR,
    BOUNDARY_FAILURE_COMPLETION_TIMEOUT,
    BOUNDARY_FAILURE_WAIT_RESULT,
    BOUNDARY_FAILURE_THROUGHPUT_TIMEOUT,
    BOUNDARY_FAILURE_CLEANUP
} boundary_failure_t;

typedef struct {
    semaphore_t sem;
    task_handle_t waiter_handle;
    volatile uint32_t waiter_started;
    volatile uint32_t start;
    volatile uint32_t stop;
    volatile uint32_t waiter_done;

    volatile uint64_t waiter_round;
    volatile uint64_t prepared_round;
    volatile uint64_t signaled_round;
    volatile uint64_t completed_round;
    volatile uint64_t released_round;

    volatile uint64_t target_hook_hits;
    volatile uint64_t foreign_hook_hits;
    volatile uint64_t waits_ok;
    volatile uint64_t signals_woke;
    volatile uint64_t signals_permit;
    volatile uint64_t errors;
    volatile uint64_t last_wait_generation;
    volatile uint32_t last_event_prepared;

    uint32_t rounds;
    volatile boundary_failure_t first_failure;
    volatile uint64_t failure_round;
    uint64_t started_ms;
    volatile uint64_t last_progress_ms;
    volatile uint32_t negative_detected;
} sync_boundary_test_t;

typedef struct
{
    semaphore_t sem;
    task_handle_t waiter_handle;
    volatile uint32_t waiter_started;
    volatile uint32_t start;
    volatile uint32_t stop;
    volatile uint32_t waiter_done;
    volatile uint64_t waiter_round;
    volatile uint64_t completed_round;
    volatile uint64_t released_round;
    volatile uint64_t waits_ok;
    volatile uint64_t signals_woke;
    volatile uint64_t signals_permit;
    volatile uint64_t errors;
    uint32_t rounds;
} sync_boundary_noise_t;

typedef struct
{
    boundary_failure_t failure;
    uint64_t failure_round;
    uint64_t rounds;
    uint64_t prepared;
    uint64_t signaled;
    uint64_t completed;
    uint64_t released;
    uint64_t waits;
    uint64_t signals_woke;
    uint64_t signals_permit;
    uint64_t errors;
    uint64_t started_ms;
    uint64_t last_progress_ms;
    uint64_t observed_ms;
} boundary_failure_observation_t;

typedef struct {
    volatile uint32_t running;
    volatile uint32_t remaining;
    volatile uint32_t start;
    volatile uint64_t timeout_ok;
    volatile uint64_t cancelled;
    volatile uint64_t errors;
    sync_worker_ctx_t workers[32];
} sync_sleep_test_t;

static sync_sem_test_t g_sem_test;
static sync_boundary_test_t g_boundary;
static sync_boundary_noise_t g_boundary_noise;
static sync_sleep_test_t g_sleep;
static volatile uint32_t g_any_test_running;
static volatile uint64_t g_async_next_run;
static volatile uint64_t g_async_active_run;
static volatile uint64_t g_async_completed_run;
static volatile uint32_t g_async_kind;
static volatile uint32_t g_async_state;
static volatile uint64_t g_async_requested;
static volatile uint64_t g_async_completed;
static volatile uint64_t g_async_errors;
static volatile uint64_t g_async_detail_a;
static volatile uint64_t g_async_detail_b;
static volatile uint32_t g_async_publish_sequence;
static volatile uint64_t g_async_diagnostic_generation;

typedef enum {
    TIMER_TEST_CALLBACK_CANCEL_PENDING = 1,
    TIMER_TEST_CALLBACK_CANCEL_CLAIMED,
    TIMER_TEST_CALLBACK_ORDER_OLD,
    TIMER_TEST_CALLBACK_ORDER_NEW,
    TIMER_TEST_CALLBACK_BACKLOG_OLD,
    TIMER_TEST_CALLBACK_BACKLOG_NEW
} timer_test_callback_role_t;

typedef struct {
    volatile uint64_t generation;
    volatile uint32_t active;
    uint32_t role;
    uint32_t index;
} timer_test_callback_ticket_t;

typedef struct {
    volatile uint64_t generation;
    volatile uint64_t expected_generation;
    volatile uint32_t run_active;
    volatile uint32_t callbacks;
    volatile uint32_t late_callbacks;
    volatile uint32_t errors;
    timer_test_callback_ticket_t pending;
    timer_test_callback_ticket_t claimed;
} timer_cancel_test_state_t;

typedef struct {
    volatile uint64_t generation;
    volatile uint64_t expected_generation;
    volatile uint32_t run_active;
    volatile uint32_t callbacks;
    volatile uint32_t late_callbacks;
    volatile uint32_t errors;
    volatile uint32_t sequence;
    volatile uint32_t old_position;
    volatile uint32_t new_position;
    timer_test_callback_ticket_t old_ticket;
    timer_test_callback_ticket_t new_ticket;
} timer_order_test_state_t;

#define TIMER_BACKLOG_OLD_COUNT 8u
#define TIMER_BACKLOG_NEW_COUNT 8u
#define TIMER_BACKLOG_COUNT (TIMER_BACKLOG_OLD_COUNT + TIMER_BACKLOG_NEW_COUNT)

typedef struct {
    volatile uint64_t generation;
    volatile uint64_t expected_generation;
    volatile uint32_t run_active;
    volatile uint32_t callbacks;
    volatile uint32_t late_callbacks;
    volatile uint32_t errors;
    volatile uint32_t old_completed;
    volatile uint32_t order_violations;
    timer_test_callback_ticket_t tickets[TIMER_BACKLOG_COUNT];
    timer_handle_t handles[TIMER_BACKLOG_COUNT];
} timer_backlog_test_state_t;

static timer_cancel_test_state_t g_timer_cancel_test;
static timer_order_test_state_t g_timer_order_test;
static timer_backlog_test_state_t g_timer_backlog_test;
static bool sync_test_wait_until(volatile uint32_t *value, uint32_t expected,
                                 uint64_t timeout_ms);
static void print_u64(uint64_t value);

#define SYNC_CONTRACT_RECORD_MAX 256u

typedef struct
{
    char bytes[SYNC_CONTRACT_RECORD_MAX];
    uint32_t length;
    uint8_t overflow;
} sync_contract_record_t;

static void sync_contract_reset(sync_contract_record_t *record)
{
    record->length = 0;
    record->overflow = 0;
    record->bytes[0] = '\0';
}

static bool sync_contract_char(sync_contract_record_t *record, char value)
{
    if (record->overflow || record->length + 1u >= SYNC_CONTRACT_RECORD_MAX) {
        record->overflow = 1;
        return false;
    }
    record->bytes[record->length++] = value;
    record->bytes[record->length] = '\0';
    return true;
}

static bool sync_contract_text(sync_contract_record_t *record,
                               const char *text)
{
    if (!text) return false;
    while (*text)
        if (!sync_contract_char(record, *text++)) return false;
    return true;
}

static bool sync_contract_u64(sync_contract_record_t *record, uint64_t value)
{
    char reversed[20];
    uint32_t count = 0;
    do {
        reversed[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (count)
        if (!sync_contract_char(record, reversed[--count])) return false;
    return true;
}

static bool sync_contract_finish(sync_contract_record_t *record)
{
    return sync_contract_char(record, '\n') && !record->overflow;
}

static void sync_contract_emit_record(sync_contract_record_t *record)
{
    if (!record->overflow && record->length >= 3u &&
        record->bytes[0] == '\n' &&
        record->bytes[record->length - 1u] == '\n') {
        serial_write_all(record->bytes);
        return;
    }
    serial_write_all("\n[SYNC][ASYNC] FAIL reason=record-overflow\n");
}

static const char *sync_async_kind_name(synctest_async_kind_t kind)
{
    switch (kind) {
    case SYNCTEST_ASYNC_SEM: return "SEM";
    case SYNCTEST_ASYNC_BOUNDARY: return "BOUNDARY";
    case SYNCTEST_ASYNC_BOUNDARY_NOISE: return "BOUNDARY_NOISE";
    case SYNCTEST_ASYNC_SLEEP: return "SLEEP";
    case SYNCTEST_ASYNC_CANCEL: return "CANCEL";
    case SYNCTEST_ASYNC_TIMER_CANCEL: return "TIMER_CANCEL";
    case SYNCTEST_ASYNC_RACE: return "RACE";
    default: return "NONE";
    }
}

static const char *synctest_async_command_name(synctest_async_kind_t kind)
{
    switch (kind) {
    case SYNCTEST_ASYNC_SEM: return "synctest sem";
    case SYNCTEST_ASYNC_BOUNDARY: return "synctest boundary";
    case SYNCTEST_ASYNC_BOUNDARY_NOISE: return "synctest boundary-noise";
    case SYNCTEST_ASYNC_SLEEP: return "synctest sleep";
    case SYNCTEST_ASYNC_CANCEL: return "synctest cancel";
    case SYNCTEST_ASYNC_TIMER_CANCEL: return "synctest timer-cancel";
    case SYNCTEST_ASYNC_RACE: return "synctest race";
    default: return "synctest async";
    }
}

static const char *sync_async_state_name(synctest_async_state_t state)
{
    switch (state) {
    case SYNCTEST_ASYNC_RUNNING: return "RUNNING";
    case SYNCTEST_ASYNC_PASS: return "PASS";
    case SYNCTEST_ASYNC_FAIL: return "FAIL";
    default: return "IDLE";
    }
}

const char *synctest_async_kind_string(synctest_async_kind_t kind)
{
    return sync_async_kind_name(kind);
}

const char *synctest_async_state_string(synctest_async_state_t state)
{
    return sync_async_state_name(state);
}

static void sync_contract_emit(synctest_async_kind_t kind,
                               const char *phase,
                               uint64_t run,
                               uint64_t requested,
                               uint64_t completed,
                               uint64_t errors)
{
    sync_contract_record_t record;
    sync_contract_reset(&record);
    (void)sync_contract_char(&record, '\n');
    (void)sync_contract_text(&record, "[SYNC][");
    (void)sync_contract_text(&record, sync_async_kind_name(kind));
    (void)sync_contract_text(&record, "] ");
    (void)sync_contract_text(&record, phase);
    (void)sync_contract_text(&record, " run=");
    (void)sync_contract_u64(&record, run);
    (void)sync_contract_text(&record, " requested=");
    (void)sync_contract_u64(&record, requested);
    (void)sync_contract_text(&record, " completed=");
    (void)sync_contract_u64(&record, completed);
    (void)sync_contract_text(&record, " errors=");
    (void)sync_contract_u64(&record, errors);
    (void)sync_contract_finish(&record);
    sync_contract_emit_record(&record);
}

typedef struct {
    semaphore_t sem;
    semaphore_t done;
    volatile uint32_t started;
    volatile uint32_t release;
    volatile uint32_t returned;
    volatile uint32_t result;
    task_handle_t handle;
} preblock_test_t;

typedef enum
{
    PREBLOCK_CASE_INIT = 0,
    PREBLOCK_CASE_CREATED,
    PREBLOCK_CASE_STARTED,
    PREBLOCK_CASE_KILL_ACCEPTED,
    PREBLOCK_CASE_RETURNED,
    PREBLOCK_CASE_ZOMBIE,
    PREBLOCK_CASE_VALIDATED,
    PREBLOCK_CASE_REAPED,
    PREBLOCK_CASE_FAILED
} preblock_case_stage_t;

typedef struct
{
    bool passed;
    bool timer;
    preblock_case_stage_t last_completed;
    preblock_case_stage_t first_failure;
    preblock_case_stage_t cleanup_stage;
    task_handle_t handle;
    task_wait_result_t wait_result;
    task_exit_reason_t exit_reason;
    task_snapshot_t initial;
    task_snapshot_t final;
    timers_test_task_wake_snapshot_t timer_nodes;
    uint64_t global_armed_delta;
    uint64_t global_ref_delta;
    bool target_gone;
    bool free_inflight_zero;
    bool cleanup_ok;
} preblock_case_result_t;

typedef struct {
    volatile uint32_t started;
    volatile uint32_t stop;
    volatile uint32_t stopped;
    volatile uint64_t cycles;
    task_handle_t handle;
} preblock_timer_noise_t;

static preblock_test_t g_preblock;
static volatile uint32_t g_preblock_claimed;
static preblock_timer_noise_t g_preblock_noise;

static void preblock_failure(preblock_case_result_t *result,
                             preblock_case_stage_t stage)
{
    if (result->first_failure == PREBLOCK_CASE_INIT)
        result->first_failure = stage;
}

static void preblock_sem_worker(void *arg)
{
    preblock_test_t *test = arg;
    __atomic_store_n(&test->started, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&test->release, __ATOMIC_ACQUIRE)) spin_cpu_relax();
    task_wait_result_t result = sem_wait_interruptible(&test->sem);
    __atomic_store_n(&test->result, (uint32_t)result, __ATOMIC_RELEASE);
    __atomic_store_n(&test->returned, 1, __ATOMIC_RELEASE);
    (void)sem_signal(&test->done);
    task_cancel_point();
}

static void preblock_timer_worker(void *arg)
{
    preblock_test_t *test = arg;
    __atomic_store_n(&test->started, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&test->release, __ATOMIC_ACQUIRE)) spin_cpu_relax();
    task_wait_result_t result = timer_sleep_interruptible(60000);
    __atomic_store_n(&test->result, (uint32_t)result, __ATOMIC_RELEASE);
    __atomic_store_n(&test->returned, 1, __ATOMIC_RELEASE);
    (void)sem_signal(&test->done);
    task_cancel_point();
}

static bool preblock_case_cleanup(preblock_test_t *test,
                                  preblock_case_result_t *result)
{
    __atomic_store_n(&test->release, 1, __ATOMIC_RELEASE);
    scheduler_test_clear_preblock_negative();

    task_snapshot_t snapshot = {0};
    bool present = result->handle.id != TASK_ID_INVALID &&
        scheduler_snapshot_task_by_handle(result->handle, &snapshot);
    if (present && snapshot.state != TASK_ZOMBIE) {
        task_kill_result_t kill = scheduler_request_kill(result->handle.id);
        if (kill != TASK_KILL_ACCEPTED && kill != TASK_KILL_ALREADY_PENDING &&
            kill != TASK_KILL_ALREADY_EXITING &&
            kill != TASK_KILL_ALREADY_ZOMBIE)
            preblock_failure(result, PREBLOCK_CASE_FAILED);
        if (!result->timer)
            (void)sem_signal(&test->sem);
    }

    uint64_t deadline = timer_get_uptime_ms() + 10000;
    while (scheduler_snapshot_task_by_handle(result->handle, &snapshot) &&
           snapshot.state != TASK_ZOMBIE &&
           timer_get_uptime_ms() < deadline)
        timer_sleep(1);
    bool zombie = scheduler_snapshot_task_by_handle(result->handle, &snapshot) &&
                  snapshot.state == TASK_ZOMBIE;
    if (zombie) {
        result->final = snapshot;
        result->exit_reason = snapshot.exit_reason;
        result->cleanup_stage = PREBLOCK_CASE_ZOMBIE;
    }

    if (zombie)
        (void)scheduler_test_hold_reap(result->handle.id, false);

    task_reaper_stats_t reaper = {0};
    deadline = timer_get_uptime_ms() + 10000;
    do {
        (void)scheduler_reap_zombies(0);
        scheduler_reaper_stats_snapshot(&reaper);
        result->target_gone = !scheduler_snapshot_task_by_handle(
            result->handle, &snapshot);
        result->free_inflight_zero = reaper.free_inflight == 0;
        if (result->target_gone && result->free_inflight_zero)
            break;
        timer_sleep(1);
    } while (timer_get_uptime_ms() < deadline);

    bool exact = timers_test_task_wake_snapshot(result->handle,
                                                 &result->timer_nodes);
    bool timer_clean = exact && !result->timer_nodes.pending_nodes &&
        !result->timer_nodes.claimed_nodes && !result->timer_nodes.refs_held &&
        !result->timer_nodes.found_wrong_lifecycle;
    result->cleanup_ok = zombie && result->target_gone &&
        result->free_inflight_zero && timer_clean;
    result->cleanup_stage = result->cleanup_ok ? PREBLOCK_CASE_REAPED :
                                                PREBLOCK_CASE_FAILED;
    if (!result->cleanup_ok)
        preblock_failure(result, PREBLOCK_CASE_FAILED);
    return result->cleanup_ok;
}

static bool run_preblock_case_result(bool timer, bool quiet,
                                     preblock_case_result_t *result)
{
    if (!result || __atomic_exchange_n(&g_preblock_claimed, 1,
                                        __ATOMIC_ACQ_REL))
        return false;
    preblock_test_t *test = &g_preblock;
    memset(test, 0, sizeof(*test));
    memset(result, 0, sizeof(*result));
    result->timer = timer;
    result->wait_result = TASK_WAIT_RESULT_ERROR;
    sem_init(&test->sem, 0);
    sem_init(&test->done, 0);

    scheduler_wait_stats_t before = {0}, after = {0};
    timer_stats_t timer_before = {0}, timer_after = {0};
    scheduler_wait_stats_snapshot(&before);
    timers_get_stats(&timer_before);
    task_create_options_t options = {
        .name = timer ? "preblock-timer" : "preblock-sem",
        .task_class = TASK_CLASS_NORMAL,
        .flags = TASK_FLAG_SYSTEM | TASK_FLAG_KILLABLE,
        .test_reap_hold = 1
    };
    bool created = thread_create_ex_handle(
        timer ? preblock_timer_worker : preblock_sem_worker,
        test, &options, &test->handle);
    if (!created) {
        preblock_failure(result, PREBLOCK_CASE_CREATED);
        result->cleanup_ok = true;
        __atomic_store_n(&g_preblock_claimed, 0, __ATOMIC_RELEASE);
        return false;
    }
    result->handle = test->handle;
    result->last_completed = PREBLOCK_CASE_CREATED;

    bool started = sync_test_wait_until(&test->started, 1, 5000);
    if (started) {
        result->last_completed = PREBLOCK_CASE_STARTED;
        started = scheduler_snapshot_task_by_handle(test->handle,
                                                     &result->initial);
    }
    if (!started)
        preblock_failure(result, PREBLOCK_CASE_STARTED);

    task_kill_result_t kill = started ? scheduler_request_kill(
        test->handle.id) : TASK_KILL_ERR_INVALID;
    bool kill_accepted = kill == TASK_KILL_ACCEPTED;
    if (kill_accepted)
        result->last_completed = PREBLOCK_CASE_KILL_ACCEPTED;
    else
        preblock_failure(result, PREBLOCK_CASE_KILL_ACCEPTED);

#ifdef HOBBYOS_WAIT_NEGATIVE_IGNORE_PREBLOCK_CANCEL
    bool armed = kill_accepted && !timer &&
        scheduler_test_arm_ignore_preblock_cancel(test->handle,
            TASK_WAIT_SEMAPHORE, (uintptr_t)&test->sem);
    if (!armed)
        preblock_failure(result, PREBLOCK_CASE_RETURNED);
    __atomic_store_n(&test->release, 1, __ATOMIC_RELEASE);
    uint64_t deadline = timer_get_uptime_ms() + 2000;
    task_snapshot_t observed = {0};
    bool lost = false;
    while (timer_get_uptime_ms() < deadline) {
        if (scheduler_snapshot_task_by_handle(test->handle, &observed) &&
            observed.wait_active && observed.kill_pending &&
            observed.wake_reason == TASK_WAKE_NONE) { lost = true; break; }
        timer_sleep(1);
    }
    result->passed = armed && lost;
#else
    __atomic_store_n(&test->release, 1, __ATOMIC_RELEASE);
    bool returned = sem_wait_interruptible(&test->done) ==
                        TASK_WAIT_RESULT_OK &&
        __atomic_load_n(&test->returned, __ATOMIC_ACQUIRE);
    if (returned) {
        result->last_completed = PREBLOCK_CASE_RETURNED;
        result->wait_result = (task_wait_result_t)__atomic_load_n(
            &test->result, __ATOMIC_ACQUIRE);
    } else {
        preblock_failure(result, PREBLOCK_CASE_RETURNED);
    }

    uint64_t deadline = timer_get_uptime_ms() + 5000;
    task_snapshot_t snapshot = {0};
    while (scheduler_snapshot_task_by_handle(test->handle, &snapshot) &&
           snapshot.state != TASK_ZOMBIE &&
           timer_get_uptime_ms() < deadline)
        timer_sleep(1);
    scheduler_wait_stats_snapshot(&after);
    bool held = scheduler_snapshot_task_by_handle(test->handle, &snapshot) &&
        snapshot.state == TASK_ZOMBIE &&
        snapshot.exit_reason == TASK_EXIT_KILLED;
    if (held) {
        result->last_completed = PREBLOCK_CASE_ZOMBIE;
        result->final = snapshot;
        result->exit_reason = snapshot.exit_reason;
    } else {
        preblock_failure(result, PREBLOCK_CASE_ZOMBIE);
    }
    bool exact = timers_test_task_wake_snapshot(test->handle,
                                                 &result->timer_nodes);
    bool validated = returned && held &&
        result->wait_result == TASK_WAIT_RESULT_CANCELLED &&
        after.preblock_cancelled == before.preblock_cancelled + 1 &&
        snapshot.wait_generation == result->initial.wait_generation &&
        !snapshot.wait_active && snapshot.wait_kind == TASK_WAIT_NONE &&
        snapshot.queue_membership == TASK_QUEUE_NONE;
    if (timer) {
        validated = validated &&
            snapshot.timer_ref_acquires == result->initial.timer_ref_acquires &&
            snapshot.timer_ref_releases == result->initial.timer_ref_releases &&
            !snapshot.task_wake_timer_refs && exact &&
            !result->timer_nodes.pending_nodes &&
            !result->timer_nodes.claimed_nodes &&
            !result->timer_nodes.refs_held &&
            !result->timer_nodes.found_wrong_lifecycle;
    }
    if (validated)
        result->last_completed = PREBLOCK_CASE_VALIDATED;
    else
        preblock_failure(result, PREBLOCK_CASE_VALIDATED);
    result->passed = validated;
#endif

    bool cleaned = preblock_case_cleanup(test, result);
    timers_get_stats(&timer_after);
    result->global_armed_delta = timer_after.armed - timer_before.armed;
    result->global_ref_delta = timer_after.task_refs_acquired -
                               timer_before.task_refs_acquired;
    result->passed = result->passed && cleaned;
    if (result->passed)
        result->last_completed = PREBLOCK_CASE_REAPED;

#ifdef HOBBYOS_WAIT_NEGATIVE_IGNORE_PREBLOCK_CANCEL
    if (!quiet)
        serial_write_all(result->passed ?
            "[SYNCTEST][NEGATIVE] PREBLOCK_CANCELLATION_LOST_DETECTED\n" :
            "[SYNCTEST][NEGATIVE] PREBLOCK_CANCELLATION_LOST_MISSED\n");
#else
    if (!quiet) {
        serial_write_all(result->passed ?
            (timer ? "[SYNCTEST][PREBLOCK_TIMER] PASS" :
                     "[SYNCTEST][PREBLOCK_SEM] PASS") :
            (timer ? "[SYNCTEST][PREBLOCK_TIMER] FAIL" :
                     "[SYNCTEST][PREBLOCK_SEM] FAIL"));
        serial_write_all(" cancelled=");
        print_u64(result->wait_result == TASK_WAIT_RESULT_CANCELLED);
        serial_write_all(" handles_gone=");
        print_u64(result->target_gone);
        serial_write_all(" free_inflight=");
        print_u64(result->free_inflight_zero ? 0 : 1);
        serial_write_all(" target_nodes=");
        print_u64(result->timer_nodes.pending_nodes +
                  result->timer_nodes.claimed_nodes);
        serial_write_all(" target_refs=");
        print_u64(result->timer_nodes.refs_held);
        serial_write_all("\n");
    }
#endif
    if (cleaned)
        __atomic_store_n(&g_preblock_claimed, 0, __ATOMIC_RELEASE);
    return result->passed;
}

static int run_preblock_case(bool timer, bool quiet)
{
    preblock_case_result_t result;
    return run_preblock_case_result(timer, quiet, &result) ? 0 : 1;
}

static int run_preblock_loop(uint32_t count, const char *mode)
{
    bool do_sem = !mode || strcmp(mode, "all") == 0 || strcmp(mode, "sem") == 0;
    bool do_timer = !mode || strcmp(mode, "all") == 0 || strcmp(mode, "timer") == 0;
    if (!count || (!do_sem && !do_timer)) return 1;
    uint32_t sem = 0, timer = 0, normal = 0, handles_gone = 0;
    uint64_t wait_residual = 0, timer_residual = 0;
    bool free_inflight_zero = true;
    scheduler_test_set_lifecycle_log_quiet(true);
    for (uint32_t i = 0; i < count; i++) {
        preblock_case_result_t result;
        if (do_sem) {
            bool passed = run_preblock_case_result(false, true, &result);
            normal += result.exit_reason == TASK_EXIT_NORMAL;
            handles_gone += result.target_gone;
            free_inflight_zero = free_inflight_zero &&
                                 result.free_inflight_zero;
            wait_residual += result.final.wait_active ||
                             result.final.wait_kind != TASK_WAIT_NONE ||
                             result.final.queue_membership != TASK_QUEUE_NONE;
            timer_residual += result.timer_nodes.pending_nodes +
                result.timer_nodes.claimed_nodes + result.timer_nodes.refs_held;
            if (!passed)
                break;
            sem++;
        }
        if (do_timer) {
            bool passed = run_preblock_case_result(true, true, &result);
            normal += result.exit_reason == TASK_EXIT_NORMAL;
            handles_gone += result.target_gone;
            free_inflight_zero = free_inflight_zero &&
                                 result.free_inflight_zero;
            wait_residual += result.final.wait_active ||
                             result.final.wait_kind != TASK_WAIT_NONE ||
                             result.final.queue_membership != TASK_QUEUE_NONE;
            timer_residual += result.timer_nodes.pending_nodes +
                result.timer_nodes.claimed_nodes + result.timer_nodes.refs_held;
            if (!passed)
                break;
            timer++;
        }
    }
    scheduler_test_set_lifecycle_log_quiet(false);
    scheduler_runtime_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    bool ok = sem == (do_sem ? count : 0) && timer == (do_timer ? count : 0) &&
              scheduler_validate_runtime_invariants(&stats) &&
              stats.pending_cancel_wait_violations == 0 && !normal &&
              !wait_residual && !timer_residual && free_inflight_zero;
    serial_write_all(ok ? "[SYNCTEST][PREBLOCK_LOOP] PASS count=" :
                          "[SYNCTEST][PREBLOCK_LOOP] FAIL count=");
    print_u64(count); serial_write_all(" sem="); print_u64(sem);
    serial_write_all(" timer="); print_u64(timer);
    serial_write_all(" normal="); print_u64(normal);
    serial_write_all(" handles_gone="); print_u64(handles_gone);
    serial_write_all(" free_inflight="); print_u64(free_inflight_zero ? 0 : 1);
    serial_write_all(" wait_residual="); print_u64(wait_residual);
    serial_write_all(" timer_residual="); print_u64(timer_residual);
    serial_write_all(" violations=");
    print_u64(stats.pending_cancel_wait_violations); serial_write_all("\n");
    return ok ? 0 : 1;
}

static void preblock_timer_noise_worker(void *arg)
{
    preblock_timer_noise_t *noise = arg;
    __atomic_store_n(&noise->started, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&noise->stop, __ATOMIC_ACQUIRE)) {
        if (timer_sleep_interruptible(1) != TASK_WAIT_RESULT_OK)
            break;
        __atomic_add_fetch(&noise->cycles, 1, __ATOMIC_ACQ_REL);
    }
    __atomic_store_n(&noise->stopped, 1, __ATOMIC_RELEASE);
}

static int run_preblock_timer_noise(uint32_t count)
{
    if (!count)
        return 1;
    preblock_timer_noise_t *noise = &g_preblock_noise;
    memset(noise, 0, sizeof(*noise));
    task_create_options_t options = {
        .name = "preblock-timer-noise",
        .task_class = TASK_CLASS_NORMAL,
        .flags = TASK_FLAG_SYSTEM,
        .test_reap_hold = 1
    };
    bool created = thread_create_ex_handle(preblock_timer_noise_worker, noise,
                                            &options, &noise->handle);
    bool started = created && sync_test_wait_until(&noise->started, 1, 5000);
    timer_stats_t timer_before = {0}, timer_after = {0};
    uint64_t starting_cycles = __atomic_load_n(&noise->cycles,
                                                __ATOMIC_ACQUIRE);
    timers_get_stats(&timer_before);
    uint64_t deadline = timer_get_uptime_ms() + 5000;
    while (started && __atomic_load_n(&noise->cycles, __ATOMIC_ACQUIRE) ==
           starting_cycles && timer_get_uptime_ms() < deadline)
        timer_sleep(1);

    uint32_t passed = 0, normal = 0, handles_gone = 0;
    uint64_t target_nodes = 0, target_refs = 0;
    scheduler_test_set_lifecycle_log_quiet(true);
    while (passed < count) {
        preblock_case_result_t result;
        bool ok = run_preblock_case_result(true, true, &result);
        normal += result.exit_reason == TASK_EXIT_NORMAL;
        handles_gone += result.target_gone;
        target_nodes += result.timer_nodes.pending_nodes +
                        result.timer_nodes.claimed_nodes;
        target_refs += result.timer_nodes.refs_held;
        if (!ok)
            break;
        passed++;
    }
    scheduler_test_set_lifecycle_log_quiet(false);

    __atomic_store_n(&noise->stop, 1, __ATOMIC_RELEASE);
    bool stopped = started && sync_test_wait_until(&noise->stopped, 1, 5000);
    task_snapshot_t snapshot = {0};
    deadline = timer_get_uptime_ms() + 5000;
    while (scheduler_snapshot_task_by_handle(noise->handle, &snapshot) &&
           snapshot.state != TASK_ZOMBIE && timer_get_uptime_ms() < deadline)
        timer_sleep(1);
    bool noise_zombie = scheduler_snapshot_task_by_handle(noise->handle,
                                                           &snapshot) &&
                        snapshot.state == TASK_ZOMBIE &&
                        snapshot.exit_reason == TASK_EXIT_NORMAL;
    if (noise_zombie)
        (void)scheduler_test_hold_reap(noise->handle.id, false);
    task_reaper_stats_t reaper = {0};
    deadline = timer_get_uptime_ms() + 5000;
    do {
        (void)scheduler_reap_zombies(0);
        scheduler_reaper_stats_snapshot(&reaper);
        if (!scheduler_snapshot_task_by_handle(noise->handle, &snapshot) &&
            !reaper.free_inflight)
            break;
        timer_sleep(1);
    } while (timer_get_uptime_ms() < deadline);
    bool noise_gone = !scheduler_snapshot_task_by_handle(noise->handle,
                                                          &snapshot) &&
                      !reaper.free_inflight;
    timers_get_stats(&timer_after);
    uint64_t armed_delta = timer_after.armed - timer_before.armed;
    uint64_t ref_delta = timer_after.task_refs_acquired -
                         timer_before.task_refs_acquired;
    bool ok = created && started && stopped && noise_zombie && noise_gone &&
        passed == count && !normal && handles_gone == count &&
        !target_nodes && !target_refs && armed_delta > 0 && ref_delta > 0;
    serial_write_all(ok ? "[SYNCTEST][PREBLOCK_TIMER_NOISE] PASS count=" :
                          "[SYNCTEST][PREBLOCK_TIMER_NOISE] FAIL count=");
    print_u64(count);
    serial_write_all(" global_armed_delta="); print_u64(armed_delta);
    serial_write_all(" global_ref_delta="); print_u64(ref_delta);
    serial_write_all(" target_nodes="); print_u64(target_nodes);
    serial_write_all(" target_refs="); print_u64(target_refs);
    serial_write_all(" target_normal_exits="); print_u64(normal);
    serial_write_all("\n");
    return ok ? 0 : 1;
}

static uint32_t parse_u32(const char *text, uint32_t fallback)
{
    if (!text || !*text) return fallback;
    uint32_t value = 0;
    while (*text) {
        if (*text < '0' || *text > '9') return fallback;
        uint32_t digit = (uint32_t)(*text++ - '0');
        if (value > (UINT32_MAX - digit) / 10u) return fallback;
        value = value * 10u + digit;
    }
    return value;
}

static bool parse_exact_u64(const char *text, uint64_t *out)
{
    if (!text || !*text || !out) return false;
    uint64_t value = 0;
    while (*text) {
        if (*text < '0' || *text > '9') return false;
        uint64_t digit = (uint64_t)(*text++ - '0');
        if (value > (UINT64_MAX - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    *out = value;
    return true;
}

static void print_u64(uint64_t value)
{
    char buf[21];
    uint32_t count = 0;
    do { buf[count++] = (char)('0' + value % 10u); value /= 10u; } while (value);
    while (count) serial_putc_all(buf[--count]);
}

static bool sync_test_wait_until(volatile uint32_t *value, uint32_t expected, uint64_t timeout_ms)
{
    uint64_t start = timer_get_uptime_ms();
    while (__atomic_load_n(value, __ATOMIC_ACQUIRE) != expected) {
        if (timer_get_uptime_ms() - start >= timeout_ms) return false;
        timer_sleep(1);
    }
    return true;
}

void synctest_async_snapshot(synctest_async_snapshot_t *out)
{
    if (!out) return;
    for (;;) {
        uint32_t before = __atomic_load_n(&g_async_publish_sequence,
                                          __ATOMIC_ACQUIRE);
        if (before & 1u) continue;
        out->run = __atomic_load_n(&g_async_active_run, __ATOMIC_RELAXED);
        out->kind = (synctest_async_kind_t)__atomic_load_n(
            &g_async_kind, __ATOMIC_RELAXED);
        out->state = (synctest_async_state_t)__atomic_load_n(
            &g_async_state, __ATOMIC_RELAXED);
        out->requested = __atomic_load_n(&g_async_requested,
                                          __ATOMIC_RELAXED);
        out->completed = __atomic_load_n(&g_async_completed,
                                          __ATOMIC_RELAXED);
        out->errors = __atomic_load_n(&g_async_errors, __ATOMIC_RELAXED);
        out->detail_a = __atomic_load_n(&g_async_detail_a,
                                         __ATOMIC_RELAXED);
        out->detail_b = __atomic_load_n(&g_async_detail_b,
                                         __ATOMIC_RELAXED);
        uint32_t after = __atomic_load_n(&g_async_publish_sequence,
                                         __ATOMIC_ACQUIRE);
        if (before == after && !(after & 1u)) return;
    }
}

bool synctest_async_snapshot_for_run(uint64_t run,
                                     synctest_async_snapshot_t *out)
{
    if (!run || !out) return false;
    synctest_async_snapshot(out);
    uint64_t completed = __atomic_load_n(&g_async_completed_run,
                                          __ATOMIC_ACQUIRE);
    return out->run == run || completed == run;
}

bool synctest_async_completion_contract_valid(void)
{
    synctest_async_snapshot_t snapshot;
    synctest_async_snapshot(&snapshot);
    if (snapshot.state != SYNCTEST_ASYNC_PASS &&
        snapshot.state != SYNCTEST_ASYNC_FAIL)
        return true;
    return snapshot.run != 0 &&
        __atomic_load_n(&g_async_completed_run, __ATOMIC_ACQUIRE) ==
            snapshot.run &&
        __atomic_load_n(&g_any_test_running, __ATOMIC_ACQUIRE) == 0;
}

static void sync_async_publish_begin(void)
{
    (void)__atomic_add_fetch(&g_async_publish_sequence, 1u,
                             __ATOMIC_ACQ_REL);
}

static void sync_async_publish_end(void)
{
    (void)__atomic_add_fetch(&g_async_publish_sequence, 1u,
                             __ATOMIC_RELEASE);
}

static bool sync_async_begin(synctest_async_kind_t kind,
                             uint64_t requested,
                             uint64_t *out_run)
{
    if (__atomic_exchange_n(&g_any_test_running, 1, __ATOMIC_ACQ_REL) != 0) {
        (void)diagnostic_result_complete(
            synctest_async_command_name(kind), 1);
        return false;
    }

    uint64_t previous = __atomic_load_n(&g_async_next_run, __ATOMIC_ACQUIRE);
    if (previous == UINT64_MAX) {
        __atomic_store_n(&g_any_test_running, 0, __ATOMIC_RELEASE);
        (void)diagnostic_result_complete(
            synctest_async_command_name(kind), 1);
        return false;
    }
    uint64_t run = previous + 1u;
    __atomic_store_n(&g_async_next_run, run, __ATOMIC_RELEASE);
    sync_async_publish_begin();
    __atomic_store_n(&g_async_active_run, run, __ATOMIC_RELAXED);
    __atomic_store_n(&g_async_kind, (uint32_t)kind, __ATOMIC_RELAXED);
    __atomic_store_n(&g_async_state, SYNCTEST_ASYNC_RUNNING,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_async_requested, requested, __ATOMIC_RELAXED);
    __atomic_store_n(&g_async_completed, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_async_errors, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_async_detail_a, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_async_detail_b, 0, __ATOMIC_RELAXED);
    sync_async_publish_end();
    if (out_run) *out_run = run;
    sync_contract_emit(kind, "START", run, requested, 0, 0);
    uint64_t diagnostic_generation = diagnostic_result_async_started(
        synctest_async_command_name(kind));
    __atomic_store_n(&g_async_diagnostic_generation,
                     diagnostic_generation, __ATOMIC_RELEASE);
    return true;
}

static uint64_t sync_async_active_run(void)
{
    return __atomic_load_n(&g_async_active_run, __ATOMIC_ACQUIRE);
}

static void sync_async_finish(uint64_t run, bool passed,
                              uint64_t completed, uint64_t errors,
                              uint64_t detail_a, uint64_t detail_b)
{
    synctest_async_kind_t kind = (synctest_async_kind_t)__atomic_load_n(
        &g_async_kind, __ATOMIC_ACQUIRE);
    uint64_t diagnostic_generation = __atomic_load_n(
        &g_async_diagnostic_generation, __ATOMIC_ACQUIRE);
    uint64_t active = __atomic_load_n(&g_async_active_run, __ATOMIC_ACQUIRE);
    if (!run || run != active) {
        passed = false;
        errors++;
    }
    sync_async_publish_begin();
    __atomic_store_n(&g_async_completed, completed, __ATOMIC_RELAXED);
    __atomic_store_n(&g_async_errors, errors, __ATOMIC_RELAXED);
    __atomic_store_n(&g_async_detail_a, detail_a, __ATOMIC_RELAXED);
    __atomic_store_n(&g_async_detail_b, detail_b, __ATOMIC_RELAXED);
    __atomic_store_n(&g_async_completed_run, run, __ATOMIC_RELAXED);
    __atomic_store_n(&g_async_state,
                     passed ? SYNCTEST_ASYNC_PASS : SYNCTEST_ASYNC_FAIL,
                     __ATOMIC_RELAXED);
    sync_async_publish_end();

    /* Completion publication is observable only after the busy gate clears. */
    __atomic_store_n(&g_any_test_running, 0, __ATOMIC_RELEASE);
    sync_contract_emit(kind, passed ? "PASS" : "FAIL", run,
                       __atomic_load_n(&g_async_requested, __ATOMIC_ACQUIRE),
                       completed, errors);
    diagnostic_result_async_completed(
        synctest_async_command_name(kind), diagnostic_generation, passed);
}

static void sync_async_emit_snapshot(const char *tag, bool passed,
                                     const char *reason,
                                     const synctest_async_snapshot_t *snapshot)
{
    sync_contract_record_t record;
    sync_contract_reset(&record);
    (void)sync_contract_char(&record, '\n');
    (void)sync_contract_text(&record, "[SYNC][");
    (void)sync_contract_text(&record, tag);
    (void)sync_contract_text(&record, "] ");
    (void)sync_contract_text(&record, passed ? "PASS" : "FAIL");
    if (reason) {
        (void)sync_contract_text(&record, " reason=");
        (void)sync_contract_text(&record, reason);
    }
    (void)sync_contract_text(&record, " run=");
    (void)sync_contract_u64(&record, snapshot->run);
    (void)sync_contract_text(&record, " kind=");
    (void)sync_contract_text(&record, sync_async_kind_name(snapshot->kind));
    (void)sync_contract_text(&record, " state=");
    (void)sync_contract_text(&record, sync_async_state_name(snapshot->state));
    (void)sync_contract_text(&record, " requested=");
    (void)sync_contract_u64(&record, snapshot->requested);
    (void)sync_contract_text(&record, " completed=");
    (void)sync_contract_u64(&record, snapshot->completed);
    (void)sync_contract_text(&record, " errors=");
    (void)sync_contract_u64(&record, snapshot->errors);
    (void)sync_contract_finish(&record);
    sync_contract_emit_record(&record);
}

static bool sync_async_run_known(uint64_t run,
                                 const synctest_async_snapshot_t *snapshot)
{
    if (!run) return false;
    uint64_t completed = __atomic_load_n(&g_async_completed_run,
                                          __ATOMIC_ACQUIRE);
    return run == snapshot->run || run == completed;
}

static int run_async_status(uint64_t requested_run, bool has_run)
{
    synctest_async_snapshot_t snapshot;
    synctest_async_snapshot(&snapshot);
    if (has_run && !sync_async_run_known(requested_run, &snapshot)) {
        snapshot.run = requested_run;
        sync_async_emit_snapshot("ASYNC_STATUS", false, "unknown-run",
                                 &snapshot);
        return 1;
    }
    sync_async_emit_snapshot("ASYNC_STATUS", true, NULL, &snapshot);
    return 0;
}

static int run_async_wait(uint64_t run, uint64_t timeout_ms)
{
    uint64_t start = timer_get_uptime_ms();
    for (;;) {
        synctest_async_snapshot_t snapshot;
        synctest_async_snapshot(&snapshot);
        if (!sync_async_run_known(run, &snapshot)) {
            snapshot.run = run;
            sync_async_emit_snapshot("ASYNC_WAIT", false, "unknown-run",
                                     &snapshot);
            return 1;
        }
        if (snapshot.run == run && snapshot.state == SYNCTEST_ASYNC_PASS) {
            sync_async_emit_snapshot("ASYNC_WAIT", true, NULL, &snapshot);
            return 0;
        }
        if (snapshot.run == run && snapshot.state == SYNCTEST_ASYNC_FAIL) {
            sync_async_emit_snapshot("ASYNC_WAIT", false, "workload",
                                     &snapshot);
            return 1;
        }
        if (timer_get_uptime_ms() - start >= timeout_ms) {
            snapshot.run = run;
            sync_async_emit_snapshot("ASYNC_WAIT", false, "timeout",
                                     &snapshot);
            return 1;
        }
        timer_sleep(10);
    }
}

static void sem_waiter_worker(void *arg)
{
    sync_worker_ctx_t *ctx = (sync_worker_ctx_t *)arg;
    (void)ctx;
    while (!__atomic_load_n(&g_sem_test.start, __ATOMIC_ACQUIRE)) schedule_voluntary();
    if (__atomic_load_n(&g_sem_test.abort, __ATOMIC_ACQUIRE)) {
        __atomic_sub_fetch(&g_sem_test.remaining, 1, __ATOMIC_ACQ_REL);
        return;
    }
    for (;;) {
        uint32_t ticket = __atomic_fetch_add(&g_sem_test.next_wait, 1, __ATOMIC_RELAXED);
        if (ticket >= g_sem_test.iterations) break;
        task_wait_result_t result = sem_wait_interruptible(&g_sem_test.sem);
        if (result == TASK_WAIT_RESULT_OK) __atomic_add_fetch(&g_sem_test.waits_ok, 1, __ATOMIC_RELAXED);
        else if (result == TASK_WAIT_RESULT_CANCELLED) __atomic_add_fetch(&g_sem_test.cancelled, 1, __ATOMIC_RELAXED);
        else __atomic_add_fetch(&g_sem_test.errors, 1, __ATOMIC_RELAXED);
    }
    __atomic_sub_fetch(&g_sem_test.remaining, 1, __ATOMIC_ACQ_REL);
}

static void sem_signaler_worker(void *arg)
{
    sync_worker_ctx_t *ctx = (sync_worker_ctx_t *)arg;
    (void)ctx;
    while (!__atomic_load_n(&g_sem_test.start, __ATOMIC_ACQUIRE)) schedule_voluntary();
    if (__atomic_load_n(&g_sem_test.abort, __ATOMIC_ACQUIRE)) {
        __atomic_sub_fetch(&g_sem_test.remaining, 1, __ATOMIC_ACQ_REL);
        return;
    }
    for (;;) {
        uint32_t ticket = __atomic_fetch_add(&g_sem_test.next_signal, 1, __ATOMIC_RELAXED);
        if (ticket >= g_sem_test.iterations) break;
        if (sem_signal(&g_sem_test.sem)) __atomic_add_fetch(&g_sem_test.signals_ok, 1, __ATOMIC_RELAXED);
        else __atomic_add_fetch(&g_sem_test.errors, 1, __ATOMIC_RELAXED);
        if ((ticket & 63u) == 0) schedule_voluntary();
    }
    __atomic_sub_fetch(&g_sem_test.remaining, 1, __ATOMIC_ACQ_REL);
}

static void sem_controller(void *arg)
{
    (void)arg;
    uint64_t run = sync_async_active_run();
    uint32_t total = g_sem_test.waiters + g_sem_test.signalers;
    uint32_t created_count = 0;
    bool created = true;
    for (uint32_t i = 0; i < g_sem_test.waiters; i++) {
        g_sem_test.workers[i].index = i;
        if (!thread_create_named(sem_waiter_worker, &g_sem_test.workers[i], "synctest-sem-wait")) created = false;
        else created_count++;
    }
    for (uint32_t i = 0; i < g_sem_test.signalers; i++) {
        uint32_t slot = g_sem_test.waiters + i;
        g_sem_test.workers[slot].index = i;
        if (!thread_create_named(sem_signaler_worker, &g_sem_test.workers[slot], "synctest-sem-signal")) created = false;
        else created_count++;
    }
    if (!created) {
        serial_write_all("[SYNC][SEM] FAIL reason=create\n");
        __atomic_store_n(&g_sem_test.remaining, created_count, __ATOMIC_RELEASE);
        __atomic_store_n(&g_sem_test.abort, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&g_sem_test.start, 1, __ATOMIC_RELEASE);
        (void)sync_test_wait_until(&g_sem_test.remaining, 0, 5000);
        sync_async_finish(run, false, created_count, 1, created_count, total);
        return;
    }
    __atomic_store_n(&g_sem_test.remaining, total, __ATOMIC_RELEASE);
    __atomic_store_n(&g_sem_test.start, 1, __ATOMIC_RELEASE);
    bool complete = sync_test_wait_until(&g_sem_test.remaining, 0, SYNC_WATCHDOG_MS);
    bool ok = complete && g_sem_test.waits_ok == g_sem_test.iterations &&
              g_sem_test.signals_ok == g_sem_test.iterations &&
              g_sem_test.cancelled == 0 && g_sem_test.errors == 0 &&
              g_sem_test.sem.count == 0 && list_empty(&g_sem_test.sem.wait_queue.head);
    serial_write_all("[SYNC][SEM] "); serial_write_all(ok ? "PASS" : "FAIL");
    serial_write_all(" iterations="); print_u64(g_sem_test.iterations);
    serial_write_all(" waiters="); print_u64(g_sem_test.waiters);
    serial_write_all(" signalers="); print_u64(g_sem_test.signalers);
    serial_write_all(" waits="); print_u64(g_sem_test.waits_ok);
    serial_write_all(" signals="); print_u64(g_sem_test.signals_ok);
    serial_write_all(" permits="); print_u64((uint64_t)(g_sem_test.sem.count < 0 ? 0 : g_sem_test.sem.count));
    serial_write_all(" incomplete="); print_u64(complete ? 0 : 1);
    serial_write_all(" errors="); print_u64(g_sem_test.errors);
    serial_write_all("\n");
    sync_async_finish(run, ok, g_sem_test.waits_ok, g_sem_test.errors,
                      g_sem_test.signals_ok,
                      (uint64_t)(g_sem_test.sem.count < 0 ? 0 :
                                 g_sem_test.sem.count));
}

static int start_sem_test(uint32_t iterations, uint32_t waiters, uint32_t signalers)
{
    uint64_t run = 0;
    if (!iterations || !waiters || !signalers ||
        waiters + signalers > SYNC_MAX_WORKERS ||
        !sync_async_begin(SYNCTEST_ASYNC_SEM, iterations, &run)) return 1;
    memset(&g_sem_test, 0, sizeof(g_sem_test));
    sem_init(&g_sem_test.sem, 0);
    g_sem_test.iterations = iterations;
    g_sem_test.waiters = waiters;
    g_sem_test.signalers = signalers;
    if (!thread_create_named(sem_controller, NULL, "synctest-sem-ctl")) {
        sync_async_finish(run, false, 0, 1, 0, waiters + signalers);
        return 1;
    }
    return 0;
}

static const char *boundary_failure_name(boundary_failure_t failure)
{
    switch (failure) {
    case BOUNDARY_FAILURE_NONE: return "NONE";
    case BOUNDARY_FAILURE_CREATE: return "CREATE";
    case BOUNDARY_FAILURE_ARM: return "ARM";
    case BOUNDARY_FAILURE_PREPARE_TIMEOUT: return "PREPARE_TIMEOUT";
    case BOUNDARY_FAILURE_HOOK_CROSSTALK: return "HOOK_CROSSTALK";
    case BOUNDARY_FAILURE_HOOK_ROUND: return "HOOK_ROUND";
    case BOUNDARY_FAILURE_SIGNAL_PERMIT: return "SIGNAL_PERMIT";
    case BOUNDARY_FAILURE_SIGNAL_ERROR: return "SIGNAL_ERROR";
    case BOUNDARY_FAILURE_COMPLETION_TIMEOUT: return "COMPLETION_TIMEOUT";
    case BOUNDARY_FAILURE_WAIT_RESULT: return "WAIT_RESULT";
    case BOUNDARY_FAILURE_THROUGHPUT_TIMEOUT: return "THROUGHPUT_TIMEOUT";
    case BOUNDARY_FAILURE_CLEANUP: return "CLEANUP";
    default: return "UNKNOWN";
    }
}

static void boundary_fail(boundary_failure_t failure, uint64_t round)
{
    boundary_failure_t expected = BOUNDARY_FAILURE_NONE;
    if (__atomic_compare_exchange_n(&g_boundary.first_failure, &expected,
                                    failure, false, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE))
        __atomic_store_n(&g_boundary.failure_round, round, __ATOMIC_RELEASE);
}

static void boundary_progress(void)
{
    __atomic_store_n(&g_boundary.last_progress_ms, timer_get_uptime_ms(),
                     __ATOMIC_RELEASE);
}

static bool boundary_wait_round(volatile uint64_t *value, uint64_t expected,
                                boundary_failure_t stall_failure,
                                uint64_t round)
{
    while (__atomic_load_n(value, __ATOMIC_ACQUIRE) < expected) {
        if (__atomic_load_n(&g_boundary.stop, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&g_boundary.first_failure, __ATOMIC_ACQUIRE) !=
                BOUNDARY_FAILURE_NONE)
            return false;
        uint64_t last = __atomic_load_n(&g_boundary.last_progress_ms,
                                        __ATOMIC_ACQUIRE);
        uint64_t now = timer_get_uptime_ms();
        if (__atomic_load_n(value, __ATOMIC_ACQUIRE) >= expected)
            return true;
        if (now - g_boundary.started_ms >= BOUNDARY_TOTAL_WATCHDOG_MS) {
            boundary_fail(BOUNDARY_FAILURE_THROUGHPUT_TIMEOUT, round);
            return false;
        }
        if (now >= last && now - last >= BOUNDARY_STALL_WATCHDOG_MS) {
            boundary_fail(stall_failure, round);
            return false;
        }
        timer_sleep(1);
    }
    return true;
}

static void boundary_hook(const sem_test_after_prepare_event_t *event,
                          void *ctx)
{
    sync_boundary_test_t *test = (sync_boundary_test_t *)ctx;
    if (!event || test != &g_boundary ||
        event->semaphore != &test->sem ||
        event->task.id != test->waiter_handle.id ||
        event->task.lifecycle_generation !=
            test->waiter_handle.lifecycle_generation ||
        event->wait_kind != TASK_WAIT_SEMAPHORE) {
        boundary_fail(BOUNDARY_FAILURE_HOOK_CROSSTALK,
                      __atomic_load_n(&test->waiter_round,
                                      __ATOMIC_ACQUIRE));
        return;
    }

    uint64_t round = __atomic_load_n(&test->waiter_round, __ATOMIC_ACQUIRE);
    uint64_t prepared = __atomic_load_n(&test->prepared_round,
                                        __ATOMIC_ACQUIRE);
    uint64_t previous_generation = __atomic_load_n(
        &test->last_wait_generation, __ATOMIC_ACQUIRE);
#ifdef HOBBYOS_SYNC_NEGATIVE_OLD_SEM_GAP
    bool event_valid = !event->prepared && event->wait_generation == 0;
#else
    bool event_valid = event->prepared &&
                       event->wait_generation > previous_generation;
#endif
    if (!round || prepared + 1 != round || !event_valid) {
        boundary_fail(BOUNDARY_FAILURE_HOOK_ROUND, round);
        return;
    }
    __atomic_store_n(&test->last_event_prepared, event->prepared,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&test->last_wait_generation, event->wait_generation,
                     __ATOMIC_RELEASE);
    __atomic_add_fetch(&test->target_hook_hits, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&test->prepared_round, round, __ATOMIC_RELEASE);
    boundary_progress();
#ifdef HOBBYOS_SYNC_NEGATIVE_OLD_SEM_GAP
    semaphore_debug_snapshot_t before_signal = {0};
    (void)semaphore_debug_snapshot(&test->sem, &before_signal);
    sem_signal_result_t signal = sem_signal_detailed(&test->sem);
    if (before_signal.waiters == 0 && signal == SEM_SIGNAL_ADDED_PERMIT) {
        __atomic_add_fetch(&test->signals_permit, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&test->signaled_round, round, __ATOMIC_RELEASE);
        __atomic_store_n(&test->negative_detected, 1, __ATOMIC_RELEASE);
        boundary_progress();
    } else {
        __atomic_add_fetch(&test->errors, 1, __ATOMIC_RELAXED);
        boundary_fail(signal == SEM_SIGNAL_WOKE_WAITER ?
                          BOUNDARY_FAILURE_HOOK_ROUND :
                          BOUNDARY_FAILURE_SIGNAL_ERROR,
                      round);
    }
#endif
}

static void boundary_waiter(void *arg)
{
    (void)arg;
    __atomic_store_n(&g_boundary.waiter_started, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&g_boundary.start, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&g_boundary.stop, __ATOMIC_ACQUIRE))
        timer_sleep(1);
    for (uint64_t round = 1; round <= g_boundary.rounds; round++) {
        if (__atomic_load_n(&g_boundary.stop, __ATOMIC_ACQUIRE))
            break;
        __atomic_store_n(&g_boundary.waiter_round, round, __ATOMIC_RELEASE);
        boundary_progress();
        task_wait_result_t result = sem_wait_interruptible(&g_boundary.sem);
        if (result != TASK_WAIT_RESULT_OK) {
            __atomic_add_fetch(&g_boundary.errors, 1, __ATOMIC_RELAXED);
            if (!__atomic_load_n(&g_boundary.stop, __ATOMIC_ACQUIRE))
                boundary_fail(BOUNDARY_FAILURE_WAIT_RESULT, round);
            break;
        }
        __atomic_add_fetch(&g_boundary.waits_ok, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&g_boundary.completed_round, round, __ATOMIC_RELEASE);
        boundary_progress();
        while (__atomic_load_n(&g_boundary.released_round,
                               __ATOMIC_ACQUIRE) < round &&
               !__atomic_load_n(&g_boundary.stop, __ATOMIC_ACQUIRE))
            timer_sleep(1);
    }
    __atomic_store_n(&g_boundary.waiter_done, 1, __ATOMIC_RELEASE);
    task_cancel_point();
}

static void boundary_noise_waiter(void *arg)
{
    (void)arg;
    __atomic_store_n(&g_boundary_noise.waiter_started, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&g_boundary_noise.start, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&g_boundary_noise.stop, __ATOMIC_ACQUIRE))
        timer_sleep(1);
    for (uint64_t round = 1; round <= g_boundary_noise.rounds; round++) {
        if (__atomic_load_n(&g_boundary_noise.stop, __ATOMIC_ACQUIRE))
            break;
        __atomic_store_n(&g_boundary_noise.waiter_round, round,
                         __ATOMIC_RELEASE);
        task_wait_result_t result = sem_wait_interruptible(
            &g_boundary_noise.sem);
        if (result != TASK_WAIT_RESULT_OK) {
            __atomic_add_fetch(&g_boundary_noise.errors, 1,
                               __ATOMIC_RELAXED);
            break;
        }
        __atomic_add_fetch(&g_boundary_noise.waits_ok, 1,
                           __ATOMIC_RELAXED);
        __atomic_store_n(&g_boundary_noise.completed_round, round,
                         __ATOMIC_RELEASE);
        boundary_progress();
        while (__atomic_load_n(&g_boundary_noise.released_round,
                               __ATOMIC_ACQUIRE) < round &&
               !__atomic_load_n(&g_boundary_noise.stop, __ATOMIC_ACQUIRE))
            timer_sleep(1);
    }
    __atomic_store_n(&g_boundary_noise.waiter_done, 1, __ATOMIC_RELEASE);
    task_cancel_point();
}

static bool boundary_run_noise_round(uint64_t round)
{
    if (!boundary_wait_round(&g_boundary_noise.waiter_round, round,
                             BOUNDARY_FAILURE_PREPARE_TIMEOUT, round))
        return false;
    for (;;) {
        semaphore_debug_snapshot_t snapshot = {0};
        (void)semaphore_debug_snapshot(&g_boundary_noise.sem, &snapshot);
        if (snapshot.waiters)
            break;
        uint64_t last = __atomic_load_n(&g_boundary.last_progress_ms,
                                        __ATOMIC_ACQUIRE);
        uint64_t now = timer_get_uptime_ms();
        (void)semaphore_debug_snapshot(&g_boundary_noise.sem, &snapshot);
        if (snapshot.waiters)
            break;
        if (now >= last && now - last >= BOUNDARY_STALL_WATCHDOG_MS) {
            boundary_fail(BOUNDARY_FAILURE_PREPARE_TIMEOUT, round);
            return false;
        }
        if (now - g_boundary.started_ms >= BOUNDARY_TOTAL_WATCHDOG_MS) {
            boundary_fail(BOUNDARY_FAILURE_THROUGHPUT_TIMEOUT, round);
            return false;
        }
        timer_sleep(1);
    }
    sem_signal_result_t signal = sem_signal_detailed(&g_boundary_noise.sem);
    if (signal == SEM_SIGNAL_WOKE_WAITER) {
        __atomic_add_fetch(&g_boundary_noise.signals_woke, 1,
                           __ATOMIC_RELAXED);
    } else if (signal == SEM_SIGNAL_ADDED_PERMIT) {
        __atomic_add_fetch(&g_boundary_noise.signals_permit, 1,
                           __ATOMIC_RELAXED);
        boundary_fail(BOUNDARY_FAILURE_SIGNAL_PERMIT, round);
        return false;
    } else {
        __atomic_add_fetch(&g_boundary_noise.errors, 1, __ATOMIC_RELAXED);
        boundary_fail(BOUNDARY_FAILURE_SIGNAL_ERROR, round);
        return false;
    }
    boundary_progress();
    if (!boundary_wait_round(&g_boundary_noise.completed_round, round,
                             BOUNDARY_FAILURE_COMPLETION_TIMEOUT, round))
        return false;
    __atomic_store_n(&g_boundary_noise.released_round, round,
                     __ATOMIC_RELEASE);
    boundary_progress();
    return true;
}

static bool boundary_wait_zombie_or_gone(task_handle_t handle)
{
    if (handle.id == TASK_ID_INVALID)
        return true;
    uint64_t deadline = timer_get_uptime_ms() + BOUNDARY_CLEANUP_WATCHDOG_MS;
    task_snapshot_t snapshot = {0};
    while (timer_get_uptime_ms() < deadline) {
        if (!scheduler_snapshot_task_by_handle(handle, &snapshot))
            return true;
        if (snapshot.state == TASK_ZOMBIE)
            return true;
        timer_sleep(1);
    }
    return false;
}

static bool boundary_wait_gone(task_handle_t handle)
{
    if (handle.id == TASK_ID_INVALID)
        return true;
    uint64_t deadline = timer_get_uptime_ms() + BOUNDARY_CLEANUP_WATCHDOG_MS;
    task_snapshot_t snapshot = {0};
    task_reaper_stats_t reaper = {0};
    while (timer_get_uptime_ms() < deadline) {
        (void)scheduler_reap_zombies(0);
        scheduler_reaper_stats_snapshot(&reaper);
        if (!scheduler_snapshot_task_by_handle(handle, &snapshot) &&
            !reaper.free_inflight)
            return true;
        timer_sleep(1);
    }
    return false;
}

static bool boundary_cleanup(bool target_created, bool noise_created)
{
    __atomic_store_n(&g_boundary.stop, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_boundary.start, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_boundary.released_round, UINT64_MAX,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_boundary_noise.stop, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_boundary_noise.start, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_boundary_noise.released_round, UINT64_MAX,
                     __ATOMIC_RELEASE);

    semaphore_debug_snapshot_t sem = {0}, foreign = {0};
    (void)semaphore_debug_snapshot(&g_boundary.sem, &sem);
    if (sem.waiters)
        (void)sem_signal_detailed(&g_boundary.sem);
    (void)semaphore_debug_snapshot(&g_boundary_noise.sem, &foreign);
    if (foreign.waiters)
        (void)sem_signal_detailed(&g_boundary_noise.sem);

    task_snapshot_t task = {0};
    if (target_created &&
        scheduler_snapshot_task_by_handle(g_boundary.waiter_handle, &task) &&
        task.state != TASK_ZOMBIE)
        (void)scheduler_request_kill(g_boundary.waiter_handle.id);
    if (noise_created &&
        scheduler_snapshot_task_by_handle(g_boundary_noise.waiter_handle,
                                          &task) &&
        task.state != TASK_ZOMBIE)
        (void)scheduler_request_kill(g_boundary_noise.waiter_handle.id);

    bool observer_clear = sem_test_clear_after_prepare_observer(
        BOUNDARY_CLEANUP_WATCHDOG_MS);
    bool target_zombie = !target_created ||
        boundary_wait_zombie_or_gone(g_boundary.waiter_handle);
    bool noise_zombie = !noise_created ||
        boundary_wait_zombie_or_gone(g_boundary_noise.waiter_handle);
    if (target_created)
        (void)scheduler_test_hold_reap(g_boundary.waiter_handle.id, false);
    if (noise_created)
        (void)scheduler_test_hold_reap(g_boundary_noise.waiter_handle.id,
                                       false);
    bool target_gone = !target_created ||
        boundary_wait_gone(g_boundary.waiter_handle);
    bool noise_gone = !noise_created ||
        boundary_wait_gone(g_boundary_noise.waiter_handle);

    sem_test_after_prepare_snapshot_t observer = {0};
    sem_test_after_prepare_snapshot(&observer);
    (void)semaphore_debug_snapshot(&g_boundary.sem, &sem);
    (void)semaphore_debug_snapshot(&g_boundary_noise.sem, &foreign);
    task_reaper_stats_t reaper = {0};
    scheduler_reaper_stats_snapshot(&reaper);
    return observer_clear && target_zombie && noise_zombie && target_gone &&
           noise_gone && !observer.active && !observer.callback_inflight &&
           sem.waiters == 0 && foreign.waiters == 0 && reaper.free_inflight == 0;
}

static void boundary_emit_failure(
    const boundary_failure_observation_t *failure_observation,
    const task_snapshot_t *task, bool waiter_present,
    const semaphore_debug_snapshot_t *sem,
    const sem_test_after_prepare_snapshot_t *observer,
    bool cleanup_ok)
{
    boundary_failure_t failure = failure_observation->failure;
    bool wake_lost_confirmed =
        failure == BOUNDARY_FAILURE_COMPLETION_TIMEOUT && waiter_present &&
        failure_observation->prepared == failure_observation->failure_round &&
        failure_observation->signaled == failure_observation->failure_round &&
        sem->count == 0 && sem->waiters == 0 && !task->wait_active &&
        task->queue_membership != TASK_QUEUE_WAIT &&
        task->state == TASK_BLOCKED && observer->foreign_hits == 0;
    serial_write_all("[SYNC][BOUNDARY] FAIL reason=");
    serial_write_all(wake_lost_confirmed ? "WAKE_LOST_CONFIRMED" :
                     boundary_failure_name(failure));
    serial_write_all(" round="); print_u64(failure_observation->failure_round);
    serial_write_all(" rounds="); print_u64(failure_observation->rounds);
    serial_write_all(" prepared="); print_u64(failure_observation->prepared);
    serial_write_all(" signaled="); print_u64(failure_observation->signaled);
    serial_write_all(" completed="); print_u64(failure_observation->completed);
    serial_write_all(" released="); print_u64(failure_observation->released);
    serial_write_all(" waits="); print_u64(failure_observation->waits);
    serial_write_all(" signals_woke="); print_u64(failure_observation->signals_woke);
    serial_write_all(" signals_permit="); print_u64(failure_observation->signals_permit);
    serial_write_all(" errors="); print_u64(failure_observation->errors);
    serial_write_all(" target_hits="); print_u64(observer->target_hits);
    serial_write_all(" foreign_hits="); print_u64(observer->foreign_hits);
    serial_write_all(" observer_inflight=");
    print_u64(observer->callback_inflight);
    serial_write_all(" sem_count="); print_u64((uint64_t)(sem->count < 0 ? 0 : sem->count));
    serial_write_all(" sem_waiters="); print_u64(sem->waiters);
    serial_write_all(" waiter_present="); print_u64(waiter_present);
    serial_write_all(" waiter_state=");
    serial_write_all(waiter_present ? scheduler_task_state_to_string(task->state) :
                     "ABSENT");
    serial_write_all(" waiter_on_cpu="); print_u64(waiter_present ? task->on_cpu : 0);
    serial_write_all(" waiter_cpu="); print_u64(waiter_present ? task->current_cpu_slot : TASK_CPU_SLOT_NONE);
    serial_write_all(" waiter_queue="); print_u64(waiter_present ? task->queue_membership : TASK_QUEUE_NONE);
    serial_write_all(" wait_active="); print_u64(waiter_present ? task->wait_active : 0);
    serial_write_all(" wait_kind="); print_u64(waiter_present ? task->wait_kind : TASK_WAIT_NONE);
    serial_write_all(" wait_generation="); print_u64(waiter_present ? task->wait_generation : 0);
    serial_write_all(" wake_reason="); print_u64(waiter_present ? task->wake_reason : TASK_WAKE_NONE);
    serial_write_all(" elapsed_ms=");
    print_u64(failure_observation->observed_ms -
              failure_observation->started_ms);
    serial_write_all(" stalled_ms=");
    print_u64(failure_observation->observed_ms >=
                      failure_observation->last_progress_ms ?
                  failure_observation->observed_ms -
                      failure_observation->last_progress_ms : 0);
    serial_write_all(" cleanup="); serial_write_all(cleanup_ok ? "PASS" : "FAIL");
    serial_write_all("\n");
}

static void boundary_controller(void *arg)
{
    (void)arg;
    uint64_t run = sync_async_active_run();
    bool noise_requested = g_boundary_noise.rounds != 0;
    bool target_created = false, noise_created = false, observer_armed = false;
    task_create_options_t target_options = {
        .name = "synctest-boundary-wait",
        .task_class = TASK_CLASS_NORMAL,
        .flags = TASK_FLAG_SYSTEM | TASK_FLAG_KILLABLE,
        .test_reap_hold = 1
    };
    task_create_options_t noise_options = {
        .name = "synctest-boundary-foreign",
        .task_class = TASK_CLASS_NORMAL,
        .flags = TASK_FLAG_SYSTEM | TASK_FLAG_KILLABLE,
        .test_reap_hold = 1
    };
    g_boundary.started_ms = timer_get_uptime_ms();
    boundary_progress();
    target_created = thread_create_ex_handle(
        boundary_waiter, NULL, &target_options, &g_boundary.waiter_handle);
    if (noise_requested)
        noise_created = thread_create_ex_handle(
            boundary_noise_waiter, NULL, &noise_options,
            &g_boundary_noise.waiter_handle);
    if (!target_created || (noise_requested && !noise_created))
        boundary_fail(BOUNDARY_FAILURE_CREATE, 0);
    if (target_created &&
        !sync_test_wait_until(&g_boundary.waiter_started, 1, 5000))
        boundary_fail(BOUNDARY_FAILURE_CREATE, 0);
    if (noise_created &&
        !sync_test_wait_until(&g_boundary_noise.waiter_started, 1, 5000))
        boundary_fail(BOUNDARY_FAILURE_CREATE, 0);
    if (g_boundary.first_failure == BOUNDARY_FAILURE_NONE) {
        observer_armed = sem_test_arm_after_prepare_observer(
            &g_boundary.sem, g_boundary.waiter_handle, boundary_hook,
            &g_boundary);
        if (!observer_armed)
            boundary_fail(BOUNDARY_FAILURE_ARM, 0);
    }
    if (observer_armed) {
        __atomic_store_n(&g_boundary_noise.start, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&g_boundary.start, 1, __ATOMIC_RELEASE);
    }

    for (uint64_t round = 1;
         observer_armed && round <= g_boundary.rounds &&
         g_boundary.first_failure == BOUNDARY_FAILURE_NONE;
         round++) {
        if (!boundary_wait_round(&g_boundary.prepared_round, round,
                                 BOUNDARY_FAILURE_PREPARE_TIMEOUT, round))
            break;
#ifdef HOBBYOS_SYNC_NEGATIVE_OLD_SEM_GAP
        /* The pre-prepare hook performed the causal signal itself. */
        if (__atomic_load_n(&g_boundary.negative_detected,
                            __ATOMIC_ACQUIRE))
            break;
#endif
        semaphore_debug_snapshot_t before_signal = {0};
        (void)semaphore_debug_snapshot(&g_boundary.sem, &before_signal);
        sem_signal_result_t signal = sem_signal_detailed(&g_boundary.sem);
        if (signal == SEM_SIGNAL_WOKE_WAITER) {
            __atomic_add_fetch(&g_boundary.signals_woke, 1,
                               __ATOMIC_RELAXED);
        } else if (signal == SEM_SIGNAL_ADDED_PERMIT) {
            __atomic_add_fetch(&g_boundary.signals_permit, 1,
                               __ATOMIC_RELAXED);
            boundary_fail(BOUNDARY_FAILURE_SIGNAL_PERMIT, round);
            break;
        } else {
            __atomic_add_fetch(&g_boundary.errors, 1, __ATOMIC_RELAXED);
            boundary_fail(BOUNDARY_FAILURE_SIGNAL_ERROR, round);
            break;
        }
        __atomic_store_n(&g_boundary.signaled_round, round,
                         __ATOMIC_RELEASE);
        boundary_progress();
        if (!boundary_wait_round(&g_boundary.completed_round, round,
                                 BOUNDARY_FAILURE_COMPLETION_TIMEOUT, round))
            break;
        __atomic_store_n(&g_boundary.released_round, round,
                         __ATOMIC_RELEASE);
        boundary_progress();
        if (noise_requested && round <= g_boundary_noise.rounds &&
            !boundary_run_noise_round(round))
            break;
    }

#ifndef HOBBYOS_SYNC_NEGATIVE_OLD_SEM_GAP
    for (uint64_t round = (uint64_t)g_boundary.rounds + 1;
         observer_armed && noise_requested &&
         round <= g_boundary_noise.rounds &&
         g_boundary.first_failure == BOUNDARY_FAILURE_NONE;
         round++)
        if (!boundary_run_noise_round(round))
            break;
    if (g_boundary.first_failure == BOUNDARY_FAILURE_NONE &&
        !sync_test_wait_until(&g_boundary.waiter_done, 1,
                              BOUNDARY_STALL_WATCHDOG_MS))
        boundary_fail(BOUNDARY_FAILURE_COMPLETION_TIMEOUT,
                      g_boundary.rounds);
    if (noise_requested &&
        g_boundary.first_failure == BOUNDARY_FAILURE_NONE &&
        !sync_test_wait_until(&g_boundary_noise.waiter_done, 1,
                              BOUNDARY_STALL_WATCHDOG_MS))
        boundary_fail(BOUNDARY_FAILURE_COMPLETION_TIMEOUT,
                      g_boundary_noise.rounds);
#endif

    sem_test_after_prepare_snapshot_t observed = {0};
    sem_test_after_prepare_snapshot(&observed);
    __atomic_store_n(&g_boundary.foreign_hook_hits, observed.foreign_hits,
                     __ATOMIC_RELEASE);
    semaphore_debug_snapshot_t sem_before_cleanup = {0};
    (void)semaphore_debug_snapshot(&g_boundary.sem, &sem_before_cleanup);
    task_snapshot_t task_before_cleanup = {0};
    bool waiter_present = target_created &&
        scheduler_snapshot_task_by_handle(g_boundary.waiter_handle,
                                          &task_before_cleanup);
    boundary_failure_observation_t failure_observation = {
        .failure = g_boundary.first_failure,
        .failure_round = g_boundary.failure_round,
        .rounds = g_boundary.rounds,
        .prepared = g_boundary.prepared_round,
        .signaled = g_boundary.signaled_round,
        .completed = g_boundary.completed_round,
        .released = g_boundary.released_round,
        .waits = g_boundary.waits_ok,
        .signals_woke = g_boundary.signals_woke,
        .signals_permit = g_boundary.signals_permit,
        .errors = g_boundary.errors,
        .started_ms = g_boundary.started_ms,
        .last_progress_ms = g_boundary.last_progress_ms,
        .observed_ms = timer_get_uptime_ms()
    };

#ifdef HOBBYOS_SYNC_NEGATIVE_OLD_SEM_GAP
    bool expected_negative = g_boundary.negative_detected &&
        g_boundary.target_hook_hits == 1 &&
        g_boundary.signals_permit == 1 && g_boundary.signals_woke == 0;
#else
    bool expected_negative = false;
#endif
    bool counts_ok = g_boundary.first_failure == BOUNDARY_FAILURE_NONE &&
        g_boundary.waits_ok == g_boundary.rounds &&
        g_boundary.prepared_round == g_boundary.rounds &&
        g_boundary.signaled_round == g_boundary.rounds &&
        g_boundary.completed_round == g_boundary.rounds &&
        g_boundary.released_round == g_boundary.rounds &&
        g_boundary.signals_woke == g_boundary.rounds &&
        g_boundary.signals_permit == 0 && g_boundary.errors == 0 &&
        observed.target_hits == g_boundary.rounds &&
        sem_before_cleanup.count == 0 && sem_before_cleanup.waiters == 0;
    bool noise_ok = !noise_requested ||
        (g_boundary_noise.waits_ok == g_boundary_noise.rounds &&
         g_boundary_noise.signals_woke == g_boundary_noise.rounds &&
         g_boundary_noise.signals_permit == 0 &&
         g_boundary_noise.errors == 0 && observed.foreign_hits > 0);
    bool cleanup_ok = boundary_cleanup(target_created, noise_created);
    if (!cleanup_ok && g_boundary.first_failure == BOUNDARY_FAILURE_NONE &&
        !expected_negative)
        boundary_fail(BOUNDARY_FAILURE_CLEANUP,
                      g_boundary.completed_round + 1);

#ifdef HOBBYOS_SYNC_NEGATIVE_OLD_SEM_GAP
    if (expected_negative && cleanup_ok) {
        serial_write_all("[SYNC][NEGATIVE] OLD_SEM_GAP_DETECTED prepared=0 signal=ADDED_PERMIT waiters_before=0 cleanup=PASS\n");
    } else {
        serial_write_all("[SYNC][NEGATIVE] OLD_SEM_GAP_MISSED cleanup=");
        serial_write_all(cleanup_ok ? "PASS\n" : "FAIL\n");
    }
    bool async_pass = expected_negative && cleanup_ok;
#else
    bool ok = counts_ok && noise_ok && cleanup_ok;
    bool async_pass = ok;
    if (ok) {
        sem_test_after_prepare_snapshot_t residual = {0};
        semaphore_debug_snapshot_t sem_after = {0};
        sem_test_after_prepare_snapshot(&residual);
        (void)semaphore_debug_snapshot(&g_boundary.sem, &sem_after);
        serial_write_all("[SYNC][BOUNDARY] PASS rounds="); print_u64(g_boundary.rounds);
        serial_write_all(" prepared="); print_u64(g_boundary.prepared_round);
        serial_write_all(" signaled="); print_u64(g_boundary.signaled_round);
        serial_write_all(" completed="); print_u64(g_boundary.completed_round);
        serial_write_all(" waits="); print_u64(g_boundary.waits_ok);
        serial_write_all(" signals_woke="); print_u64(g_boundary.signals_woke);
        serial_write_all(" signals_permit="); print_u64(g_boundary.signals_permit);
        serial_write_all(" errors="); print_u64(g_boundary.errors);
        serial_write_all(" target_hits="); print_u64(observed.target_hits);
        serial_write_all(" foreign_hits="); print_u64(observed.foreign_hits);
        serial_write_all(" sem_count="); print_u64((uint64_t)sem_after.count);
        serial_write_all(" sem_waiters="); print_u64(sem_after.waiters);
        serial_write_all(" elapsed_ms=");
        print_u64(timer_get_uptime_ms() - g_boundary.started_ms);
        serial_write_all(" cleanup=PASS observer_active=");
        print_u64(residual.active);
        serial_write_all(" observer_inflight=");
        print_u64(residual.callback_inflight);
        serial_write_all("\n");
        if (noise_requested) {
            serial_write_all("[SYNC][BOUNDARY_NOISE] PASS target_rounds=");
            print_u64(g_boundary.rounds);
            serial_write_all(" foreign_rounds=");
            print_u64(g_boundary_noise.rounds);
            serial_write_all(" target_hits="); print_u64(observed.target_hits);
            serial_write_all(" foreign_hits="); print_u64(observed.foreign_hits);
            serial_write_all(" foreign_waits="); print_u64(g_boundary_noise.waits_ok);
            serial_write_all(" foreign_signals="); print_u64(g_boundary_noise.signals_woke);
            serial_write_all(" foreign_errors="); print_u64(g_boundary_noise.errors);
            serial_write_all(" cleanup=PASS\n");
        }
    } else {
        boundary_emit_failure(&failure_observation,
                              &task_before_cleanup, waiter_present,
                              &sem_before_cleanup, &observed, cleanup_ok);
    }
#endif
    sync_async_finish(run, async_pass, g_boundary.completed_round,
                      g_boundary.errors,
                      g_boundary_noise.completed_round,
                      g_boundary.foreign_hook_hits);
}

static int start_boundary_ex(uint32_t rounds, uint32_t foreign_rounds)
{
    synctest_async_kind_t kind = foreign_rounds ?
        SYNCTEST_ASYNC_BOUNDARY_NOISE : SYNCTEST_ASYNC_BOUNDARY;
    uint64_t run = 0;
    if (!rounds || (foreign_rounds == UINT32_MAX) ||
        !sync_async_begin(kind, rounds, &run))
        return 1;
    memset(&g_boundary, 0, sizeof(g_boundary));
    memset(&g_boundary_noise, 0, sizeof(g_boundary_noise));
    sem_init(&g_boundary.sem, 0);
    sem_init(&g_boundary_noise.sem, 0);
    g_boundary.rounds = rounds;
    g_boundary_noise.rounds = foreign_rounds;
    if (!thread_create_named(boundary_controller, NULL,
                             "synctest-boundary-ctl")) {
        sync_async_finish(run, false, 0, 1, 0, foreign_rounds);
        return 1;
    }
    return 0;
}

static int start_boundary(uint32_t rounds)
{
    return start_boundary_ex(rounds, 0);
}

static void sleep_worker(void *arg)
{
    sync_worker_ctx_t *ctx = (sync_worker_ctx_t *)arg;
    while (!__atomic_load_n(&g_sleep.start, __ATOMIC_ACQUIRE)) schedule_voluntary();
    for (uint32_t i = 0; i < ctx->iterations; i++) {
        task_wait_result_t result = timer_sleep_interruptible(ctx->delay_ms);
        if (result == TASK_WAIT_RESULT_TIMEOUT) __atomic_add_fetch(&g_sleep.timeout_ok, 1, __ATOMIC_RELAXED);
        else if (result == TASK_WAIT_RESULT_CANCELLED) __atomic_add_fetch(&g_sleep.cancelled, 1, __ATOMIC_RELAXED);
        else __atomic_add_fetch(&g_sleep.errors, 1, __ATOMIC_RELAXED);
    }
    __atomic_sub_fetch(&g_sleep.remaining, 1, __ATOMIC_ACQ_REL);
}

static void sleep_controller(void *arg)
{
    (void)arg;
    uint64_t run = sync_async_active_run();
    static const uint64_t delays[4] = {1, 2, 10, 100};
    uint32_t created_count = 0;
    bool created = true;
    for (uint32_t i = 0; i < 32; i++) {
        g_sleep.workers[i].index = i;
        g_sleep.workers[i].iterations = 100;
        g_sleep.workers[i].delay_ms = delays[i / 8];
        if (!thread_create_named(sleep_worker, &g_sleep.workers[i], "synctest-sleep")) created = false;
        else created_count++;
    }
    __atomic_store_n(&g_sleep.remaining, created_count, __ATOMIC_RELEASE);
    __atomic_store_n(&g_sleep.start, 1, __ATOMIC_RELEASE);
    bool complete = created && sync_test_wait_until(&g_sleep.remaining, 0, SYNC_WATCHDOG_MS);
    timer_stats_t stats; timers_get_stats(&stats);
    bool ok = complete && g_sleep.timeout_ok == 3200 && g_sleep.cancelled == 0 && g_sleep.errors == 0;
    serial_write_all("[SYNC][SLEEP] "); serial_write_all(ok ? "PASS" : "FAIL");
    serial_write_all(" workers=32 iterations=100 timeout="); print_u64(g_sleep.timeout_ok);
    serial_write_all(" cancelled="); print_u64(g_sleep.cancelled);
    serial_write_all(" errors="); print_u64(g_sleep.errors);
    serial_write_all(" pending="); print_u64(stats.pending); serial_write_all("\n");
    sync_async_finish(run, ok, g_sleep.timeout_ok, g_sleep.errors,
                      g_sleep.cancelled, created_count);
}

static int start_sleep_test(void)
{
    uint64_t run = 0;
    if (!sync_async_begin(SYNCTEST_ASYNC_SLEEP, 3200, &run)) return 1;
    memset(&g_sleep, 0, sizeof(g_sleep));
    if (!thread_create_named(sleep_controller, NULL, "synctest-sleep-ctl")) {
        sync_async_finish(run, false, 0, 1, 0, 32);
        return 1;
    }
    return 0;
}

typedef struct {
    semaphore_t sem;
    volatile uint32_t done;
    volatile uint32_t result;
    task_handle_t handle;
} cancel_case_t;
static cancel_case_t g_cancel_case;

static void cancel_sem_worker(void *arg)
{
    cancel_case_t *test = (cancel_case_t *)arg;
    test->result = (uint32_t)sem_wait_interruptible(&test->sem);
    __atomic_store_n(&test->done, 1, __ATOMIC_RELEASE);
    if (test->result == TASK_WAIT_RESULT_CANCELLED)
        task_cancel_point();
}

static void cancel_sleep_worker(void *arg)
{
    cancel_case_t *test = (cancel_case_t *)arg;
    test->result = (uint32_t)timer_sleep_interruptible(60000);
    __atomic_store_n(&test->done, 1, __ATOMIC_RELEASE);
    if (test->result == TASK_WAIT_RESULT_CANCELLED)
        task_cancel_point();
}

static bool wait_task_active(task_handle_t handle,uint64_t timeout)
{
    uint64_t start = timer_get_uptime_ms();
    for(;;){task_snapshot_t s;if(scheduler_snapshot_task_by_id(handle.id,&s)&&s.lifecycle_generation==handle.lifecycle_generation&&s.wait_active)return true;
        if (timer_get_uptime_ms() - start >= timeout) return false;
        timer_sleep(1);
    }
}

static void cancel_controller(void *arg)
{
    (void)arg;
    uint64_t run = sync_async_active_run();
    memset(&g_cancel_case, 0, sizeof(g_cancel_case));
    sem_init(&g_cancel_case.sem, 0);
    bool sem_created=thread_create_named_with_class_flags_handle(cancel_sem_worker,&g_cancel_case,TASK_CLASS_NORMAL,"synctest-cancel-sem",TASK_FLAG_SYSTEM|TASK_FLAG_KILLABLE,&g_cancel_case.handle);
    bool sem_active=sem_created&&wait_task_active(g_cancel_case.handle,5000);
    if(sem_active){scheduler_request_kill(g_cancel_case.handle.id);scheduler_request_kill(g_cancel_case.handle.id);}
    bool sem_done = sync_test_wait_until(&g_cancel_case.done, 1, 5000);
    bool sem_ok = sem_done && g_cancel_case.result == TASK_WAIT_RESULT_CANCELLED;
    serial_write_all(sem_ok ? "[SYNC][CANCEL][SEM] PASS result=CANCELLED\n" : "[SYNC][CANCEL][SEM] FAIL\n");

    memset(&g_cancel_case, 0, sizeof(g_cancel_case));
    bool sleep_created=thread_create_named_with_class_flags_handle(cancel_sleep_worker,&g_cancel_case,TASK_CLASS_NORMAL,"synctest-cancel-sleep",TASK_FLAG_SYSTEM|TASK_FLAG_KILLABLE,&g_cancel_case.handle);
    bool sleep_active=sleep_created&&wait_task_active(g_cancel_case.handle,5000);
    if(sleep_active){scheduler_request_kill(g_cancel_case.handle.id);scheduler_request_kill(g_cancel_case.handle.id);}
    bool sleep_done = sync_test_wait_until(&g_cancel_case.done, 1, 5000);
    bool sleep_ok = sleep_done && g_cancel_case.result == TASK_WAIT_RESULT_CANCELLED;
    serial_write_all(sleep_ok ? "[SYNC][CANCEL][SLEEP] PASS result=CANCELLED\n" : "[SYNC][CANCEL][SLEEP] FAIL\n");
    bool ok = sem_ok && sleep_ok;
    sync_async_finish(run, ok, (uint64_t)sem_ok + (uint64_t)sleep_ok,
                      ok ? 0 : 1, sem_ok, sleep_ok);
}

static int start_cancel_test(void)
{
    uint64_t run = 0;
    if (!sync_async_begin(SYNCTEST_ASYNC_CANCEL, 2, &run)) return 1;
    if (!thread_create_named(cancel_controller, NULL, "synctest-cancel-ctl")) {
        sync_async_finish(run, false, 0, 1, 0, 2);
        return 1;
    }
    return 0;
}

typedef struct {
    volatile uint32_t running, done, killer_done;
    volatile uint64_t wait_generation_seen, timeout_count, cancel_count, errors;
    uint32_t rounds;
    task_handle_t handle;
} sync_race_test_t;
static sync_race_test_t g_race;

static void race_sleep_worker(void *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < g_race.rounds; i++) {
        task_wait_result_t result = timer_sleep_interruptible(2);
        if (result == TASK_WAIT_RESULT_TIMEOUT) __atomic_add_fetch(&g_race.timeout_count, 1, __ATOMIC_RELAXED);
        else if (result == TASK_WAIT_RESULT_CANCELLED) __atomic_add_fetch(&g_race.cancel_count, 1, __ATOMIC_RELAXED);
        else __atomic_add_fetch(&g_race.errors, 1, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&g_race.done, 1, __ATOMIC_RELEASE);
}

static void race_killer_worker(void *arg)
{
    (void)arg;
    uint64_t observed = 0;
    for (uint32_t i = 0; i < g_race.rounds; i++) {
        while (!g_race.done) {
            task_snapshot_t s={0};bool found=scheduler_snapshot_task_by_id(g_race.handle.id,&s)&&s.lifecycle_generation==g_race.handle.lifecycle_generation;
            uint64_t generation=found?s.wait_generation:0;uint8_t active=found?s.wait_active:0;
            if (active && generation != observed) { observed = generation; break; }
            schedule_voluntary();
        }
        if (g_race.done) break;
        schedule_voluntary();
        task_snapshot_t s={0};if(!scheduler_snapshot_task_by_id(g_race.handle.id,&s))break;
        task_id_t id=g_race.handle.id;uint64_t lifecycle=g_race.handle.lifecycle_generation;uint64_t generation=s.wait_generation;
        scheduler_wake_task_by_identity(id, lifecycle, generation,
                                        TASK_WAKE_CANCELLED);
    }
    __atomic_store_n(&g_race.killer_done, 1, __ATOMIC_RELEASE);
}

static void race_controller(void *arg)
{
    (void)arg;
    uint64_t run = sync_async_active_run();
    bool created=thread_create_named_handle(race_sleep_worker,NULL,"synctest-race-sleep",&g_race.handle)&&thread_create_named(race_killer_worker,NULL,"synctest-race-kill");
    bool complete = created && sync_test_wait_until(&g_race.done, 1, SYNC_WATCHDOG_MS);
    if (complete) sync_test_wait_until(&g_race.killer_done, 1, 5000);
    uint64_t winners = g_race.timeout_count + g_race.cancel_count;
    bool ok = complete && winners == g_race.rounds && g_race.errors == 0;
    serial_write_all("[SYNC][RACE] "); serial_write_all(ok ? "PASS" : "FAIL");
    serial_write_all(" rounds="); print_u64(g_race.rounds);
    serial_write_all(" timeout="); print_u64(g_race.timeout_count);
    serial_write_all(" cancelled="); print_u64(g_race.cancel_count);
    serial_write_all(" double=0 stuck="); print_u64(complete ? 0 : 1); serial_write_all("\n");
    sync_async_finish(run, ok, winners, g_race.errors,
                      g_race.timeout_count, g_race.cancel_count);
}

static int start_race(uint32_t rounds)
{
    uint64_t run = 0;
    if (!rounds || !sync_async_begin(SYNCTEST_ASYNC_RACE, rounds, &run))
        return 1;
    memset(&g_race, 0, sizeof(g_race)); g_race.rounds = rounds;
    if (!thread_create_named(race_controller, NULL, "synctest-race-ctl")) {
        sync_async_finish(run, false, 0, 1, 0, rounds);
        return 1;
    }
    return 0;
}

static bool timer_test_wait_count(volatile uint32_t *value, uint32_t expected,
                                  uint64_t timeout_ms)
{
    uint64_t start = timer_get_uptime_ms();
    while (__atomic_load_n(value, __ATOMIC_ACQUIRE) < expected) {
        if (timer_get_uptime_ms() - start >= timeout_ms)
            return false;
        schedule_voluntary();
    }
    return true;
}

static void timer_cancel_callback(void *ctx)
{
    timer_test_callback_ticket_t *ticket =
        (timer_test_callback_ticket_t *)ctx;
    if (!ticket ||
        !__atomic_exchange_n(&ticket->active, 0, __ATOMIC_ACQ_REL)) {
        __atomic_add_fetch(&g_timer_cancel_test.late_callbacks, 1,
                           __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_timer_cancel_test.errors, 1,
                           __ATOMIC_RELAXED);
        return;
    }
    uint64_t generation = __atomic_load_n(&ticket->generation,
                                           __ATOMIC_ACQUIRE);
    if (!__atomic_load_n(&g_timer_cancel_test.run_active, __ATOMIC_ACQUIRE) ||
        generation != __atomic_load_n(
            &g_timer_cancel_test.expected_generation, __ATOMIC_ACQUIRE) ||
        ticket->role != TIMER_TEST_CALLBACK_CANCEL_CLAIMED) {
        __atomic_add_fetch(&g_timer_cancel_test.late_callbacks, 1,
                           __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_timer_cancel_test.errors, 1,
                           __ATOMIC_RELAXED);
        return;
    }
    __atomic_add_fetch(&g_timer_cancel_test.callbacks, 1,
                       __ATOMIC_RELEASE);
}

static bool timer_cancel_test_prepare(uint64_t *out_generation)
{
    if (__atomic_load_n(&g_timer_cancel_test.run_active, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&g_timer_cancel_test.pending.active, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&g_timer_cancel_test.claimed.active, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&g_timer_cancel_test.late_callbacks, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&g_timer_cancel_test.errors, __ATOMIC_ACQUIRE))
        return false;
    uint64_t generation = __atomic_load_n(&g_timer_cancel_test.generation,
                                           __ATOMIC_ACQUIRE) + 1u;
    if (!generation)
        return false;
    __atomic_store_n(&g_timer_cancel_test.generation, generation,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_timer_cancel_test.expected_generation, generation,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_timer_cancel_test.callbacks, 0, __ATOMIC_RELEASE);
    g_timer_cancel_test.pending.role = TIMER_TEST_CALLBACK_CANCEL_PENDING;
    g_timer_cancel_test.pending.index = 0;
    __atomic_store_n(&g_timer_cancel_test.pending.generation, generation,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_timer_cancel_test.pending.active, 1,
                     __ATOMIC_RELEASE);
    g_timer_cancel_test.claimed.role = TIMER_TEST_CALLBACK_CANCEL_CLAIMED;
    g_timer_cancel_test.claimed.index = 1;
    __atomic_store_n(&g_timer_cancel_test.claimed.generation, generation,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_timer_cancel_test.claimed.active, 1,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_timer_cancel_test.run_active, 1, __ATOMIC_RELEASE);
    if (out_generation) *out_generation = generation;
    return true;
}

static void timer_cancel_controller(void *arg)
{
    (void)arg;
    uint64_t run = sync_async_active_run();
    uint64_t generation = 0;
    if (!timer_cancel_test_prepare(&generation)) {
        serial_write_all("[SYNC][TIMER_CANCEL] FAIL reason=residual-test-state\n");
        sync_async_finish(run, false, 0, 1, 0, 0);
        return;
    }

    timer_handle_t pending = {0};
    bool armed = timers_add_callback(60000, timer_cancel_callback,
                                     &g_timer_cancel_test.pending, &pending);
    if (!armed)
        __atomic_store_n(&g_timer_cancel_test.pending.active, 0,
                         __ATOMIC_RELEASE);
    timer_cancel_result_t first = armed ? timers_cancel(pending) : TIMER_CANCEL_INVALID;
    if (first == TIMER_CANCELLED)
        __atomic_store_n(&g_timer_cancel_test.pending.active, 0,
                         __ATOMIC_RELEASE);
    timer_cancel_result_t duplicate = timers_cancel(pending);

    timer_handle_t claimed = {0};
    timers_test_hold_claimed(true);
    bool claim_armed = timers_add_callback(1, timer_cancel_callback,
                                           &g_timer_cancel_test.claimed,
                                           &claimed);
    if (!claim_armed)
        __atomic_store_n(&g_timer_cancel_test.claimed.active, 0,
                         __ATOMIC_RELEASE);
    uint64_t start = timer_get_uptime_ms();
    timer_node_state_t claimed_state = TIMER_NODE_PENDING;
    bool reached_claimed = false;
    while (claim_armed && timer_get_uptime_ms() - start < 5000u) {
        if (timers_test_handle_present(claimed, &claimed_state) &&
            claimed_state == TIMER_NODE_CLAIMED) {
            reached_claimed = true;
            break;
        }
        schedule_voluntary();
    }
    timer_stats_t before_release;
    timers_get_stats(&before_release);
    timer_cancel_result_t claim_result = reached_claimed
        ? timers_cancel(claimed) : TIMER_CANCEL_INVALID;
    timers_test_hold_claimed(false);
    if (claim_armed)
        timer_sleep(10);
    bool callback_complete = timer_test_wait_count(
        &g_timer_cancel_test.callbacks, 1, 5000);
    timer_stats_t after;
    timers_get_stats(&after);
    bool residual = claim_armed && timers_test_handle_present(claimed, NULL);
    uint32_t callbacks = __atomic_load_n(&g_timer_cancel_test.callbacks,
                                         __ATOMIC_ACQUIRE);
    uint32_t late = __atomic_load_n(&g_timer_cancel_test.late_callbacks,
                                    __ATOMIC_ACQUIRE);
    uint32_t errors = __atomic_load_n(&g_timer_cancel_test.errors,
                                      __ATOMIC_ACQUIRE);
    bool ticket_active = __atomic_load_n(&g_timer_cancel_test.claimed.active,
                                         __ATOMIC_ACQUIRE) != 0;
    __atomic_store_n(&g_timer_cancel_test.run_active, 0, __ATOMIC_RELEASE);
    bool ok = generation && armed && first == TIMER_CANCELLED &&
              duplicate == TIMER_CANCEL_NOT_FOUND && claim_armed &&
              reached_claimed &&
              claim_result == TIMER_CANCEL_ALREADY_CLAIMED &&
              callback_complete && callbacks == 1 && !late && !errors &&
              !residual && !ticket_active;
    serial_write_all("[SYNC][TIMER_CANCEL] "); serial_write_all(ok ? "PASS" : "FAIL");
    if (!ok) {
        if (late) serial_write_all(" reason=late-callback");
        else if (!reached_claimed) serial_write_all(" reason=claim-timeout");
        else if (!callback_complete) serial_write_all(" reason=callback-timeout");
        else if (residual) serial_write_all(" reason=claimed-residual");
        else serial_write_all(" reason=contract");
    }
    serial_write_all(" pending_cancel="); print_u64(first);
    serial_write_all(" duplicate_cancel="); print_u64(duplicate);
    serial_write_all(" claimed_cancel="); print_u64(claim_result);
    serial_write_all(" callbacks="); print_u64(callbacks);
    serial_write_all(" claimed_before_release=");
    print_u64(before_release.claimed_now);
    serial_write_all(" claimed_after="); print_u64(after.claimed_now);
    serial_write_all(" residual="); print_u64(residual);
    serial_write_all(" late="); print_u64(late);
    serial_write_all(" errors="); print_u64(errors);
    serial_write_all(" generation="); print_u64(generation);
    serial_write_all("\n");
    sync_async_finish(run, ok, ok ? 3 : 0, ok ? 0 : 1,
                      first, claim_result);
}

static int start_timer_cancel(void)
{
    uint64_t run = 0;
    if (!sync_async_begin(SYNCTEST_ASYNC_TIMER_CANCEL, 3, &run)) return 1;
    if (!thread_create_named(timer_cancel_controller, NULL,
                             "synctest-timer-cancel")) {
        sync_async_finish(run, false, 0, 1, 0, 3);
        return 1;
    }
    return 0;
}

static bool timer_direct_test_begin(void)
{
    return __atomic_exchange_n(&g_any_test_running, 1,
                               __ATOMIC_ACQ_REL) == 0;
}

static void timer_direct_test_finish(void)
{
    __atomic_store_n(&g_any_test_running, 0, __ATOMIC_RELEASE);
}

static void timer_order_callback(void *ctx)
{
    timer_test_callback_ticket_t *ticket =
        (timer_test_callback_ticket_t *)ctx;
    if (!ticket ||
        !__atomic_exchange_n(&ticket->active, 0, __ATOMIC_ACQ_REL)) {
        __atomic_add_fetch(&g_timer_order_test.late_callbacks, 1,
                           __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_timer_order_test.errors, 1,
                           __ATOMIC_RELAXED);
        return;
    }
    uint64_t generation = __atomic_load_n(&ticket->generation,
                                           __ATOMIC_ACQUIRE);
    if (!__atomic_load_n(&g_timer_order_test.run_active, __ATOMIC_ACQUIRE) ||
        generation != __atomic_load_n(
            &g_timer_order_test.expected_generation, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&g_timer_order_test.late_callbacks, 1,
                           __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_timer_order_test.errors, 1,
                           __ATOMIC_RELAXED);
        return;
    }
    uint32_t position = __atomic_add_fetch(&g_timer_order_test.sequence, 1,
                                            __ATOMIC_ACQ_REL);
    if (ticket->role == TIMER_TEST_CALLBACK_ORDER_OLD)
        __atomic_store_n(&g_timer_order_test.old_position, position,
                         __ATOMIC_RELEASE);
    else if (ticket->role == TIMER_TEST_CALLBACK_ORDER_NEW)
        __atomic_store_n(&g_timer_order_test.new_position, position,
                         __ATOMIC_RELEASE);
    else
        __atomic_add_fetch(&g_timer_order_test.errors, 1,
                           __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_timer_order_test.callbacks, 1, __ATOMIC_RELEASE);
}

static int run_timer_order(void)
{
    if (!timer_direct_test_begin()) {
        serial_write_all("[SYNC][TIMER_ORDER] FAIL reason=busy\n");
        return 1;
    }
    bool clean = !__atomic_load_n(&g_timer_order_test.run_active,
                                   __ATOMIC_ACQUIRE) &&
                 !__atomic_load_n(&g_timer_order_test.old_ticket.active,
                                   __ATOMIC_ACQUIRE) &&
                 !__atomic_load_n(&g_timer_order_test.new_ticket.active,
                                   __ATOMIC_ACQUIRE) &&
                 !__atomic_load_n(&g_timer_order_test.late_callbacks,
                                   __ATOMIC_ACQUIRE) &&
                 !__atomic_load_n(&g_timer_order_test.errors,
                                   __ATOMIC_ACQUIRE);
    uint64_t generation = __atomic_load_n(&g_timer_order_test.generation,
                                           __ATOMIC_ACQUIRE) + 1u;
    if (!clean || !generation) {
        serial_write_all("[SYNC][TIMER_ORDER] FAIL reason=residual-test-state\n");
        timer_direct_test_finish();
        return 1;
    }
    __atomic_store_n(&g_timer_order_test.generation, generation,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_timer_order_test.expected_generation, generation,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_timer_order_test.callbacks, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_timer_order_test.sequence, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_timer_order_test.old_position, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_timer_order_test.new_position, 0, __ATOMIC_RELEASE);
    g_timer_order_test.old_ticket.role = TIMER_TEST_CALLBACK_ORDER_OLD;
    g_timer_order_test.old_ticket.index = 0;
    __atomic_store_n(&g_timer_order_test.old_ticket.generation, generation,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_timer_order_test.old_ticket.active, 1,
                     __ATOMIC_RELEASE);
    g_timer_order_test.new_ticket.role = TIMER_TEST_CALLBACK_ORDER_NEW;
    g_timer_order_test.new_ticket.index = 1;
    __atomic_store_n(&g_timer_order_test.new_ticket.generation, generation,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_timer_order_test.new_ticket.active, 1,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_timer_order_test.run_active, 1, __ATOMIC_RELEASE);

    uint64_t hook_generation = 0;
    bool armed = timers_test_arm_order_pair(
        timer_order_callback, &g_timer_order_test.old_ticket,
        &g_timer_order_test.new_ticket, &hook_generation);
    bool completed = armed && timer_test_wait_count(
        &g_timer_order_test.callbacks, 2, 5000);
    timers_test_order_snapshot_t hook = {0};
    bool finished = armed && timers_test_order_finish(hook_generation, &hook);
    __atomic_store_n(&g_timer_order_test.run_active, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_timer_order_test.old_ticket.active, 0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_timer_order_test.new_ticket.active, 0,
                     __ATOMIC_RELEASE);
    uint32_t callbacks = __atomic_load_n(&g_timer_order_test.callbacks,
                                         __ATOMIC_ACQUIRE);
    uint32_t old_position = __atomic_load_n(
        &g_timer_order_test.old_position, __ATOMIC_ACQUIRE);
    uint32_t new_position = __atomic_load_n(
        &g_timer_order_test.new_position, __ATOMIC_ACQUIRE);
    uint32_t late = __atomic_load_n(&g_timer_order_test.late_callbacks,
                                    __ATOMIC_ACQUIRE);
    uint32_t errors = __atomic_load_n(&g_timer_order_test.errors,
                                      __ATOMIC_ACQUIRE);
#ifdef HOBBYOS_TIMER_NEGATIVE_NEW_DUE_BYPASS
    bool bypass_detected = armed && completed && finished && callbacks == 2 &&
                           new_position == 1 && old_position == 2 &&
                           hook.order_violation && !hook.remaining &&
                           !hook.residual && !late && !errors;
    serial_write_all(bypass_detected
        ? "[TIMER][NEGATIVE] CLAIMED_ORDER_BYPASS_DETECTED\n"
        : "[TIMER][NEGATIVE] CLAIMED_ORDER_BYPASS_MISSED\n");
    bool ok = bypass_detected;
#else
    bool ok = armed && completed && finished && callbacks == 2 &&
              old_position == 1 && new_position == 2 &&
              hook.old_selected && hook.new_selected &&
              !hook.order_violation && !hook.remaining &&
              !hook.residual && !late && !errors;
#endif
    serial_write_all("[SYNC][TIMER_ORDER] ");
    serial_write_all(ok ? "PASS" : "FAIL");
    serial_write_all(" old_position="); print_u64(old_position);
    serial_write_all(" new_position="); print_u64(new_position);
    serial_write_all(" callbacks="); print_u64(callbacks);
    serial_write_all(" hook_violation="); print_u64(hook.order_violation);
    serial_write_all(" residual="); print_u64(hook.residual);
    serial_write_all(" late="); print_u64(late);
    serial_write_all(" errors="); print_u64(errors);
    serial_write_all("\n");
    timer_direct_test_finish();
    return ok ? 0 : 1;
}

static void timer_backlog_callback(void *ctx)
{
    timer_test_callback_ticket_t *ticket =
        (timer_test_callback_ticket_t *)ctx;
    if (!ticket ||
        !__atomic_exchange_n(&ticket->active, 0, __ATOMIC_ACQ_REL)) {
        __atomic_add_fetch(&g_timer_backlog_test.late_callbacks, 1,
                           __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_timer_backlog_test.errors, 1,
                           __ATOMIC_RELAXED);
        return;
    }
    uint64_t generation = __atomic_load_n(&ticket->generation,
                                           __ATOMIC_ACQUIRE);
    if (!__atomic_load_n(&g_timer_backlog_test.run_active, __ATOMIC_ACQUIRE) ||
        generation != __atomic_load_n(
            &g_timer_backlog_test.expected_generation, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&g_timer_backlog_test.late_callbacks, 1,
                           __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_timer_backlog_test.errors, 1,
                           __ATOMIC_RELAXED);
        return;
    }
    if (ticket->role == TIMER_TEST_CALLBACK_BACKLOG_OLD) {
        __atomic_add_fetch(&g_timer_backlog_test.old_completed, 1,
                           __ATOMIC_ACQ_REL);
    } else if (ticket->role == TIMER_TEST_CALLBACK_BACKLOG_NEW) {
        if (__atomic_load_n(&g_timer_backlog_test.old_completed,
                            __ATOMIC_ACQUIRE) < TIMER_BACKLOG_OLD_COUNT)
            __atomic_add_fetch(&g_timer_backlog_test.order_violations, 1,
                               __ATOMIC_RELAXED);
    } else {
        __atomic_add_fetch(&g_timer_backlog_test.errors, 1,
                           __ATOMIC_RELAXED);
    }
    __atomic_add_fetch(&g_timer_backlog_test.callbacks, 1, __ATOMIC_RELEASE);
}

static bool timer_backlog_old_claimed(uint32_t count)
{
    for (uint32_t index = 0; index < count; index++) {
        timer_node_state_t state = TIMER_NODE_PENDING;
        if (!timers_test_handle_present(g_timer_backlog_test.handles[index],
                                        &state) ||
            state != TIMER_NODE_CLAIMED)
            return false;
    }
    return true;
}

static int run_timer_backlog(void)
{
    if (!timer_direct_test_begin()) {
        serial_write_all("[SYNC][TIMER_BACKLOG] FAIL reason=busy\n");
        return 1;
    }
    bool clean = !__atomic_load_n(&g_timer_backlog_test.run_active,
                                   __ATOMIC_ACQUIRE) &&
                 !__atomic_load_n(&g_timer_backlog_test.late_callbacks,
                                   __ATOMIC_ACQUIRE) &&
                 !__atomic_load_n(&g_timer_backlog_test.errors,
                                   __ATOMIC_ACQUIRE);
    for (uint32_t index = 0; index < TIMER_BACKLOG_COUNT; index++)
        clean = clean && !__atomic_load_n(
            &g_timer_backlog_test.tickets[index].active, __ATOMIC_ACQUIRE);
    uint64_t generation = __atomic_load_n(&g_timer_backlog_test.generation,
                                           __ATOMIC_ACQUIRE) + 1u;
    if (!clean || !generation) {
        serial_write_all("[SYNC][TIMER_BACKLOG] FAIL reason=residual-test-state\n");
        timer_direct_test_finish();
        return 1;
    }
    __atomic_store_n(&g_timer_backlog_test.generation, generation,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_timer_backlog_test.expected_generation, generation,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_timer_backlog_test.callbacks, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_timer_backlog_test.old_completed, 0,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_timer_backlog_test.order_violations, 0,
                     __ATOMIC_RELEASE);
    memset(g_timer_backlog_test.handles, 0,
           sizeof(g_timer_backlog_test.handles));
    __atomic_store_n(&g_timer_backlog_test.run_active, 1, __ATOMIC_RELEASE);

    timer_stats_t before;
    timers_get_stats(&before);
    timers_test_hold_claimed(true);
    uint32_t armed = 0;
    for (uint32_t index = 0; index < TIMER_BACKLOG_OLD_COUNT; index++) {
        timer_test_callback_ticket_t *ticket =
            &g_timer_backlog_test.tickets[index];
        ticket->role = TIMER_TEST_CALLBACK_BACKLOG_OLD;
        ticket->index = index;
        __atomic_store_n(&ticket->generation, generation, __ATOMIC_RELEASE);
        __atomic_store_n(&ticket->active, 1, __ATOMIC_RELEASE);
        if (!timers_add_callback(0, timer_backlog_callback, ticket,
                                 &g_timer_backlog_test.handles[index])) {
            __atomic_store_n(&ticket->active, 0, __ATOMIC_RELEASE);
            break;
        }
        armed++;
    }
    uint64_t start = timer_get_uptime_ms();
    bool old_claimed = false;
    while (armed == TIMER_BACKLOG_OLD_COUNT &&
           timer_get_uptime_ms() - start < 5000u) {
        if (timer_backlog_old_claimed(TIMER_BACKLOG_OLD_COUNT)) {
            old_claimed = true;
            break;
        }
        schedule_voluntary();
    }
    timer_stats_t claimed_snapshot;
    timers_get_stats(&claimed_snapshot);
    if (old_claimed) {
        for (uint32_t index = TIMER_BACKLOG_OLD_COUNT;
             index < TIMER_BACKLOG_COUNT; index++) {
            timer_test_callback_ticket_t *ticket =
                &g_timer_backlog_test.tickets[index];
            ticket->role = TIMER_TEST_CALLBACK_BACKLOG_NEW;
            ticket->index = index;
            __atomic_store_n(&ticket->generation, generation,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&ticket->active, 1, __ATOMIC_RELEASE);
            if (!timers_add_callback(0, timer_backlog_callback, ticket,
                                     &g_timer_backlog_test.handles[index])) {
                __atomic_store_n(&ticket->active, 0, __ATOMIC_RELEASE);
                break;
            }
            armed++;
        }
    }
    timers_test_hold_claimed(false);
    bool completed = timer_test_wait_count(&g_timer_backlog_test.callbacks,
                                           armed, 10000);
    __atomic_store_n(&g_timer_backlog_test.run_active, 0, __ATOMIC_RELEASE);

    uint32_t residual = 0;
    uint32_t active_tickets = 0;
    for (uint32_t index = 0; index < armed; index++) {
        if (timers_test_handle_present(g_timer_backlog_test.handles[index],
                                       NULL))
            residual++;
        if (__atomic_load_n(&g_timer_backlog_test.tickets[index].active,
                            __ATOMIC_ACQUIRE))
            active_tickets++;
    }
    timer_stats_t after;
    timers_get_stats(&after);
    uint32_t callbacks = __atomic_load_n(&g_timer_backlog_test.callbacks,
                                         __ATOMIC_ACQUIRE);
    uint32_t violations = __atomic_load_n(
        &g_timer_backlog_test.order_violations, __ATOMIC_ACQUIRE);
    uint32_t late = __atomic_load_n(&g_timer_backlog_test.late_callbacks,
                                    __ATOMIC_ACQUIRE);
    uint32_t errors = __atomic_load_n(&g_timer_backlog_test.errors,
                                      __ATOMIC_ACQUIRE);
    bool ok = armed == TIMER_BACKLOG_COUNT && old_claimed && completed &&
              callbacks == TIMER_BACKLOG_COUNT && !violations && !residual &&
              !active_tickets && !late && !errors;
    serial_write_all("[SYNC][TIMER_BACKLOG] ");
    serial_write_all(ok ? "PASS" : "FAIL");
    serial_write_all(" old="); print_u64(TIMER_BACKLOG_OLD_COUNT);
    serial_write_all(" new="); print_u64(TIMER_BACKLOG_NEW_COUNT);
    serial_write_all(" callbacks="); print_u64(callbacks);
    serial_write_all(" order_violations="); print_u64(violations);
    serial_write_all(" residual="); print_u64(residual);
    serial_write_all(" active_tickets="); print_u64(active_tickets);
    serial_write_all(" late="); print_u64(late);
    serial_write_all(" errors="); print_u64(errors);
    serial_write_all(" claimed_before="); print_u64(before.claimed_now);
    serial_write_all(" claimed_ready=");
    print_u64(claimed_snapshot.claimed_now);
    serial_write_all(" claimed_after="); print_u64(after.claimed_now);
    serial_write_all("\n");
    timer_direct_test_finish();
    return ok ? 0 : 1;
}

static int run_check(void)
{
    scheduler_runtime_stats_t scheduler;
    timer_stats_t timers;
    uint32_t cancel_active =
        __atomic_load_n(&g_timer_cancel_test.run_active, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&g_timer_cancel_test.pending.active, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&g_timer_cancel_test.claimed.active, __ATOMIC_ACQUIRE);
    uint32_t order_active =
        __atomic_load_n(&g_timer_order_test.run_active, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&g_timer_order_test.old_ticket.active,
                        __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&g_timer_order_test.new_ticket.active,
                        __ATOMIC_ACQUIRE);
    uint32_t backlog_active = __atomic_load_n(
        &g_timer_backlog_test.run_active, __ATOMIC_ACQUIRE);
    for (uint32_t index = 0; index < TIMER_BACKLOG_COUNT; index++)
        backlog_active = backlog_active || __atomic_load_n(
            &g_timer_backlog_test.tickets[index].active, __ATOMIC_ACQUIRE);
    uint64_t late = __atomic_load_n(&g_timer_cancel_test.late_callbacks,
                                    __ATOMIC_ACQUIRE) +
                    __atomic_load_n(&g_timer_order_test.late_callbacks,
                                    __ATOMIC_ACQUIRE) +
                    __atomic_load_n(&g_timer_backlog_test.late_callbacks,
                                    __ATOMIC_ACQUIRE);
    uint64_t test_errors = __atomic_load_n(&g_timer_cancel_test.errors,
                                           __ATOMIC_ACQUIRE) +
                           __atomic_load_n(&g_timer_order_test.errors,
                                           __ATOMIC_ACQUIRE) +
                           __atomic_load_n(&g_timer_backlog_test.errors,
                                           __ATOMIC_ACQUIRE);
    bool test_clean = !__atomic_load_n(&g_any_test_running,
                                        __ATOMIC_ACQUIRE) &&
                      !cancel_active && !order_active && !backlog_active &&
                      !late && !test_errors;
    timers_get_stats(&timers);
    bool ok = scheduler_validate_runtime_invariants(&scheduler) &&
              timers_validate() && test_clean && timers.claimed_now == 0;
    serial_write_all("[SYNC][CHECK] "); serial_write_all(ok ? "PASS" : "FAIL");
    serial_write_all(" waits_active=0 timers_pending="); print_u64(timers.pending);
    serial_write_all(" timers_claimed="); print_u64(timers.claimed_now);
    serial_write_all(" test_active=");
    print_u64(cancel_active || order_active || backlog_active);
    serial_write_all(" late="); print_u64(late);
    serial_write_all(" timer_errors="); print_u64(test_errors);
    serial_write_all(" violations="); print_u64(scheduler.violations);
    serial_write_all("\n");
    return ok ? 0 : 1;
}

static int run_oom(void)
{
    task_t *task = get_current_task();
    timer_stats_t before, after;
    timers_get_stats(&before);
    timers_test_fail_next_allocation();
    task_wait_result_t result = timer_sleep_interruptible(10);
    timers_get_stats(&after);
    bool clean = result == TASK_WAIT_RESULT_ERROR && task && task->state == TASK_RUNNING &&
                 !task->wait_active && task->wait_kind == TASK_WAIT_NONE &&
                 task->wake_reason == TASK_WAKE_NONE && before.pending == after.pending;
    serial_write_all(clean ? "[SYNC][OOM] PASS state_clean=1 timers_clean=1\n" : "[SYNC][OOM] FAIL\n");
    return clean ? 0 : 1;
}

static int run_fallback(void)
{
    irq_flags_t before;
    __asm__ volatile("pushfq; pop %0" : "=r"(before));
    bool on = timers_test_fallback_delay(2, true) == TASK_WAIT_RESULT_OK;
    irq_flags_t disabled = irq_save();
    bool off = timers_test_fallback_delay(1, false) == TASK_WAIT_RESULT_OK;
    irq_flags_t during;
    __asm__ volatile("pushfq; pop %0" : "=r"(during));
    irq_restore(disabled);
    irq_flags_t after;
    __asm__ volatile("pushfq; pop %0" : "=r"(after));
    bool ok = on && off && !(during & (1ULL << 9)) && ((before ^ after) & (1ULL << 9)) == 0;
    serial_write_all(ok ? "[SYNC][FALLBACK] PASS if_on=1 if_off=1 idle=1\n" : "[SYNC][FALLBACK] FAIL\n");
    return ok ? 0 : 1;
}

static int run_stale(void)
{
    cancel_case_t *test = &g_cancel_case;
    memset(test, 0, sizeof(*test));
    if(!thread_create_named_with_class_flags_handle(cancel_sleep_worker,test,TASK_CLASS_NORMAL,"synctest-stale",TASK_FLAG_SYSTEM|TASK_FLAG_KILLABLE,&test->handle)||!wait_task_active(test->handle,5000))return 1;
    task_snapshot_t s;if(!scheduler_snapshot_task_by_id(test->handle.id,&s))return 1;
    task_id_t id=test->handle.id;uint64_t lifecycle=test->handle.lifecycle_generation;uint64_t wait_generation=s.wait_generation;
    scheduler_request_kill(id);
    if (!sync_test_wait_until(&test->done, 1, 5000)) return 1;
    timer_sleep(5);
    scheduler_reap_zombies(0);
    int status = timers_test_dispatch_task_wake(id, lifecycle, wait_generation);
    bool ok = status == SCHED_WAKE_NO_MATCH;
    serial_write_all("[SYNC][STALE] "); serial_write_all(ok ? "PASS" : "FAIL");
    serial_write_all(" reaped_id="); print_u64(id); serial_write_all(" stale_noop="); print_u64(ok); serial_write_all("\n");
    return ok ? 0 : 1;
}

static void print_stats(void)
{
    timer_stats_t stats; timers_get_stats(&stats);
    serial_write_all("[SYNC][STATS] armed="); print_u64(stats.armed);
    serial_write_all(" cancelled="); print_u64(stats.cancelled);
    serial_write_all(" claimed="); print_u64(stats.claimed);
    serial_write_all(" dispatched="); print_u64(stats.dispatched);
    serial_write_all(" pending="); print_u64(stats.pending);
    serial_write_all(" claimed_now="); print_u64(stats.claimed_now);
    serial_write_all(" alloc_failures="); print_u64(stats.alloc_failures);
    serial_write_all(" stale="); print_u64(stats.stale_task_wakes); serial_write_all("\n");
}

int cmd_synctest(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "check";
    if (strcmp(sub, "async-status") == 0) {
        uint64_t run = 0;
        if (argc > 3 || (argc == 3 && !parse_exact_u64(argv[2], &run)))
            return 1;
        return run_async_status(run, argc == 3);
    }
    if (strcmp(sub, "async-wait") == 0) {
        uint64_t run = 0;
        uint64_t timeout_ms = 300000;
        if (argc < 3 || argc > 4 || !parse_exact_u64(argv[2], &run) ||
            !run || (argc == 4 && !parse_exact_u64(argv[3], &timeout_ms)) ||
            timeout_ms < 1 || timeout_ms > 3600000)
            return 1;
        return run_async_wait(run, timeout_ms);
    }
    if (strcmp(sub, "check") == 0) return run_check();
    if (strcmp(sub, "sem") == 0) {
        uint32_t iterations = argc > 2 ? parse_u32(argv[2], 100000) : 100000;
        uint32_t waiters = argc > 3 ? parse_u32(argv[3], 4) : 4;
        uint32_t signalers = argc > 4 ? parse_u32(argv[4], 4) : 4;
        return start_sem_test(iterations, waiters, signalers);
    }
    if (strcmp(sub, "boundary") == 0) return start_boundary(argc > 2 ? parse_u32(argv[2], 10000) : 10000);
    if (strcmp(sub, "boundary-noise") == 0) return start_boundary_ex(
        argc > 2 ? parse_u32(argv[2], 5000) : 5000,
        argc > 3 ? parse_u32(argv[3], 5000) : 5000);
    if (strcmp(sub, "sleep") == 0) return start_sleep_test();
    if (strcmp(sub, "cancel") == 0) return start_cancel_test();
    if ((strcmp(sub, "timer-cancel") == 0 || strcmp(sub, "timer_cancel") == 0 || strcmp(sub, "timercancel") == 0)) return start_timer_cancel();
    if (strcmp(sub, "timer-order") == 0) return run_timer_order();
    if (strcmp(sub, "timer-backlog") == 0) return run_timer_backlog();
    if (strcmp(sub, "oom") == 0) return run_oom();
    if (strcmp(sub, "fallback") == 0) return run_fallback();
    if (strcmp(sub, "stale") == 0) return run_stale();
    if (strcmp(sub, "preblock-sem") == 0) return run_preblock_case(false, false);
    if (strcmp(sub, "preblock-timer") == 0) return run_preblock_case(true, false);
    if (strcmp(sub, "preblock-negative") == 0) return run_preblock_case(false, false);
    if (strcmp(sub, "preblock-loop") == 0) return run_preblock_loop(
        argc > 2 ? parse_u32(argv[2], 1000) : 1000, argc > 3 ? argv[3] : "all");
    if (strcmp(sub, "preblock-timer-noise") == 0)
        return run_preblock_timer_noise(
            argc > 2 ? parse_u32(argv[2], 1000) : 1000);
    if (strcmp(sub, "race") == 0) return start_race(argc > 2 ? parse_u32(argv[2], 5000) : 5000);
    if (strcmp(sub, "stats") == 0) { print_stats(); return 0; }
    console_write("usage: synctest check|sem N [W S]|boundary N|boundary-noise N FOREIGN|sleep|cancel|race N|stale|timer-cancel|timer-order|timer-backlog|async-status [RUN]|async-wait RUN [TIMEOUT_MS]|preblock-sem|preblock-timer|preblock-loop N [all|sem|timer]|preblock-timer-noise N|fallback|oom|stats\n");
    return 1;
}
