#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/src/core/irq_latency_boundary.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"controller check failed line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
SmpCpuInfo g_cpus[HOBBYOS_MAX_CPUS];
uint32_t g_cpu_count = 2;
char bsp_stack_bottom[1], bsp_stack_top[1];
static uint32_t host_cpu, host_stack = STACK_CPU_SLOTS;
static uint64_t host_flags, clock_value, owner_generation, identity_queries;

#ifndef IRQ_CLOCKSOURCE_INTEGRATION
bool clocksource_terminal_capture(cpu_slot_t slot)
{ CHECK(slot==host_cpu && !irq_is_enabled());return true; }
#endif

bool irq_latency_host_slot(uint32_t *slot);
uint32_t irq_latency_host_stack(uint64_t rsp);
uint64_t irq_latency_host_read_tsc(void);
bool irq_latency_host_slot(uint32_t *slot) { ++identity_queries; *slot=host_cpu; return host_cpu<g_cpu_count; }
uint32_t irq_latency_host_stack(uint64_t rsp) { (void)rsp; return host_stack; }
uint64_t irq_latency_host_read_tsc(void) { clock_value+=10; return clock_value; }
bool irq_is_enabled(void) {
#ifdef IRQ_LATENCY_HOST_IF_QUERY
    IRQ_LATENCY_HOST_IF_QUERY();
#endif
    return (host_flags & IRQ_LATENCY_IF)!=0;
}
bool kernel_stack_read(uint32_t slot, stack_owner_view_t *owner)
{
    if (slot!=host_stack || !owner_generation) return false;
    memset(owner,0,sizeof(*owner));owner->generation=owner_generation;
    owner->base=1;owner->end=UINT64_MAX;
    return true;
}

irq_flags_t irq_save(void)
{
    irq_latency_boundary_t b={.flags=host_flags,.observed_flags=host_flags,.site=0xffffffff80001000};
    bool ready=irq_latency_before_mask(&b);b.tsc=irq_latency_host_read_tsc();host_flags&=~IRQ_LATENCY_IF;
    if(ready)irq_latency_after_mask(&b);
    return b.flags;
}
void irq_restore(irq_flags_t flags)
{
    irq_latency_boundary_t b={.flags=flags,.observed_flags=host_flags,.site=0xffffffff80002000};
    bool ready=irq_latency_before_restore(&b);host_flags=flags;b.tsc=irq_latency_host_read_tsc();
    if(ready)irq_latency_after_restore(&b);
}

static void reset(void)
{
    memset(g_irq_latency,0,sizeof(g_irq_latency));memset(g_irq_scopes,0,sizeof(g_irq_scopes));
    memset(g_irq_scope_barriers,0,sizeof(g_irq_scope_barriers));memset(g_irq_scope_depth,0,sizeof(g_irq_scope_depth));
    memset(g_irq_switch_sequence,0,sizeof(g_irq_switch_sequence));memset(g_irq_entry_sequence,0,sizeof(g_irq_entry_sequence));
    memset(g_irq_last_exit,0,sizeof(g_irq_last_exit));memset(g_irq_last_exit_entry,0,sizeof(g_irq_last_exit_entry));
    memset(g_irq_last_exit_switch,0,sizeof(g_irq_last_exit_switch));memset(g_irq_started,0,sizeof(g_irq_started));
    memset(g_irq_lock_depth,0,sizeof(g_irq_lock_depth));memset(g_irq_invalid_sites,0,sizeof(g_irq_invalid_sites));
    memset(g_irq_mask_identity,0,sizeof(g_irq_mask_identity));memset(g_irq_mask_segment,0,sizeof(g_irq_mask_segment));
    memset(g_irq_nested_elided,0,sizeof(g_irq_nested_elided));
    memset(g_irq_frozen,0,sizeof(g_irq_frozen));g_irq_freeze_requested=0;
    memset(g_irq_probe_active,0,sizeof(g_irq_probe_active));
    memset(g_irq_probe_armed,0,sizeof(g_irq_probe_armed));
    g_irq_interior_fault_sequence=g_irq_interior_fault_overflow=0;
    host_cpu=0;host_flags=IRQ_LATENCY_IF;clock_value=100;owner_generation=1;identity_queries=0;
    g_irq_ready=g_irq_latency_identity_ready=1;g_irq_invalid_boundaries=0;
    for(unsigned i=0;i<2;++i){CHECK(irq_latency_record_init(&g_irq_latency[i],i,1,1000));g_irq_boundary_allowance[i]=40;g_cpus[i].slot=i;g_cpus[i].apic_id=i;}
}

