/* Actual fixed-window table preparation; hosted physical/IRQ boundaries only. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/src/memory/paging.c"

static PageTable root;
static struct { PageTable *pointer; bool freed; } frames[32];
static unsigned count, frees, fail_at, translate_fail_at;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t memory_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t preparation;
static bool concurrent_prepare;
static _Thread_local unsigned held, local_allocations;
static _Thread_local irq_flags_t host_flags=512;
static uint64_t data_frame=0x1000;
static void (*interleave)(void);

irq_flags_t spin_lock_irqsave(spinlock_t *lock)
{
    assert(lock==&g_paging_lock && !held);
    irq_flags_t prior=host_flags;host_flags=0;
    assert(!pthread_mutex_lock(&mutex));held=1;return prior;
}
void spin_unlock_irqrestore(spinlock_t *lock,irq_flags_t prior)
{
    assert(lock==&g_paging_lock && held && !host_flags);
    held=0;assert(!pthread_mutex_unlock(&mutex));host_flags=prior;
}
bool irq_is_enabled(void) { return (host_flags&512)!=0; }
uint64_t physical_allocation_ceiling(void) { return UINT64_C(1)<<48; }
uint64_t pmm_get_total_memory(void) { return UINT64_C(1)<<48; }
bool pmm_is_frame_free(uint64_t address) { return address!=data_frame; }
bool image_permissions_overlaps(const image_permissions_t *plan,uint64_t first,uint64_t bytes)
{ assert(plan==&g_image_permissions && bytes==4096);return first==0x2000; }
void *pmm_alloc_frame(void)
{
    assert(!held);
    if(interleave){void (*callback)(void)=interleave;interleave=NULL;callback();}
    assert(!pthread_mutex_lock(&memory_mutex));
    if(fail_at && count+1==fail_at){assert(!pthread_mutex_unlock(&memory_mutex));return NULL;}
    assert(count<32);void *p;assert(!posix_memalign(&p,4096,4096));memset(p,0xa5,4096);
    frames[count++]=(typeof(frames[0])){p,false};
    assert(!pthread_mutex_unlock(&memory_mutex));
    if(concurrent_prepare && !local_allocations++){
        int rc=pthread_barrier_wait(&preparation);assert(!rc || rc==PTHREAD_BARRIER_SERIAL_THREAD);
    }
    return p;
}
void pmm_free_frame(void *pointer)
{
    assert(!held);assert(!pthread_mutex_lock(&memory_mutex));bool found=false;
    for(unsigned i=0;i<count;++i)if(frames[i].pointer==pointer){
        assert(!frames[i].freed);frames[i].freed=true;++frees;found=true;
        for(unsigned j=0;j<g_observer_count;++j)assert(g_observer_tables[j]!=(uintptr_t)pointer);
    }
    assert(found);assert(!pthread_mutex_unlock(&memory_mutex));
}
bool physical_pointer_range(uint64_t address,size_t bytes,physical_kind_t kind,void **out)
{
    assert(bytes==4096 && kind==PHYSICAL_RAM);
    if(translate_fail_at && count==translate_fail_at && !held)return false;
    *out=(void *)(uintptr_t)address;return true;
}
uint64_t physical_require_address(const void *pointer,size_t bytes)
{ assert(bytes==4096);return (uintptr_t)pointer; }
static void reset(void)
{
    assert(!held);for(unsigned i=0;i<count;++i)free(frames[i].pointer);
    memset(frames,0,sizeof(frames));count=frees=fail_at=translate_fail_at=0;
    memset(&root,0,sizeof(root));g_kernel_pml4=&root;
    memset(g_tlb_fixture_entries,0,sizeof(g_tlb_fixture_entries));
    memset(g_tlb_fixture_frames,0,sizeof(g_tlb_fixture_frames));
    memset(g_tlb_fixture_structure,0,sizeof(g_tlb_fixture_structure));
    memset(g_tlb_fixture_levels,0,sizeof(g_tlb_fixture_levels));
    memset(g_observer_tables,0,sizeof(g_observer_tables));
    g_tlb_fixture_root=NULL;g_tlb_fixture_tables=g_observer_count=0;
    g_observer_limit=UINT64_C(1)<<48;g_observer_epoch=g_image_mapping_epoch=0;
    g_ram_plan.features.nx=true;host_flags=512;interleave=NULL;concurrent_prepare=false;local_allocations=0;
}
static void valid_leaves(unsigned window)
{
    assert(!(g_image_mapping_epoch&1));
    for(unsigned p=0;p<TLB_FIXTURE_PAGES;++p){
        assert(g_tlb_fixture_entries[window][p]);
        assert(*g_tlb_fixture_entries[window][p]==(data_frame|fixture_data_flags()));
        assert(g_tlb_fixture_frames[window][p]==data_frame);
    }
    for(unsigned i=0;i<g_tlb_fixture_tables;++i){
        assert(g_tlb_fixture_levels[i]>=1 && g_tlb_fixture_levels[i]<=3);
        assert(g_observer_tables[i]==g_tlb_fixture_structure[i]);
    }
}
static void basic(void)
{
    reset();assert(paging_tlb_fixture_prepare(0,data_frame));valid_leaves(0);
    assert(count==4 && !frees && g_tlb_fixture_tables==4 && irq_is_enabled());
    assert(!paging_tlb_fixture_prepare(0,data_frame));assert(count==4);
    assert(paging_tlb_fixture_clear(0));assert(paging_tlb_fixture_prepare(0,data_frame));assert(count==4);
    assert(paging_tlb_fixture_prepare(1,data_frame));valid_leaves(1);assert(count==7 && !frees);
    assert(!paging_tlb_fixture_prepare(2,data_frame));assert(!paging_tlb_fixture_prepare(0,0));
    assert(!paging_tlb_fixture_prepare(0,data_frame+1));
    reset();host_flags=0;assert(paging_tlb_fixture_prepare(0,data_frame) && !host_flags);valid_leaves(0);
    reset();g_ram_plan.features.nx=false;assert(paging_tlb_fixture_prepare(0,data_frame));valid_leaves(0);
}
static void exhaust_registry(void){g_observer_count=PAGING_OBSERVER_CAPACITY;}
static void corrupt_root(void){root.entries[PML4_INDEX(TLB_FIXTURE_BASE)]=PAGE_PRESENT|PAGE_RW|PAGE_HUGE;}
static void failures(void)
{
    for(unsigned at=1;at<=4;++at){
        reset();fail_at=at;assert(!paging_tlb_fixture_prepare(0,data_frame));
        assert(frees==count && !g_tlb_fixture_tables && !g_observer_count && !(g_image_mapping_epoch&1));
        reset();translate_fail_at=at;assert(!paging_tlb_fixture_prepare(0,data_frame));
        assert(frees==count && !g_tlb_fixture_tables);
    }
    reset();interleave=exhaust_registry;assert(!paging_tlb_fixture_prepare(0,data_frame));assert(frees==count);
    reset();interleave=corrupt_root;assert(!paging_tlb_fixture_prepare(0,data_frame));assert(frees==count);
    reset();assert(paging_tlb_fixture_prepare(0,data_frame));assert(paging_tlb_fixture_clear(0));
    *g_tlb_fixture_entries[0][64]=PAGE_PRESENT;assert(!paging_tlb_fixture_prepare(0,data_frame));
    *g_tlb_fixture_entries[0][64]=0;g_tlb_fixture_entries[0][64]=g_tlb_fixture_entries[0][0];
    assert(!paging_tlb_fixture_prepare(0,data_frame));
}
static void *prepare_thread(void *unused)
{ (void)unused;return (void *)(uintptr_t)paging_tlb_fixture_prepare(0,data_frame); }
static void competing(void)
{
    reset();concurrent_prepare=true;assert(!pthread_barrier_init(&preparation,NULL,2));
    pthread_t a,b;void *x,*y;
    assert(!pthread_create(&a,NULL,prepare_thread,NULL));assert(!pthread_create(&b,NULL,prepare_thread,NULL));
    assert(!pthread_join(a,&x));assert(!pthread_join(b,&y));concurrent_prepare=false;
    assert(!pthread_barrier_destroy(&preparation));assert((uintptr_t)x+(uintptr_t)y==1);
    assert(count==8 && frees==4 && g_tlb_fixture_tables==4);valid_leaves(0);
}
int main(void)
{
    basic();failures();competing();reset();
    puts("Paging actual-C private table preparation, recheck and publication: PASS");return 0;
}
