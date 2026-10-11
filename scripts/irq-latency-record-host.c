#include "../kernel/src/core/irq_latency_record.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static irq_latency_recorder_t recorder;
static irq_latency_snapshot_t snapshot;

static void fresh(void)
{
    memset(&recorder, 0, sizeof(recorder));
    assert(!irq_latency_record_snapshot(&recorder, &snapshot));
    assert(irq_latency_record_init(&recorder, 2, 1, 149997));
    assert(!irq_latency_record_init(&recorder, 2, 2, 149997));
}

static irq_latency_ticket_t interval(uint64_t start, uint64_t end, uint64_t paths)
{
    assert(irq_latency_record_begin(&recorder, IRQ_LATENCY_IF, start, 10, paths, IRQ_LATENCY_THREAD));
    irq_latency_ticket_t t = irq_latency_record_close(&recorder, IRQ_LATENCY_IF, 20);
    assert(t.valid);
    assert(irq_latency_record_complete(&recorder, t, end, 2, IRQ_LATENCY_VALID));
    assert(!irq_latency_record_complete(&recorder, t, end, 2, IRQ_LATENCY_VALID));
    assert(irq_latency_record_drain(&recorder));
    return t;
}

int main(void)
{
    for (unsigned bad = 0; bad < 3; ++bad) {
        fresh();
        assert(irq_latency_record_begin(&recorder, IRQ_LATENCY_IF, 100, 10, 1, IRQ_LATENCY_HARD_IRQ));
        irq_latency_ticket_t tail = irq_latency_record_close(&recorder, IRQ_LATENCY_IF, 20);
        assert(!irq_latency_record_prepare_tail(&recorder, tail, 3, 0));
        irq_latency_pending_t *pending = irq_latency_record_prepare_tail(&recorder, tail, 2, bad == 2 ? UINT64_MAX : 0);
        assert(pending && (pending->ready & 3) == 3);
        assert(irq_latency_record_drain(&recorder) && recorder.pending_count == 1);
        pending->witness.cells[IL_END] = bad == 0 ? 99 : 200;
        if (bad == 1) pending->witness.cells[IL_END_CPU] = 3;
        __atomic_store_n(&pending->ready, (tail.sequence << 2) | 2, __ATOMIC_RELEASE);
        assert(irq_latency_record_drain(&recorder));
        assert(recorder.invalid == 1 && recorder.failure_count == 1);
    }
    fresh();
    assert(irq_latency_record_begin(&recorder, IRQ_LATENCY_IF, 100, 10, 1, IRQ_LATENCY_THREAD));
    assert(irq_latency_record_begin(&recorder, 0, 500, 11, 2, IRQ_LATENCY_THREAD));
    assert(!irq_latency_record_close(&recorder, 0, 12).valid);
    assert(irq_latency_record_components(&recorder, 400, 100));
    irq_latency_ticket_t outer = irq_latency_record_close(&recorder, IRQ_LATENCY_IF, 13);
    assert(outer.valid && recorder.pending_count == 1);
    assert(irq_latency_record_snapshot(&recorder, &snapshot));
    assert(snapshot.pending == 1 && !snapshot.path[0].count);
    assert(irq_latency_record_complete(&recorder, outer, 800, 2, IRQ_LATENCY_VALID));
    assert(irq_latency_record_drain(&recorder));
    assert(irq_latency_record_snapshot(&recorder, &snapshot));
    for (unsigned path = 0; path < 2; ++path) {
        assert(snapshot.path[path].count == 1 && snapshot.path[path].maximum_cycles == 700);
        assert(snapshot.path[path].maximum.cells[IL_BEGIN] == 100);
        assert(snapshot.path[path].maximum.cells[IL_END] == 800);
        assert(snapshot.path[path].maximum.cells[IL_PATHS] == 3);
        assert(snapshot.path[path].maximum.cells[IL_WAIT] == 400);
        assert(snapshot.path[path].maximum.cells[IL_HELD] == 100);
    }
    interval(1000, 1100, 1);
    assert(irq_latency_record_snapshot(&recorder, &snapshot));
    assert(snapshot.path[0].maximum_cycles == 700 && snapshot.path[0].count == 2);
    /* An IF-enabled wait between two intervals contributes to neither one. */
    interval(UINT64_C(10000000000), UINT64_C(10000000100), 1);
    assert(recorder.path[0].maximum_cycles == 700);
    interval(2000, 151997, 4);
    assert(recorder.failure_count == 0);
    interval(2000, 152000, 4);
    assert(recorder.failure_count == 1 && recorder.path[2].maximum_cycles == 150000);
    assert(recorder.failures[0].cells[IL_END] == 152000);
    fresh();
    /* Repeated CLI does not reset the active beginning. */
    assert(irq_latency_record_begin(&recorder, IRQ_LATENCY_IF, 100, 10, 1, IRQ_LATENCY_THREAD));
    for (unsigned i = 0; i < 16; ++i)
        assert(irq_latency_record_begin(&recorder, 0, 200+i, 11, 2, IRQ_LATENCY_THREAD));
    outer = irq_latency_record_close(&recorder, IRQ_LATENCY_IF, 20);
    assert(irq_latency_record_complete(&recorder, outer, 1000, 2, IRQ_LATENCY_VALID));
    assert(irq_latency_record_drain(&recorder));
    assert(recorder.path[0].maximum_cycles == 900 && recorder.path[1].maximum_cycles == 900);
    assert(!irq_latency_record_close(&recorder, IRQ_LATENCY_IF, 20).valid);
    assert(recorder.missing_begin == 1 && recorder.invalid == 1);
    fresh();
    assert(irq_latency_record_begin(&recorder, 0, 100, 10, 1, IRQ_LATENCY_THREAD));
    outer = irq_latency_record_close(&recorder, IRQ_LATENCY_IF, 20);
    assert(irq_latency_record_complete(&recorder, outer, 200, 2, IRQ_LATENCY_VALID));
    assert(irq_latency_record_drain(&recorder));
    assert(recorder.invalid == 1 && recorder.path[0].invalid == 1);
    assert(recorder.path[0].maximum_cycles == 0 && recorder.failure_count == 1);
    for (unsigned context = IRQ_LATENCY_HARD_IRQ; context <= IRQ_LATENCY_PANIC; ++context) {
        fresh();
        assert(irq_latency_record_begin(&recorder, 0, 100, 10, 1, context));
        outer = irq_latency_record_close(&recorder, IRQ_LATENCY_IF, 20);
        assert(irq_latency_record_complete(&recorder, outer, 200, 2, IRQ_LATENCY_VALID));
        assert(irq_latency_record_drain(&recorder));
        assert(!recorder.invalid);
        assert(recorder.path[0].count == (context == IRQ_LATENCY_HARD_IRQ));
    }
    for (unsigned bad = 0; bad < 5; ++bad) {
        fresh();
        uint64_t start = bad == 3 ? UINT64_MAX-4 : 100;
        assert(irq_latency_record_begin(&recorder, IRQ_LATENCY_IF, start, 10, 1, IRQ_LATENCY_THREAD));
        outer = irq_latency_record_close(&recorder, IRQ_LATENCY_IF, 20);
        assert(irq_latency_record_complete(&recorder, outer, bad == 2 ? 100 : bad == 3 ? 2 : 99,
            bad == 0 ? 3 : 2, bad == 4 ? IRQ_LATENCY_BOUNDARY_UNCERTAIN : IRQ_LATENCY_VALID));
        assert(irq_latency_record_drain(&recorder));
        assert(recorder.invalid == 1 && recorder.path[0].invalid == 1 && recorder.failure_count == 1);
    }
    fresh();
    recorder.busy = 1;
    assert(!irq_latency_record_begin(&recorder, IRQ_LATENCY_IF, 100, 10, 1, IRQ_LATENCY_THREAD));
    assert(recorder.recursion == 1 && !irq_latency_record_snapshot(&recorder, &snapshot));
    recorder.busy = 0; recorder.sequence = 1;
    assert(!irq_latency_record_snapshot(&recorder, &snapshot));
    fresh(); recorder.next_sequence = UINT64_MAX;
    assert(!irq_latency_record_begin(&recorder, IRQ_LATENCY_IF, 100, 10, 1, IRQ_LATENCY_THREAD));
    assert(recorder.exhausted);
    fresh(); recorder.sequence = UINT64_MAX-1;
    assert(!irq_latency_record_begin(&recorder, IRQ_LATENCY_IF, 100, 10, 1, IRQ_LATENCY_THREAD));
    assert(recorder.exhausted);
    fresh();
    for (unsigned i = 0; i < IRQ_LATENCY_PENDING; ++i) {
        assert(irq_latency_record_begin(&recorder, IRQ_LATENCY_IF, 100, 10, 1, IRQ_LATENCY_THREAD));
        assert(irq_latency_record_close(&recorder, IRQ_LATENCY_IF, 20).valid);
    }
    assert(irq_latency_record_begin(&recorder, IRQ_LATENCY_IF, 100, 10, 1, IRQ_LATENCY_THREAD));
    assert(!irq_latency_record_close(&recorder, IRQ_LATENCY_IF, 20).valid);
    assert(recorder.exhausted && recorder.pending_count == IRQ_LATENCY_PENDING && recorder.unclosed == 1);
    fresh(); recorder.path[0].count = UINT64_MAX;
    interval(100, 200, 1);
    assert(recorder.exhausted && recorder.path[0].count == UINT64_MAX);
    fresh();
    for (unsigned i = 0; i <= IRQ_LATENCY_FAILURES; ++i) interval(100, 200000, 1);
    assert(recorder.failure_count == IRQ_LATENCY_FAILURES && recorder.failure_overflow == 1);
    assert(recorder.failures[0].cells[IL_SEQUENCE] == 1);
    /* The first IF-enabled IRQ supplies a same-CPU upper envelope even when
     * the post-POPFQ publisher is suspended or migrates before publication. */
    for (unsigned migrated = 0; migrated < 2; ++migrated) {
        fresh();
        assert(irq_latency_record_begin(&recorder, IRQ_LATENCY_IF, 100, 10, 1, IRQ_LATENCY_THREAD));
        outer = irq_latency_record_close(&recorder, IRQ_LATENCY_IF, 20);
        assert(!irq_latency_record_interrupt(&recorder, 180, 3));
        assert(irq_latency_record_interrupt(&recorder, 200, 2));
        assert(irq_latency_record_interrupt(&recorder, 50000, 2));
        assert(irq_latency_record_complete_envelope(&recorder, outer, 1000000,
            migrated ? UINT32_MAX : 2,
            migrated ? IRQ_LATENCY_BOUNDARY_UNCERTAIN : IRQ_LATENCY_VALID, 0, IRQ_BOUNDARY_DIRECT));
        assert(irq_latency_record_drain(&recorder));
        assert(!recorder.invalid && recorder.path[0].maximum_cycles == 100);
        const irq_latency_witness_t *w = &recorder.path[0].maximum;
        assert(w->cells[IL_END] == 200 && w->cells[IL_END_CPU] == 2);
        assert(w->cells[IL_OBSERVED_END] == 1000000);
        assert(w->cells[IL_OBSERVED_END_CPU] == (migrated ? UINT32_MAX : 2));
        assert(w->cells[IL_END_KIND] == IRQ_BOUNDARY_BEFORE_IRQ);
    }
    fresh();
    assert(irq_latency_record_begin_envelope(&recorder, IRQ_LATENCY_IF, 200, 10, 1,
        IRQ_LATENCY_THREAD, 10000000, UINT32_MAX, IRQ_BOUNDARY_AFTER_IRQ));
    outer = irq_latency_record_close(&recorder, IRQ_LATENCY_IF, 20);
    assert(irq_latency_record_complete(&recorder, outer, 350, 2, IRQ_LATENCY_VALID));
    assert(irq_latency_record_drain(&recorder));
    assert(recorder.path[0].maximum_cycles == 150);
    assert(recorder.path[0].maximum.cells[IL_OBSERVED_BEGIN] == 10000000);
    assert(recorder.path[0].maximum.cells[IL_BEGIN_KIND] == IRQ_BOUNDARY_AFTER_IRQ);
    for (unsigned bad = 0; bad < 3; ++bad) {
        fresh();
        assert(irq_latency_record_begin(&recorder, IRQ_LATENCY_IF, 100, 10, 1, IRQ_LATENCY_THREAD));
        outer = irq_latency_record_close(&recorder, IRQ_LATENCY_IF, 20);
        assert(irq_latency_record_interrupt(&recorder, bad == 0 ? 99 : 200, 2));
        assert(irq_latency_record_complete_envelope(&recorder, outer, bad == 1 ? 99 : 1000000,
            2, IRQ_LATENCY_VALID, bad == 2 ? UINT64_MAX : 0, IRQ_BOUNDARY_DIRECT));
        assert(irq_latency_record_drain(&recorder));
        assert(recorder.invalid == 1 && recorder.failure_count == 1);
    }
    fresh();
    assert(!irq_latency_record_begin_envelope(&recorder, IRQ_LATENCY_IF, 100, 10, 1,
        IRQ_LATENCY_THREAD, 100, 2, IRQ_BOUNDARY_BEFORE_IRQ));
    assert(irq_latency_record_begin(&recorder, IRQ_LATENCY_IF, 100, 10, 1, IRQ_LATENCY_THREAD));
    outer = irq_latency_record_close(&recorder, IRQ_LATENCY_IF, 20);
    assert(!irq_latency_record_complete_envelope(&recorder, outer, 200, 2,
        IRQ_LATENCY_VALID, 0, (irq_latency_boundary_kind_t)99));
    assert(irq_latency_record_complete(&recorder, outer, 200, 2, IRQ_LATENCY_VALID));
    assert(irq_latency_record_drain(&recorder));
    for (unsigned observed = 0; observed < 2; ++observed) {
        fresh();
        assert(irq_latency_record_begin(&recorder, IRQ_LATENCY_IF, 100, 10, 1, IRQ_LATENCY_THREAD));
        outer = irq_latency_record_close_owned(&recorder, IRQ_LATENCY_IF, 20, 140, 7);
        if (observed) assert(irq_latency_record_interrupt(&recorder, 200, 2));
        assert(irq_latency_record_retire_owner(&recorder, 141, 7) == 0);
        assert(irq_latency_record_retire_owner(&recorder, 140, 8) == 0);
        assert(irq_latency_record_retire_owner(&recorder, 140, 7) == 1);
        assert(irq_latency_record_retire_owner(&recorder, 140, 7) == 0);
        assert(!irq_latency_record_complete(&recorder, outer, 300, 2, IRQ_LATENCY_VALID));
        assert(irq_latency_record_drain(&recorder) && !recorder.pending_count);
        assert(recorder.invalid == !observed);
        if (observed) {
            assert(recorder.path[0].maximum_cycles == 100);
            const irq_latency_witness_t *w = &recorder.path[0].maximum;
            assert(w->cells[IL_END_KIND] == IRQ_BOUNDARY_RETIRED_PUBLISHER);
            assert(w->cells[IL_OWNER_SLOT] == 140 && w->cells[IL_OWNER_GENERATION] == 7);
            assert(!w->cells[IL_OBSERVED_END] && w->cells[IL_OBSERVED_END_CPU] == UINT32_MAX);
        } else assert(!recorder.path[0].maximum_cycles && recorder.failure_count == 1);
    }
    fresh();
    assert(!irq_latency_record_ticket(&recorder, 1234, 1));
    assert(irq_latency_record_begin(&recorder, IRQ_LATENCY_IF, 100, 10, 3, IRQ_LATENCY_THREAD));
    assert(irq_latency_record_ticket(&recorder, 1234, 2));
    assert(irq_latency_record_ticket(&recorder, 5678, 4));
    assert(irq_latency_record_ticket(&recorder, 9999, 0));
    assert(irq_latency_record_snapshot(&recorder, &snapshot));
    for (unsigned path = 0; path < 2; ++path) {
        assert(snapshot.contention[path].observations == 3 && snapshot.contention[path].contended == 2);
        assert(snapshot.contention[path].maximum_distance == 4 && snapshot.contention[path].lock == 5678);
        assert(snapshot.contention[path].paths == 3 && snapshot.contention[path].begin_sequence == 1);
    }
    assert(!snapshot.contention[2].observations);
    fresh();
    for (unsigned i = 0; i < IRQ_LATENCY_FAILURES+7; ++i) {
        assert(irq_latency_record_begin(&recorder, IRQ_LATENCY_IF, 100, 10, 3, IRQ_LATENCY_THREAD));
        outer = irq_latency_record_close(&recorder, IRQ_LATENCY_IF, 20);
        assert(irq_latency_record_complete(&recorder, outer, 200000+i, 2, IRQ_LATENCY_VALID));
        assert(irq_latency_record_drain(&recorder));
    }
    assert(recorder.failure_count == IRQ_LATENCY_FAILURES && recorder.failure_overflow == 7);
    assert(recorder.path[0].violations == IRQ_LATENCY_FAILURES+7);
    assert(recorder.path[1].violations == recorder.path[0].count);
    assert(recorder.path[0].maximum_cycles == 199900+IRQ_LATENCY_FAILURES+6);
    puts("IRQ latency actual-C nesting, maxima and bounded publication: PASS");
    return 0;
}
