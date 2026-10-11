#ifndef STACK_MAP_H
#define STACK_MAP_H
#include "stack_domain.h"
#include "mapping_core.h"
#include "image_permissions.h"
#include "paging_entry.h"

#define STACK_WINDOW_PTS (STACK_REGION_BYTES / UINT64_C(0x200000))
#define STACK_WINDOW_TABLES (STACK_WINDOW_PTS + 2u)
typedef struct {
    stack_owner_record_t *owners;
    uint64_t borrowed_ist[3];
    uint64_t bsp_guard;
} stack_layout_t;
typedef struct {
    mapping_table_t tables[STACK_WINDOW_TABLES];
    ram_features_t features;
    uint32_t allocated;
    bool ready;
} stack_window_t;

bool stack_backing_allowed(const stack_layout_t *layout, const image_permissions_t *image,
                           const ram_map_plan_t *ram, const stack_owner_view_t *owner);
bool stack_layout_expected(const stack_layout_t *layout, const image_permissions_t *image,
                           const ram_map_plan_t *ram, uint64_t *bytes);
bool stack_layout_leaf(const stack_layout_t *layout, const image_permissions_t *image,
                       const ram_map_plan_t *ram, uint64_t address, uint64_t physical,
                       uint64_t bytes, bool writable, bool nx);
bool stack_window_construct(stack_window_t *window, PageTable *root,
                            const ram_features_t *features,
                            const mapping_table_ops_t *ops, void *context);
bool stack_window_install(stack_window_t *window, const stack_owner_view_t *owner);
bool stack_window_remove(stack_window_t *window, const stack_owner_view_t *owner);
bool stack_window_check(const stack_window_t *window, const PageTable *root,
                        const stack_owner_view_t *owner);
bool stack_window_verify(const stack_window_t *window, const PageTable *root,
                         const stack_layout_t *layout, const image_permissions_t *image,
                         const ram_map_plan_t *ram);
#endif
