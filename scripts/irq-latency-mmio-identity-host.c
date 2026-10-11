#define main inherited_controller_main
#include "irq-latency-controller-host.c"
#undef main
#include "../kernel/src/cpu/mmio.c"

static unsigned architectural_queries, mmio_reads;
void arch_host_mmio_access_point(void) { CHECK(!irq_is_enabled()); ++mmio_reads; }
bool arch_host_cpu_local_arch_id(uint32_t *id)
{ ++architectural_queries; *id = host_cpu; return true; }

static void traced_read(bool reuse)
{
    uint32_t value = UINT32_C(0x12345678);
    unsigned queries = architectural_queries, reads = mmio_reads;
    CHECK(mmio_read32(&value) == value);
    CHECK(mmio_reads == reads + 1);
    CHECK(architectural_queries == queries + (reuse ? 0u : 1u));
    mmio_trace_snapshot_t snapshot;
    CHECK(mmio_trace_snapshot_record(host_cpu, host_cpu, true, &snapshot, 1));
    CHECK(snapshot.cpu_id == host_cpu && snapshot.slot == host_cpu && snapshot.width == 4);
    CHECK(snapshot.value == value && snapshot.value_valid && snapshot.phase == MMIO_TRACE_PHASE_COMPLETE);
    CHECK(!g_mmio_trace_records[host_cpu].writer_active && !irq_is_enabled());
}

static void invalidation(void)
{
    for (unsigned mutation = 0; mutation < 13; ++mutation) {
        reset();
        (void)irq_save();
        uint32_t id = UINT32_MAX;
        CHECK(irq_latency_mmio_identity(&id) && id == 0);
        uint64_t queries = identity_queries, reused = g_irq_nested_elided[0].mmio_reused;
        if (mutation == 0) host_flags = IRQ_LATENCY_IF;
        if (mutation == 1) { irq_latency_boundary_t b = {.flags=IRQ_LATENCY_IF,.observed_flags=0};
            (void)irq_latency_before_restore(&b); host_flags=IRQ_LATENCY_IF; host_flags=0; }
        if (mutation == 2) { irq_latency_context_switch(); host_cpu=1; }
        if (mutation == 3) ++owner_generation;
        if (mutation == 4) ++g_irq_entry_sequence[0];
        if (mutation == 5) ++g_irq_interior_fault_sequence;
        if (mutation == 6) g_irq_interior_fault_overflow=1;
        if (mutation == 7) ++g_irq_latency[0].epoch;
        if (mutation == 8) g_irq_mask_segment[0]=UINT64_MAX;
        if (mutation == 9) irq_latency_stack_retired(host_stack,owner_generation);
        if (mutation == 10) ++g_cpus[0].apic_id;
        if (mutation == 11) g_irq_frozen[0]=1;
        if (mutation == 12) g_irq_nested_elided[0].mmio_reused=UINT64_MAX;
        id=UINT32_MAX; queries=identity_queries;
        CHECK(!irq_latency_mmio_identity(&id) && id==UINT32_MAX && identity_queries==queries);
        CHECK(g_irq_nested_elided[0].mmio_reused==(mutation==12 ? UINT64_MAX : reused));
        g_irq_frozen[0]=0;
    }
    reset(); (void)irq_save(); CHECK(!irq_latency_mmio_identity(NULL));
    uint64_t count = UINT64_MAX;
    CHECK(!irq_latency_mmio_reuses(0,&count) && count==UINT64_MAX);
    traced_read(true);
    g_irq_frozen[0]=1;
    CHECK(irq_latency_mmio_reuses(0,&count) && count==1);
    CHECK(!irq_latency_mmio_reuses(0,NULL) && !irq_latency_mmio_reuses(g_cpu_count,&count));
    traced_read(false);
    g_irq_frozen[0]=0;
    reset(); g_irq_ready=0; host_flags=0; traced_read(false);
    reset(); (void)irq_save(); g_irq_nested_elided[0].mmio_reused=UINT64_MAX; traced_read(false);
}

static void fallback_after_transition(void)
{
    reset();
    (void)irq_save();
    traced_read(true);
    irq_latency_context_switch(); host_cpu=1;
    unsigned queries=architectural_queries;
    uint32_t id,slot;
    CHECK(mmio_trace_resolve_local(&id,&slot) && id==1 && slot==1);
    CHECK(architectural_queries==queries+1);
    (void)irq_save();
    traced_read(true);
    CHECK(g_irq_nested_elided[1].mmio_reused==1);
}

int main(void)
{
    reset();
    for (unsigned cpu = 0; cpu < g_cpu_count; ++cpu) {
        g_cpus[cpu].slot = cpu; g_cpus[cpu].apic_id = cpu;
    }
    (void)irq_save();
    traced_read(true);
    invalidation();
    fallback_after_transition();
    puts("IRQ latency actual-C MMIO same-mask identity and fallback: PASS hardware=HOSTED");
    return inherited_controller_main();
}
