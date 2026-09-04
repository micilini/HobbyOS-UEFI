#include "runtime_ready.h"

#include "clock.h"
#include "dpc.h"
#include "input_router.h"
#include "modal_session.h"
#include "modal_ui.h"
#include "scheduler.h"
#include "selftest.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../drivers/usb/xhci/usb_hotplug.h"
#include "../shell/shell.h"
#include "../smp/smp_boot.h"
#include "../smp/smp_topology.h"

#include <stddef.h>

#define RUNTIME_RECORD_MAX 256u

static volatile uint32_t g_ready_state;
static volatile uint8_t g_pci_scan_complete;
static volatile uint8_t g_autorun_enabled;
static volatile uint8_t g_autorun_passed;
static volatile uint64_t g_published_ns;
static volatile uint64_t g_test_ready_ns;
static volatile uint64_t g_ready_violations;

typedef struct
{
    char bytes[RUNTIME_RECORD_MAX];
    uint32_t length;
    uint8_t overflow;
} runtime_record_t;

static void record_reset(runtime_record_t *record)
{
    record->length = 0;
    record->overflow = 0;
    record->bytes[0] = 0;
}

static bool record_char(runtime_record_t *record, char value)
{
    if (!record || record->overflow || record->length + 1u >= RUNTIME_RECORD_MAX) {
        if (record) record->overflow = 1;
        return false;
    }
    record->bytes[record->length++] = value;
    record->bytes[record->length] = 0;
    return true;
}

static bool record_text(runtime_record_t *record, const char *text)
{
    if (!text) return false;
    while (*text)
        if (!record_char(record, *text++)) return false;
    return true;
}

