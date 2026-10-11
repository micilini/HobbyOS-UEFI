/* Test-only adapter: runs the actual pair builder on supplied scalar records. */
#include "../kernel/src/core/irq_latency_pairs.h"
#include <inttypes.h>
#include <stdio.h>
#define LIMIT 8192u
static irq_latency_reference_point_t points[LIMIT];
static irq_latency_point_ref_t refs[LIMIT];
static irq_latency_point_disposition_t disposition[LIMIT];
static irq_latency_pair_t pairs[LIMIT/2];
static uint32_t order[LIMIT];
int main(void)
{
    unsigned n,cpus;
    if (scanf("%u %u",&n,&cpus)!=2 || !n || n>LIMIT) return 2;
    for (unsigned i=0;i<n;++i) {
        uint64_t v[21];
        for (unsigned j=0;j<21;++j) if (scanf("%" SCNu64,&v[j])!=1) return 2;
        points[i]=(irq_latency_reference_point_t){.tsc_before=v[0],.ticks=v[1],.tsc_after=v[2],
            .switch_sequence=v[3],.switch_after=v[4],.cpu=v[5],.end_cpu=v[6],.mode=v[7],
            .counter_bits=v[8],.retries=v[9],.valid=v[10],.initialized=v[11],.interrupts_enabled=v[12],
            .period_fs=v[13],.period_after=v[14],.acquired=v[15],.sample_valid=v[16],
            .identity_valid=v[17],.if_after=v[18]};
        refs[i]=(irq_latency_point_ref_t){.point=&points[i],.id=i,.epoch=v[19],.role=0,.complete=v[20]};
    }
    uint32_t count=123;
    bool valid=irq_latency_pairs_build(refs,n,cpus,1,0,order,disposition,pairs,LIMIT/2,&count);
    printf("%u %u\n",valid,count);
    if (!valid) return count ? 3 : 0;
    for (unsigned i=0;i<count;++i) printf("P %u %u %u %u\n",pairs[i].begin,pairs[i].end,pairs[i].cpu,pairs[i].valid);
    for (unsigned i=0;i<n;++i) printf("D %u %u %u\n",disposition[i].kind,disposition[i].reason,disposition[i].pair);
    return 0;
}
