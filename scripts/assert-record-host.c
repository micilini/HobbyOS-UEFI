#include "../kernel/src/core/assert_selftest.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum
{
    RECORD_EXECUTE = 1,
    RECORD_COMPLETE = 2,
    RECORD_PREPARED = 3,
    RECORD_CONTEXT_READY = 4,
    RECORD_CAPACITY = 96,
    GUARD_SIZE = 16,
    CAPTURE_CAPACITY = 1024
};

int assertion_test_format_record_for_test(
    char *record,
    size_t record_capacity,
    uint32_t kind,
    uint32_t scenario,
    uint32_t slot,
    uint32_t cpu_id,
    uint32_t value);
bool assertion_test_emit_record_for_test(
    size_t record_capacity,
    uint32_t kind,
    uint32_t scenario,
    uint32_t slot,
    uint32_t cpu_id,
    uint32_t value);

typedef struct test_result
{
    unsigned int cases;
    unsigned int assertions;
    unsigned int failures;
} test_result_t;

static test_result_t test_result;
static char serial_capture[CAPTURE_CAPACITY];
static size_t serial_length;
static unsigned int serial_write_calls;
static unsigned int serial_putc_calls;
static unsigned int serial_operations;
static bool inject_writer;
static bool writer_injected;

static bool check_at(bool condition, unsigned int line, const char *expression)
{
    test_result.assertions++;
    if (!condition)
    {
        test_result.failures++;
        printf("[ASSERT_RECORD_HOST][ASSERT] status=FAIL line=%u expression=%s\n",
               line, expression);
    }
    return condition;
}

#define check(condition) check_at((condition), __LINE__, #condition)

static void finish_case(const char *name, unsigned int failures_before)
{
    test_result.cases++;
    printf("[ASSERT_RECORD_HOST][CASE] id=%s status=%s\n", name,
           test_result.failures == failures_before ? "PASS" : "FAIL");
}

static bool bytes_are(
    const unsigned char *bytes,
    size_t count,
    unsigned char expected)
{
    for (size_t index = 0; index < count; index++)
        if (bytes[index] != expected)
            return false;
    return true;
}

static void capture_append(const char *text, size_t count)
{
    if (count >= sizeof(serial_capture) - serial_length)
    {
        test_result.failures++;
        return;
    }
    memcpy(serial_capture + serial_length, text, count);
    serial_length += count;
    serial_capture[serial_length] = '\0';
}

static void maybe_inject_writer(void)
{
    if (inject_writer && !writer_injected && serial_operations != 0u)
    {
        static const char interloper[] =
            "[BOOT][PROGRESS] stage=RUNTIME_READY monotonic_ms=8206"
            " clockevent_ticks=3995\n";
        capture_append(interloper, sizeof(interloper) - 1u);
        writer_injected = true;
    }
}

void serial_write_all(const char *text)
{
    serial_write_calls++;
    maybe_inject_writer();
    serial_operations++;
    if (!text)
    {
        test_result.failures++;
        return;
    }
    capture_append(text, strlen(text));
}

void serial_putc_all(char value)
{
    serial_putc_calls++;
    maybe_inject_writer();
    serial_operations++;
    capture_append(&value, 1u);
}

static void reset_capture(bool with_writer)
{
    memset(serial_capture, 0, sizeof(serial_capture));
    serial_length = 0;
    serial_write_calls = 0;
    serial_putc_calls = 0;
    serial_operations = 0;
    inject_writer = with_writer;
    writer_injected = false;
    memset(&g_assertion_test_state, 0, sizeof(g_assertion_test_state));
}

static void check_format(
    const char *name,
    uint32_t kind,
    uint32_t scenario,
    uint32_t slot,
    uint32_t cpu_id,
    uint32_t value,
    const char *expected)
{
    unsigned int failures_before = test_result.failures;
    unsigned char guarded[GUARD_SIZE + RECORD_CAPACITY + GUARD_SIZE];
    memset(guarded, 0xa5, sizeof(guarded));
    char *record = (char *)&guarded[GUARD_SIZE];
    int result = assertion_test_format_record_for_test(
        record, RECORD_CAPACITY, kind, scenario, slot, cpu_id, value);
    size_t expected_length = strlen(expected);
    check(result == (int)expected_length);
    check(strcmp(record, expected) == 0);
    check(bytes_are(guarded, GUARD_SIZE, 0xa5));
    check(bytes_are(
        guarded + GUARD_SIZE + expected_length + 1u,
        RECORD_CAPACITY - expected_length - 1u, 0xa5));
    check(bytes_are(
        guarded + GUARD_SIZE + RECORD_CAPACITY, GUARD_SIZE, 0xa5));
    finish_case(name, failures_before);
}

static void format_cases(void)
{
    check_format(
        "execute-zero", RECORD_EXECUTE, 0, 0, 0, 0,
        "\n[ASSERT_TEST][EXECUTE] scenario=0\n");
    check_format(
        "complete-pass", RECORD_COMPLETE, UINT32_MAX, 0, 0, 1,
        "\n[ASSERT_TEST][COMPLETE] scenario=4294967295 status=PASS\n");
    check_format(
        "complete-fail", RECORD_COMPLETE, UINT32_MAX, 0, 0, 0,
        "\n[ASSERT_TEST][COMPLETE] scenario=4294967295 status=FAIL\n");
    check_format(
        "prepared-max", RECORD_PREPARED, UINT32_MAX, UINT32_MAX,
        UINT32_MAX, 0,
        "\n[ASSERT_TEST][PREPARED] scenario=4294967295 slot=4294967295"
        " cpu_id=4294967295\n");
    check_format(
        "context-ready-max", RECORD_CONTEXT_READY, 0, 0, 0,
        UINT32_MAX,
        "\n[ASSERT_TEST][CONTEXT_READY] valid_mask=4294967295\n");
}

