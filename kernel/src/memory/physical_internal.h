#ifndef PHYSICAL_INTERNAL_H
#define PHYSICAL_INTERNAL_H
#include "physical_translation.h"
#include "physical_layout.h"
#include "../../../shared/mem_map.h"

#define PHYSICAL_DESCRIPTOR_CAPACITY 1024u
#define PHYSICAL_DEVICE_CAPACITY 256u
typedef struct {
    uint64_t first, end, attributes;
    uint32_t type, kind;
} physical_region_t;
typedef struct {
    physical_region_t regions[PHYSICAL_DESCRIPTOR_CAPACITY];
    uint64_t image_first, image_end, identity_limit, allocation_ceiling, epoch;
    uint32_t count;
    bool ready, identity_retired;
    const struct ram_map_plan *direct_map;
} physical_contract_t;
typedef struct { uint64_t physical, bytes, epoch; } physical_borrow_t;

/* Pure decisions shared by the actual boundary and hosted tests. These never
 * dereference an output address. The supplied descriptor buffer is CPU memory. */
bool physical_contract_init(physical_contract_t *contract, const MemoryMap *map,
                            uint64_t image_first, uint64_t image_end, uint64_t epoch);
bool physical_contract_translate(const physical_contract_t *contract,
    uint64_t physical, uint64_t bytes, physical_kind_t kind, uint64_t *virtual_address);
bool physical_contract_inverse(const physical_contract_t *contract,
    uint64_t virtual_address, uint64_t bytes, uint64_t *physical);
bool physical_contract_borrow(const physical_contract_t *contract,
    const physical_borrow_t *borrow, physical_kind_t kind, uint64_t *virtual_address);
bool physical_range_valid(uint64_t first, uint64_t bytes, uint64_t *end);
bool physical_canonical_range(uint64_t first, uint64_t bytes);

/* Private boot/memory integration. No public unchecked mode exists. */
extern physical_contract_t g_physical_contract;
bool physical_translation_publish(const MemoryMap *map, uint64_t image_first,
                                  uint64_t image_end);
bool physical_translation_activate(uint64_t limit);
bool physical_translation_activate_direct(const struct ram_map_plan *plan);
bool physical_translation_retire_identity(void);
void physical_translation_mapping(uint64_t virtual_address, uint64_t physical,
                                  uint64_t flags);
bool physical_provenance_range(uint64_t first, uint64_t bytes, physical_kind_t kind);
bool physical_device_extent_allowed(uint64_t first, uint64_t bytes);
bool physical_identity_hole(uint64_t first, uint64_t bytes);
bool physical_identity_range_present(uint64_t first, uint64_t bytes);
#endif
