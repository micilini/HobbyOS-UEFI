/* Actual controller and assembly; only privileged IF effects are substituted. */
#include <stdint.h>
uint64_t irq_latency_host_if;
#define IRQ_LATENCY_HOST_IF_QUERY() (host_flags = irq_latency_host_if)
#define irq_latency_enable inherited_enable_stub
#define main inherited_controller_main
#include "irq-latency-controller-host.c"
#undef main
#undef irq_latency_enable

void irq_latency_host_save_probe(uint64_t flags, uint64_t *returned, uint64_t *observed);
void irq_latency_host_restore_probe(uint64_t flags, uint64_t *observed);
static irq_latency_recorder_t before_nested;

int main(void)
{
    const uint64_t mask = UINT64_C(0xcd5);
    const uint64_t variants[] = {UINT64_C(0x202), UINT64_C(0xed7), UINT64_C(0x247)};
    for (unsigned i = 0; i < sizeof(variants)/sizeof(variants[0]); ++i) {
        reset();
        uint64_t outer, inner, observed;
        irq_latency_host_save_probe(variants[i], &outer, &observed);
        CHECK((outer & (mask|IRQ_LATENCY_IF)) == (variants[i] & (mask|IRQ_LATENCY_IF)));
        CHECK(!irq_is_enabled() && g_irq_latency[0].active);
        CHECK(irq_latency_record_tag(&g_irq_latency[0], 3));
        before_nested = g_irq_latency[0];
        uint64_t queries = identity_queries;
        irq_latency_host_save_probe(variants[i] & ~IRQ_LATENCY_IF, &inner, &observed);
        CHECK(!(inner & IRQ_LATENCY_IF) && !irq_is_enabled());
        CHECK((observed & mask) == (inner & mask));
        CHECK(g_irq_nested_elided[0].prechecked == 1 && g_irq_nested_elided[0].count == 1);
        CHECK(identity_queries == queries);
        CHECK(memcmp(&before_nested, &g_irq_latency[0], sizeof(before_nested)) == 0);
        irq_latency_host_restore_probe(inner, &observed);
        CHECK(!irq_is_enabled() && (observed & mask) == (inner & mask));
        CHECK(memcmp(&before_nested, &g_irq_latency[0], sizeof(before_nested)) == 0);
        irq_latency_host_restore_probe(outer, &observed);
        CHECK(irq_is_enabled() && (observed & mask) == (outer & mask));
        CHECK(irq_latency_record_drain(&g_irq_latency[0]));
        CHECK(!g_irq_invalid_boundaries && !g_irq_latency[0].pending_count);
        CHECK(g_irq_latency[0].path[0].count == 1 && g_irq_latency[0].path[1].count == 1);
        CHECK(g_irq_latency[0].path[0].maximum.cells[IL_BEGIN] == before_nested.begin);
        CHECK(g_irq_latency[0].path[0].maximum.cells[IL_END] > before_nested.begin);
    }
    puts("IRQ latency actual controller/assembly nested precheck: PASS flags=EXACT outer=RETAINED privileged_IF=HOSTED");
    return 0;
}
