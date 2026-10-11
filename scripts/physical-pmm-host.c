#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/src/memory/pmem.c"

/* Execute the real allocation/free/integrity wrappers. Only hardware lock/IF
 * primitives and the boot-established ceiling are substituted. */
static uint64_t ceiling;
static irq_flags_t host_flags;
static unsigned locks, unlocks, checks;
uint64_t physical_allocation_ceiling(void) { return ceiling; }
void serial_write_all(const char *text) { assert(text); }
irq_flags_t spin_lock_irqsave(spinlock_t *lock)
{
    assert(lock==&g_pmm_lock && !lock->locked);
    irq_flags_t saved=host_flags; host_flags &= ~UINT64_C(0x200);
    lock->locked=1; locks++; return saved;
}
void spin_unlock_irqrestore(spinlock_t *lock, irq_flags_t saved)
{
    assert(lock==&g_pmm_lock && lock->locked && !(host_flags&0x200));
    lock->locked=0; host_flags=saved; unlocks++;
}
static void prepare(size_t bits, size_t limit, size_t minimum)
{
    g_bitmap.size=(bits+7)/8; g_bitmap.total_bits=bits;
    g_bitmap.buffer=malloc(g_bitmap.size);
    g_reserved_bitmap=(Bitmap){malloc(g_bitmap.size),g_bitmap.size,bits};
    assert(g_bitmap.buffer && g_reserved_bitmap.buffer);
    memset(g_bitmap.buffer,0xff,g_bitmap.size);
    memset(g_reserved_bitmap.buffer,0,g_bitmap.size);
    g_total_frames=bits; g_bitmap_size=g_bitmap.size; g_free_frames=0;
    g_alloc_limit_frames=limit; g_min_alloc_frame=minimum; ceiling=limit*PAGE_SIZE;
}
static void available(size_t frame)
{ assert(bitmap_get(&g_bitmap,frame)); bitmap_set(&g_bitmap,frame,false); g_free_frames++; }
static void finish(void)
{ free(g_bitmap.buffer); free(g_reserved_bitmap.buffer); }
static void allocate(size_t count, uint64_t expected_frame, bool single)
{
    uint8_t *before=malloc(g_bitmap.size); assert(before);
    memcpy(before,g_bitmap.buffer,g_bitmap.size);
    uint64_t free_before=g_free_frames;
    irq_flags_t entry=host_flags;
    void *value=single?pmm_alloc_frame():pmm_alloc_contiguous_frames(count);
    assert((uintptr_t)value==expected_frame*PAGE_SIZE);
    assert(host_flags==entry && locks==unlocks && !g_pmm_lock.locked);
    if (!expected_frame) {
        assert(g_free_frames==free_before && !memcmp(before,g_bitmap.buffer,g_bitmap.size));
    } else {
        assert(expected_frame+count<=g_alloc_limit_frames && g_free_frames==free_before-count);
        for(size_t i=0;i<count;i++) { assert(!((before[(expected_frame+i)/8]>>((expected_frame+i)%8))&1)); }
    }
    assert(pmm_validate_integrity());
    assert(host_flags==entry); checks++; free(before);
}
int main(void)
{
    const size_t limits[]={0,1,7,8,9,63,64,65,127,128,129,256,257,511};
    for(unsigned enabled=0;enabled<2;enabled++) {
        host_flags=0x45|(enabled?0x200:0);
        for(size_t n=0;n<sizeof(limits)/sizeof(limits[0]);n++) {
            size_t limit=limits[n]; prepare(513,limit,1);
            available(limit); available(512);
            allocate(1,0,true); allocate(2,0,false);
            if(limit>1) { available(limit-1); allocate(1,limit-1,true); }
            if(limit>3) { available(limit-2); available(limit-1); allocate(2,limit-2,false); }
            allocate(0,0,false); allocate(SIZE_MAX,0,false);
            finish();
        }
        prepare(513,129,256); available(128); allocate(1,0,true); finish();
        prepare(513,129,1); available(63); available(64); available(128); available(129);
        allocate(2,63,false); allocate(2,0,false); allocate(1,128,true); finish();
        prepare(513,129,1); available(3); ceiling=128*PAGE_SIZE;
        allocate(1,0,true); finish();
        prepare(513,129,1); available(3); ceiling=129*PAGE_SIZE+1;
        allocate(1,0,true); allocate(2,0,false); finish();
    }
    prepare(268435456,1048576,256);
    available(1048575); available(1048576); available(268435455);
    allocate(2,0,false); allocate(1,1048575,true);
    assert(!bitmap_get(&g_bitmap,1048576) && !bitmap_get(&g_bitmap,268435455));
    bitmap_set(&g_reserved_bitmap,268435455,true);
    assert(!pmm_validate_integrity());
    bitmap_set(&g_reserved_bitmap,268435455,false); finish();
    printf("status=PASS checks=%u logical_frames=268435456 scope=actual_pmm_wrappers privileged_operations=SUBSTITUTED\n",checks);
    return 0;
}
