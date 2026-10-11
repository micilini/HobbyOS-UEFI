#include "../kernel/src/core/irq_latency_writeback_clock.h"
#include "../kernel/src/core/irq_latency_tsc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned calls, mutation;
static uint32_t sample_cpu=2;
static uint64_t clock_value=100;
uint64_t irq_latency_host_read_tsc(void) { clock_value+=10; return clock_value; }
irq_latency_writeback_token_t irq_latency_writeback_begin(uint64_t generation,uint64_t root)
{
    irq_latency_writeback_token_t t={.request_generation=generation,.root=root,.active=true};
    t.begin=(irq_latency_interior_point_t){.cpu=sample_cpu,.epoch=1,.segment=3,.switches=4,
        .entries=5,.faults=6,.owner=7,.generation=8,.queries=10,.recorder_sequence=20};
    return t;
}
irq_latency_writeback_witness_t irq_latency_writeback_points(irq_latency_writeback_token_t t,
    const irq_latency_writeback_stamps_t *s)
{
    irq_latency_writeback_witness_t w={.begin=t.begin,.end=t.begin,
        .request_generation=t.request_generation,.root=t.root};
    w.begin.tsc=s->begin;w.end.tsc=s->end;w.begin.flags=s->begin_flags;w.end.flags=s->end_flags;
    ++w.end.queries;
    if(mutation==1)++w.end.segment;
    if(mutation==2)++w.end.cpu;
    if(mutation==3)++w.end.recorder_sequence;
    return w;
}
bool hpet_read_clock_sample(hpet_clock_sample_t *out)
{
    ++calls;clock_value+=200000;
    *out=(hpet_clock_sample_t){.ticks=100,.ns=1000,.mode=HPET_COUNTER_READ_SPLIT64_STABLE,.valid=1};
    if(mutation==4){out->valid=0;return false;}
    if(mutation==5)out->retries=5;
    return true;
}

int main(void)
{
    for(unsigned site=0;site<3;++site){
        hpet_clock_sample_t out={0};
        assert(irq_latency_writeback_clock_read(site,7,4096,&out));
        assert(calls==site+1 && out.ticks==100 && out.ns==1000 && out.valid);
        const irq_latency_writeback_clock_record_t *r=irq_latency_writeback_clock_read_record(2,site);
        assert(r && r->count==1 && !r->invalid && r->result && r->witness.end.tsc>r->witness.begin.tsc);
    }
    hpet_clock_sample_t out;
    assert(irq_latency_writeback_clock_read(0,8,4096,&out));
    const irq_latency_writeback_clock_record_t *r=irq_latency_writeback_clock_read_record(2,0);
    assert(r->count==2 && r->invalid==1 && r->witness.request_generation==7);
    irq_latency_writeback_clock_record_t local={0};
    irq_latency_writeback_token_t t=irq_latency_writeback_begin(7,4096);
    irq_latency_writeback_stamps_t s={100,201000,2,2};
    hpet_clock_sample_t sample={.ticks=100,.ns=1000,.mode=1,.valid=1};
    for(mutation=0;mutation<6;++mutation){
        memset(&local,0,sizeof(local));irq_latency_writeback_witness_t w=irq_latency_writeback_points(t,&s);
        sample.valid=mutation!=4;sample.retries=mutation==5?5:0;
        irq_latency_writeback_clock_record(&local,&w,&sample,mutation!=4);
        assert(local.count==1 && local.invalid==(mutation!=0));
        assert(local.witness.begin.tsc==100 && local.sample.valid==sample.valid && local.result==(mutation!=4));
    }
    local.count=UINT64_MAX;irq_latency_writeback_clock_record(&local,&local.witness,&sample,true);
    assert(local.count==UINT64_MAX && local.invalid);
    assert(!irq_latency_writeback_clock_read_record(2,3));
    for(mutation=1;mutation<6;++mutation){
        sample_cpu=3+mutation;unsigned old_calls=calls;
        bool result=irq_latency_writeback_clock_read(1,7,4096,&out);
        r=irq_latency_writeback_clock_read_record(sample_cpu,1);
        assert(result==(mutation!=4) && calls==old_calls+1 && out.valid==(mutation!=4));
        assert(r->count==1 && r->invalid==1 && r->sample.ticks==100 && r->result==result);
    }
    puts("IRQ latency actual-C writeback clock calls and bounded raw outcomes: PASS hardware=HOSTED minimum_only=1");
}
