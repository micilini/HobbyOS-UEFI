#include "arch_selftest.h"

#ifdef HOBBYOS_ARCH_TEST

#include "cpu.h"
#include "mmio.h"
#include "../core/idt.h"
#include "../core/panic.h"
#include "../core/scheduler.h"
#include "../drivers/serial.h"
#include "../libc/string.h"
#include "../smp/smp_topology.h"

#include <stddef.h>

#define ARCH_TEST_FS_BASE_MSR 0xc0000100u
#define ARCH_TEST_PKRS_MSR 0x000006e1u
#define ARCH_TEST_TSC_MSR 0x10u
#define ARCH_TEST_ITERATIONS 256u
#define ARCH_TEST_OBSERVER_POLL_LIMIT 200000000u

arch_test_state_t g_arch_test_state;
static volatile uint64_t g_arch_test_mmio_cells[HOBBYOS_MAX_CPUS];
static volatile uint64_t g_arch_test_task_cell;
volatile uint32_t g_arch_test_invalid_msr;

arch_test_unhandled_gp_observation_t g_arch_test_unhandled_gp_observation;

_Static_assert(sizeof(arch_test_unhandled_gp_observation_t) == 32u,
               "architecture unhandled #GP observation layout");
_Static_assert(offsetof(arch_test_unhandled_gp_observation_t, count) == 0u,
               "architecture unhandled #GP count offset");
_Static_assert(offsetof(arch_test_unhandled_gp_observation_t,
                        context_valid) == 4u,
               "architecture unhandled #GP context offset");
_Static_assert(offsetof(arch_test_unhandled_gp_observation_t, rip) == 8u,
               "architecture unhandled #GP RIP offset");
_Static_assert(offsetof(arch_test_unhandled_gp_observation_t,
                        error_code) == 16u,
               "architecture unhandled #GP error offset");
_Static_assert(offsetof(arch_test_unhandled_gp_observation_t, cs) == 24u,
               "architecture unhandled #GP CS offset");

void arch_selftest_record_unhandled_gp(uint64_t rip, uint64_t error_code,
                                       uint64_t cs)
{
    arch_test_unhandled_gp_observation_t *observation =
        &g_arch_test_unhandled_gp_observation;
    uint32_t count = __atomic_load_n(&observation->count, __ATOMIC_RELAXED);
    uint32_t context_valid = error_code == 0u && cs == 8u;

    if (count == 0u)
    {
        observation->rip = rip;
        observation->error_code = error_code;
        observation->cs = cs;
        __atomic_store_n(&observation->context_valid, context_valid,
                         __ATOMIC_RELAXED);
    }
    else if (observation->rip != rip ||
             observation->error_code != error_code ||
             observation->cs != cs || !context_valid)
    {
        __atomic_store_n(&observation->context_valid, 0u, __ATOMIC_RELAXED);
    }

    __atomic_store_n(&observation->count, count + 1u, __ATOMIC_RELEASE);
}

_Static_assert(sizeof(arch_test_state_t) == 1424u,
               "architecture test state layout");

static void arch_cpuid_raw(uint32_t leaf, uint32_t subleaf,
                           uint32_t *eax, uint32_t *ebx,
                           uint32_t *ecx, uint32_t *edx)
{
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(subleaf)
                     : "memory");
}

static bool arch_emit(const char *format, const char *status, uint32_t first,
                      uint32_t second, uint32_t third,
                      uint32_t fourth, uint32_t fifth, uint32_t sixth,
                      uint32_t seventh)
{
    char record[256];
    int required = ksnprintf(record, sizeof(record), format,
                             status, (unsigned int)first, (unsigned int)second,
                             (unsigned int)third, (unsigned int)fourth,
                             (unsigned int)fifth, (unsigned int)sixth,
                             (unsigned int)seventh);
    if (required < 0 || (size_t)required >= sizeof(record))
    {
        __atomic_store_n(&g_arch_test_state.error, 1, __ATOMIC_RELEASE);
        serial_write_all("[ARCH_TEST][RECORD_ERROR] status=FAIL\n");
        return false;
    }
    serial_write_all(record);
    return true;
}

static bool arch_record_error(void)
{
    __atomic_store_n(&g_arch_test_state.error, 1, __ATOMIC_RELEASE);
    serial_write_all("[ARCH_TEST][RECORD_ERROR] status=FAIL\n");
    return false;
}

static bool arch_emit_cpuid(const char *case_name, const char *domain,
                            uint32_t cpu_id, uint32_t leaf,
                            uint32_t subleaf, bool accepted,
                            const uint32_t api[4], bool raw_valid,
                            const uint32_t raw[4], bool legacy_valid,
                            const uint32_t legacy[4])
{
    char record[512];
    int required = ksnprintf(
        record, sizeof(record),
        "[ARCH_TEST][CPUID] case=%s domain=%s cpu_id=%u leaf=0x%x "
        "subleaf=%u accepted=%u api_eax=0x%x api_ebx=0x%x "
        "api_ecx=0x%x api_edx=0x%x raw_valid=%u raw_eax=0x%x "
        "raw_ebx=0x%x raw_ecx=0x%x raw_edx=0x%x legacy_valid=%u "
        "legacy_eax=0x%x legacy_ebx=0x%x legacy_ecx=0x%x "
        "legacy_edx=0x%x\n",
        case_name, domain, (unsigned int)cpu_id, (unsigned int)leaf,
        (unsigned int)subleaf, accepted ? 1u : 0u,
        (unsigned int)api[0], (unsigned int)api[1],
        (unsigned int)api[2], (unsigned int)api[3], raw_valid ? 1u : 0u,
        (unsigned int)raw[0], (unsigned int)raw[1],
        (unsigned int)raw[2], (unsigned int)raw[3],
        legacy_valid ? 1u : 0u, (unsigned int)legacy[0],
        (unsigned int)legacy[1], (unsigned int)legacy[2],
        (unsigned int)legacy[3]);
    if (required < 0 || (size_t)required >= sizeof(record))
        return arch_record_error();
    serial_write_all(record);
    return true;
}

