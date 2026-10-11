#define IRQ_CLOCKSOURCE_INTEGRATION 1
#define main inherited_controller_main
#include "irq-latency-controller-host.c"
#undef main
#include "../kernel/src/core/clock.c"
#include "../kernel/src/core/clocksource.c"
#include "../kernel/src/core/pvclock.c"

static uint64_t sample_ns, sample_delay;
static bool sample_ok;
static unsigned sample_calls, masked_samples, enabled_samples, sample_action;
bool hpet_is_available(void) { return true; }
uint64_t hpet_frequency_hz(void) { return 100000000; }
uint64_t hpet_period_fs(void) { return 10000000; }
bool hpet_read_clock_sample(hpet_clock_sample_t *sample)
{
    ++sample_calls;
    if (irq_is_enabled()) ++enabled_samples; else ++masked_samples;
    clock_value+=sample_delay;
    *sample=(hpet_clock_sample_t){.ticks=sample_ns/10,.ns=sample_ns,.valid=sample_ok,
        .mode=HPET_COUNTER_READ_SPLIT64_STABLE};
    bool ok=sample_ok;
    unsigned action=sample_action;sample_action=0;
    if (action==1) {
        CHECK(irq_is_enabled());
        sample_ns=200;sample_ok=true;
        CHECK(clock_monotonic_ns()==200);
    } else if (action==2) {
        CHECK(irq_is_enabled());
        irq_latency_context_switch();host_cpu=1;sample_ns=200;
    } else if (action==3) {
        CHECK(irq_is_enabled());host_flags=IRQ_LATENCY_IF | 2;
    }
    return ok;
}
bool hpet_read_clock_sample_attributed(hpet_clock_sample_t *sample, uint64_t *counter_end)
{
    if (counter_end) *counter_end=0;
    bool ok=hpet_read_clock_sample(sample);
    if (ok && counter_end) *counter_end=irq_latency_read_tsc();
    return ok;
}
bool smp_current_cpu_slot(cpu_slot_t *slot) { *slot=host_cpu;return host_cpu<g_cpu_count; }
uint32_t lapic_get_id(void) { return host_cpu; }
void spinlock_init(spinlock_t *lock) { memset(lock,0,sizeof(*lock)); }
int spin_trylock(spinlock_t *lock) { (void)lock;return 0; }
void spin_unlock(spinlock_t *lock) { (void)lock; }
__attribute__((noreturn)) void kpanic(const char *message) { fprintf(stderr,"panic: %s\n",message);exit(2); }

static void clock_reset(void)
{
    reset();memset(g_irq_clock_body,0,sizeof(g_irq_clock_body));memset(g_irq_frozen,0,sizeof(g_irq_frozen));
    g_irq_mask_segment[0]=g_irq_mask_segment[1]=1;
    g_irq_scopes[host_stack].paths=UINT64_C(1)<<IRQ_PATH_SHOOTDOWN;
    sample_ns=100;sample_delay=0;sample_ok=true;CHECK(clock_monotonic_init());
    sample_calls=masked_samples=enabled_samples=sample_action=0;
}