static void calibration_after_snapshot(void)
{
    reset();CHECK(irq_latency_record_init(&g_irq_calibration_records[0],0,1,1000));
    CHECK(irq_latency_freeze_request());host_flags=0;
    CHECK(!irq_latency_interrupt_enter(120,INT_VECTOR_LAPIC_TIMER,IRQ_LATENCY_IF,0));
    CHECK(g_irq_frozen[0] && !g_irq_latency[0].pending_count);
    static irq_latency_snapshot_t saved;
    saved=g_irq_terminal[0];
    g_irq_probe_armed[0]=1;
    uint64_t entry=irq_latency_interrupt_enter(150,INT_VECTOR_SPURIOUS,IRQ_LATENCY_IF,0);
    CHECK(entry==3 && g_irq_probe_active[0] && g_irq_calibration_records[0].active);
    irq_latency_tail_t tail=irq_latency_interrupt_exit(INT_VECTOR_SPURIOUS,IRQ_LATENCY_IF,entry);
    CHECK(tail.pending && tail.probe_end && !g_irq_probe_active[0] && !g_irq_probe_armed[0]);
    CHECK(tail.pending->witness.cells[IL_CONTEXT]==IRQ_LATENCY_TEST_PROBE);
    CHECK(tail.pending->witness.cells[IL_BOUNDARY_ADJUSTMENT]==0);
    CHECK(!memcmp(&saved,&g_irq_terminal[0],sizeof(saved)) && !g_irq_latency[0].active);
    CHECK(!g_irq_invalid_boundaries && !g_irq_scope_depth[host_stack]);
}

static void nested_ticket(void)
{
    reset();irq_flags_t outer=irq_save();CHECK(!irq_is_enabled());
    CHECK(irq_latency_record_tag(&g_irq_latency[0],1));
    uint64_t begin=g_irq_latency[0].begin;
    irq_latency_lock_token_t token=irq_latency_lock_wait(0x1000);
    CHECK(token.active);uint64_t queries=identity_queries;
    clock_value+=200;irq_latency_lock_acquired(0x1000,token);
    CHECK(identity_queries==queries);CHECK(g_irq_locks[0][0].acquired>begin);
    irq_flags_t inner=irq_save();CHECK(!(inner & IRQ_LATENCY_IF));
    CHECK(g_irq_latency[0].begin==begin);irq_restore(inner);CHECK(!irq_is_enabled());
    irq_latency_lock_released(0x1000);irq_restore(outer);CHECK(irq_is_enabled());
    CHECK(irq_latency_record_drain(&g_irq_latency[0]));
    CHECK(g_irq_latency[0].path[0].count==1 && g_irq_latency[0].path[0].maximum.cells[IL_WAIT]>=200);
    CHECK(g_irq_invalid_boundaries==0);
}

static void token_invalidation(void)
{
    for(unsigned mutation=0;mutation<5;++mutation){
        reset();(void)irq_save();CHECK(irq_latency_record_tag(&g_irq_latency[0],1));
        irq_latency_lock_token_t token=irq_latency_lock_wait(0x1000);
        if(mutation==0)host_flags|=IRQ_LATENCY_IF;
        if(mutation==1)irq_latency_context_switch();
        if(mutation==2)++g_irq_entry_sequence[0];
        if(mutation==3)++g_irq_latency[0].begin_sequence;
        if(mutation==4)++token.epoch;
        irq_latency_lock_acquired(0x1000,token);
        CHECK(g_irq_invalid_sites[26]==1 && !g_irq_locks[0][0].acquired);
    }
}

