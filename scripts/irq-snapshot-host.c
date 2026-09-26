#include "../kernel/src/core/interrupt_context.h"
#include "../kernel/src/core/irq_bootstrap.h"
#include "../kernel/src/drivers/timer.h"
#include "../kernel/src/memory/heap.h"
#include "../kernel/src/timer/hpet.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAPTURE_CAPACITY (256u * 1024u)
#define CAPTURE_CALLS 128u
#define ALLOCATION_CAPACITY (16u * 1024u)
#define GUARD_BYTES 32u
#define EXPECTED_RECORD_CAPACITY 768u

int irq_boot_for_test(void);
void irq_boot_set_record_capacity_for_test(size_t capacity);
size_t irq_boot_sample_storage_bytes_for_test(uint32_t cpu_count);

typedef struct
{
    unsigned int cases;
    unsigned int assertions;
    unsigned int failures;
} test_state_t;

static test_state_t test_state;
static irq_bootstrap_snapshot_t boot_source;
static timer_clockevent_snapshot_t clock_source;
static hpet_runtime_snapshot_t hpet_source;
static irq_bootstrap_cpu_snapshot_t cpu_source[HOBBYOS_MAX_CPUS];
static interrupt_cpu_snapshot_t journal_source[HOBBYOS_MAX_CPUS];
static interrupt_cpu_snapshot_t last_source[HOBBYOS_MAX_CPUS];
static uint32_t stable_calls[HOBBYOS_MAX_CPUS];
static uint32_t transient_failures[HOBBYOS_MAX_CPUS];
static bool permanent_busy[HOBBYOS_MAX_CPUS];
static bool snapshot_available[HOBBYOS_MAX_CPUS];
static bool bootstrap_cpu_available[HOBBYOS_MAX_CPUS];
static bool initial_snapshots_available;
static uint64_t fake_time_ms;
static uint64_t timer_read_cost_ms;
static uint64_t output_cost_ms;
static uint32_t sleep_calls;
static uint32_t allocation_calls;
static uint32_t free_calls;
static bool allocation_fail;
static bool allocation_active;
static size_t allocation_size;
static unsigned char allocation_arena[
    GUARD_BYTES + ALLOCATION_CAPACITY + GUARD_BYTES];
static char serial_capture[CAPTURE_CAPACITY];
static size_t serial_length;
static size_t serial_call_offsets[CAPTURE_CALLS];
static size_t serial_call_lengths[CAPTURE_CALLS];
static uint32_t serial_write_calls;
static uint32_t serial_putc_calls;
static uint32_t console_write_calls;

static bool check_at(bool condition, unsigned int line, const char *expression)
{
    test_state.assertions++;
    if (!condition)
    {
        test_state.failures++;
        printf("[IRQ_SNAPSHOT_HOST][ASSERT] status=FAIL line=%u expression=%s\n",
               line, expression);
    }
    return condition;
}

#define check(condition) check_at((condition), __LINE__, #condition)

static void finish_case(const char *name, unsigned int failures_before)
{
    bool pass = failures_before == test_state.failures;
    test_state.cases++;
    printf("[IRQ_SNAPSHOT_HOST][CASE] id=%s status=%s\n",
           name, pass ? "PASS" : "FAIL");
}

static bool bytes_are(const unsigned char *bytes, size_t count,
                      unsigned char value)
{
    for (size_t index = 0; index < count; index++)
        if (bytes[index] != value)
            return false;
    return true;
}

