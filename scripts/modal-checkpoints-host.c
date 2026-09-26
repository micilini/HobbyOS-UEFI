#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOBBYOS_TASKMAN_MODAL_CHECKPOINT_HOST_TEST 1
#include "../kernel/src/shell/commands/cmd_taskmantest.h"
#include "../kernel/src/core/modal_session.h"
#include "../kernel/src/core/modal_ui.h"
#include "../kernel/src/memory/heap.h"

#define MOCK_MAX_WORKERS 32u
#define SERIAL_CAPACITY (256u * 1024u)
#define SERIAL_CALLS_MAX 256u

typedef struct
{
    task_snapshot_t snapshot;
    void *allocation;
    size_t allocation_bytes;
    bool present;
} mock_worker_t;

typedef struct
{
    unsigned int cases;
    unsigned int assertions;
    unsigned int failures;
} test_state_t;

static mock_worker_t workers[MOCK_MAX_WORKERS];
static uint32_t worker_count;
static uint64_t fake_time_ns;
static uint32_t sleep_calls;
static uint32_t clear_dirty_on_sleep;
static uint32_t clear_reaper_on_sleep;
static modal_ui_stats_t modal_stats;
static modal_ui_runtime_snapshot_t modal_runtime;
static modal_session_snapshot_t modal_session;
static taskman_stats_t taskman_stats;
static task_reaper_stats_t reaper_stats;
static HeapStats heap_stats;
static bool modal_valid;
static bool session_valid;
static bool heap_available;
static char serial_capture[SERIAL_CAPACITY];
static size_t serial_length;
static size_t serial_call_offsets[SERIAL_CALLS_MAX];
static size_t serial_call_lengths[SERIAL_CALLS_MAX];
static uint32_t serial_write_calls;
static uint32_t serial_putc_calls;
static test_state_t test_state;

static void check(bool condition)
{
    test_state.assertions++;
    if (!condition)
        test_state.failures++;
}

static void finish_case(const char *name, unsigned int failures_before)
{
    bool passed = failures_before == test_state.failures;
    test_state.cases++;
    printf("[MODAL_CHECKPOINT_HOST][CASE] id=%s status=%s\n",
           name, passed ? "PASS" : "FAIL");
}

static void free_worker_allocations(void)
{
    for (uint32_t i = 0; i < MOCK_MAX_WORKERS; i++)
    {
        free(workers[i].allocation);
        workers[i].allocation = NULL;
        workers[i].allocation_bytes = 0;
        workers[i].present = false;
    }
}

static void reset_serial(void)
{
    memset(serial_capture, 0, sizeof(serial_capture));
    memset(serial_call_offsets, 0, sizeof(serial_call_offsets));
    memset(serial_call_lengths, 0, sizeof(serial_call_lengths));
    serial_length = 0;
    serial_write_calls = 0;
    serial_putc_calls = 0;
}

static void reset_mocks(void)
{
    free_worker_allocations();
    memset(workers, 0, sizeof(workers));
    worker_count = 0;
    fake_time_ns = 0;
    sleep_calls = 0;
    clear_dirty_on_sleep = 0;
    clear_reaper_on_sleep = 0;
    memset(&modal_stats, 0, sizeof(modal_stats));
    memset(&modal_runtime, 0, sizeof(modal_runtime));
    memset(&modal_session, 0, sizeof(modal_session));
    memset(&taskman_stats, 0, sizeof(taskman_stats));
    memset(&reaper_stats, 0, sizeof(reaper_stats));
    heap_stats = (HeapStats){
        .total_bytes = 1024u * 1024u,
        .used_bytes = 4096u,
        .free_bytes = 1024u * 1024u - 4096u,
        .blocks_total = 12u,
        .blocks_free = 4u,
        .largest_free_bytes = 8192u,
    };
    modal_session.state = MODAL_SESSION_INACTIVE;
    modal_session.router_state = INPUT_ROUTE_DEFAULT;
    modal_valid = true;
    session_valid = true;
    heap_available = true;
    reset_serial();
    taskmantest_memory_checkpoint_test_reset();
}

