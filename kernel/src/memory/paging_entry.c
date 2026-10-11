#include "paging_entry.h"

#define ENTRY_NX (UINT64_C(1) << 63)
static bool parameters(const ram_features_t *f, unsigned level)
{ return f && f->physical_bits >= 36 && f->physical_bits <= 52 && level >= 1 && level <= 4; }

bool paging_entry_encode(const ram_features_t *f, unsigned level, bool leaf,
                         uint64_t physical, uint64_t cache, bool writable,
                         bool nx, uint64_t *out)
{
    if (!out || !parameters(f, level) || (nx && !f->nx) || (leaf && level == 4) ||
        (!leaf && level == 1) || (leaf && level == 3 && !f->page1g)) return false;
    unsigned shift = leaf ? 12 + 9 * (level - 1) : 12;
    uint64_t bytes = UINT64_C(1) << shift;
    uint64_t allowed_cache = leaf ? UINT64_C(0x18) | (level == 1 ? UINT64_C(0x80) : UINT64_C(0x1000)) : 0;
    if (physical >= (UINT64_C(1) << f->physical_bits) || (physical & (bytes - 1)) ||
        (cache & ~allowed_cache) || (!leaf && !physical)) return false;
    uint64_t entry = physical | 1 | (writable ? 2 : 0) | cache | (nx ? ENTRY_NX : 0);
    if (leaf && level > 1) entry |= 128;
    *out = entry;
    return true;
}

bool paging_entry_decode(const ram_features_t *f, unsigned level, uint64_t entry,
                         paging_entry_view_t *out)
{
    if (!out || !parameters(f, level) || !(entry & 1)) return false;
    bool leaf = level == 1 || (entry & 128);
    if ((level == 4 && leaf) || (level == 3 && leaf && !f->page1g) ||
        ((entry & ENTRY_NX) && !f->nx)) return false;
    uint64_t address_mask = ((UINT64_C(1) << f->physical_bits) - 1) & ~UINT64_C(4095);
    uint64_t allowed = address_mask | UINT64_C(0x27) | ENTRY_NX;
    if (leaf) allowed |= UINT64_C(0x58) | (level == 1 ? UINT64_C(0x80) : UINT64_C(0x1080));
    if (entry & ~allowed) return false;
    unsigned shift = leaf ? 12 + 9 * (level - 1) : 12;
    uint64_t bytes = UINT64_C(1) << shift;
    uint64_t physical = entry & address_mask;
    unsigned slot = 0;
    if (leaf) {
        slot = ((entry >> 3) & 1) | ((entry >> 3) & 2) |
            ((entry >> (level == 1 ? 5 : 10)) & 4);
        if (level > 1) physical &= ~UINT64_C(0x1000);
    }
    if ((physical & (bytes - 1)) || (!leaf && !physical)) return false;
    *out = (paging_entry_view_t){physical, bytes, level, slot, leaf,
                                 (entry & 2) != 0, (entry & 4) != 0, (entry & ENTRY_NX) != 0};
    return true;
}
