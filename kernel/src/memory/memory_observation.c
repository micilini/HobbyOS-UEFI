#include "memory_observation.h"
#include "paging_inspect.h"
#include "boot_address.h"
#include "../cpu/cpu.h"
#include "../core/idt.h"

static memory_boot_observation_t g_boot;

void memory_observation_init(const BootInfo *boot)
{
    /* Retain original physical identities; the adapted map is a kernel-owned
     * byte copy. PMM does not reclaim the source UEFI LoaderData handoff. */
    if (!boot || !boot->memory_map || !boot->framebuffer) return;
    const MemoryMap *map = boot->memory_map;
    const boot_physical_objects_t *objects = physical_boot_objects();
    if (!objects) return;
    g_boot.base = objects->map;
    g_boot.bytes = map->Size;
    g_boot.stride = map->DescriptorSize;
    g_boot.framebuffer = objects->framebuffer;
    g_boot.framebuffer_bytes = boot->framebuffer->BufferSize;
    g_boot.rsdp = objects->rsdp;
    if (!memory_map_shape(g_boot.base, g_boot.bytes, g_boot.stride, &g_boot.count)) return;
    for (uint64_t i = 0; i < g_boot.count; i++)
    {
        memory_descriptor_t descriptor;
        if (!memory_descriptor_decode((const uint8_t *)map->Base, map->Size,
                                        map->DescriptorSize, i, &descriptor)) return;
        if (descriptor.type == EFI_LOADER_DATA && descriptor.range_valid &&
            g_boot.base >= descriptor.physical &&
            g_boot.base + g_boot.bytes <= descriptor.end) g_boot.valid = true;
    }
}

const memory_boot_observation_t *memory_observation_boot(void) { return &g_boot; }

bool memory_observation_bytes(uint64_t index, uint32_t offset, uint8_t *out, uint32_t size)
{
    if (!g_boot.valid || index >= g_boot.count || offset > g_boot.stride ||
        size > g_boot.stride - offset) return false;
    return paging_inspect_copy_physical(g_boot.base + index * g_boot.stride + offset,
                                        out, size);
}

bool memory_observation_descriptor(uint64_t index, memory_descriptor_t *out)
{
    uint8_t prefix[40];
    if (!memory_observation_bytes(index, 0, prefix, sizeof(prefix))) return false;
    return memory_descriptor_decode(prefix, sizeof(prefix), sizeof(prefix), 0, out);
}

void memory_observation_mtrr(memory_mtrr_observation_t *out)
{
    *out = (memory_mtrr_observation_t){0};
    irq_flags_t flags = irq_save();
    cpu_control_read_local(&out->cpu);
    out->supported = (out->cpu.cpuid1_edx & (1u << 12)) != 0 &&
                     (out->cpu.cpuid1_edx & (1u << 5)) != 0;
    if (out->supported)
    {
        out->capability_valid = cpu_read_msr_safe(0xfe, &out->capability);
        out->defaults_valid = cpu_read_msr_safe(0x2ff, &out->defaults);
        if (out->capability_valid)
        {
            out->count = (uint8_t)out->capability;
            for (uint32_t i = 0; i < out->count; i++)
            {
                out->pairs[i].base_valid = cpu_read_msr_safe(0x200 + i * 2,
                                                            &out->pairs[i].base);
                out->pairs[i].mask_valid = cpu_read_msr_safe(0x201 + i * 2,
                                                            &out->pairs[i].mask);
            }
        }
    }
    irq_restore(flags);
}