static void set_sources(uint32_t cpu_count)
{
    memset(&boot_source, 0, sizeof(boot_source));
    memset(&clock_source, 0, sizeof(clock_source));
    memset(&hpet_source, 0, sizeof(hpet_source));
    memset(cpu_source, 0, sizeof(cpu_source));
    memset(journal_source, 0, sizeof(journal_source));
    memset(last_source, 0, sizeof(last_source));
    memset(stable_calls, 0, sizeof(stable_calls));
    memset(transient_failures, 0, sizeof(transient_failures));
    memset(permanent_busy, 0, sizeof(permanent_busy));
    for (uint32_t slot = 0; slot < HOBBYOS_MAX_CPUS; slot++)
    {
        snapshot_available[slot] = true;
        bootstrap_cpu_available[slot] = true;
        cpu_source[slot].slot = slot;
        cpu_source[slot].apic_id = 100u + slot;
        cpu_source[slot].handoff_complete = 1u;
        cpu_source[slot].preemption_enabled = 1u;
        cpu_source[slot].runtime_ready = 1u;
        journal_source[slot].initialized = 1u;
        journal_source[slot].first_vector = 32u + slot;
        journal_source[slot].entered_total = 1000u + slot;
        journal_source[slot].returned_total = 1000u + slot;
        last_source[slot] = journal_source[slot];
        last_source[slot].entered_total++;
        last_source[slot].depth = 1u;
    }
    boot_source.state = IRQ_BOOTSTRAP_SERVICES_ACTIVE;
    boot_source.cpus_expected = cpu_count;
    boot_source.cpus_prepared = cpu_count;
    boot_source.cpus_verified = cpu_count;
    boot_source.handoffs_complete = cpu_count;
    boot_source.preemption_enabled = cpu_count;
    boot_source.cpus_runtime_ready = cpu_count;
    boot_source.bsp_probe_vector = 241u;
    boot_source.bsp_lapic_entered = 11u;
    boot_source.bsp_lapic_returned = 11u;
    boot_source.hpet_clocksource_verified = 1u;
    boot_source.hpet_timer0_quiescent = 1u;
    boot_source.hpet_probe_samples = 8u;
    boot_source.hpet_counter_before = 100u;
    boot_source.hpet_counter_after = 200u;
    boot_source.hpet_counter_delta = 100u;
    boot_source.keyboard_gsi = 1u;
    clock_source.source = TIMER_CLOCKEVENT_BSP_LAPIC;
    clock_source.period_us = 1000u;
    clock_source.total_ticks = 3000u;
    hpet_source.timer0_interrupt_enabled = 0u;
    hpet_source.timer0_route_enabled = 0u;
    initial_snapshots_available = true;
}

static void reset_mocks(uint32_t cpu_count)
{
    set_sources(cpu_count);
    fake_time_ms = 0;
    timer_read_cost_ms = 0;
    output_cost_ms = 0;
    sleep_calls = 0;
    allocation_calls = 0;
    free_calls = 0;
    allocation_fail = false;
    allocation_active = false;
    allocation_size = 0;
    memset(allocation_arena, 0xa5, sizeof(allocation_arena));
    memset(serial_capture, 0, sizeof(serial_capture));
    memset(serial_call_offsets, 0, sizeof(serial_call_offsets));
    memset(serial_call_lengths, 0, sizeof(serial_call_lengths));
    serial_length = 0;
    serial_write_calls = 0;
    serial_putc_calls = 0;
    console_write_calls = 0;
    irq_boot_set_record_capacity_for_test(EXPECTED_RECORD_CAPACITY);
}

static size_t count_prefix(const char *prefix)
{
    size_t count = 0;
    size_t prefix_length = strlen(prefix);
    const char *cursor = serial_capture;
    while ((cursor = strstr(cursor, prefix)) != NULL)
    {
        count++;
        cursor += prefix_length;
    }
    return count;
}

static bool capture_has(const char *text)
{
    return strstr(serial_capture, text) != NULL;
}

static bool calls_are_complete_records(void)
{
    for (uint32_t index = 0; index < serial_write_calls; index++)
    {
        size_t length = serial_call_lengths[index];
        if (!length || serial_capture[serial_call_offsets[index] + length - 1u]
            != '\n')
            return false;
        const char *record = serial_capture + serial_call_offsets[index];
        const char *next = strchr(record, '\n');
        if (!next || (size_t)(next - record + 1) != length)
            return false;
    }
    return true;
}

static size_t maximum_record_length(void)
{
    size_t maximum = 0;
    for (uint32_t index = 0; index < serial_write_calls; index++)
        if (serial_call_lengths[index] > maximum)
            maximum = serial_call_lengths[index];
    return maximum;
}

static bool allocation_clean(void)
{
    return !allocation_active && allocation_calls == free_calls &&
           bytes_are(allocation_arena, GUARD_BYTES, 0xa5) &&
           bytes_are(allocation_arena + GUARD_BYTES + allocation_size,
                     ALLOCATION_CAPACITY - allocation_size + GUARD_BYTES,
                     0xa5);
}

bool irq_bootstrap_snapshot(irq_bootstrap_snapshot_t *out)
{
    if (!initial_snapshots_available || !out)
        return false;
    *out = boot_source;
    return true;
}

bool timer_clockevent_snapshot(timer_clockevent_snapshot_t *out)
{
    if (!initial_snapshots_available || !out)
        return false;
    *out = clock_source;
    return true;
}

