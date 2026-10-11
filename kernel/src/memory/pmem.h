#ifndef PMEM_H
#define PMEM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../../shared/mem_map.h"

#define PAGE_SIZE 4096

typedef struct PmmHardeningStats
{
    uint64_t reject_null;
    uint64_t reject_unaligned;
    uint64_t reject_out_of_span;
    uint64_t reject_reserved;
    uint64_t reject_already_free;
    uint64_t word_search_calls;
    uint64_t word_iterations;
} PmmHardeningStats;

void init_pmm(MemoryMap *memory_map);
struct ram_map_plan;
/* Boot-only: validate storage coverage before CR3, then rebase both bitmap
 * owners and publish reachability in the interrupt-disabled BSP transition. */
bool pmm_direct_map_ready(const struct ram_map_plan *plan);
bool pmm_direct_map_activate(const struct ram_map_plan *plan);
bool pmm_reserve_boot_framebuffer(void);

/* Compatibility representation of numeric physical addresses, never CPU
 * pointers. Returned extents are wholly below the published reachability ceiling.
 * Free takes that same physical representation, not a translated alias. */
void *pmm_alloc_frame(void);

void *pmm_alloc_contiguous_frames(size_t count);
/* Physical run, bounded by an exclusive page-aligned limit and the immutable
 * reachability ceiling. Failure changes neither output nor allocation state. */
bool pmm_alloc_constrained_frames(size_t count, size_t alignment_frames,
                                  uint64_t exclusive_limit, uint64_t *physical);

void pmm_free_frame(void *paddr);

bool pmm_is_frame_free(uint64_t physical_address);
void pmm_mark_frame_used(uint64_t physical_address);

uint64_t pmm_get_free_memory(void);
uint64_t pmm_get_total_memory(void);

bool pmm_get_hardening_stats(PmmHardeningStats *out_stats);
bool pmm_validate_integrity(void);

typedef struct {
    uint64_t represented_frames, free_frames, eligible_free_frames;
    uint64_t above_ceiling_free_frames, ceiling, minimum;
    uint64_t bitmap_physical, bitmap_bytes;
} PmmReachability;
/* Read-only coherent accounting. The original free count covers the entire
 * represented span; eligible excludes the existing low reservation and ceiling. */
bool pmm_get_reachability(PmmReachability *out);

#ifdef HOBBYOS_SELFTEST
typedef struct {
    uint64_t physical, bytes, bitmap_owner, reservation_owner, bitmap_member, reservation_member;
} PmmPointerSnapshot;
bool pmm_pointer_snapshot(PmmPointerSnapshot *out);
void *pmm_test_reserved_frame(void);
bool pmm_test_alloc_range(size_t count, size_t alignment_frames,
                          uint64_t minimum, uint64_t exclusive_limit, uint64_t *physical);
#endif

#endif