static void masked_identity_reuse(void)
{
    reset();irq_flags_t outer=irq_save();
    uint64_t queries=identity_queries;
    CHECK(irq_latency_record_tag(&g_irq_latency[0],1));
    irq_latency_lock_token_t token=irq_latency_lock_wait(0x1000);
    irq_latency_lock_ticket(0x1000,0);irq_latency_lock_acquired(0x1000,token);
    irq_flags_t nested=irq_save();irq_restore(nested);
    irq_latency_lock_released(0x1000);
    CHECK(identity_queries==queries);
    irq_restore(outer);CHECK(irq_is_enabled());
    CHECK(identity_queries==queries+1); /* Actual after-restore identity, never cached. */
    uint32_t cpu;uint64_t switches;
    CHECK(irq_latency_identity_sample(&cpu,&switches));
    CHECK(identity_queries==queries+2); /* Calibration endpoints always query hardware. */
    CHECK(g_irq_invalid_boundaries==0);
}

static void masked_identity_boundaries(void)
{
    for (unsigned mutation=0;mutation<10;++mutation) {
        reset();(void)irq_save();uint64_t queries=identity_queries;
        if(mutation==0) { irq_latency_boundary_t b={.flags=IRQ_LATENCY_IF,.observed_flags=0};
            (void)irq_latency_before_restore(&b);host_flags=IRQ_LATENCY_IF;host_flags=0; }
        if(mutation==1) { irq_latency_context_switch();host_cpu=1;queries=identity_queries; }
        if(mutation==2) ++owner_generation;
        if(mutation==3) ++g_irq_entry_sequence[0];
        if(mutation==4) ++g_irq_interior_fault_sequence;
        if(mutation==5) g_irq_interior_fault_overflow=1;
        if(mutation==6) ++g_irq_latency[0].epoch;
        if(mutation==7) g_irq_mask_segment[0]=UINT64_MAX;
        if(mutation==8) host_flags=IRQ_LATENCY_IF;
        if(mutation==9) { irq_latency_stack_retired(host_stack,owner_generation); }
        uint32_t cpu;CHECK(masked_slot_now(&cpu));
        CHECK(cpu==host_cpu && identity_queries==queries+1);
        if(mutation==5 || mutation==7 || mutation==8) {
            CHECK(masked_slot_now(&cpu) && identity_queries==queries+2);
        }
    }
    reset();(void)irq_save();uint32_t cpu;uint64_t switches;
    uint64_t queries=identity_queries;
    CHECK(irq_latency_identity_sample(&cpu,&switches));
    CHECK(irq_latency_identity_sample(&cpu,&switches));
    CHECK(identity_queries==queries+2);
    uint64_t scoped=irq_latency_interrupt_enter(200,255,0,0);
    CHECK(scoped);queries=identity_queries;
    CHECK(masked_slot_now(&cpu) && identity_queries==queries);
    (void)irq_latency_interrupt_exit(255,0,scoped);
    CHECK(masked_slot_now(&cpu) && identity_queries==queries+1);
}

static uint64_t switch_saved_rsp, switch_finishes;
extern void switch_context(uint64_t *old_rsp, const uint64_t *new_rsp,
                           void *cpu_state, uint64_t expected_sequence);
void scheduler_finish_switch(void *cpu_state, uint64_t expected_sequence)
{
    CHECK(cpu_state==&switch_saved_rsp && expected_sequence==UINT64_C(0x123456789abcdef0));
    CHECK(switch_saved_rsp && !irq_is_enabled());
    /* The finish handoff can make the outgoing stack runnable on another CPU. */
    CHECK(g_irq_switch_sequence[0]==1);
    host_cpu=1;
    uint32_t cpu;uint64_t queries=identity_queries;
    CHECK(masked_slot_now(&cpu) && cpu==1 && identity_queries==queries+1);
    host_cpu=0;++switch_finishes;
}
void irq_latency_enable(void) { CHECK(false); }
void thread_exit_normal(void) { CHECK(false); }

static void switch_publication_order(void)
{
    reset();(void)irq_save();switch_saved_rsp=0;switch_finishes=0;
    /* Aliased saved-RSP cells execute the real switch/return on this hosted
     * stack. The finish substitute tests the outgoing-owner publication race. */
    switch_context(&switch_saved_rsp,&switch_saved_rsp,&switch_saved_rsp,
                   UINT64_C(0x123456789abcdef0));
    CHECK(switch_finishes==1 && g_irq_switch_sequence[0]==1);
}

