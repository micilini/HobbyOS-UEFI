#include "diagnostic_result.h"

#include "shell.h"
#include "../graphics/console.h"
#include "../drivers/serial.h"
#include "../libc/string.h"

#ifdef HOBBYOS_SELFTEST
#include "../core/scheduler.h"
#include "../drivers/timer.h"
#endif

static const char *diagnostic_result_prefix(diagnostic_result_state_t state)
{
    switch (state)
    {
    case DIAGNOSTIC_RESULT_RUNNING: return "[RUNNING] ";
    case DIAGNOSTIC_RESULT_PASS: return "[PASS] ";
    case DIAGNOSTIC_RESULT_FAIL: return "[FAIL] ";
    default: return "[FAIL] ";
    }
}

static uint32_t diagnostic_result_text_length(const char *text)
{
    uint32_t length = 0;
    if (!text)
        return 0;
    while (text[length] && length < UINT32_MAX)
        length++;
    return length;
}

static void diagnostic_result_append(char *line, uint32_t *length,
                                     uint32_t limit, const char *text)
{
    if (!line || !length || !text)
        return;
    while (*text && *length < limit)
    {
        char value = *text++;
        if (value == '\n' || value == '\r' || value == '\t')
            value = ' ';
        line[(*length)++] = value;
    }
}

static void diagnostic_result_append_name(char *line, uint32_t *length,
                                          uint32_t limit,
                                          const char *canonical_command,
                                          uint32_t available)
{
    const char *name = canonical_command && canonical_command[0]
        ? canonical_command : "diagnostic";
    uint32_t name_length = diagnostic_result_text_length(name);
    if (available > limit - *length)
        available = limit - *length;
    if (name_length <= available)
    {
        diagnostic_result_append(line, length, limit, name);
        return;
    }
    if (available <= 3u)
    {
        for (uint32_t index = 0; index < available; index++)
            line[(*length)++] = '.';
        return;
    }
    uint32_t prefix_length = available - 3u;
    for (uint32_t index = 0; index < prefix_length; index++)
    {
        char value = name[index];
        if (value == '\n' || value == '\r' || value == '\t')
            value = ' ';
        line[(*length)++] = value;
    }
    diagnostic_result_append(line, length, limit, "...");
}

static bool diagnostic_result_build(char line[DIAGNOSTIC_RESULT_MAX],
                                    diagnostic_result_state_t state,
                                    const char *canonical_command,
                                    bool newline)
{
    static const char fail_suffix[] = " - details on serial";
    if (!line)
        return false;

    uint32_t limit = DIAGNOSTIC_RESULT_MAX - 1u - (newline ? 1u : 0u);
    uint32_t console_columns = console_get_max_cols();
    if (console_columns && console_columns < limit)
        limit = console_columns;

    const char *prefix = diagnostic_result_prefix(state);
    uint32_t prefix_length = diagnostic_result_text_length(prefix);
    uint32_t suffix_length = state == DIAGNOSTIC_RESULT_FAIL
        ? (uint32_t)(sizeof(fail_suffix) - 1u) : 0u;
    uint32_t length = 0;

    if (prefix_length > limit)
        prefix_length = limit;
    for (uint32_t index = 0; index < prefix_length; index++)
        line[length++] = prefix[index];

    uint32_t remaining = limit - length;
    uint32_t reserved_suffix = suffix_length < remaining
        ? suffix_length : 0u;
    diagnostic_result_append_name(line, &length, limit, canonical_command,
                                  remaining - reserved_suffix);
    if (reserved_suffix)
        diagnostic_result_append(line, &length, limit, fail_suffix);

    if (newline)
        line[length++] = '\n';
    line[length] = '\0';
    return length != 0;
}

int diagnostic_result_complete(const char *canonical_command,
                               int return_code)
{
    char line[DIAGNOSTIC_RESULT_MAX];
    diagnostic_result_state_t state = return_code == 0
        ? DIAGNOSTIC_RESULT_PASS : DIAGNOSTIC_RESULT_FAIL;
    if (diagnostic_result_build(line, state, canonical_command, true))
        console_write(line);
    return return_code;
}

uint64_t diagnostic_result_async_started(const char *canonical_command)
{
    char line[DIAGNOSTIC_RESULT_MAX];
    if (!diagnostic_result_build(line, DIAGNOSTIC_RESULT_RUNNING,
                                 canonical_command, false))
        return 0;
    return shell_status_begin(line);
}

void diagnostic_result_async_completed(const char *canonical_command,
                                       uint64_t generation,
                                       bool passed)
{
    char line[DIAGNOSTIC_RESULT_MAX];
    diagnostic_result_state_t state = passed ? DIAGNOSTIC_RESULT_PASS
                                             : DIAGNOSTIC_RESULT_FAIL;
    if (diagnostic_result_build(line, state, canonical_command, false))
        (void)shell_status_complete(generation, line);
}

