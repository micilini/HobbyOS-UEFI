#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/src/memory/ram_map_boot.h"
#include "../kernel/src/cpu/cpu.h"

static cpu_policy_t policy;
static uint32_t maximum, extended, width, unavailable_leaf, unavailable_msr;
static unsigned reads;
const cpu_policy_t *cpu_policy_get(void) { return &policy; }
bool cpu_get_cpuid_count(uint32_t leaf, uint32_t subleaf,
    uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    assert(subleaf == 0);
    if (a) *a=0;
    if (b) *b=0;
    if (c) *c=0;
    if (d) *d=0;
    if (leaf==unavailable_leaf) return false;
    switch (leaf) {
    case 1: if (d) *d=CPU_FEATURE_REQUIRED | (1u<<12); break;
    case 0x80000000: if (a) *a=maximum; break;
    case 0x80000001: if (d) *d=extended; break;
    case 0x80000008: if (a) *a=width; break;
    default: assert(0);
    }
    return true;
}
bool cpu_read_msr_safe(uint32_t msr, uint64_t *value)
{
    ++reads;
    if (msr==unavailable_msr) return false;
    if (msr==0xfe) *value=0x100;
    else if (msr==0x2ff) *value=0xc06;
    else { assert(msr==0x250 || msr==0x258 || msr==0x259 || (msr>=0x268 && msr<=0x26f));
        *value=UINT64_C(0x0606060606060606); }
    return true;
}
int main(void)
{
    unsigned cases=0;
    for (unsigned nx=0; nx<2; ++nx) for (unsigned large=0; large<2; ++large) {
        maximum=0x80000008; extended=CPU_FEATURE_LM | (nx?CPU_FEATURE_NX:0) | (large?CPU_FEATURE_PAGE1G:0);
        width=40; unavailable_leaf=unavailable_msr=0; reads=0;
        cpu_policy_features_t raw={CPU_FEATURE_REQUIRED,extended,0,true,true,true};
        assert(cpu_policy_build(&raw,&policy)==CPU_POLICY_OK);
        ram_features_t f={0}; ram_mtrr_inputs_t m={0};
        assert(ram_map_boot_inputs(&f,&m));
        assert(f.page1g==!!large && f.nx==!!nx && f.physical_bits==40 && m.available && reads==13); ++cases;
        unavailable_leaf=0x80000008; assert(!ram_map_boot_inputs(&f,&m)); ++cases;
        unavailable_leaf=0; width=0; assert(!ram_map_boot_inputs(&f,&m)); ++cases;
        width=40; unavailable_msr=0x2ff; assert(!ram_map_boot_inputs(&f,&m)); ++cases;
    }
    printf("status=PASS cases=%u scope=actual_boot_feature_cache_selection privileged_reads=HOSTED\n",cases);
}
