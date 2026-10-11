#include "irq_latency_heap.h"
#include "irq_latency.h"
#include "irq_latency_record.h"

bool irq_latency_heap_valid(const irq_latency_heap_event_t *e)
{
    if (!e || !irq_latency_interior_valid(&e->body) || !e->operation ||
        e->operation>IRQ_HEAP_OPERATIONS || !e->sequence || e->sequence==UINT64_MAX ||
        !e->marks || e->marks>IRQ_HEAP_MARKS || !e->wait_start ||
        e->wait_start>=e->acquired || e->acquired>=e->body.begin.tsc ||
        e->pending_begin>IRQ_LATENCY_PENDING || e->pending_end>IRQ_LATENCY_PENDING) return false;
    uint64_t previous=e->body.begin.tsc, previous_site=0;
    for (unsigned i=0;i<e->marks;++i) {
        const irq_latency_heap_mark_t *m=&e->mark[i];
        if (m->tsc<=previous || m->tsc>=e->body.end.tsc || !m->site ||
            m->site>IRQ_HEAP_INIT || m->site==previous_site || m->work==UINT64_MAX ||
            m->bytes==UINT64_MAX) return false;
        previous=m->tsc;previous_site=m->site;
    }
    return true;
}

bool irq_latency_heap_record(irq_latency_heap_record_t *r, const irq_latency_heap_event_t *e)
{
    if (!r) return false;
    if (!irq_latency_heap_valid(e) || r->count==UINT64_MAX || e->sequence<=r->last_sequence) {
        if (r->invalid!=UINT64_MAX) ++r->invalid;
        return false;
    }
    ++r->count;r->last_sequence=e->sequence;
    if (e->mark[e->marks-1].site==IRQ_HEAP_METADATA) ++r->metadata_count;
    uint64_t cycles=e->body.end.tsc-e->body.begin.tsc;
    if (cycles>r->maximum_cycles) {
        r->maximum_cycles=cycles;
        r->maximum.body=e->body;
        r->maximum.operation=e->operation;r->maximum.sequence=e->sequence;
        r->maximum.marks=e->marks;r->maximum.wait_start=e->wait_start;
        r->maximum.acquired=e->acquired;r->maximum.pending_begin=e->pending_begin;
        r->maximum.pending_end=e->pending_end;r->maximum.requested_pages=e->requested_pages;
        for (unsigned i=0;i<e->marks;++i) r->maximum.mark[i]=e->mark[i];
    }
    return true;
}

bool irq_latency_heap_operation_valid(const irq_latency_heap_operation_t *w)
{
    if (!w || !irq_latency_interior_valid(&w->interval) || !irq_latency_interior_valid(&w->parent) ||
        !w->sequence || w->sequence==UINT64_MAX ||
        (w->operation!=IRQ_HEAP_ALLOCATE && w->operation!=IRQ_HEAP_RECHECK) ||
        (w->header_bytes!=0 && w->header_bytes!=32) ||
        w->parent.paths!=(UINT64_C(1)<<IRQ_PATH_HEAP) || w->interval.paths!=w->parent.paths ||
        !(w->parent.begin.tsc<w->previous_mark && w->previous_mark<w->prepared &&
          w->prepared<w->interval.begin.tsc && w->interval.begin.tsc<w->interval.end.tsc &&
          w->interval.end.tsc<w->validated && w->validated<w->next_mark &&
          w->next_mark<w->parent.end.tsc)) return false;
    irq_latency_interior_witness_t enclosure={w->parent.begin,w->interval.begin,w->parent.paths};
    if (!irq_latency_interior_valid(&enclosure)) return false;
    enclosure.begin=w->interval.end;enclosure.end=w->parent.end;
    return irq_latency_interior_valid(&enclosure);
}

bool irq_latency_heap_operation_record(irq_latency_heap_operation_record_t *r,
    const irq_latency_heap_operation_t *w)
{
    if (!r) return false;
    if (!irq_latency_heap_operation_valid(w) || r->count==UINT64_MAX || w->sequence<=r->last_sequence) {
        if (r->invalid!=UINT64_MAX) ++r->invalid;
        return false;
    }
    ++r->count;r->last_sequence=w->sequence;
    uint64_t cycles=w->interval.end.tsc-w->interval.begin.tsc;
    if (cycles>r->maximum_cycles) { r->maximum_cycles=cycles;r->maximum=*w; }
    return true;
}
