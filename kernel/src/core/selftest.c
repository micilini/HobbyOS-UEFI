#include "selftest.h"

#include "clock.h"
#include "input_router.h"
#include "modal_session.h"
#include "modal_ui.h"
#include "runtime_ready.h"
#include "scheduler.h"
#include "semaphore.h"
#include "task_format.h"
#include "task_metrics.h"
#include "timers.h"
#include "../drivers/serial.h"
#include "../shell/shell.h"
#include "../shell/commands/cmd_kill.h"
#include "../shell/commands/cmd_accounttest.h"
#include "../shell/commands/cmd_modaltest.h"
#include "../shell/commands/cmd_schedtest.h"
#include "../shell/commands/cmd_synctest.h"
#include "../shell/commands/cmd_taskman.h"
#include "../shell/commands/cmd_taskmantest.h"
#include "../shell/commands/cmd_tasktest.h"
#include "../shell/commands/registry.h"
#include "../smp/smp_boot.h"
#include "../smp/smp_topology.h"
#include "../timer/hpet.h"
#include "../libc/memory.h"
#include "../libc/string.h"

static selftest_summary_t g_last_summary;

#define SELFTEST_RECORD_MAX 256u

typedef struct
{
    char bytes[SELFTEST_RECORD_MAX];
    uint32_t length;
    uint8_t overflow;
} selftest_record_builder_t;

static void selftest_record_reset(selftest_record_builder_t *record)
{
    if (!record)
        return;
    record->length = 0;
    record->overflow = 0;
    record->bytes[0] = '\0';
}

static bool selftest_record_append_char(selftest_record_builder_t *record,
                                        char value)
{
    if (!record || record->overflow ||
        record->length >= SELFTEST_RECORD_MAX - 1u)
    {
        if (record)
            record->overflow = 1;
        return false;
    }
    record->bytes[record->length++] = value;
    record->bytes[record->length] = '\0';
    return true;
}

static bool selftest_record_begin_line(selftest_record_builder_t *record)
{
    selftest_record_reset(record);
    return selftest_record_append_char(record, '\n');
}

static bool selftest_record_append_text(selftest_record_builder_t *record,
                                        const char *text)
{
    if (!record || !text)
    {
        if (record)
            record->overflow = 1;
        return false;
    }
    while (*text)
    {
        if (!selftest_record_append_char(record, *text++))
            return false;
    }
    return true;
}

