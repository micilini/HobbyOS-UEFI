#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/src/memory/paging_build.h"

static unsigned checks, attempts, fail_at, freed, loads;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
static physical_contract_t contract;
static ram_map_plan_t plan, saved_plan;
static ram_mtrr_inputs_t mtrrs;
static ram_features_t features;
static physical_contract_t edge_contract;
static ram_map_plan_t edge_plan;
static EfiMemoryDescriptor edge_descriptors[PHYSICAL_DESCRIPTOR_CAPACITY+1];
static const ram_extent_t authorized_framebuffer = {0x2000000, 0x2002000};
static paging_build_t build;
static image_permissions_t permissions;
static PageTable backing[1024];
static bool live[1024];
static EfiMemoryDescriptor descriptors[] = {
    {.Type=EFI_CONVENTIONAL_MEMORY, .PhysicalStart=UINT64_C(0x100000000), .NumberOfPages=0x80000},
    {.Type=EFI_RESERVED_MEMORY_TYPE, .PhysicalStart=UINT64_C(0xff00000000), .NumberOfPages=0x100000},
    {.Type=EFI_CONVENTIONAL_MEMORY, .PhysicalStart=0, .NumberOfPages=0x4000},
    {.Type=EFI_RESERVED_MEMORY_TYPE, .PhysicalStart=0x4000000, .NumberOfPages=0x4000},
};
static bool allocate(void *ctx, mapping_table_t *out)
{
    (void)ctx;
    unsigned at = attempts++;
    if (at == fail_at) return false;
    CHECK(at < 1024 && !live[at]); live[at] = true;
    *out = (mapping_table_t){&backing[at], UINT64_C(0x1000000) + at*4096};
    CHECK((uintptr_t)out->cpu != out->physical);
    return true;
}
static void release(void *ctx, mapping_table_t table)
{
    (void)ctx;
    unsigned at = (unsigned)((table.physical - 0x1000000) / 4096);
    CHECK(at < 1024 && table.cpu == &backing[at] && live[at]);
    live[at] = false; ++freed;
}
static void load(void *ctx, uint64_t physical)
{ (void)ctx; CHECK(physical == 0x1000000); ++loads; }
static void reset_build(void)
{
    paging_build_abort(&build);
    CHECK(attempts == 0 || freed == attempts || freed + 1 == attempts);
    memset(&build, 0, sizeof(build));
    memset(live, 0, sizeof(live)); attempts = freed = loads = 0; fail_at = UINT32_MAX;
}
static void initialize(void)
{
    fail_at = UINT32_MAX;
    MemoryMap map = {.Base=descriptors, .Size=sizeof(descriptors), .DescriptorSize=sizeof(descriptors[0])};
    CHECK(physical_contract_init(&contract, &map, KERNEL_IMAGE_OFFSET+0x200000,
                                KERNEL_IMAGE_OFFSET+0x400000, 1));
    /* Small construction fixture retains the real low-bound contract. */
    contract.identity_limit = contract.allocation_ceiling = 0x4000000;
    CHECK(ram_features_decode(0x80000008, (1u<<29)|(1u<<26)|(1u<<20), 48, true, &features));
    mtrrs.available = true; mtrrs.defaults = 0x806;
    CHECK(ram_map_plan_create(&plan, &contract, &features, &mtrrs, 0x2000003, 4096));
    const image_permission_input_t input={.boot_first=0x100000,.boot_end=0x104000,
        .sections={{0x200000,0x201000,IMAGE_TEXT},{0x201000,0x201400,IMAGE_TRAMPOLINE},
          {0x202000,0x202100,IMAGE_RODATA},{0x202100,0x202120,IMAGE_MSR_FIXUP},
          {0x203000,0x203100,IMAGE_DATA},{0x203100,0x400000,IMAGE_BSS},
          {0x100000,0x100200,IMAGE_BOOT_TEXT},{0x101000,0x104000,IMAGE_BOOT_DATA}}};
    CHECK(image_permissions_create(&permissions,&contract,&input));
    saved_plan = plan;
}
static void plans(void)
{
    CHECK(plan.ceiling == UINT64_C(0x180000000));
    CHECK(plan.bytes == UINT64_C(0x84000000)-8192);
    CHECK(ram_map_contains(&plan, 0, 4096));
    CHECK(!ram_map_contains(&plan, 0x2000000, 1));
    CHECK(!ram_map_contains(&plan, 0x2001000, 4096));
    CHECK(ram_map_contains(&plan, 0x2002000, 4096));
    CHECK(!ram_map_contains(&plan, 0x3fff000, 4097));
    CHECK(!ram_map_contains(&plan, UINT64_C(0xff00000000), 4096));
    uint64_t pointer = 37, physical = 91;
    CHECK(ram_map_pointer(&plan, UINT64_C(0x100000003), 4093, &pointer));
    CHECK(pointer == UINT64_C(0xffff800100000003));
    CHECK(ram_map_inverse(&plan, pointer, 4093, &physical) && physical == UINT64_C(0x100000003));
    CHECK(!ram_map_inverse(&plan, UINT64_C(0xffffc00000000000), 1, &physical));
    CHECK(!ram_map_pointer(&plan, UINT64_MAX, 2, &pointer));
    CHECK(!ram_map_pointer(&plan, 4096, 0, &pointer));
    CHECK(physical == UINT64_C(0x100000003) && pointer == UINT64_C(0xffff800100000003));
    plan.intervals[plan.count-1].end -= 4096; plan.bytes -= 4096; plan.ceiling -= 4096;
    CHECK(!ram_map_plan_verify(&plan, &contract, &mtrrs, authorized_framebuffer)); plan = saved_plan;
    plan.intervals[0].type = EFI_RESERVED_MEMORY_TYPE;
    CHECK(!ram_map_plan_verify(&plan, &contract, &mtrrs, authorized_framebuffer)); plan = saved_plan;
    ram_features_t bad = features;
    CHECK(!ram_features_decode(0x80000001, bad.extended_edx, 48, true, &bad));
    CHECK(!ram_features_decode(0x80000008, bad.extended_edx, 0, true, &bad));
    CHECK(!ram_features_decode(0x80000008, 1u<<29, 36, true, &bad));
    CHECK(ram_features_decode(0x80000008, 1u<<29, 36, false, &bad) && !bad.nx && !bad.page1g);
    memset(&plan, 0, sizeof(plan));
    mtrrs.capability = mtrrs.count = 1;
    mtrrs.base[0] = UINT64_C(0x140000000) | 6;
    mtrrs.mask[0] = UINT64_C(0xffffe0000800);
    CHECK(ram_map_plan_create(&plan, &contract, &features, &mtrrs, 0x2000003, 4096));
    CHECK(ram_map_interval(&plan, UINT64_C(0x140000000))->end == UINT64_C(0x160000000));
    mtrrs.mask[0] ^= UINT64_C(0x400000);
    CHECK(!ram_map_plan_verify(&plan, &contract, &mtrrs, authorized_framebuffer));
    mtrrs = (ram_mtrr_inputs_t){.available=true, .defaults=0x806}; plan = saved_plan;
}
static void entries(void)
{
    for (unsigned level = 1; level <= 3; ++level) {
        uint64_t entry = 77, cache = 8 | (level == 1 ? 128 : 4096);
        CHECK(paging_entry_encode(&features, level, true, UINT64_C(0x100000000), cache, true, true, &entry));
        paging_entry_view_t v;
        CHECK(paging_entry_decode(&features, level, entry, &v));
        CHECK(v.leaf && v.pat_slot == 5 && v.physical == UINT64_C(0x100000000) && v.nx);
        CHECK(v.bytes == (UINT64_C(1) << (12+9*(level-1))));
        CHECK(!paging_entry_encode(&features, level, true, 3, cache, true, true, &entry));
        CHECK(!paging_entry_decode(&features, level, entry | (UINT64_C(1)<<49), &v));
        if (level > 1) CHECK(!paging_entry_decode(&features, level, entry | 0x2000, &v));
    }
    uint64_t out = 13; paging_entry_view_t v;
    CHECK(!paging_entry_encode(&features, 4, true, 0, 0, true, false, &out) && out == 13);
    CHECK(!paging_entry_decode(&features, 4, 0x100083, &v));
    ram_features_t absent = features; absent.page1g = absent.nx = false;
    CHECK(!paging_entry_encode(&absent, 3, true, 0, 0, true, false, &out));
    CHECK(!paging_entry_encode(&absent, 1, true, 0, 0, true, true, &out));
    CHECK(!paging_entry_decode(&absent, 1, UINT64_C(0x8000000000000003), &v));
}
static void descriptor_edges(void)
{
    MemoryMap map = {.Base=edge_descriptors, .DescriptorSize=sizeof(edge_descriptors[0])};
    for (unsigned count=1; count<=PHYSICAL_DESCRIPTOR_CAPACITY; count*=2) {
        memset(edge_descriptors,0,sizeof(edge_descriptors));
        for (unsigned i=0;i<count;++i)
            edge_descriptors[i]=(EfiMemoryDescriptor){.Type=EFI_CONVENTIONAL_MEMORY,
                .PhysicalStart=(uint64_t)(count-1-i)*0x200000,.NumberOfPages=512};
        map.Size=count*sizeof(edge_descriptors[0]);memset(&edge_contract,0,sizeof(edge_contract));
        CHECK(physical_contract_init(&edge_contract,&map,KERNEL_IMAGE_OFFSET+4096,KERNEL_IMAGE_OFFSET+8192,1));
        memset(&edge_plan,0,sizeof(edge_plan));
        CHECK(ram_map_plan_create(&edge_plan,&edge_contract,&features,&mtrrs,0,0));
        CHECK(edge_plan.bytes==(uint64_t)count*0x200000 && ram_map_contains(&edge_plan,0,edge_plan.bytes));
    }
    map.Size=0;memset(&edge_contract,0,sizeof(edge_contract));
    CHECK(!physical_contract_init(&edge_contract,&map,KERNEL_IMAGE_OFFSET+4096,KERNEL_IMAGE_OFFSET+8192,1));
    map.Size=sizeof(edge_descriptors);
    CHECK(!physical_contract_init(&edge_contract,&map,KERNEL_IMAGE_OFFSET+4096,KERNEL_IMAGE_OFFSET+8192,1));
    map.Size=2*sizeof(edge_descriptors[0]);edge_descriptors[1]=edge_descriptors[0];
    CHECK(!physical_contract_init(&edge_contract,&map,KERNEL_IMAGE_OFFSET+4096,KERNEL_IMAGE_OFFSET+8192,1));
    edge_descriptors[1].PhysicalStart=UINT64_MAX-4095;edge_descriptors[1].NumberOfPages=2;
    CHECK(!physical_contract_init(&edge_contract,&map,KERNEL_IMAGE_OFFSET+4096,KERNEL_IMAGE_OFFSET+8192,1));
    for (unsigned type=0;type<=14;++type) {
        edge_descriptors[0]=(EfiMemoryDescriptor){.Type=EFI_CONVENTIONAL_MEMORY,.NumberOfPages=512};
        edge_descriptors[1]=(EfiMemoryDescriptor){.Type=type,.PhysicalStart=0x200000,.NumberOfPages=512};
        memset(&edge_contract,0,sizeof(edge_contract));memset(&edge_plan,0,sizeof(edge_plan));
        CHECK(physical_contract_init(&edge_contract,&map,KERNEL_IMAGE_OFFSET+4096,KERNEL_IMAGE_OFFSET+8192,1));
        CHECK(ram_map_plan_create(&edge_plan,&edge_contract,&features,&mtrrs,0,0));
        bool expected=(type>=1 && type<=7) || type==9 || type==10;
        CHECK(ram_map_contains(&edge_plan,0x200000,0x200000)==expected);
        CHECK(edge_plan.bytes==(expected?0x400000:0x200000));
    }
    for (unsigned boundary=12;boundary<=30;boundary+=9) {
        uint64_t size=UINT64_C(1)<<boundary;
        ram_interval_t interval={.first=UINT64_C(0x100000000),.end=UINT64_C(0x100000000)+size,.maximum_shift=30};
        CHECK(ram_map_leaf_shift(&plan,&interval,interval.first)==boundary);
        interval.end-=4096;
        CHECK(ram_map_leaf_shift(&plan,&interval,interval.first)==(boundary==12?0:boundary-9));
        interval.end+=8192;
        CHECK(ram_map_leaf_shift(&plan,&interval,interval.first)==boundary);
        CHECK(ram_map_leaf_shift(&plan,&interval,interval.first+4096)==12);
    }
    edge_descriptors[1]=(EfiMemoryDescriptor){.Type=EFI_CONVENTIONAL_MEMORY,.PhysicalStart=RAM_DIRECT_BYTES,.NumberOfPages=1};
    memset(&edge_contract,0,sizeof(edge_contract));memset(&edge_plan,0,sizeof(edge_plan));
    CHECK(physical_contract_init(&edge_contract,&map,KERNEL_IMAGE_OFFSET+4096,KERNEL_IMAGE_OFFSET+8192,1));
    CHECK(!ram_map_plan_create(&edge_plan,&edge_contract,&features,&mtrrs,0,0));
    edge_descriptors[1].PhysicalStart=UINT64_C(1)<<40;
    memset(&edge_contract,0,sizeof(edge_contract));memset(&edge_plan,0,sizeof(edge_plan));
    CHECK(physical_contract_init(&edge_contract,&map,KERNEL_IMAGE_OFFSET+4096,KERNEL_IMAGE_OFFSET+8192,1));
    ram_features_t narrow=features;narrow.physical_bits=40;narrow.address_eax=40;
    CHECK(!ram_map_plan_create(&edge_plan,&edge_contract,&narrow,&mtrrs,0,0));
    ram_mtrr_inputs_t cache=mtrrs;cache.count=2;cache.capability=2;
    cache.base[0]=UINT64_C(0x100000000)|6;cache.mask[0]=UINT64_C(0xffffc0000800);
    cache.base[1]=UINT64_C(0x100000000)|0;cache.mask[1]=UINT64_C(0xffffe0000800);
    memset(&edge_plan,0,sizeof(edge_plan));
    CHECK(ram_map_plan_create(&edge_plan,&contract,&features,&cache,0x2000003,4096));
    CHECK(ram_map_interval(&edge_plan,UINT64_C(0x100000000))->end==UINT64_C(0x120000000));
    cache.defaults=6;memset(&edge_plan,0,sizeof(edge_plan));
    CHECK(ram_map_plan_create(&edge_plan,&contract,&features,&cache,0x2000003,4096));
    CHECK(edge_plan.cache_count==0);
    cache.available=false;memset(&edge_plan,0,sizeof(edge_plan));
    CHECK(!ram_map_plan_create(&edge_plan,&contract,&features,&cache,0x2000003,4096));
}
static uint64_t *entry_at(uint64_t va, unsigned target)
{
    PageTable *table = build.tables[0].table.cpu;
    for (unsigned level = 4; level; --level) {
        uint64_t *e = &table->entries[(va >> (12+9*(level-1))) & 511];
        if (level == target) return e;
        uint64_t physical = *e & UINT64_C(0x000ffffffffff000);
        unsigned i;
        for (i=0; i<build.count && build.tables[i].table.physical != physical; ++i) {}
        CHECK(i < build.count); table = build.tables[i].table.cpu;
    }
    abort();
}
static void cache_overlaps(void)
{
    ram_mtrr_inputs_t cache = {.available=true, .defaults=0x806, .capability=0x402, .count=2};
    cache.base[0] = UINT64_C(0x100000006);
    cache.base[1] = UINT64_C(0x100000001);
    cache.mask[0] = cache.mask[1] = UINT64_C(0xffff80000800);
    memset(&edge_plan, 0, sizeof(edge_plan));
    CHECK(!ram_map_plan_create(&edge_plan, &contract, &features, &cache, 0x2000003, 4096));
    CHECK(!edge_plan.ready);
    CHECK(!ram_map_plan_verify(&saved_plan, &contract, &cache, authorized_framebuffer));
    cache.base[1] = UINT64_C(0x100000006);
    memset(&edge_plan, 0, sizeof(edge_plan));
    CHECK(ram_map_plan_create(&edge_plan, &contract, &features, &cache, 0x2000003, 4096));
    cache.base[1] = UINT64_C(0x100000004);
    memset(&edge_plan, 0, sizeof(edge_plan));
    CHECK(ram_map_plan_create(&edge_plan, &contract, &features, &cache, 0x2000003, 4096));
    cache.base[0] = UINT64_C(0x100000000); cache.base[1] = UINT64_C(0x100000001);
    memset(&edge_plan, 0, sizeof(edge_plan));
    CHECK(ram_map_plan_create(&edge_plan, &contract, &features, &cache, 0x2000003, 4096));
    cache.capability = 2;
    memset(&edge_plan, 0, sizeof(edge_plan));
    CHECK(!ram_map_plan_create(&edge_plan, &contract, &features, &cache, 0x2000003, 4096));
    cache = (ram_mtrr_inputs_t){.available=true, .defaults=0x801};
    memset(&edge_plan, 0, sizeof(edge_plan));
    CHECK(!ram_map_plan_create(&edge_plan, &contract, &features, &cache, 0x2000003, 4096));
    cache.capability = 0x400;
    memset(&edge_plan, 0, sizeof(edge_plan));
    CHECK(ram_map_plan_create(&edge_plan, &contract, &features, &cache, 0x2000003, 4096));
    cache = (ram_mtrr_inputs_t){.available=true, .defaults=0xc06, .capability=0x502, .count=2};
    for (unsigned i=0; i<11; ++i) cache.fixed[i] = UINT64_C(0x0606060606060606);
    cache.base[0] = 6; cache.base[1] = 1;
    cache.mask[0] = cache.mask[1] = UINT64_C(0xfffffff00800);
    memset(&edge_plan, 0, sizeof(edge_plan));
    CHECK(ram_map_plan_create(&edge_plan, &contract, &features, &cache, 0x2000003, 4096));
    cache.mask[0] = cache.mask[1] = UINT64_C(0xffffffe00800);
    memset(&edge_plan, 0, sizeof(edge_plan));
    CHECK(!ram_map_plan_create(&edge_plan, &contract, &features, &cache, 0x2000003, 4096));
}
static const paging_required_path_t paths[] = {
    {KERNEL_IMAGE_OFFSET+0x200000, 4096, true,false}, {RAM_DIRECT_BASE+0x7000, 8192, false,true}
};
static bool verify(void)
{ return paging_build_verify(&build, &contract, &mtrrs, authorized_framebuffer, paths, 2); }
static void reject_mutation(uint64_t *entry, uint64_t replacement)
{
    uint64_t saved = *entry; *entry = replacement;
    CHECK(!verify()); CHECK(!paging_build_activate(&build, load, NULL) && loads == 0);
    *entry = saved; CHECK(verify());
}
static void tables(void)
{
    const mapping_table_ops_t ops = {allocate, release};
    CHECK(paging_build_create(&build, &contract, &plan, &permissions,NULL,&ops, NULL));
    CHECK(verify());
    unsigned count = build.count;
    CHECK(build.leaves[2] == 2 && build.leaves[1] > 0 && build.leaves[0] > 0);
    uint64_t *e = entry_at(RAM_DIRECT_BASE+UINT64_C(0x100000000), 3);
    reject_mutation(e, *e ^ UINT64_C(0x40000000));
    reject_mutation(e, 0);
    uint64_t original_leaf=*e;
    *e&=~PAGE_NX;
    CHECK(verify()); /* The data-only parent still denies execution. */
    uint64_t *parent=entry_at(RAM_DIRECT_BASE+UINT64_C(0x100000000),4);
    uint64_t original_parent=*parent;
    *parent&=~PAGE_NX;
    CHECK(!verify()); CHECK(!paging_build_activate(&build,load,NULL) && loads==0);
    *parent=original_parent;*e=original_leaf;CHECK(verify());
    reject_mutation(e, *e | 0x2000);
    reject_mutation(e, *e ^ 128);
    e = entry_at(RAM_DIRECT_BASE+0x2000000, 2);
    reject_mutation(e, UINT64_C(0x8000000002000083));
    e = entry_at(RAM_DIRECT_BASE+0x7000, 1); reject_mutation(e, 0);
    e = entry_at(KERNEL_IMAGE_OFFSET+0x200000, 1); reject_mutation(e, 0);
    e = entry_at(KERNEL_IMAGE_OFFSET+0x200000, 4);
    reject_mutation(e, *e | (UINT64_C(1)<<63));
    e = entry_at(UINT64_C(0x100000000), 4);
    reject_mutation(e, UINT64_C(0x100000083));
    e = entry_at(RAM_DIRECT_BASE, 4);
    reject_mutation(e, build.tables[0].table.physical | UINT64_C(0x8000000000000003));
    reject_mutation(e, *e | 0x800);
    mtrrs = (ram_mtrr_inputs_t){.available=true, .defaults=0x806, .capability=0x402, .count=2};
    mtrrs.base[0] = UINT64_C(0x100000006); mtrrs.base[1] = UINT64_C(0x100000001);
    mtrrs.mask[0] = mtrrs.mask[1] = UINT64_C(0xffff80000800);
    CHECK(!verify()); CHECK(!paging_build_activate(&build, load, NULL) && loads == 0);
    mtrrs = (ram_mtrr_inputs_t){.available=true, .defaults=0x806}; CHECK(verify());
    uint64_t physical; bool nx;
    CHECK(paging_build_lookup(&build, RAM_DIRECT_BASE+UINT64_C(0x100001234), &physical, &nx));
    CHECK(physical == UINT64_C(0x100001234) && nx);
    CHECK(!paging_build_lookup(&build, RAM_DIRECT_BASE+0x4000000, &physical, &nx));
    CHECK(paging_build_activate(&build, load, NULL) && loads == 1);
    CHECK(!paging_build_activate(&build, load, NULL) && loads == 1);
    paging_build_abort(&build); CHECK(freed == 0);
    /* Hosted end-of-boot teardown only; production never clears activation. */
    build.activated = false; reset_build();
    for (unsigned failure=0; failure<count; ++failure) {
        fail_at = failure;
        CHECK(!paging_build_create(&build, &contract, &plan, &permissions,NULL,&ops, NULL));
        CHECK(!build.count && !build.built && freed == failure);
        CHECK(!paging_build_activate(&build, load, NULL) && loads == 0);
        reset_build();
    }
    features.page1g = features.nx = false; features.extended_edx &= ~((1u<<26)|(1u<<20));
    memset(&plan, 0, sizeof(plan));
    CHECK(ram_map_plan_create(&plan, &contract, &features, &mtrrs, 0x2000003, 4096));
    CHECK(paging_build_create(&build, &contract, &plan, &permissions,NULL,&ops, NULL) && verify());
    CHECK(build.leaves[2] == 0 && build.leaves[1] >= 1024);
    e = entry_at(RAM_DIRECT_BASE+UINT64_C(0x100000000), 3);
    reject_mutation(e, UINT64_C(0x100000083));
    CHECK(paging_build_lookup(&build, RAM_DIRECT_BASE+UINT64_C(0x100001234), &physical, &nx) && !nx);
    reset_build();
}
int main(void)
{
    initialize(); plans(); entries(); descriptor_edges(); cache_overlaps(); tables();
    printf("DIRECT_MAP_HOST checks=%u status=PASS\n", checks);
    return 0;
}
