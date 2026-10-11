#include "../kernel/src/core/irq_latency_heap.h"
#include <assert.h>
#include <stdio.h>
static irq_latency_heap_event_t good(void)
{
    irq_latency_heap_event_t e={0};
    e.operation=IRQ_HEAP_FREE;e.sequence=1;e.marks=2;
    e.body.paths=1;
    e.body.begin=(irq_latency_interior_point_t){.tsc=100,.cpu=2,.epoch=1,.segment=3,
        .switches=4,.entries=5,.owner=6,.generation=1,.depth=1,.lock=4096};
    e.body.end=e.body.begin;e.body.end.tsc=1000;
    e.mark[0]=(irq_latency_heap_mark_t){.tsc=300,.site=IRQ_HEAP_REGION,.work=1};
    e.mark[1]=(irq_latency_heap_mark_t){.tsc=900,.site=IRQ_HEAP_CONTAINMENT,.work=15};
    e.wait_start=50;e.acquired=80;return e;
}
int main(void)
{
    irq_latency_heap_event_t e=good();irq_latency_heap_record_t r={0};
    assert(irq_latency_heap_valid(&e));assert(irq_latency_heap_record(&r,&e));
    assert(r.count==1 && r.maximum_cycles==900 && r.maximum.mark[1].work==15);
    e.sequence=2;e.body.end.tsc=950;assert(irq_latency_heap_record(&r,&e));
    assert(r.count==2 && r.maximum_cycles==900 && r.maximum.sequence==1);
    assert(!irq_latency_heap_record(&r,&e) && r.invalid==1);
#define REJECT(statement) do { e=good();statement;assert(!irq_latency_heap_valid(&e)); } while(0)
    REJECT(e.body.end.flags=512);REJECT(++e.body.end.segment);
    REJECT(++e.body.end.cpu);REJECT(++e.body.end.owner);REJECT(++e.body.end.generation);
    REJECT(++e.body.end.depth);REJECT(++e.body.end.entries);REJECT(++e.body.end.switches);
    REJECT(++e.body.end.faults);REJECT(e.body.end.lock=0);REJECT(e.marks=9);
    REJECT(e.mark[0].tsc=100);REJECT(e.mark[1].tsc=300);REJECT(e.mark[1].tsc=1000);
    REJECT(e.mark[1].site=99);REJECT(e.mark[1].site=IRQ_HEAP_REGION);
    REJECT(e.acquired=101);REJECT(e.wait_start=80);REJECT(e.sequence=0);
    REJECT(e.operation=0);REJECT(e.pending_begin=65);REJECT(e.pending_end=65);
    r.count=UINT64_MAX;e=good();e.sequence=3;
    assert(!irq_latency_heap_record(&r,&e));
    puts("IRQ latency actual-C bounded heap stages and continuity: PASS minimum_only=1");
}
