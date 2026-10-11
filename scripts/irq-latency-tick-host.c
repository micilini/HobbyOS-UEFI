#define main inherited_controller_main
#include "irq-latency-controller-host.c"
#undef main

volatile int g_panic_in_progress;
static unsigned delayed, operation, eois, changed;
static bool tag_memory=true;
uint64_t clock_monotonic_ns(void)
{
    CHECK(operation++==0 && !irq_is_enabled());
    if (delayed==4) clock_value+=500000;
    irq_latency_tick_sample_t *window=irq_latency_tick_clock_window(host_cpu);CHECK(window);
    window->begin=irq_latency_read_tsc();
    if (delayed==1) clock_value+=500000;
    window->counter_end=irq_latency_read_tsc();
    if (delayed==6) clock_value+=500000;
    window->end=irq_latency_read_tsc();
    window->ticks=12345;window->mode=1;window->valid=true;window->source=1;window->source_epoch=1;
    CHECK(!irq_latency_tick_clock_window(host_cpu));
    if (delayed==5) clock_value+=500000;
    return 123456;
}
void irq_stats_record(uint8_t vector) { CHECK(vector==INT_VECTOR_LAPIC_TIMER); }
void lapic_timer_record_irq_at(uint64_t now) { CHECK(now==123456); }
bool smp_current_cpu_slot(cpu_slot_t *slot) { *slot=host_cpu;return true; }
cpu_slot_t smp_bsp_cpu_slot(void) { return 0; }
void timer_clockevent_on_lapic_tick(cpu_slot_t slot,uint64_t now)
{
    CHECK(slot==0 && now==123456 && operation++==1 && !irq_is_enabled());
    if (tag_memory) CHECK(irq_latency_record_tag(&g_irq_latency[0],1));
    if (delayed==2) clock_value+=500000;
    if (changed==1) ++g_irq_mask_segment[0];
    if (changed==2) ++owner_generation;
    if (changed==3) ++g_irq_entry_sequence[0];
    if (changed==4) ++g_irq_switch_sequence[0];
    if (changed==5) ++g_irq_interior_fault_sequence;
    if (changed==6) ++g_irq_latency[0].epoch;
    if (changed==7) host_flags=IRQ_LATENCY_IF;
    if (changed==8) host_cpu=1;
    if (changed==9) ++g_irq_latency[0].begin_sequence;
    if (changed==10) ++g_irq_lock_depth[0];
    if (changed==11) { irq_restore(IRQ_LATENCY_IF);(void)irq_save(); }
}
void interrupts_request_reschedule(void) { CHECK(operation++==2); }
void lapic_eoi(void)
{
    CHECK(operation++==3);++eois;
    if (delayed==3) clock_value+=500000;
}
#include IRQ_TICK_HANDLER_SOURCE

