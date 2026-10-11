#define main inherited_controller_main
#include "irq-latency-controller-host.c"
#undef main

static void heap_reset(void)
{
    reset();memset(g_irq_heap_open,0,sizeof(g_irq_heap_open));
    memset(g_irq_heap_max,0,sizeof(g_irq_heap_max));memset(g_irq_heap_sequence,0,sizeof(g_irq_heap_sequence));
    memset(g_irq_frozen,0,sizeof(g_irq_frozen));
    g_irq_mask_segment[0]=g_irq_mask_segment[1]=1;
    (void)irq_save();CHECK(irq_latency_record_tag(&g_irq_latency[0],1));
    irq_latency_lock_token_t token=irq_latency_lock_wait(0x1000);
    irq_latency_lock_acquired(0x1000,token);
}
int main(void)
{
    heap_reset();irq_latency_heap_begin(0x1000,IRQ_HEAP_ALLOCATE,0);
    irq_latency_heap_mark(0x1000,IRQ_HEAP_FIRST_FIT,123,0);
    irq_latency_heap_mark(0x1000,IRQ_HEAP_METADATA,1,32);
    irq_latency_heap_end(0x1000);
    CHECK(!g_irq_invalid_boundaries && g_irq_heap_max[0][0].count==1);
    CHECK(g_irq_heap_max[0][0].maximum.mark[0].work==123);
    CHECK(!g_irq_heap_open[0].operation && !irq_is_enabled());
    for (unsigned mutation=0;mutation<8;++mutation) {
        heap_reset();irq_latency_heap_begin(0x1000,IRQ_HEAP_FREE,0);
        irq_latency_heap_mark(0x1000,IRQ_HEAP_REGION,1,0);
        if(mutation==0)++g_irq_mask_segment[0];
        if(mutation==1)++owner_generation;
        if(mutation==2)++g_irq_entry_sequence[0];
        if(mutation==3)++g_irq_switch_sequence[0];
        if(mutation==4)++g_irq_interior_fault_sequence;
        if(mutation==5)g_irq_heap_open[0].marks=9;
        if(mutation==6){irq_latency_context_switch();host_cpu=1;}
        if(mutation==7)irq_latency_lock_released(0x1000);
        irq_latency_heap_end(0x1000);
        CHECK(g_irq_invalid_boundaries || g_irq_heap_open[0].operation);
        CHECK(!g_irq_heap_max[0][IRQ_HEAP_FREE-1].count);
        g_irq_frozen[0]=1;
        CHECK(!irq_latency_heap_read(0,IRQ_HEAP_FREE) || g_irq_heap_max[0][IRQ_HEAP_FREE-1].invalid);
    }
    heap_reset();irq_latency_heap_begin(0x1000,IRQ_HEAP_FREE,0);
    irq_latency_heap_begin(0x1000,IRQ_HEAP_FREE,0);CHECK(g_irq_invalid_boundaries);
    puts("IRQ latency actual-C heap controller identity and ownership: PASS minimum_only=1");
    return inherited_controller_main();
}
