#include "irq_latency_pairs.h"
#include <stddef.h>

static uint32_t point_reason(const irq_latency_reference_point_t *p, uint32_t cpus)
{
    uint32_t reason=0;
    if (!p->acquired || !p->sample_valid || !p->initialized) reason |= IRQ_POINT_ACQUISITION;
    if (!p->interrupts_enabled || !p->if_after) reason |= IRQ_POINT_CONTEXT;
    if (!p->identity_valid || p->cpu >= cpus || p->cpu != p->end_cpu ||
        p->switch_sequence != p->switch_after) reason |= IRQ_POINT_IDENTITY;
    if (!((p->mode==1 && p->counter_bits==64 && p->retries<=4) ||
          (p->mode==2 && p->counter_bits==32 && !p->retries))) reason |= IRQ_POINT_COUNTER;
    if (!p->tsc_before || p->tsc_after <= p->tsc_before) reason |= IRQ_POINT_ORDER;
    return reason;
}

void irq_latency_pair_window(const irq_latency_point_ref_t *refs,
    const irq_latency_pair_t *pair, irq_latency_calibration_window_t *window)
{
    *window=(irq_latency_calibration_window_t){.begin=*refs[pair->begin].point,
        .end=*refs[pair->end].point,.period_fs=refs[pair->begin].point->period_fs};
}

static bool precedes(const irq_latency_point_ref_t *refs, uint32_t a, uint32_t b)
{
    const irq_latency_reference_point_t *x=refs[a].point, *y=refs[b].point;
    if (x->cpu != y->cpu) return x->cpu < y->cpu;
    if (x->tsc_before != y->tsc_before) return x->tsc_before < y->tsc_before;
    return a < b;
}

bool irq_latency_pairs_build(const irq_latency_point_ref_t *refs, uint32_t count,
    uint32_t cpus, uint32_t epoch, uint32_t role, uint32_t *order,
    irq_latency_point_disposition_t *disposition, irq_latency_pair_t *pairs,
    uint32_t capacity, uint32_t *pair_count)
{
    if (!pair_count) return false;
    *pair_count=0;
    if (!refs || !order || !disposition || !pairs || !count || !cpus || !epoch || role>1)
        return false;
    uint32_t used=0, total=0, mode=0;
    uint64_t period=0, target=0;
    for (uint32_t i=0;i<count;++i) {
        const irq_latency_point_ref_t *r=&refs[i];
        if (!r->point || !r->point->acquired || r->id!=i || r->complete!=1 || r->epoch!=epoch || r->role!=role)
            return false;
        const irq_latency_reference_point_t *p=r->point;
        uint32_t reason=point_reason(p,cpus);
        disposition[i]=(irq_latency_point_disposition_t){IRQ_POINT_INELIGIBLE,reason,UINT32_MAX};
        if (p->valid != (reason==0)) return false;
        if (!reason) {
            if (!p->period_fs || p->period_fs>UINT64_C(100000000) ||
                p->period_fs!=p->period_after) return false;
            if (period && (period!=p->period_fs || mode!=p->mode)) return false;
            period=p->period_fs; mode=p->mode;
            uint32_t j=used++;
            while (j && precedes(refs,i,order[j-1])) { order[j]=order[j-1]; --j; }
            order[j]=i;
        }
    }
    if (!used) return true;
    if (!irq_latency_muldiv(IRQ_LATENCY_PAIR_TARGET_NS,UINT64_C(1000000),period,true,&target))
        return false;
    for (uint32_t i=1;i<used;++i) {
        const irq_latency_reference_point_t *a=refs[order[i-1]].point, *b=refs[order[i]].point;
        if (a->cpu==b->cpu && (b->tsc_before<=a->tsc_after || b->ticks<a->ticks)) return false;
    }
    for (uint32_t i=0;i<used;) {
        const irq_latency_reference_point_t *a=refs[order[i]].point;
        uint32_t end=i+1;
        while (end<used && refs[order[end]].point->cpu==a->cpu &&
               refs[order[end]].point->ticks-a->ticks<target) ++end;
        if (end==used || refs[order[end]].point->cpu!=a->cpu) {
            while (i<end) disposition[order[i++]].kind=IRQ_POINT_TAIL;
            continue;
        }
        if (total==capacity) return false;
        irq_latency_pair_t pair={order[i],order[end],a->cpu,0};
        irq_latency_calibration_window_t window;
        irq_latency_frequency_interval_t interval;
        irq_latency_pair_window(refs,&pair,&window);
        pair.valid=irq_latency_calibration_interval(&window,&interval);
        pairs[total]=pair;
        for (uint32_t j=i;j<=end;++j)
            disposition[order[j]]=(irq_latency_point_disposition_t){
                j==i ? IRQ_POINT_BEGIN : j==end ? IRQ_POINT_END : IRQ_POINT_INTERIOR,0,total};
        ++total; i=end+1;
    }
    *pair_count=total;
    return true;
}