static bool arch_emit_msr_read(const char *case_name, uint32_t cpu_id,
                               uint32_t index, bool result,
                               uint64_t before, uint64_t after,
                               uint32_t if_before, uint32_t if_after)
{
    char record[320];
    int required = ksnprintf(
        record, sizeof(record),
        "[ARCH_TEST][MSR] case=%s cpu_id=%u index=0x%x result=%u "
        "before=0x%llx after=0x%llx if_before=%u if_after=%u\n",
        case_name, (unsigned int)cpu_id, (unsigned int)index,
        result ? 1u : 0u, (unsigned long long)before,
        (unsigned long long)after, (unsigned int)if_before,
        (unsigned int)if_after);
    if (required < 0 || (size_t)required >= sizeof(record))
        return arch_record_error();
    serial_write_all(record);
    return true;
}

static bool arch_emit_msr_write(const char *case_name, uint32_t cpu_id,
                                uint32_t index, bool result, uint64_t value,
                                uint32_t if_before, uint32_t if_after)
{
    char record[256];
    int required = ksnprintf(
        record, sizeof(record),
        "[ARCH_TEST][MSR] case=%s cpu_id=%u index=0x%x result=%u "
        "value=0x%llx if_before=%u if_after=%u\n",
        case_name, (unsigned int)cpu_id, (unsigned int)index,
        result ? 1u : 0u, (unsigned long long)value,
        (unsigned int)if_before, (unsigned int)if_after);
    if (required < 0 || (size_t)required >= sizeof(record))
        return arch_record_error();
    serial_write_all(record);
    return true;
}

static bool arch_emit_msr_fs(uint32_t cpu_id, bool initial_read,
                             uint64_t original, bool temporary_write,
                             uint64_t temporary, bool temporary_read,
                             uint64_t readback, bool restore_write,
                             bool restore_read, uint64_t restored,
                             uint32_t if_before, uint32_t if_after)
{
    char record[448];
    int required = ksnprintf(
        record, sizeof(record),
        "[ARCH_TEST][MSR] case=fs-base cpu_id=%u index=0x%x "
        "initial_read=%u original=0x%llx temporary_write=%u "
        "temporary=0x%llx temporary_read=%u readback=0x%llx "
        "restore_write=%u restore_read=%u restored=0x%llx "
        "if_before=%u if_after=%u\n",
        (unsigned int)cpu_id, (unsigned int)ARCH_TEST_FS_BASE_MSR,
        initial_read ? 1u : 0u, (unsigned long long)original,
        temporary_write ? 1u : 0u, (unsigned long long)temporary,
        temporary_read ? 1u : 0u, (unsigned long long)readback,
        restore_write ? 1u : 0u, restore_read ? 1u : 0u,
        (unsigned long long)restored, (unsigned int)if_before,
        (unsigned int)if_after);
    if (required < 0 || (size_t)required >= sizeof(record))
        return arch_record_error();
    serial_write_all(record);
    return true;
}

typedef struct
{
    uint32_t calls;
    uint32_t succeed_on;
} arch_budget_probe_t;

static bool arch_budget_probe_condition(void *context)
{
    arch_budget_probe_t *probe = context;
    probe->calls++;
    return probe->calls == probe->succeed_on;
}

static uint32_t arch_budget_selftest(void)
{
    uint32_t checks = 0;
    arch_budget_probe_t probe = {.calls = 0, .succeed_on = 1};
    if (arch_test_poll_budget(arch_budget_probe_condition, &probe, 0) &&
        probe.calls == 1)
        checks |= 1u << 0;
    probe = (arch_budget_probe_t){.calls = 0, .succeed_on = UINT32_MAX};
    if (!arch_test_poll_budget(arch_budget_probe_condition, &probe, 0) &&
        probe.calls == 1)
        checks |= 1u << 1;
    probe = (arch_budget_probe_t){.calls = 0, .succeed_on = 2};
    if (arch_test_poll_budget(arch_budget_probe_condition, &probe, 1) &&
        probe.calls == 2)
        checks |= 1u << 2;
    probe = (arch_budget_probe_t){.calls = 0, .succeed_on = 4};
    if (arch_test_poll_budget(arch_budget_probe_condition, &probe, 3) &&
        probe.calls == 4)
        checks |= 1u << 3;
    probe = (arch_budget_probe_t){.calls = 0, .succeed_on = UINT32_MAX};
    if (!arch_test_poll_budget(arch_budget_probe_condition, &probe, 3) &&
        probe.calls == 4)
        checks |= 1u << 4;
    probe = (arch_budget_probe_t){.calls = 0, .succeed_on = 5};
    if (!arch_test_poll_budget(arch_budget_probe_condition, &probe, 3) &&
        probe.calls == 4)
        checks |= 1u << 5;
    return checks;
}

__attribute__((noinline)) void arch_selftest_gdb_done(void)
{
    __asm__ volatile("" : : : "memory");
}

__attribute__((noinline)) void arch_test_runtime_complete_point(void)
{
    __asm__ volatile("" : : : "memory");
}

