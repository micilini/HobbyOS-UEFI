#include "mmio.h"

#if HOBBYOS_DEBUG_ASSERT

#include "cpu.h"
#include "../core/idt.h"
#include "../smp/cpu_limits.h"
#include "../smp/smp_topology.h"

#include <stddef.h>

typedef struct
{
    volatile uint32_t writer_active;
    volatile uint32_t sequence;
    volatile uint64_t address;
    volatile uint64_t value;
    volatile uint32_t cpu_id;
    volatile uint32_t slot;
    volatile uint8_t width;
    volatile uint8_t operation;
    volatile uint8_t phase;
    volatile uint8_t value_valid;
} __attribute__((aligned(64))) mmio_trace_record_t;

static mmio_trace_record_t g_mmio_trace_records[HOBBYOS_MAX_CPUS];
static volatile uint64_t g_mmio_trace_slot_hints[HOBBYOS_MAX_CPUS];

#if defined(HOBBYOS_ARCH_HOST_TEST)
extern bool arch_host_cpu_local_arch_id(uint32_t *cpu_id);
#define MMIO_LOCAL_ARCH_ID(output) arch_host_cpu_local_arch_id(output)
#else
#define MMIO_LOCAL_ARCH_ID(output) cpu_local_arch_id(output)
#endif

static uint32_t mmio_trace_hint_start(uint32_t cpu_id)
{
    uint32_t mixed = cpu_id;
    mixed ^= mixed >> 16;
    mixed *= 0x7feb352du;
    mixed ^= mixed >> 15;
    return mixed % HOBBYOS_MAX_CPUS;
}

static bool mmio_trace_hint_lookup(uint32_t cpu_id, uint32_t count,
                                   uint32_t *slot)
{
    uint32_t start = mmio_trace_hint_start(cpu_id);

    for (uint32_t probe = 0; probe < HOBBYOS_MAX_CPUS; probe++)
    {
        uint32_t index = (start + probe) % HOBBYOS_MAX_CPUS;
        uint64_t hint = __atomic_load_n(&g_mmio_trace_slot_hints[index],
                                        __ATOMIC_ACQUIRE);
        if (hint == 0 || (uint32_t)(hint >> 32) != cpu_id)
            continue;

        uint32_t hinted_slot = (uint32_t)hint - 1u;
        if (hinted_slot < count &&
            __atomic_load_n(&g_cpus[hinted_slot].slot,
                            __ATOMIC_RELAXED) == hinted_slot &&
            __atomic_load_n(&g_cpus[hinted_slot].apic_id,
                            __ATOMIC_RELAXED) == cpu_id)
        {
            *slot = hinted_slot;
            return true;
        }
    }
    return false;
}

static void mmio_trace_hint_publish(uint32_t cpu_id, uint32_t slot)
{
    uint32_t start = mmio_trace_hint_start(cpu_id);
    uint64_t hint = ((uint64_t)cpu_id << 32) | ((uint64_t)slot + 1u);

    for (uint32_t probe = 0; probe < HOBBYOS_MAX_CPUS; probe++)
    {
        uint32_t index = (start + probe) % HOBBYOS_MAX_CPUS;
        uint64_t current = __atomic_load_n(&g_mmio_trace_slot_hints[index],
                                           __ATOMIC_ACQUIRE);
        if (current == hint)
            return;
        if (current != 0 && (uint32_t)(current >> 32) == cpu_id)
        {
            __atomic_store_n(&g_mmio_trace_slot_hints[index], hint,
                             __ATOMIC_RELEASE);
            return;
        }
        if (current == 0)
        {
            uint64_t expected = 0;
            if (__atomic_compare_exchange_n(
                    &g_mmio_trace_slot_hints[index], &expected, hint, false,
                    __ATOMIC_RELEASE, __ATOMIC_RELAXED))
                return;
        }
    }
}

