#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/src/memory/pmem.c"

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t barrier;
static _Thread_local irq_flags_t host_flags = 0x245;
static _Thread_local bool writer;
static unsigned concurrent_writes, completed;
uint64_t physical_allocation_ceiling(void) { return g_alloc_limit_frames*PAGE_SIZE; }
void serial_write_all(const char *text) { assert(text); }
irq_flags_t spin_lock_irqsave(spinlock_t *lock)
{
    irq_flags_t saved = host_flags; host_flags &= ~UINT64_C(512);
    assert(!pthread_mutex_lock(&mutex)); assert(lock == &g_pmm_lock && !lock->locked);
    lock->locked=1; return saved;
}
void spin_unlock_irqrestore(spinlock_t *lock, irq_flags_t flags)
{
    assert(lock==&g_pmm_lock && lock->locked && !(host_flags&512));
    if (writer && g_pmm_observation_count) ++concurrent_writes;
    lock->locked=0; assert(!pthread_mutex_unlock(&mutex)); host_flags=flags;
}
static void *worker(void *argument)
{
    writer = (uintptr_t)argument < 4;
    int value=pthread_barrier_wait(&barrier); assert(!value || value==PTHREAD_BARRIER_SERIAL_THREAD);
    if (writer) for (unsigned n=0;n<10000;++n) {
        void *frame=pmm_alloc_frame(); assert(frame); pmm_free_frame(frame);
    } else for (unsigned n=0;n<30;++n) {
        PmmReachability r; assert(pmm_validate_integrity()); assert(pmm_get_reachability(&r));
        assert(r.free_frames==r.eligible_free_frames && !r.above_ceiling_free_frames);
        __atomic_add_fetch(&completed,1,__ATOMIC_RELAXED);
    }
    assert(host_flags==0x245); return NULL;
}
int main(void)
{
    g_total_frames=65539; g_bitmap_size=(g_total_frames+7)/8; g_alloc_limit_frames=g_total_frames; g_min_alloc_frame=256;
    g_bitmap=(Bitmap){malloc(g_bitmap_size),g_bitmap_size,g_total_frames};
    g_reserved_bitmap=(Bitmap){calloc(g_bitmap_size,1),g_bitmap_size,g_total_frames};
    assert(g_bitmap.buffer && g_reserved_bitmap.buffer); memset(g_bitmap.buffer,0xff,g_bitmap_size);
    for (size_t n=1000;n<2000;++n) bitmap_set(&g_bitmap,n,false);
    g_free_frames=1000; pthread_t threads[6]; assert(!pthread_barrier_init(&barrier,NULL,6));
    for (uintptr_t n=0;n<6;++n) assert(!pthread_create(&threads[n],NULL,worker,(void *)n));
    for (unsigned n=0;n<6;++n) assert(!pthread_join(threads[n],NULL));
    assert(completed==60 && concurrent_writes && g_free_frames==1000 && !g_pmm_observation_count);
    assert(pmm_validate_integrity()); free(g_bitmap.buffer); free(g_reserved_bitmap.buffer);
    assert(!pthread_barrier_destroy(&barrier));
    puts("PMM actual-C concurrent writers and complete observations: PASS");
}