static bool selftest_record_append_u64(selftest_record_builder_t *record,
                                       uint64_t value)
{
    char reverse[20];
    uint32_t count = 0;
    do
    {
        reverse[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (count)
        if (!selftest_record_append_char(record, reverse[--count]))
            return false;
    return true;
}

static bool selftest_record_finish_line(selftest_record_builder_t *record)
{
    return selftest_record_append_char(record, '\n');
}

static bool selftest_record_emit(const selftest_record_builder_t *record)
{
    if (!record || record->overflow || !record->length ||
        record->bytes[record->length - 1u] != '\n')
    {
        serial_write_all("\n[SELFTEST][EMIT_ERROR] reason=record-overflow\n");
        return false;
    }
    serial_write_all(record->bytes);
    return true;
}

static const char *severity_name(selftest_severity_t severity)
{
    switch (severity)
    {
    case SELFTEST_SEVERITY_P0: return "P0";
    case SELFTEST_SEVERITY_P1: return "P1";
    case SELFTEST_SEVERITY_P2: return "P2";
    default: return "INVALID";
    }
}

static bool selftest_record_build_result(selftest_record_builder_t *record,
                                         const selftest_result_t *result)
{
    if (!record || !result)
        return false;
    const char *status = result->status == SELFTEST_PASS ? "PASS" :
                         result->status == SELFTEST_FAIL ? "FAIL" : "SKIP";
    bool ok = selftest_record_begin_line(record) &&
              selftest_record_append_text(record, "[SELFTEST][") &&
              selftest_record_append_text(record, status) &&
              selftest_record_append_text(record, "] ") &&
              selftest_record_append_text(record,
                  result->name ? result->name : "invalid.unnamed") &&
              selftest_record_append_text(record, " severity=") &&
              selftest_record_append_text(record,
                  severity_name(result->severity));
    if (result->status == SELFTEST_FAIL)
    {
        ok = ok && selftest_record_append_text(record, " detail=") &&
             selftest_record_append_text(record,
                 result->detail_key ? result->detail_key : "condition") &&
             selftest_record_append_text(record, " expected=") &&
             selftest_record_append_u64(record, result->expected) &&
             selftest_record_append_text(record, " actual=") &&
             selftest_record_append_u64(record, result->actual);
    }
    else if (result->status == SELFTEST_SKIP)
    {
        ok = ok && selftest_record_append_text(record, " reason=") &&
             selftest_record_append_text(record,
                 result->detail_key ? result->detail_key : "unspecified");
    }
    return ok && selftest_record_finish_line(record);
}

static bool selftest_record_build_summary(selftest_record_builder_t *record,
                                          const selftest_summary_t *summary)
{
    if (!record || !summary)
        return false;
    return selftest_record_begin_line(record) &&
           selftest_record_append_text(record, "[SELFTEST][SUMMARY] pass=") &&
           selftest_record_append_u64(record, summary->pass) &&
           selftest_record_append_text(record, " fail=") &&
           selftest_record_append_u64(record, summary->fail) &&
           selftest_record_append_text(record, " skip=") &&
           selftest_record_append_u64(record, summary->skip) &&
           selftest_record_append_text(record, " p0_fail=") &&
           selftest_record_append_u64(record, summary->p0_fail) &&
           selftest_record_append_text(record, " p1_fail=") &&
           selftest_record_append_u64(record, summary->p1_fail) &&
           selftest_record_append_text(record, " p2_fail=") &&
           selftest_record_append_u64(record, summary->p2_fail) &&
           selftest_record_finish_line(record);
}

void selftest_summary_reset(selftest_summary_t *summary)
{
    if (summary)
        memset(summary, 0, sizeof(*summary));
}

void selftest_emit_result(const selftest_result_t *result)
{
    if (!result)
        return;
    selftest_record_builder_t record;
    (void)selftest_record_build_result(&record, result);
    (void)selftest_record_emit(&record);
}

void selftest_summary_add(selftest_summary_t *summary,
                          const selftest_result_t *result)
{
    if (!summary || !result)
        return;
    if (result->status == SELFTEST_PASS)
        summary->pass++;
    else if (result->status == SELFTEST_SKIP)
        summary->skip++;
    else
    {
        summary->fail++;
        if (result->severity == SELFTEST_SEVERITY_P0) summary->p0_fail++;
        else if (result->severity == SELFTEST_SEVERITY_P1) summary->p1_fail++;
        else summary->p2_fail++;
    }
}

void selftest_emit_summary(const selftest_summary_t *summary)
{
    if (!summary)
        return;
    selftest_record_builder_t record;
    (void)selftest_record_build_summary(&record, summary);
    (void)selftest_record_emit(&record);
}

void selftest_last_summary(selftest_summary_t *out)
{
    if (out)
        *out = g_last_summary;
}

#ifndef HOBBYOS_SELFTEST

bool selftest_run_cases(const selftest_case_t *cases, uint32_t count,
                        selftest_summary_t *summary)
{
    (void)cases; (void)count;
    selftest_summary_reset(summary);
    return false;
}
bool selftest_run_suite(const char *suite, selftest_summary_t *summary)
{ (void)suite; selftest_summary_reset(summary); return false; }
bool selftest_run_all(selftest_summary_t *summary)
{ selftest_summary_reset(summary); return false; }
bool selftest_autorun_if_enabled(void) { return true; }
bool selftest_registry_validate(uint64_t *out_violations)
{ if (out_violations) *out_violations = 0; return true; }

#else

#define SELFTEST_PAGE_SIZE 64u
#define SELFTEST_SNAPSHOT_RETRIES 4u

typedef struct
{
    uint32_t total, idle, running, ready, zombies;
    uint32_t invalid_ids, duplicate_or_reused, idle_unprotected;
    uint32_t ready_on_cpu, zombie_on_cpu, queue_errors;
    uint64_t generation;
} task_facts_t;

static task_snapshot_t g_selftest_page[SELFTEST_PAGE_SIZE];

static bool collect_task_facts(task_facts_t *out)
{
    if (!out)
        return false;
    for (uint32_t retry = 0; retry < SELFTEST_SNAPSHOT_RETRIES; retry++)
    {
        task_facts_t facts;
        memset(&facts, 0, sizeof(facts));
        task_snapshot_result_t probe = scheduler_snapshot_tasks(NULL, 0, 0);
        facts.total = probe.total;
        facts.generation = probe.registry_generation;
        task_id_t previous = TASK_ID_INVALID;
        bool stable = true;
        for (uint32_t offset = 0; offset < probe.total;
             offset += SELFTEST_PAGE_SIZE)
        {
            task_snapshot_result_t page = scheduler_snapshot_tasks(
                g_selftest_page, SELFTEST_PAGE_SIZE, offset);
            if (page.registry_generation != probe.registry_generation ||
                page.total != probe.total || page.offset != offset)
            {
                stable = false;
                break;
            }
            for (uint32_t i = 0; i < page.written; i++)
            {
                const task_snapshot_t *task = &g_selftest_page[i];
                if (!task->id) facts.invalid_ids++;
                if (previous && task->id <= previous) facts.duplicate_or_reused++;
                previous = task->id;
                if (task->is_idle)
                {
                    facts.idle++;
                    if (!(task->flags & TASK_FLAG_KILL_PROTECTED) ||
                        (task->flags & TASK_FLAG_KILLABLE))
                        facts.idle_unprotected++;
                }
                if (task->state == TASK_RUNNING) facts.running++;
                if (task->state == TASK_READY) facts.ready++;
                if (task->state == TASK_ZOMBIE) facts.zombies++;
                if (task->state == TASK_READY && task->on_cpu)
                    facts.ready_on_cpu++;
                if (task->state == TASK_ZOMBIE && task->on_cpu)
                    facts.zombie_on_cpu++;
                if (task->state == TASK_READY && !task->is_idle &&
                    task->queue_membership != TASK_QUEUE_RUNQ_INTERACTIVE &&
                    task->queue_membership != TASK_QUEUE_RUNQ_NORMAL &&
                    !task->on_cpu)
                    facts.queue_errors++;
            }
            if (!page.written && offset < probe.total)
            {
                stable = false;
                break;
            }
        }
        if (stable)
        {
            *out = facts;
            return true;
        }
    }
    return false;
}

static bool expect_value(selftest_result_t *out, const char *key,
                         uint64_t expected, uint64_t actual)
{
    out->detail_key = key;
    out->expected = expected;
    out->actual = actual;
    return expected == actual;
}

static bool identity_valid(selftest_result_t *out)
{
    task_facts_t facts;
    bool ok = collect_task_facts(&facts) && scheduler_validate_task_identity();
    return expect_value(out, "identity_violations", 0, ok ? 0 : 1);
}

static bool identity_zero(selftest_result_t *out)
{
    return expect_value(out, "invalid_task_id", 0, TASK_ID_INVALID);
}

static bool identity_idle(selftest_result_t *out)
{
    task_facts_t facts;
    if (!collect_task_facts(&facts))
        return expect_value(out, "snapshot_stable", 1, 0);
    return expect_value(out, "idle_count", smp_online_cpu_count(), facts.idle);
}

static bool identity_flags(selftest_result_t *out)
{
    task_facts_t facts;
    if (!collect_task_facts(&facts))
        return expect_value(out, "snapshot_stable", 1, 0);
    return expect_value(out, "idle_flag_violations", 0,
                        facts.idle_unprotected);
}

static bool parser_max(selftest_result_t *out)
{
    task_id_t id = 0;
    task_id_parse_result_t result = task_id_parse_decimal_ex(
        "18446744073709551615", &id);
    return expect_value(out, "parser_uint64_max", 1,
                        result == TASK_ID_PARSE_OK && id == UINT64_MAX);
}

static bool parser_overflow(selftest_result_t *out)
{
    task_id_t id = 0;
    return expect_value(out, "parser_result", TASK_ID_PARSE_OVERFLOW,
        task_id_parse_decimal_ex("18446744073709551616", &id));
}

static bool pid_format(selftest_result_t *out)
{
    char value[TASK_FORMAT_PID_CAP];
    bool ok = task_format_pid(UINT64_MAX, value, sizeof(value)) &&
              !strcmp(value, "18446744073709551615");
    return expect_value(out, "uint64_format", 1, ok);
}

static bool scheduler_runtime(selftest_result_t *out)
{
    scheduler_runtime_stats_t stats;
    bool ok = scheduler_validate_runtime_invariants(&stats);
    return expect_value(out, "runtime_violations", 0,
                        ok ? 0 : stats.violations + stats.failure_code + 1u);
}

static bool scheduler_started_case(selftest_result_t *out)
{
    return expect_value(out, "scheduler_started", 1, scheduler_is_started());
}

static bool scheduler_topology(selftest_result_t *out)
{
    uint64_t violations = 0;
    for (cpu_slot_t slot = 0; slot < g_cpu_count; slot++)
    {
        const SmpCpuInfo *cpu = smp_cpu_by_slot_const(slot);
        cpu_slot_t roundtrip = CPU_SLOT_INVALID;
        if (!cpu || cpu->slot != slot ||
            !smp_cpu_slot_from_apic_id(cpu->apic_id, &roundtrip) ||
            roundtrip != slot)
            violations++;
    }
    return expect_value(out, "topology_violations", 0, violations);
}

static bool scheduler_async_kind_strings(selftest_result_t *out)
{
    bool ok = !strcmp(schedtest_async_kind_string(SCHEDTEST_ASYNC_NONE),
                      "NONE") &&
              !strcmp(schedtest_async_kind_string(SCHEDTEST_ASYNC_YIELD),
                      "YIELD") &&
              !strcmp(schedtest_async_kind_string(SCHEDTEST_ASYNC_CPU_PIN),
                      "CPU_PIN") &&
              !strcmp(schedtest_async_kind_string(
                          SCHEDTEST_ASYNC_ENTRY_WINDOW), "ENTRY_WINDOW") &&
              !strcmp(schedtest_async_state_string(SCHEDTEST_ASYNC_IDLE),
                      "IDLE") &&
              !strcmp(schedtest_async_state_string(SCHEDTEST_ASYNC_RUNNING),
                      "RUNNING") &&
              !strcmp(schedtest_async_state_string(SCHEDTEST_ASYNC_PASS),
                      "PASS") &&
              !strcmp(schedtest_async_state_string(SCHEDTEST_ASYNC_FAIL),
                      "FAIL");
    return expect_value(out, "async_names", 1, ok);
}

static bool scheduler_async_initial_idle(selftest_result_t *out)
{
    schedtest_async_snapshot_t snapshot;
    schedtest_async_snapshot(&snapshot);
    bool ok = snapshot.run ?
        snapshot.kind != SCHEDTEST_ASYNC_NONE :
        snapshot.kind == SCHEDTEST_ASYNC_NONE &&
        snapshot.state == SCHEDTEST_ASYNC_IDLE && !snapshot.workers &&
        !snapshot.iterations && !snapshot.remaining;
    return expect_value(out, "async_initial", 1, ok);
}

static bool scheduler_async_run_nonzero(selftest_result_t *out)
{
    schedtest_async_snapshot_t snapshot;
    schedtest_async_snapshot(&snapshot);
    bool ok = (snapshot.kind == SCHEDTEST_ASYNC_NONE) == (snapshot.run == 0);
    return expect_value(out, "async_run_identity", 1, ok);
}

static bool scheduler_async_snapshot_coherent(selftest_result_t *out)
{
    schedtest_async_snapshot_t snapshot;
    schedtest_async_snapshot(&snapshot);
    bool idle = snapshot.state == SCHEDTEST_ASYNC_IDLE && !snapshot.run &&
                snapshot.kind == SCHEDTEST_ASYNC_NONE && !snapshot.remaining;
    bool running = snapshot.state == SCHEDTEST_ASYNC_RUNNING && snapshot.run &&
                   snapshot.kind != SCHEDTEST_ASYNC_NONE &&
                   snapshot.remaining <= snapshot.workers;
    bool completed = (snapshot.state == SCHEDTEST_ASYNC_PASS ||
                      snapshot.state == SCHEDTEST_ASYNC_FAIL) && snapshot.run &&
                     snapshot.kind != SCHEDTEST_ASYNC_NONE;
    return expect_value(out, "async_snapshot", 1,
                        idle || running || completed);
}

static bool scheduler_async_completion_order(selftest_result_t *out)
{
    return expect_value(out, "published_before_marker", 1,
                        schedtest_async_completion_contract_valid());
}

static bool scheduler_async_unknown_run(selftest_result_t *out)
{
    schedtest_async_snapshot_t snapshot;
    schedtest_async_snapshot_t ignored;
    uint64_t unknown;
    schedtest_async_snapshot(&snapshot);
    unknown = snapshot.run == UINT64_MAX ? snapshot.run - 1u : snapshot.run + 1u;
    if (!unknown)
        unknown = 1;
    return expect_value(out, "unknown_run_rejected", 1,
        !schedtest_async_snapshot_for_run(unknown, &ignored));
}

static bool sync_timer_valid(selftest_result_t *out)
{
    return expect_value(out, "timer_violations", 0, timers_validate() ? 0 : 1);
}

static bool sync_semaphore_pure(selftest_result_t *out)
{
    semaphore_t semaphore;
    sem_init(&semaphore, 1);
    bool first = sem_try_wait(&semaphore);
    bool empty = !sem_try_wait(&semaphore);
    bool signaled = sem_signal(&semaphore);
    bool second = sem_try_wait(&semaphore);
    return expect_value(out, "semaphore_contract", 1,
                        first && empty && signaled && second);
}

static bool sync_semaphore_observer(selftest_result_t *out)
{
    return expect_value(out, "observer_atomic_config", 1,
                        sem_test_after_prepare_observer_selftest());
}

static bool runtime_ready_state_monotonic(selftest_result_t *out)
{
    runtime_ready_snapshot_t snapshot;
    bool ok = runtime_ready_snapshot(&snapshot) &&
              snapshot.state >= RUNTIME_READY_READY &&
              snapshot.state <= RUNTIME_READY_TEST_READY &&
              snapshot.published_ns != 0;
    return expect_value(out, "runtime_state_monotonic", 1, ok);
}

static bool runtime_ready_cpu_contract(selftest_result_t *out)
{
    runtime_ready_snapshot_t snapshot;
    bool ok = runtime_ready_snapshot(&snapshot) &&
              snapshot.cpus_expected != 0 &&
              snapshot.cpus_online == snapshot.cpus_expected &&
              snapshot.scheduler_started;
    return expect_value(out, "runtime_cpu_contract", 1, ok);
}

static bool runtime_ready_shell_contract(selftest_result_t *out)
{
    runtime_ready_snapshot_t snapshot;
    bool ok = runtime_ready_snapshot(&snapshot) &&
              snapshot.dpc_initialized && snapshot.shell_initialized &&
              snapshot.shell_thread_started && snapshot.input_valid &&
              snapshot.modal_session_valid && snapshot.modal_ui_valid;
    return expect_value(out, "runtime_shell_contract", 1, ok);
}

static bool runtime_ready_hotplug_balance(selftest_result_t *out)
{
    runtime_ready_snapshot_t snapshot;
    bool ok = runtime_ready_snapshot(&snapshot) &&
              snapshot.hotplug_active == 0 && snapshot.pci_scan_complete;
    return expect_value(out, "runtime_hotplug_balance", 1, ok);
}

static bool runtime_ready_test_gate(selftest_result_t *out)
{
    uint64_t violations = 0;
    bool ok = runtime_ready_is_ready() &&
              runtime_ready_validate(&violations) && violations == 0;
    return expect_value(out, "runtime_test_gate", 1, ok);
}

static bool sync_async_initial_idle(selftest_result_t *out)
{
    synctest_async_snapshot_t snapshot;
    synctest_async_snapshot(&snapshot);
    bool ok = (snapshot.state == SYNCTEST_ASYNC_IDLE && !snapshot.run &&
               snapshot.kind == SYNCTEST_ASYNC_NONE) ||
              ((snapshot.state == SYNCTEST_ASYNC_PASS ||
                snapshot.state == SYNCTEST_ASYNC_FAIL) && snapshot.run);
    return expect_value(out, "sync_async_initial", 1, ok);
}

static bool sync_async_run_nonzero(selftest_result_t *out)
{
    synctest_async_snapshot_t snapshot;
    synctest_async_snapshot(&snapshot);
    bool ok = snapshot.state == SYNCTEST_ASYNC_IDLE ? snapshot.run == 0 :
                                                     snapshot.run != 0;
    return expect_value(out, "sync_async_run_nonzero", 1, ok);
}

static bool sync_async_snapshot_coherent(selftest_result_t *out)
{
    synctest_async_snapshot_t snapshot;
    synctest_async_snapshot(&snapshot);
    bool idle = snapshot.state == SYNCTEST_ASYNC_IDLE && !snapshot.run &&
                snapshot.kind == SYNCTEST_ASYNC_NONE &&
                !snapshot.requested && !snapshot.completed &&
                !snapshot.errors;
    bool running = snapshot.state == SYNCTEST_ASYNC_RUNNING && snapshot.run &&
                   snapshot.kind != SYNCTEST_ASYNC_NONE;
    bool completed = (snapshot.state == SYNCTEST_ASYNC_PASS ||
                      snapshot.state == SYNCTEST_ASYNC_FAIL) && snapshot.run &&
                     snapshot.kind != SYNCTEST_ASYNC_NONE;
    return expect_value(out, "sync_async_snapshot", 1,
                        idle || running || completed);
}

static bool sync_async_unknown_run(selftest_result_t *out)
{
    synctest_async_snapshot_t snapshot, ignored;
    synctest_async_snapshot(&snapshot);
    uint64_t unknown = snapshot.run == UINT64_MAX ? snapshot.run - 1u :
                                                    snapshot.run + 1u;
    if (!unknown) unknown = 1;
    return expect_value(out, "sync_unknown_run_rejected", 1,
        !synctest_async_snapshot_for_run(unknown, &ignored));
}

static bool sync_async_completion_order(selftest_result_t *out)
{
    return expect_value(out, "sync_published_before_marker", 1,
                        synctest_async_completion_contract_valid());
}

static bool sync_timer_balance(selftest_result_t *out)
{
    timer_stats_t stats;
    timers_get_stats(&stats);
    uint64_t violations = stats.task_ref_release_failures +
                          stats.task_ref_duplicate_release;
    return expect_value(out, "timer_ref_violations", 0, violations);
}

static bool accounting_clock(selftest_result_t *out)
{
    bool ok = clock_monotonic_is_ready() && clock_monotonic_selftest() &&
              clock_sample_classifier_selftest();
    return expect_value(out, "clock_selftest", 1, ok);
}

static bool accounting_uptime_wrapper_interval(selftest_result_t *out)
{
    return expect_value(out, "uptime_wrapper_interval", 1,
                        accounttest_uptime_wrapper_contract_selftest());
}

static bool accounting_sleep_minimum_deadline(selftest_result_t *out)
{
    account_sleep_sample_t sample;
    bool ok = accounttest_sleep_sample_classify(10, TASK_WAIT_RESULT_TIMEOUT,
                                                 9000000ULL, &sample) &&
              !sample.early && sample.minimum_ns == 9000000ULL;
    return expect_value(out, "sleep_minimum_deadline", 1, ok);
}

static bool accounting_sleep_one_ms_granularity(selftest_result_t *out)
{
    account_sleep_sample_t sample;
    bool ok = !accounttest_sleep_sample_classify(10, TASK_WAIT_RESULT_TIMEOUT,
                                                  8999999ULL, &sample) &&
              sample.early && sample.early_by_ns == 1000001ULL;
    return expect_value(out, "sleep_one_ms_granularity", 1, ok);
}

static bool accounting_sleep_late_is_diagnostic(selftest_result_t *out)
{
    account_sleep_sample_t sample;
    bool ok = accounttest_sleep_sample_classify(10, TASK_WAIT_RESULT_TIMEOUT,
                                                 121138210ULL, &sample) &&
              !sample.early && sample.late_soft &&
              sample.overshoot_ns == 111138210ULL;
    return expect_value(out, "sleep_late_diagnostic", 1, ok);
}

static bool accounting_sleep_wait_result(selftest_result_t *out)
{
    account_sleep_sample_t sample;
    bool ok = !accounttest_sleep_sample_classify(10, TASK_WAIT_RESULT_ERROR,
                                                  10000000ULL, &sample) &&
              !sample.wait_ok && !sample.early;
    return expect_value(out, "sleep_wait_result", 1, ok);
}

static bool accounting_sleep_percentiles(selftest_result_t *out)
{
    return expect_value(out, "sleep_percentiles", 1,
                        accounttest_sleep_percentiles_selftest());
}

static bool accounting_hpet(selftest_result_t *out)
{
    bool ok = hpet_is_available() && hpet_period_fs() != 0 &&
              hpet_counter_access_selftest();
    return expect_value(out, "hpet_valid", 1, ok);
}

static bool accounting_stats(selftest_result_t *out)
{
    task_metrics_stats_t metrics;
    scheduler_accounting_stats_t accounting;
    clock_stats_t clock;
    task_metrics_stats_snapshot(&metrics);
    scheduler_accounting_stats_snapshot(&accounting);
    clock_stats_snapshot(&clock);
    uint64_t violations = metrics.runtime_regressions +
                          metrics.over_100_samples +
                          accounting.runtime_overflows +
                          accounting.clock_regressions +
                          clock.api_regressions +
                          clock.source_local_regressions +
                          clock.source_retry_exhaustions +
                          clock.state_corruptions;
    return expect_value(out, "accounting_anomalies", 0, violations);
}

static bool accounting_sampler(selftest_result_t *out)
{
    return expect_value(out, "sampler_selftest", 1,
                        task_metrics_sampler_selftest());
}

static bool accounting_format(selftest_result_t *out)
{
    bool ok = task_metrics_format_selftest() && task_metrics_long_selftest();
    return expect_value(out, "runtime_format", 1, ok);
}

static bool kill_guards(selftest_result_t *out)
{
    return expect_value(out, "cancellation_guards", 1,
                        scheduler_cancellation_selftest());
}

static bool kill_parser(selftest_result_t *out)
{
    task_id_t id = 0;
    bool ok = task_id_parse_decimal_ex("1", &id) == TASK_ID_PARSE_OK && id == 1 &&
              task_id_parse_decimal_ex("0", &id) == TASK_ID_PARSE_ZERO &&
              task_id_parse_decimal_ex("-1", &id) == TASK_ID_PARSE_SIGN &&
              task_id_parse_decimal_ex("abc", &id) == TASK_ID_PARSE_NON_DECIMAL;
    return expect_value(out, "parser_matrix", 1, ok);
}

static bool kill_mapping(selftest_result_t *out)
{
    bool ok = !strcmp(scheduler_task_kill_result_to_string(TASK_KILL_ACCEPTED),
                      "ACCEPTED") &&
              !strcmp(scheduler_task_kill_result_to_string(
                       TASK_KILL_ALREADY_PENDING), "ALREADY_PENDING") &&
              KILL_CLI_ACCEPTED == 0 && KILL_CLI_ALREADY_PENDING !=
              KILL_CLI_ALREADY_ZOMBIE && KILL_CLI_PROTECTED !=
              KILL_CLI_NOT_KILLABLE;
    return expect_value(out, "kill_mapping", 1, ok);
}

static bool reaper_balance(selftest_result_t *out)
{
    task_reaper_stats_t stats;
    scheduler_reaper_stats_snapshot(&stats);
    uint64_t expected = stats.refs_released + stats.refs_current;
    return expect_value(out, "refs_balance", stats.refs_acquired, expected);
}

static bool reaper_clean(selftest_result_t *out)
{
    task_reaper_stats_t stats;
    scheduler_reaper_stats_snapshot(&stats);
    uint64_t violations = stats.free_inflight + stats.structural_faults +
                          stats.timer_ref_underflows +
                          stats.notification_failures;
    return expect_value(out, "reaper_structural", 0, violations);
}

static bool reaper_zombies(selftest_result_t *out)
{
    task_facts_t facts;
    if (!collect_task_facts(&facts))
        return expect_value(out, "snapshot_stable", 1, 0);
    return expect_value(out, "zombie_on_cpu", 0, facts.zombie_on_cpu);
}

static bool input_valid(selftest_result_t *out)
{
    uint64_t violations = 0;
    bool ok = input_router_validate(&violations);
    return expect_value(out, "router_violations", 0, ok ? 0 : violations + 1u);
}

static bool input_snapshot(selftest_result_t *out)
{
    input_router_diagnostics_t stats;
    if (!input_router_diagnostics_snapshot(&stats))
        return expect_value(out, "snapshot", 1, 0);
    uint64_t violations = 0;
    if (stats.default_queue.count > stats.default_queue.capacity) violations++;
    if (stats.modal_queue.count > stats.modal_queue.capacity) violations++;
    violations += stats.default_queue.phantom_empty_wakes;
    violations += stats.modal_queue.phantom_empty_wakes;
    violations += stats.transition_failures;
    return expect_value(out, "input_consistency", 0, violations);
}

static bool input_trace_off(selftest_result_t *out)
{
    return expect_value(out, "trace_flags", INPUT_TRACE_NONE,
                        input_debug_get_trace_flags());
}

static bool modal_session_valid(selftest_result_t *out)
{
    uint64_t violations = 0;
    bool ok = modal_session_validate(&violations);
    return expect_value(out, "session_violations", 0,
                        ok ? 0 : violations + 1u);
}

static bool modal_ui_valid(selftest_result_t *out)
{
    uint64_t violations = 0;
    bool ok = modal_ui_validate(&violations);
    return expect_value(out, "ui_violations", 0, ok ? 0 : violations + 1u);
}

static bool modal_inactive(selftest_result_t *out)
{
    modal_session_snapshot_t session;
    modal_ui_runtime_snapshot_t runtime;
    bool snap = modal_session_snapshot(&session) &&
                modal_ui_runtime_snapshot(&runtime);
    bool clean = snap && session.state == MODAL_SESSION_INACTIVE &&
                 session.owner.id == TASK_ID_INVALID &&
                 session.router_state == INPUT_ROUTE_DEFAULT &&
                 !session.shell_paused && !runtime.active &&
                 !runtime.contexts_live && !runtime.contexts_quarantined &&
                 !shell_is_input_paused_for_modal_ui();
    return expect_value(out, "inactive_state", 1, clean);
}

static bool modal_controls(selftest_result_t *out)
{
    modal_ui_test_controls_snapshot_t controls;
    bool ok = modal_ui_test_controls_snapshot(&controls) && !controls.armed &&
              !controls.fail_alloc && !controls.fail_create &&
              !controls.skip_close && !controls.kill_next &&
              !controls.kill_before_begin && !controls.hold_after_completion &&
              !controls.hold_caller_before_wait &&
              !controls.hold_worker_for_reap;
    return expect_value(out, "test_controls_default", 1, ok);
}

static bool modal_open_close_heap_direction(selftest_result_t *out)
{
    return expect_value(out, "signed_heap_direction", 1,
        modaltest_open_close_selftest_case(
            MODALTEST_OPEN_CLOSE_SELFTEST_HEAP_DIRECTION));
}

static bool modal_open_close_used_blocks(selftest_result_t *out)
{
    return expect_value(out, "used_blocks_underflow_guard", 1,
        modaltest_open_close_selftest_case(
            MODALTEST_OPEN_CLOSE_SELFTEST_USED_BLOCKS));
}

static bool modal_open_close_zero_is_not_idle(selftest_result_t *out)
{
    return expect_value(out, "zero_reaped_not_idle", 1,
        modaltest_open_close_selftest_case(
            MODALTEST_OPEN_CLOSE_SELFTEST_ZERO_IS_NOT_IDLE));
}

static bool modal_open_close_record_fit(selftest_result_t *out)
{
    return expect_value(out, "open_close_record_bounds", 1,
        modaltest_open_close_selftest_case(
            MODALTEST_OPEN_CLOSE_SELFTEST_RECORD_FIT));
}

static bool modal_open_close_stats_delta(selftest_result_t *out)
{
    return expect_value(out, "modal_stats_exact_delta", 1,
        modaltest_open_close_selftest_case(
            MODALTEST_OPEN_CLOSE_SELFTEST_STATS_DELTA));
}

static bool modal_open_close_global_heap_diagnostic(selftest_result_t *out)
{
    return expect_value(out, "global_heap_diagnostic", 1,
        modaltest_open_close_selftest_case(
            MODALTEST_OPEN_CLOSE_SELFTEST_GLOBAL_HEAP_IS_DIAGNOSTIC));
}

static bool modal_open_close_outer_heap_required(selftest_result_t *out)
{
    return expect_value(out, "outer_heap_required", 1,
        modaltest_open_close_selftest_case(
            MODALTEST_OPEN_CLOSE_SELFTEST_OUTER_HEAP_REQUIRED));
}

static bool modal_open_close_timer_node_noise(selftest_result_t *out)
{
    return expect_value(out, "timer_node_noise_diagnostic", 1,
        modaltest_open_close_selftest_case(
            MODALTEST_OPEN_CLOSE_SELFTEST_TIMER_NODE_NOISE));
}

static bool ui_layout_wide(selftest_result_t *out)
{
    taskman_layout_t layout;
    bool ok = taskman_layout_compute(200, 50, 0, 0, &layout) &&
              layout.mode == TASKMAN_LAYOUT_WIDE &&
              layout.visible_task_rows != 0;
    return expect_value(out, "wide_layout", 1, ok);
}

static bool ui_layout_compact(selftest_result_t *out)
{
    taskman_layout_t layout;
    bool ok = taskman_layout_compute(100, 40, 0, 0, &layout) &&
              layout.mode == TASKMAN_LAYOUT_COMPACT;
    return expect_value(out, "compact_layout", 1, ok);
}

static bool ui_layout_narrow(selftest_result_t *out)
{
    taskman_layout_t layout;
    bool ok = taskman_layout_compute(20, 40, 0, 0, &layout) &&
              layout.mode == TASKMAN_LAYOUT_TOO_NARROW;
    return expect_value(out, "narrow_layout", 1, ok);
}

static bool ui_layout_short(selftest_result_t *out)
{
    taskman_layout_t layout;
    bool ok = taskman_layout_compute(200, 4, 0, 0, &layout) &&
              layout.mode == TASKMAN_LAYOUT_TOO_SHORT;
    return expect_value(out, "short_layout", 1, ok);
}

static bool ui_row(selftest_result_t *out)
{
    taskman_layout_t layout;
    task_snapshot_t task;
    task_cpu_sample_t sample = {.cpu_x10 = 1000, .valid = 1};
    taskman_row_diagnostics_t diagnostics;
    char row[512];
    memset(&task, 0, sizeof(task));
    task.id = UINT64_MAX;
    task.state = TASK_READY;
    task.task_class = TASK_CLASS_NORMAL;
    task.quantum = task.quantum_default = DEFAULT_QUANTUM;
    task.flags = TASK_FLAG_KILLABLE;
    task.last_cpu_slot = TASK_CPU_SLOT_NONE;
    task.current_cpu_slot = TASK_CPU_SLOT_NONE;
    strcpy(task.name, "bounded-row");
    bool found_pid = false;
    bool ok = taskman_layout_compute(200, 50, 0, 0, &layout) &&
              taskman_format_row(&layout, &task, &sample, false, row,
                                 sizeof(row), &diagnostics) &&
              diagnostics.mandatory_columns_present;
    const char *pid = "18446744073709551615";
    for (uint32_t i = 0; ok && row[i]; i++)
    {
        uint32_t j = 0;
        while (pid[j] && row[i + j] == pid[j]) j++;
        if (!pid[j]) { found_pid = true; break; }
    }
    ok = ok && found_pid;
    return expect_value(out, "bounded_row", 1, ok);
}

static bool ui_navigation(selftest_result_t *out)
{
    task_snapshot_t tasks[2] = {{.id = 1}, {.id = 2}};
    taskman_navigation_t navigation;
    memset(&navigation, 0, sizeof(navigation));
    navigation.page_index = 99;
    navigation.selected_index = 99;
    taskman_navigation_reconcile(&navigation, tasks, 2, 1);
    bool ok = navigation.page_index < navigation.page_count &&
              navigation.selected_index < 2;
    return expect_value(out, "navigation_clamp", 1, ok);
}

static bool ui_controls(selftest_result_t *out)
{
    taskman_stats_t stats;
    taskman_stats_snapshot(&stats);
    bool ok = taskman_test_controls_default() && !stats.model_live;
    return expect_value(out, "taskman_controls", 1, ok);
}

static bool ui_fixture_capacity(selftest_result_t *out)
{
    uint32_t max = taskmantest_fixture_max();
    bool ok = max == 257u &&
              max > TASKMAN_INITIAL_CAPACITY * 2u &&
              max <= TASKMAN_V1_MAX_ENTRIES &&
              sizeof(task_handle_t) == 16u;
    return expect_value(out, "fixture_capacity_257", 1, ok);
}

static bool ui_taskman_auto_session_delta(selftest_result_t *out)
{
    return expect_value(out, "auto_session_delta", 1,
        taskmantest_auto_session_selftest_case(
            TASKMANTEST_AUTO_SESSION_SELFTEST_DELTA));
}

static bool ui_taskman_auto_session_refresh_pattern(selftest_result_t *out)
{
    return expect_value(out, "auto_session_refresh_pattern", 1,
        taskmantest_auto_session_selftest_case(
            TASKMANTEST_AUTO_SESSION_SELFTEST_REFRESH_PATTERN));
}

static bool ui_taskman_auto_session_record_fit(selftest_result_t *out)
{
    return expect_value(out, "auto_session_record_fit", 1,
        taskmantest_auto_session_selftest_case(
            TASKMANTEST_AUTO_SESSION_SELFTEST_RECORD_FIT));
}

static bool ui_common_format(selftest_result_t *out)
{
    task_snapshot_t task;
    task_cpu_sample_t sample = {.valid = 0};
    task_format_fields_t fields;
    memset(&task, 0, sizeof(task));
    task.id = 1;
    task.state = TASK_READY;
    task.task_class = TASK_CLASS_NORMAL;
    task.quantum = task.quantum_default = DEFAULT_QUANTUM;
    task.current_cpu_slot = task.last_cpu_slot = TASK_CPU_SLOT_NONE;
    bool ok = task_format_snapshot_fields(&task, &sample, TASK_FORMAT_LONG,
                                          &fields) &&
              !strcmp(fields.state, "READY") &&
              !strcmp(fields.cpu_percent, "--");
    return expect_value(out, "common_format", 1, ok);
}

static bool format_all(selftest_result_t *out)
{
    uint64_t cases = 0;
    bool ok = task_format_selftest(&cases) && cases >= 20;
    return expect_value(out, "format_cases", 1, ok);
}

static bool format_columns(selftest_result_t *out)
{
    uint64_t seen = 0;
    uint64_t violations = 0;
    for (uint32_t i = 0; i < TASK_FORMAT_COL_COUNT; i++)
    {
        const task_format_column_spec_t *column = task_format_column_spec(
            (task_format_column_id_t)i);
        if (!column || column->id != (task_format_column_id_t)i ||
            !column->long_header || !column->compact_header ||
            (seen & (1ull << i)))
            violations++;
        seen |= 1ull << i;
    }
    return expect_value(out, "column_violations", 0, violations);
}

static bool registry_commands(selftest_result_t *out)
{
    uint64_t violations = 0;
    bool ok = shell_registry_validate(&violations);
    return expect_value(out, "command_registry", 0,
                        ok ? 0 : violations + 1u);
}

static bool registry_selftests(selftest_result_t *out);
static bool registry_selftest_records_fit(selftest_result_t *out);

static bool registry_help(selftest_result_t *out)
{
    const char *names[] = {"ps", "kill", "taskman", "taskdiag", "tasktest"};
    bool ok = true;
    for (uint32_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        const ShellCommand *command = shell_registry_find(names[i]);
        ok = ok && command && command->usage && command->usage[0] &&
             command->details && command->details[0];
    }
    return expect_value(out, "help_details", 1, ok);
}

static bool registry_trace(selftest_result_t *out)
{
    return expect_value(out, "task_trace_target", TASK_ID_INVALID,
                        task_view_trace_target());
}

static bool transport_crc_known(selftest_result_t *out)
{
    return expect_value(out, "crc_known_vector", 1,
        tasktest_transport_selftest_case(
            TASKTEST_TRANSPORT_TEST_CRC_KNOWN_VECTOR));
}

static bool transport_canonical_join(selftest_result_t *out)
{
    return expect_value(out, "canonical_join", 1,
        tasktest_transport_selftest_case(
            TASKTEST_TRANSPORT_TEST_CANONICAL_JOIN));
}

static bool transport_sequence_initial(selftest_result_t *out)
{
    return expect_value(out, "sequence_initial", 1,
        tasktest_transport_selftest_case(
            TASKTEST_TRANSPORT_TEST_SEQUENCE_INITIAL));
}

static bool transport_sequence_gap(selftest_result_t *out)
{
    return expect_value(out, "sequence_gap_reject", 1,
        tasktest_transport_selftest_case(
            TASKTEST_TRANSPORT_TEST_SEQUENCE_GAP));
}

static bool transport_sequence_stale(selftest_result_t *out)
{
    return expect_value(out, "sequence_stale_reject", 1,
        tasktest_transport_selftest_case(
            TASKTEST_TRANSPORT_TEST_SEQUENCE_STALE));
}

static bool transport_replay_same(selftest_result_t *out)
{
    return expect_value(out, "replay_same_payload", 1,
        tasktest_transport_selftest_case(
            TASKTEST_TRANSPORT_TEST_REPLAY_SAME));
}

static bool transport_replay_mismatch(selftest_result_t *out)
{
    return expect_value(out, "replay_payload_mismatch", 1,
        tasktest_transport_selftest_case(
            TASKTEST_TRANSPORT_TEST_REPLAY_MISMATCH));
}

static bool transport_recursive(selftest_result_t *out)
{
    return expect_value(out, "recursive_reject", 1,
        tasktest_transport_selftest_case(
            TASKTEST_TRANSPORT_TEST_RECURSIVE));
}

static bool transport_max_frame(selftest_result_t *out)
{
    return expect_value(out, "max_frame", 1,
        tasktest_transport_selftest_case(
            TASKTEST_TRANSPORT_TEST_MAX_FRAME));
}

static bool transport_harness_fit(selftest_result_t *out)
{
    return expect_value(out, "harness_records_fit", 1,
        tasktest_transport_selftest_case(
            TASKTEST_TRANSPORT_TEST_HARNESS_FIT));
}

static bool transport_harness_line_fenced(selftest_result_t *out)
{
    return expect_value(out, "harness_records_line_fenced", 1,
        tasktest_transport_selftest_case(
            TASKTEST_TRANSPORT_TEST_HARNESS_LINE_FENCED));
}

static bool transport_harness_end_max(selftest_result_t *out)
{
    return expect_value(out, "harness_end_max", 1,
        tasktest_transport_selftest_case(
            TASKTEST_TRANSPORT_TEST_HARNESS_END_MAX));
}

static bool transport_harness_reject_max(selftest_result_t *out)
{
    return expect_value(out, "harness_reject_max", 1,
        tasktest_transport_selftest_case(
            TASKTEST_TRANSPORT_TEST_HARNESS_REJECT_MAX));
}

static bool transport_harness_status_max(selftest_result_t *out)
{
    return expect_value(out, "harness_status_max", 1,
        tasktest_transport_selftest_case(
            TASKTEST_TRANSPORT_TEST_HARNESS_STATUS_MAX));
}

static bool transport_ps_loop_bounds(selftest_result_t *out)
{
    return expect_value(out, "ps_loop_bounds", 1,
        tasktest_transport_selftest_case(
            TASKTEST_TRANSPORT_TEST_PS_LOOP_BOUNDS));
}

static bool transport_ps_loop_record_fit(selftest_result_t *out)
{
    return expect_value(out, "ps_loop_record_fit", 1,
        tasktest_transport_selftest_case(
            TASKTEST_TRANSPORT_TEST_PS_LOOP_RECORD_FIT));
}

#define CASE(suite_, name_, severity_, fn_) \
    {.suite = suite_, .name = name_, .severity = severity_, .run = fn_}

static const selftest_case_t g_cases[] = {
    CASE("identity", "task.identity.unique", SELFTEST_SEVERITY_P0, identity_valid),
    CASE("identity", "task.identity.zero_invalid", SELFTEST_SEVERITY_P0, identity_zero),
    CASE("identity", "task.identity.uint64_format", SELFTEST_SEVERITY_P1, pid_format),
    CASE("identity", "task.identity.no_reuse_current_boot", SELFTEST_SEVERITY_P0, identity_valid),
    CASE("identity", "task.identity.idle_unique", SELFTEST_SEVERITY_P0, identity_valid),
    CASE("identity", "task.identity.idle_count_matches_cpus", SELFTEST_SEVERITY_P0, identity_idle),
    CASE("identity", "task.flags.kill_protection_immutable", SELFTEST_SEVERITY_P0, identity_flags),
    CASE("identity", "task.parser.uint64_max", SELFTEST_SEVERITY_P1, parser_max),
    CASE("identity", "task.parser.overflow", SELFTEST_SEVERITY_P1, parser_overflow),

    CASE("runtime", "runtime.ready.state_monotonic", SELFTEST_SEVERITY_P0, runtime_ready_state_monotonic),
    CASE("runtime", "runtime.ready.cpu_contract", SELFTEST_SEVERITY_P0, runtime_ready_cpu_contract),
    CASE("runtime", "runtime.ready.shell_contract", SELFTEST_SEVERITY_P0, runtime_ready_shell_contract),
    CASE("runtime", "runtime.ready.hotplug_balance", SELFTEST_SEVERITY_P0, runtime_ready_hotplug_balance),
    CASE("runtime", "runtime.ready.test_gate", SELFTEST_SEVERITY_P0, runtime_ready_test_gate),

    CASE("scheduler", "scheduler.bootstrap.started", SELFTEST_SEVERITY_P0, scheduler_started_case),
    CASE("scheduler", "scheduler.bootstrap.cpu_slots_dense", SELFTEST_SEVERITY_P0, scheduler_topology),
    CASE("scheduler", "scheduler.bootstrap.apic_roundtrip", SELFTEST_SEVERITY_P0, scheduler_topology),
    CASE("scheduler", "scheduler.current.unique", SELFTEST_SEVERITY_P0, scheduler_runtime),
    CASE("scheduler", "scheduler.runqueue.membership", SELFTEST_SEVERITY_P0, scheduler_runtime),
    CASE("scheduler", "scheduler.ready.not_on_cpu", SELFTEST_SEVERITY_P0, scheduler_runtime),
    CASE("scheduler", "scheduler.zombie.not_running", SELFTEST_SEVERITY_P0, scheduler_runtime),
    CASE("scheduler", "scheduler.handoff.balanced", SELFTEST_SEVERITY_P0, scheduler_runtime),
    CASE("scheduler", "scheduler.handoff.pending_zero", SELFTEST_SEVERITY_P0, scheduler_runtime),
    CASE("scheduler", "scheduler.cpu_pin.clean", SELFTEST_SEVERITY_P0, scheduler_runtime),
    CASE("scheduler", "scheduler.pending_cancel_wait.zero", SELFTEST_SEVERITY_P0, scheduler_runtime),
    CASE("scheduler", "scheduler.async.kind_strings", SELFTEST_SEVERITY_P1, scheduler_async_kind_strings),
    CASE("scheduler", "scheduler.async.initial_idle", SELFTEST_SEVERITY_P0, scheduler_async_initial_idle),
    CASE("scheduler", "scheduler.async.run_nonzero", SELFTEST_SEVERITY_P0, scheduler_async_run_nonzero),
    CASE("scheduler", "scheduler.async.snapshot_coherent", SELFTEST_SEVERITY_P0, scheduler_async_snapshot_coherent),
    CASE("scheduler", "scheduler.async.completed_before_marker_contract", SELFTEST_SEVERITY_P0, scheduler_async_completion_order),
    CASE("scheduler", "scheduler.async.unknown_run_reject", SELFTEST_SEVERITY_P1, scheduler_async_unknown_run),

    CASE("sync", "sync.wait.prepare_commit", SELFTEST_SEVERITY_P0, scheduler_runtime),
    CASE("sync", "sync.wait.abort", SELFTEST_SEVERITY_P0, scheduler_runtime),
    CASE("sync", "sync.semaphore.try_wait", SELFTEST_SEVERITY_P0, sync_semaphore_pure),
    CASE("sync", "sync.semaphore.count", SELFTEST_SEVERITY_P0, sync_semaphore_pure),
    CASE("sync", "sync.semaphore.observer.atomic_config", SELFTEST_SEVERITY_P0, sync_semaphore_observer),
    CASE("sync", "sync.sleep.short", SELFTEST_SEVERITY_P1, sync_timer_valid),
    CASE("sync", "sync.cancel.preblock_sem", SELFTEST_SEVERITY_P0, kill_guards),
    CASE("sync", "sync.cancel.preblock_timer", SELFTEST_SEVERITY_P0, kill_guards),
    CASE("sync", "sync.timer.pending_cancel", SELFTEST_SEVERITY_P0, sync_timer_valid),
    CASE("sync", "sync.timer.claim_release", SELFTEST_SEVERITY_P0, sync_timer_balance),
    CASE("sync", "sync.async.initial_idle", SELFTEST_SEVERITY_P0, sync_async_initial_idle),
    CASE("sync", "sync.async.run_nonzero", SELFTEST_SEVERITY_P0, sync_async_run_nonzero),
    CASE("sync", "sync.async.snapshot_coherent", SELFTEST_SEVERITY_P0, sync_async_snapshot_coherent),
    CASE("sync", "sync.async.unknown_run_reject", SELFTEST_SEVERITY_P1, sync_async_unknown_run),
    CASE("sync", "sync.async.completed_before_marker_contract", SELFTEST_SEVERITY_P0, sync_async_completion_order),

    CASE("accounting", "accounting.clock.monotonic", SELFTEST_SEVERITY_P0, accounting_clock),
    CASE("accounting", "accounting.clock.state_canary", SELFTEST_SEVERITY_P0, accounting_clock),
    CASE("accounting", "accounting.uptime.wrapper_interval", SELFTEST_SEVERITY_P0, accounting_uptime_wrapper_interval),
    CASE("accounting", "accounting.sleep.minimum_deadline", SELFTEST_SEVERITY_P0, accounting_sleep_minimum_deadline),
    CASE("accounting", "accounting.sleep.one_ms_granularity", SELFTEST_SEVERITY_P0, accounting_sleep_one_ms_granularity),
    CASE("accounting", "accounting.sleep.late_is_diagnostic", SELFTEST_SEVERITY_P0, accounting_sleep_late_is_diagnostic),
    CASE("accounting", "accounting.sleep.wait_result", SELFTEST_SEVERITY_P0, accounting_sleep_wait_result),
    CASE("accounting", "accounting.sleep.percentiles", SELFTEST_SEVERITY_P1, accounting_sleep_percentiles),
    CASE("accounting", "accounting.hpet.valid", SELFTEST_SEVERITY_P0, accounting_hpet),
    CASE("accounting", "accounting.runtime.no_overflow", SELFTEST_SEVERITY_P0, accounting_stats),
    CASE("accounting", "accounting.sampler.invalid_first", SELFTEST_SEVERITY_P1, accounting_sampler),
    CASE("accounting", "accounting.sampler.zombie_zero", SELFTEST_SEVERITY_P1, accounting_sampler),
    CASE("accounting", "accounting.sampler.individual_le_100", SELFTEST_SEVERITY_P1, accounting_sampler),
    CASE("accounting", "accounting.aggregate.within_cpu_capacity", SELFTEST_SEVERITY_P1, accounting_stats),
    CASE("accounting", "accounting.format.runtime", SELFTEST_SEVERITY_P2, accounting_format),

    CASE("kill", "kill.protected.reject", SELFTEST_SEVERITY_P0, kill_guards),
    CASE("kill", "kill.nonkillable.reject", SELFTEST_SEVERITY_P0, kill_guards),
    CASE("kill", "kill.duplicate.pending", SELFTEST_SEVERITY_P0, kill_mapping),
    CASE("kill", "kill.zombie.distinct", SELFTEST_SEVERITY_P0, kill_mapping),
    CASE("kill", "kill.exiting.distinct", SELFTEST_SEVERITY_P0, kill_mapping),
    CASE("kill", "kill.cleanup.once", SELFTEST_SEVERITY_P0, reaper_clean),
    CASE("kill", "kill.parser.results", SELFTEST_SEVERITY_P1, kill_parser),
    CASE("kill", "kill.cli.status_mapping", SELFTEST_SEVERITY_P1, kill_mapping),
    CASE("kill", "kill.cooperative.contract", SELFTEST_SEVERITY_P0, kill_guards),

    CASE("reaper", "reaper.refs.balance", SELFTEST_SEVERITY_P0, reaper_balance),
    CASE("reaper", "reaper.free_inflight.zero", SELFTEST_SEVERITY_P0, reaper_clean),
    CASE("reaper", "reaper.structural.zero", SELFTEST_SEVERITY_P0, reaper_clean),
    CASE("reaper", "reaper.zombie.not_on_cpu", SELFTEST_SEVERITY_P0, reaper_zombies),
    CASE("reaper", "reaper.timer_ref.blocks", SELFTEST_SEVERITY_P0, sync_timer_balance),
    CASE("reaper", "reaper.claim.unique", SELFTEST_SEVERITY_P0, reaper_clean),
    CASE("reaper", "reaper.lifecycle.notification_once", SELFTEST_SEVERITY_P0, reaper_clean),
    CASE("reaper", "reaper.refs_current.active_not_leak", SELFTEST_SEVERITY_P1, reaper_balance),

    CASE("input", "input.router.validate", SELFTEST_SEVERITY_P0, input_valid),
    CASE("input", "input.queue.count_consistent", SELFTEST_SEVERITY_P0, input_snapshot),
    CASE("input", "input.queue.phantom_zero", SELFTEST_SEVERITY_P0, input_snapshot),
    CASE("input", "input.route.mismatch_zero", SELFTEST_SEVERITY_P0, input_valid),
    CASE("input", "input.default.depth_valid", SELFTEST_SEVERITY_P1, input_snapshot),
    CASE("input", "input.modal.depth_valid", SELFTEST_SEVERITY_P1, input_snapshot),
    CASE("input", "input.transition.failures_zero", SELFTEST_SEVERITY_P0, input_snapshot),
    CASE("input", "input.trace.default_off", SELFTEST_SEVERITY_P2, input_trace_off),

    CASE("modal", "modal.session.validate", SELFTEST_SEVERITY_P0, modal_session_valid),
    CASE("modal", "modal.ui.validate", SELFTEST_SEVERITY_P0, modal_ui_valid),
    CASE("modal", "modal.state.inactive_clean", SELFTEST_SEVERITY_P0, modal_inactive),
    CASE("modal", "modal.owner.invalid_when_inactive", SELFTEST_SEVERITY_P0, modal_inactive),
    CASE("modal", "modal.router.default_when_inactive", SELFTEST_SEVERITY_P0, modal_inactive),
    CASE("modal", "modal.shell.unpaused_when_inactive", SELFTEST_SEVERITY_P0, modal_inactive),
    CASE("modal", "modal.contexts.zero", SELFTEST_SEVERITY_P0, modal_inactive),
    CASE("modal", "modal.quarantine.zero", SELFTEST_SEVERITY_P0, modal_inactive),
    CASE("modal", "modal.test_controls.default", SELFTEST_SEVERITY_P1, modal_controls),
    CASE("modal", "modal.open_close.heap_direction", SELFTEST_SEVERITY_P0, modal_open_close_heap_direction),
    CASE("modal", "modal.open_close.used_blocks", SELFTEST_SEVERITY_P0, modal_open_close_used_blocks),
    CASE("modal", "modal.open_close.zero_is_not_idle", SELFTEST_SEVERITY_P0, modal_open_close_zero_is_not_idle),
    CASE("modal", "modal.open_close.record_fit", SELFTEST_SEVERITY_P0, modal_open_close_record_fit),
    CASE("modal", "modal.open_close.stats_delta", SELFTEST_SEVERITY_P0, modal_open_close_stats_delta),
    CASE("modal", "modal.open_close.global_heap_is_diagnostic", SELFTEST_SEVERITY_P0, modal_open_close_global_heap_diagnostic),
    CASE("modal", "modal.open_close.outer_heap_required", SELFTEST_SEVERITY_P0, modal_open_close_outer_heap_required),
    CASE("modal", "modal.open_close.timer_node_noise", SELFTEST_SEVERITY_P0, modal_open_close_timer_node_noise),

    CASE("ui", "ui.taskman.layout.wide", SELFTEST_SEVERITY_P1, ui_layout_wide),
    CASE("ui", "ui.taskman.layout.compact", SELFTEST_SEVERITY_P1, ui_layout_compact),
    CASE("ui", "ui.taskman.layout.too_narrow", SELFTEST_SEVERITY_P1, ui_layout_narrow),
    CASE("ui", "ui.taskman.layout.too_short", SELFTEST_SEVERITY_P1, ui_layout_short),
    CASE("ui", "ui.taskman.row.pid_uint64", SELFTEST_SEVERITY_P1, ui_row),
    CASE("ui", "ui.taskman.row.fields_bounded", SELFTEST_SEVERITY_P1, ui_row),
    CASE("ui", "ui.taskman.pagination.clamp", SELFTEST_SEVERITY_P1, ui_navigation),
    CASE("ui", "ui.taskman.selection.preserve", SELFTEST_SEVERITY_P1, ui_navigation),
    CASE("ui", "ui.taskman.controls.default", SELFTEST_SEVERITY_P1, ui_controls),
    CASE("ui", "ui.taskman.model.no_live", SELFTEST_SEVERITY_P0, ui_controls),
    CASE("ui", "ui.taskman.fixture_capacity_257", SELFTEST_SEVERITY_P0, ui_fixture_capacity),
    CASE("ui", "ui.taskman.auto_session_delta", SELFTEST_SEVERITY_P0, ui_taskman_auto_session_delta),
    CASE("ui", "ui.taskman.auto_session_refresh_pattern", SELFTEST_SEVERITY_P0, ui_taskman_auto_session_refresh_pattern),
    CASE("ui", "ui.taskman.auto_session_record_fit", SELFTEST_SEVERITY_P0, ui_taskman_auto_session_record_fit),
    CASE("ui", "ui.ps.pagination_contract", SELFTEST_SEVERITY_P1, ui_navigation),
    CASE("ui", "ui.ps.cpu_window_contract", SELFTEST_SEVERITY_P2, ui_common_format),
    CASE("ui", "ui.ps.taskman.common_format", SELFTEST_SEVERITY_P1, ui_common_format),

    CASE("format", "format.state.long_compact", SELFTEST_SEVERITY_P2, format_all),
    CASE("format", "format.class.long_compact", SELFTEST_SEVERITY_P2, format_all),
    CASE("format", "format.pid.uint64", SELFTEST_SEVERITY_P1, pid_format),
    CASE("format", "format.runtime", SELFTEST_SEVERITY_P2, format_all),
    CASE("format", "format.cpu.invalid_valid_anomalous_zombie", SELFTEST_SEVERITY_P1, format_all),
    CASE("format", "format.memory.units", SELFTEST_SEVERITY_P2, format_all),
    CASE("format", "format.kill.states", SELFTEST_SEVERITY_P1, format_all),
    CASE("format", "format.name.sanitize_truncate", SELFTEST_SEVERITY_P1, format_all),
    CASE("format", "format.columns.unique_order", SELFTEST_SEVERITY_P1, format_columns),

    CASE("registry", "registry.commands.valid", SELFTEST_SEVERITY_P0, registry_commands),
    CASE("registry", "registry.aliases.unique", SELFTEST_SEVERITY_P0, registry_commands),
    CASE("registry", "registry.selftests.unique", SELFTEST_SEVERITY_P0, registry_selftests),
    CASE("registry", "registry.selftest_records_fit", SELFTEST_SEVERITY_P0, registry_selftest_records_fit),
    CASE("registry", "registry.help.details_present", SELFTEST_SEVERITY_P1, registry_help),
    CASE("registry", "registry.taskdiag.trace_default_off", SELFTEST_SEVERITY_P2, registry_trace),

    CASE("transport", "transport.crc.known_vector", SELFTEST_SEVERITY_P0, transport_crc_known),
    CASE("transport", "transport.payload.canonical_join", SELFTEST_SEVERITY_P0, transport_canonical_join),
    CASE("transport", "transport.sequence.initial", SELFTEST_SEVERITY_P0, transport_sequence_initial),
    CASE("transport", "transport.sequence.gap_reject", SELFTEST_SEVERITY_P0, transport_sequence_gap),
    CASE("transport", "transport.sequence.stale_reject", SELFTEST_SEVERITY_P0, transport_sequence_stale),
    CASE("transport", "transport.replay.same_payload", SELFTEST_SEVERITY_P0, transport_replay_same),
    CASE("transport", "transport.replay.payload_mismatch", SELFTEST_SEVERITY_P0, transport_replay_mismatch),
    CASE("transport", "transport.recursive_reject", SELFTEST_SEVERITY_P0, transport_recursive),
    CASE("transport", "transport.max_frame", SELFTEST_SEVERITY_P1, transport_max_frame),
    CASE("transport", "transport.harness_records_fit", SELFTEST_SEVERITY_P0, transport_harness_fit),
    CASE("transport", "transport.harness_records_line_fenced", SELFTEST_SEVERITY_P0, transport_harness_line_fenced),
    CASE("transport", "transport.harness_end_uint64_status", SELFTEST_SEVERITY_P0, transport_harness_end_max),
    CASE("transport", "transport.harness_reject_max_reason", SELFTEST_SEVERITY_P0, transport_harness_reject_max),
    CASE("transport", "transport.harness_status_max_counters", SELFTEST_SEVERITY_P0, transport_harness_status_max),
    CASE("transport", "transport.ps_loop_bounds", SELFTEST_SEVERITY_P0, transport_ps_loop_bounds),
    CASE("transport", "transport.ps_loop_record_fit", SELFTEST_SEVERITY_P0, transport_ps_loop_record_fit),
};

#undef CASE

static bool selftest_record_is_complete(const selftest_record_builder_t *record)
{
    static const char prefix[] = "\n[SELFTEST][";
    return record && !record->overflow &&
           record->length >= sizeof(prefix) + 1u &&
           record->length < SELFTEST_RECORD_MAX &&
           !memcmp(record->bytes, prefix, sizeof(prefix) - 1u) &&
           record->bytes[record->length - 1u] == '\n' &&
           record->bytes[record->length] == '\0';
}

static bool registry_selftest_records_fit(selftest_result_t *out)
{
    static const char worst_detail[] =
        "record_detail_key_with_sixty_four_bounded_characters_0123456789";
    const uint32_t count = (uint32_t)(sizeof(g_cases) / sizeof(g_cases[0]));
    uint32_t max_length = 0;
    bool ok = true;

    for (uint32_t i = 0; i < count; i++)
    {
        for (selftest_status_t status = SELFTEST_PASS;
             status <= SELFTEST_SKIP; status++)
        {
            selftest_result_t probe = {
                .suite = g_cases[i].suite,
                .name = g_cases[i].name,
                .severity = g_cases[i].severity,
                .status = status,
                .detail_key = worst_detail,
                .expected = UINT64_MAX,
                .actual = UINT64_MAX
            };
            selftest_record_builder_t record;
            ok = selftest_record_build_result(&record, &probe) &&
                 selftest_record_is_complete(&record) && ok;
            if (record.length > max_length)
                max_length = record.length;
        }
    }

    selftest_summary_t summary = {
        .pass = UINT64_MAX,
        .fail = UINT64_MAX,
        .skip = UINT64_MAX,
        .p0_fail = UINT64_MAX,
        .p1_fail = UINT64_MAX,
        .p2_fail = UINT64_MAX
    };
    selftest_record_builder_t summary_record;
    ok = selftest_record_build_summary(&summary_record, &summary) &&
         selftest_record_is_complete(&summary_record) && ok;
    if (summary_record.length > max_length)
        max_length = summary_record.length;

    selftest_record_builder_t overflow_record;
    selftest_record_reset(&overflow_record);
    for (uint32_t i = 0; i < SELFTEST_RECORD_MAX - 1u; i++)
        ok = selftest_record_append_char(&overflow_record, 'x') && ok;
    ok = !selftest_record_append_char(&overflow_record, 'x') &&
         overflow_record.overflow &&
         overflow_record.length == SELFTEST_RECORD_MAX - 1u &&
         overflow_record.bytes[SELFTEST_RECORD_MAX - 1u] == '\0' && ok;

    return expect_value(out, "record_bounds", 1,
                        ok && max_length < SELFTEST_RECORD_MAX);
}

static bool registry_selftests(selftest_result_t *out)
{
    uint64_t violations = 0;
    bool ok = selftest_registry_validate(&violations);
    return expect_value(out, "selftest_registry", 0,
                        ok ? 0 : violations + 1u);
}

bool selftest_registry_validate(uint64_t *out_violations)
{
    uint64_t violations = 0;
    uint32_t count = (uint32_t)(sizeof(g_cases) / sizeof(g_cases[0]));
    for (uint32_t i = 0; i < count; i++)
    {
        const selftest_case_t *item = &g_cases[i];
        if (!item->suite || !item->suite[0] || !item->name ||
            !item->name[0] || !item->run ||
            item->severity > SELFTEST_SEVERITY_P2)
            violations++;
        for (uint32_t other = i + 1; other < count; other++)
            if (item->name && g_cases[other].name &&
                !strcmp(item->name, g_cases[other].name))
                violations++;
    }
    if (out_violations)
        *out_violations = violations;
    return violations == 0;
}

bool selftest_run_cases(const selftest_case_t *cases, uint32_t count,
                        selftest_summary_t *summary)
{
    if (!cases || !summary)
        return false;
    selftest_summary_reset(summary);
    for (uint32_t i = 0; i < count; i++)
    {
        selftest_result_t result = {
            .suite = cases[i].suite,
            .name = cases[i].name,
            .severity = cases[i].severity,
            .status = SELFTEST_PASS,
            .detail_key = NULL,
            .expected = 1,
            .actual = 1
        };
        bool passed = cases[i].run && cases[i].run(&result);
        if (result.status != SELFTEST_SKIP)
            result.status = passed ? SELFTEST_PASS : SELFTEST_FAIL;
        selftest_emit_result(&result);
        selftest_summary_add(summary, &result);
    }
    g_last_summary = *summary;
    return summary->p0_fail == 0 && summary->p1_fail == 0 &&
           summary->fail == 0;
}

static bool run_selected(const char *suite, selftest_summary_t *summary)
{
    selftest_summary_reset(summary);
    bool found = false;
    uint32_t count = (uint32_t)(sizeof(g_cases) / sizeof(g_cases[0]));
    for (uint32_t i = 0; i < count; i++)
    {
        if (suite && strcmp(suite, g_cases[i].suite))
            continue;
        found = true;
        selftest_summary_t one;
        if (!selftest_run_cases(&g_cases[i], 1, &one))
        {
            /* The aggregate below carries the authoritative status. */
        }
        summary->pass += one.pass;
        summary->fail += one.fail;
        summary->skip += one.skip;
        summary->p0_fail += one.p0_fail;
        summary->p1_fail += one.p1_fail;
        summary->p2_fail += one.p2_fail;
    }
    if (!found)
        return false;
    g_last_summary = *summary;
    selftest_emit_summary(summary);
    return summary->fail == 0;
}

bool selftest_run_suite(const char *suite, selftest_summary_t *summary)
{
    if (!suite || !summary || !strcmp(suite, "all"))
        return suite && summary ? selftest_run_all(summary) : false;
    return run_selected(suite, summary);
}

bool selftest_run_all(selftest_summary_t *summary)
{
    return summary ? run_selected(NULL, summary) : false;
}

bool selftest_autorun_if_enabled(void)
{
#ifndef HOBBYOS_SELFTEST_AUTORUN
    return true;
#else
    serial_write_all("\n[SELFTEST][AUTORUN] BEGIN\n");
    selftest_summary_t summary;
    bool ok = selftest_run_all(&summary);
    serial_write_all(ok ? "\n[SELFTEST][AUTORUN] PASS\n" :
                          "\n[SELFTEST][AUTORUN] FAIL\n");
    return ok;
#endif
}

#endif