bool hpet_runtime_snapshot(hpet_runtime_snapshot_t *out)
{
    if (!initial_snapshots_available || !out)
        return false;
    *out = hpet_source;
    return true;
}

bool irq_bootstrap_cpu_snapshot(cpu_slot_t slot,
                                irq_bootstrap_cpu_snapshot_t *out)
{
    if (!out || slot >= HOBBYOS_MAX_CPUS || !bootstrap_cpu_available[slot])
        return false;
    *out = cpu_source[slot];
    return true;
}

bool interrupt_context_snapshot_stable(cpu_slot_t slot,
                                       interrupt_cpu_snapshot_t *out,
                                       uint32_t attempts)
{
    (void)attempts;
    if (!out || slot >= HOBBYOS_MAX_CPUS)
        return false;
    stable_calls[slot]++;
    if (permanent_busy[slot] ||
        stable_calls[slot] <= transient_failures[slot])
        return false;
    *out = journal_source[slot];
    return true;
}

bool interrupt_context_snapshot(cpu_slot_t slot,
                                interrupt_cpu_snapshot_t *out)
{
    if (!out || slot >= HOBBYOS_MAX_CPUS || !snapshot_available[slot])
        return false;
    *out = last_source[slot];
    return true;
}

uint64_t timer_get_uptime_ms(void)
{
    uint64_t observed = fake_time_ms;
    fake_time_ms += timer_read_cost_ms;
    return observed;
}

void timer_sleep(uint64_t milliseconds)
{
    if (UINT64_MAX - fake_time_ms < milliseconds)
        fake_time_ms = UINT64_MAX;
    else
        fake_time_ms += milliseconds;
    sleep_calls++;
}

void *kmalloc(size_t size)
{
    allocation_calls++;
    if (allocation_fail || allocation_active || !size ||
        size > ALLOCATION_CAPACITY)
        return NULL;
    allocation_active = true;
    allocation_size = size;
    memset(allocation_arena, 0xa5, sizeof(allocation_arena));
    return allocation_arena + GUARD_BYTES;
}

void kfree(void *pointer)
{
    free_calls++;
    if (pointer != allocation_arena + GUARD_BYTES || !allocation_active)
    {
        test_state.failures++;
        return;
    }
    allocation_active = false;
}

void console_write(const char *text)
{
    if (!text)
        test_state.failures++;
    console_write_calls++;
}

void serial_write_all(const char *text)
{
    if (!text || serial_write_calls >= CAPTURE_CALLS)
    {
        test_state.failures++;
        return;
    }
    size_t length = strlen(text);
    if (length + serial_length >= CAPTURE_CAPACITY)
    {
        test_state.failures++;
        return;
    }
    serial_call_offsets[serial_write_calls] = serial_length;
    serial_call_lengths[serial_write_calls] = length;
    serial_write_calls++;
    memcpy(serial_capture + serial_length, text, length + 1u);
    serial_length += length;
    fake_time_ms += output_cost_ms;
}

void serial_putc_all(char value)
{
    (void)value;
    serial_putc_calls++;
}

static void case_all_slots_stable(void)
{
    unsigned int before = test_state.failures;
    reset_mocks(4u);
    int status = irq_boot_for_test();
    check(status == 0);
    check(count_prefix("[IRQ][BOOT_CPU]") == 4u);
    check(count_prefix("[IRQ][BOOT_RESULT] PASS") == 1u);
    check(capture_has("snapshots=4 polls=4"));
    check(capture_has("acquisition_ms=0 publication_ms=0 budget_ms=5000"));
    check(serial_write_calls == 8u);
    check(console_write_calls == serial_write_calls);
    check(serial_putc_calls == 0u);
    check(calls_are_complete_records());
    check(allocation_clean());
    finish_case("all-slots-stable", before);
}

static void case_slow_output_after_acquisition(void)
{
    unsigned int before = test_state.failures;
    reset_mocks(24u);
    transient_failures[17] = 1u;
    output_cost_ms = 300u;
    int status = irq_boot_for_test();
    check(status == 0);
    check(stable_calls[17] == 2u);
    check(sleep_calls == 1u);
    check(count_prefix("[IRQ][BOOT_CPU]") == 24u);
    check(capture_has("snapshots=24 polls=25"));
    check(capture_has("acquisition_ms=1 publication_ms=8100"));
    check(fake_time_ms == 8401u);
    check(calls_are_complete_records());
    check(allocation_clean());
    finish_case("slow-output-after-acquisition", before);
}

