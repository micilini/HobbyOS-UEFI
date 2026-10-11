#ifndef PAGING_ENTRY_H
#define PAGING_ENTRY_H
#include "ram_map_plan.h"

typedef struct {
    uint64_t physical, bytes;
    uint8_t level, pat_slot;
    bool leaf, writable, user, nx;
} paging_entry_view_t;
/* Levels are 4=PML4, 3=PDPT, 2=PD, 1=PT. Absent entries must be zero.
 * Hardware A/D bits are accepted; software, global, protection-key and reserved
 * bits are not part of this boot construction contract. */
bool paging_entry_encode(const ram_features_t *features, unsigned level, bool leaf,
                         uint64_t physical, uint64_t cache_bits, bool writable,
                         bool nx, uint64_t *entry);
bool paging_entry_decode(const ram_features_t *features, unsigned level,
                         uint64_t entry, paging_entry_view_t *view);
#endif