static bool mmio_trace_resolve_local(uint32_t *cpu_id, uint32_t *slot)
{
    uint32_t local_id = 0;
    uint32_t count = __atomic_load_n(&g_cpu_count, __ATOMIC_ACQUIRE);
    uint32_t match_count = 0;
    uint32_t match_slot = UINT32_MAX;

    if (!cpu_id || !slot || count == 0 || count > HOBBYOS_MAX_CPUS ||
        !MMIO_LOCAL_ARCH_ID(&local_id))
        return false;

    if (mmio_trace_hint_lookup(local_id, count, &match_slot))
    {
        *cpu_id = local_id;
        *slot = match_slot;
        return true;
    }

    for (uint32_t index = 0; index < count; index++)
    {
        uint32_t published_slot = __atomic_load_n(&g_cpus[index].slot,
                                                   __ATOMIC_RELAXED);
        uint32_t published_id = __atomic_load_n(&g_cpus[index].apic_id,
                                                 __ATOMIC_RELAXED);
        if (published_slot != index)
            return false;
        if (published_id == local_id)
        {
            match_count++;
            match_slot = index;
        }
    }
    if (match_count != 1u)
        return false;
    *cpu_id = local_id;
    *slot = match_slot;
    mmio_trace_hint_publish(local_id, match_slot);
    return true;
}

static void mmio_trace_publish(mmio_trace_record_t *record,
                               uint64_t address, uint64_t value,
                               uint32_t cpu_id, uint32_t slot,
                               uint8_t width, uint8_t operation,
                               uint8_t phase, uint8_t value_valid)
{
    uint32_t sequence = __atomic_load_n(&record->sequence,
                                        __ATOMIC_RELAXED) & ~1u;
    uint32_t updating = sequence + 1u;
    uint32_t published = updating + 1u;
    if (published == 0)
        published = 2u;
    __atomic_store_n(&record->sequence, updating,
                     __ATOMIC_SEQ_CST);
    __atomic_store_n(&record->address, address, __ATOMIC_RELAXED);
    __atomic_store_n(&record->value, value, __ATOMIC_RELAXED);
    __atomic_store_n(&record->cpu_id, cpu_id, __ATOMIC_RELAXED);
    __atomic_store_n(&record->slot, slot, __ATOMIC_RELAXED);
    __atomic_store_n(&record->width, width, __ATOMIC_RELAXED);
    __atomic_store_n(&record->operation, operation, __ATOMIC_RELAXED);
    __atomic_store_n(&record->phase, phase, __ATOMIC_RELAXED);
    __atomic_store_n(&record->value_valid, value_valid,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&record->sequence, published,
                     __ATOMIC_RELEASE);
}

static bool mmio_trace_snapshot_record(uint32_t slot, uint32_t cpu_id,
                                       bool enforce_cpu,
                                       mmio_trace_snapshot_t *snapshot,
                                       uint32_t attempt_budget)
{
    if (!snapshot || slot >= HOBBYOS_MAX_CPUS || attempt_budget == 0)
        return false;

    mmio_trace_record_t *record = &g_mmio_trace_records[slot];
    for (uint32_t attempt = 0; attempt < attempt_budget; attempt++)
    {
        uint32_t before = __atomic_load_n(&record->sequence,
                                          __ATOMIC_ACQUIRE);
        if (before & 1u)
            continue;
        mmio_trace_snapshot_t candidate = {
            .address = __atomic_load_n(&record->address,
                                       __ATOMIC_RELAXED),
            .value = __atomic_load_n(&record->value, __ATOMIC_RELAXED),
            .cpu_id = __atomic_load_n(&record->cpu_id,
                                      __ATOMIC_RELAXED),
            .slot = __atomic_load_n(&record->slot, __ATOMIC_RELAXED),
            .width = __atomic_load_n(&record->width, __ATOMIC_RELAXED),
            .operation = __atomic_load_n(&record->operation,
                                         __ATOMIC_RELAXED),
            .phase = __atomic_load_n(&record->phase, __ATOMIC_RELAXED),
            .value_valid = __atomic_load_n(&record->value_valid,
                                           __ATOMIC_RELAXED),
            .cpu_valid = 1,
            .slot_valid = 1,
            .coherent = 1,
            .sequence = before};
        uint32_t after = __atomic_load_n(&record->sequence,
                                         __ATOMIC_ACQUIRE);
        if (before == after && !(after & 1u) && after != 0 &&
            candidate.phase != MMIO_TRACE_PHASE_NONE &&
            candidate.slot == slot &&
            (!enforce_cpu || candidate.cpu_id == cpu_id))
        {
            candidate.sequence = after;
            *snapshot = candidate;
            return true;
        }
    }
    return false;
}

