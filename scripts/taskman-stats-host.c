#include "../kernel/src/shell/commands/cmd_taskman.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int taskmantest_format_stats_record_for_test(
    char *record,
    size_t record_capacity,
    const taskman_stats_t *stats,
    uint32_t auto_exit_pending);
bool taskmantest_emit_stats_record_for_test(size_t record_capacity);

#define EXPECTED_RECORD_CAPACITY 1953u
#define ALLOCATION_ARENA_CAPACITY 4096u
#define SERIAL_CAPTURE_CAPACITY 8192u
#define GUARD_BYTES 16u

typedef struct host_test_state
{
    unsigned int cases;
    unsigned int assertions;
    unsigned int failures;
} host_test_state_t;

static host_test_state_t test_state;
static taskman_stats_t snapshot_source;
static uint32_t pending_source;
static unsigned int snapshot_calls;
static unsigned int pending_calls;
static unsigned int allocation_calls;
static unsigned int free_calls;
static unsigned int allocation_failures;
static bool fail_allocation;
static bool allocation_active;
static size_t allocation_size;
static unsigned char allocation_arena[
    GUARD_BYTES + ALLOCATION_ARENA_CAPACITY + GUARD_BYTES];
static char serial_capture[SERIAL_CAPTURE_CAPACITY];
static size_t serial_length;
static unsigned int serial_write_calls;
static unsigned int serial_putc_calls;

static bool check_at(bool condition, unsigned int line, const char *expression)
{
    test_state.assertions++;
    if (!condition)
    {
        test_state.failures++;
        printf("[STATS_HOST][ASSERT] status=FAIL line=%u expression=%s\n",
               line, expression);
    }
    return condition;
}

#define check(condition) check_at((condition), __LINE__, #condition)

static void finish_case(const char *name, unsigned int failures_before)
{
    test_state.cases++;
    printf("[STATS_HOST][CASE] id=%s status=%s\n", name,
           test_state.failures == failures_before ? "PASS" : "FAIL");
}

static bool bytes_are(
    const unsigned char *bytes,
    size_t length,
    unsigned char value)
{
    for (size_t i = 0; i < length; i++)
        if (bytes[i] != value)
            return false;
    return true;
}

static void reset_mocks(void)
{
    snapshot_calls = 0;
    pending_calls = 0;
    allocation_calls = 0;
    free_calls = 0;
    allocation_failures = 0;
    fail_allocation = false;
    allocation_active = false;
    allocation_size = 0;
    memset(allocation_arena, 0xa5, sizeof(allocation_arena));
    memset(serial_capture, 0, sizeof(serial_capture));
    serial_length = 0;
    serial_write_calls = 0;
    serial_putc_calls = 0;
}

void taskman_stats_snapshot(taskman_stats_t *out)
{
    snapshot_calls++;
    if (out)
        *out = snapshot_source;
}

uint32_t taskman_test_auto_exit_pending(void)
{
    pending_calls++;
    return pending_source;
}

void *kmalloc(size_t size)
{
    allocation_calls++;
    if (fail_allocation || allocation_active || size == 0u ||
        size > ALLOCATION_ARENA_CAPACITY)
    {
        allocation_failures++;
        return NULL;
    }
    allocation_active = true;
    allocation_size = size;
    memset(allocation_arena, 0xa5, sizeof(allocation_arena));
    return &allocation_arena[GUARD_BYTES];
}

void kfree(void *pointer)
{
    free_calls++;
    if (pointer != &allocation_arena[GUARD_BYTES] || !allocation_active)
    {
        test_state.failures++;
        return;
    }
    allocation_active = false;
}

void serial_write_all(const char *text)
{
    serial_write_calls++;
    if (!text)
    {
        test_state.failures++;
        return;
    }
    size_t length = strlen(text);
    if (length >= SERIAL_CAPTURE_CAPACITY - serial_length)
    {
        test_state.failures++;
        return;
    }
    memcpy(serial_capture + serial_length, text, length + 1u);
    serial_length += length;
}

void serial_putc_all(char value)
{
    serial_putc_calls++;
    if (serial_length + 1u >= SERIAL_CAPTURE_CAPACITY)
    {
        test_state.failures++;
        return;
    }
    serial_capture[serial_length++] = value;
    serial_capture[serial_length] = '\0';
}

