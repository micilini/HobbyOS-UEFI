#include "../kernel/src/core/irq_latency_interior.h"
#include <assert.h>
#include <stdio.h>
static irq_latency_interior_witness_t good(void)
{
    irq_latency_interior_witness_t w={.paths=1};
    w.begin=(irq_latency_interior_point_t){.tsc=100,.cpu=2,.epoch=1,.segment=3,.switches=4,
        .entries=5,.faults=6,.owner=7,.generation=8,.depth=1,.lock=4096,.queries=10,.recorder_sequence=20};
    w.end=w.begin;w.end.tsc=100000;w.end.queries=12;w.end.recorder_sequence=22;return w;
}
int main(void)
{
    irq_latency_interior_witness_t w=good();assert(irq_latency_interior_valid(&w));
#define REJECT(field,value) do{w=good();w.end.field=(value);assert(!irq_latency_interior_valid(&w));}while(0)
    REJECT(flags,512);REJECT(cpu,3);REJECT(epoch,2);REJECT(segment,4);
    REJECT(switches,5);REJECT(entries,6);REJECT(faults,7);REJECT(owner,8);
    REJECT(generation,9);REJECT(depth,2);REJECT(lock,8192);REJECT(tsc,100);
    REJECT(queries,9);REJECT(recorder_sequence,19);
    w=good();w.begin.tsc=0;assert(!irq_latency_interior_valid(&w));
    w=good();w.begin.segment=w.end.segment=UINT64_MAX;assert(!irq_latency_interior_valid(&w));
    uint64_t hz=0,ns=0;
    const uint64_t cycles[]={50499,50500,50502};
    for(unsigned i=0;i<3;++i){assert(irq_latency_interior_minimum(cycles[i],1000000000,1000000000,&hz,&ns));
        assert(hz==1010000000 && ns==49999+i);}
    assert(irq_latency_interior_minimum(101,999999999,1000000000,&hz,&ns) && ns==100);
    hz=7;ns=9;
    assert(!irq_latency_interior_minimum(1,UINT64_MAX,1,&hz,&ns) && hz==7 && ns==9);
    assert(!irq_latency_interior_minimum(UINT64_MAX,1,1,&hz,&ns) && hz==7 && ns==9);
    assert(!irq_latency_interior_minimum(1,0,1,&hz,&ns));
    puts("IRQ latency actual-C interior continuity and downward conversion: PASS diagnostic=MINIMUM_ONLY");
}
