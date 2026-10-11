#include "cpu_control_snapshot.h"
#include "cpu.h"
#include "../core/idt.h"

#ifndef HOBBYOS_CPU_CONTROL_HOST_TEST
void cpu_control_read_local(cpu_control_sample_t *out)
{
    irq_flags_t flags = irq_save();
    *out = (cpu_control_sample_t){.slot = CPU_SLOT_INVALID};
    uint32_t a, b, c, d;
    out->identity_valid = cpu_local_arch_id(&out->apic_id) &&
        smp_cpu_slot_from_apic_id(out->apic_id, &out->slot);
    out->is_bsp = out->identity_valid && out->slot == smp_bsp_cpu_slot();
    __asm__ volatile("mov %%cr0, %0" : "=r"(out->paging.cr0));
    __asm__ volatile("mov %%cr3, %0" : "=r"(out->paging.cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(out->paging.cr4));
    if (cpu_get_cpuid_count(1, 0, &a, &b, &c, &d))
        out->cpuid1_edx = d;
    if (cpu_get_cpuid_count(0x80000001u, 0, &a, &b, &c, &d))
        out->extended_edx = d;
    if (cpu_get_cpuid_count(0x80000008u, 0, &a, &b, &c, &d))
        out->address_eax = a;
    out->paging.physical_bits = out->address_eax ?
        (uint8_t)out->address_eax : 36;
    out->paging.nx_supported = (out->extended_edx & (1u << 20)) != 0;
    out->paging.page1g_supported = (out->extended_edx & (1u << 26)) != 0;
    out->paging.pat_supported = (out->cpuid1_edx & (1u << 16)) != 0;
    if (out->cpuid1_edx & (1u << 5))
    {
        if (out->extended_edx & (1u << 29))
            out->paging.efer_valid = cpu_read_msr_safe(0xc0000080u,
                                                       &out->paging.efer);
        if (out->paging.pat_supported)
            out->paging.pat_valid = cpu_read_msr_safe(0x277, &out->paging.pat);
    }
    irq_restore(flags);
}
#endif

#ifdef HOBBYOS_SELFTEST
#include "../apic/lapic.h"
#include "../core/clock.h"
#include "../core/interrupts.h"
#include "../core/irq_bootstrap.h"
#include "../core/runtime_ready.h"
#include "../smp/smp_boot.h"

extern volatile int g_panic_in_progress;

/* The deadline is fixed before any runtime acceptance; no CPU waits in IRQ. */
#define CPU_CONTROL_TIMEOUT_NS UINT64_C(2000000000)
static cpu_control_capture_t g_capture;
static uint64_t g_slot_publication[HOBBYOS_MAX_CPUS];
static uint64_t g_active_generation, g_next_generation;
static uint32_t g_owner;
static bool g_quarantined;

void cpu_control_snapshot_irq(void)
{
    uint64_t generation = __atomic_load_n(&g_active_generation, __ATOMIC_ACQUIRE);
    if (!generation)
        return;
    irq_flags_t flags = irq_save();
    uint32_t apic, slot;
    if (!cpu_local_arch_id(&apic) || !smp_cpu_slot_from_apic_id(apic, &slot) ||
        slot >= HOBBYOS_MAX_CPUS)
        goto out;
    uint64_t pending = (generation << 2) | 1;
    if (!__atomic_compare_exchange_n(&g_slot_publication[slot], &pending,
            (generation << 2) | 2, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        goto out;
    /* A claimed slot is never reused after timeout; stale IPIs carry no pointer. */
    cpu_control_read_local(&g_capture.cpus[slot]);
    __atomic_store_n(&g_slot_publication[slot], (generation << 2) | 3,
                     __ATOMIC_RELEASE);
out:
    irq_restore(flags);
}

const cpu_control_capture_t *cpu_control_snapshot(void)
{
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&g_owner, &expected, 1, false,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return 0;
    if (g_quarantined)
    {
        g_capture.complete = false;
        g_capture.reason = "previous_timeout_quarantined";
        return &g_capture;
    }
    g_capture = (cpu_control_capture_t){.reason = "startup_or_panic"};
    if (!irq_is_enabled() || !runtime_ready_is_test_ready() ||
        !irq_bootstrap_services_are_ready() || !clock_monotonic_is_ready() ||
        __atomic_load_n(&g_panic_in_progress, __ATOMIC_ACQUIRE) ||
        g_cpu_count == 0 || g_cpu_count > HOBBYOS_MAX_CPUS)
        return &g_capture;
    if (g_next_generation == (UINT64_MAX >> 2))
    { g_capture.reason = "generation_exhausted"; return &g_capture; }
    uint64_t generation = ++g_next_generation;
    g_capture.generation = generation;
    g_capture.x2apic = lapic_is_x2apic();
    for (uint32_t slot = 0; slot < g_cpu_count; slot++)
    {
        const SmpCpuInfo *cpu = smp_cpu_by_slot_const(slot);
        if (!cpu || __atomic_load_n(&cpu->state, __ATOMIC_ACQUIRE) != CPU_STATE_ONLINE)
            continue;
        g_capture.target_mask |= 1u << slot;
        g_capture.apic_ids[slot] = cpu->apic_id;
        if (!g_capture.x2apic && cpu->apic_id > 255)
        { g_capture.reason = "xapic_destination_unrepresentable"; return &g_capture; }
    }
    if (!g_capture.target_mask)
    { g_capture.reason = "empty_online_set"; return &g_capture; }
    for (uint32_t slot = 0; slot < HOBBYOS_MAX_CPUS; slot++)
        __atomic_store_n(&g_slot_publication[slot],
            (g_capture.target_mask & (1u << slot)) ? (generation << 2) | 1 : 0,
            __ATOMIC_RELAXED);
    __atomic_store_n(&g_active_generation, generation, __ATOMIC_RELEASE);
    uint64_t started = clock_monotonic_ns();
    for (uint32_t slot = 0; slot < g_cpu_count; slot++)
    {
        if (!(g_capture.target_mask & (1u << slot))) continue;
        irq_flags_t flags = irq_save();
        uint32_t current;
        if (cpu_local_arch_id(&current))
        {
            if (current == g_capture.apic_ids[slot]) cpu_control_snapshot_irq();
            else if (lapic_send_ipi(g_capture.apic_ids[slot],
                         INT_VECTOR_RUNTIME_RENDEZVOUS_WAKE) != LAPIC_DELIVERY_SUBMITTED)
                g_capture.error_mask |= 1u << slot;
        }
        /* Protect only one xAPIC ICR pair; acknowledgement waits run with IF=1. */
        irq_restore(flags);
    }
    do {
        g_capture.complete_mask = 0;
        for (uint32_t slot = 0; slot < HOBBYOS_MAX_CPUS; slot++)
            if ((g_capture.target_mask & (1u << slot)) &&
                __atomic_load_n(&g_slot_publication[slot], __ATOMIC_ACQUIRE) ==
                    ((generation << 2) | 3))
                g_capture.complete_mask |= 1u << slot;
        if (g_capture.complete_mask == g_capture.target_mask) break;
        __asm__ volatile("pause" ::: "memory");
    } while (clock_monotonic_ns() - started < CPU_CONTROL_TIMEOUT_NS);
    __atomic_store_n(&g_active_generation, 0, __ATOMIC_RELEASE);
    if (g_capture.complete_mask != g_capture.target_mask)
    {
        g_quarantined = true;
        g_capture.reason = "missing_cpu_response";
        return &g_capture;
    }
    for (uint32_t slot = 0; slot < HOBBYOS_MAX_CPUS; slot++)
    {
        if (!(g_capture.target_mask & (1u << slot))) continue;
        const cpu_control_sample_t *cpu = &g_capture.cpus[slot];
        if (!cpu->identity_valid || cpu->slot != slot ||
            cpu->apic_id != g_capture.apic_ids[slot] || !cpu->paging.efer_valid ||
            (cpu->paging.pat_supported && !cpu->paging.pat_valid))
            g_capture.error_mask |= 1u << slot;
    }
    g_capture.complete = !g_capture.error_mask;
    g_capture.reason = g_capture.complete ? "collected" : "invalid_cpu_sample";
    return &g_capture;
}

void cpu_control_snapshot_release(void)
{
    __atomic_store_n(&g_owner, 0, __ATOMIC_RELEASE);
}
#endif