static void start(void)
{
    reset();memset(g_irq_frozen,0,sizeof(g_irq_frozen));
    memset(g_irq_tick_body,0,sizeof(g_irq_tick_body));
    g_irq_mask_segment[0]=1;
    operation=eois=delayed=changed=0;tag_memory=true;
    host_flags=0;
    CHECK(irq_latency_interrupt_enter(clock_value,INT_VECTOR_LAPIC_TIMER,IRQ_LATENCY_IF,0));
}
static irq_latency_tick_witness_t sample(unsigned delay)
{
    start();delayed=delay;
    CHECK(irq_lapic_timer_handler_inner()==123456 && operation==4 && eois==1);
    CHECK(g_irq_tick_body[0].count==1 && !g_irq_tick_body[0].invalid &&
        !g_irq_tick_body[0].pending && g_irq_tick_body[0].retired==1);
    return g_irq_tick_body[0].maximum;
}
int main(void)
{
    irq_latency_tick_witness_t base=sample(0);
    CHECK(base.end.tsc-base.begin.tsc==60);
    irq_latency_tick_witness_t fast=base;
    fast.sample_source=2;fast.source_epoch=2;fast.sample_mode=0;
    fast.sample_ticks=fast.sample_begin+1;
    CHECK(irq_latency_tick_valid(&fast));
    fast.sample_ticks=fast.counter_end;
    CHECK(!irq_latency_tick_valid(&fast));
    for (unsigned phase=1;phase<=3;++phase) {
        irq_latency_tick_witness_t w=sample(phase);
        uint64_t v[4]={w.begin.tsc,w.after_clock,w.after_work,w.end.tsc};
        CHECK(w.end.tsc-w.begin.tsc==500060);
        for (unsigned i=1;i<4;++i) CHECK(v[i]-v[i-1]==(i==1 ? 40u : 10u)+(i==phase ? 500000u : 0u));
        CHECK(irq_latency_tick_valid(&w));
    }
    for (unsigned phase=4;phase<=5;++phase) {
        irq_latency_tick_witness_t w=sample(phase);
        CHECK(w.end.tsc-w.begin.tsc==500060 && w.sample_end-w.sample_begin==20);
        CHECK((phase==4 ? w.sample_begin-w.begin.tsc : w.after_clock-w.sample_end)==500010);
    }
    irq_latency_tick_witness_t conversion=sample(6);
    CHECK(conversion.counter_end-conversion.sample_begin==10 &&
        conversion.sample_end-conversion.counter_end==500010 &&
        conversion.end.tsc-conversion.begin.tsc==500060);
    for (unsigned bad=1;bad<=11;++bad) {
        start();changed=bad;(void)irq_lapic_timer_handler_inner();
        CHECK(!g_irq_tick_body[0].count && g_irq_tick_body[0].invalid==1);
    }
    for (unsigned bad=0;bad<19;++bad) {
        irq_latency_tick_witness_t w=base;
        if (bad==0) w.after_clock=0;
        if (bad==1) w.after_work=w.after_clock;
        if (bad==2) w.after_work=w.end.tsc;
        if (bad==3) w.outer_sequence=0;
        if (bad==4) w.sequence=UINT64_MAX;
        if (bad==5) w.paths=UINT64_MAX;
        if (bad==6) w.end.lock=1;
        if (bad==7) w.end.tsc=0;
        if (bad==8) w.counter_end=0;
        if (bad==9) w.counter_end=w.sample_end;
        if (bad==10) w.sample_valid=0;
        if (bad==11) w.sample_ticks=0;
        if (bad==12) w.sample_mode=3;
        if (bad==13) w.sample_retries=5;
        if (bad==14) { w.sample_mode=2;w.sample_retries=1; }
        if (bad==15) w.sample_source=0;
        if (bad==16) w.source_epoch=0;
        if (bad==17) w.sample_source=2;
        if (bad==18) w.source_epoch=2;
        CHECK(!irq_latency_tick_valid(&w));
    }
    start();tag_memory=false;(void)irq_lapic_timer_handler_inner();
    CHECK(!g_irq_tick_body[0].count && g_irq_tick_body[0].without_memory==1 && !g_irq_tick_body[0].maximum_cycles);
    start();irq_latency_tick_token_t t=irq_latency_tick_prepare();CHECK(t.active);
    CHECK(!irq_latency_tick_prepare().active && g_irq_tick_body[0].pending==t.sequence);
    t.begin.tsc=irq_latency_read_tsc();irq_latency_tick_sample_t *window=irq_latency_tick_clock_window(0);CHECK(window);
    window->begin=irq_latency_read_tsc();window->counter_end=irq_latency_read_tsc();window->end=irq_latency_read_tsc();
    window->ticks=1;window->mode=1;window->valid=true;window->source=1;window->source_epoch=1;
    t.after_clock=irq_latency_read_tsc();t.after_work=irq_latency_read_tsc();
    CHECK(irq_latency_record_tag(&g_irq_latency[0],1));
    irq_latency_tick_complete(&t,irq_latency_read_tsc());uint64_t maximum=g_irq_tick_body[0].maximum_cycles;
    irq_latency_tick_complete(&t,irq_latency_read_tsc());
    CHECK(g_irq_tick_body[0].invalid==2 && g_irq_tick_body[0].maximum_cycles==maximum);
    start();g_irq_tick_body[0].started=UINT64_MAX-1;CHECK(!irq_latency_tick_prepare().active);
    CHECK(g_irq_tick_body[0].invalid==1);
    start();t=irq_latency_tick_prepare();g_irq_frozen[0]=true;
    CHECK(irq_latency_tick_read(0)->pending==t.sequence && !irq_latency_tick_read(0)->retired);
    puts("LAPIC actual handler/controller: PASS phases=SAME_EVENT continuity=CHECKED EOI=ONCE minimum_only=1");
    return 0;
}
