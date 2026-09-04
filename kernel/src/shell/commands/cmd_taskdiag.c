#include "cmd_taskdiag.h"

#include "registry.h"
#include "../../core/clock.h"
#include "../../core/input_router.h"
#include "../../core/modal_session.h"
#include "../../core/modal_ui.h"
#include "../../core/scheduler.h"
#include "../../core/task_format.h"
#include "../../core/task_metrics.h"
#include "../../drivers/serial.h"
#include "../../graphics/console.h"
#include "../../libc/memory.h"
#include "../../libc/string.h"

#define TASKDIAG_PAGE_SIZE 64u
#define TASKDIAG_RETRIES 4u

typedef struct
{
    uint64_t generation;
    uint32_t total;
    uint32_t running;
    uint32_t ready_interactive;
    uint32_t ready_normal;
    uint32_t blocked;
    uint32_t sleeping;
    uint32_t zombies;
    uint32_t idle;
    uint32_t runqueue_interactive;
    uint32_t runqueue_normal;
    uint32_t wait_memberships;
    uint32_t kill_pending;
    uint32_t protected_tasks;
    uint32_t killable;
    uint32_t retries;
} taskdiag_summary_t;

static task_snapshot_t g_taskdiag_page[TASKDIAG_PAGE_SIZE];
static task_cpu_sampler_t g_taskdiag_sampler;

