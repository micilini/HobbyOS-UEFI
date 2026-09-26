#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>

#include "../kernel/src/core/io.h"
#include "../kernel/src/cpu/cpu.h"
#include "../kernel/src/cpu/mmio.h"
#include "../kernel/src/core/idt.h"
#include "../kernel/src/core/io.h"
#include "../kernel/src/smp/smp_topology.h"
#if HOBBYOS_DEBUG_ASSERT
#include "../kernel/src/cpu/arch_selftest.h"
#endif

SmpCpuInfo g_cpus[HOBBYOS_MAX_CPUS];
uint32_t g_cpu_count;
uint32_t g_bsp_apic_id;

const char _text_start[1] = {0};
const char _text_end[1] = {0};

static unsigned int checks;

#if HOBBYOS_DEBUG_ASSERT
static _Thread_local uint32_t host_cpu_id;
static _Thread_local bool host_irq_enabled = true;
static _Thread_local uint32_t host_pin_depth;
static _Thread_local bool host_migration_requested;
static _Thread_local uint32_t host_migration_target;
static _Thread_local uint32_t host_operation_cpu;
static _Thread_local uint32_t host_deferred_migrations;

irq_flags_t irq_save(void)
{
    irq_flags_t flags = host_irq_enabled ? (1ULL << 9) : 0;
    host_irq_enabled = false;
    host_pin_depth++;
    return flags;
}

void irq_restore(irq_flags_t flags)
{
    if (host_pin_depth == 0)
        return;
    host_pin_depth--;
    host_irq_enabled = (flags & (1ULL << 9)) != 0;
    if (host_pin_depth == 0 && host_irq_enabled && host_migration_requested)
    {
        host_cpu_id = host_migration_target;
        host_migration_requested = false;
    }
}

bool arch_host_cpu_local_arch_id(uint32_t *cpu_id)
{
    if (!cpu_id)
        return false;
    *cpu_id = host_cpu_id;
    return true;
}

void arch_host_mmio_access_point(void)
{
    host_operation_cpu = host_cpu_id;
    if (!host_migration_requested)
        return;
    if (host_irq_enabled)
    {
        host_cpu_id = host_migration_target;
        host_migration_requested = false;
    }
    else
    {
        host_deferred_migrations++;
    }
}
#endif

#define CHECK(condition)                                                     \
    do                                                                       \
    {                                                                        \
        checks++;                                                            \
        if (!(condition))                                                    \
        {                                                                    \
            fprintf(stderr, "ARCH_HOST_FAIL line=%u expression=%s\n",       \
                    (unsigned int)__LINE__, #condition);                     \
            return false;                                                    \
        }                                                                    \
    } while (0)

static void raw_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t output[4])
{
    __asm__ volatile("cpuid"
                     : "=a"(output[0]), "=b"(output[1]),
                       "=c"(output[2]), "=d"(output[3])
                     : "a"(leaf), "c"(subleaf)
                     : "memory");
}

static bool cpuid_tests(void)
{
    uint32_t raw[4] = {0};
    uint32_t actual[4] = {0};
    uint32_t legacy[4] = {0};
    uint32_t maximum = 0;
    uint32_t extended_maximum = 0;
    unsigned int start = checks;

    raw_cpuid(0, 0, raw);
    maximum = raw[0];
    CHECK(cpu_get_cpuid_count(0, 0, &actual[0], &actual[1],
                              &actual[2], &actual[3]));
    CHECK(memcmp(raw, actual, sizeof(raw)) == 0);

    if (maximum >= 7)
    {
        raw_cpuid(7, 0, raw);
        CHECK(cpu_get_cpuid_count(7, 0, &actual[0], &actual[1],
                                  &actual[2], &actual[3]));
        cpu_get_cpuid(7, &legacy[0], &legacy[1], &legacy[2], &legacy[3]);
        CHECK(memcmp(raw, actual, sizeof(raw)) == 0);
        CHECK(memcmp(raw, legacy, sizeof(raw)) == 0);
    }

    if (maximum != UINT32_MAX)
    {
        actual[0] = actual[1] = actual[2] = actual[3] = UINT32_MAX;
        CHECK(!cpu_get_cpuid_count(maximum + 1u, 0, &actual[0],
                                   &actual[1], &actual[2], &actual[3]));
        CHECK(actual[0] == 0 && actual[1] == 0 &&
              actual[2] == 0 && actual[3] == 0);
    }

    raw_cpuid(0x80000000u, 0, raw);
    extended_maximum = raw[0];
    if (extended_maximum != UINT32_MAX)
    {
        actual[0] = actual[1] = actual[2] = actual[3] = UINT32_MAX;
        CHECK(!cpu_get_cpuid_count(extended_maximum + 1u, 0,
                                   &actual[0], &actual[1],
                                   &actual[2], &actual[3]));
        CHECK(actual[0] == 0 && actual[1] == 0 &&
              actual[2] == 0 && actual[3] == 0);
    }

    CHECK(!cpu_get_cpuid_count(0x50000000u, 0, NULL, NULL, NULL, NULL));
    CHECK(cpu_get_cpuid_count(0, 0, NULL, NULL, NULL, NULL));

    if (maximum >= 4)
    {
        uint32_t first[4] = {0};
        uint32_t second[4] = {0};
        raw_cpuid(4, 0, raw);
        if ((raw[0] & 0x1fu) != 0)
        {
            CHECK(cpu_get_cpuid_count(4, 0, &first[0], &first[1],
                                      &first[2], &first[3]));
            raw_cpuid(4, 1, raw);
            if ((raw[0] & 0x1fu) != 0)
            {
                CHECK(cpu_get_cpuid_count(4, 1, &second[0], &second[1],
                                          &second[2], &second[3]));
                CHECK(memcmp(raw, second, sizeof(raw)) == 0);
                CHECK(memcmp(first, second, sizeof(first)) != 0);
            }
        }
    }

    printf("ARCH_HOST_CPUID_CHECKS=%u\n", checks - start);
    return true;
}