#ifdef HOBBYOS_SELFTEST
static volatile uint8_t g_diagnostic_result_modal_timer_armed;

static void diagnostic_result_selftest_emit(const char *name, bool passed)
{
    char record[64];
    uint32_t length = 0;
    const char *prefix = "[SHELLRESULTTEST][";
    const char *suffix = passed ? "] PASS\n" : "] FAIL\n";
    while (*prefix && length < sizeof(record) - 1u)
        record[length++] = *prefix++;
    while (name && *name && length < sizeof(record) - 1u)
        record[length++] = *name++;
    while (*suffix && length < sizeof(record) - 1u)
        record[length++] = *suffix++;
    record[length] = '\0';
    serial_write_all(record);
}

static void diagnostic_result_modal_timer_worker(void *arg)
{
    (void)arg;
    uint64_t started_ms = timer_get_uptime_ms();
    bool modal_seen = false;

    while (timer_get_uptime_ms() - started_ms < 30000u)
    {
        if (shell_is_input_paused_for_modal_ui())
        {
            modal_seen = true;
            break;
        }
        timer_sleep(1);
    }

    int rc = modal_seen
        ? shell_execute_command_line_for_selftest("synctest timer-cancel")
        : 1;
    diagnostic_result_selftest_emit("MODAL_TIMER_START",
                                    modal_seen && rc == 0);
    __atomic_store_n(&g_diagnostic_result_modal_timer_armed, 0,
                     __ATOMIC_RELEASE);
}

static int diagnostic_result_arm_modal_timer(void)
{
    if (__atomic_exchange_n(&g_diagnostic_result_modal_timer_armed, 1,
                            __ATOMIC_ACQ_REL))
    {
        diagnostic_result_selftest_emit("MODAL_TIMER_ARM", false);
        return 1;
    }

    bool created = thread_create_named(diagnostic_result_modal_timer_worker,
                                       NULL,
                                       "shell-result-modal");
    if (!created)
        __atomic_store_n(&g_diagnostic_result_modal_timer_armed, 0,
                         __ATOMIC_RELEASE);
    diagnostic_result_selftest_emit("MODAL_TIMER_ARM", created);
    return created ? 0 : 1;
}

static bool diagnostic_result_ends_with(const char *text, const char *suffix)
{
    uint32_t text_length = diagnostic_result_text_length(text);
    uint32_t suffix_length = diagnostic_result_text_length(suffix);
    if (suffix_length > text_length)
        return false;
    return strcmp(text + text_length - suffix_length, suffix) == 0;
}