static void set_worker(uint32_t index, uint64_t id, uint64_t generation,
                       size_t bytes, uint64_t schedule, uint64_t runtime)
{
    if (index >= MOCK_MAX_WORKERS)
        abort();
    workers[index].allocation = malloc(bytes ? bytes : 1u);
    if (!workers[index].allocation)
        abort();
    workers[index].allocation_bytes = bytes;
    workers[index].present = true;
    workers[index].snapshot.id = id;
    workers[index].snapshot.lifecycle_generation = generation;
    workers[index].snapshot.schedule_count = schedule;
    workers[index].snapshot.runtime_ns_total = runtime;
    workers[index].snapshot.kernel_mem_est_bytes = bytes;
    snprintf(workers[index].snapshot.name,
             sizeof(workers[index].snapshot.name), "smpstress-%u", index);
    if (index + 1u > worker_count)
        worker_count = index + 1u;
}

static void create_workers(uint32_t count, size_t bytes)
{
    for (uint32_t i = 0; i < count; i++)
        set_worker(i, 1000u + i, 9000u + i, bytes,
                   10u + i, 1000u + i * 100u);
}

static void progress_workers(void)
{
    for (uint32_t i = 0; i < worker_count; i++)
    {
        if (!workers[i].present)
            continue;
        workers[i].snapshot.schedule_count++;
        workers[i].snapshot.runtime_ns_total += 100u;
    }
}

static void release_worker(uint32_t index)
{
    if (index >= worker_count)
        return;
    workers[index].present = false;
    free(workers[index].allocation);
    workers[index].allocation = NULL;
    workers[index].allocation_bytes = 0;
}

static void release_all_workers(void)
{
    for (uint32_t i = 0; i < worker_count; i++)
        release_worker(i);
}

static void complete_modal(uint32_t cycles)
{
    uint64_t completed = (uint64_t)cycles + 1u;
    modal_stats.runs += completed;
    modal_stats.workers_created += completed;
    modal_stats.normal_completions += completed;
    modal_stats.cleanup_closes += completed;
    modal_stats.completion_signals += completed;
}

static bool capture_contains(const char *needle)
{
    return strstr(serial_capture, needle) != NULL;
}

static size_t maximum_serial_call(void)
{
    size_t maximum = 0;
    for (uint32_t i = 0; i < serial_write_calls; i++)
        if (serial_call_lengths[i] > maximum)
            maximum = serial_call_lengths[i];
    return maximum;
}

static bool calls_are_complete_records(void)
{
    for (uint32_t i = 0; i < serial_write_calls; i++)
    {
        const char *text = serial_capture + serial_call_offsets[i];
        size_t length = serial_call_lengths[i];
        if (!length || text[length - 1u] != '\n')
            return false;
        const char *first = strstr(text, "[TASKMANTEST][");
        if (!first || first >= text + length)
            return false;
        const char *second = strstr(first + 1, "[TASKMANTEST][");
        if (second && second < text + length)
            return false;
    }
    return true;
}

uint64_t clock_monotonic_ns(void)
{
    return fake_time_ns;
}

void timer_sleep(uint64_t ms)
{
    if (UINT64_MAX - fake_time_ns < ms * 1000000ULL)
        fake_time_ns = UINT64_MAX;
    else
        fake_time_ns += ms * 1000000ULL;
    sleep_calls++;
    if (clear_dirty_on_sleep && sleep_calls >= clear_dirty_on_sleep)
    {
        modal_runtime.active = 0;
        modal_runtime.contexts_live = 0;
        modal_runtime.contexts_quarantined = 0;
        modal_stats.active = 0;
        modal_stats.contexts_live = 0;
        modal_stats.contexts_quarantined = 0;
        modal_session.state = MODAL_SESSION_INACTIVE;
        modal_session.router_state = INPUT_ROUTE_DEFAULT;
        modal_session.shell_paused = 0;
    }
    if (clear_reaper_on_sleep && sleep_calls >= clear_reaper_on_sleep)
        reaper_stats.free_inflight = 0;
}

