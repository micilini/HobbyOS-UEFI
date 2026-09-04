#include "cmd_schedtest.h"
#include "../../core/clock.h"
#include "../../core/scheduler.h"
#include "../../drivers/serial.h"
#include "../../drivers/timer.h"
#include "../../graphics/console.h"
#include "../../libc/memory.h"
#include "../../libc/string.h"
#include <stdint.h>

#define SCHEDTEST_MAX_WORKERS 64u
#define SCHEDTEST_ASYNC_WAIT_DEFAULT_MS 300000u
#define SCHEDTEST_ASYNC_WAIT_MAX_MS 3600000u

typedef struct
{
    uint32_t iterations;
    uint32_t worker;
    uint8_t mode;
    uint64_t run;
} schedtest_ctx_t;

static schedtest_ctx_t g_contexts[SCHEDTEST_MAX_WORKERS];
static volatile uint32_t g_remaining;
static volatile uint32_t g_running;
static volatile uint64_t g_async_next_run;
static volatile uint64_t g_async_active_run;
static volatile uint64_t g_async_completed_run;
static volatile uint32_t g_async_kind;
static volatile uint32_t g_async_state;
static volatile uint32_t g_async_workers;
static volatile uint32_t g_async_iterations;
static volatile uint64_t g_async_migrations;
static volatile uint64_t g_async_mismatches;
static volatile uint64_t g_async_scheduler_violations;
static volatile uint64_t g_async_snapshot_sequence;

static void negative_stack_worker(void *arg)
{
    (void)arg;
    for (;;)
    {
        task_cancel_point();
        schedule_voluntary();
    }
}

static bool parse_u32_exact(const char *text, uint32_t *out)
{
    uint64_t value = 0;
    if (!text || !*text || !out)
        return false;
    while (*text)
    {
        if (*text < '0' || *text > '9')
            return false;
        value = value * 10u + (uint32_t)(*text++ - '0');
        if (value > UINT32_MAX)
            return false;
    }
    *out = (uint32_t)value;
    return true;
}

static bool parse_u64_exact(const char *text, uint64_t *out)
{
    uint64_t value = 0;
    if (!text || !*text || !out)
        return false;
    while (*text)
    {
        uint64_t digit;
        if (*text < '0' || *text > '9')
            return false;
        digit = (uint64_t)(*text++ - '0');
        if (value > (UINT64_MAX - digit) / 10u)
            return false;
        value = value * 10u + digit;
    }
    *out = value;
    return true;
}

static uint32_t parse_u32(const char *text, uint32_t fallback)
{
    uint32_t value = 0;
    return parse_u32_exact(text, &value) ? value : fallback;
}