static uint32_t arch_cpuid_selftest(void)
{
    uint32_t raw[4] = {0};
    uint32_t checked[4] = {0};
    uint32_t legacy[4] = {0};
    uint32_t maximum = 0;
    uint32_t extended_maximum = 0;
    uint32_t rejected[4] = {1, 1, 1, 1};
    uint32_t checks = 0;
    uint32_t cpu_id = UINT32_MAX;

    if (!cpu_local_arch_id(&cpu_id))
        return checks;

    arch_cpuid_raw(0, 0, &maximum, &raw[1], &raw[2], &raw[3]);
    raw[0] = maximum;
    if (!cpu_get_cpuid_count(0, 0, &checked[0], &checked[1],
                             &checked[2], &checked[3]) ||
        checked[0] != maximum || checked[1] != raw[1] ||
        checked[2] != raw[2] || checked[3] != raw[3] ||
        !arch_emit_cpuid("basic-maximum", "basic", cpu_id, 0, 0, true,
                         checked, true, raw, false, legacy))
        return checks;
    checks |= 1u << 0;

    arch_cpuid_raw(7, 0, &raw[0], &raw[1], &raw[2], &raw[3]);
    if (!cpu_get_cpuid_count(7, 0, &checked[0], &checked[1],
                             &checked[2], &checked[3]))
        return checks;
    cpu_get_cpuid(7, &legacy[0], &legacy[1], &legacy[2], &legacy[3]);
    for (uint32_t index = 0; index < 4; index++)
        if (checked[index] != raw[index] || legacy[index] != raw[index])
            return checks;
    if (!arch_emit_cpuid("legacy-wrapper", "basic", cpu_id, 7, 0, true,
                         checked, true, raw, true, legacy))
        return checks;
    checks |= 1u << 1;

    uint32_t rejected_leaf = maximum == UINT32_MAX ? maximum : maximum + 1u;
    bool rejected_accepted = cpu_get_cpuid_count(
        rejected_leaf, 0, &rejected[0], &rejected[1],
        &rejected[2], &rejected[3]);
    if (maximum != UINT32_MAX && rejected_accepted)
        return checks;
    for (uint32_t index = 0; index < 4; index++)
        if (rejected[index] != 0)
            return checks;
    uint32_t no_raw[4] = {0};
    if (!arch_emit_cpuid("basic-rejected", "basic", cpu_id,
                         rejected_leaf, 0, rejected_accepted, rejected,
                         false, no_raw, false, no_raw))
        return checks;
    checks |= 1u << 2;

    arch_cpuid_raw(0x80000000u, 0, &extended_maximum,
                   &raw[1], &raw[2], &raw[3]);
    raw[0] = extended_maximum;
    if (!cpu_get_cpuid_count(0x80000000u, 0, &checked[0], &checked[1],
                             &checked[2], &checked[3]))
        return checks;
    for (uint32_t index = 0; index < 4; index++)
        if (checked[index] != raw[index])
            return checks;
    if (!arch_emit_cpuid("extended-maximum", "extended", cpu_id,
                         0x80000000u, 0, true, checked, true, raw,
                         false, no_raw))
        return checks;
    rejected[0] = rejected[1] = rejected[2] = rejected[3] = 1;
    uint32_t extended_rejected_leaf = extended_maximum == UINT32_MAX
                                          ? extended_maximum
                                          : extended_maximum + 1u;
    bool extended_accepted = cpu_get_cpuid_count(
        extended_rejected_leaf, 0, &rejected[0], &rejected[1],
        &rejected[2], &rejected[3]);
    if (extended_maximum != UINT32_MAX && extended_accepted)
        return checks;
    for (uint32_t index = 0; index < 4; index++)
        if (rejected[index] != 0)
            return checks;
    if (!arch_emit_cpuid("extended-rejected", "extended", cpu_id,
                         extended_rejected_leaf, 0, extended_accepted,
                         rejected, false, no_raw, false, no_raw))
        return checks;
    checks |= 1u << 3;

    uint32_t cache0[4] = {0};
    uint32_t cache1[4] = {0};
    uint32_t raw_cache0[4] = {0};
    uint32_t raw_cache1[4] = {0};
    if (!cpu_get_cpuid_count(4, 0, &cache0[0], &cache0[1],
                             &cache0[2], &cache0[3]) ||
        !cpu_get_cpuid_count(4, 1, &cache1[0], &cache1[1],
                             &cache1[2], &cache1[3]))
        return checks;
    arch_cpuid_raw(4, 0, &raw_cache0[0], &raw_cache0[1],
                   &raw_cache0[2], &raw_cache0[3]);
    for (uint32_t index = 0; index < 4; index++)
        if (cache0[index] != raw_cache0[index])
            return checks;
    if (!arch_emit_cpuid("cache", "basic", cpu_id, 4, 0, true,
                         cache0, true, raw_cache0, false, no_raw))
        return checks;
    checks |= 1u << 4;
    arch_cpuid_raw(4, 1, &raw_cache1[0], &raw_cache1[1],
                   &raw_cache1[2], &raw_cache1[3]);
    bool distinct = false;
    for (uint32_t index = 0; index < 4; index++)
    {
        if (cache1[index] != raw_cache1[index])
            return checks;
        if (cache0[index] != cache1[index])
            distinct = true;
    }
    if (!distinct)
        return checks;
    if (!arch_emit_cpuid("cache", "basic", cpu_id, 4, 1, true,
                         cache1, true, raw_cache1, false, no_raw))
        return checks;
    checks |= 1u << 5;
    return checks;
}

