#ifndef PAGING_PRIVATE_H
#define PAGING_PRIVATE_H
#include "paging.h"
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


void paging_map(uint64_t vaddr, uint64_t paddr, uint64_t flags);
bool paging_query_range(uint64_t virtual_address, size_t bytes, uint64_t *physical);
uint64_t paging_get_physical_address(uint64_t vaddr);

/* Only memory-owned views can reach these operations. No public force mode. */
bool paging_owned_initialize(void);
bool paging_owned_extent_allowed(uint64_t physical, uint64_t bytes);
bool paging_owned_install(uint64_t first, uint64_t physical, uint64_t pages, uint64_t flags);
bool paging_owned_remove(uint64_t first, uint64_t physical, uint64_t pages, uint64_t flags);
bool paging_owned_withdraw_identity(uint64_t physical, uint64_t bytes);
#endif
