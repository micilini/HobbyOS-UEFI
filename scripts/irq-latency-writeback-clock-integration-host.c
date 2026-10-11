#include "irq-latency-controller-host.c"
#define switch_context hpet_unused_switch_declaration
#include "../kernel/src/timer/hpet.c"
#undef switch_context
#include "../kernel/src/cpu/mmio.c"
#include "../kernel/src/core/irq_latency_writeback_clock.h"
static unsigned mmio_reads,trace_identities;
static uint64_t mmio_delay;
static uint32_t *changing_high;
void arch_host_mmio_access_point(void)
{
    CHECK(!irq_is_enabled());++mmio_reads;clock_value+=mmio_delay;
    if (changing_high) ++*changing_high;
}
bool arch_host_cpu_local_arch_id(uint32_t *id) { ++trace_identities; *id=host_cpu; return true; }
irq_flags_t spin_lock_irqsave(spinlock_t *lock) { (void)lock; CHECK(false); return 0; }
void spin_unlock_irqrestore(spinlock_t *lock,irq_flags_t flags) { (void)lock;(void)flags;CHECK(false); }
__attribute__((constructor)) static void clock_call_integration(void)
{
    reset();g_irq_mask_segment[0]=1;
    for(unsigned c=0;c<2;++c){g_cpus[c].slot=c;g_cpus[c].apic_id=c;}
    static uint32_t registers[128];registers[HPET_REG_MAIN_COUNTER/4]=100;
    g_hpet_base=(uintptr_t)registers;g_clk_period_fs=10000000;g_hpet_counter_is_64bit=1;
    irq_latency_recorder_t *recorder = &g_irq_latency[0];
    CHECK(irq_latency_record_begin(recorder, IRQ_LATENCY_IF, 10, 1, 1, IRQ_LATENCY_HARD_IRQ));
    irq_latency_ticket_t pending = irq_latency_record_close_calibrated(recorder, IRQ_LATENCY_IF, 2, host_stack, 1, 40);
    CHECK(pending.valid);
    (void)irq_save();
    hpet_clock_sample_t sample={0};
    CHECK(irq_latency_writeback_clock_read(0,7,4096,&sample));
    const irq_latency_writeback_clock_record_t *r=irq_latency_writeback_clock_read_record(0,0);
    CHECK(r && r->count==1 && r->invalid==1);
    CHECK(r->witness.end.recorder_sequence-r->witness.begin.recorder_sequence==12);
    CHECK(r->witness.begin.segment==r->witness.end.segment);
    CHECK(sample.valid && sample.ticks==100 && sample.ns==1000 && sample.mode==1 && !sample.retries);
    CHECK(mmio_reads==3 && trace_identities==0 && !g_irq_invalid_boundaries);
    CHECK(g_irq_nested_elided[0].mmio_reused==3);
    CHECK(recorder->pending_count==1 && !g_irq_nested_elided[0].count);
    CHECK(irq_latency_record_interrupt(recorder, clock_value+10, 0));
    CHECK(irq_latency_record_retire_owner(recorder, host_stack, 1)==1);
    CHECK(irq_latency_record_drain(recorder) && !recorder->pending_count);
    CHECK(recorder->path[0].maximum.cells[IL_CLOSE_ADJUSTMENT]==40);
    host_cpu=1;host_flags=IRQ_LATENCY_IF;g_irq_mask_segment[1]=1;
    (void)irq_save();
    CHECK(irq_latency_writeback_clock_read(0,8,4096,&sample));
    r=irq_latency_writeback_clock_read_record(1,0);
    CHECK(r && r->count==1 && !r->invalid);
    CHECK(r->witness.end.recorder_sequence==r->witness.begin.recorder_sequence);
    CHECK(r->witness.begin.segment==r->witness.end.segment && g_irq_nested_elided[1].count==3);
    CHECK(sample.valid && sample.ticks==100 && sample.ns==1000 && sample.mode==1 && !sample.retries);
    CHECK(mmio_reads==6 && trace_identities==0 && !g_irq_invalid_boundaries);
    CHECK(g_irq_nested_elided[1].mmio_reused==3);
    uint64_t counter_end=0,begin=irq_latency_read_tsc();
    CHECK(hpet_read_clock_sample_attributed(&sample,&counter_end));
    uint64_t baseline=counter_end-begin;
    unsigned reads_before=mmio_reads;
    begin=irq_latency_read_tsc();mmio_delay=500000;
    CHECK(hpet_read_clock_sample_attributed(&sample,&counter_end));
    uint64_t end=irq_latency_read_tsc();mmio_delay=0;
    CHECK(mmio_reads==reads_before+3 && counter_end-begin==baseline+1500000 && end-counter_end==10);
    CHECK(sample.valid && sample.ticks==100 && sample.ns==1000 && sample.mode==1 && !sample.retries);
    CHECK(hpet_read_clock_sample(&sample) && sample.ns==1000);
    CHECK(hpet_read_clock_sample_attributed(&sample,NULL) && sample.ns==1000);
    counter_end=123;CHECK(!hpet_read_clock_sample_attributed(NULL,&counter_end) && !counter_end);
    g_hpet_base=0;counter_end=123;
    CHECK(!hpet_read_clock_sample_attributed(&sample,&counter_end) && !counter_end && !sample.valid);
    g_hpet_base=(uintptr_t)registers;changing_high=&registers[HPET_REG_MAIN_COUNTER/4+1];
    reads_before=mmio_reads;counter_end=123;
    CHECK(!hpet_read_clock_sample_attributed(&sample,&counter_end) && !counter_end && !sample.valid);
    CHECK(mmio_reads==reads_before+3*(HPET_COUNTER_SPLIT_RETRY_LIMIT+1) && sample.retries==HPET_COUNTER_SPLIT_RETRY_LIMIT+1);
    changing_high=NULL;
    puts("IRQ latency actual-C HPET/MMIO nesting: PASS legacy_calls=2 legacy_mmio_reads=6 pending_delta=12 empty_delta=0 attributed=CHECKED minimum_only=1 hardware=HOSTED");
}
