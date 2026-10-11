#ifndef PHYSICAL_TRANSLATION_H
#define PHYSICAL_TRANSLATION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t physical_address_t;
typedef enum {
    PHYSICAL_KNOWN = 0,
    PHYSICAL_RAM = 1,
    PHYSICAL_FIRMWARE = 2,
    PHYSICAL_DEVICE = 4
} physical_kind_t;
typedef enum {
    PHYSICAL_DEVICE_FRAMEBUFFER = 1,
    PHYSICAL_DEVICE_ACPI,
    PHYSICAL_DEVICE_PCI
} physical_device_origin_t;
typedef struct {
    uint64_t validations, rejected, identity_limit, allocation_ceiling;
    uint64_t epoch, descriptor_count, device_ranges;
    bool initialized, bootstrap_sealed, normal_mapping;
} physical_translation_stats_t;

/* Nonempty whole ranges only. Failure leaves outputs unchanged. Physical zero
 * is a valid numeric address, but cannot produce a dereferenceable C NULL.
 * Before activation, private boot owners use checked identity reachability.
 * After verified activation, ordinary RAM uses the direct map and complete
 * descriptor-derived coverage, without locks, allocation or a recursive walk.
 * Reserved firmware uses an owned WB/RO view. Existing image pointers
 * keep their linked VA; only the linked image admits image-offset inversion.
 * Arbitrary mapped aliases are queried through paging_query_range instead.
 * Returned pointers are borrowed within the current boundary epoch; owners
 * retain physical origins and rederive their complete objects at activation.
 */
bool physical_pointer_range(physical_address_t physical, size_t bytes,
                            physical_kind_t kind, void **pointer);
bool physical_address_range(const void *pointer, size_t bytes,
                            physical_address_t *physical);
bool physical_image_address_range(uint64_t virtual_address, size_t bytes,
                                  physical_address_t *physical);
bool physical_image_extent(physical_address_t *physical, size_t *bytes);
uint64_t physical_allocation_ceiling(void);
void physical_translation_stats(physical_translation_stats_t *out);

/* Borrows a validated device interval only after the existing mapping path has
 * established identity reachability for its entire extent. This does not map,
 * allocate, set cache flags or classify the interval as allocatable RAM.
 * The origin identifies the bounded BootInfo/ACPI/PCI consumer; it cannot bypass
 * arithmetic or actual page-table proof. Call before caching a register pointer.
 */
bool physical_device_pointer(physical_address_t physical, size_t bytes,
                             physical_device_origin_t origin, void **pointer);

/* Fail-closed adapters for established required objects, never error-as-zero. */
void *physical_require_pointer(physical_address_t physical, size_t bytes,
                               physical_kind_t kind);
physical_address_t physical_require_address(const void *pointer, size_t bytes);
#endif
