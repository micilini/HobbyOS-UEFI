#ifndef PAGING_BUILD_H
#define PAGING_BUILD_H
#include "paging_entry.h"
#include "mapping_core.h"
#include "image_permissions.h"
#include "stack_map.h"

#define PAGING_BUILD_TABLES 8192u
typedef struct {
    mapping_table_t table;
    uint64_t verified_hash;
    uint8_t level;
    bool seen;
} paging_build_table_t;
typedef struct {
    paging_build_table_t tables[PAGING_BUILD_TABLES];
    mapping_window_t window;
    stack_window_t stacks;
    const stack_layout_t *stack_layout;
    mapping_table_ops_t ops;
    void *context;
    const ram_map_plan_t *plan;
    const image_permissions_t *permissions;
    uint64_t identity_limit, image_first, image_end, leaves[3], direct_bytes;
    uint32_t count, verified_tables;
    bool built, verified, activated;
} paging_build_t;
typedef struct { uint64_t address, bytes; bool executable, writable; } paging_required_path_t;

/* All frames are privately owned, reachable through the old root and below
 * the entry limit. No caller publishes this root before verify succeeds. */
bool paging_build_create(paging_build_t *build, const physical_contract_t *contract,
                         const ram_map_plan_t *plan, const image_permissions_t *permissions,
                         const stack_layout_t *stacks,
                         const mapping_table_ops_t *ops,
                         void *context);
void paging_build_abort(paging_build_t *build);
bool paging_build_verify(paging_build_t *build, const physical_contract_t *contract,
                         const ram_mtrr_inputs_t *mtrrs, ram_extent_t authorized_framebuffer,
                         const paging_required_path_t *paths, unsigned path_count);
bool paging_build_lookup(const paging_build_t *build, uint64_t address,
                         uint64_t *physical, bool *nx);
bool paging_build_rights(const paging_build_t *build, uint64_t address,
                        uint64_t *physical, bool *writable, bool *nx);
uint64_t paging_table_hash(const PageTable *table);
/* The only activation seam used by boot and hosted refusal tests. */
bool paging_build_activate(paging_build_t *build, void (*load)(void *, uint64_t), void *context);
#endif
