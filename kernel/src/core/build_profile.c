#include "build_profile.h"

#include "interrupts.h"
#include "../libc/memory.h"
#include "../smp/cpu_limits.h"

typedef struct
{
    volatile uint8_t bsp_seen;
    volatile uint8_t bsp_rsp_mod16;
    volatile uint8_t bsp_df;
    volatile uint8_t ap_seen[HOBBYOS_MAX_CPUS];
    volatile uint8_t ap_rsp_mod16[HOBBYOS_MAX_CPUS];
    volatile uint8_t ap_df[HOBBYOS_MAX_CPUS];
    volatile uint64_t irq_entries;
    volatile uint64_t irq_df_violations;
    volatile uint64_t irq_probe_entries;
    volatile uint64_t irq_probe_df_violations;
} build_profile_state_t;

#if HOBBYOS_DEBUG_ASSERT
static build_profile_state_t g_build_profile;
#endif

void build_profile_record_bsp_entry(uint64_t rsp_mod16, uint64_t df)
{
#if HOBBYOS_DEBUG_ASSERT
    __atomic_store_n(&g_build_profile.bsp_rsp_mod16,
                     (uint8_t)rsp_mod16, __ATOMIC_RELAXED);
    __atomic_store_n(&g_build_profile.bsp_df, (uint8_t)df,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_build_profile.bsp_seen, 1u, __ATOMIC_RELEASE);
#else
    (void)rsp_mod16;
    (void)df;
#endif
}

void build_profile_record_ap_entry(uint32_t slot, uint64_t rsp_mod16,
                                   uint64_t df)
{
#if HOBBYOS_DEBUG_ASSERT
    if (slot >= HOBBYOS_MAX_CPUS)
        return;
    __atomic_store_n(&g_build_profile.ap_rsp_mod16[slot],
                     (uint8_t)rsp_mod16, __ATOMIC_RELAXED);
    __atomic_store_n(&g_build_profile.ap_df[slot], (uint8_t)df,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_build_profile.ap_seen[slot], 1u,
                     __ATOMIC_RELEASE);
#else
    (void)slot;
    (void)rsp_mod16;
    (void)df;
#endif
}

void build_profile_record_irq_entry(uint8_t vector, uint64_t df)
{
#if HOBBYOS_DEBUG_ASSERT
    __atomic_add_fetch(&g_build_profile.irq_entries, 1u,
                       __ATOMIC_RELAXED);
    if (df != 0)
        __atomic_add_fetch(&g_build_profile.irq_df_violations, 1u,
                           __ATOMIC_RELAXED);
    if (vector == INT_VECTOR_RUNTIME_RENDEZVOUS_WAKE)
    {
        __atomic_add_fetch(&g_build_profile.irq_probe_entries, 1u,
                           __ATOMIC_RELAXED);
        if (df != 0)
            __atomic_add_fetch(
                &g_build_profile.irq_probe_df_violations, 1u,
                __ATOMIC_RELAXED);
    }
#else
    (void)vector;
    (void)df;
#endif
}

bool build_profile_snapshot(build_profile_snapshot_t *out)
{
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
#if HOBBYOS_DEBUG_ASSERT
    out->bsp_seen = __atomic_load_n(&g_build_profile.bsp_seen,
                                    __ATOMIC_ACQUIRE);
    out->bsp_rsp_mod16 = __atomic_load_n(
        &g_build_profile.bsp_rsp_mod16, __ATOMIC_RELAXED);
    out->bsp_df = __atomic_load_n(&g_build_profile.bsp_df,
                                  __ATOMIC_RELAXED);
    for (uint32_t slot = 0; slot < HOBBYOS_MAX_CPUS; slot++)
    {
        if (!__atomic_load_n(&g_build_profile.ap_seen[slot],
                             __ATOMIC_ACQUIRE))
            continue;
        uint32_t bit = 1u << slot;
        out->ap_seen_mask |= bit;
        if (__atomic_load_n(&g_build_profile.ap_rsp_mod16[slot],
                            __ATOMIC_RELAXED) != 8u)
            out->ap_bad_rsp_mask |= bit;
        if (__atomic_load_n(&g_build_profile.ap_df[slot],
                            __ATOMIC_RELAXED) != 0u)
            out->ap_bad_df_mask |= bit;
    }
    out->irq_entries = __atomic_load_n(&g_build_profile.irq_entries,
                                       __ATOMIC_ACQUIRE);
    out->irq_df_violations = __atomic_load_n(
        &g_build_profile.irq_df_violations, __ATOMIC_RELAXED);
    out->irq_probe_entries = __atomic_load_n(
        &g_build_profile.irq_probe_entries, __ATOMIC_ACQUIRE);
    out->irq_probe_df_violations = __atomic_load_n(
        &g_build_profile.irq_probe_df_violations, __ATOMIC_RELAXED);
    return true;
#else
    return false;
#endif
}
