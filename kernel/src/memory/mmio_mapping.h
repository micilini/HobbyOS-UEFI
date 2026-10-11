#ifndef MMIO_MAPPING_H
#define MMIO_MAPPING_H
#include "owned_mapping.h"

typedef enum {
    MMIO_FRAMEBUFFER = 1, MMIO_LAPIC, MMIO_IOAPIC, MMIO_HPET,
    MMIO_WATCHDOG, MMIO_PCI_ECAM, MMIO_PCI_BAR
} mmio_origin_kind_t;
typedef struct {
    mmio_origin_kind_t kind;
    uint64_t physical, bytes, source, generation;
} mmio_origin_t;

/* Resolve an exact retained BootInfo/controller/table/PCI source identity.
 * There is no public arbitrary-range registration. Sources are immutable for
 * this boot; generation binds a claim to the physical-boundary epoch. */
bool mmio_origin_resolve(mmio_origin_kind_t kind, uint64_t source, mmio_origin_t *out);
bool mmio_origin_valid(const mmio_origin_t *origin);

/* origin describes a discovered BootInfo/ACPI/PCI extent. It cannot authorize
 * RAM, linked-image or firmware-table storage as registers. UC/UC-/WC only;
 * every page-rounded byte is validated before a cache-alias transition.
 * First acquisition must precede any consumer of the device's old alias.
 * Keep a stable handle for repeated register access, including IRQ paths.
 */
#if defined(HOBBYOS_SELFTEST) && defined(HOBBYOS_FRAMEBUFFER_REFERENCE)
mapping_status_t mmio_framebuffer_reference(const mmio_origin_t *origin, mapping_view_t *out);
#endif
mapping_status_t mmio_mapping_acquire(const mmio_origin_t *origin,
    cache_attribute_t cache, mapping_view_t *out);
#endif