static void write_u64(uint64_t value)
{
    char buffer[21];
    uint32_t count = 0;
    do
    {
        buffer[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (count)
        serial_putc_all(buffer[--count]);
}

const char *schedtest_async_kind_string(schedtest_async_kind_t kind)
{
    switch (kind)
    {
    case SCHEDTEST_ASYNC_NONE: return "NONE";
    case SCHEDTEST_ASYNC_YIELD: return "YIELD";
    case SCHEDTEST_ASYNC_CPU_PIN: return "CPU_PIN";
    case SCHEDTEST_ASYNC_ENTRY_WINDOW: return "ENTRY_WINDOW";
    default: return "INVALID";
    }
}

const char *schedtest_async_state_string(schedtest_async_state_t state)
{
    switch (state)
    {
    case SCHEDTEST_ASYNC_IDLE: return "IDLE";
    case SCHEDTEST_ASYNC_RUNNING: return "RUNNING";
    case SCHEDTEST_ASYNC_PASS: return "PASS";
    case SCHEDTEST_ASYNC_FAIL: return "FAIL";
    default: return "INVALID";
    }
}

void schedtest_async_snapshot(schedtest_async_snapshot_t *out)
{
    uint64_t before;
    uint64_t after;
    if (!out)
        return;
    do
    {
        before = __atomic_load_n(&g_async_snapshot_sequence,
                                 __ATOMIC_ACQUIRE);
        if (before & 1u)
            continue;
        out->run = __atomic_load_n(&g_async_active_run, __ATOMIC_ACQUIRE);
        out->kind = (schedtest_async_kind_t)__atomic_load_n(
            &g_async_kind, __ATOMIC_ACQUIRE);
        out->state = (schedtest_async_state_t)__atomic_load_n(
            &g_async_state, __ATOMIC_ACQUIRE);
        out->workers = __atomic_load_n(&g_async_workers, __ATOMIC_ACQUIRE);
        out->iterations = __atomic_load_n(&g_async_iterations,
                                          __ATOMIC_ACQUIRE);
        out->remaining = __atomic_load_n(&g_remaining, __ATOMIC_ACQUIRE);
        out->migrations = __atomic_load_n(&g_async_migrations,
                                          __ATOMIC_ACQUIRE);
        out->mismatches = __atomic_load_n(&g_async_mismatches,
                                          __ATOMIC_ACQUIRE);
        out->scheduler_violations = __atomic_load_n(
            &g_async_scheduler_violations, __ATOMIC_ACQUIRE);
        after = __atomic_load_n(&g_async_snapshot_sequence,
                                __ATOMIC_ACQUIRE);
    } while (before != after || (after & 1u));
}

bool schedtest_async_snapshot_for_run(uint64_t run,
                                      schedtest_async_snapshot_t *out)
{
    schedtest_async_snapshot_t snapshot;
    if (!run || !out)
        return false;
    schedtest_async_snapshot(&snapshot);
    if (snapshot.run != run)
        return false;
    *out = snapshot;
    return true;
}

bool schedtest_async_completion_contract_valid(void)
{
    schedtest_async_snapshot_t snapshot;
    schedtest_async_snapshot(&snapshot);
    if (snapshot.state == SCHEDTEST_ASYNC_PASS ||
        snapshot.state == SCHEDTEST_ASYNC_FAIL)
    {
        return snapshot.run != 0 && snapshot.remaining == 0 &&
               __atomic_load_n(&g_running, __ATOMIC_ACQUIRE) == 0 &&
               __atomic_load_n(&g_async_completed_run,
                               __ATOMIC_ACQUIRE) == snapshot.run;
    }
    return snapshot.state == SCHEDTEST_ASYNC_IDLE ||
           snapshot.state == SCHEDTEST_ASYNC_RUNNING;
}

static void async_write_begin(void)
{
    __atomic_add_fetch(&g_async_snapshot_sequence, 1, __ATOMIC_ACQ_REL);
}

static void async_write_end(void)
{
    __atomic_add_fetch(&g_async_snapshot_sequence, 1, __ATOMIC_RELEASE);
}

static uint64_t async_allocate_run(void)
{
    uint64_t current = __atomic_load_n(&g_async_next_run, __ATOMIC_ACQUIRE);
    for (;;)
    {
        uint64_t next;
        if (current == UINT64_MAX)
            return 0;
        next = current + 1u;
        if (__atomic_compare_exchange_n(&g_async_next_run, &current, next,
                                        false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return next;
    }
}

static bool async_begin(schedtest_async_kind_t kind, uint32_t workers,
                        uint32_t iterations, uint64_t *out_run)
{
    uint64_t run;
    if (!out_run || !workers || !iterations ||
        __atomic_load_n(&g_remaining, __ATOMIC_ACQUIRE) != 0 ||
        __atomic_exchange_n(&g_running, 1, __ATOMIC_ACQ_REL))
        return false;
    run = async_allocate_run();
    if (!run)
    {
        __atomic_store_n(&g_running, 0, __ATOMIC_RELEASE);
        return false;
    }
    async_write_begin();
    __atomic_store_n(&g_async_active_run, run, __ATOMIC_RELAXED);
    __atomic_store_n(&g_async_kind, (uint32_t)kind, __ATOMIC_RELAXED);
    __atomic_store_n(&g_async_state, SCHEDTEST_ASYNC_RUNNING,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_async_workers, workers, __ATOMIC_RELAXED);
    __atomic_store_n(&g_async_iterations, iterations, __ATOMIC_RELAXED);
    __atomic_store_n(&g_remaining, workers, __ATOMIC_RELAXED);
    __atomic_store_n(&g_async_migrations, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_async_mismatches, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_async_scheduler_violations, 0, __ATOMIC_RELAXED);
    async_write_end();
    *out_run = run;
    return true;
}

static void emit_async_failure(uint64_t run, schedtest_async_kind_t kind,
                               const char *reason, uint32_t created,
                               uint32_t expected)
{
    serial_write_all("[SCHED][ASYNC] FAIL run="); write_u64(run);
    serial_write_all(" kind="); serial_write_all(schedtest_async_kind_string(kind));
    serial_write_all(" reason="); serial_write_all(reason);
    serial_write_all(" created="); write_u64(created);
    serial_write_all(" expected="); write_u64(expected);
    serial_write_all(" remaining=");
    write_u64(__atomic_load_n(&g_remaining, __ATOMIC_ACQUIRE));
    serial_write_all("\n");
}

static void async_fail_create(uint64_t run, schedtest_async_kind_t kind,
                              uint32_t created, uint32_t expected)
{
    async_write_begin();
    __atomic_store_n(&g_async_state, SCHEDTEST_ASYNC_FAIL,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_async_completed_run, run, __ATOMIC_RELAXED);
    __atomic_store_n(&g_running, 0, __ATOMIC_RELAXED);
    __atomic_sub_fetch(&g_remaining, expected - created, __ATOMIC_RELAXED);
    async_write_end();
    emit_async_failure(run, kind, "create", created, expected);
}

static bool async_worker_enter(const schedtest_ctx_t *context)
{
    uint64_t active;
    if (!context)
        return false;
    active = __atomic_load_n(&g_async_active_run, __ATOMIC_ACQUIRE);
    if (context->run == active)
        return true;
    __atomic_add_fetch(&g_async_mismatches, 1, __ATOMIC_RELAXED);
    serial_write_all("[SCHED][ASYNC] FAIL run="); write_u64(context->run);
    serial_write_all(" kind=");
    serial_write_all(schedtest_async_kind_string(
        (schedtest_async_kind_t)context->mode));
    serial_write_all(" reason=stale-worker active_run="); write_u64(active);
    serial_write_all("\n");
    return false;
}

static void async_publish_final(uint64_t run, bool pass,
                                uint64_t scheduler_violations)
{
    async_write_begin();
    __atomic_store_n(&g_async_state,
                     pass ? SCHEDTEST_ASYNC_PASS : SCHEDTEST_ASYNC_FAIL,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_async_completed_run, run, __ATOMIC_RELAXED);
    __atomic_store_n(&g_async_scheduler_violations, scheduler_violations,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_remaining, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_running, 0, __ATOMIC_RELAXED);
    async_write_end();
}

static bool print_stats(const char *kind)
{
    scheduler_runtime_stats_t stats;
    bool ok = scheduler_validate_runtime_invariants(&stats);
    serial_write_all("[SCHED][CHECK] ");
    serial_write_all(ok ? "PASS" : "FAIL");
    if (!ok) { serial_write_all(" code="); write_u64(stats.failure_code); }
    serial_write_all(" cpus="); write_u64(stats.cpus);
    serial_write_all(" tasks="); write_u64(stats.tasks);
    serial_write_all(" started="); write_u64(stats.started);
    serial_write_all(" finished="); write_u64(stats.finished);
    serial_write_all(" pending="); write_u64(stats.pending);
    serial_write_all(" violations="); write_u64(stats.violations);
    serial_write_all(" stale_cpu_slot_entries=");
    write_u64(stats.stale_cpu_slot_entries);
    serial_write_all(" pinned_current_mismatches=");
    write_u64(stats.pinned_current_mismatches);
    serial_write_all(" wrong_finish_cpu="); write_u64(stats.finish_wrong_cpu);
    serial_write_all(" sequence_mismatch=");
    write_u64(stats.finish_sequence_mismatch);
    serial_write_all(" finish_without_pending=");
    write_u64(stats.finish_without_pending);
    serial_write_all(" pending_overwrite="); write_u64(stats.pending_overwrite);
    if (kind) { serial_write_all(" source="); serial_write_all(kind); }
    serial_write_all("\n");
    return ok;
}

static void yield_worker(void *arg)
{
    schedtest_ctx_t *context = (schedtest_ctx_t *)arg;
    volatile uint64_t value;
    if (!async_worker_enter(context))
        return;
    value = (uint64_t)context->worker + 1u;
    for (uint32_t i = 0; i < context->iterations; i++)
    {
        value = value * 6364136223846793005ULL + 1442695040888963407ULL;
        schedule_voluntary();
    }
    (void)value;
    if (__atomic_sub_fetch(&g_remaining, 1, __ATOMIC_ACQ_REL) == 0 &&
        __atomic_load_n(&g_async_state, __ATOMIC_ACQUIRE) ==
            SCHEDTEST_ASYNC_RUNNING)
    {
        uint64_t run = context->run;
        uint32_t workers = __atomic_load_n(&g_async_workers, __ATOMIC_ACQUIRE);
        uint32_t iterations = __atomic_load_n(&g_async_iterations,
                                              __ATOMIC_ACQUIRE);
        async_publish_final(run, true, 0);
        serial_write_all("[SCHED][STRESS] YIELD_COMPLETE run="); write_u64(run);
        serial_write_all(" workers="); write_u64(workers);
        serial_write_all(" iterations="); write_u64(iterations);
        serial_write_all("\n");
    }
}

static void cpu_pin_worker(void *arg)
{
    schedtest_ctx_t *context = (schedtest_ctx_t *)arg;
    cpu_slot_t last_slot = CPU_SLOT_INVALID;
    if (!async_worker_enter(context))
        return;
    for (uint32_t i = 0; i < context->iterations; i++)
    {
        scheduler_cpu_pin_t pin;
        if (!scheduler_cpu_pin(&pin))
            __atomic_add_fetch(&g_async_mismatches, 1, __ATOMIC_RELAXED);
        else
        {
            if (!scheduler_cpu_pin_validate(&pin))
                __atomic_add_fetch(&g_async_mismatches, 1, __ATOMIC_RELAXED);
            if (last_slot != CPU_SLOT_INVALID && last_slot != pin.slot)
                __atomic_add_fetch(&g_async_migrations, 1, __ATOMIC_RELAXED);
            last_slot = pin.slot;
            scheduler_cpu_unpin(&pin);
        }
        schedule_voluntary();
    }
    if (__atomic_sub_fetch(&g_remaining, 1, __ATOMIC_ACQ_REL) == 0 &&
        __atomic_load_n(&g_async_state, __ATOMIC_ACQUIRE) ==
            SCHEDTEST_ASYNC_RUNNING)
    {
        scheduler_runtime_stats_t stats;
        bool runtime_ok = scheduler_validate_runtime_invariants(&stats);
        uint64_t migrations = __atomic_load_n(&g_async_migrations,
                                               __ATOMIC_ACQUIRE);
        uint64_t mismatches = __atomic_load_n(&g_async_mismatches,
                                               __ATOMIC_ACQUIRE);
        bool pass = runtime_ok && !mismatches &&
                    (stats.cpus <= 1 || migrations > 0);
        uint64_t run = context->run;
        uint32_t workers = __atomic_load_n(&g_async_workers, __ATOMIC_ACQUIRE);
        uint32_t iterations = __atomic_load_n(&g_async_iterations,
                                              __ATOMIC_ACQUIRE);
        schedtest_async_kind_t kind = context->mode == SCHEDTEST_ASYNC_CPU_PIN ?
            SCHEDTEST_ASYNC_CPU_PIN : SCHEDTEST_ASYNC_ENTRY_WINDOW;
        async_publish_final(run, pass, stats.violations);
        serial_write_all(kind == SCHEDTEST_ASYNC_CPU_PIN ?
                         "[SCHED][CPU_PIN] " : "[SCHED][ENTRY_WINDOW] ");
        serial_write_all(pass ? "PASS" : "FAIL");
        serial_write_all(" run="); write_u64(run);
        serial_write_all(" workers="); write_u64(workers);
        serial_write_all(" iterations="); write_u64(iterations);
        serial_write_all(" migrations="); write_u64(migrations);
        serial_write_all(" mismatches="); write_u64(mismatches);
        serial_write_all(" scheduler_violations="); write_u64(stats.violations);
        if (kind == SCHEDTEST_ASYNC_CPU_PIN)
        {
            serial_write_all(" stale="); write_u64(stats.stale_cpu_slot_entries);
            serial_write_all(" current_mismatch=");
            write_u64(stats.pinned_current_mismatches);
            serial_write_all(" wrong_finish_cpu=");
            write_u64(stats.finish_wrong_cpu);
            serial_write_all(" sequence_mismatch=");
            write_u64(stats.finish_sequence_mismatch);
        }
        serial_write_all("\n");
    }
}

static void emit_async_snapshot(const char *tag,
                                const schedtest_async_snapshot_t *snapshot)
{
    serial_write_all("[SCHED]["); serial_write_all(tag); serial_write_all("] PASS run=");
    write_u64(snapshot->run);
    serial_write_all(" kind=");
    serial_write_all(schedtest_async_kind_string(snapshot->kind));
    serial_write_all(" state=");
    serial_write_all(schedtest_async_state_string(snapshot->state));
    serial_write_all(" workers="); write_u64(snapshot->workers);
    serial_write_all(" iterations="); write_u64(snapshot->iterations);
    serial_write_all(" remaining="); write_u64(snapshot->remaining);
    serial_write_all(" migrations="); write_u64(snapshot->migrations);
    serial_write_all(" mismatches="); write_u64(snapshot->mismatches);
    serial_write_all(" scheduler_violations=");
    write_u64(snapshot->scheduler_violations);
    serial_write_all("\n");
}

static int async_status_command(int argc, char **argv)
{
    schedtest_async_snapshot_t snapshot;
    uint64_t requested = 0;
    schedtest_async_snapshot(&snapshot);
    if (argc >= 3 && (!parse_u64_exact(argv[2], &requested) || !requested ||
                      requested != snapshot.run))
    {
        serial_write_all("[SCHED][ASYNC_STATUS] FAIL reason=unknown-run requested=");
        write_u64(requested);
        serial_write_all(" active="); write_u64(snapshot.run);
        serial_write_all(" completed=");
        write_u64(__atomic_load_n(&g_async_completed_run, __ATOMIC_ACQUIRE));
        serial_write_all("\n");
        return 1;
    }
    emit_async_snapshot("ASYNC_STATUS", &snapshot);
    return 0;
}

static int async_wait_command(int argc, char **argv)
{
    schedtest_async_snapshot_t snapshot;
    uint64_t run;
    uint32_t timeout_ms = SCHEDTEST_ASYNC_WAIT_DEFAULT_MS;
    uint64_t deadline;
    if (argc < 3 || !parse_u64_exact(argv[2], &run) || !run ||
        (argc >= 4 && (!parse_u32_exact(argv[3], &timeout_ms) ||
                       !timeout_ms || timeout_ms > SCHEDTEST_ASYNC_WAIT_MAX_MS)))
    {
        serial_write_all("[SCHED][ASYNC_WAIT] FAIL reason=parse-or-range\n");
        return 1;
    }
    deadline = clock_monotonic_ns() + (uint64_t)timeout_ms * 1000000ULL;
    for (;;)
    {
        if (!schedtest_async_snapshot_for_run(run, &snapshot))
        {
            serial_write_all("[SCHED][ASYNC_WAIT] FAIL reason=unknown-run run=");
            write_u64(run); serial_write_all("\n");
            return 1;
        }
        if (snapshot.state == SCHEDTEST_ASYNC_PASS)
        {
            serial_write_all("[SCHED][ASYNC_WAIT] PASS run="); write_u64(run);
            serial_write_all(" kind=");
            serial_write_all(schedtest_async_kind_string(snapshot.kind));
            serial_write_all(" workers="); write_u64(snapshot.workers);
            serial_write_all(" iterations="); write_u64(snapshot.iterations);
            serial_write_all(" remaining="); write_u64(snapshot.remaining);
            serial_write_all("\n");
            return 0;
        }
        if (snapshot.state == SCHEDTEST_ASYNC_FAIL)
        {
            serial_write_all("[SCHED][ASYNC_WAIT] FAIL reason=workload run=");
            write_u64(run);
            serial_write_all(" kind=");
            serial_write_all(schedtest_async_kind_string(snapshot.kind));
            serial_write_all(" remaining="); write_u64(snapshot.remaining);
            serial_write_all(" mismatches="); write_u64(snapshot.mismatches);
            serial_write_all(" scheduler_violations=");
            write_u64(snapshot.scheduler_violations);
            serial_write_all("\n");
            return 1;
        }
        if (clock_monotonic_ns() >= deadline)
        {
            serial_write_all("[SCHED][ASYNC_WAIT] FAIL reason=timeout run=");
            write_u64(run);
            serial_write_all(" remaining="); write_u64(snapshot.remaining);
            serial_write_all("\n");
            return 1;
        }
        timer_sleep(10);
    }
}

static int start_pin_workload(const char *command, uint32_t iterations)
{
    scheduler_runtime_stats_t stats;
    uint32_t workers;
    uint32_t created = 0;
    uint64_t run;
    schedtest_async_kind_t kind = !strcmp(command, "cpu-pin") ?
        SCHEDTEST_ASYNC_CPU_PIN : SCHEDTEST_ASYNC_ENTRY_WINDOW;
    if (!iterations || !scheduler_validate_runtime_invariants(&stats) ||
        !stats.cpus)
    {
        console_write("schedtest: invalid or scheduler fault\n");
        return 1;
    }
    workers = stats.cpus * 4u;
    if (workers > SCHEDTEST_MAX_WORKERS)
        workers = SCHEDTEST_MAX_WORKERS;
    if (!async_begin(kind, workers, iterations, &run))
    {
        console_write("schedtest: async busy, draining, or run exhausted\n");
        return 1;
    }
    serial_write_all(kind == SCHEDTEST_ASYNC_CPU_PIN ?
                     "[SCHED][CPU_PIN] START run=" :
                     "[SCHED][ENTRY_WINDOW] START run=");
    write_u64(run);
    serial_write_all(" workers="); write_u64(workers);
    serial_write_all(" iterations="); write_u64(iterations);
    serial_write_all("\n");
    for (uint32_t i = 0; i < workers; i++)
    {
        g_contexts[i].iterations = iterations / workers +
            (i < iterations % workers ? 1u : 0u);
        g_contexts[i].worker = i;
        g_contexts[i].mode = (uint8_t)kind;
        g_contexts[i].run = run;
        if (!thread_create_named(cpu_pin_worker, &g_contexts[i],
                                 kind == SCHEDTEST_ASYNC_CPU_PIN ?
                                 "schedtest-cpu-pin" : "schedtest-entry"))
        {
            async_fail_create(run, kind, created, workers);
            return 1;
        }
        created++;
    }
    return 0;
}

static int start_yield_workload(uint32_t workers, uint32_t iterations)
{
    uint32_t created = 0;
    uint64_t run;
    if (!workers || workers > SCHEDTEST_MAX_WORKERS || !iterations ||
        !async_begin(SCHEDTEST_ASYNC_YIELD, workers, iterations, &run))
    {
        console_write("schedtest: invalid, async busy, draining, or run exhausted\n");
        return 1;
    }
    serial_write_all("[SCHED][STRESS] YIELD_START run="); write_u64(run);
    serial_write_all(" workers="); write_u64(workers);
    serial_write_all(" iterations="); write_u64(iterations);
    serial_write_all("\n");
    for (uint32_t i = 0; i < workers; i++)
    {
        g_contexts[i].iterations = iterations;
        g_contexts[i].worker = i;
        g_contexts[i].mode = SCHEDTEST_ASYNC_YIELD;
        g_contexts[i].run = run;
        if (!thread_create_named(yield_worker, &g_contexts[i],
                                 "schedtest-yield"))
        {
            async_fail_create(run, SCHEDTEST_ASYNC_YIELD, created, workers);
            return 1;
        }
        created++;
    }
    return 0;
}

int cmd_schedtest(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "check") == 0)
    {
        return print_stats("check") ? 0 : 1;
    }
    if (strcmp(argv[1], "stats") == 0)
    {
        (void)print_stats("stats");
        return 0;
    }
    if (strcmp(argv[1], "stack") == 0)
    {
        scheduler_report_stack_usage();
        return 0;
    }
    if (strcmp(argv[1], "async-status") == 0)
        return async_status_command(argc, argv);
    if (strcmp(argv[1], "async-wait") == 0)
        return async_wait_command(argc, argv);
    if (strcmp(argv[1], "negative-finish") == 0)
    {
        scheduler_test_force_finish_validation_failure(1u << 9);
        schedule_voluntary();
        return 1;
    }
    if (strcmp(argv[1], "negative-guard") == 0)
    {
        task_handle_t handle;
        task_kill_result_t result;
        scheduler_test_hold_next_ready_creation();
        if (!thread_create_named_handle(negative_stack_worker, NULL,
                                        "stack-negative", &handle) ||
            !scheduler_test_corrupt_stack_guard(handle.id, 0))
            return 1;
        scheduler_test_request_kill_when(handle.id,
                                         TASK_TEST_MATCH_READY_OFF_CPU,
                                         &result, NULL);
        schedule_voluntary();
        return 1;
    }
    if (strcmp(argv[1], "cpu-pin") == 0 ||
        strcmp(argv[1], "entry-window") == 0)
    {
        uint32_t iterations = argc >= 3 ? parse_u32(argv[2], 200000) : 200000;
        return start_pin_workload(argv[1], iterations);
    }
    if (strcmp(argv[1], "reap0") == 0)
    {
        uint32_t reaped = scheduler_reap_zombies(0);
        serial_write_all("[SCHED][STRESS] REAP0 reaped=");
        write_u64(reaped); serial_write_all("\n");
        (void)print_stats("reap0");
        return 0;
    }
    if (strcmp(argv[1], "yield") == 0)
    {
        uint32_t workers = argc >= 3 ? parse_u32(argv[2], 1) : 1;
        uint32_t iterations = argc >= 4 ? parse_u32(argv[3], 10) : 10;
        return start_yield_workload(workers, iterations);
    }
    console_write("usage: schedtest check|stats|stack|async-status [run]|async-wait <run> [timeout_ms]|cpu-pin <iterations>|entry-window <iterations>|yield <workers> <iterations>|reap0\n");
    return 1;
}
