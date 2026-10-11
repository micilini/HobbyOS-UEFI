#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/src/memory/pmem.c"

static irq_flags_t host_flags;
static unsigned locks, unlocks, action, fired;
static bool injecting;
static uint64_t ceiling;
uint64_t physical_allocation_ceiling(void) { return ceiling; }
void serial_write_all(const char *text) { assert(text); }
irq_flags_t spin_lock_irqsave(spinlock_t *lock)
{
    assert(lock == &g_pmm_lock && !lock->locked);
    irq_flags_t saved = host_flags;
    host_flags &= ~UINT64_C(512); lock->locked = 1; ++locks;
    return saved;
}
void spin_unlock_irqrestore(spinlock_t *lock, irq_flags_t flags)
{
    assert(lock == &g_pmm_lock && lock->locked && !(host_flags & 512));
    lock->locked = 0; host_flags = flags; ++unlocks;
    if (!injecting && action && unlocks == 3 && (flags & 512)) {
        injecting = true; ++fired;
        if (action == 1) pmm_free_frame((void *)(uintptr_t)(2 * PAGE_SIZE));
        if (action == 2) {
            pmm_free_frame((void *)(uintptr_t)(2 * PAGE_SIZE));
            assert((uintptr_t)pmm_alloc_frame() == 2 * PAGE_SIZE);
        }
        if (action == 3) bitmap_set(&g_reserved_bitmap, g_total_frames-1, true);
        if (action == 4) assert(pmm_validate_integrity());
        if (action == 5) pmm_reserve_range(0,10);
        if (action == 6) g_bitmap.size--;
        injecting = false;
    }
}
static void setup(size_t bits)
{
    g_bitmap_size = (bits + 7) / 8; g_total_frames = bits;
    g_bitmap = (Bitmap){malloc(g_bitmap_size),g_bitmap_size,bits};
    g_reserved_bitmap = (Bitmap){malloc(g_bitmap_size),g_bitmap_size,bits};
    assert(g_bitmap.buffer && g_reserved_bitmap.buffer);
    memset(g_bitmap.buffer,0xff,g_bitmap_size); memset(g_reserved_bitmap.buffer,0,g_bitmap_size);
    bitmap_set(&g_bitmap,bits-1,false); g_free_frames = 1;
    g_min_alloc_frame = 1; g_alloc_limit_frames = bits; ceiling = bits * PAGE_SIZE;
    host_flags = 0x245; locks = unlocks = fired = 0; action = 0; injecting = false;
}
static void finish(void)
{
    assert(locks == unlocks && !g_pmm_lock.locked && host_flags == 0x245);
    free(g_bitmap.buffer); free(g_reserved_bitmap.buffer);
}
int main(void)
{
    setup(65539); assert(pmm_validate_integrity());
    assert(locks > 2); finish();
    for (action = 1; action <= 6;) {
        unsigned next = action; setup(65539); action = next;
        assert(pmm_validate_integrity() == (action != 3 && action != 6)); assert(fired == 1);
        finish(); action = next + 1;
    }
    setup(65539); g_free_frames++; assert(!pmm_validate_integrity()); finish();
    setup(65539); g_bitmap.size--; assert(!pmm_validate_integrity()); finish();
    setup(65539); host_flags &= ~UINT64_C(512);
    assert(pmm_validate_integrity() && locks == 1 && !(host_flags & 512));
    host_flags |= 512; finish();
    setup(65539); g_pmm_observation_count = PMM_OBSERVATION_SLOTS;
    assert(pmm_validate_integrity() && locks == 1); g_pmm_observation_count = 0; finish();
    for (unsigned a = 1; a <= 5; ++a) {
        setup(65539); action = a; PmmReachability r;
        assert(pmm_get_reachability(&r) == (a != 3));
        if (a != 3) {
            size_t count = 0, below = 0, low = 0;
            for (size_t n=0;n<g_total_frames;++n) if (!bitmap_get(&g_bitmap,n)) {
                ++count; if (n < g_alloc_limit_frames) ++below;
                if (n < g_min_alloc_frame) ++low;
            }
            assert(r.free_frames == count && r.eligible_free_frames == below-low);
            assert(r.above_ceiling_free_frames == count-below);
        }
        finish();
    }
    setup(137);
    for (size_t n=0;n<137;++n) {
        bitmap_set(&g_bitmap,n,(n*11%7) < 3); bitmap_set(&g_reserved_bitmap,n,(n*13%11) < 3);
    }
    for (size_t start=0;start<=137;++start) for (size_t width=0;width<=137-start;++width) {
        size_t free=0,bad=0,f=SIZE_MAX,b=SIZE_MAX;
        for (size_t n=start;n<start+width;++n) if (!bitmap_get(&g_bitmap,n)) {
            ++free; if (bitmap_get(&g_reserved_bitmap,n)) ++bad;
        }
        assert(bitmap_observe_allocation(&g_bitmap,&g_reserved_bitmap,start,width,&f,&b));
        assert(f==free && b==bad);
    }
    size_t free=19,bad=23;
    assert(!bitmap_observe_allocation(&g_bitmap,&g_reserved_bitmap,SIZE_MAX,1,&free,&bad));
    assert(free==19 && bad==23); finish();
    puts("PMM actual-C coherent incremental observation: PASS");
}
