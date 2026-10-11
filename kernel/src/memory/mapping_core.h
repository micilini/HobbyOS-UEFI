#ifndef MAPPING_CORE_H
#define MAPPING_CORE_H
#include "owned_mapping.h"
#include "paging_private.h"

#define MAPPING_WINDOW_TABLES 514u
#define MAPPING_WINDOW_PAGES (DEVICE_WINDOW_BYTES / 4096u)
typedef struct { uint64_t first, pages, offset, bytes; } mapping_range_t;
typedef struct { PageTable *cpu; uint64_t physical; } mapping_table_t;
typedef struct {
    mapping_table_t tables[MAPPING_WINDOW_TABLES];
    uint32_t allocated;
    bool ready, nx;
} mapping_window_t;
typedef struct {
    bool (*allocate)(void *, mapping_table_t *);
    void (*release)(void *, mapping_table_t);
} mapping_table_ops_t;

bool mapping_range_checked(uint64_t physical, uint64_t bytes, mapping_range_t *out);
bool dma_extent_limit(uint64_t bytes, uint64_t alignment, uint64_t maximum_address,
    uint64_t reachable_ceiling, mapping_range_t *out, uint64_t *exclusive_limit);
mapping_status_t mapping_window_construct(mapping_window_t *window, PageTable *root,
    const mapping_table_ops_t *ops, void *context, bool nx);
mapping_status_t mapping_window_install(mapping_window_t *window, uint64_t first_page,
    uint64_t physical, uint64_t pages, uint64_t flags);
mapping_status_t mapping_window_remove(mapping_window_t *window, uint64_t first_page,
    uint64_t physical, uint64_t pages, uint64_t flags);
bool mapping_window_entry(const mapping_window_t *window, uint64_t page, uint64_t *entry);
#endif