static void set_distinct_snapshot(void)
{
    memset(&snapshot_source, 0, sizeof(snapshot_source));
    snapshot_source.sessions = 1u;
    snapshot_source.frames = 2u;
    snapshot_source.full_frames = 3u;
    snapshot_source.fallback_frames = 4u;
    snapshot_source.auto_exit_sessions = 5u;
    snapshot_source.auto_exit_shortfalls = 6u;
    snapshot_source.max_full_frame_gap_ns = 7000999u;
    snapshot_source.shell_exit_scroll_delta = 8u;
    snapshot_source.last_session_shell_exit_scroll_delta = 9u;
    snapshot_source.last_session_full_frames = 10u;
    snapshot_source.last_session_fallback_frames = 11u;
    snapshot_source.last_layout_mode = TASKMAN_LAYOUT_COMPACT;
    snapshot_source.last_pages = 12u;
    snapshot_source.last_captured = 13u;
    snapshot_source.last_total = 14u;
    snapshot_source.last_truncated = 15u;
    snapshot_source.render_failures = 16u;
    snapshot_source.page_changes = 17u;
    snapshot_source.selection_changes = 18u;
    snapshot_source.ignored_inputs = 19u;
    snapshot_source.scroll_delta = 20u;
    snapshot_source.last_session_scroll_delta = 21u;
    snapshot_source.last_session_clipped_writes = 22u;
    snapshot_source.builder_truncations = 23u;
    snapshot_source.summary_format_failures = 24u;
    snapshot_source.footer_format_failures = 25u;
    snapshot_source.pre_render_clear_failures = 26u;
    snapshot_source.present_calls = 27u;
    snapshot_source.last_selected_id = 28u;
    snapshot_source.rows_examined = 29u;
    snapshot_source.rows_changed = 30u;
    snapshot_source.rows_unchanged = 31u;
    snapshot_source.cells_examined = 32u;
    snapshot_source.cells_changed = 33u;
    snapshot_source.cells_unchanged = 34u;
    snapshot_source.glyph_changes = 35u;
    snapshot_source.style_changes = 36u;
    snapshot_source.first_frame_full_presents = 37u;
    snapshot_source.cleanup_clears = 38u;
    snapshot_source.geometry_invalidation_clears = 39u;
    snapshot_source.stable_frame_full_clears = 40u;
    snapshot_source.tail_rows_cleared_by_present = 41u;
    snapshot_source.separator_mismatches = 42u;
    snapshot_source.field_overflows = 43u;
    snapshot_source.field_truncations = 44u;
    snapshot_source.split_overlaps = 45u;
    snapshot_source.visual_workspace_live = 46u;
    snapshot_source.visual_workspace_allocations = 47u;
    snapshot_source.visual_workspace_reallocations = 48u;
    snapshot_source.visual_workspace_frees = 49u;
    snapshot_source.stale_cells = 50u;
    pending_source = 51u;
}

static const char distinct_expected[] =
    "\n[TASKMANTEST][STATS] sessions=1 frames=2 full_frames=3"
    " fallback_frames=4 auto_exit_sessions=5 auto_exit_shortfalls=6"
    " max_frame_gap_ms=7 shell_exit_scroll_delta=8"
    " last_session_shell_exit_scroll_delta=9"
    " last_session_full_frames=10 last_session_fallback_frames=11"
    " last_mode=COMPACT pages=12 captured=13 total=14 truncated=15"
    " render_failures=16 page_changes=17 selection_changes=18 ignored=19"
    " scroll_delta=20 last_session_scroll_delta=21 clipped=22"
    " builder_truncations=23 summary_failures=24 footer_failures=25"
    " region_clear_failures=26 present_calls=27 selected_pid=28"
    " rows_examined=29 rows_changed=30 rows_unchanged=31"
    " cells_examined=32 cells_changed=33 cells_unchanged=34"
    " glyph_changes=35 style_changes=36 first_frame_presents=37"
    " cleanup_clears=38 geometry_clears=39 stable_frame_full_clears=40"
    " tail_rows_cleared=41 separator_mismatches=42 field_overflows=43"
    " field_truncations=44 split_overlaps=45 workspace_live=46"
    " workspace_allocations=47 workspace_reallocations=48"
    " workspace_frees=49 stale_cells=50 auto_exit_pending=51\n";