static void migration_before_cli(void)
{
    reset();irq_latency_boundary_t b={.flags=IRQ_LATENCY_IF,.observed_flags=IRQ_LATENCY_IF,.site=0xffffffff80001000};
    CHECK(irq_latency_before_mask(&b));b.tsc=150;host_cpu=1;host_flags=0;
    irq_latency_after_mask(&b);CHECK(g_irq_latency[1].context==IRQ_LATENCY_UNTRACKED);
    reset();b=(irq_latency_boundary_t){.flags=IRQ_LATENCY_IF,.site=0xffffffff80001000};
    CHECK(irq_latency_before_mask(&b));b.tsc=150;host_cpu=1;host_flags=0;
    g_irq_last_exit[1]=140;g_irq_entry_sequence[1]=g_irq_last_exit_entry[1]=2;
    irq_latency_after_mask(&b);
    CHECK(g_irq_latency[1].begin==140 && g_irq_latency[1].begin_kind==IRQ_BOUNDARY_AFTER_IRQ);
    CHECK(g_irq_latency[1].observed_begin_cpu==UINT32_MAX);
}

static void interrupted_completion(bool migrate, bool retire)
{
    reset();(void)irq_save();CHECK(irq_latency_record_tag(&g_irq_latency[0],1));
    irq_latency_boundary_t b={.flags=IRQ_LATENCY_IF,.observed_flags=0,.site=0xffffffff80002000};
    CHECK(irq_latency_before_restore(&b));host_flags=IRQ_LATENCY_IF;
    uint64_t first=irq_latency_host_read_tsc();
    CHECK(irq_latency_record_interrupt(&g_irq_latency[0],first,0));
    CHECK(irq_latency_record_interrupt(&g_irq_latency[0],first+50,0));
    if(retire){
        irq_latency_stack_retired(host_stack,owner_generation+1);
        CHECK(g_irq_latency[0].pending_count==1);
        irq_latency_stack_retired(host_stack,owner_generation);
        owner_generation++;
    }else{
        b.tsc=first+100;if(migrate){host_cpu=1;++g_irq_switch_sequence[0];}
        irq_latency_after_restore(&b);
    }
    CHECK(irq_latency_record_drain(&g_irq_latency[0]));
    irq_latency_witness_t *w=&g_irq_latency[0].path[0].maximum;
    CHECK(w->cells[IL_END]==first && w->cells[IL_FALLBACK_END]==first);
    CHECK(w->cells[IL_END_KIND]==(retire?IRQ_BOUNDARY_RETIRED_PUBLISHER:IRQ_BOUNDARY_BEFORE_IRQ));
    CHECK(!g_irq_latency[0].pending_count && !g_irq_latency[0].invalid);
}

static void activation_during_interrupt(void)
{
    for (unsigned active=0;active<2;++active) {
        reset();host_flags=0;g_irq_ready=0;
        uint64_t entry=irq_latency_interrupt_enter(110,32,IRQ_LATENCY_IF,0);
        CHECK(!entry && !g_irq_latency[0].active);
        g_irq_ready=1;
        if (active) { (void)irq_save();CHECK(g_irq_latency[0].context==IRQ_LATENCY_EARLY); }
        irq_latency_tail_t tail=irq_latency_interrupt_exit(32,IRQ_LATENCY_IF,entry);
        CHECK((tail.pending!=NULL)==(active!=0));
        CHECK(!g_irq_invalid_boundaries && !g_irq_latency[0].invalid && !g_irq_latency[0].active);
        if(active)CHECK(tail.pending->witness.cells[IL_CONTEXT]==IRQ_LATENCY_EARLY);
    }
    reset();host_flags=0;
    uint64_t entry=irq_latency_interrupt_enter(110,32,IRQ_LATENCY_IF,0);
    CHECK(entry==3);g_irq_latency[0].active=0;
    CHECK(!irq_latency_interrupt_exit(32,IRQ_LATENCY_IF,entry).pending);
    CHECK(g_irq_invalid_sites[22]==1 && g_irq_latency[0].missing_begin==1);
    for(unsigned bad=2;bad<=4;bad+=2){
        reset();host_flags=0;
        CHECK(!irq_latency_interrupt_exit(32,IRQ_LATENCY_IF,bad).pending);
        CHECK(g_irq_invalid_sites[30]==1);
    }
    reset();host_flags=0;g_irq_scope_depth[host_stack]=4;
    entry=irq_latency_interrupt_enter(110,32,IRQ_LATENCY_IF,0);
    CHECK(entry==IRQ_ENTRY_ADMITTED && g_irq_invalid_sites[18]==1);
    CHECK(irq_latency_interrupt_exit(32,IRQ_LATENCY_IF,entry).pending);
    CHECK(g_irq_invalid_boundaries==1);
}

