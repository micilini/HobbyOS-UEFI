#define main irq_latency_original_controller_main
#include "irq-latency-controller-host.c"
#undef main

int main(void)
{
    reset();
    host_flags = 0;
    g_irq_ready = 0;
    uint64_t entry = irq_latency_interrupt_enter(110, 32, IRQ_LATENCY_IF, 0);
    CHECK(!entry && !g_irq_latency[0].active);
    g_irq_ready = 1;
    CHECK(irq_save() == 0);
    irq_latency_tail_t tail = irq_latency_interrupt_exit(32, IRQ_LATENCY_IF, entry);
    CHECK(tail.pending && !tail.probe_end);
    /* Substitute only the assembly tail's scalar timestamp and READY stores. */
    tail.pending->witness.cells[IL_END] = 300;
    tail.pending->witness.cells[IL_OBSERVED_END] = 300;
    --tail.pending->ready;
    CHECK(irq_latency_record_drain(&g_irq_latency[0]));
    CHECK(!g_irq_latency[0].pending_count && !g_irq_latency[0].active);
    CHECK(!g_irq_latency[0].invalid && !g_irq_invalid_boundaries);
    CHECK(g_irq_latency[0].excluded[0].count == 1 && g_irq_latency[0].early == 1);
    for (unsigned path = 0; path < IRQ_LATENCY_PATHS; ++path)
        CHECK(!g_irq_latency[0].path[path].count);
    const irq_latency_witness_t *w = &g_irq_latency[0].excluded[0].maximum;
    printf("EARLY_WITNESS [");
    for (unsigned i = 0; i < IL_FIELDS; ++i)
        printf("%s%llu", i ? "," : "", (unsigned long long)w->cells[i]);
    puts("]");
    return 0;
}
