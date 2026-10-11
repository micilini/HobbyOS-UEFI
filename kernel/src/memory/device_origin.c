#include "mmio_mapping.h"
#include "physical_internal.h"
#include "boot_address.h"
#include "mapping_core.h"
#include "../acpi/acpi.h"
#include "../acpi/madt.h"
#include "../apic/lapic.h"
#include "../timer/hpet.h"
#include "../drivers/pci.h"
#include "../drivers/watchdog/acpi_wdat.h"

/* Discovery queries only read retained facts. In particular they must not call
 * acpi_find_table: a failed admission cannot acquire a new firmware mapping. */
bool mmio_origin_resolve(mmio_origin_kind_t kind, uint64_t source, mmio_origin_t *out)
{
    if (!out || !g_physical_contract.ready) return false;
    mmio_origin_t origin = {.kind=kind, .source=source, .generation=g_physical_contract.epoch};
    const AcpiSdtHeader *header;
    uint64_t physical;
    switch (kind) {
    case MMIO_FRAMEBUFFER: {
        const boot_physical_objects_t *boot = physical_boot_objects();
        if (!boot || source != boot->framebuffer_descriptor) return false;
        origin.physical = boot->framebuffer; origin.bytes = boot->framebuffer_bytes; break;
    }
    case MMIO_LAPIC:
        if (!lapic_discovered_extent(source, &origin.physical, &origin.bytes)) return false;
        break;
    case MMIO_IOAPIC: {
        madt_ioapic_t controller;
        if (!source || source > madt_get_ioapic_count() || !madt_ioapic_at(source-1, &controller)) return false;
        origin.physical = controller.mmio_base; origin.bytes = 0x14; break;
    }
    case MMIO_HPET: {
        header = acpi_retained_table(ACPI_SIG_HPET, &physical);
        if (!header || physical != source || header->length < sizeof(HpetTable)) return false;
        const HpetTable *hpet = (const HpetTable *)header;
        if (hpet->address_space_id != 0 || hpet->register_bit_offset) return false;
        origin.physical = hpet->address; origin.bytes = 0x120; break;
    }
    case MMIO_WATCHDOG:
        if (!acpi_wdat_discovered_extent(source, &origin.physical, &origin.bytes)) return false;
        break;
    case MMIO_PCI_ECAM: {
        header = acpi_retained_table(ACPI_SIG_MCFG, &physical);
        if (!header || header->length < sizeof(McfgHeader) || source < physical + sizeof(McfgHeader)) return false;
        uint64_t offset = source - physical;
        if ((offset-sizeof(McfgHeader)) % sizeof(McfgDeviceConfig) || offset > header->length ||
            sizeof(McfgDeviceConfig) > header->length-offset) return false;
        const McfgDeviceConfig *config = (const void *)((const uint8_t *)header + offset);
        if (config->StartBus > config->EndBus || (config->BaseAddress & ((1u << 20)-1))) return false;
        uint64_t first_bus = (uint64_t)config->StartBus << 20;
        if (config->BaseAddress > UINT64_MAX-first_bus) return false;
        origin.physical = config->BaseAddress + first_bus;
        origin.bytes = ((uint64_t)config->EndBus-config->StartBus+1) << 20; break;
    }
    case MMIO_PCI_BAR:
        if (!pci_discovered_xhci_extent(source, &origin.physical, &origin.bytes)) return false;
        break;
    default: return false;
    }
    mapping_range_t range;
    if (!origin.physical || !mapping_range_checked(origin.physical, origin.bytes, &range) ||
        !physical_device_extent_allowed(range.first, range.pages*4096)) return false;
    *out = origin; return true;
}

bool mmio_origin_valid(const mmio_origin_t *origin)
{
    mmio_origin_t discovered;
    mapping_range_t requested, backing;
    if (!origin || !origin->generation || !mmio_origin_resolve(origin->kind, origin->source, &discovered) ||
        discovered.generation != origin->generation || origin->physical < discovered.physical ||
        origin->physical-discovered.physical >= discovered.bytes || !origin->bytes ||
        origin->bytes > discovered.bytes-(origin->physical-discovered.physical) ||
        !mapping_range_checked(origin->physical, origin->bytes, &requested) ||
        !mapping_range_checked(discovered.physical, discovered.bytes, &backing)) return false;
    return requested.first >= backing.first && requested.first+requested.pages*4096 <= backing.first+backing.pages*4096;
}