static uint32_t arch_msr_selftest(void)
{
    uint64_t tsc = 0;
    uint64_t unchanged = 0x9e3779b97f4a7c15ULL;
    uint64_t fs_base = 0;
    uint64_t changed = 0;
    uint64_t observed = 0;
    uint64_t restored = 0;
    uint32_t checks = 0;
    uint32_t cpu_id = UINT32_MAX;
    static const uint32_t invalid_candidates[] = {
        0x00000480u, 0x00000570u, 0x000006a0u, 0x000006a2u,
        0x000006e0u, 0x00000802u, 0x00000d90u, 0x00000da0u,
        0xc0000103u,
        0x00000006u, 0x00000007u, 0x00000009u, 0x000005ffu,
        0x40000100u, 0x7fffffffu, 0xdeadbeefu, 0xffffffffu};

    if (!cpu_local_arch_id(&cpu_id))
        return checks;
    uint32_t if_before = irq_are_enabled() ? 1u : 0u;
    bool tsc_read = cpu_read_msr_safe(ARCH_TEST_TSC_MSR, &tsc);
    uint32_t if_after = irq_are_enabled() ? 1u : 0u;
    if (!arch_emit_msr_read("tsc", cpu_id, ARCH_TEST_TSC_MSR, tsc_read,
                            0, tsc, if_before, if_after) || !tsc_read ||
        if_before != if_after)
        return checks;
    checks |= 1u << 0;
    uint32_t invalid_msr = 0;
    uint32_t attempted_msr = 0;
    bool invalid_read_result = true;
    uint64_t invalid_before = unchanged;
    for (size_t index = 0;
         index < sizeof(invalid_candidates) / sizeof(invalid_candidates[0]);
         index++)
    {
        unchanged = 0x9e3779b97f4a7c15ULL;
        attempted_msr = invalid_candidates[index];
        invalid_before = unchanged;
        invalid_read_result = cpu_read_msr_safe(attempted_msr, &unchanged);
        if (!invalid_read_result)
        {
            invalid_msr = attempted_msr;
            break;
        }
    }
    if_after = irq_are_enabled() ? 1u : 0u;
    if (!arch_emit_msr_read("invalid-read", cpu_id, attempted_msr,
                            invalid_read_result, invalid_before, unchanged,
                            if_before, if_after) || if_before != if_after)
        return checks;
    if (invalid_msr != 0)
    {
        if (unchanged != 0x9e3779b97f4a7c15ULL)
            return checks;
        g_arch_test_invalid_msr = invalid_msr;
        checks |= 1u << 1;
        __atomic_store_n(&g_arch_test_state.safe_read_failure, 1,
                         __ATOMIC_RELEASE);
    }

    bool invalid_write_result = cpu_write_msr_safe(ARCH_TEST_PKRS_MSR,
                                                    1ULL << 32);
    if_after = irq_are_enabled() ? 1u : 0u;
    if (!arch_emit_msr_write("invalid-write", cpu_id,
                             ARCH_TEST_PKRS_MSR, invalid_write_result,
                             1ULL << 32, if_before, if_after) ||
        invalid_write_result || if_before != if_after)
        return checks;
    checks |= 1u << 2;
    __atomic_store_n(&g_arch_test_state.safe_write_failure, 1,
                     __ATOMIC_RELEASE);

    bool initial_read = cpu_read_msr_safe(ARCH_TEST_FS_BASE_MSR, &fs_base);
    bool temporary_write = false;
    bool temporary_read = false;
    bool restore_write = false;
    bool restore_read = false;
    if (initial_read)
    {
        checks |= 1u << 3;
        changed = fs_base ^ 1u;
        temporary_write = cpu_write_msr_safe(ARCH_TEST_FS_BASE_MSR, changed);
        if (temporary_write)
        {
            temporary_read = cpu_read_msr_safe(ARCH_TEST_FS_BASE_MSR,
                                                &observed);
            restore_write = cpu_write_msr_safe(ARCH_TEST_FS_BASE_MSR,
                                                fs_base);
            if (restore_write)
                restore_read = cpu_read_msr_safe(ARCH_TEST_FS_BASE_MSR,
                                                  &restored);
        }
    }
    if_after = irq_are_enabled() ? 1u : 0u;
    if (!arch_emit_msr_fs(cpu_id, initial_read, fs_base, temporary_write,
                          changed, temporary_read, observed, restore_write,
                          restore_read, restored, if_before, if_after))
        return checks;
    if (!initial_read || !temporary_write || !temporary_read ||
        observed != changed || !restore_write || !restore_read ||
        restored != fs_base || if_before != if_after)
        return checks;
    checks |= 1u << 4;
    return checks;
}

static bool arch_tcg_rdmsr_compatibility(void)
{
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;

    arch_cpuid_raw(1, 0, &eax, &ebx, &ecx, &edx);
    if ((ecx & (1u << 31)) == 0)
        return false;
    arch_cpuid_raw(0x40000000u, 0, &eax, &ebx, &ecx, &edx);
    return ebx == 0x54474354u && ecx == 0x43544743u &&
           edx == 0x47435447u;
}

bool arch_selftest_early(void)
{
    uint32_t if_before = irq_are_enabled() ? 1u : 0u;
    uint32_t cpuid_checks = arch_cpuid_selftest();
    uint32_t msr_checks = arch_msr_selftest();
    uint32_t budget_checks = arch_budget_selftest();
    __atomic_store_n(&g_arch_test_state.budget_checks, budget_checks,
                     __ATOMIC_RELEASE);
    bool cpuid_ok = cpuid_checks == 0x3fu;
    bool tcg_rdmsr_limited = arch_tcg_rdmsr_compatibility() &&
                             msr_checks == 0x1du;
    bool msr_ok = msr_checks == 0x1fu || tcg_rdmsr_limited;
    bool budget_ok = budget_checks == 0x3fu;
    uint32_t if_after = irq_are_enabled() ? 1u : 0u;
    bool pass = cpuid_ok && msr_ok && budget_ok && if_before == if_after &&
                if_before == 0u;

    if (!arch_emit("[ARCH_TEST][BUDGET] status=%s checks=%u expected=%u\n",
                   budget_ok ? "PASS" : "FAIL", budget_checks, 0x3fu,
                   0, 0, 0, 0, 0))
        pass = false;

    __atomic_store_n(&g_arch_test_state.early_pass, pass ? 1u : 0u,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_arch_test_state.early_done, 1, __ATOMIC_RELEASE);
    arch_selftest_gdb_done();
    if (!arch_emit("[ARCH_TEST][EARLY] status=%s cpuid_checks=%u "
                   "msr_checks=%u read_gp=%u write_gp=%u "
                   "if_preserved=%u invalid_msr=%u rdmsr_limited=%u\n",
                   pass ? "PASS" : "FAIL", cpuid_checks,
                   msr_checks,
                   g_arch_test_state.safe_read_failure,
                   g_arch_test_state.safe_write_failure,
                   if_before == if_after ? 1u : 0u,
                   g_arch_test_invalid_msr,
                   tcg_rdmsr_limited ? 1u : 0u))
        return false;
    return pass;
}