task_snapshot_result_t scheduler_snapshot_tasks(task_snapshot_t *out_entries,
                                                uint32_t capacity,
                                                uint32_t offset)
{
    task_snapshot_t visible[MOCK_MAX_WORKERS];
    uint32_t total = 0;
    for (uint32_t i = 0; i < worker_count; i++)
        if (workers[i].present)
            visible[total++] = workers[i].snapshot;
    task_snapshot_result_t result = {
        .total = total,
        .offset = offset,
        .registry_generation = 77u,
        .sample_time_ns = fake_time_ns,
    };
    if (offset < total && out_entries && capacity)
    {
        uint32_t remaining = total - offset;
        result.written = remaining < capacity ? remaining : capacity;
        memcpy(out_entries, &visible[offset],
               result.written * sizeof(*out_entries));
    }
    result.truncated = offset + result.written < total;
    return result;
}

bool scheduler_snapshot_task_by_handle(task_handle_t handle,
                                       task_snapshot_t *out)
{
    for (uint32_t i = 0; i < worker_count; i++)
    {
        if (!workers[i].present || workers[i].snapshot.id != handle.id ||
            workers[i].snapshot.lifecycle_generation !=
                handle.lifecycle_generation)
            continue;
        if (out)
            *out = workers[i].snapshot;
        return true;
    }
    return false;
}

uint32_t scheduler_reap_zombies(uint64_t grace_ms)
{
    (void)grace_ms;
    return 0;
}

void scheduler_reaper_stats_snapshot(task_reaper_stats_t *out)
{
    if (out)
        *out = reaper_stats;
}

void modal_ui_stats_snapshot(modal_ui_stats_t *out)
{
    if (out)
        *out = modal_stats;
}

bool modal_ui_runtime_snapshot(modal_ui_runtime_snapshot_t *out)
{
    if (out)
        *out = modal_runtime;
    return true;
}

bool modal_ui_validate(uint64_t *violations)
{
    if (violations)
        *violations = modal_valid ? 0u : 1u;
    return modal_valid;
}

bool modal_session_snapshot(modal_session_snapshot_t *out)
{
    if (out)
        *out = modal_session;
    return true;
}

bool modal_session_validate(uint64_t *violations)
{
    if (violations)
        *violations = session_valid ? 0u : 1u;
    return session_valid;
}

void taskman_stats_snapshot(taskman_stats_t *out)
{
    if (out)
        *out = taskman_stats;
}

bool heap_get_stats(HeapStats *out)
{
    if (out && heap_available)
        *out = heap_stats;
    return out && heap_available;
}

void serial_write_all(const char *text)
{
    if (!text || serial_write_calls >= SERIAL_CALLS_MAX)
        abort();
    size_t length = strlen(text);
    if (serial_length + length + 1u >= sizeof(serial_capture))
        abort();
    serial_call_offsets[serial_write_calls] = serial_length;
    serial_call_lengths[serial_write_calls] = length;
    serial_write_calls++;
    memcpy(serial_capture + serial_length, text, length);
    serial_length += length;
    serial_capture[serial_length] = '\0';
}

void serial_putc_all(char value)
{
    serial_putc_calls++;
    char text[2] = {value, '\0'};
    serial_write_all(text);
}

