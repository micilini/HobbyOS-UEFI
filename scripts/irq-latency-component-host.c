#include "../kernel/src/core/irq_latency_component.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    irq_latency_component_t row = {0};
    assert(irq_latency_component_add(&row, 100, 50100, 1, 4096, 2, true, true));
    assert(irq_latency_component_add(&row, 100000, 100001, 2, 4096, 2, true, true));
    assert(row.count == 2 && row.maximum == 50000 && row.generation == 1 && row.last_generation == 2);
    assert(!irq_latency_component_add(&row, 30, 20, 3, 4096, 2, true, true));
    assert(!irq_latency_component_add(&row, 30, 40, 4, 4096, 2, false, true));
    assert(!irq_latency_component_add(&row, 30, 40, 0, 4096, 2, true, true));
    assert(!irq_latency_component_add(&row, 30, 40, 5, 4097, 2, true, false));
    assert(row.count == 6 && row.invalid == 4 && row.failures == 1 && row.maximum == 50000);
    assert(irq_latency_component_add(&row, 1, UINT64_MAX, 6, 8192, 1, true, false));
    assert(row.maximum == UINT64_MAX-1 && row.begin == 1 && row.end == UINT64_MAX && row.failures == 2);
    row.count = UINT64_MAX;
    assert(!irq_latency_component_add(&row, 1, 2, 7, 8192, 1, true, true));
    assert(row.count == UINT64_MAX && row.maximum == UINT64_MAX-1);
    puts("IRQ latency actual-C callback and request maximum witnesses: PASS");
    return 0;
}