mmio_trace_token_t mmio_trace_begin(void *address, uint8_t width,
                                    mmio_trace_operation_t operation,
                                    uint64_t value, bool value_valid)
{
    mmio_trace_token_t token = {
        .irq_flags = irq_save(),
        .cpu_id = 0,
        .slot = UINT32_MAX,
        .active = 0,
        .pin_owned = 1};
    uint32_t cpu_id = 0;
    uint32_t slot = UINT32_MAX;

    if (!mmio_trace_resolve_local(&cpu_id, &slot))
        return token;
    mmio_trace_record_t *record = &g_mmio_trace_records[slot];
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&record->writer_active, &expected, 1,
                                     false, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED))
        return token;

    mmio_trace_publish(record, (uint64_t)(uintptr_t)address, value,
                       cpu_id, slot, width, (uint8_t)operation,
                       MMIO_TRACE_PHASE_ATTEMPT,
                       value_valid ? 1u : 0u);
    token.slot = slot;
    token.cpu_id = cpu_id;
    token.active = 1;
    return token;
}

void mmio_trace_complete(mmio_trace_token_t token, uint64_t value,
                         bool value_valid)
{
    if (token.active && token.slot < HOBBYOS_MAX_CPUS)
    {
        mmio_trace_record_t *record = &g_mmio_trace_records[token.slot];
        mmio_trace_publish(
            record,
            __atomic_load_n(&record->address, __ATOMIC_RELAXED), value,
            token.cpu_id, token.slot,
            __atomic_load_n(&record->width, __ATOMIC_RELAXED),
            __atomic_load_n(&record->operation, __ATOMIC_RELAXED),
            MMIO_TRACE_PHASE_COMPLETE, value_valid ? 1u : 0u);
        __atomic_store_n(&record->writer_active, 0, __ATOMIC_RELEASE);
    }
    if (token.pin_owned)
        irq_restore(token.irq_flags);
}

bool mmio_trace_snapshot_current(mmio_trace_snapshot_t *snapshot,
                                 uint32_t attempt_budget)
{
    uint32_t cpu_id = 0;
    uint32_t slot = UINT32_MAX;

    if (!snapshot)
        return false;
    *snapshot = (mmio_trace_snapshot_t){0};
    if (attempt_budget == 0)
        return false;

    irq_flags_t flags = irq_save();
    bool resolved = mmio_trace_resolve_local(&cpu_id, &slot);
    bool valid = resolved && mmio_trace_snapshot_record(
        slot, cpu_id, true, snapshot, attempt_budget);
    irq_restore(flags);
    return valid;
}

#if defined(HOBBYOS_ARCH_TEST)
bool mmio_trace_snapshot_slot_for_test(uint32_t slot,
                                       mmio_trace_snapshot_t *snapshot,
                                       uint32_t attempt_budget)
{
    if (!snapshot)
        return false;
    *snapshot = (mmio_trace_snapshot_t){0};
    return mmio_trace_snapshot_record(slot, 0, false, snapshot,
                                      attempt_budget);
}
#endif

#endif
