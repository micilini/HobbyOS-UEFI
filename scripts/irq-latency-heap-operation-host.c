#include <limits.h>
static void query_hook(void);
#define IRQ_LATENCY_HOST_IF_QUERY query_hook
#define main inherited_controller_main
#include "irq-latency-controller-host.c"
#undef main
#define IRQ_LATENCY_HEAP_OPERATION_HOSTED 1
#include "../kernel/src/memory/heap.c"
static unsigned delay_mode, mutation;
static unsigned invalid_case=UINT_MAX;
static bool injected;
static void *backing;
void irq_latency_heap_metadata_hosted(void)
{
    CHECK(g_irq_heap_operation_state[0]==1 && !(host_flags&512));
    if(delay_mode==2) { clock_value+=500000;injected=true; }
    if(invalid_case==0) { irq_latency_context_switch();host_cpu=1; }
    if(invalid_case==1) ++owner_generation;
    if(invalid_case==2) ++g_irq_mask_segment[0];
    if(invalid_case==3) { irq_restore(512);(void)irq_save(); }
    if(invalid_case==4) ++g_irq_interior_fault_sequence;
    if(invalid_case==5) ++g_irq_locks[0][g_irq_lock_depth[0]-1].lock;
    if(invalid_case==6) (void)irq_latency_heap_operation_prepare((uintptr_t)&g_heap_lock);
    if(invalid_case==7) g_irq_heap_operation_state[0]=2;
    if(invalid_case==8) ++g_irq_heap_operation_open[0].sequence;
}
static void query_hook(void)
{
    if (injected || !g_irq_heap_open[0].operation || !g_irq_heap_open[0].body.begin.tsc) return;
    if ((delay_mode==1 && !g_irq_heap_open[0].marks) ||
        (delay_mode==3 && g_irq_heap_operation_state[0]==1)) {
        clock_value+=500000;injected=true;
    }
}
void spinlock_init(spinlock_t *lock) { memset(lock,0,sizeof(*lock)); }
irq_flags_t spin_lock_irqsave(spinlock_t *lock)
{
    irq_flags_t flags=irq_save();CHECK(irq_latency_record_tag(&g_irq_latency[0],1));
    irq_latency_lock_token_t token=irq_latency_lock_wait((uintptr_t)lock);
    irq_latency_lock_acquired((uintptr_t)lock,token);return flags;
}
void spin_unlock_irqrestore(spinlock_t *lock,irq_flags_t flags)
{
    irq_latency_interior_before_unlock((uintptr_t)lock);
    irq_latency_lock_released((uintptr_t)lock);irq_restore(flags);
}
void console_write_debug(const char *s) { (void)s;CHECK(false); }
void *pmm_alloc_contiguous_frames(size_t n) { (void)n;CHECK(false);return NULL; }
void pmm_free_frame(void *p) { (void)p;CHECK(false); }
bool physical_pointer_range(uint64_t p,size_t n,physical_kind_t k,void **out)
{ (void)p;(void)n;(void)k;(void)out;CHECK(false);return false; }
static void start(void)
{
    reset();memset(g_irq_frozen,0,sizeof(g_irq_frozen));
    memset(g_irq_heap_open,0,sizeof(g_irq_heap_open));memset(g_irq_heap_max,0,sizeof(g_irq_heap_max));
    memset(g_irq_heap_sequence,0,sizeof(g_irq_heap_sequence));
    memset(g_irq_heap_operation_state,0,sizeof(g_irq_heap_operation_state));
    memset(g_irq_heap_operation_open,0,sizeof(g_irq_heap_operation_open));
    memset(g_irq_heap_operation_max,0,sizeof(g_irq_heap_operation_max));
    g_irq_mask_segment[0]=1;delay_mode=0;injected=false;invalid_case=UINT_MAX;
    if(backing)free(backing);
    backing=aligned_alloc(PAGE_SIZE,PAGE_SIZE);CHECK(backing);
    memset(backing,0,PAGE_SIZE);memset(g_heap_regions,0,sizeof(g_heap_regions));g_heap_region_count=0;
    g_heap_initialized=0;g_heap_lock_ready=1;g_heap_total_bytes=PAGE_SIZE;g_heap_used_bytes=0;
    heap_blocks_init_once();block_initialize(backing,PAGE_SIZE-sizeof(HeapBlock),HEAP_BLOCK_FREE);
    CHECK(heap_register_region_locked((uintptr_t)backing,PAGE_SIZE));heap_append_tail(backing);
}
static irq_latency_heap_operation_t sample(unsigned mode)
{
    start();delay_mode=mode;
    void *p=kmalloc(64);CHECK(p==payload_from_block(backing));
    CHECK(g_heap_used_bytes==64 && ((HeapBlock *)backing)->state==HEAP_BLOCK_ALLOCATED);
    CHECK(next_block(backing) && block_is_free(next_block(backing)) && next_block(backing)->size==3968);
    CHECK(g_irq_heap_operation_max[0].count==1 && !g_irq_heap_operation_max[0].invalid);
    CHECK(g_irq_heap_max[0][0].metadata_count==1 && !g_irq_invalid_boundaries);
    CHECK(irq_latency_record_drain(&g_irq_latency[0]));
    return g_irq_heap_operation_max[0].maximum;
}
int main(void)
{
    irq_latency_heap_operation_t base=sample(0),before=sample(1),after=sample(3),inside=sample(2);
    CHECK(base.interval.end.tsc-base.interval.begin.tsc==10);
    CHECK(before.interval.end.tsc-before.interval.begin.tsc==10);
    CHECK(after.interval.end.tsc-after.interval.begin.tsc==10);
    CHECK(inside.interval.end.tsc-inside.interval.begin.tsc==500010);
    CHECK(before.parent.end.tsc-before.parent.begin.tsc==base.parent.end.tsc-base.parent.begin.tsc+500000);
    CHECK(after.parent.end.tsc-after.parent.begin.tsc==base.parent.end.tsc-base.parent.begin.tsc+500000);
    for(mutation=0;mutation<17;++mutation) {
        irq_latency_heap_operation_t w=base;
        if(mutation==0)++w.interval.end.cpu;
        if(mutation==1)++w.interval.end.generation;
        if(mutation==2)++w.interval.end.segment;
        if(mutation==3)++w.interval.end.switches;
        if(mutation==4)++w.interval.end.entries;
        if(mutation==5)++w.interval.end.faults;
        if(mutation==6)++w.interval.end.lock;
        if(mutation==7)++w.interval.end.depth;
        if(mutation==8)w.interval.end.flags=512;
        if(mutation==9)w.interval.end.tsc=0;
        if(mutation==10)w.prepared=w.interval.begin.tsc;
        if(mutation==11)w.validated=w.interval.end.tsc;
        if(mutation==12)w.sequence=0;
        if(mutation==13)w.operation=IRQ_HEAP_FREE;
        if(mutation==14)w.header_bytes=64;
        if(mutation==15)++w.parent.end.segment;
        if(mutation==16)w.interval.begin.queries=UINT64_MAX;
        CHECK(!irq_latency_heap_operation_valid(&w));
    }
    irq_latency_heap_operation_record_t r={0};CHECK(irq_latency_heap_operation_record(&r,&base));
    CHECK(!irq_latency_heap_operation_record(&r,&base) && r.count==1 && r.invalid==1);
    irq_latency_heap_operation_t smaller=base;smaller.sequence=2;
    smaller.interval.end.tsc=smaller.interval.begin.tsc+5;
    CHECK(irq_latency_heap_operation_record(&r,&smaller) && r.count==2 && r.maximum_cycles==10 && r.maximum.sequence==1);
    r.count=UINT64_MAX;smaller.sequence=3;
    CHECK(!irq_latency_heap_operation_record(&r,&smaller) && r.count==UINT64_MAX && r.invalid==2 && r.maximum_cycles==10);
    for(unsigned i=0;i<9;++i) {
        start();invalid_case=i;CHECK(kmalloc(64));
        CHECK(g_irq_invalid_boundaries || g_irq_heap_operation_max[0].invalid || g_irq_heap_operation_state[0]);
        if (g_irq_heap_operation_max[0].count) CHECK(g_irq_invalid_boundaries);
    }
    free(backing);backing=NULL;
    puts("Heap actual call-site and controller: PASS observer_tail_excluded=500000 parent_retained=1 minimum_only=1");
    return 0;
}