static void test_success_with_raw_global_delta(void)
{
    unsigned int before = test_state.failures;
    reset_mocks();
    check(taskmantest_memory_checkpoint_begin_for_test(2u, 2u));
    create_workers(2u, 64u);
    check(taskmantest_memory_checkpoint_track_for_test());
    progress_workers();
    check(taskmantest_memory_checkpoint_progress_for_test());
    complete_modal(2u);
    release_all_workers();
    heap_stats.used_bytes += 64u;
    heap_stats.free_bytes -= 64u;
    heap_stats.blocks_total++;
    check(taskmantest_memory_checkpoint_end_for_test());
    check(capture_contains("scope=TARGET_IDENTITIES"));
    check(capture_contains("global_direction=UP global_drift=64"));
    check(capture_contains("global_comparable=0 endpoint=ESTABLISHED"));
    check(capture_contains("released=2 retained=0 retained_bytes=0"));
    check(serial_write_calls == 10u && serial_putc_calls == 0u);
    check(calls_are_complete_records());
    finish_case("target-lifetime-with-raw-global-delta", before);
}

static void test_initial_timeout_zero(void)
{
    unsigned int before = test_state.failures;
    reset_mocks();
    modal_runtime.active = 1;
    modal_stats.active = 1;
    taskmantest_memory_checkpoint_test_limits(
        0u, TASKMANTEST_MEMORY_CHECKPOINT_TEST_RECORD_CAPACITY);
    check(!taskmantest_memory_checkpoint_begin_for_test(1u, 1u));
    check(sleep_calls == 0u);
    check(serial_write_calls == 1u && serial_putc_calls == 0u);
    check(capture_contains("MEMORY_CHECKPOINT_BEGIN] FAIL"));
    check(capture_contains("reason=timeout"));
    finish_case("initial-timeout-zero", before);
}

static void test_initial_success_at_deadline(void)
{
    unsigned int before = test_state.failures;
    reset_mocks();
    modal_runtime.active = 1;
    modal_stats.active = 1;
    modal_session.state = MODAL_SESSION_ACTIVE;
    modal_session.router_state = INPUT_ROUTE_MODAL;
    modal_session.shell_paused = 1;
    clear_dirty_on_sleep = 2u;
    taskmantest_memory_checkpoint_test_limits(
        20u, TASKMANTEST_MEMORY_CHECKPOINT_TEST_RECORD_CAPACITY);
    check(taskmantest_memory_checkpoint_begin_for_test(1u, 1u));
    check(sleep_calls == 2u && fake_time_ns == 20000000ULL);
    check(capture_contains("attempts=3"));
    finish_case("initial-success-at-deadline", before);
}

static void test_existing_worker_rejected(void)
{
    unsigned int before = test_state.failures;
    reset_mocks();
    create_workers(1u, 64u);
    taskmantest_memory_checkpoint_test_limits(
        0u, TASKMANTEST_MEMORY_CHECKPOINT_TEST_RECORD_CAPACITY);
    check(!taskmantest_memory_checkpoint_begin_for_test(1u, 1u));
    check(capture_contains("initial_workers=1"));
    check(capture_contains("reason=timeout"));
    finish_case("dirty-worker-initial-state", before);
}

static void test_track_timeout_and_generation(void)
{
    unsigned int before = test_state.failures;
    reset_mocks();
    check(taskmantest_memory_checkpoint_begin_for_test(1u, 1u));
    set_worker(0u, 1000u, 0u, 64u, 10u, 100u);
    taskmantest_memory_checkpoint_test_limits(
        0u, TASKMANTEST_MEMORY_CHECKPOINT_TEST_RECORD_CAPACITY);
    check(!taskmantest_memory_checkpoint_track_for_test());
    check(capture_contains("MEMORY_CHECKPOINT_TRACK] FAIL"));
    check(capture_contains("generations=0"));
    check(!capture_contains("MEMORY_RESOURCE_TRACK"));
    finish_case("track-invalid-generation", before);
}

