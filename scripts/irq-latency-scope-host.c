#include "../kernel/src/core/irq_latency_scope.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    irq_latency_scope_state_t a = {0}, b = {0};
    irq_latency_scope_token_t heap = {0}, pmm = {0}, other = {0};
    assert(irq_latency_scope_push(&a, 128, 7, 1, &heap));
    assert(irq_latency_scope_push(&a, 128, 7, 2, &pmm));
    assert(a.paths == 3 && a.depth == 2);
    irq_latency_scope_token_t irq = {0}, callback = {0};
    assert(irq_latency_scope_interrupt(&a, 128, 7, &irq));
    assert(!a.paths && a.depth == 3);
    assert(irq_latency_scope_push(&a, 128, 7, 256, &callback));
    assert(a.paths == 256);
    assert(irq_latency_scope_pop(&a, 128, 7, &callback));
    assert(!a.paths);
    assert(irq_latency_scope_pop(&a, 128, 7, &irq));
    assert(a.paths == 3 && a.depth == 2);
    /* Migration carries the task's stack slot; another task on the original
     * CPU observes its own slot, not the suspended task's tags. */
    assert(irq_latency_scope_push(&b, 129, 9, 4, &other));
    assert(b.paths == 4 && a.paths == 3);
    assert(irq_latency_scope_pop(&a, 128, 7, &pmm));
    assert(a.paths == 1 && a.depth == 1);
    assert(irq_latency_scope_pop(&a, 128, 7, &heap));
    assert(!a.paths && !a.depth && irq_latency_scope_retired(&a, 7));
    assert(irq_latency_scope_pop(&b, 129, 9, &other));
    assert(irq_latency_scope_push(&a, 128, 8, 2, &heap));
    assert(!irq_latency_scope_pop(&a, 128, 7, &heap));
    assert(a.paths == 2 && a.depth == 1 && a.invalid == 1);
    assert(!irq_latency_scope_retired(&a, 8));
    assert(!irq_latency_scope_push(&a, 128, 9, 4, &other));
    assert(irq_latency_scope_pop(&a, 128, 8, &heap));
    assert(a.invalid == 3);
    irq_latency_scope_token_t nested[IRQ_LATENCY_SCOPE_DEPTH] = {0};
    for (unsigned i = 0; i < IRQ_LATENCY_SCOPE_DEPTH; ++i)
        assert(irq_latency_scope_push(&b, 129, 9, 1, &nested[i]));
    assert(!irq_latency_scope_push(&b, 129, 9, 1, &other));
    assert(!irq_latency_scope_pop(&b, 129, 9, &nested[0]));
    for (unsigned i = IRQ_LATENCY_SCOPE_DEPTH; i != 0; --i)
        assert(irq_latency_scope_pop(&b, 129, 9, &nested[i-1]));
    assert(!b.paths && !b.depth && b.invalid == 2);
    irq_latency_scope_token_t abandoned[2] = {0};
    assert(irq_latency_scope_interrupt(&a, 128, 8, &abandoned[0]));
    assert(irq_latency_scope_interrupt(&a, 128, 8, &abandoned[1]));
    assert(!irq_latency_scope_retire_interrupts(&a, 128, 9, abandoned, 2));
    assert(a.depth == 2 && abandoned[0].active && abandoned[1].active);
    assert(irq_latency_scope_retire_interrupts(&a, 128, 8, abandoned, 2));
    assert(!a.depth && !a.paths && !abandoned[0].active && !abandoned[1].active);
    assert(irq_latency_scope_push(&a, 128, 9, 1, &heap));
    assert(irq_latency_scope_interrupt(&a, 128, 9, &abandoned[0]));
    assert(!irq_latency_scope_retire_interrupts(&a, 128, 9, abandoned, 1));
    assert(a.depth == 2 && abandoned[0].active);
    assert(irq_latency_scope_pop(&a, 128, 9, &abandoned[0]));
    assert(irq_latency_scope_pop(&a, 128, 9, &heap));
    puts("IRQ latency actual-C stack-owned scope attribution: PASS");
    return 0;
}
