#include "irq_latency_writeback_clock.h"
#include "irq_latency_tsc.h"
#include "irq_latency.h"
#include "../smp/smp_topology.h"

static irq_latency_writeback_clock_record_t g_writeback_clocks[HOBBYOS_MAX_CPUS][IRQ_WRITEBACK_CLOCK_SITES];

void irq_latency_writeback_clock_record(irq_latency_writeback_clock_record_t *r,
    const irq_latency_writeback_witness_t *w, const hpet_clock_sample_t *sample, bool result)
{
    if (!r) return;
    if (r->count) {
        if (r->count != UINT64_MAX) ++r->count;
        if (r->invalid != UINT64_MAX) ++r->invalid;
        return;
    }
    r->count=1;
    if (w) r->witness=*w;
    if (sample) r->sample=*sample;
    r->result=result;
    bool valid=irq_latency_writeback_valid(w) && result && sample && sample->valid==1 &&
        ((sample->mode==HPET_COUNTER_READ_SPLIT64_STABLE && sample->retries<=HPET_COUNTER_SPLIT_RETRY_LIMIT) ||
         (sample->mode==HPET_COUNTER_READ_EXTENDED32 && !sample->retries));
    r->invalid=!valid;
}

static uint64_t flags_now(void)
{
#ifdef IRQ_LATENCY_HOSTED_WRITEBACK_CLOCK
#if !__STDC_HOSTED__
#error IRQ_LATENCY_HOSTED_WRITEBACK_CLOCK requires hosted tests
#endif
    return 2;
#else
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags) :: "memory");
    return flags;
#endif
}

bool irq_latency_writeback_clock_read(unsigned site,uint64_t generation,uint64_t root,
    hpet_clock_sample_t *sample)
{
    irq_latency_writeback_token_t t=irq_latency_writeback_begin(generation,root);
    irq_latency_writeback_stamps_t stamps;
    stamps.begin_flags=flags_now();
    stamps.begin=irq_latency_read_tsc();
    bool result=hpet_read_clock_sample(sample);
    stamps.end=irq_latency_read_tsc();
    stamps.end_flags=flags_now();
    if (t.active && t.begin.cpu<HOBBYOS_MAX_CPUS && site<IRQ_WRITEBACK_CLOCK_SITES) {
        irq_latency_writeback_witness_t w=irq_latency_writeback_points(t,&stamps);
        irq_latency_writeback_clock_record(&g_writeback_clocks[t.begin.cpu][site],&w,sample,result);
    }
    return result;
}

const irq_latency_writeback_clock_record_t *irq_latency_writeback_clock_read_record(unsigned cpu,unsigned site)
{
    return cpu<HOBBYOS_MAX_CPUS && site<IRQ_WRITEBACK_CLOCK_SITES ? &g_writeback_clocks[cpu][site] : 0;
}