int main(void)
{
    uint64_t refused=1234;
    clock_reset();CHECK(clock_monotonic_ns()==100 && irq_is_enabled());
    CHECK(g_irq_clock_body[0].count==2 && !g_irq_clock_body[0].invalid);
    uint64_t baseline=g_irq_clock_body[0].maximum_cycles;
    clock_reset();sample_delay=500000;CHECK(clock_monotonic_ns()==100 && irq_is_enabled());
    CHECK(sample_calls==1 && enabled_samples==1 && !masked_samples);
    CHECK(g_irq_clock_body[0].maximum_cycles==baseline);
    CHECK(g_irq_clock_body[0].maximum.incoming_flags==IRQ_LATENCY_IF);
    CHECK(irq_latency_record_drain(&g_irq_latency[0]));
    CHECK(g_irq_latency[0].path[IRQ_PATH_SHOOTDOWN].maximum_cycles<500000);
    CHECK(clock_monotonic_ns()==100 && !g_clock_state.stats.source_local_regressions);
    sample_ns=90;CHECK(!clock_monotonic_read_ns(&refused) && refused==1234);
    CHECK(g_clock_state.stats.source_local_regressions==1);
    host_cpu=1;CHECK(clock_monotonic_ns()==100);
    CHECK(g_clock_state.stats.cross_cpu_lag_clamps==1 && g_clock_state.stats.source_local_regressions==1);
    sample_ok=false;CHECK(!clock_monotonic_read_ns(&refused) && refused==1234 && g_clock_state.stats.saturations==1);
    clock_reset();irq_flags_t saved=irq_save();sample_delay=500000;
    CHECK(clock_monotonic_ns()==100 && !irq_is_enabled());
    CHECK(sample_calls==1 && masked_samples==1 && !enabled_samples);
    CHECK(!g_irq_clock_body[0].maximum.incoming_flags && g_irq_clock_body[0].maximum.phase==3);
    CHECK(g_irq_clock_body[0].maximum_cycles>=500000);irq_restore(saved);
    CHECK(irq_latency_record_drain(&g_irq_latency[0]));
    CHECK(g_irq_latency[0].path[IRQ_PATH_SHOOTDOWN].maximum_cycles>=500000);

    clock_reset();CHECK(clock_monotonic_ns()==100);sample_ns=150;sample_action=1;
    CHECK(clock_monotonic_ns()==200 && irq_is_enabled());
    CHECK(sample_calls==4 && masked_samples==1 && enabled_samples==3);
    CHECK(!g_clock_state.stats.source_local_regressions && !g_clock_state.stats.saturations);
    CHECK(g_clock_state.cpu[0].last_raw_ns==200 && g_clock_state.cpu[0].read_revision==3);
    CHECK(!g_irq_clock_body[0].invalid && !g_irq_clock_body[0].pending);

    clock_reset();CHECK(clock_monotonic_ns()==100);sample_ns=150;sample_action=2;
    CHECK(clock_monotonic_ns()==200 && irq_is_enabled());
    CHECK(sample_calls==3 && masked_samples==1 && enabled_samples==2);
    CHECK(g_clock_state.cpu[0].last_raw_ns==100 && g_clock_state.cpu[0].read_revision==1);
    CHECK(g_clock_state.cpu[1].last_raw_ns==200 && g_clock_state.cpu[1].read_revision==1);
    CHECK(!g_clock_state.stats.source_local_regressions && !g_irq_clock_body[0].pending && !g_irq_clock_body[1].pending);

    clock_reset();CHECK(clock_monotonic_ns()==100);sample_ns=90;sample_action=1;
    CHECK(!clock_monotonic_read_ns(&refused) && refused==1234 && sample_calls==3 && !masked_samples);
    CHECK(g_clock_state.stats.source_local_regressions==1 && g_clock_state.stats.max_local_backward_ns==10);
    CHECK(g_clock_state.cpu[0].last_raw_ns==200);
    clock_reset();CHECK(clock_monotonic_ns()==100);sample_ok=false;sample_action=1;
    CHECK(!clock_monotonic_read_ns(&refused) && refused==1234 && sample_calls==3 && !masked_samples);
    CHECK(g_clock_state.stats.saturations==1 && g_clock_state.cpu[0].last_raw_ns==200);

    clock_reset();g_clock_state.cpu[0].read_revision=UINT64_MAX;
    CHECK(clock_monotonic_ns()==100 && irq_is_enabled());
    CHECK(sample_calls==1 && masked_samples==1 && g_clock_state.cpu[0].read_revision==UINT64_MAX);
    clock_reset();g_clock_state.ready=0;
    CHECK(clock_monotonic_ns()==0 && irq_is_enabled() && !sample_calls && g_clock_state.stats.not_ready==1);
    clock_reset();host_flags=IRQ_LATENCY_IF | 0x46;sample_action=3;
    CHECK(clock_monotonic_ns()==100 && host_flags==(IRQ_LATENCY_IF | 0x46));
    uint64_t checked=1234;
    clock_reset();CHECK(clock_monotonic_read_ns(&checked) && checked==100 && irq_is_enabled());
    CHECK(!clock_monotonic_read_ns(NULL));
    sample_ok=false;checked=1234;
    CHECK(!clock_monotonic_read_ns(&checked) && checked==1234 && irq_is_enabled());
    sample_ok=true;sample_ns=90;
    CHECK(!clock_monotonic_read_ns(&checked) && checked==1234 && g_clock_state.stats.source_local_regressions==1);
    host_cpu=1;
    CHECK(clock_monotonic_read_ns(&checked) && checked==100 && g_clock_state.stats.cross_cpu_lag_clamps==1);
    clock_reset();g_clock_state.ready=0;checked=1234;
    CHECK(!clock_monotonic_read_ns(&checked) && checked==1234 && !sample_calls && irq_is_enabled());
    clock_reset();sample_ns=UINT64_MAX;checked=1234;
    CHECK(!clock_monotonic_read_ns(&checked) && checked==1234 && irq_is_enabled());
    clock_reset();CHECK(clock_monotonic_read_ns(&checked));sample_ns=90;sample_action=1;checked=1234;
    CHECK(!clock_monotonic_read_ns(&checked) && checked==1234 && g_clock_state.stats.source_local_regressions==1);
    clock_reset();CHECK(clock_monotonic_read_ns(&checked));sample_ok=false;sample_action=2;checked=1234;
    CHECK(!clock_monotonic_read_ns(&checked) && checked==1234 && g_clock_state.stats.saturations==1);
    clock_reset();irq_flags_t checked_flags=irq_save();checked=1234;
    CHECK(clock_monotonic_read_ns(&checked) && checked==100 && !irq_is_enabled());irq_restore(checked_flags);
    for (unsigned mutation=0;mutation<9;++mutation) {
        clock_reset();(void)irq_save();
        irq_latency_clock_token_t token=irq_latency_clock_prepare(IRQ_LATENCY_IF,0xffffffff80001000);
        CHECK(token.active);token.begin.tsc=irq_latency_host_read_tsc();
        if(mutation==0)++g_irq_mask_segment[0];
        if(mutation==1)++owner_generation;
        if(mutation==2)++g_irq_entry_sequence[0];
        if(mutation==3)++g_irq_switch_sequence[0];
        if(mutation==4)++g_irq_interior_fault_sequence;
        if(mutation==5)++g_irq_latency[0].epoch;
        if(mutation==6)host_flags=IRQ_LATENCY_IF;
        if(mutation==7){irq_latency_context_switch();host_cpu=1;}
        if(mutation==8)token.begin.tsc=UINT64_MAX;
        irq_latency_clock_complete(token,irq_latency_host_read_tsc());
        CHECK(g_irq_clock_body[0].invalid==1 && !g_irq_clock_body[0].count);
    }
    clock_reset();(void)irq_save();
    irq_latency_clock_token_t token=irq_latency_clock_prepare(IRQ_LATENCY_IF,0xffffffff80001000);
    CHECK(token.active);token.begin.tsc=irq_latency_host_read_tsc();
    CHECK(!irq_latency_clock_prepare(IRQ_LATENCY_IF,token.site).active);
    CHECK(g_irq_clock_body[0].invalid==1 && g_irq_clock_body[0].pending==token.sequence);
    irq_latency_clock_complete(token,irq_latency_host_read_tsc());
    CHECK(g_irq_clock_body[0].count==1 && !g_irq_clock_body[0].pending && g_irq_clock_body[0].retired==1);
    uint64_t maximum=g_irq_clock_body[0].maximum_cycles;
    irq_latency_clock_complete(token,irq_latency_host_read_tsc());
    CHECK(g_irq_clock_body[0].invalid==2 && g_irq_clock_body[0].count==1 && g_irq_clock_body[0].maximum_cycles==maximum);
    clock_reset();(void)irq_save();
    token=irq_latency_clock_prepare(IRQ_LATENCY_IF,0xffffffff80001000);
    g_irq_frozen[0]=true;
    CHECK(irq_latency_clock_read(0)->pending==token.sequence && !irq_latency_clock_read(0)->retired);
    clock_reset();(void)irq_save();g_irq_clock_body[0].started=UINT64_MAX-1;
    CHECK(!irq_latency_clock_prepare(IRQ_LATENCY_IF,0xffffffff80001000).active && g_irq_clock_body[0].invalid==1);
    clock_reset();memset(g_irq_tick_body,0,sizeof(g_irq_tick_body));host_flags=0;
    CHECK(irq_latency_interrupt_enter(clock_value,INT_VECTOR_LAPIC_TIMER,IRQ_LATENCY_IF,0));
    irq_latency_tick_token_t tick=irq_latency_tick_prepare();CHECK(tick.active);
    tick.begin.tsc=irq_latency_read_tsc();sample_delay=500000;
    CHECK(clock_monotonic_ns()==100 && !irq_is_enabled());
    tick.after_clock=irq_latency_read_tsc();
    uint64_t first=g_irq_tick_sample[0].begin,last=g_irq_tick_sample[0].end;
    CHECK(first>tick.begin.tsc && last-first==500020 && last<tick.after_clock);
    CHECK(g_irq_tick_sample[0].counter_end-first==500010 && last-g_irq_tick_sample[0].counter_end==10);
    sample_delay=700000;CHECK(clock_monotonic_ns()==100);
    CHECK(g_irq_tick_sample[0].begin==first && g_irq_tick_sample[0].end==last);
    CHECK(!irq_latency_tick_clock_window(UINT32_MAX));
    tick.after_work=irq_latency_read_tsc();CHECK(irq_latency_record_tag(&g_irq_latency[0],1));
    irq_latency_tick_complete(&tick,irq_latency_read_tsc());
    CHECK(g_irq_tick_body[0].count==1 && !g_irq_tick_body[0].invalid && sample_calls==2);
    CHECK(g_irq_tick_body[0].maximum.sample_end-g_irq_tick_body[0].maximum.sample_begin==500020);
    puts("IRQ latency actual clock/controller: PASS stalled=RETAINED regression=CLASSIFIED mask=EXACT minimum_only=1");
    return 0;
}
