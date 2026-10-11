#include <assert.h>
#include <stdio.h>
#include "../kernel/src/core/spinlock.h"

/* Compile the real wrapper; only the privileged lock primitives are substituted.
 * Unused PMM functions are discarded by --gc-sections, never called on the host. */
#include "../kernel/src/memory/pmem.c"

static irq_flags_t emulated_flags;
static unsigned acquisitions, releases;
uint64_t physical_allocation_ceiling(void) { return g_alloc_limit_frames * PAGE_SIZE; }
irq_flags_t spin_lock_irqsave(spinlock_t *lock)
{
    assert(lock == &g_pmm_lock && !lock->locked);
    irq_flags_t saved = emulated_flags;
    emulated_flags &= ~(UINT64_C(1) << 9);
    lock->locked = 1; acquisitions++;
    return saved;
}
void spin_unlock_irqrestore(spinlock_t *lock, irq_flags_t flags)
{
    assert(lock == &g_pmm_lock && lock->locked && !(emulated_flags & (1u << 9)));
    lock->locked = 0; emulated_flags = flags; releases++;
}
static void check(bool expected, irq_flags_t entry)
{
    emulated_flags = entry;
    uint64_t count = g_free_frames, total = g_total_frames;
    unsigned before = acquisitions;
    assert(pmm_validate_integrity() == expected);
    assert(acquisitions == before + 1 && releases == acquisitions && !g_pmm_lock.locked);
    assert(emulated_flags == entry && count == g_free_frames && total == g_total_frames);
}
int main(void)
{
    uint8_t allocated[2] = {0xfe, 0xff}, reserved[2] = {0xfe, 0xff};
    for (unsigned if_enabled = 0; if_enabled < 2; if_enabled++)
    {
        irq_flags_t entry = 0x45 | (if_enabled ? 0x200 : 0);
        g_bitmap = (Bitmap){allocated, 2, 9}; g_reserved_bitmap = (Bitmap){reserved, 2, 9};
        g_total_frames = 9; g_free_frames = 1; g_bitmap_size = 2;
        check(true, entry);
        g_free_frames = 0; check(false, entry);
        g_free_frames = 2; check(false, entry);
        g_free_frames = 1; reserved[0] |= 1; check(false, entry); reserved[0] &= 0xfe;
        g_bitmap.total_bits = 8; check(false, entry); g_bitmap.total_bits = 9;
        g_bitmap.size = 1; check(false, entry); g_bitmap.size = 2;
    }
    printf("{\"status\":\"PASS\",\"scope\":\"real_wrapper_substituted_host_lock_primitives\",\"checks\":%u}\n", acquisitions);
    return 0;
}