static void case_timing_partition_uses_shared_boundaries(void)
{
    unsigned int before = test_state.failures;
    reset_mocks(1u);
    timer_read_cost_ms = 1u;
    int status = irq_boot_for_test();
    check(status == 0);
    check(capture_has("elapsed_ms=2 acquisition_ms=1 publication_ms=1"));
    check(fake_time_ms == 3u);
    check(calls_are_complete_records());
    check(allocation_clean());
    finish_case("timing-partition-shared-boundaries", before);
}

static void case_permanent_busy(void)
{
    unsigned int before = test_state.failures;
    reset_mocks(4u);
    permanent_busy[2] = true;
    int status = irq_boot_for_test();
    check(status == 1);
    check(fake_time_ms == 5000u);
    check(sleep_calls == 5000u);
    check(count_prefix("[IRQ][BOOT_CPU]") == 2u);
    check(count_prefix("[IRQ][BOOT_SNAPSHOT] FAIL") == 1u);
    check(capture_has("failed_slot=2 reason=timeout"));
    check(capture_has("acquisition_ms=5000"));
    check(!capture_has("[IRQ][BOOT_RESULT] PASS"));
    check(allocation_clean());
    finish_case("permanent-busy", before);
}

static void case_bootstrap_unavailable(void)
{
    unsigned int before = test_state.failures;
    reset_mocks(4u);
    bootstrap_cpu_available[1] = false;
    int status = irq_boot_for_test();
    check(status == 1);
    check(count_prefix("[IRQ][BOOT_CPU]") == 1u);
    check(capture_has("slot=1 reason=bootstrap-unavailable"));
    check(capture_has("snapshots=1 failed_slot=1"));
    check(!capture_has("[IRQ][BOOT_RESULT] PASS"));
    check(allocation_clean());
    finish_case("bootstrap-unavailable", before);
}

static void case_structural_snapshot(void)
{
    unsigned int before = test_state.failures;
    reset_mocks(2u);
    transient_failures[1] = 1u;
    last_source[1].underflow = 1u;
    int status = irq_boot_for_test();
    check(status == 1);
    check(capture_has("slot=1 reason=structural"));
    check(capture_has("underflow=1"));
    check(sleep_calls == 0u);
    check(allocation_clean());
    finish_case("structural-snapshot", before);
}

static void case_topology_invalid(void)
{
    unsigned int before = test_state.failures;
    reset_mocks(0u);
    int zero_status = irq_boot_for_test();
    check(zero_status == 1);
    check(capture_has("reason=topology-invalid cpus=0 max=32"));
    check(allocation_calls == 0u);

    reset_mocks(HOBBYOS_MAX_CPUS + 1u);
    int high_status = irq_boot_for_test();
    check(high_status == 1);
    check(capture_has("reason=topology-invalid cpus=33 max=32"));
    check(allocation_calls == 0u);
    finish_case("topology-invalid", before);
}

static void case_initial_snapshot_unavailable(void)
{
    unsigned int before = test_state.failures;
    reset_mocks(4u);
    initial_snapshots_available = false;
    int status = irq_boot_for_test();
    check(status == 1);
    check(capture_has("[IRQ][BOOT] FAIL snapshot=0\n"));
    check(capture_has("reason=snapshot-unavailable"));
    check(allocation_calls == 0u);
    check(calls_are_complete_records());
    finish_case("initial-snapshot-unavailable", before);
}

static void case_storage_unavailable(void)
{
    unsigned int before = test_state.failures;
    reset_mocks(4u);
    allocation_fail = true;
    int status = irq_boot_for_test();
    check(status == 1);
    check(allocation_calls == 1u);
    check(free_calls == 0u);
    check(!allocation_active);
    check(count_prefix("[IRQ][BOOT_ERROR] reason=storage-unavailable") == 1u);
    check(!capture_has("[IRQ][BOOT] state="));
    check(!capture_has("[IRQ][BOOT_RESULT] PASS"));
    check(calls_are_complete_records());
    finish_case("storage-unavailable", before);
}

static void case_format_failure_cleanup(void)
{
    unsigned int before = test_state.failures;
    reset_mocks(4u);
    irq_boot_set_record_capacity_for_test(16u);
    int status = irq_boot_for_test();
    check(status == 1);
    check(count_prefix("[IRQ][BOOT_ERROR] reason=format") == 1u);
    check(!capture_has("[IRQ][BOOT] state="));
    check(!capture_has("[IRQ][BOOT_RESULT] PASS"));
    check(calls_are_complete_records());
    check(allocation_clean());
    finish_case("format-failure-cleanup", before);
}