static uint32_t arch_target_mask(uint32_t count)
{
    return count == 32u ? UINT32_MAX : ((1u << count) - 1u);
}

static void arch_runtime_fail(void)
{
    __atomic_store_n(&g_arch_test_state.error, 1, __ATOMIC_RELEASE);
}

typedef struct
{
    volatile uint32_t *value;
    uint32_t mask;
    uint32_t expected;
} arch_mask_condition_t;

static bool arch_mask_condition(void *context)
{
    arch_mask_condition_t *condition = context;
    uint32_t value = __atomic_load_n(condition->value, __ATOMIC_ACQUIRE);
    return (value & condition->mask) == condition->expected;
}

static bool arch_wait_mask(volatile uint32_t *value, uint32_t mask,
                           uint32_t expected, uint32_t budget)
{
    arch_mask_condition_t condition = {
        .value = value, .mask = mask, .expected = expected};
    return arch_test_poll_budget(arch_mask_condition, &condition, budget);
}

static bool arch_emit_pin(bool pass, uint32_t if_before,
                          uint32_t if_during, uint32_t if_after,
                          uint32_t cpu_before, uint32_t cpu_during,
                          uint32_t cpu_after, uint32_t slot,
                          uint32_t snapshot_if1, uint32_t snapshot_if0)
{
    char record[320];
    int required = ksnprintf(
        record, sizeof(record),
        "[ARCH_TEST][PIN] status=%s if_before=%u if_during=%u "
        "if_after=%u cpu_before=%u cpu_during=%u cpu_after=%u slot=%u "
        "snapshot_if1=%u snapshot_if0=%u\n",
        pass ? "PASS" : "FAIL", (unsigned int)if_before,
        (unsigned int)if_during, (unsigned int)if_after,
        (unsigned int)cpu_before, (unsigned int)cpu_during,
        (unsigned int)cpu_after, (unsigned int)slot,
        (unsigned int)snapshot_if1, (unsigned int)snapshot_if0);
    if (required < 0 || (size_t)required >= sizeof(record))
        return arch_record_error();
    serial_write_all(record);
    return true;
}

static bool arch_emit_runtime(bool pass, uint32_t count, uint32_t joined,
                              uint32_t complete, uint32_t overlap,
                              uint32_t snapshots, uint32_t samples,
                              uint32_t unavailable, uint32_t active_samples,
                              uint32_t progress, uint32_t budget_ok)
{
    char record[384];
    int required = ksnprintf(
        record, sizeof(record),
        "[ARCH_TEST][RUNTIME] status=%s cpus=%u joined=%u complete=%u "
        "overlap=%u snapshots=%u concurrent_samples=%u "
        "concurrent_unavailable=%u concurrent_active=%u progress=%u "
        "budget_ok=%u\n",
        pass ? "PASS" : "FAIL", (unsigned int)count,
        (unsigned int)joined, (unsigned int)complete,
        (unsigned int)overlap, (unsigned int)snapshots,
        (unsigned int)samples, (unsigned int)unavailable,
        (unsigned int)active_samples, (unsigned int)progress,
        (unsigned int)budget_ok);
    if (required < 0 || (size_t)required >= sizeof(record))
        return arch_record_error();
    serial_write_all(record);
    return true;
}