static size_t count_fields(const char *record)
{
    size_t count = 0;
    for (const char *cursor = record; *cursor; cursor++)
        if (*cursor == '=')
            count++;
    return count;
}

static bool record_has_one_complete_line(const char *record)
{
    if (!record || record[0] != '\n')
        return false;
    const char *line = record + 1;
    const char *end = strchr(line, '\n');
    return end && end[1] == '\0' &&
           strncmp(line, "[TASKMANTEST][STATS] ", 21u) == 0 &&
           count_fields(line) == 52u;
}

static void test_field_compatibility(void)
{
    unsigned int before = test_state.failures;
    set_distinct_snapshot();
    taskman_stats_t preserved = snapshot_source;
    char record[2048];
    memset(record, 0x5a, sizeof(record));
    int result = taskmantest_format_stats_record_for_test(
        record, sizeof(record), &snapshot_source, pending_source);
    check(result == (int)strlen(distinct_expected));
    check(strcmp(record, distinct_expected) == 0);
    check(count_fields(record) == 52u);
    check(record_has_one_complete_line(record));
    check(memcmp(&snapshot_source, &preserved, sizeof(preserved)) == 0);
    check((unsigned char)record[result + 1] == 0x5au);
    finish_case("field-compatibility", before);
}

static void test_capacity_and_guards(void)
{
    unsigned int before = test_state.failures;
    set_distinct_snapshot();
    int required = taskmantest_format_stats_record_for_test(
        NULL, 0, &snapshot_source, pending_source);
    check(required == (int)strlen(distinct_expected));
    check(required > 0 && (size_t)required + 1u < EXPECTED_RECORD_CAPACITY);

    unsigned char arena[2048];
    memset(arena, 0xa5, sizeof(arena));
    char *record = (char *)&arena[8];
    int exact = taskmantest_format_stats_record_for_test(
        record, (size_t)required + 1u, &snapshot_source, pending_source);
    check(exact == required && strcmp(record, distinct_expected) == 0);
    check(bytes_are(arena, 8u, 0xa5));
    check(bytes_are(&arena[8u + (size_t)required + 1u],
                    sizeof(arena) - 8u - (size_t)required - 1u, 0xa5));

    memset(arena, 0xa5, sizeof(arena));
    int short_result = taskmantest_format_stats_record_for_test(
        record, (size_t)required, &snapshot_source, pending_source);
    check(short_result == required);
    check(record[required - 1] == '\0');
    check(memcmp(record, distinct_expected, (size_t)required - 1u) == 0);
    check((unsigned char)record[required] == 0xa5u);
    finish_case("capacity-and-guards", before);
}

static void test_modes(void)
{
    unsigned int before = test_state.failures;
    static const char *names[] = {
        "WIDE", "COMPACT", "TOO_NARROW", "TOO_SHORT"
    };
    char record[2048];
    char expected[32];
    memset(&snapshot_source, 0, sizeof(snapshot_source));
    for (uint32_t mode = 0; mode < 4u; mode++)
    {
        snapshot_source.last_layout_mode = mode;
        int result = taskmantest_format_stats_record_for_test(
            record, sizeof(record), &snapshot_source, 0u);
        snprintf(expected, sizeof(expected), " last_mode=%s ", names[mode]);
        check(result > 0 && strstr(record, expected) != NULL);
        check(record_has_one_complete_line(record));
    }
    finish_case("layout-modes", before);
}

static void test_extremes(void)
{
    unsigned int before = test_state.failures;
    char record[2048];
    memset(&snapshot_source, 0xff, sizeof(snapshot_source));
    int result = taskmantest_format_stats_record_for_test(
        record, sizeof(record), &snapshot_source, UINT32_MAX);
    check(result > 0 && (size_t)result < EXPECTED_RECORD_CAPACITY);
    check(strstr(record, " sessions=18446744073709551615 ") != NULL);
    check(strstr(record, " max_frame_gap_ms=18446744073709 ") != NULL);
    check(strstr(record, " last_mode=TOO_SHORT ") != NULL);
    check(strstr(record, " pages=4294967295 ") != NULL);
    check(strstr(record, " truncated=255 ") != NULL);
    check(strstr(record, " auto_exit_pending=4294967295\n") != NULL);
    check(count_fields(record) == 52u && record_has_one_complete_line(record));
    finish_case("numeric-extremes", before);
}

