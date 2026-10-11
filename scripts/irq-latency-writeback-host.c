#include "../kernel/src/core/irq_latency_interior.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static irq_latency_writeback_witness_t good(void)
{
    irq_latency_writeback_witness_t w={.request_generation=7,.root=4096};
    w.begin=(irq_latency_interior_point_t){.tsc=100,.cpu=2,.epoch=1,.segment=3,.switches=4,
        .entries=5,.faults=6,.owner=7,.generation=8,.queries=10,.recorder_sequence=20};
    w.end=w.begin;w.end.tsc=100000;++w.end.queries;return w;
}

int main(void)
{
    irq_latency_writeback_witness_t w=good();assert(irq_latency_writeback_valid(&w));
#define REJECT(field,value) do{w=good();w.end.field=(value);assert(!irq_latency_writeback_valid(&w));}while(0)
    REJECT(flags,512);REJECT(cpu,3);REJECT(epoch,2);REJECT(segment,4);REJECT(switches,5);
    REJECT(entries,6);REJECT(faults,7);REJECT(owner,8);REJECT(generation,9);REJECT(depth,1);
    REJECT(lock,4096);REJECT(tsc,100);REJECT(queries,12);REJECT(recorder_sequence,21);
    irq_latency_writeback_record_t r={0};w=good();assert(irq_latency_writeback_record(&r,&w));
    assert(r.count==1 && r.maximum_cycles==99900 && r.first.request_generation==7);
    assert(!irq_latency_writeback_record(&r,&w) && r.invalid==1);
    ++w.request_generation;w.end.tsc=200000;assert(irq_latency_writeback_record(&r,&w));
    assert(r.maximum_cycles==199900 && r.first.request_generation==7 && r.last_generation==8);
    r.count=UINT64_MAX;++w.request_generation;assert(!irq_latency_writeback_record(&r,&w));
    w=good();w.root=4097;assert(!irq_latency_writeback_valid(&w));
    w=good();w.request_generation=0;assert(!irq_latency_writeback_valid(&w));
    uint64_t hz,ns;
    for(unsigned n=49999;n<=50001;++n){
        uint64_t cycles=((uint64_t)n*1010000000+999999999)/1000000000;
        assert(irq_latency_interior_minimum(cycles,1000000000,1000000000,&hz,&ns) && ns==n);
    }
    irq_latency_writeback_stamps_t stamps={0};
    irq_latency_writeback_execute(&stamps);
    assert(stamps.begin && stamps.end>stamps.begin);
    assert((stamps.begin_flags&512) && (stamps.end_flags&512));
    puts("IRQ latency actual-C writeback continuity and assembly timestamps: PASS wbinvd=HOSTED_NOP minimum_only=1");
}