static void arch_runtime_task(void *argument)
{
    (void)argument;
    __atomic_store_n(&g_arch_test_state.pin_task_started, 1,
                     __ATOMIC_RELEASE);
    if (!arch_wait_mask(&g_arch_test_state.runtime_started, 1u, 1u,
                        ARCH_TEST_OBSERVER_POLL_LIMIT))
    {
        arch_runtime_fail();
        __atomic_store_n(&g_arch_test_state.pin_task_done, 1,
                         __ATOMIC_RELEASE);
        thread_exit_normal();
    }

    uint32_t if_before = irq_are_enabled() ? 1u : 0u;
    uint32_t cpu_before = UINT32_MAX;
    uint32_t cpu_during = UINT32_MAX;
    uint32_t cpu_after = UINT32_MAX;
    (void)cpu_local_arch_id(&cpu_before);
    mmio_trace_token_t token = mmio_trace_begin(
        (void *)&g_arch_test_task_cell, 8, MMIO_TRACE_OPERATION_WRITE,
        0x6172636870696e31ULL, true);
    uint32_t if_during = irq_are_enabled() ? 1u : 0u;
    bool during_id_valid = cpu_local_arch_id(&cpu_during);
    g_arch_test_task_cell = 0x6172636870696e31ULL;
    mmio_trace_complete(token, g_arch_test_task_cell, true);
    uint32_t if_after = irq_are_enabled() ? 1u : 0u;
    bool after_id_valid = cpu_local_arch_id(&cpu_after);

    mmio_trace_snapshot_t snapshot = {0};
    uint32_t snapshot_if1_before = irq_are_enabled() ? 1u : 0u;
    bool snapshot_if1_result = mmio_trace_snapshot_current(&snapshot, 32u);
    uint32_t snapshot_if1_after = irq_are_enabled() ? 1u : 0u;
    irq_flags_t outer_flags = irq_save();
    uint32_t snapshot_if0_before = irq_are_enabled() ? 1u : 0u;
    bool snapshot_if0_result = mmio_trace_snapshot_current(&snapshot, 32u);
    uint32_t snapshot_if0_after = irq_are_enabled() ? 1u : 0u;
    irq_restore(outer_flags);
    uint32_t snapshot_restore_if = irq_are_enabled() ? 1u : 0u;

    uint32_t snapshot_if1 = snapshot_if1_result && snapshot_if1_before == 1u &&
                            snapshot_if1_after == 1u;
    uint32_t snapshot_if0 = snapshot_if0_result && snapshot_if0_before == 0u &&
                            snapshot_if0_after == 0u &&
                            snapshot_restore_if == 1u;
    uint32_t count = __atomic_load_n(&g_cpu_count, __ATOMIC_ACQUIRE);
    bool token_identity = token.active && token.pin_owned &&
                          token.slot < count && during_id_valid &&
                          token.cpu_id == cpu_during &&
                          __atomic_load_n(&g_cpus[token.slot].apic_id,
                                          __ATOMIC_RELAXED) == cpu_during;
    bool pass = if_before == 1u && if_during == 0u && if_after == 1u &&
                token_identity && after_id_valid && snapshot_if1 &&
                snapshot_if0;

    __atomic_store_n(&g_arch_test_state.pin_if_before, if_before,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_arch_test_state.pin_if_during, if_during,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_arch_test_state.pin_if_after, if_after,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_arch_test_state.pin_cpu_before, cpu_before,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_arch_test_state.pin_cpu_during, cpu_during,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_arch_test_state.pin_cpu_after, cpu_after,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_arch_test_state.pin_slot, token.slot,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_arch_test_state.pin_snapshot_if1, snapshot_if1,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_arch_test_state.pin_snapshot_if0, snapshot_if0,
                     __ATOMIC_RELAXED);
    if (!arch_emit_pin(pass, if_before, if_during, if_after, cpu_before,
                       cpu_during, cpu_after, token.slot, snapshot_if1,
                       snapshot_if0))
        pass = false;
    __atomic_store_n(&g_arch_test_state.pin_task_pass, pass ? 1u : 0u,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_arch_test_state.pin_task_done, 1,
                     __ATOMIC_RELEASE);
    if (!pass)
        arch_runtime_fail();
    thread_exit_normal();
}

bool arch_selftest_start_runtime_task(void)
{
    return thread_create_named_with_class_flags(
        arch_runtime_task, NULL, TASK_CLASS_NORMAL, "arch-trace-observer",
        TASK_FLAG_SYSTEM | TASK_FLAG_KILL_PROTECTED);
}

