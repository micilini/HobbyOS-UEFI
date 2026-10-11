#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/src/memory/heap.c"

typedef struct { void *base; size_t pages, freed; unsigned char *released; } backing_t;
static backing_t backing[64];
static size_t backing_count, freed_pages;
static pthread_mutex_t heap_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t backing_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t prepared;
static _Thread_local unsigned held;
static _Thread_local irq_flags_t flags=512;
static bool fail_allocation, fail_translation, concurrent_prepare;
static void (*prepare_interleave)(void);

void spinlock_init(spinlock_t *lock) { memset(lock,0,sizeof(*lock)); }
irq_flags_t spin_lock_irqsave(spinlock_t *lock)
{
    assert(lock==&g_heap_lock && !held);
    irq_flags_t saved=flags;flags=0;
    assert(!pthread_mutex_lock(&heap_mutex));held=1;return saved;
}
void spin_unlock_irqrestore(spinlock_t *lock,irq_flags_t saved)
{
    assert(lock==&g_heap_lock && held && !flags);held=0;
    assert(!pthread_mutex_unlock(&heap_mutex));flags=saved;
}
bool irq_is_enabled(void) { return (flags&512)!=0; }
void console_write_debug(const char *text) { assert(!held && text); }

void *pmm_alloc_contiguous_frames(size_t pages)
{
    assert(!held && pages && pages<=SIZE_MAX/PAGE_SIZE);
    if (prepare_interleave) {
        void (*callback)(void)=prepare_interleave;prepare_interleave=NULL;callback();
    }
    if (fail_allocation) return NULL;
    void *ptr=NULL;assert(!posix_memalign(&ptr,PAGE_SIZE,pages*PAGE_SIZE));
    memset(ptr,0xa5,pages*PAGE_SIZE);
    assert(!pthread_mutex_lock(&backing_mutex));assert(backing_count<64);
    backing[backing_count++]=(backing_t){ptr,pages,0,calloc(pages,1)};
    assert(backing[backing_count-1].released);assert(!pthread_mutex_unlock(&backing_mutex));
    if (concurrent_prepare) {
        int status=pthread_barrier_wait(&prepared);
        assert(status==0 || status==PTHREAD_BARRIER_SERIAL_THREAD);
    }
    return ptr;
}
void pmm_free_frame(void *pointer)
{
    assert(!held && !((uintptr_t)pointer&(PAGE_SIZE-1)));
    assert(!pthread_mutex_lock(&backing_mutex));bool found=false;
    for (size_t i=0;i<backing_count;++i) {
        backing_t *b=&backing[i];uintptr_t p=(uintptr_t)pointer,base=(uintptr_t)b->base;
        if (p>=base && p-base<b->pages*PAGE_SIZE) {
            size_t page=(p-base)/PAGE_SIZE;assert(!b->released[page]);b->released[page]=1;
            ++b->freed;++freed_pages;found=true;break;
        }
    }
    assert(found);assert(!pthread_mutex_unlock(&backing_mutex));
}
bool physical_pointer_range(uint64_t physical,size_t bytes,physical_kind_t kind,void **out)
{
    assert(!held && kind==PHYSICAL_RAM && bytes && out);
    if (fail_translation) return false;
    *out=(void *)(uintptr_t)physical;return true;
}

