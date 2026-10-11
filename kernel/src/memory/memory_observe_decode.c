#include "memory_observe_decode.h"

bool memory_map_shape(uint64_t base, uint64_t bytes, uint64_t stride,
                       uint64_t *count)
{
    if (!base || !count || stride < 40 || stride > 4096 || !bytes ||
        bytes > 1024 * 1024 || bytes % stride || base > UINT64_MAX - bytes)
        return false;
    *count = bytes / stride;
    return *count <= 4096;
}

static uint64_t little(const uint8_t *data, unsigned size)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < size; i++) value |= (uint64_t)data[i] << (8 * i);
    return value;
}

bool memory_descriptor_decode(const uint8_t *data, uint64_t bytes,
                               uint64_t stride, uint64_t index,
                               memory_descriptor_t *out)
{
    uint64_t count;
    if (!data || !out || !memory_map_shape((uintptr_t)data, bytes, stride, &count) ||
        index >= count) return false;
    const uint8_t *entry = data + index * stride;
    *out = (memory_descriptor_t){.type = (uint32_t)little(entry, 4),
        .physical = little(entry + 8, 8), .virtual_address = little(entry + 16, 8),
        .pages = little(entry + 24, 8), .attributes = little(entry + 32, 8)};
    if ((out->physical & 4095) || !out->pages || out->pages > UINT64_MAX / 4096 ||
        out->physical > UINT64_MAX - out->pages * 4096) return true;
    out->end = out->physical + out->pages * 4096;
    out->range_valid = true;
    return true;
}

bool memory_interval_overlap(uint64_t a, uint64_t end_a,
                              uint64_t b, uint64_t end_b,
                              uint64_t *start, uint64_t *end)
{
    if (a >= end_a || b >= end_b) return false;
    uint64_t low = a > b ? a : b, high = end_a < end_b ? end_a : end_b;
    if (low >= high) return false;
    if (start) *start = low;
    if (end) *end = high;
    return true;
}

const char *memory_efi_type(uint32_t type)
{
    static const char *const names[] = {"Reserved", "LoaderCode", "LoaderData",
        "BootServicesCode", "BootServicesData", "RuntimeServicesCode",
        "RuntimeServicesData", "ConventionalMemory", "UnusableMemory",
        "ACPIReclaimMemory", "ACPIMemoryNVS", "MemoryMappedIO",
        "MemoryMappedIOPortSpace", "PalCode", "PersistentMemory",
        "UnacceptedMemory"};
    return type < sizeof(names) / sizeof(names[0]) ? names[type] : "Unknown";
}

void memory_mtrr_decode(uint64_t base, uint64_t mask, unsigned bits,
                        bool supported, bool read_valid, memory_mtrr_range_t *out)
{
    *out = (memory_mtrr_range_t){.type = (uint8_t)base,
        .enabled = (mask & (1u << 11)) != 0, .reason = "unsupported"};
    if (!supported) return;
    out->reason = "read_failed";
    if (!read_valid) return;
    out->reason = "physical_width";
    if (bits < 32 || bits > 52) return;
    uint64_t maximum = (UINT64_C(1) << bits) - 1;
    uint64_t addresses = maximum & ~UINT64_C(4095);
    out->reason = "reserved_bits_or_type";
    if ((base & ~(addresses | UINT64_C(0xff))) ||
        (mask & ~(addresses | UINT64_C(0x800))) ||
        !(out->type == 0 || out->type == 1 || out->type == 4 ||
          out->type == 5 || out->type == 6)) return;
    if (!out->enabled) { out->valid = true; out->reason = "disabled"; return; }
    uint64_t size = ((~mask) & addresses) + 4096;
    out->reason = "noncontiguous_mask_or_alignment";
    if (!size || (size & (size - 1))) return;
    out->base = base & addresses;
    if (out->base & (size - 1)) return;
    if (out->base > maximum - (size - 1)) return;
    out->size = size;
    out->end = out->base + size;
    out->valid = true;
    out->reason = "decoded";
}
