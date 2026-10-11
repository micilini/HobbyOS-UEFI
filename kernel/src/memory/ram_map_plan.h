#ifndef RAM_MAP_PLAN_H
#define RAM_MAP_PLAN_H
#include "physical_internal.h"

#define RAM_MAP_INTERVALS 4096u
#define RAM_MTRR_VARIABLES 256u
typedef struct { uint64_t first, end; } ram_extent_t;
typedef struct {
    uint64_t capability, defaults, fixed[11];
    uint64_t base[RAM_MTRR_VARIABLES], mask[RAM_MTRR_VARIABLES];
    uint32_t count;
    bool available;
} ram_mtrr_inputs_t;
typedef struct {
    uint32_t extended_max, extended_edx, address_eax;
    uint8_t physical_bits;
    bool nx, page1g;
} ram_features_t;
typedef struct {
    uint64_t first, end, attributes;
    uint32_t type;
    uint8_t maximum_shift;
} ram_interval_t;
typedef struct ram_map_plan {
    ram_interval_t intervals[RAM_MAP_INTERVALS];
    ram_extent_t cache_ranges[RAM_MTRR_VARIABLES];
    ram_extent_t framebuffer;
    ram_features_t features;
    uint64_t ceiling, bytes, image_first, image_end;
    uint32_t count, cache_count;
    bool ready;
} ram_map_plan_t;

/* Pure, bounded construction. Runtime-services RAM remains reserved by PMM;
 * mapping it does not authorize firmware execution or reclamation. Only the
 * original BootInfo framebuffer envelope is an early device exclusion. */
bool ram_features_decode(uint32_t maximum, uint32_t extended_edx,
                         uint32_t address_eax, bool nx_policy, ram_features_t *out);
bool ram_map_plan_create(ram_map_plan_t *plan, const physical_contract_t *contract,
                        const ram_features_t *features, const ram_mtrr_inputs_t *mtrrs,
                        uint64_t framebuffer, uint64_t framebuffer_bytes);
bool ram_map_plan_verify(const ram_map_plan_t *plan, const physical_contract_t *contract,
                        const ram_mtrr_inputs_t *mtrrs, ram_extent_t authorized_framebuffer);
bool ram_map_contains(const ram_map_plan_t *plan, uint64_t first, uint64_t bytes);
bool ram_map_pointer(const ram_map_plan_t *plan, uint64_t first, uint64_t bytes,
                     uint64_t *pointer);
bool ram_map_inverse(const ram_map_plan_t *plan, uint64_t pointer, uint64_t bytes,
                     uint64_t *physical);
const ram_interval_t *ram_map_interval(const ram_map_plan_t *plan, uint64_t physical);
unsigned ram_map_leaf_shift(const ram_map_plan_t *plan, const ram_interval_t *interval,
                           uint64_t physical);
#endif
