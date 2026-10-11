#include "physical_internal.h"
#include "ram_map_plan.h"
#include "../libc/memory.h"

bool physical_range_valid(uint64_t first, uint64_t bytes, uint64_t *end)
{
    if (!end || !bytes || first >= PHYSICAL_ADDRESS_LIMIT ||
        bytes > PHYSICAL_ADDRESS_LIMIT - first) return false;
    *end = first + bytes;
    return true;
}

bool physical_canonical_range(uint64_t first, uint64_t bytes)
{
    if (!bytes || bytes - 1 > UINT64_MAX - first) return false;
    uint64_t last = first + bytes - 1;
    return (first < (UINT64_C(1) << 47) && last < (UINT64_C(1) << 47)) ||
        (first >= UINT64_C(0xffff800000000000) && last >= first);
}

static uint32_t region_kind(uint32_t type)
{
    if ((type >= EFI_LOADER_CODE && type <= EFI_CONVENTIONAL_MEMORY) ||
        type == EFI_ACPI_RECLAIM_MEMORY || type == EFI_ACPI_MEMORY_NVS)
        return PHYSICAL_RAM | PHYSICAL_FIRMWARE;
    if (type == EFI_RESERVED_MEMORY_TYPE) return PHYSICAL_FIRMWARE;
    if (type == EFI_MEMORY_MAPPED_IO) return PHYSICAL_DEVICE;
    return 0;
}

bool physical_contract_init(physical_contract_t *c, const MemoryMap *map,
                            uint64_t image_first, uint64_t image_end, uint64_t epoch)
{
    if (!c || c->ready || !map || !map->Base || !epoch ||
        map->DescriptorSize < sizeof(EfiMemoryDescriptor) ||
        map->DescriptorSize > 256 || !map->Size ||
        map->Size % map->DescriptorSize ||
        map->Size / map->DescriptorSize > PHYSICAL_DESCRIPTOR_CAPACITY ||
        (uintptr_t)map->Base > UINTPTR_MAX - (map->Size - 1) ||
        image_first < KERNEL_IMAGE_OFFSET || image_end <= image_first ||
        !physical_canonical_range(image_first, image_end - image_first) ||
        image_end - KERNEL_IMAGE_OFFSET > UINT64_C(0x40000000)) return false;
    uint64_t highest = 0;
    uint32_t count = (uint32_t)(map->Size / map->DescriptorSize);
    for (uint32_t i = 0; i < count; ++i) {
        EfiMemoryDescriptor d;
        memcpy(&d, (const uint8_t *)map->Base + i * map->DescriptorSize, sizeof(d));
        uint64_t end;
        if (!d.NumberOfPages || (d.PhysicalStart & 4095) ||
            d.NumberOfPages > UINT64_MAX / PHYSICAL_PAGE_BYTES ||
            !physical_range_valid(d.PhysicalStart, d.NumberOfPages * PHYSICAL_PAGE_BYTES, &end)) return false;
        /* Match the existing PMM represented span; Reserved is not free RAM. */
        if (d.Type <= EFI_ACPI_MEMORY_NVS && d.Type != EFI_UNUSABLE_MEMORY && end > highest)
            highest = end;
        for (uint32_t j = 0; j < i; ++j) {
            EfiMemoryDescriptor prior;
            memcpy(&prior, (const uint8_t *)map->Base + j * map->DescriptorSize, sizeof(prior));
            uint64_t prior_end = prior.PhysicalStart + prior.NumberOfPages * PHYSICAL_PAGE_BYTES;
            if (d.PhysicalStart < prior_end && prior.PhysicalStart < end) return false;
        }
    }
    uint64_t ceiling = highest < PHYSICAL_ENTRY_LIMIT ? highest : PHYSICAL_ENTRY_LIMIT;
    if (!ceiling || image_end - KERNEL_IMAGE_OFFSET > ceiling) return false;
    for (uint32_t i = 0; i < count; ++i) {
        EfiMemoryDescriptor d;
        memcpy(&d, (const uint8_t *)map->Base + i * map->DescriptorSize, sizeof(d));
        physical_region_t region = {d.PhysicalStart,
            d.PhysicalStart + d.NumberOfPages * PHYSICAL_PAGE_BYTES,
            d.Attribute, d.Type, region_kind(d.Type)};
        uint32_t at = i;
        while (at && c->regions[at-1].first > region.first) {
            c->regions[at] = c->regions[at-1]; --at;
        }
        c->regions[at] = region;
    }
    c->count = count; c->image_first = image_first; c->image_end = image_end;
    c->identity_limit = ceiling; c->allocation_ceiling = ceiling; c->epoch = epoch;
    c->ready = true;
    return true;
}

bool physical_contract_translate(const physical_contract_t *c,
    uint64_t physical, uint64_t bytes, physical_kind_t kind, uint64_t *virtual_address)
{
    uint64_t end;
    if (!c || !c->ready || !virtual_address ||
        (kind != PHYSICAL_KNOWN && kind != PHYSICAL_RAM &&
         kind != PHYSICAL_FIRMWARE && kind != PHYSICAL_DEVICE) ||
        !physical_range_valid(physical, bytes, &end)) return false;
    if (c->direct_map) {
        if (kind == PHYSICAL_DEVICE) return false;
        return ram_map_pointer(c->direct_map, physical, bytes, virtual_address);
    }
    if (end > c->identity_limit) return false;
    uint64_t cursor = physical;
    uint32_t lo = 0, hi = c->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi-lo)/2;
        if (c->regions[mid].end <= cursor) lo = mid+1; else hi = mid;
    }
    for (uint32_t i = lo; cursor < end && i < c->count; ++i) {
        const physical_region_t *r = &c->regions[i];
        if (r->first > cursor || r->end <= cursor || !r->kind ||
            (kind && !(r->kind & kind))) return false;
        cursor = r->end < end ? r->end : end;
    }
    if (cursor != end) return false;
    *virtual_address = physical;
    return true;
}

bool physical_contract_inverse(const physical_contract_t *c,
    uint64_t virtual_address, uint64_t bytes, uint64_t *physical)
{
    if (!c || !c->ready || !physical || !physical_canonical_range(virtual_address, bytes)) return false;
    if (virtual_address >= c->image_first && virtual_address < c->image_end &&
        bytes <= c->image_end - virtual_address) {
        *physical = virtual_address - KERNEL_IMAGE_OFFSET;
        return true;
    }
    if (c->direct_map) return ram_map_inverse(c->direct_map, virtual_address, bytes, physical);
    return physical_contract_translate(c, virtual_address, bytes, PHYSICAL_KNOWN, physical);
}

bool physical_contract_borrow(const physical_contract_t *c,
    const physical_borrow_t *borrow, physical_kind_t kind, uint64_t *virtual_address)
{
    return c && borrow && borrow->epoch == c->epoch &&
        physical_contract_translate(c, borrow->physical, borrow->bytes, kind, virtual_address);
}