__attribute__((noinline)) uint64_t arch_probe_ordered_read64(void *address)
{
    return mmio_read64(address);
}

__attribute__((noinline)) uint64_t arch_probe_relaxed_read64(void *address)
{
    return mmio_read64_relaxed(address);
}

__attribute__((noinline)) void arch_probe_ordered_write64(void *address,
                                                          uint64_t value)
{
    mmio_write64(address, value);
}

__attribute__((noinline)) void arch_probe_relaxed_write64(void *address,
                                                          uint64_t value)
{
    mmio_write64_relaxed(address, value);
}

static bool mmio_tests(void)
{
    struct
    {
        uint64_t before;
        uint64_t value;
        uint64_t after;
    } cell = {
        .before = 0x1122334455667788ULL,
        .value = 0,
        .after = 0x8877665544332211ULL};
    unsigned int start = checks;

    arch_probe_relaxed_write64(&cell.value, 0x0123456789abcdefULL);
    CHECK(arch_probe_relaxed_read64(&cell.value) ==
          0x0123456789abcdefULL);
    arch_probe_ordered_write64(&cell.value, 0xfedcba9876543210ULL);
    CHECK(arch_probe_ordered_read64(&cell.value) ==
          0xfedcba9876543210ULL);
    CHECK(cell.before == 0x1122334455667788ULL);
    CHECK(cell.after == 0x8877665544332211ULL);

#if HOBBYOS_DEBUG_ASSERT
    uint32_t local_id = 0;
    CHECK(cpu_local_arch_id(&local_id));
    host_cpu_id = local_id;
    host_irq_enabled = true;
    host_pin_depth = 0;
    memset(g_cpus, 0, sizeof(g_cpus));
    g_cpu_count = 1;
    g_cpus[0].slot = 0;
    g_cpus[0].apic_id = local_id;

    mmio_trace_snapshot_t snapshot = {0};
    mmio_write32_relaxed(&cell.value, 0xa5a55a5au);
    CHECK(mmio_trace_snapshot_current(&snapshot, 4));
    CHECK(snapshot.coherent && snapshot.slot_valid && snapshot.cpu_valid);
    CHECK(snapshot.slot == 0 && snapshot.cpu_id == local_id);
    CHECK(snapshot.address == (uint64_t)(uintptr_t)&cell.value);
    CHECK(snapshot.operation == MMIO_TRACE_OPERATION_WRITE);
    CHECK(snapshot.width == 4 && snapshot.value == 0xa5a55a5au);
    CHECK(snapshot.value_valid && snapshot.phase == MMIO_TRACE_PHASE_COMPLETE);

    mmio_trace_token_t pending = mmio_trace_begin(
        &cell.value, 8, MMIO_TRACE_OPERATION_READ, 0, false);
    CHECK(pending.active);
    CHECK(pending.pin_owned && !host_irq_enabled && host_pin_depth == 1);
    CHECK(mmio_trace_snapshot_current(&snapshot, 1));
    CHECK(snapshot.phase == MMIO_TRACE_PHASE_ATTEMPT);
    CHECK(!snapshot.value_valid);
    mmio_trace_token_t nested = mmio_trace_begin(
        &cell.value, 8, MMIO_TRACE_OPERATION_READ, 0, false);
    CHECK(!nested.active);
    CHECK(nested.pin_owned && !host_irq_enabled && host_pin_depth == 2);
    mmio_trace_complete(nested, 0, false);
    CHECK(!host_irq_enabled && host_pin_depth == 1);
    CHECK(!mmio_trace_snapshot_current(&snapshot, 0));
    mmio_trace_complete(pending, cell.value, true);
    CHECK(host_irq_enabled && host_pin_depth == 0);
    CHECK(mmio_trace_snapshot_current(&snapshot, 1));
    CHECK(snapshot.phase == MMIO_TRACE_PHASE_COMPLETE &&
          snapshot.value_valid && snapshot.value == cell.value);

    g_cpu_count = 2;
    g_cpus[0].slot = 0;
    g_cpus[0].apic_id = 10;
    g_cpus[1].slot = 1;
    g_cpus[1].apic_id = 20;
    host_cpu_id = 10;
    host_migration_requested = true;
    host_migration_target = 20;
    host_deferred_migrations = 0;
    host_operation_cpu = UINT32_MAX;
    mmio_write64_relaxed(&cell.value, 0xdecafbad12345678ULL);
    CHECK(host_operation_cpu == 10);
    CHECK(host_deferred_migrations == 1);
    CHECK(host_cpu_id == 20 && host_irq_enabled && host_pin_depth == 0);
    host_cpu_id = 10;
    CHECK(mmio_trace_snapshot_current(&snapshot, 8));
    CHECK(snapshot.cpu_id == 10 && snapshot.slot == 0);
    CHECK(snapshot.value == 0xdecafbad12345678ULL);

    host_irq_enabled = false;
    mmio_write32_relaxed(&cell.value, 0x13579bdfu);
    CHECK(!host_irq_enabled && host_pin_depth == 0);
    CHECK(mmio_trace_snapshot_current(&snapshot, 8));
    CHECK(!host_irq_enabled);
    host_irq_enabled = true;

    g_cpu_count = 0;
    mmio_trace_token_t unavailable = mmio_trace_begin(
        &cell.value, 8, MMIO_TRACE_OPERATION_READ, 0, false);
    CHECK(!unavailable.active && unavailable.pin_owned && !host_irq_enabled);
    mmio_trace_complete(unavailable, 0, false);
    CHECK(host_irq_enabled && host_pin_depth == 0);
#endif

    printf("ARCH_HOST_MMIO_CHECKS=%u\n", checks - start);
    return true;
}

