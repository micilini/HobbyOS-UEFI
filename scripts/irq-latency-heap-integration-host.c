#define main inherited_heap_main
#include "heap-preparation-host.c"
#undef main
static _Thread_local unsigned operation, mark_count;
static _Thread_local uint64_t first_fit_work, containing_work, prior_site;
static _Thread_local bool active;
void irq_latency_heap_begin(uintptr_t lock,unsigned op,uint64_t pages)
{
    (void)pages;assert(lock==(uintptr_t)&g_heap_lock && held && !flags && !active);
    active=true;operation=op;mark_count=0;prior_site=0;
}
void irq_latency_heap_mark(uintptr_t lock,unsigned site,uint64_t work,uint64_t bytes)
{
    (void)bytes;
    if(!held)return; /* The inherited test directly calls a search helper. */
    assert(lock==(uintptr_t)&g_heap_lock && active && !flags && ++mark_count<=8);
    assert(site!=prior_site && site>=1 && site<=10);prior_site=site;
    if(site==IRQ_HEAP_FIRST_FIT)first_fit_work=work;
    if(site==IRQ_HEAP_CONTAINMENT)containing_work=work;
}
void irq_latency_heap_end(uintptr_t lock)
{
    assert(lock==(uintptr_t)&g_heap_lock && active && held && !flags && mark_count);
    assert(operation>=1 && operation<=7);active=false;
}
int main(void)
{
    int result=inherited_heap_main();assert(!active);
    init_heap();void *p[128];
    for(unsigned i=0;i<128;++i){p[i]=kmalloc(64);assert(p[i]);assert(first_fit_work==i+1);}
    assert(heap_test_free(p[127]) && containing_work==128);
    for(unsigned i=0;i<127;++i)assert(heap_test_free(p[i]));
    assert(heap_validate_integrity());reset();
    puts("Heap actual-C operation checkpoints and node counts: PASS clocks=HOSTED");
    return result;
}

uint64_t irq_latency_host_read_tsc(void) { static _Thread_local uint64_t t;return ++t; }
irq_latency_heap_operation_token_t irq_latency_heap_operation_prepare(uintptr_t lock)
{ assert(lock==(uintptr_t)&g_heap_lock && active && held);return (irq_latency_heap_operation_token_t){.active=true}; }
void irq_latency_heap_operation_publish(const irq_latency_heap_operation_token_t *t,uint64_t a,uint64_t b,uint64_t bytes)
{ assert(t->active && a<b && (bytes==0 || bytes==32) && active && held); }
