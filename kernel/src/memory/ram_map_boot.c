#include "ram_map_boot.h"
#include "cache_attributes.h"
#include "../cpu/cpu.h"

bool ram_map_boot_inputs(ram_features_t *features, ram_mtrr_inputs_t *mtrrs)
{
    if (!features || !mtrrs) return false;
    const cpu_policy_t *policy = cpu_policy_get();
    uint32_t maximum, edx, width, basic;
    if (!policy || !cpu_get_cpuid_count(1, 0, NULL, NULL, NULL, &basic) ||
        !(basic & (1u << 12)) ||
        !cpu_get_cpuid_count(0x80000000, 0, &maximum, NULL, NULL, NULL) ||
        !cpu_get_cpuid_count(0x80000001, 0, NULL, NULL, NULL, &edx) ||
        !cpu_get_cpuid_count(0x80000008, 0, &width, NULL, NULL, NULL) ||
        !ram_features_decode(maximum, edx, width, (policy->efer_write & CPU_EFER_NXE) != 0, features)) return false;
    for (unsigned shift = 12; shift <= 30; shift += 9) {
        if (shift == 30 && !features->page1g) continue;
        uint64_t bits;
        if (!cache_attribute_encode(policy, CACHE_WRITE_BACK, (cache_leaf_t)shift, &bits) || bits) return false;
    }
    if (!cpu_read_msr_safe(0xfe, &mtrrs->capability) ||
        !cpu_read_msr_safe(0x2ff, &mtrrs->defaults)) return false;
    mtrrs->count = mtrrs->capability & 255;
    for (unsigned i = 0; i < mtrrs->count; ++i)
        if (!cpu_read_msr_safe(0x200 + 2*i, &mtrrs->base[i]) ||
            !cpu_read_msr_safe(0x201 + 2*i, &mtrrs->mask[i])) return false;
    if (mtrrs->capability & (UINT64_C(1) << 8)) {
        static const uint32_t fixed[] = {0x250,0x258,0x259,0x268,0x269,0x26a,0x26b,0x26c,0x26d,0x26e,0x26f};
        for (unsigned i = 0; i < 11; ++i)
            if (!cpu_read_msr_safe(fixed[i], &mtrrs->fixed[i])) return false;
    }
    mtrrs->available = true;
    return true;
}