static bool record_u64(runtime_record_t *record, uint64_t value)
{
    char reverse[21];
    uint32_t count = 0;
    do {
        reverse[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (count)
        if (!record_char(record, reverse[--count])) return false;
    return true;
}

static void record_emit(runtime_record_t *record)
{
    if (!record || record->overflow || !record_char(record, '\n')) {
        serial_write_all("\n[BOOT][READY_EMIT_ERROR] reason=overflow\n");
        return;
    }
    serial_write_all(record->bytes);
}

static bool ready_state_advance(runtime_ready_state_t expected,
                                runtime_ready_state_t desired)
{
    uint32_t old = (uint32_t)expected;
    if ((uint32_t)desired <= (uint32_t)expected)
        return false;
    if (__atomic_compare_exchange_n(&g_ready_state, &old, (uint32_t)desired,
                                    false, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE))
        return true;
    __atomic_add_fetch(&g_ready_violations, 1, __ATOMIC_RELAXED);
    return false;
}

void runtime_ready_init(void)
{
    __atomic_store_n(&g_pci_scan_complete, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_autorun_enabled, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_autorun_passed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_published_ns, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_test_ready_ns, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_ready_violations, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_ready_state, RUNTIME_READY_WAITING_SERVICES,
                     __ATOMIC_RELEASE);
}

void runtime_ready_mark_pci_scan_complete(void)
{
    __atomic_store_n(&g_pci_scan_complete, 1, __ATOMIC_RELEASE);
}

static bool runtime_services_snapshot(runtime_ready_snapshot_t *out)
{
    dpc_runtime_snapshot_t dpc = {0};
    shell_runtime_snapshot_t shell = {0};
    usb_hotplug_readiness_snapshot_t hotplug = {0};
    scheduler_runtime_stats_t scheduler = {0};
    uint64_t input_violations = 0, session_violations = 0;
    uint64_t ui_violations = 0;

    if (!out) return false;
    out->cpus_expected = g_cpu_count;
    out->cpus_online = smp_online_cpu_count();
    out->scheduler_started = scheduler_is_started() ? 1u : 0u;
    (void)dpc_runtime_snapshot(&dpc);
    (void)shell_runtime_snapshot(&shell);
    (void)usb_hotplug_readiness_snapshot(&hotplug);
    out->dpc_initialized = dpc.initialized && dpc.worker_started;
    out->shell_initialized = shell.initialized;
    out->shell_thread_started = shell.thread_started;
    out->input_valid = input_router_validate(&input_violations) ? 1u : 0u;
    out->modal_session_valid = modal_session_validate(&session_violations) ? 1u : 0u;
    out->modal_ui_valid = modal_ui_validate(&ui_violations) ? 1u : 0u;
    out->pci_scan_complete = __atomic_load_n(&g_pci_scan_complete,
                                              __ATOMIC_ACQUIRE);
    out->hotplug_initialized = hotplug.initialized;
    out->hotplug_active = hotplug.active_enumerations;
    out->hotplug_generation = hotplug.activity_generation;
    uint64_t now = clock_monotonic_ns();
    out->hotplug_quiet_ms = now >= hotplug.last_activity_ns ?
        (now - hotplug.last_activity_ns) / 1000000u : 0u;
    out->violations = input_violations + session_violations + ui_violations;
    if (!scheduler_validate_runtime_invariants(&scheduler))
        out->violations += scheduler.violations ? scheduler.violations : 1u;
    if (!scheduler_validate_task_identity()) out->violations++;
    if (smp_failed_cpu_count() != 0) out->violations++;
    return true;
}

static bool runtime_services_ready(const runtime_ready_snapshot_t *snapshot,
                                   uint64_t quiet_ms)
{
    return snapshot && snapshot->cpus_expected != 0 &&
        snapshot->cpus_online == snapshot->cpus_expected &&
        snapshot->scheduler_started && snapshot->dpc_initialized &&
        snapshot->shell_initialized && snapshot->shell_thread_started &&
        snapshot->input_valid && snapshot->modal_session_valid &&
        snapshot->modal_ui_valid && snapshot->pci_scan_complete &&
        snapshot->violations == 0 &&
        (!snapshot->hotplug_initialized ||
         (snapshot->hotplug_active == 0 &&
          snapshot->hotplug_quiet_ms >= quiet_ms));
}

bool runtime_ready_wait_and_publish(uint64_t timeout_ms,
                                    uint64_t required_quiet_ms)
{
    if (!timeout_ms) timeout_ms = 10000;
    if (!required_quiet_ms) required_quiet_ms = 1000;
    if (!ready_state_advance(RUNTIME_READY_WAITING_SERVICES,
                             RUNTIME_READY_WAITING_DRIVERS))
        return false;
    uint64_t started = timer_get_uptime_ms();
    uint64_t generation = UINT64_MAX;
    runtime_ready_snapshot_t snapshot = {0};
    while (timer_get_uptime_ms() - started <= timeout_ms) {
        (void)runtime_services_snapshot(&snapshot);
        if (snapshot.hotplug_generation != generation)
            generation = snapshot.hotplug_generation;
        if (runtime_services_ready(&snapshot, required_quiet_ms)) {
            __atomic_store_n(&g_published_ns, clock_monotonic_ns(),
                             __ATOMIC_RELEASE);
            if (!ready_state_advance(RUNTIME_READY_WAITING_DRIVERS,
                                     RUNTIME_READY_READY))
                return false;
            runtime_record_t record;
            record_reset(&record);
            record_text(&record, "\n[BOOT][RUNTIME_READY] PASS cpus=");
            record_u64(&record, snapshot.cpus_online);
            record_char(&record, '/'); record_u64(&record, snapshot.cpus_expected);
            record_text(&record, " scheduler=1 dpc=1 shell=1 input=1 modal=1 pci=1 hotplug_active=");
            record_u64(&record, snapshot.hotplug_active);
            record_text(&record, " hotplug_quiet_ms=");
            record_u64(&record, snapshot.hotplug_quiet_ms);
            record_emit(&record);
            return true;
        }
        timer_sleep(10);
    }
    __atomic_store_n(&g_ready_state, RUNTIME_READY_FAILED, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_ready_violations, 1, __ATOMIC_RELAXED);
    return false;
}

void runtime_ready_mark_testing(bool autorun_enabled)
{
    __atomic_store_n(&g_autorun_enabled, autorun_enabled ? 1u : 0u,
                     __ATOMIC_RELEASE);
    if (autorun_enabled)
        (void)ready_state_advance(RUNTIME_READY_READY, RUNTIME_READY_TESTING);
}

void runtime_ready_mark_test_complete(bool autorun_enabled, bool passed)
{
    runtime_record_t record;
    runtime_ready_state_t state = (runtime_ready_state_t)__atomic_load_n(
        &g_ready_state, __ATOMIC_ACQUIRE);
    __atomic_store_n(&g_autorun_enabled, autorun_enabled ? 1u : 0u,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_autorun_passed,
                     (!autorun_enabled || passed) ? 1u : 0u,
                     __ATOMIC_RELEASE);
    bool runtime_ok = state == RUNTIME_READY_READY ||
                      state == RUNTIME_READY_TESTING;
    if (runtime_ok && (!autorun_enabled || passed)) {
        __atomic_store_n(&g_test_ready_ns, clock_monotonic_ns(),
                         __ATOMIC_RELEASE);
        __atomic_store_n(&g_ready_state, RUNTIME_READY_TEST_READY,
                         __ATOMIC_RELEASE);
    } else {
        __atomic_store_n(&g_ready_state, RUNTIME_READY_FAILED,
                         __ATOMIC_RELEASE);
    }
    selftest_summary_t summary = {0};
    if (autorun_enabled) selftest_last_summary(&summary);
    record_reset(&record);
    record_text(&record, runtime_ok && (!autorun_enabled || passed) ?
        "\n[BOOT][TEST_READY] PASS autorun=" :
        "\n[BOOT][TEST_READY] FAIL autorun=");
    record_u64(&record, autorun_enabled ? 1u : 0u);
    record_text(&record, " selftests=");
    record_u64(&record, summary.pass + summary.fail + summary.skip);
    if (!runtime_ok) record_text(&record, " reason=runtime");
    else if (autorun_enabled && !passed) record_text(&record, " reason=selftest");
    record_emit(&record);
}

bool runtime_ready_is_ready(void)
{
    uint32_t state = __atomic_load_n(&g_ready_state, __ATOMIC_ACQUIRE);
    return state == RUNTIME_READY_READY || state == RUNTIME_READY_TESTING ||
           state == RUNTIME_READY_TEST_READY;
}

bool runtime_ready_is_test_ready(void)
{
    return __atomic_load_n(&g_ready_state, __ATOMIC_ACQUIRE) ==
           RUNTIME_READY_TEST_READY;
}

bool runtime_ready_snapshot(runtime_ready_snapshot_t *out)
{
    if (!out) return false;
    *out = (runtime_ready_snapshot_t){0};
    (void)runtime_services_snapshot(out);
    out->state = (runtime_ready_state_t)__atomic_load_n(&g_ready_state,
                                                        __ATOMIC_ACQUIRE);
    out->autorun_enabled = __atomic_load_n(&g_autorun_enabled,
                                           __ATOMIC_ACQUIRE);
    out->autorun_passed = __atomic_load_n(&g_autorun_passed,
                                          __ATOMIC_ACQUIRE);
    out->published_ns = __atomic_load_n(&g_published_ns, __ATOMIC_ACQUIRE);
    out->test_ready_ns = __atomic_load_n(&g_test_ready_ns, __ATOMIC_ACQUIRE);
    out->violations += __atomic_load_n(&g_ready_violations, __ATOMIC_ACQUIRE);
    return true;
}

bool runtime_ready_validate(uint64_t *out_violations)
{
    runtime_ready_snapshot_t snapshot;
    uint64_t violations = 0;
    if (!runtime_ready_snapshot(&snapshot)) violations++;
    else {
        if (snapshot.state < RUNTIME_READY_READY ||
            snapshot.state == RUNTIME_READY_FAILED) violations++;
        if (snapshot.published_ns == 0) violations++;
        if (!runtime_services_ready(&snapshot, 0)) violations++;
        violations += snapshot.violations;
    }
    if (out_violations) *out_violations = violations;
    return violations == 0;
}
