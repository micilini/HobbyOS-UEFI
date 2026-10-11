#include "irq_latency_interior.h"
#include "irq_latency_time.h"
#include "irq_latency_record.h"

bool irq_latency_interior_valid(const irq_latency_interior_witness_t *w)
{
    if (!w || !w->paths || (w->paths & ~IRQ_LATENCY_ALL_PATHS)) return false;
    const irq_latency_interior_point_t *a=&w->begin, *b=&w->end;
    return a->tsc && a->tsc < b->tsc && !(a->flags & IRQ_LATENCY_IF) && !(b->flags & IRQ_LATENCY_IF) &&
        a->cpu == b->cpu && a->epoch && a->epoch == b->epoch &&
        a->segment && a->segment != UINT64_MAX && a->segment == b->segment &&
        a->switches != UINT64_MAX && a->switches == b->switches &&
        a->entries != UINT64_MAX && a->entries == b->entries &&
        a->faults != UINT64_MAX && a->faults == b->faults &&
        a->owner == b->owner && a->generation && a->generation == b->generation &&
        a->depth && a->depth == b->depth && a->lock && a->lock == b->lock &&
        a->queries <= b->queries && b->queries != UINT64_MAX &&
        a->recorder_sequence <= b->recorder_sequence && b->recorder_sequence != UINT64_MAX;
}

bool irq_latency_interior_minimum(uint64_t cycles, uint64_t pre_upper,
    uint64_t post_upper, uint64_t *frequency_upper, uint64_t *minimum_ns)
{
    uint64_t upper, ns;
    if (!frequency_upper || !minimum_ns || !pre_upper || !post_upper ||
        !irq_latency_muldiv(pre_upper > post_upper ? pre_upper : post_upper,
            UINT64_C(1010000), UINT64_C(1000000), true, &upper) || !upper ||
        !irq_latency_muldiv(cycles, UINT64_C(1000000000), upper, false, &ns)) return false;
    *frequency_upper=upper;*minimum_ns=ns;return true;
}

bool irq_latency_writeback_valid(const irq_latency_writeback_witness_t *w)
{
    if (!w || !w->request_generation || w->request_generation == UINT64_MAX ||
        !w->root || (w->root & 4095)) return false;
    const irq_latency_interior_point_t *a=&w->begin, *b=&w->end;
    return a->tsc && a->tsc < b->tsc && !(a->flags & IRQ_LATENCY_IF) && !(b->flags & IRQ_LATENCY_IF) &&
        a->cpu == b->cpu && a->epoch && a->epoch == b->epoch &&
        a->segment && a->segment != UINT64_MAX && a->segment == b->segment &&
        a->switches != UINT64_MAX && a->switches == b->switches &&
        a->entries != UINT64_MAX && a->entries == b->entries &&
        a->faults != UINT64_MAX && a->faults == b->faults &&
        a->owner == b->owner && a->generation && a->generation == b->generation &&
        a->depth == b->depth && !a->lock && !b->lock &&
        a->queries < UINT64_MAX-1 && a->queries+1 == b->queries &&
        a->recorder_sequence == b->recorder_sequence && b->recorder_sequence != UINT64_MAX;
}

bool irq_latency_writeback_record(irq_latency_writeback_record_t *r,
    const irq_latency_writeback_witness_t *w)
{
    if (!r) return false;
    if (!irq_latency_writeback_valid(w) || r->count == UINT64_MAX ||
        w->request_generation <= r->last_generation) {
        if (r->invalid != UINT64_MAX) ++r->invalid;
        return false;
    }
    if (!r->count) { r->first=*w; r->first_generation=w->request_generation; }
    ++r->count; r->last_generation=w->request_generation;
    uint64_t cycles=w->end.tsc-w->begin.tsc;
    if (cycles > r->maximum_cycles) { r->maximum_cycles=cycles; r->maximum=*w; }
    return true;
}

bool irq_latency_clock_valid(const irq_latency_clock_witness_t *w)
{
    if (!w || !w->site || !w->sequence || w->sequence == UINT64_MAX || w->phase<1 || w->phase>4 ||
        !w->paths || (w->paths & ~IRQ_LATENCY_ALL_PATHS)) return false;
    const irq_latency_interior_point_t *a=&w->begin, *b=&w->end;
    return a->tsc && a->tsc < b->tsc && !(a->flags & IRQ_LATENCY_IF) && !(b->flags & IRQ_LATENCY_IF) &&
        a->cpu == b->cpu && a->epoch && a->epoch == b->epoch &&
        a->segment && a->segment != UINT64_MAX && a->segment == b->segment &&
        a->switches != UINT64_MAX && a->switches == b->switches &&
        a->entries != UINT64_MAX && a->entries == b->entries &&
        a->faults != UINT64_MAX && a->faults == b->faults &&
        a->owner == b->owner && a->generation && a->generation == b->generation &&
        a->depth == b->depth && !a->lock && !b->lock &&
        a->queries <= b->queries && b->queries != UINT64_MAX &&
        a->recorder_sequence <= b->recorder_sequence && b->recorder_sequence != UINT64_MAX;
}

bool irq_latency_tick_valid(const irq_latency_tick_witness_t *w)
{
    if (!w || !w->sequence || w->sequence==UINT64_MAX || !w->outer_sequence ||
        w->outer_sequence==UINT64_MAX || (w->paths & ~IRQ_LATENCY_ALL_PATHS) ||
        w->sample_valid!=1 || !w->sample_ticks ||
        !((w->sample_source==1 && w->source_epoch==1 && w->sample_mode>=1 && w->sample_mode<=2 &&
           w->sample_retries<=4 && (w->sample_mode!=2 || !w->sample_retries)) ||
          (w->sample_source==2 && w->source_epoch==2 && !w->sample_mode && w->sample_retries<4 &&
           w->sample_begin<w->sample_ticks && w->sample_ticks<w->counter_end)) ||
        !(w->begin.tsc < w->sample_begin && w->sample_begin < w->counter_end && w->counter_end < w->sample_end &&
          w->sample_end < w->after_clock && w->after_clock < w->after_work &&
          w->after_work < w->end.tsc)) return false;
    const irq_latency_interior_point_t *a=&w->begin, *b=&w->end;
    return a->tsc && !(a->flags & IRQ_LATENCY_IF) && !(b->flags & IRQ_LATENCY_IF) &&
        a->cpu==b->cpu && a->epoch && a->epoch==b->epoch &&
        a->segment && a->segment!=UINT64_MAX && a->segment==b->segment &&
        a->switches!=UINT64_MAX && a->switches==b->switches &&
        a->entries!=UINT64_MAX && a->entries==b->entries &&
        a->faults!=UINT64_MAX && a->faults==b->faults &&
        a->owner==b->owner && a->generation && a->generation==b->generation &&
        a->depth==b->depth && !a->lock && !b->lock &&
        a->queries<=b->queries && b->queries!=UINT64_MAX &&
        a->recorder_sequence<=b->recorder_sequence && b->recorder_sequence!=UINT64_MAX;
}
