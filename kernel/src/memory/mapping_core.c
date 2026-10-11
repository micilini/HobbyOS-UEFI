#include "../core/irq_latency.h"
#include "mapping_core.h"
#include "physical_layout.h"
#include "../libc/memory.h"

bool mapping_range_checked(uint64_t physical, uint64_t bytes, mapping_range_t *out)
{
    if (!out || !bytes || physical >= PHYSICAL_ADDRESS_LIMIT ||
        bytes > PHYSICAL_ADDRESS_LIMIT - physical) return false;
    uint64_t first = physical & ~UINT64_C(4095), end = physical + bytes;
    if (end > PHYSICAL_ADDRESS_LIMIT - 4095 && (end & 4095)) return false;
    uint64_t rounded = (end + 4095) & ~UINT64_C(4095);
    *out = (mapping_range_t){first, (rounded - first) / 4096,
                             physical - first, bytes};
    return true;
}

bool dma_extent_limit(uint64_t bytes, uint64_t alignment, uint64_t maximum,
    uint64_t ceiling, mapping_range_t *out, uint64_t *limit)
{
    mapping_range_t range;
    if (!out || !limit || !alignment || (alignment & (alignment - 1)) ||
        (ceiling & 4095) || ceiling > PHYSICAL_ADDRESS_LIMIT ||
        !mapping_range_checked(0, bytes, &range)) return false;
    uint64_t bound = ceiling;
    if (maximum != UINT64_MAX && maximum + 1 < bound) bound = maximum + 1;
    bound &= ~UINT64_C(4095);
    if (!bound || range.pages > bound / 4096 || alignment > bound) return false;
    *out = range; *limit = bound;
    return true;
}

mapping_status_t mapping_window_construct(mapping_window_t *w, PageTable *root,
    const mapping_table_ops_t *ops, void *context, bool nx)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_MAPPING);
    const unsigned index = (DEVICE_WINDOW_BASE >> 39) & 511;
    if (!w || !root || !ops || !ops->allocate || !ops->release || w->ready || w->allocated)
        return MAPPING_INVALID;
    if (root->entries[index]) return MAPPING_CONFLICT;
    for (unsigned i = 0; i < MAPPING_WINDOW_TABLES; ++i) {
        mapping_table_t table = {0};
        if (!ops->allocate(context, &table)) {
            while (w->allocated) {
                unsigned at = --w->allocated;
                ops->release(context, w->tables[at]);
                w->tables[at] = (mapping_table_t){0};
            }
            return MAPPING_NO_MEMORY;
        }
        if (!table.cpu || !table.physical || (table.physical & 4095) ||
            table.physical > PHYSICAL_ADDRESS_LIMIT - 4096) {
            if (table.cpu) ops->release(context, table);
            while (w->allocated) {
                unsigned at = --w->allocated;
                ops->release(context, w->tables[at]);
                w->tables[at] = (mapping_table_t){0};
            }
            return MAPPING_INVALID;
        }
        memset(table.cpu, 0, sizeof(PageTable));
        w->tables[w->allocated++] = table;
    }
    uint64_t flags=PAGE_PRESENT|PAGE_RW|(nx ? PAGE_NX : 0);
    w->tables[0].cpu->entries[0] = w->tables[1].physical | flags;
    for (unsigned i = 0; i < 512; ++i)
        w->tables[1].cpu->entries[i] = w->tables[i + 2].physical | flags;
    /* No child is visible to a hardware walker before this one publication.
     * On failure only unpublished frames are released. Published structure is
     * retained for the boot, including after the last managed leaf is removed. */
    __atomic_store_n(&root->entries[index], w->tables[0].physical | flags,
                     __ATOMIC_RELEASE);
    w->nx=nx; w->ready = true;
    return MAPPING_OK;
}

static PageEntry *leaf(mapping_window_t *w, uint64_t page)
{ return &w->tables[2 + page / 512].cpu->entries[page % 512]; }

static bool valid(const mapping_window_t *w, uint64_t first, uint64_t physical,
                  uint64_t pages, uint64_t flags)
{
    return w && w->ready && pages && first < MAPPING_WINDOW_PAGES &&
        pages <= MAPPING_WINDOW_PAGES - first && !(physical & 4095) &&
        physical < PHYSICAL_ADDRESS_LIMIT && pages <= (PHYSICAL_ADDRESS_LIMIT - physical) / 4096 &&
        (flags & PAGE_PRESENT) && !!(flags & PAGE_NX)==w->nx &&
        !(flags & ~(uint64_t)(PAGE_PRESENT | PAGE_RW | PAGE_PWT | PAGE_PCD | PAGE_PAT | PAGE_NX));
}

mapping_status_t mapping_window_install(mapping_window_t *w, uint64_t first,
    uint64_t physical, uint64_t pages, uint64_t flags)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_MAPPING);
    if (!valid(w, first, physical, pages, flags)) return MAPPING_INVALID;
    for (uint64_t i = 0; i < pages; ++i) {
        uint64_t old = *leaf(w, first + i), expected = (physical + i * 4096) | flags;
        if (old && (old & ~(uint64_t)(PAGE_ACCESSED | PAGE_DIRTY)) != expected)
            return MAPPING_CONFLICT;
    }
    for (uint64_t i = 0; i < pages; ++i) {
        PageEntry *entry = leaf(w, first + i);
        if (!*entry) __atomic_store_n(entry, (physical + i * 4096) | flags, __ATOMIC_RELEASE);
    }
    return MAPPING_OK;
}

mapping_status_t mapping_window_remove(mapping_window_t *w, uint64_t first,
    uint64_t physical, uint64_t pages, uint64_t flags)
{
    IRQ_LATENCY_SCOPE_MASK((UINT64_C(1) << IRQ_PATH_MAPPING) | (UINT64_C(1) << IRQ_PATH_UNMAP));
    if (!valid(w, first, physical, pages, flags)) return MAPPING_INVALID;
    for (uint64_t i = 0; i < pages; ++i)
        if ((*leaf(w, first + i) & ~(uint64_t)(PAGE_ACCESSED | PAGE_DIRTY)) !=
            ((physical + i * 4096) | flags)) return MAPPING_CONFLICT;
    for (uint64_t i = 0; i < pages; ++i)
        __atomic_store_n(leaf(w, first + i), 0, __ATOMIC_RELEASE);
    return MAPPING_OK;
}

bool mapping_window_entry(const mapping_window_t *w, uint64_t page, uint64_t *entry)
{
    if (!w || !w->ready || !entry || page >= MAPPING_WINDOW_PAGES) return false;
    *entry = w->tables[2 + page / 512].cpu->entries[page % 512];
    return true;
}