static void reset(void)
{
    assert(!held);
    for(size_t i=0;i<backing_count;++i){free(backing[i].released);free(backing[i].base);}
    memset(backing,0,sizeof(backing));backing_count=freed_pages=0;
    memset(g_heap_regions,0,sizeof(g_heap_regions));memset(&g_heap_hardening,0,sizeof(g_heap_hardening));
    g_heap_lock_ready=g_heap_initialized=0;g_heap_total_bytes=g_heap_used_bytes=0;g_heap_region_count=0;
    fail_allocation=fail_translation=concurrent_prepare=false;prepare_interleave=NULL;flags=512;
}
static void empty_initialized(void)
{
    reset();spinlock_init(&g_heap_lock);g_heap_lock_ready=1;heap_blocks_init_once();
}
static size_t live_backings(void)
{
    size_t live=0;for(size_t i=0;i<backing_count;++i)live+=backing[i].freed!=backing[i].pages;return live;
}
static void basic(void)
{
    reset();init_heap();assert(g_heap_total_bytes==HEAP_DEFAULT_GROW_BYTES && irq_is_enabled());
    assert(backing_count==1 && heap_validate_integrity());
    void *a=kmalloc(64),*b=kmalloc(64),*c=kmalloc(64);assert(a&&b&&c);
    assert(heap_test_free(a));assert(kmalloc(48)==a);
    assert(heap_test_free(a)&&heap_test_free(b)&&heap_test_free(c));
    assert(!heap_test_free(c));assert(heap_validate_integrity() && !g_heap_used_bytes);
    void *aligned=kmalloc_aligned(173,256);assert(aligned && !((uintptr_t)aligned&255));
    assert(!heap_test_free((char *)aligned+1));assert(heap_test_free(aligned));
    assert(!kmalloc(SIZE_MAX) && !kmalloc_aligned(16,3));assert(heap_validate_integrity());
    flags=0;void *large=kmalloc(HEAP_DEFAULT_GROW_BYTES+PAGE_SIZE);assert(large && !flags);
    assert(heap_test_free(large) && !flags && heap_validate_integrity());flags=512;
}

static void *freed_by_competitor;
static void competitor_free(void) { assert(heap_test_free(freed_by_competitor)); }
static void recheck(bool allocation_failure)
{
    reset();init_heap();freed_by_competitor=kmalloc(HEAP_DEFAULT_GROW_BYTES-sizeof(HeapBlock));
    assert(freed_by_competitor && !heap_find_first_fit(16));
    fail_allocation=allocation_failure;prepare_interleave=competitor_free;
    void *chosen=kmalloc(32);assert(chosen==freed_by_competitor && !prepare_interleave);
    assert(g_heap_total_bytes==HEAP_DEFAULT_GROW_BYTES && live_backings()==1);
    assert(backing_count==(allocation_failure?1u:2u));
    if (!allocation_failure)assert(freed_pages==backing[1].pages);
    assert(heap_test_free(chosen) && heap_validate_integrity());
}
static void exhausted_regions(void) { g_heap_region_count=HEAP_MAX_REGIONS; }
static void exhausted_total(void) { g_heap_total_bytes=UINT64_MAX-1; }
static void failures(void)
{
    empty_initialized();fail_allocation=true;assert(!kmalloc(32));assert(!g_heap_total_bytes && !backing_count);
    empty_initialized();fail_translation=true;assert(!kmalloc(32));assert(!g_heap_total_bytes && !live_backings());
    for(unsigned which=0;which<2;++which){
        empty_initialized();prepare_interleave=which?exhausted_total:exhausted_regions;
        assert(!kmalloc(32));assert(!live_backings() && !g_heap_used_bytes && list_empty(&g_heap_blocks));
        g_heap_region_count=0;g_heap_total_bytes=0;assert(heap_validate_integrity());
    }
    empty_initialized();assert(!kmalloc(SIZE_MAX-15) && !backing_count);
}
static void *grow_thread(void *unused)
{
    (void)unused;void *p=kmalloc(65536);assert(p && irq_is_enabled());return p;
}
static void competing_growth(void)
{
    empty_initialized();concurrent_prepare=true;assert(!pthread_barrier_init(&prepared,NULL,2));
    pthread_t a,b;void *x,*y;
    assert(!pthread_create(&a,NULL,grow_thread,NULL)&&!pthread_create(&b,NULL,grow_thread,NULL));
    assert(!pthread_join(a,&x)&&!pthread_join(b,&y));concurrent_prepare=false;
    assert(!pthread_barrier_destroy(&prepared));assert(x!=y && backing_count==2 && live_backings()==1);
    assert(g_heap_total_bytes==33*PAGE_SIZE && g_heap_used_bytes==2*65536);
    assert(freed_pages==33 && heap_validate_integrity());
    assert(heap_test_free(x)&&heap_test_free(y)&&heap_validate_integrity()&&!g_heap_used_bytes);
}
int main(void)
{
    basic();recheck(false);recheck(true);failures();competing_growth();reset();
    puts("Heap actual-C private preparation, first-fit recheck and competing growth: PASS");
    return 0;
}
