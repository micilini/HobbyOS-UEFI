#include "../kernel/src/core/irq_latency_pairs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define N 32
static irq_latency_reference_point_t points[N];
static irq_latency_point_ref_t refs[N];
static irq_latency_pair_t pairs[N/2];
static irq_latency_point_disposition_t dispositions[N];
static uint32_t order[N];
static uint32_t count;
static void reset(void)
{
    memset(points, 0, sizeof(points));
    for (unsigned i=0;i<N;++i) {
        unsigned cpu=i%2, local=i/2;
        uint64_t ticks=1000+local*100000;
        uint64_t t=1000000000000ULL*cpu+ticks*240;
        points[i]=(irq_latency_reference_point_t){.tsc_before=t,.tsc_after=t+100,
            .ticks=ticks,.cpu=cpu,.end_cpu=cpu,.mode=1,.counter_bits=64,
            .valid=true,.initialized=true,.interrupts_enabled=true,.acquired=true,
            .sample_valid=true,.identity_valid=true,.if_after=true,
            .period_fs=100000000,.period_after=100000000};
        refs[i]=(irq_latency_point_ref_t){.point=&points[i],.id=i,.epoch=1,.role=0,.complete=1};
    }
}
static bool build(void)
{ return irq_latency_pairs_build(refs,N,2,1,0,order,dispositions,pairs,N/2,&count); }
int main(void)
{
    reset(); assert(build() && count==10);
    assert(pairs[0].begin==0 && pairs[0].end==4 && pairs[0].valid);
    assert(dispositions[2].kind==IRQ_POINT_INTERIOR);
    assert(dispositions[30].kind==IRQ_POINT_TAIL);
    assert(pairs[5].begin==1 && pairs[5].end==5 && pairs[5].valid);
    points[4].tsc_after+=1000000;
    assert(build() && count==10 && !pairs[0].valid && pairs[0].end==4);
    reset(); points[2].switch_after=1; points[2].valid=false;
    assert(build() && dispositions[2].kind==IRQ_POINT_INELIGIBLE);
    reset(); points[2].switch_after=1; assert(!build() && !count);
    for (unsigned i=0;i<N;i+=N/2-1) {
        reset(); refs[i].complete=0; assert(!build() && !count);
    }
    reset(); points[0].acquired=false;points[0].valid=false;assert(!build() && !count);
    reset(); refs[1].id=0; assert(!build() && !count);
    reset(); refs[1].epoch=2; assert(!build() && !count);
    reset(); refs[1].role=1; assert(!build() && !count);
    reset(); points[2].period_fs=points[2].period_after=99999999; assert(!build() && !count);
    reset(); points[2].mode=2;points[2].counter_bits=32; assert(!build() && !count);
    reset(); points[2].tsc_before=points[0].tsc_before; assert(!build() && !count);
    reset(); points[2].ticks=points[0].ticks-1; assert(!build() && !count);
    reset(); points[0].period_fs=points[0].period_after=UINT64_MAX;assert(!build() && !count);
    reset(); assert(!irq_latency_pairs_build(refs,N,2,1,0,order,dispositions,pairs,1,&count) && !count);
    reset(); assert(build());
    puts("IRQ latency actual-C deterministic CPU-local pairs: PASS clocks=HOSTED");
}
