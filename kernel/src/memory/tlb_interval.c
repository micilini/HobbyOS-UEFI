#include "tlb_interval.h"

static bool canonical(uint64_t address)
{
    return address <= UINT64_C(0x7fffffffffff) ||
           address >= UINT64_C(0xffff800000000000);
}

bool tlb_interval_normalize(uint64_t start, uint64_t length, tlb_interval_t *out)
{
    if (!out || !canonical(start)) return false;
    if (!length) {
        *out = (tlb_interval_t){0};
        return true;
    }
    if (length - 1 > UINT64_MAX - start) return false;
    uint64_t end = start + length - 1;
    if (!canonical(end) || ((start ^ end) & (UINT64_C(1) << 47))) return false;
    uint64_t first = start & ~UINT64_C(4095), last = end & ~UINT64_C(4095);
    uint64_t pages = ((last - first) >> 12) + 1;
    *out = (tlb_interval_t){first, last, pages,
        pages <= TLB_SMALL_PAGE_LIMIT ? TLB_PAGES : TLB_CONTEXT};
    return true;
}

bool tlb_interval_valid(const tlb_interval_t *i)
{
    if (!i) return false;
    if (!i->pages) return !i->first && !i->last && i->strategy == TLB_EMPTY;
    if ((i->first & 4095) || (i->last & 4095) || i->last < i->first ||
        !canonical(i->first) || !canonical(i->last) ||
        ((i->first ^ i->last) & (UINT64_C(1) << 47))) return false;
    return i->pages == ((i->last - i->first) >> 12) + 1 &&
        i->strategy == (i->pages <= TLB_SMALL_PAGE_LIMIT ? TLB_PAGES : TLB_CONTEXT);
}

bool tlb_local_execute(const tlb_interval_t *i, uint64_t root,
                       const tlb_local_ops_t *ops, void *context)
{
    if (!tlb_interval_valid(i) || !root || (root & 4095) ||
        (root & ~UINT64_C(0x000ffffffffff000)) || !ops || !ops->read_cr3 ||
        !ops->read_cr4 || !ops->invalidate_page || !ops->reload_cr3) return false;
    uint64_t cr3 = ops->read_cr3(context), cr4 = ops->read_cr4(context);
    /* PAE only: no PCID, global translations, or five-level paging. The root
     * address and permitted PWT/PCD flags are retained exactly on reload. */
    if (cr4 != 0x20 || (cr3 & ~UINT64_C(0x18)) != root) return false;
    if (i->strategy == TLB_CONTEXT) ops->reload_cr3(context, cr3);
    else for (uint64_t page = 0; page < i->pages; ++page)
        ops->invalidate_page(context, i->first + page * 4096);
    return true;
}
