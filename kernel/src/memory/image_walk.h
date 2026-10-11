#ifndef IMAGE_WALK_H
#define IMAGE_WALK_H
#include "image_permissions.h"
#include "paging_entry.h"
#include "mapping_core.h"
#include "stack_map.h"

#define IMAGE_WALK_TABLES 8192u
typedef struct {
    uint64_t address,physical,bytes,flags;
    unsigned shift;
    bool writable,nx;
} image_leaf_run_t;
typedef bool (*image_leaf_consumer_t)(void *,const image_leaf_run_t *);
typedef bool (*image_owner_consumer_t)(void *,const stack_owner_view_t *);
typedef const PageTable *(*image_table_reader_t)(void *,uint64_t,unsigned);
typedef struct {
    uint64_t root,tables,leaves,bytes,writable_bytes,executable_bytes,rwx_bytes;
    uint64_t image_bytes,direct_bytes,managed_bytes,scratch_bytes,stack_bytes;
    bool nx,complete;
} image_walk_result_t;
typedef struct {
    uint64_t frames[IMAGE_WALK_TABLES];
    uint64_t stack_sequences[STACK_OWNER_COUNT];
    uint8_t levels[IMAGE_WALK_TABLES],active[IMAGE_WALK_TABLES];
    unsigned count,visits;
    image_leaf_run_t pending;
} image_walk_workspace_t;
typedef struct {
    const image_permissions_t *permissions;
    const ram_map_plan_t *ram;
    const stack_layout_t *stacks;
    ram_extent_t scratch[2];
    image_table_reader_t table;
    image_leaf_consumer_t consume;
    image_owner_consumer_t owner_consume;
    void *context;
} image_walk_inputs_t;

bool image_walk_main(const image_walk_inputs_t *inputs,uint64_t root,
                     image_walk_workspace_t *workspace,image_walk_result_t *out);
/* Read-only production snapshot; a concurrent mapping change fails the scan. */
bool paging_image_snapshot(image_walk_result_t *out,image_leaf_consumer_t consume,void *context);
bool paging_image_snapshot_owners(image_walk_result_t *out,image_leaf_consumer_t consume,
                                 image_owner_consumer_t owners,void *context);
bool paging_image_target(uint64_t linked,uint64_t bytes,image_role_t role,
                         uint64_t *physical,uint64_t *direct,bool *nx);
const image_permissions_t *paging_image_plan(void);
bool paging_image_boot_check(const char *stage);
#endif
