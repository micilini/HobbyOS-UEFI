#include "cpu_request.h"

_Static_assert(HOBBYOS_MAX_CPUS == 32, "request mask covers the declared topology");

bool cpu_request_context_valid(bool interrupts_enabled, uint32_t irq_depth,
    bool thread_context, bool runtime_ready, bool panic, bool critical)
{
    return interrupts_enabled && !irq_depth && thread_context && runtime_ready &&
        !panic && !critical;
}

bool cpu_request_admit(cpu_request_storage_t *s)
{
    if (!s) return false;
    uint32_t free = 0;
    return __atomic_compare_exchange_n(&s->owner, &free, 1, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

cpu_request_status_t cpu_request_publish(cpu_request_storage_t *s,
                                         const cpu_request_spec_t *spec)
{
    if (!s || !spec || !__atomic_load_n(&s->owner, __ATOMIC_RELAXED) ||
        __atomic_load_n(&s->active, __ATOMIC_ACQUIRE)) return CPU_REQUEST_INVALID;
    if (s->quarantined) return CPU_REQUEST_QUARANTINED;
    if (s->next_generation >= (UINT64_MAX >> 2)) return CPU_REQUEST_EXHAUSTED;
    if (!spec->targets || !spec->root || (spec->root & 4095) ||
        (spec->root & ~UINT64_C(0x000ffffffffff000)) ||
        !tlb_interval_valid(&spec->interval)) return CPU_REQUEST_INVALID;
    if (spec->operation != CPU_REQUEST_INVALIDATE
        && spec->operation != CPU_REQUEST_CACHE_WRITEBACK
#ifdef HOBBYOS_SELFTEST
        && spec->operation != CPU_REQUEST_SAMPLE
        && spec->operation != CPU_REQUEST_PROTECTION
#endif
       ) return CPU_REQUEST_INVALID;
#ifdef HOBBYOS_SELFTEST
    if (spec->operation==CPU_REQUEST_PROTECTION && spec->interval.pages) return CPU_REQUEST_INVALID;
#endif
    for (unsigned slot = 0; slot < HOBBYOS_MAX_CPUS; ++slot) {
        if (!(spec->targets & (UINT32_C(1) << slot))) continue;
        if (spec->apic[slot] == UINT32_MAX) return CPU_REQUEST_INVALID;
        for (unsigned other = 0; other < slot; ++other)
            if ((spec->targets & (UINT32_C(1) << other)) &&
                spec->apic[other] == spec->apic[slot]) return CPU_REQUEST_INVALID;
    }
    s->spec = *spec;
    s->spec.generation = ++s->next_generation;
    for (unsigned slot = 0; slot < HOBBYOS_MAX_CPUS; ++slot) {
        s->samples[slot] = (cpu_request_sample_t){0};
        __atomic_store_n(&s->publication[slot],
            (spec->targets & (UINT32_C(1) << slot)) ?
            (s->spec.generation << 2) | 1 : 0, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&s->active, s->spec.generation, __ATOMIC_RELEASE);
    return CPU_REQUEST_OK;
}

bool cpu_request_handle(cpu_request_storage_t *s, uint32_t slot,
    uint32_t apic, cpu_request_execute_fn execute, void *context)
{
    if (!s || slot >= HOBBYOS_MAX_CPUS || !execute) return false;
    uint64_t generation = __atomic_load_n(&s->active, __ATOMIC_ACQUIRE);
    if (!generation) return false;
    uint64_t pending = (generation << 2) | 1;
    if (!__atomic_compare_exchange_n(&s->publication[slot], &pending,
            (generation << 2) | 2, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return false;
    cpu_request_sample_t sample = {.slot = slot, .apic = apic};
    sample.success = s->spec.apic[slot] == apic &&
        execute(context, &s->spec, &sample);
    s->samples[slot] = sample;
    /* No storage reads or writes are permitted after publishing completion. */
    __atomic_store_n(&s->publication[slot], (generation << 2) | 3, __ATOMIC_RELEASE);
    return true;
}

void cpu_request_collect(const cpu_request_storage_t *s, cpu_request_result_t *out)
{
    if (!s || !out) return;
    out->generation = s->spec.generation;
    out->requested = s->spec.targets;
    out->acknowledged = out->failed = 0;
    for (unsigned slot = 0; slot < HOBBYOS_MAX_CPUS; ++slot) {
        uint32_t bit = UINT32_C(1) << slot;
        if (!(out->requested & bit)) continue;
        if (__atomic_load_n(&s->publication[slot], __ATOMIC_ACQUIRE) ==
            ((out->generation << 2) | 3)) {
            if (s->samples[slot].success) out->acknowledged |= bit;
            else out->failed |= bit;
        }
    }
    out->missing = out->requested & ~(out->acknowledged | out->failed);
}

bool cpu_request_retire(cpu_request_storage_t *s, bool success)
{
    if (!s || !__atomic_load_n(&s->owner, __ATOMIC_RELAXED)) return false;
    cpu_request_result_t result = {0};
    cpu_request_collect(s, &result);
    bool safe = success && result.requested &&
        result.acknowledged == result.requested && !result.failed && !result.missing;
    if (!safe) s->quarantined = true;
    /* Closing publication does not revoke a claimed handler. On failure the
     * storage remains immutable forever, including operands and late results. */
    __atomic_store_n(&s->active, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s->owner, 0, __ATOMIC_RELEASE);
    return safe;
}