static void test_progress_timeout(void)
{
    unsigned int before = test_state.failures;
    reset_mocks();
    check(taskmantest_memory_checkpoint_begin_for_test(1u, 1u));
    create_workers(1u, 64u);
    check(taskmantest_memory_checkpoint_track_for_test());
    taskmantest_memory_checkpoint_test_limits(
        0u, TASKMANTEST_MEMORY_CHECKPOINT_TEST_RECORD_CAPACITY);
    check(!taskmantest_memory_checkpoint_progress_for_test());
    check(capture_contains("MEMORY_CHECKPOINT_PROGRESS] FAIL"));
    check(capture_contains("progressed=0"));
    check(!capture_contains("MEMORY_RESOURCE_PROGRESS"));
    finish_case("progress-timeout", before);
}

static void test_retained_64_rejected(void)
{
    unsigned int before = test_state.failures;
    reset_mocks();
    check(taskmantest_memory_checkpoint_begin_for_test(1u, 1u));
    create_workers(1u, 64u);
    check(taskmantest_memory_checkpoint_track_for_test());
    progress_workers();
    check(taskmantest_memory_checkpoint_progress_for_test());
    complete_modal(1u);
    heap_stats.used_bytes += 64u;
    heap_stats.free_bytes -= 64u;
    taskmantest_memory_checkpoint_test_limits(
        0u, TASKMANTEST_MEMORY_CHECKPOINT_TEST_RECORD_CAPACITY);
    check(!taskmantest_memory_checkpoint_end_for_test());
    check(capture_contains("released=0 retained=1 retained_bytes=64"));
    check(capture_contains("endpoint=TIMEOUT cleanup=FAILED reason=target-retained"));
    check(capture_contains("MEMORY_RESOURCE_FINAL"));
    check(capture_contains("status=RETAINED"));
    release_all_workers();
    finish_case("retained-64", before);
}

static void test_identity_net_zero_rejected(void)
{
    unsigned int before = test_state.failures;
    reset_mocks();
    check(taskmantest_memory_checkpoint_begin_for_test(1u, 2u));
    create_workers(2u, 64u);
    check(taskmantest_memory_checkpoint_track_for_test());
    progress_workers();
    check(taskmantest_memory_checkpoint_progress_for_test());
    complete_modal(1u);
    release_worker(1u);
    taskmantest_memory_checkpoint_test_limits(
        0u, TASKMANTEST_MEMORY_CHECKPOINT_TEST_RECORD_CAPACITY);
    check(!taskmantest_memory_checkpoint_end_for_test());
    check(capture_contains("global_direction=ZERO global_drift=0"));
    check(capture_contains("released=1 retained=1 retained_bytes=64"));
    check(capture_contains("index=0 id=1000 generation=9000"));
    check(capture_contains("status=RETAINED"));
    release_all_workers();
    finish_case("identity-net-zero", before);
}

static void test_async_drain_at_deadline(void)
{
    unsigned int before = test_state.failures;
    reset_mocks();
    check(taskmantest_memory_checkpoint_begin_for_test(1u, 1u));
    create_workers(1u, 64u);
    check(taskmantest_memory_checkpoint_track_for_test());
    progress_workers();
    check(taskmantest_memory_checkpoint_progress_for_test());
    complete_modal(1u);
    release_all_workers();
    reaper_stats.free_inflight = 1u;
    clear_reaper_on_sleep = 2u;
    taskmantest_memory_checkpoint_test_limits(
        20u, TASKMANTEST_MEMORY_CHECKPOINT_TEST_RECORD_CAPACITY);
    check(taskmantest_memory_checkpoint_end_for_test());
    check(capture_contains("released=1 retained=0 retained_bytes=0"));
    check(capture_contains("release_attempts=3 drain_free_inflight=0"));
    check(capture_contains("endpoint=ESTABLISHED cleanup=COMPLETE reason=none"));
    finish_case("async-drain-at-deadline", before);
}

