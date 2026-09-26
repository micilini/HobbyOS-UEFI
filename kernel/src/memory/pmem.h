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

void *pmm_alloc_frame(void);

void *pmm_alloc_contiguous_frames(size_t count);

void pmm_free_frame(void *paddr);

bool pmm_is_frame_free(uint64_t physical_address);
void pmm_mark_frame_used(uint64_t physical_address);

uint64_t pmm_get_free_memory(void);
uint64_t pmm_get_total_memory(void);

bool pmm_get_hardening_stats(PmmHardeningStats *out_stats);
bool pmm_validate_integrity(void);

#ifdef HOBBYOS_SELFTEST
void *pmm_test_reserved_frame(void);
#endif

#endif