int cmd_diagnosticresulttest(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "arm-modal-timer-cancel") == 0)
        return diagnostic_result_arm_modal_timer();

    if (argc != 2 || strcmp(argv[1], "all") != 0)
    {
        console_write(
            "usage: diagnosticresulttest all|arm-modal-timer-cancel\n");
        return 1;
    }

    shell_test_status_reset();

    int sync_pass_rc = diagnostic_result_complete(
        "diagnosticresulttest sync-pass", 0);
    bool sync_pass = sync_pass_rc == 0 &&
        console_test_history_contains(
            "[PASS] diagnosticresulttest sync-pass");
    diagnostic_result_selftest_emit("SYNC_PASS", sync_pass);

    int sync_fail_rc = diagnostic_result_complete(
        "diagnosticresulttest sync-fail", 1);
    int sync_exact_rc = diagnostic_result_complete(
        "diagnosticresulttest return-code", 64);
    bool sync_fail = sync_fail_rc == 1 && sync_exact_rc == 64 &&
        console_test_history_contains(
            "[FAIL] diagnosticresulttest sync-fail - details on serial") &&
        console_test_history_contains(
            "[FAIL] diagnosticresulttest return-code - details on serial");
    diagnostic_result_selftest_emit("SYNC_FAIL", sync_fail);

    shell_test_status_reset();
    shell_test_status_set_runtime(true, false);
    uint64_t running_generation = diagnostic_result_async_started(
        "diagnosticresulttest async-running");
    bool async_running = running_generation != 0 &&
        console_test_status_equals(
            "[RUNNING] diagnosticresulttest async-running");
    diagnostic_result_selftest_emit("ASYNC_RUNNING", async_running);

    diagnostic_result_async_completed(
        "diagnosticresulttest async-running", running_generation, true);
    bool async_pass = console_test_status_equals(
        "[PASS] diagnosticresulttest async-running");

    shell_test_status_reset();
    shell_test_status_set_runtime(false, false);
    uint64_t fast_generation = diagnostic_result_async_started(
        "diagnosticresulttest fast");
    diagnostic_result_async_completed(
        "diagnosticresulttest fast", fast_generation, true);
    shell_status_snapshot_t fast_before;
    bool stale_rejected = !shell_status_complete(
        fast_generation, "[RUNNING] diagnosticresulttest stale");
    async_pass = async_pass && shell_status_snapshot(&fast_before) &&
        fast_before.pending && !fast_before.visible && fast_before.terminal &&
        stale_rejected &&
        strcmp(fast_before.message,
               "[PASS] diagnosticresulttest fast") == 0;
    shell_test_status_set_runtime(true, false);
    async_pass = async_pass && console_test_status_equals(
        "[PASS] diagnosticresulttest fast");
    diagnostic_result_selftest_emit("ASYNC_PASS", async_pass);

    shell_test_status_reset();
    shell_test_status_set_runtime(true, false);
    uint64_t fail_generation = diagnostic_result_async_started(
        "diagnosticresulttest async-fail");
    diagnostic_result_async_completed(
        "diagnosticresulttest async-fail", fail_generation, false);
    bool async_fail = fail_generation != 0 &&
        console_test_status_equals(
            "[FAIL] diagnosticresulttest async-fail - details on serial");
    diagnostic_result_selftest_emit("ASYNC_FAIL", async_fail);

    shell_test_status_reset();
    shell_test_status_set_runtime(true, true);
    uint64_t modal_generation = diagnostic_result_async_started(
        "diagnosticresulttest modal");
    diagnostic_result_async_completed(
        "diagnosticresulttest modal", modal_generation, true);
    bool modal_stale_rejected = !shell_status_complete(
        modal_generation, "[RUNNING] diagnosticresulttest modal-old");
    shell_status_snapshot_t modal_before;
    bool modal_defer = shell_status_snapshot(&modal_before) &&
        modal_before.pending && !modal_before.visible &&
        modal_stale_rejected && !console_test_status_visible();
    shell_test_status_set_runtime(true, false);
    shell_status_snapshot_t modal_after;
    modal_defer = modal_defer && shell_status_snapshot(&modal_after) &&
        !modal_after.pending && modal_after.visible && modal_after.flushed &&
        console_test_status_equals("[PASS] diagnosticresulttest modal");
    diagnostic_result_selftest_emit("MODAL_DEFER", modal_defer);

    shell_test_status_reset();
    shell_test_status_set_runtime(true, false);
    bool input_set = shell_test_status_set_input("echo abc", 4);
    uint32_t cursor_before_x = 0, cursor_before_y = 0;
    uint32_t cursor_after_x = 0, cursor_after_y = 0;
    console_get_cursor(&cursor_before_x, &cursor_before_y);
    uint64_t prompt_generation = diagnostic_result_async_started(
        "diagnosticresulttest prompt");
    diagnostic_result_async_completed(
        "diagnosticresulttest prompt", prompt_generation, true);
    console_get_cursor(&cursor_after_x, &cursor_after_y);
    bool prompt_preserve = input_set &&
        shell_test_status_input_equals("echo abc", 4) &&
        cursor_before_x == cursor_after_x &&
        cursor_before_y == cursor_after_y;
    shell_clear_status_on_input();
    console_get_cursor(&cursor_after_x, &cursor_after_y);
    prompt_preserve = prompt_preserve &&
        shell_test_status_input_equals("echo abc", 4) &&
        cursor_before_x == cursor_after_x &&
        cursor_before_y == cursor_after_y &&
        !console_test_status_visible();
    diagnostic_result_selftest_emit("PROMPT_PRESERVE", prompt_preserve);

    char long_name[DIAGNOSTIC_RESULT_MAX * 2u];
    for (uint32_t index = 0; index < sizeof(long_name) - 1u; index++)
        long_name[index] = 'x';
    long_name[sizeof(long_name) - 1u] = '\0';
    char bounded_line[DIAGNOSTIC_RESULT_MAX];
    bool bounded = diagnostic_result_build(
        bounded_line, DIAGNOSTIC_RESULT_FAIL, long_name, false) &&
        diagnostic_result_text_length(bounded_line) <
            DIAGNOSTIC_RESULT_MAX &&
        diagnostic_result_ends_with(bounded_line,
                                    " - details on serial");
    diagnostic_result_selftest_emit("BOUNDED", bounded);

    bool all = sync_pass && sync_fail && async_running && async_pass &&
        async_fail && modal_defer && prompt_preserve && bounded;
    diagnostic_result_selftest_emit("ALL", all);

    shell_test_status_reset();
    shell_test_status_set_runtime(false, false);
    return all ? 0 : 1;
}
#endif