#if HOBBYOS_DEBUG_ASSERT
typedef struct
{
    uint32_t cpu_id;
    uint32_t slot;
    volatile uint32_t *ready;
    volatile uint32_t *go;
    volatile uint32_t *published;
    volatile uint32_t *done;
    volatile uint64_t *cell;
    volatile uint32_t failures;
} host_writer_t;

static void *host_writer(void *argument)
{
    host_writer_t *writer = argument;
    host_cpu_id = writer->cpu_id;
    host_irq_enabled = true;
    host_pin_depth = 0;
    __atomic_fetch_add(writer->ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(writer->go, __ATOMIC_ACQUIRE))
        sched_yield();
    for (uint32_t iteration = 1; iteration <= 50000u; iteration++)
    {
        uint64_t value = ((uint64_t)(writer->slot + 1u) << 56) | iteration;
        mmio_write64_relaxed((void *)writer->cell, value);
        mmio_trace_snapshot_t snapshot = {0};
        if (!mmio_trace_snapshot_current(&snapshot, 16u) ||
            snapshot.cpu_id != writer->cpu_id ||
            snapshot.slot != writer->slot || snapshot.value != value ||
            snapshot.sequence == 0 || (snapshot.sequence & 1u))
            __atomic_fetch_add(&writer->failures, 1, __ATOMIC_RELAXED);
        if (iteration == 1u)
            __atomic_fetch_add(writer->published, 1, __ATOMIC_RELEASE);
        if ((iteration & 255u) == 0)
            sched_yield();
    }
    __atomic_fetch_add(writer->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static bool concurrent_trace_tests(void)
{
    volatile uint64_t cells[2] = {0, 0};
    volatile uint32_t ready = 0;
    volatile uint32_t go = 0;
    volatile uint32_t published = 0;
    volatile uint32_t done = 0;
    host_writer_t writers[2] = {
        {.cpu_id = 11, .slot = 0, .ready = &ready, .go = &go,
         .published = &published,
         .done = &done, .cell = &cells[0], .failures = 0},
        {.cpu_id = 18, .slot = 1, .ready = &ready, .go = &go,
         .published = &published,
         .done = &done, .cell = &cells[1], .failures = 0}};
    pthread_t threads[2];
    unsigned int start = checks;
    memset(g_cpus, 0, sizeof(g_cpus));
    g_cpu_count = 2;
    g_cpus[0].slot = 0;
    g_cpus[0].apic_id = 11;
    g_cpus[1].slot = 1;
    g_cpus[1].apic_id = 18;
    CHECK(pthread_create(&threads[0], NULL, host_writer, &writers[0]) == 0);
    CHECK(pthread_create(&threads[1], NULL, host_writer, &writers[1]) == 0);
    while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) != 2)
        sched_yield();

    mmio_trace_snapshot_t baseline[2] = {{0}, {0}};
    __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&published, __ATOMIC_ACQUIRE) != 2)
        sched_yield();
    uint32_t accepted = 0;
    uint32_t unavailable = 0;
    uint32_t progress = 0;
    uint32_t reader_failures = 0;
    while (__atomic_load_n(&done, __ATOMIC_ACQUIRE) != 2)
    {
        for (uint32_t slot = 0; slot < 2; slot++)
        {
            mmio_trace_snapshot_t snapshot = {0};
            if (!mmio_trace_snapshot_slot_for_test(slot, &snapshot, 4u))
            {
                unavailable++;
                continue;
            }
            accepted++;
            if (baseline[slot].sequence == 0)
                baseline[slot] = snapshot;
            else if (baseline[slot].sequence != snapshot.sequence)
                progress |= 1u << slot;
            if (snapshot.slot != slot ||
                snapshot.cpu_id != writers[slot].cpu_id ||
                snapshot.address != (uint64_t)(uintptr_t)&cells[slot])
                reader_failures++;
        }
    }
    CHECK(pthread_join(threads[0], NULL) == 0);
    CHECK(pthread_join(threads[1], NULL) == 0);
    CHECK(writers[0].failures == 0 && writers[1].failures == 0);
    CHECK(reader_failures == 0);
    CHECK(accepted > 0 && unavailable < UINT32_MAX);
    CHECK(progress == 3u);
    printf("ARCH_HOST_CONCURRENT_CHECKS=%u accepted=%u unavailable=%u\n",
           checks - start, accepted, unavailable);
    return true;
}