static void set_maximum_values(void)
{
    boot_source.bsp_lapic_entered = UINT64_MAX;
    boot_source.bsp_lapic_returned = UINT64_MAX;
    boot_source.hpet_counter_before = UINT64_MAX;
    boot_source.hpet_counter_after = UINT64_MAX;
    boot_source.hpet_counter_delta = UINT64_MAX;
    clock_source.total_ticks = UINT64_MAX;
    clock_source.early_attempts = UINT64_MAX;
    clock_source.non_bsp_attempts = UINT64_MAX;
    clock_source.timestamp_regressions = UINT64_MAX;
    hpet_source.stray_irqs = UINT64_MAX;
    for (uint32_t slot = 0; slot < HOBBYOS_MAX_CPUS; slot++)
    {
        cpu_source[slot].apic_id = UINT32_MAX;
        journal_source[slot].first_vector = UINT32_MAX;
        journal_source[slot].entered_total = UINT64_MAX;
        journal_source[slot].returned_total = UINT64_MAX;
        journal_source[slot].depth = UINT32_MAX;
        journal_source[slot].unexpected = UINT64_MAX;
    }
}

static void case_maximum_cpu_count(void)
{
    unsigned int before = test_state.failures;
    reset_mocks(HOBBYOS_MAX_CPUS);
    set_maximum_values();
    int status = irq_boot_for_test();
    check(status == 0);
    check(count_prefix("[IRQ][BOOT_CPU]") == HOBBYOS_MAX_CPUS);
    check(irq_boot_sample_storage_bytes_for_test(HOBBYOS_MAX_CPUS) ==
          allocation_size);
    check(allocation_size <= ALLOCATION_CAPACITY);
    check(maximum_record_length() < EXPECTED_RECORD_CAPACITY);
    check(calls_are_complete_records());
    check(allocation_clean());
    finish_case("maximum-cpu-count", before);
}

static void case_exact_record_capacity(void)
{
    unsigned int before = test_state.failures;
    reset_mocks(HOBBYOS_MAX_CPUS);
    set_maximum_values();
    check(irq_boot_for_test() == 0);
    size_t maximum = maximum_record_length();
    check(maximum > 0u && maximum < EXPECTED_RECORD_CAPACITY);

    reset_mocks(HOBBYOS_MAX_CPUS);
    set_maximum_values();
    irq_boot_set_record_capacity_for_test(maximum + 1u);
    check(irq_boot_for_test() == 0);
    check(allocation_clean());

    reset_mocks(HOBBYOS_MAX_CPUS);
    set_maximum_values();
    irq_boot_set_record_capacity_for_test(maximum);
    check(irq_boot_for_test() == 1);
    check(capture_has("[IRQ][BOOT_ERROR] reason=format"));
    check(!capture_has("[IRQ][BOOT_RESULT] PASS"));
    check(calls_are_complete_records());
    check(allocation_clean());
    finish_case("exact-record-capacity", before);
}

static void case_snapshot_unavailable(void)
{
    unsigned int before = test_state.failures;
    reset_mocks(3u);
    transient_failures[1] = 1u;
    snapshot_available[1] = false;
    int status = irq_boot_for_test();
    check(status == 1);
    check(capture_has("slot=1 reason=unavailable"));
    check(capture_has("snapshots=1 failed_slot=1"));
    check(!capture_has("[IRQ][BOOT_RESULT] PASS"));
    check(allocation_clean());
    finish_case("snapshot-unavailable", before);
}

int main(void)
{
    case_all_slots_stable();
    case_slow_output_after_acquisition();
    case_timing_partition_uses_shared_boundaries();
    case_permanent_busy();
    case_bootstrap_unavailable();
    case_structural_snapshot();
    case_topology_invalid();
    case_initial_snapshot_unavailable();
    case_storage_unavailable();
    case_format_failure_cleanup();
    case_maximum_cpu_count();
    case_exact_record_capacity();
    case_snapshot_unavailable();
    printf("[IRQ_SNAPSHOT_HOST][SUMMARY] status=%s cases=%u assertions=%u failures=%u\n",
           test_state.failures ? "FAIL" : "PASS", test_state.cases,
           test_state.assertions, test_state.failures);
    return test_state.failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
