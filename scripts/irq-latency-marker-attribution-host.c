static void observer_delay(void);
#define IRQ_LATENCY_HOST_IF_QUERY observer_delay
#define main inherited_controller_main
#include "irq-latency-controller-host.c"
#undef main
static uint64_t delay;
static void observer_delay(void) { clock_value+=delay;delay=0; }
static uint64_t sample(uint64_t injected)
{
    reset();memset(g_irq_heap_open,0,sizeof(g_irq_heap_open));
    memset(g_irq_heap_max,0,sizeof(g_irq_heap_max));memset(g_irq_heap_sequence,0,sizeof(g_irq_heap_sequence));
    g_irq_mask_segment[0]=1;(void)irq_save();CHECK(irq_latency_record_tag(&g_irq_latency[0],1));
    irq_latency_lock_token_t token=irq_latency_lock_wait(0x1000);
    irq_latency_lock_acquired(0x1000,token);
    irq_latency_heap_begin(0x1000,IRQ_HEAP_ALLOCATE,0);
    delay=injected;
    irq_latency_heap_mark(0x1000,IRQ_HEAP_FIRST_FIT,0,0);
    irq_latency_heap_mark(0x1000,IRQ_HEAP_METADATA,0,0);
    irq_latency_heap_event_t *e=&g_irq_heap_open[0];
    uint64_t cycles=e->mark[1].tsc-e->mark[0].tsc;
    irq_latency_heap_end(0x1000);CHECK(!g_irq_invalid_boundaries);
    return cycles;
}
int main(void)
{
    uint64_t base=sample(0),injected=sample(500000);
    CHECK(base==10 && injected==500010);
    printf("ACTUAL_MARKER_ATTRIBUTION baseline=%llu observer_tail_injected=%llu delta=%llu metadata_instructions=0\n",
        (unsigned long long)base,(unsigned long long)injected,(unsigned long long)(injected-base));
    return 0;
}