static void test_async_drain_timeout(void)
{
    unsigned int before = test_state.failures;
    reset_mocks();
    check(taskmantest_memory_checkpoint_begin_for_test(1u, 1u));
    create_workers(1u, 64u);
    check(taskmantest_memory_checkpoint_track_for_test());
    progress_workers();
    check(taskmantest_memory_checkpoint_progress_for_test());
    complete_modal(1u);
    release_all_workers();
    reaper_stats.free_inflight = 1u;
    taskmantest_memory_checkpoint_test_limits(
        0u, TASKMANTEST_MEMORY_CHECKPOINT_TEST_RECORD_CAPACITY);
    check(!taskmantest_memory_checkpoint_end_for_test());
    check(capture_contains("released=1 retained=0 retained_bytes=0"));
    check(capture_contains("release_attempts=1 drain_free_inflight=1"));
    check(capture_contains("endpoint=TIMEOUT cleanup=FAILED reason=async-drain"));
    finish_case("async-drain-timeout", before);
}

static void test_record_capacity_failure(void)
{
    unsigned int before = test_state.failures;
    reset_mocks();
    taskmantest_memory_checkpoint_test_limits(0u, 32u);
    check(!taskmantest_memory_checkpoint_begin_for_test(1u, 1u));
    check(serial_write_calls == 1u && serial_putc_calls == 0u);
    check(strcmp(serial_capture,
                 "[TASKMANTEST][MEMORY_CHECKPOINT_ERROR] reason=format\n") == 0);
    check(!capture_contains("MEMORY_CHECKPOINT_BEGIN] PASS"));
    finish_case("record-capacity", before);
}

static void test_maximum_record_fit(void)
{
    unsigned int before = test_state.failures;
    reset_mocks();
    heap_stats.used_bytes = UINT64_MAX;
    heap_stats.free_bytes = 0;
    heap_stats.total_bytes = UINT64_MAX;
    heap_stats.blocks_total = UINT32_MAX;
    heap_stats.blocks_free = 0;
    check(taskmantest_memory_checkpoint_begin_for_test(1000u, 32u));
    for (uint32_t i = 0; i < 32u; i++)
        set_worker(i, UINT64_MAX - 64u - i, UINT64_MAX - 128u - i,
                   64u, UINT64_MAX - 200u, UINT64_MAX - 200u);
    check(taskmantest_memory_checkpoint_track_for_test());
    progress_workers();
    check(taskmantest_memory_checkpoint_progress_for_test());
    complete_modal(1000u);
    release_all_workers();
    check(taskmantest_memory_checkpoint_end_for_test());
    check(maximum_serial_call() + 1u <
          TASKMANTEST_MEMORY_CHECKPOINT_TEST_RECORD_CAPACITY);
    check(calls_are_complete_records());
    check(serial_putc_calls == 0u);
    finish_case("maximum-record-fit", before);
}

int main(void)
{
    printf("[MODAL_CHECKPOINT_HOST][SUITE_BEGIN] cases=12 capacity=%u\n",
           TASKMANTEST_MEMORY_CHECKPOINT_TEST_RECORD_CAPACITY);
    test_success_with_raw_global_delta();
    test_initial_timeout_zero();
    test_initial_success_at_deadline();
    test_existing_worker_rejected();
    test_track_timeout_and_generation();
    test_progress_timeout();
    test_retained_64_rejected();
    test_identity_net_zero_rejected();
    test_async_drain_at_deadline();
    test_async_drain_timeout();
    test_record_capacity_failure();
    test_maximum_record_fit();
    free_worker_allocations();
    printf("[MODAL_CHECKPOINT_HOST][SUITE_END] status=%s cases=%u assertions=%u failures=%u capacity=%u\n",
           test_state.failures ? "FAIL" : "PASS", test_state.cases,
           test_state.assertions, test_state.failures,
           TASKMANTEST_MEMORY_CHECKPOINT_TEST_RECORD_CAPACITY);
    return test_state.failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