void arch_test_timer_hook(uint32_t slot)
{
    if (!__atomic_load_n(&g_arch_test_state.early_pass, __ATOMIC_ACQUIRE))
        return;
    uint32_t count = __atomic_load_n(&g_cpu_count, __ATOMIC_ACQUIRE);
    if (count == 0 || count > HOBBYOS_MAX_CPUS || slot >= count)
    {
        arch_runtime_fail();
        return;
    }

    uint32_t started = 0;
    if (__atomic_compare_exchange_n(&g_arch_test_state.runtime_started,
                                    &started, 1, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        __atomic_store_n(&g_arch_test_state.target_count, count,
                         __ATOMIC_RELEASE);
    uint32_t target_count = __atomic_load_n(
        &g_arch_test_state.target_count, __ATOMIC_ACQUIRE);
    if (target_count != count)
    {
        if (target_count == 0)
            return;
        arch_runtime_fail();
        return;
    }

    uint32_t bit = 1u << slot;
    uint32_t target_mask = arch_target_mask(count);
    __atomic_fetch_or(&g_arch_test_state.joined_mask, bit,
                      __ATOMIC_ACQ_REL);
    if ((__atomic_load_n(&g_arch_test_state.joined_mask,
                         __ATOMIC_ACQUIRE) & target_mask) != target_mask)
        return;

    uint32_t idle = 0;
    if (!__atomic_compare_exchange_n(&g_arch_test_state.iterations[slot],
                                     &idle, UINT32_MAX, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    __atomic_fetch_or(&g_arch_test_state.active_mask, bit,
                      __ATOMIC_ACQ_REL);

    uint64_t initial_value = ((uint64_t)(slot + 1u) << 56) | 1u;
    mmio_write64_relaxed((void *)&g_arch_test_mmio_cells[slot],
                         initial_value);
    mmio_trace_snapshot_t snapshot = {0};
    bool valid = mmio_trace_snapshot_current(&snapshot, 8u) &&
                 snapshot.address ==
                     (uint64_t)(uintptr_t)&g_arch_test_mmio_cells[slot] &&
                 snapshot.value == initial_value && snapshot.slot == slot &&
                 snapshot.operation == MMIO_TRACE_OPERATION_WRITE &&
                 snapshot.width == 8u &&
                 snapshot.phase == MMIO_TRACE_PHASE_COMPLETE &&
                 snapshot.value_valid && snapshot.coherent &&
                 snapshot.sequence != 0 && !(snapshot.sequence & 1u) &&
                 snapshot.cpu_id ==
                     __atomic_load_n(&g_cpus[slot].apic_id,
                                     __ATOMIC_RELAXED);

    bool active_ready = arch_wait_mask(&g_arch_test_state.active_mask,
                                       target_mask, target_mask,
                                       ARCH_TEST_OBSERVER_POLL_LIMIT);
    if (!active_ready)
        valid = false;

    uint32_t observer_pending = 0;
    if (__atomic_compare_exchange_n(&g_arch_test_state.observer_ready,
                                    &observer_pending, 1, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        (void)arch_emit("[ARCH_TEST][TRACE_ACTIVE] status=%s cpus=%u "
                        "active=%u target=%u\n",
                        "READY", count, g_arch_test_state.active_mask,
                        target_mask, 0, 0, 0, 0);

    bool observer_released = arch_wait_mask(
        &g_arch_test_state.observer_release, 1u, 1u,
        ARCH_TEST_OBSERVER_POLL_LIMIT);
    if (!observer_released)
        valid = false;

    __atomic_fetch_or(&g_arch_test_state.publication_ready_mask, bit,
                      __ATOMIC_ACQ_REL);
    bool publication_ready = true;
    if (slot == 0)
    {
        publication_ready = arch_wait_mask(
            &g_arch_test_state.publication_ready_mask, target_mask,
            target_mask, ARCH_TEST_OBSERVER_POLL_LIMIT);
        for (uint32_t observed_slot = 0;
             observed_slot < count && publication_ready; observed_slot++)
        {
            mmio_trace_snapshot_t first = {0};
            if (!mmio_trace_snapshot_slot_for_test(observed_slot, &first, 32u))
            {
                publication_ready = false;
                break;
            }
            __atomic_store_n(
                &g_arch_test_state.concurrent_first_sequence[observed_slot],
                first.sequence, __ATOMIC_RELAXED);
            __atomic_store_n(
                &g_arch_test_state.concurrent_last_sequence[observed_slot],
                first.sequence, __ATOMIC_RELAXED);
        }
        __atomic_store_n(&g_arch_test_state.sampling_release, 1,
                         __ATOMIC_RELEASE);
    }
    else
    {
        publication_ready = arch_wait_mask(
            &g_arch_test_state.sampling_release, 1u, 1u,
            ARCH_TEST_OBSERVER_POLL_LIMIT);
    }
    if (!publication_ready)
        valid = false;

    __atomic_fetch_or(&g_arch_test_state.writers_started_mask, bit,
                      __ATOMIC_ACQ_REL);

    for (uint32_t iteration = 0; iteration < ARCH_TEST_ITERATIONS; iteration++)
    {
        uint64_t value = ((uint64_t)(slot + 1u) << 56) |
                         ((uint64_t)iteration << 1) | 1u;
        mmio_write64_relaxed((void *)&g_arch_test_mmio_cells[slot], value);
        if (!mmio_trace_snapshot_current(&snapshot, 8u) ||
            snapshot.address !=
                (uint64_t)(uintptr_t)&g_arch_test_mmio_cells[slot] ||
            snapshot.value != value || snapshot.slot != slot ||
            snapshot.operation != MMIO_TRACE_OPERATION_WRITE ||
            snapshot.width != 8u ||
            snapshot.phase != MMIO_TRACE_PHASE_COMPLETE ||
            !snapshot.value_valid || !snapshot.coherent ||
            snapshot.cpu_id !=
                __atomic_load_n(&g_cpus[slot].apic_id,
                                __ATOMIC_RELAXED))
            valid = false;

        uint64_t read_value = mmio_read64(
            (void *)&g_arch_test_mmio_cells[slot]);
        if (read_value != value ||
            !mmio_trace_snapshot_current(&snapshot, 8u) ||
            snapshot.value != value || snapshot.slot != slot ||
            snapshot.operation != MMIO_TRACE_OPERATION_READ ||
            snapshot.phase != MMIO_TRACE_PHASE_COMPLETE ||
            !snapshot.value_valid || !snapshot.coherent ||
            snapshot.cpu_id !=
                __atomic_load_n(&g_cpus[slot].apic_id,
                                __ATOMIC_RELAXED))
            valid = false;

        if (slot == 0 && publication_ready)
        {
            uint32_t finished = __atomic_load_n(
                &g_arch_test_state.writers_finished_mask, __ATOMIC_ACQUIRE);
            for (uint32_t observed_slot = 0; observed_slot < count;
                 observed_slot++)
            {
                mmio_trace_snapshot_t concurrent = {0};
                if (!mmio_trace_snapshot_slot_for_test(
                        observed_slot, &concurrent, 4u))
                {
                    __atomic_fetch_add(
                        &g_arch_test_state.concurrent_unavailable, 1,
                        __ATOMIC_RELAXED);
                    continue;
                }
                __atomic_fetch_add(&g_arch_test_state.concurrent_samples, 1,
                                   __ATOMIC_RELAXED);
                if ((finished & target_mask) != target_mask)
                    __atomic_fetch_add(
                        &g_arch_test_state.concurrent_active_samples, 1,
                        __ATOMIC_RELAXED);
                uint32_t first = __atomic_load_n(
                    &g_arch_test_state.concurrent_first_sequence[observed_slot],
                    __ATOMIC_RELAXED);
                __atomic_store_n(
                    &g_arch_test_state.concurrent_last_sequence[observed_slot],
                    concurrent.sequence, __ATOMIC_RELAXED);
                if (concurrent.sequence != first)
                    __atomic_fetch_or(
                        &g_arch_test_state.concurrent_progress_mask,
                        1u << observed_slot, __ATOMIC_RELAXED);
            }
        }
        if (__atomic_load_n(&g_arch_test_state.active_mask,
                            __ATOMIC_ACQUIRE) & ~bit)
            __atomic_fetch_or(&g_arch_test_state.overlap_mask, bit,
                              __ATOMIC_RELAXED);
        for (uint32_t delay = 0; delay < 128u; delay++)
            __asm__ volatile("pause");
    }

    __atomic_fetch_or(&g_arch_test_state.writers_finished_mask, bit,
                      __ATOMIC_RELEASE);
    if (slot == 0)
    {
        uint32_t other_mask = target_mask & ~bit;
        bool writers_finished = arch_wait_mask(
            &g_arch_test_state.writers_finished_mask, other_mask,
            other_mask, ARCH_TEST_OBSERVER_POLL_LIMIT);
        if (!writers_finished)
            valid = false;
        for (uint32_t observed_slot = 0; observed_slot < count;
             observed_slot++)
        {
            mmio_trace_snapshot_t final = {0};
            if (!mmio_trace_snapshot_slot_for_test(observed_slot, &final,
                                                   32u))
            {
                valid = false;
                continue;
            }
            uint32_t first = __atomic_load_n(
                &g_arch_test_state.concurrent_first_sequence[observed_slot],
                __ATOMIC_RELAXED);
            __atomic_store_n(
                &g_arch_test_state.concurrent_last_sequence[observed_slot],
                final.sequence, __ATOMIC_RELAXED);
            if (final.sequence != first)
                __atomic_fetch_or(
                    &g_arch_test_state.concurrent_progress_mask,
                    1u << observed_slot, __ATOMIC_RELAXED);
        }
        uint32_t progress = __atomic_load_n(
            &g_arch_test_state.concurrent_progress_mask, __ATOMIC_ACQUIRE);
        uint32_t samples = __atomic_load_n(
            &g_arch_test_state.concurrent_samples, __ATOMIC_ACQUIRE);
        uint32_t active_samples = __atomic_load_n(
            &g_arch_test_state.concurrent_active_samples, __ATOMIC_ACQUIRE);
        if ((progress & target_mask) != target_mask || samples < count ||
            (count > 1u && active_samples == 0))
            valid = false;
        __atomic_store_n(&g_arch_test_state.observer_budget_pass,
                         active_ready && observer_released &&
                                 publication_ready && writers_finished
                             ? 1u
                             : 0u,
                         __ATOMIC_RELEASE);
    }

    g_arch_test_state.observed_cpu_id[slot] = snapshot.cpu_id;
    g_arch_test_state.observed_slot[slot] = snapshot.slot;
    g_arch_test_state.observed_address[slot] = snapshot.address;
    g_arch_test_state.observed_value[slot] = snapshot.value;
    g_arch_test_state.observed_meta[slot] =
        (uint32_t)snapshot.operation |
        ((uint32_t)snapshot.width << 8) |
        ((uint32_t)snapshot.phase << 16) |
        ((uint32_t)snapshot.value_valid << 24);
    __atomic_store_n(&g_arch_test_state.iterations[slot],
                     ARCH_TEST_ITERATIONS, __ATOMIC_RELEASE);
    if (valid)
        __atomic_fetch_or(&g_arch_test_state.snapshot_mask, bit,
                          __ATOMIC_RELAXED);
    else
        arch_runtime_fail();
    __atomic_fetch_and(&g_arch_test_state.active_mask, ~bit,
                       __ATOMIC_RELEASE);
    uint32_t previous = __atomic_fetch_or(&g_arch_test_state.complete_mask,
                                           bit, __ATOMIC_ACQ_REL);
    uint32_t complete = previous | bit;
    if ((complete & target_mask) != target_mask)
        return;

    arch_test_runtime_complete_point();
    uint32_t overlap = __atomic_load_n(&g_arch_test_state.overlap_mask,
                                        __ATOMIC_ACQUIRE);
    uint32_t snapshots = __atomic_load_n(&g_arch_test_state.snapshot_mask,
                                          __ATOMIC_ACQUIRE);
    uint32_t error = __atomic_load_n(&g_arch_test_state.error,
                                      __ATOMIC_ACQUIRE);
    uint32_t samples = __atomic_load_n(
        &g_arch_test_state.concurrent_samples, __ATOMIC_ACQUIRE);
    uint32_t unavailable = __atomic_load_n(
        &g_arch_test_state.concurrent_unavailable, __ATOMIC_ACQUIRE);
    uint32_t active_samples = __atomic_load_n(
        &g_arch_test_state.concurrent_active_samples, __ATOMIC_ACQUIRE);
    uint32_t progress = __atomic_load_n(
        &g_arch_test_state.concurrent_progress_mask, __ATOMIC_ACQUIRE);
    uint32_t budget_ok = __atomic_load_n(
        &g_arch_test_state.observer_budget_pass, __ATOMIC_ACQUIRE);
    bool pass = snapshots == target_mask && error == 0 &&
                (progress & target_mask) == target_mask && budget_ok == 1u &&
                samples >= count &&
                (count == 1u || ((overlap & target_mask) != 0 &&
                                 active_samples != 0));
    if (!arch_emit_runtime(pass, count, g_arch_test_state.joined_mask,
                           complete, overlap, snapshots, samples,
                           unavailable, active_samples, progress, budget_ok))
        pass = false;
    if (!pass)
        arch_runtime_fail();

#ifdef HOBBYOS_ARCH_NEGATIVE_GP
    serial_write_all("[ARCH_TEST][UNRELATED_GP] action=execute\n");
    panic_config(PANIC_ACTION_HALT, 1);
    __asm__ volatile(
        ".globl arch_test_unrelated_gp_site\n\t"
        "arch_test_unrelated_gp_site:\n\t"
        "wrmsr\n\t"
        :
        : "c"(ARCH_TEST_PKRS_MSR), "a"(0u), "d"(1u)
        : "memory");
    arch_runtime_fail();
    serial_write_all("[ARCH_TEST][UNRELATED_GP_RETURNED] status=FAIL\n");
#endif
}

#endif