static void test_single_emission(void)
{
    unsigned int before = test_state.failures;
    reset_mocks();
    set_distinct_snapshot();
    taskman_stats_t preserved = snapshot_source;
    bool result = taskmantest_emit_stats_record_for_test(
        EXPECTED_RECORD_CAPACITY);
    check(result);
    check(snapshot_calls == 1u && pending_calls == 1u);
    check(allocation_calls == 1u && allocation_failures == 0u);
    check(free_calls == 1u && !allocation_active);
    check(serial_write_calls == 1u && serial_putc_calls == 0u);
    check(strcmp(serial_capture, distinct_expected) == 0);
    check(bytes_are(allocation_arena, GUARD_BYTES, 0xa5));
    check(bytes_are(&allocation_arena[GUARD_BYTES + allocation_size],
                    ALLOCATION_ARENA_CAPACITY - allocation_size + GUARD_BYTES,
                    0xa5));
    check(memcmp(&snapshot_source, &preserved, sizeof(preserved)) == 0);
    finish_case("single-emission", before);
}

static void test_storage_failure(void)
{
    unsigned int before = test_state.failures;
    reset_mocks();
    set_distinct_snapshot();
    fail_allocation = true;
    bool result = taskmantest_emit_stats_record_for_test(
        EXPECTED_RECORD_CAPACITY);
    check(!result);
    check(snapshot_calls == 1u && pending_calls == 1u);
    check(allocation_calls == 1u && allocation_failures == 1u);
    check(free_calls == 0u && !allocation_active);
    check(serial_write_calls == 1u && serial_putc_calls == 0u);
    check(strcmp(serial_capture,
                 "[TASKMANTEST][STATS_ERROR] storage\n") == 0);
    check(strstr(serial_capture, "[TASKMANTEST][STATS]") == NULL);
    finish_case("storage-failure", before);
}

static void test_formatting_failure(void)
{
    unsigned int before = test_state.failures;
    reset_mocks();
    set_distinct_snapshot();
    bool result = taskmantest_emit_stats_record_for_test(64u);
    check(!result);
    check(snapshot_calls == 1u && pending_calls == 1u);
    check(allocation_calls == 1u && allocation_failures == 0u);
    check(free_calls == 1u && !allocation_active);
    check(serial_write_calls == 1u && serial_putc_calls == 0u);
    check(strcmp(serial_capture,
                 "[TASKMANTEST][STATS_ERROR] formatting\n") == 0);
    check(strstr(serial_capture, "[TASKMANTEST][STATS]") == NULL);
    finish_case("formatting-failure", before);
}

static void test_fragmented_control(void)
{
    unsigned int before = test_state.failures;
    char fragmented[2048];
    set_distinct_snapshot();
    const char *needle = "page_changes=17";
    const char *position = strstr(distinct_expected, needle);
    check(position != NULL);
    size_t prefix = (size_t)(position - distinct_expected) +
                    strlen("page_changes=");
    memcpy(fragmented, distinct_expected, prefix);
    fragmented[prefix] = '\n';
    strcpy(fragmented + prefix + 1u, distinct_expected + prefix);
    check(!record_has_one_complete_line(fragmented));
    check(strstr(fragmented, "page_changes=\n17") != NULL);
    check(count_fields(fragmented) == 52u);
    finish_case("fragmented-control", before);
}

static void test_null_snapshot(void)
{
    unsigned int before = test_state.failures;
    char record[32];
    memset(record, 0xa5, sizeof(record));
    int result = taskmantest_format_stats_record_for_test(
        record, sizeof(record), NULL, 0u);
    check(result == -1);
    check((unsigned char)record[0] == 0xa5u);
    finish_case("invalid-snapshot", before);
}

int main(void)
{
    printf("[STATS_HOST][SUITE_BEGIN] cases=9 fields=52 capacity=1953\n");
    test_field_compatibility();
    test_capacity_and_guards();
    test_modes();
    test_extremes();
    test_single_emission();
    test_storage_failure();
    test_formatting_failure();
    test_fragmented_control();
    test_null_snapshot();
    printf("[STATS_HOST][SUITE_END] status=%s cases=%u assertions=%u "
           "failures=%u fields=52 capacity=1953\n",
           test_state.failures ? "FAIL" : "PASS",
           test_state.cases, test_state.assertions, test_state.failures);
    return test_state.failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
