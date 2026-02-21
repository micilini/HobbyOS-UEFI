#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>
#include <stdbool.h>
#include "pmem.h"

#define PAGE_PRESENT (1 << 0)
#define PAGE_RW (1 << 1)
#define PAGE_USER (1 << 2)
#define PAGE_PWT (1 << 3)
#define PAGE_PCD (1 << 4)
#define PAGE_ACCESSED (1 << 5)
#define PAGE_DIRTY (1 << 6)
#define PAGE_HUGE (1 << 7)
#define PAGE_PAT (1 << 7)
#define PAGE_GLOBAL (1 << 8)
#define PAGE_NX (1ULL << 63)

#define PAGE_ADDR_MASK 0x000FFFFFFFFFF000

typedef uint64_t PageEntry;

typedef struct
{
    PageEntry entries[512];
} __attribute__((aligned(4096))) PageTable;

void init_paging(uint64_t fb_base, uint64_t fb_size);
void paging_map(uint64_t vaddr, uint64_t paddr, uint64_t flags);
void paging_load_map(PageTable *pml4);

uint64_t paging_get_physical_address(uint64_t vaddr);

PageTable *paging_create_bootstrap_table();

#endif