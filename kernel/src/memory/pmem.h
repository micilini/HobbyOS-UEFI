#ifndef PMEM_H
#define PMEM_H

#include <stdint.h>
#include <stddef.h>
#include "../../../shared/mem_map.h"

#define PAGE_SIZE 4096

void init_pmm(MemoryMap *memory_map);

void *pmm_alloc_frame();

void *pmm_alloc_contiguous_frames(size_t count);

void pmm_free_frame(void *paddr);

uint64_t pmm_get_free_memory();
uint64_t pmm_get_total_memory();

#endif