static void capacity_case(void)
{
    unsigned int failures_before = test_result.failures;
    static const char expected[] =
        "\n[ASSERT_TEST][PREPARED] scenario=4294967295 slot=4294967295"
        " cpu_id=4294967295\n";
    size_t required = sizeof(expected) - 1u;
    unsigned char guarded[GUARD_SIZE + RECORD_CAPACITY + GUARD_SIZE];
    memset(guarded, 0xa5, sizeof(guarded));
    char *record = (char *)&guarded[GUARD_SIZE];
    int result = assertion_test_format_record_for_test(
        record, required + 1u, RECORD_PREPARED, UINT32_MAX, UINT32_MAX,
        UINT32_MAX, 0);
    check(result == (int)required);
    check(strcmp(record, expected) == 0);
    check(guarded[GUARD_SIZE + required] == 0);
    check(guarded[GUARD_SIZE + required + 1u] == 0xa5);

    memset(guarded, 0xa5, sizeof(guarded));
    result = assertion_test_format_record_for_test(
        record, required, RECORD_PREPARED, UINT32_MAX, UINT32_MAX,
        UINT32_MAX, 0);
    check(result == (int)required);
    check(record[required - 1u] == '\0');
    check(guarded[GUARD_SIZE + required] == 0xa5);
    check(bytes_are(guarded, GUARD_SIZE, 0xa5));
    check(bytes_are(
        guarded + GUARD_SIZE + RECORD_CAPACITY, GUARD_SIZE, 0xa5));

    result = assertion_test_format_record_for_test(
        NULL, 0, RECORD_PREPARED, UINT32_MAX, UINT32_MAX, UINT32_MAX, 0);
    check(result == (int)required);
    finish_case("capacity-boundaries", failures_before);
}

static void single_emission_case(void)
{
    unsigned int failures_before = test_result.failures;
    const uint32_t kinds[] = {
        RECORD_EXECUTE, RECORD_COMPLETE, RECORD_PREPARED,
        RECORD_CONTEXT_READY};
    for (size_t index = 0; index < sizeof(kinds) / sizeof(kinds[0]); index++)
    {
        reset_capture(false);
        bool result = assertion_test_emit_record_for_test(
            RECORD_CAPACITY, kinds[index], 3u, 11u, 11u, 7u);
        check(result);
        check(serial_write_calls == 1u);
        check(serial_putc_calls == 0u);
        check(serial_operations == 1u);
        check(strstr(serial_capture, "[ASSERT_TEST]") != NULL);
        check(strstr(serial_capture, "[ASSERT_TEST][RECORD_ERROR]") == NULL);
        check(g_assertion_test_state.arm_error == 0u);
    }
    finish_case("single-emission", failures_before);
}

static void insufficient_capacity_case(void)
{
    unsigned int failures_before = test_result.failures;
    reset_capture(false);
    bool result = assertion_test_emit_record_for_test(
        44u, RECORD_EXECUTE, UINT32_MAX, 0, 0, 0);
    check(!result);
    check(serial_write_calls == 1u);
    check(serial_putc_calls == 0u);
    check(strcmp(serial_capture,
                 "[ASSERT_TEST][RECORD_ERROR] status=FAIL\n") == 0);
    check(strstr(serial_capture, "[ASSERT_TEST][EXECUTE]") == NULL);
    check(strstr(serial_capture, "status=PASS") == NULL);
    check(g_assertion_test_state.arm_error == 1u);
    check(g_assertion_test_state.result == 0u);
    finish_case("insufficient-capacity", failures_before);
}

static void fragmented_execute_control(void)
{
    serial_write_all("\n[ASSERT_TEST][EXECUTE] scenario=");
    serial_putc_all('3');
    serial_write_all("\n");
}

static void writer_boundary_case(void)
{
    unsigned int failures_before = test_result.failures;
    reset_capture(true);
    bool result = assertion_test_emit_record_for_test(
        RECORD_CAPACITY, RECORD_EXECUTE, 3u, 0, 0, 0);
    check(result);
    check(serial_write_calls == 1u);
    check(serial_putc_calls == 0u);
    check(!writer_injected);
    check(strcmp(serial_capture,
                 "\n[ASSERT_TEST][EXECUTE] scenario=3\n") == 0);

    reset_capture(true);
    fragmented_execute_control();
    check(writer_injected);
    check(serial_write_calls == 2u);
    check(serial_putc_calls == 1u);
    check(strstr(serial_capture,
                 "[ASSERT_TEST][EXECUTE] scenario=[BOOT][PROGRESS]") != NULL);
    check(strstr(serial_capture, "\n3\n") != NULL);
    check(strstr(serial_capture,
                 "[ASSERT_TEST][EXECUTE] scenario=3\n") == NULL);
    finish_case("writer-boundary", failures_before);
}

int main(void)
{
    printf("[ASSERT_RECORD_HOST][SUITE_BEGIN] records=4 capacity=96\n");
    format_cases();
    capacity_case();
    single_emission_case();
    insufficient_capacity_case();
    writer_boundary_case();
    printf("[ASSERT_RECORD_HOST][SUITE_END] status=%s cases=%u assertions=%u"
           " failures=%u records=4 capacity=96\n",
           test_result.failures == 0u ? "PASS" : "FAIL",
           test_result.cases, test_result.assertions, test_result.failures);
    return test_result.failures == 0u ? 0 : 1;
}