static void first_enable_and_irq(void)
{
    reset();host_flags=0;
    CHECK(irq_latency_interrupt_enter(110,32,IRQ_LATENCY_IF,0)==3);
    CHECK(irq_latency_record_tag(&g_irq_latency[0],1));irq_latency_context_switch();
    clock_value=200; /* Hardware entry's injected timestamp precedes first enable. */
    irq_restore(IRQ_LATENCY_IF);CHECK(irq_latency_record_drain(&g_irq_latency[0]));
    CHECK(g_irq_latency[0].path[0].maximum.cells[IL_BOUNDARY_ADJUSTMENT]==40);
    reset();host_flags=0;
    uint64_t scoped=irq_latency_interrupt_enter(110,32,IRQ_LATENCY_IF,0);
    CHECK(scoped==3);CHECK(irq_latency_record_tag(&g_irq_latency[0],1));
    irq_latency_tail_t tail=irq_latency_interrupt_exit(32,IRQ_LATENCY_IF,scoped);
    CHECK(tail.pending && !tail.probe_end);
    CHECK(tail.pending->witness.cells[IL_BOUNDARY_ADJUSTMENT]==40);
    CHECK(tail.pending->witness.cells[IL_END_KIND]==IRQ_BOUNDARY_IRQ_ADJACENT);
    static irq_latency_snapshot_t snapshot;
    g_irq_latency[0].busy=1;CHECK(!irq_latency_record_snapshot(&g_irq_latency[0],&snapshot));
    g_irq_latency[0].busy=0;
}

static void irq_origin_retired_publisher(void)
{
    reset();host_flags=0;
    CHECK(irq_latency_interrupt_enter(110,32,IRQ_LATENCY_IF,0)==3);
    CHECK(irq_latency_record_tag(&g_irq_latency[0],1));irq_latency_context_switch();
    irq_latency_boundary_t b={.flags=IRQ_LATENCY_IF,.observed_flags=0,.site=0xffffffff80002000};
    CHECK(irq_latency_before_restore(&b));
    CHECK(b.end_cpu==40);
    host_flags=IRQ_LATENCY_IF;
    CHECK(irq_latency_record_interrupt(&g_irq_latency[0],250,0));
    irq_latency_stack_retired(host_stack,owner_generation);
    CHECK(irq_latency_record_drain(&g_irq_latency[0]));
    irq_latency_witness_t *w=&g_irq_latency[0].path[0].maximum;
    CHECK(w->cells[IL_END_KIND]==IRQ_BOUNDARY_RETIRED_PUBLISHER);
    CHECK(w->cells[IL_OBSERVED_END]==0 && w->cells[IL_END]==250);
    CHECK(w->cells[IL_BOUNDARY_ADJUSTMENT]==40);
    CHECK(w->cells[IL_CLOSE_ADJUSTMENT]==40);
    CHECK(w->cells[IL_RETIRE_OWNER_SLOT]==host_stack);
    CHECK(w->cells[IL_RETIRE_OWNER_GENERATION]==owner_generation);
    CHECK(w->cells[IL_RETIRE_SEQUENCE]==w->cells[IL_SEQUENCE]);
}