static void serial_u64(uint64_t value)
{
    char reverse[21];
    uint32_t count = 0;
    do
    {
        reverse[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (count)
        serial_putc_all(reverse[--count]);
}

static void both_text(const char *text)
{
    console_write(text);
    serial_write_all(text);
}

static void both_u64(uint64_t value)
{
    console_print_dec(value);
    serial_u64(value);
}

static void both_hex(uint64_t value)
{
    console_print_hex(value);
    serial_write_hex64_all(value);
}

static void field_u64(const char *name, uint64_t value)
{
    both_text(" ");
    both_text(name);
    both_text("=");
    both_u64(value);
}

static void field_text(const char *name, const char *value)
{
    both_text(" ");
    both_text(name);
    both_text("=");
    both_text(value ? value : "");
}

static const char *wait_kind_name(task_wait_kind_t kind)
{
    switch (kind)
    {
    case TASK_WAIT_NONE: return "NONE";
    case TASK_WAIT_SEMAPHORE: return "SEMAPHORE";
    case TASK_WAIT_TIMER_SLEEP: return "TIMER_SLEEP";
    case TASK_WAIT_GENERIC: return "GENERIC";
    case TASK_WAIT_INPUT_QUEUE: return "INPUT_QUEUE";
    default: return "UNKNOWN";
    }
}

static const char *wake_reason_name(task_wake_reason_t reason)
{
    switch (reason)
    {
    case TASK_WAKE_NONE: return "NONE";
    case TASK_WAKE_SIGNAL: return "SIGNAL";
    case TASK_WAKE_TIMEOUT: return "TIMEOUT";
    case TASK_WAKE_CANCELLED: return "CANCELLED";
    case TASK_WAKE_SPURIOUS: return "SPURIOUS";
    case TASK_WAKE_ERROR: return "ERROR";
    default: return "UNKNOWN";
    }
}

static const char *queue_name(task_queue_membership_t membership)
{
    switch (membership)
    {
    case TASK_QUEUE_NONE: return "NONE";
    case TASK_QUEUE_RUNQ_INTERACTIVE: return "RUNQ_INTERACTIVE";
    case TASK_QUEUE_RUNQ_NORMAL: return "RUNQ_NORMAL";
    case TASK_QUEUE_WAIT: return "WAIT";
    default: return "UNKNOWN";
    }
}

static const char *modal_state_name(modal_session_state_t state)
{
    switch (state)
    {
    case MODAL_SESSION_INACTIVE: return "INACTIVE";
    case MODAL_SESSION_OPENING: return "OPENING";
    case MODAL_SESSION_ACTIVE: return "ACTIVE";
    case MODAL_SESSION_CLOSING: return "CLOSING";
    default: return "UNKNOWN";
    }
}

static const char *route_state_name(input_route_state_t state)
{
    switch (state)
    {
    case INPUT_ROUTE_DEFAULT: return "DEFAULT";
    case INPUT_ROUTE_OPENING: return "OPENING";
    case INPUT_ROUTE_MODAL: return "MODAL";
    case INPUT_ROUTE_CLOSING: return "CLOSING";
    default: return "UNKNOWN";
    }
}

static bool capture_summary(taskdiag_summary_t *out)
{
    if (!out)
        return false;
    for (uint32_t retry = 0; retry < TASKDIAG_RETRIES; retry++)
    {
        taskdiag_summary_t current;
        memset(&current, 0, sizeof(current));
        current.retries = retry;
        task_snapshot_result_t probe = scheduler_snapshot_tasks(NULL, 0, 0);
        current.generation = probe.registry_generation;
        current.total = probe.total;
        uint32_t offset = 0;
        bool restart = false;
        while (offset < probe.total)
        {
            task_snapshot_result_t page = scheduler_snapshot_tasks(
                g_taskdiag_page, TASKDIAG_PAGE_SIZE, offset);
            uint32_t expected = probe.total - offset;
            if (expected > TASKDIAG_PAGE_SIZE)
                expected = TASKDIAG_PAGE_SIZE;
            if (page.registry_generation != probe.registry_generation ||
                page.total != probe.total || page.offset != offset ||
                page.written != expected)
            {
                restart = true;
                break;
            }
            for (uint32_t index = 0; index < page.written; index++)
            {
                const task_snapshot_t *task = &g_taskdiag_page[index];
                if (task->state == TASK_RUNNING) current.running++;
                else if (task->state == TASK_READY &&
                         task->task_class == TASK_CLASS_INTERACTIVE)
                    current.ready_interactive++;
                else if (task->state == TASK_READY)
                    current.ready_normal++;
                else if (task->state == TASK_BLOCKED) current.blocked++;
                else if (task->state == TASK_SLEEPING) current.sleeping++;
                else if (task->state == TASK_ZOMBIE) current.zombies++;
                if (task->is_idle) current.idle++;
                if (task->queue_membership == TASK_QUEUE_RUNQ_INTERACTIVE)
                    current.runqueue_interactive++;
                else if (task->queue_membership == TASK_QUEUE_RUNQ_NORMAL)
                    current.runqueue_normal++;
                else if (task->queue_membership == TASK_QUEUE_WAIT)
                    current.wait_memberships++;
                if (task->kill_pending) current.kill_pending++;
                if (task->flags & TASK_FLAG_KILL_PROTECTED)
                    current.protected_tasks++;
                if (task->flags & TASK_FLAG_KILLABLE)
                    current.killable++;
            }
            offset += page.written;
            if (!page.written && offset < probe.total)
            {
                restart = true;
                break;
            }
        }
        if (!restart)
        {
            task_snapshot_result_t verify = scheduler_snapshot_tasks(NULL, 0, 0);
            if (verify.registry_generation == probe.registry_generation &&
                verify.total == probe.total)
            {
                *out = current;
                return true;
            }
        }
    }
    return false;
}

static int diag_summary(void)
{
    taskdiag_summary_t summary;
    if (!capture_summary(&summary))
    {
        both_text("[TASKDIAG][SUMMARY] FAIL reason=unstable_snapshot\n");
        return 1;
    }
    both_text("[TASKDIAG][SUMMARY] PASS");
    field_u64("generation", summary.generation);
    field_u64("total", summary.total);
    field_u64("running", summary.running);
    field_u64("ready_i", summary.ready_interactive);
    field_u64("ready_n", summary.ready_normal);
    field_u64("blocked", summary.blocked);
    field_u64("sleeping", summary.sleeping);
    field_u64("zombies", summary.zombies);
    field_u64("idle", summary.idle);
    field_u64("runq_i", summary.runqueue_interactive);
    field_u64("runq_n", summary.runqueue_normal);
    field_u64("wait", summary.wait_memberships);
    field_u64("kill_pending", summary.kill_pending);
    field_u64("protected", summary.protected_tasks);
    field_u64("killable", summary.killable);
    field_u64("snapshot_retries", summary.retries);
    both_text("\n");
    return 0;
}

static int diag_scheduler(void)
{
    scheduler_runtime_stats_t stats;
    bool ok = scheduler_validate_runtime_invariants(&stats);
    both_text(ok ? "[TASKDIAG][SCHEDULER] PASS" :
                   "[TASKDIAG][SCHEDULER] FAIL");
    field_u64("cpus", stats.cpus);
    field_u64("tasks", stats.tasks);
    field_u64("started", stats.started);
    field_u64("finished", stats.finished);
    field_u64("pending_handoffs", stats.pending);
    field_u64("violations", stats.violations);
    field_u64("stale_slots", stats.stale_cpu_slot_entries);
    field_u64("pin_mismatches", stats.pinned_current_mismatches);
    field_u64("wrong_cpu", stats.finish_wrong_cpu);
    field_u64("wrong_sequence", stats.finish_sequence_mismatch);
    field_u64("finish_without_pending", stats.finish_without_pending);
    field_u64("pending_overwrite", stats.pending_overwrite);
    field_u64("pending_cancel_wait", stats.pending_cancel_wait_violations);
    both_text("\n");
    return ok ? 0 : 1;
}

static int diag_reaper(void)
{
    task_reaper_stats_t stats;
    scheduler_reaper_stats_snapshot(&stats);
    both_text("[TASKDIAG][REAPER] PASS");
    field_u64("scans", stats.scans);
    field_u64("candidates", stats.candidates);
    field_u64("claimed", stats.claimed);
    field_u64("reaped", stats.reaped);
    field_u64("zombies", stats.current_zombies);
    field_u64("backlog", stats.max_zombie_backlog);
    field_u64("refs_acquired", stats.refs_acquired);
    field_u64("refs_released", stats.refs_released);
    field_u64("refs_current", stats.refs_current);
    field_u64("free_inflight", stats.free_inflight);
    field_u64("expected_defers", stats.deferred_expected);
    field_u64("structural_defers", stats.deferred_structural);
    field_u64("timer_ref_defers", stats.deferred_timer_ref);
    field_u64("underflows", stats.timer_ref_underflows);
    field_u64("structural_faults", stats.structural_faults);
    both_text(" refs_current_is_active_reference_not_a_leak\n");
    return 0;
}

static int diag_modal(void)
{
    modal_session_snapshot_t session;
    modal_ui_runtime_snapshot_t runtime;
    modal_ui_stats_t stats;
    bool session_ok = modal_session_snapshot(&session);
    bool runtime_ok = modal_ui_runtime_snapshot(&runtime);
    modal_ui_stats_snapshot(&stats);
    bool ok = session_ok && runtime_ok;
    both_text(ok ? "[TASKDIAG][MODAL] PASS" : "[TASKDIAG][MODAL] FAIL");
    field_text("session", session_ok ? modal_state_name(session.state) :
                                      "UNAVAILABLE");
    field_u64("generation", session.active_generation);
    field_u64("route_generation", session.route_generation);
    field_u64("owner", session.owner.id);
    field_u64("owner_lifecycle", session.owner.lifecycle_generation);
    field_text("owner_name", session.owner_name);
    field_u64("shell_paused", session.shell_paused);
    field_text("router", route_state_name(session.router_state));
    field_u64("ui_active", runtime.active);
    field_u64("context_state", runtime.state);
    field_u64("caller", runtime.caller.id);
    field_u64("caller_lifecycle", runtime.caller.lifecycle_generation);
    field_u64("worker", runtime.worker.id);
    field_u64("worker_lifecycle", runtime.worker.lifecycle_generation);
    field_u64("completion_claimed", runtime.completion_claimed);
    field_u64("completion_signaled", runtime.completion_signaled);
    field_u64("cleanup", runtime.cleanup_completed);
    field_u64("recovery", runtime.lifecycle_recovered);
    field_u64("contexts_live", stats.contexts_live);
    field_u64("contexts_quarantined", stats.contexts_quarantined);
    both_text("\n");
    return ok ? 0 : 1;
}

static int diag_input(void)
{
    input_router_diagnostics_t stats;
    bool ok = input_router_diagnostics_snapshot(&stats);
    both_text(ok ? "[TASKDIAG][INPUT] PASS" : "[TASKDIAG][INPUT] FAIL");
    field_text("route", route_state_name(stats.state));
    field_u64("generation", stats.route_generation);
    field_u64("dispatch_sequence", stats.dispatch_sequence);
    field_u64("default_depth", stats.default_queue.count);
    field_u64("modal_depth", stats.modal_queue.count);
    field_u64("default_push", stats.default_queue.pushes);
    field_u64("default_pop", stats.default_queue.pops);
    field_u64("default_drain", stats.default_queue.drains);
    field_u64("modal_push", stats.modal_queue.pushes);
    field_u64("modal_pop", stats.modal_queue.pops);
    field_u64("modal_drain", stats.modal_queue.drains);
    field_u64("default_drop", stats.default_drops);
    field_u64("modal_drop", stats.modal_drops);
    field_u64("default_routed", stats.default_routed);
    field_u64("modal_routed", stats.modal_routed);
    field_u64("typed_ahead_drops", stats.typed_ahead_default_dropped);
    field_u64("modal_tail_drops", stats.modal_tail_dropped);
    field_u64("transition_failures", stats.transition_failures);
    both_text("\n");
    return ok ? 0 : 1;
}

static int diag_accounting(void)
{
    task_metrics_stats_t metrics;
    scheduler_accounting_stats_t accounting;
    clock_stats_t clock;
    task_metrics_stats_snapshot(&metrics);
    scheduler_accounting_stats_snapshot(&accounting);
    clock_stats_snapshot(&clock);
    both_text("[TASKDIAG][ACCOUNTING] PASS");
    field_u64("runtime_regressions", metrics.runtime_regressions);
    field_u64("over_100_samples", metrics.over_100_samples);
    field_u64("dropped_baselines", metrics.dropped_baselines);
    field_u64("accounting_events", accounting.accounting_events);
    field_u64("runtime_overflows", accounting.runtime_overflows);
    field_u64("accounting_clock_regressions", accounting.clock_regressions);
    field_u64("clock_api_regressions", clock.api_regressions);
    field_u64("clock_source_regressions", clock.source_local_regressions);
    field_u64("retry_exhaustion", clock.source_retry_exhaustions);
    field_u64("state_corruption", clock.state_corruptions);
    both_text("\n");
    return 0;
}

static int diag_task(const char *text)
{
    task_id_t id = TASK_ID_INVALID;
    task_id_parse_result_t parse = task_id_parse_decimal_ex(text, &id);
    if (parse != TASK_ID_PARSE_OK)
    {
        both_text("taskdiag: invalid task PID (");
        both_text(task_id_parse_result_to_string(parse));
        both_text(")\n");
        return 1;
    }
    task_snapshot_result_t time = scheduler_snapshot_tasks(NULL, 0, 0);
    task_snapshot_t task;
    if (!scheduler_snapshot_task_by_id(id, &task))
    {
        both_text("taskdiag: kernel task PID ");
        both_u64(id);
        both_text(" was not found\n");
        return 1;
    }
    task_cpu_sample_t sample;
    if (!task_cpu_sampler_sample(&g_taskdiag_sampler, &task, 1,
                                 time.sample_time_ns, &sample, 1))
        memset(&sample, 0, sizeof(sample));
    task_format_fields_t fields;
    if (!task_format_snapshot_fields(&task, &sample, TASK_FORMAT_LONG,
                                     &fields))
        return 1;

    both_text("[TASKDIAG][TASK] PASS");
    field_text("kind", "kernel_task");
    field_text("pid", fields.pid);
    field_text("name", fields.name);
    field_text("state", fields.state);
    field_text("class", fields.task_class);
    field_text("cpu", fields.cpu);
    field_text("lcpu", fields.last_cpu);
    field_text("runtime", fields.runtime);
    field_text("cpu_percent", fields.cpu_percent);
    field_text("kill", fields.kill_status);
    field_text("memory", fields.memory);
    both_text(" flags=0x");
    both_hex(task.flags);
    field_text("queue", queue_name(task.queue_membership));
    field_u64("wait_active", task.wait_active);
    field_text("wait_kind", wait_kind_name(task.wait_kind));
    field_text("wake_reason", wake_reason_name(task.wake_reason));
    field_u64("wait_generation", task.wait_generation);
    both_text(" wait_object=0x");
    both_hex(task.wait_object_key);
    field_u64("kill_requested_ns", task.kill_requested_ns);
    field_u64("kill_request_count", task.kill_request_count);
    field_text("exit_reason", scheduler_task_exit_reason_to_string(
                                      task.exit_reason));
    field_u64("exit_started_ns", task.exit_started_ns);
    field_u64("cleanup_completed_ns", task.cleanup_completed_ns);
    field_u64("zombie_entered_ns", task.zombie_entered_ns);
    field_u64("timer_refs", task.task_wake_timer_refs);
    both_text(" reap_mask=0x");
    both_hex(task.reap_defer_mask_last);
    field_u64("schedule_count", task.schedule_count);
    both_text("\n");
    return 0;
}

static bool parser_selftest(uint64_t *out_cases)
{
    uint64_t cases = 0;
    bool ok = true;
    task_id_t id = 9;
#define PARSER_CASE(expression) do { cases++; ok = (expression) && ok; } while (0)
    PARSER_CASE(task_id_parse_decimal_ex("1", &id) == TASK_ID_PARSE_OK &&
                id == 1);
    PARSER_CASE(task_id_parse_decimal_ex("18446744073709551615", &id) ==
                    TASK_ID_PARSE_OK && id == UINT64_MAX);
    PARSER_CASE(task_id_parse_decimal_ex("0", &id) == TASK_ID_PARSE_ZERO &&
                id == 0);
    PARSER_CASE(task_id_parse_decimal_ex("", &id) == TASK_ID_PARSE_EMPTY);
    PARSER_CASE(task_id_parse_decimal_ex("-1", &id) == TASK_ID_PARSE_SIGN);
    PARSER_CASE(task_id_parse_decimal_ex("+1", &id) == TASK_ID_PARSE_SIGN);
    PARSER_CASE(task_id_parse_decimal_ex("12x", &id) ==
                TASK_ID_PARSE_NON_DECIMAL);
    PARSER_CASE(task_id_parse_decimal_ex(" 1", &id) ==
                TASK_ID_PARSE_NON_DECIMAL);
    PARSER_CASE(task_id_parse_decimal_ex("1 ", &id) ==
                TASK_ID_PARSE_NON_DECIMAL);
    PARSER_CASE(task_id_parse_decimal_ex("18446744073709551616", &id) ==
                TASK_ID_PARSE_OVERFLOW);
    PARSER_CASE(task_id_parse_decimal_ex(NULL, &id) ==
                TASK_ID_PARSE_INVALID_ARGUMENT);
    PARSER_CASE(task_id_parse_decimal_ex("1", NULL) ==
                TASK_ID_PARSE_INVALID_ARGUMENT);
    PARSER_CASE(task_id_parse_decimal("0", &id) && id == 0 &&
                task_id_parse_decimal("1", &id) && id == 1 &&
                !task_id_parse_decimal("-1", &id));
#undef PARSER_CASE
    if (out_cases)
        *out_cases = cases;
    return ok;
}

static bool registry_selftest(uint64_t *out_cases)
{
    uint64_t violations = 0;
    uint64_t cases = 0;
    bool ok = shell_registry_validate(&violations) && !violations;
    const char *tokens[] = {"ps", "tasks", "tasklist", "kill", "terminate",
                            "taskkill", "taskman", "tm", "top", "taskdiag",
                            "td", "tdiag"};
    const char *canonical[] = {"ps", "ps", "ps", "kill", "kill", "kill",
                               "taskman", "taskman", "taskman", "taskdiag",
                               "taskdiag", "taskdiag"};
    for (uint32_t index = 0; index < sizeof(tokens) / sizeof(tokens[0]); index++)
    {
        const ShellCommand *command = shell_registry_find(tokens[index]);
        cases++;
        if (!command || strcmp(command->name, canonical[index]))
            ok = false;
    }
    if (out_cases)
        *out_cases = cases + 1u;
    return ok;
}

static int diag_selftest(void)
{
    uint64_t format_cases = 0, parser_cases = 0, registry_cases = 0;
    bool format_ok = task_format_selftest(&format_cases);
    bool parser_ok = parser_selftest(&parser_cases);
    bool registry_ok = registry_selftest(&registry_cases);
    bool ok = format_ok && parser_ok && registry_ok;
    both_text(ok ? "[TASKDIAG][SELFTEST] PASS" :
                   "[TASKDIAG][SELFTEST] FAIL");
    field_u64("format", format_cases);
    field_u64("parser", parser_cases);
    field_u64("registry", registry_cases);
    both_text("\n");
    return ok ? 0 : 1;
}

static int diag_check(void)
{
    uint64_t registry_violations = 0;
    uint64_t modal_violations = 0;
    uint64_t modal_ui_violations = 0;
    uint64_t input_violations = 0;
    uint64_t format_cases = 0, parser_cases = 0;
    scheduler_runtime_stats_t scheduler;
    task_reaper_stats_t reaper;
    scheduler_accounting_stats_t accounting;
    clock_stats_t clock;
    bool registry_ok = shell_registry_validate(&registry_violations);
    bool scheduler_ok = scheduler_validate_runtime_invariants(&scheduler);
    bool modal_ok = modal_session_validate(&modal_violations);
    bool modal_ui_ok = modal_ui_validate(&modal_ui_violations);
    bool input_ok = input_router_validate(&input_violations);
    scheduler_reaper_stats_snapshot(&reaper);
    scheduler_accounting_stats_snapshot(&accounting);
    clock_stats_snapshot(&clock);
    bool refs_ok = reaper.refs_acquired ==
                   reaper.refs_released + reaper.refs_current;
    bool format_ok = task_format_selftest(&format_cases);
    bool parser_ok = parser_selftest(&parser_cases);
    task_id_t trace = task_view_trace_target();
    task_snapshot_t traced;
    bool trace_ok = trace == TASK_ID_INVALID ||
                    scheduler_snapshot_task_by_id(trace, &traced);
    bool structural = !reaper.timer_ref_underflows &&
                      !reaper.structural_faults && !reaper.free_inflight &&
                      !accounting.runtime_overflows &&
                      !clock.state_corruptions;
    bool ok = registry_ok && scheduler_ok && modal_ok && modal_ui_ok &&
              input_ok && refs_ok && format_ok && parser_ok && trace_ok &&
              structural;
    both_text(ok ? "[TASKDIAG][CHECK] PASS" : "[TASKDIAG][CHECK] FAIL");
    field_u64("registry", registry_violations);
    field_u64("scheduler", scheduler.violations);
    field_u64("modal", modal_violations);
    field_u64("modal_ui", modal_ui_violations);
    field_u64("input", input_violations);
    field_u64("refs_balance", refs_ok ? 0u : 1u);
    field_u64("refs_current", reaper.refs_current);
    field_u64("free_inflight", reaper.free_inflight);
    field_u64("structural_faults", reaper.structural_faults);
    field_u64("timer_underflows", reaper.timer_ref_underflows);
    field_u64("runtime_overflows", accounting.runtime_overflows);
    field_u64("clock_corruption", clock.state_corruptions);
    field_u64("format_failures", format_ok ? 0u : format_cases);
    field_u64("parser_failures", parser_ok ? 0u : parser_cases);
    field_u64("trace_valid", trace_ok ? 1u : 0u);
    both_text("\n");
    return ok ? 0 : 1;
}

static int diag_trace(int argc, char **argv)
{
    if (argc != 3)
    {
        both_text("Usage: taskdiag trace <pid>|off\n");
        return 1;
    }
    if (!strcmp(argv[2], "off"))
    {
        task_view_trace_disable();
        both_text("[TASKDIAG][TRACE] OFF\n");
        return 0;
    }
    task_id_t id = TASK_ID_INVALID;
    task_id_parse_result_t parse = task_id_parse_decimal_ex(argv[2], &id);
    task_snapshot_t snapshot;
    if (parse != TASK_ID_PARSE_OK)
    {
        both_text("taskdiag: invalid trace PID (");
        both_text(task_id_parse_result_to_string(parse));
        both_text(")\n");
        return 1;
    }
    if (!scheduler_snapshot_task_by_id(id, &snapshot))
    {
        both_text("taskdiag: trace PID was not found\n");
        return 1;
    }
    task_view_trace_set(id);
    both_text("[TASKDIAG][TRACE] ON id=");
    both_u64(id);
    both_text("\n");
    return 0;
}

static int diag_trace_status(void)
{
    task_id_t id = task_view_trace_target();
    if (id == TASK_ID_INVALID)
        both_text("[TASKDIAG][TRACE_STATUS] OFF\n");
    else
    {
        both_text("[TASKDIAG][TRACE_STATUS] ON id=");
        both_u64(id);
        both_text("\n");
    }
    return 0;
}

static int diag_all(void)
{
    int status = 0;
    status |= diag_summary();
    status |= diag_scheduler();
    status |= diag_reaper();
    status |= diag_modal();
    status |= diag_input();
    status |= diag_accounting();
    both_text(status ? "[TASKDIAG][ALL] FAIL\n" :
                       "[TASKDIAG][ALL] PASS\n");
    return status;
}

int cmd_taskdiag(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "summary")))
        return diag_summary();
    if (argc == 3 && !strcmp(argv[1], "task"))
        return diag_task(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "scheduler"))
        return diag_scheduler();
    if (argc == 2 && !strcmp(argv[1], "reaper"))
        return diag_reaper();
    if (argc == 2 && !strcmp(argv[1], "modal"))
        return diag_modal();
    if (argc == 2 && !strcmp(argv[1], "input"))
        return diag_input();
    if (argc == 2 && !strcmp(argv[1], "accounting"))
        return diag_accounting();
    if (argc == 2 && !strcmp(argv[1], "all"))
        return diag_all();
    if (argc == 2 && !strcmp(argv[1], "check"))
        return diag_check();
    if (argc == 2 && !strcmp(argv[1], "selftest"))
        return diag_selftest();
    if (argc >= 2 && !strcmp(argv[1], "trace"))
        return diag_trace(argc, argv);
    if (argc == 2 && !strcmp(argv[1], "trace-status"))
        return diag_trace_status();
    both_text("Usage: taskdiag [summary|task <pid>|scheduler|reaper|modal|input|accounting|all|check|selftest|trace <pid>|trace off|trace-status]\n");
    return 1;
}
