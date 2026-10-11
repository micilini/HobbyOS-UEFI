#include "../kernel/src/core/irq_latency_boundary.h"
#include "../kernel/src/core/irq_latency_tsc.h"
#include <assert.h>
#include <stdio.h>

uint64_t irq_latency_host_if = IRQ_LATENCY_IF;
static bool ready;
static unsigned entered, completed;
static irq_latency_recorder_t recorder;

void irq_latency_host_save_probe(uint64_t flags, uint64_t *returned, uint64_t *observed);
void irq_latency_host_restore_probe(uint64_t flags, uint64_t *observed);
void irq_latency_host_tail_probe(irq_latency_pending_t *pending, uint64_t *probe_end);

bool irq_latency_before_mask(irq_latency_boundary_t *b)
{
    assert(*(const uint64_t *)(uintptr_t)b->stack_pointer == b->site);
    assert(b->flags == b->observed_flags);
    b->cpu = 0; b->switch_sequence = 1;
    return ready;
}
void irq_latency_after_mask(irq_latency_boundary_t *b)
{
    assert(!(irq_latency_host_if & IRQ_LATENCY_IF));
    assert(irq_latency_record_begin(&recorder, b->flags, b->tsc, b->site, 1, IRQ_LATENCY_THREAD));
    ++entered;
}
bool irq_latency_before_restore(irq_latency_boundary_t *b)
{
    assert(*(const uint64_t *)(uintptr_t)b->stack_pointer == b->site);
    if (!ready || !(b->flags & IRQ_LATENCY_IF)) return false;
    assert(!(b->observed_flags & IRQ_LATENCY_IF));
    irq_latency_ticket_t t = irq_latency_record_close(&recorder, b->flags, b->site);
    assert(t.valid);
    b->ticket_sequence = t.sequence; b->ticket_slot = t.slot;
    return true;
}
void irq_latency_after_restore(irq_latency_boundary_t *b)
{
    assert(irq_latency_host_if & IRQ_LATENCY_IF);
    irq_latency_ticket_t t = {b->ticket_sequence, (uint32_t)b->ticket_slot, true};
    assert(irq_latency_record_complete(&recorder, t, b->tsc, 0, IRQ_LATENCY_VALID));
    ++completed;
}

int main(void)
{
    assert(irq_latency_record_init(&recorder, 0, 1, UINT64_MAX));
    const uint64_t mask = UINT64_C(0xcd5); /* OF, DF, SF, ZF, AF, PF, CF. */
    const uint64_t variants[] = {UINT64_C(0x202), UINT64_C(0xed7), UINT64_C(0x247)};
    for (unsigned active = 0; active < 2; ++active) {
        ready = active != 0;
        for (unsigned i = 0; i < sizeof(variants)/sizeof(variants[0]); ++i) {
            uint64_t outer, inner, observed, target = variants[i];
            irq_latency_host_save_probe(target, &outer, &observed);
            assert((outer & (mask | IRQ_LATENCY_IF)) == (target & (mask | IRQ_LATENCY_IF)));
            assert((observed & mask) == (target & mask));
            assert(!(irq_latency_host_if & IRQ_LATENCY_IF));
            irq_latency_host_save_probe(target & ~IRQ_LATENCY_IF, &inner, &observed);
            assert(!(inner & IRQ_LATENCY_IF));
            irq_latency_host_restore_probe(inner, &observed);
            assert((observed & mask) == (inner & mask));
            assert(!(irq_latency_host_if & IRQ_LATENCY_IF));
            irq_latency_host_restore_probe(outer, &observed);
            assert((observed & mask) == (outer & mask));
            assert(irq_latency_host_if & IRQ_LATENCY_IF);
            if (active) assert(irq_latency_record_drain(&recorder));
        }
    }
    assert(entered == 6 && completed == 3 && recorder.path[0].count == 3);
    assert(!recorder.invalid && !recorder.unclosed);
    assert(irq_latency_record_begin(&recorder, IRQ_LATENCY_IF, irq_latency_read_tsc(), 0xff, 1,
                                    IRQ_LATENCY_HARD_IRQ));
    irq_latency_ticket_t ticket = irq_latency_record_close(&recorder, IRQ_LATENCY_IF, 0xff);
    irq_latency_pending_t *pending = irq_latency_record_prepare_tail(&recorder, ticket, 0, 100);
    assert(pending && (pending->ready & 3) == 3);
    assert(!irq_latency_record_prepare_tail(&recorder, ticket, 0, 100));
    uint64_t end = 0;
    irq_latency_host_tail_probe(pending, &end);
    assert(end && pending->witness.cells[IL_END] == end && pending->witness.cells[IL_OBSERVED_END] == end);
    assert((pending->ready & 3) == 2);
    assert(irq_latency_record_drain(&recorder) && !recorder.invalid && recorder.path[0].count == 4);
    irq_latency_host_tail_probe(NULL, NULL);
    puts("IRQ latency actual-assembly flags and ordered envelopes: PASS privileged_IF=HOSTED");
    return 0;
}