#if defined(HOBBYOS_IRQ_ENTRY_DIAGNOSTIC) && HOBBYOS_IRQ_ENTRY_DIAGNOSTIC
static void interior_continuity(void)
{
    reset();g_irq_mask_segment[0]=10;
    irq_latency_boundary_t already_enabled={.flags=IRQ_LATENCY_IF,.observed_flags=IRQ_LATENCY_IF};
    CHECK(!irq_latency_before_restore(&already_enabled));
    CHECK(g_irq_mask_segment[0]==10);
    for (unsigned mutation=0;mutation<9;++mutation) {
        reset();memset(g_irq_interior_max,0,sizeof(g_irq_interior_max));
        memset(g_irq_interior_open,0,sizeof(g_irq_interior_open));
        g_irq_mask_segment[0]=1;g_irq_interior_fault_sequence=g_irq_interior_fault_overflow=0;
        (void)irq_save();CHECK(irq_latency_record_tag(&g_irq_latency[0],1));
        irq_latency_lock_token_t token=irq_latency_lock_wait(0x1000);
        irq_latency_lock_acquired(0x1000,token);clock_value+=100000;
        if(mutation==1){irq_latency_boundary_t b={.flags=IRQ_LATENCY_IF,.observed_flags=0};
            (void)irq_latency_before_restore(&b);host_flags=IRQ_LATENCY_IF;host_flags=0;}
        if(mutation==2)irq_latency_context_switch();
        if(mutation==3)++g_irq_entry_sequence[0];
        if(mutation==4)++owner_generation;
        if(mutation==5)++g_irq_interior_fault_sequence;
        if(mutation==6)g_irq_interior_fault_overflow=1;
        if(mutation==7)g_irq_interior_open[0][0].tsc=0;
        if(mutation==8)host_flags=IRQ_LATENCY_IF;
        irq_latency_interior_before_unlock(0x1000);
        CHECK(g_irq_interior_max[0][0].count==(mutation==0));
        CHECK(g_irq_interior_max[0][0].invalid==(mutation!=0));
        if(!mutation){irq_latency_interior_before_unlock(0x1000);CHECK(g_irq_interior_max[0][0].invalid==1);}
    }
}

static void writeback_continuity(void)
{
    for (unsigned m=0;m<12;++m) {
        reset();memset(g_irq_writeback,0,sizeof(g_irq_writeback));g_irq_mask_segment[0]=1;
        (void)irq_save();
        irq_latency_writeback_token_t t=irq_latency_writeback_begin(7,4096);
        CHECK(t.active);
        irq_latency_writeback_stamps_t stamps={1000,201000,2,2};
        if(m==1){irq_restore(IRQ_LATENCY_IF);(void)irq_save();}
        if(m==2)host_cpu=1;
        if(m==3)++owner_generation;
        if(m==4)irq_latency_context_switch();
        if(m==5)++g_irq_entry_sequence[0];
        if(m==6)++g_irq_interior_fault_sequence;
        if(m==7)g_irq_interior_fault_overflow=1;
        if(m==8)stamps.end_flags=IRQ_LATENCY_IF;
        if(m==9)stamps.begin=0;
        if(m==10)++g_irq_identity_queries[0];
        if(m==11)++g_irq_latency[0].sequence;
        irq_latency_writeback_end(t,&stamps);
        if(g_irq_writeback[0].count!=(m==0)) fprintf(stderr,"writeback mutation=%u count=%llu invalid=%llu begin_segment=%llu begin_queries=%llu current_queries=%llu\n",m,(unsigned long long)g_irq_writeback[0].count,(unsigned long long)g_irq_writeback[0].invalid,(unsigned long long)t.begin.segment,(unsigned long long)t.begin.queries,(unsigned long long)g_irq_identity_queries[0]);
        CHECK(g_irq_writeback[0].count==(m==0));CHECK(g_irq_writeback[0].invalid==(m!=0));
        if(!m){irq_latency_writeback_end(t,&stamps);CHECK(g_irq_writeback[0].invalid==1);}
    }
    reset();CHECK(!irq_latency_writeback_begin(1,4096).active);
}
#endif

int main(void)
{
    calibration_after_snapshot();
    activation_during_interrupt();
    nested_ticket();token_invalidation();masked_identity_reuse();masked_identity_boundaries();switch_publication_order();migration_before_cli();
    interrupted_completion(false,false);interrupted_completion(true,false);interrupted_completion(false,true);
    first_enable_and_irq();
    irq_origin_retired_publisher();
#if defined(HOBBYOS_IRQ_ENTRY_DIAGNOSTIC) && HOBBYOS_IRQ_ENTRY_DIAGNOSTIC
    interior_continuity();
    writeback_continuity();
#endif
    puts("IRQ latency actual-C boundary controller and identity lifetime: PASS hardware=HOSTED");
    return 0;
}
