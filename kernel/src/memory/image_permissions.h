#ifndef IMAGE_PERMISSIONS_H
#define IMAGE_PERMISSIONS_H
#include "ram_map_plan.h"

typedef enum {
    IMAGE_TEXT, IMAGE_TRAMPOLINE, IMAGE_RODATA, IMAGE_MSR_FIXUP,
    IMAGE_DATA, IMAGE_BSS, IMAGE_BOOT_TEXT, IMAGE_BOOT_DATA, IMAGE_ROLE_COUNT
} image_role_t;
typedef struct { uint64_t first, end; image_role_t role; } image_section_t;
typedef struct {
    image_section_t sections[IMAGE_ROLE_COUNT];
    uint64_t boot_first, boot_end;
    uint64_t bsp_guard_first, bsp_guard_end, bsp_stack_first, bsp_stack_end;
} image_permission_input_t;
typedef struct {
    image_permission_input_t input;
    uint64_t first, end, linked_first, linked_end;
    bool ready;
} image_permissions_t;
typedef struct { bool writable, executable; } image_rights_t;

/* All section origins are physical. Only the checked image alias executes. */
bool image_permissions_create(image_permissions_t *out, const physical_contract_t *contract,
                              const image_permission_input_t *input);
bool image_permissions_verify(const image_permissions_t *plan, const physical_contract_t *contract);
bool image_permissions_rights(const image_permissions_t *plan, uint64_t physical,
                              uint64_t bytes, bool linked, image_rights_t *out);
bool image_permissions_check_leaf(const image_permissions_t *plan, uint64_t address,
                                 uint64_t physical, uint64_t bytes, bool writable,
                                 bool nx, bool nx_supported);
bool image_permissions_overlaps(const image_permissions_t *plan, uint64_t physical, uint64_t bytes);
unsigned image_permissions_leaf_shift(const image_permissions_t *plan, const ram_map_plan_t *ram,
                                     const ram_interval_t *interval, uint64_t physical);
bool image_permissions_from_linker(image_permissions_t *out, const physical_contract_t *contract);
bool image_permissions_linked_range(const image_permissions_t *plan, uint64_t address, uint64_t bytes);
uint64_t image_permissions_linked_bytes(const image_permissions_t *plan);
#endif