typedef struct
{
    uint32_t calls;
    uint32_t succeed_on;
} host_budget_t;

static bool host_budget_condition(void *context)
{
    host_budget_t *value = context;
    value->calls++;
    return value->calls == value->succeed_on;
}

static bool budget_tests(void)
{
    unsigned int start = checks;
    host_budget_t value = {.calls = 0, .succeed_on = 1};
    CHECK(arch_test_poll_budget(host_budget_condition, &value, 0));
    CHECK(value.calls == 1);
    value = (host_budget_t){.calls = 0, .succeed_on = UINT32_MAX};
    CHECK(!arch_test_poll_budget(host_budget_condition, &value, 0));
    CHECK(value.calls == 1);
    value = (host_budget_t){.calls = 0, .succeed_on = 2};
    CHECK(arch_test_poll_budget(host_budget_condition, &value, 1));
    CHECK(value.calls == 2);
    value = (host_budget_t){.calls = 0, .succeed_on = 4};
    CHECK(arch_test_poll_budget(host_budget_condition, &value, 3));
    CHECK(value.calls == 4);
    value = (host_budget_t){.calls = 0, .succeed_on = UINT32_MAX};
    CHECK(!arch_test_poll_budget(host_budget_condition, &value, 3));
    CHECK(value.calls == 4);
    value = (host_budget_t){.calls = 0, .succeed_on = 5};
    CHECK(!arch_test_poll_budget(host_budget_condition, &value, 3));
    CHECK(value.calls == 4);
    printf("ARCH_HOST_BUDGET_CHECKS=%u\n", checks - start);
    return true;
}
#endif

int main(void)
{
    if (!cpuid_tests() || !mmio_tests())
        return 1;
#if HOBBYOS_DEBUG_ASSERT
    if (!concurrent_trace_tests() || !budget_tests())
        return 1;
#endif
    printf("[ARCH_HOST] status=PASS checks=%u debug_trace=%u\n",
           checks, HOBBYOS_DEBUG_ASSERT ? 1u : 0u);
    return 0;
}
