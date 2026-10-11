#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/src/memory/pmem.c"

static irq_flags_t flags=0x202;
static unsigned locks,unlocks;
irq_flags_t spin_lock_irqsave(spinlock_t *lock)
{ assert(lock==&g_pmm_lock && !lock->locked);irq_flags_t saved=flags;flags&=~0x200;lock->locked=1;++locks;return saved; }
void spin_unlock_irqrestore(spinlock_t *lock,irq_flags_t saved)
{ assert(lock==&g_pmm_lock && lock->locked && !(flags&0x200));lock->locked=0;flags=saved;++unlocks; }
static ram_map_plan_t plan;
int main(void)
{
    EfiMemoryDescriptor descriptors[]={
        {.Type=EFI_CONVENTIONAL_MEMORY,.PhysicalStart=0,.NumberOfPages=0x40000},
        {.Type=EFI_CONVENTIONAL_MEMORY,.PhysicalStart=0x100000000,.NumberOfPages=0x40000},
        {.Type=EFI_RESERVED_MEMORY_TYPE,.PhysicalStart=0xff00000000,.NumberOfPages=0x100000}};
    MemoryMap map={.Base=descriptors,.Size=sizeof(descriptors),.DescriptorSize=sizeof(descriptors[0])};
    assert(physical_translation_publish(&map,KERNEL_IMAGE_OFFSET+0x200000,KERNEL_IMAGE_OFFSET+0x400000));
    ram_features_t f;ram_mtrr_inputs_t m={.available=true,.defaults=0x806};
    assert(ram_features_decode(0x80000008,(1u<<29)|(1u<<26)|(1u<<20),40,true,&f));
    assert(ram_map_plan_create(&plan,&g_physical_contract,&f,&m,0xe0000000,4096));
    g_total_frames=268435456;g_bitmap_size=g_total_frames/8;g_bitmap_storage_size=2*g_bitmap_size;
    g_bitmap_physical=0x4000000;g_bitmap_buffer=malloc(g_bitmap_storage_size);assert(g_bitmap_buffer);
    void *original=g_bitmap_buffer;
    g_reserved_bitmap_buffer=(uint8_t *)original+g_bitmap_size;
    g_bitmap=(Bitmap){g_bitmap_buffer,g_bitmap_size,g_total_frames};
    g_reserved_bitmap=(Bitmap){g_reserved_bitmap_buffer,g_bitmap_size,g_total_frames};
    g_free_frames=37;g_alloc_limit_frames=0x100000;
    memset(original,0xa5,g_bitmap_storage_size);
    assert(pmm_direct_map_ready(&plan) && physical_allocation_ceiling()==0x100000000);
    plan.ready=false;assert(!pmm_direct_map_activate(&plan));plan.ready=true;
    assert(!locks && g_bitmap_buffer==original && physical_allocation_ceiling()==0x100000000);
    g_bitmap.buffer=NULL;assert(!pmm_direct_map_activate(&plan));g_bitmap.buffer=original;
    assert(pmm_direct_map_activate(&plan));
    assert(locks==unlocks && locks==1 && flags==0x202 && !g_pmm_lock.locked);
    assert((uintptr_t)g_bitmap_buffer==RAM_DIRECT_BASE+0x4000000 && g_bitmap.buffer==g_bitmap_buffer);
    assert((uintptr_t)g_reserved_bitmap_buffer==RAM_DIRECT_BASE+0x6000000 && g_reserved_bitmap.buffer==g_reserved_bitmap_buffer);
    assert(g_alloc_limit_frames==0x140000 && physical_allocation_ceiling()==0x140000000 &&
        g_physical_contract.epoch==2 && g_free_frames==37 && g_total_frames==268435456);
    for(size_t i=0;i<g_bitmap_storage_size;++i)assert(((unsigned char *)original)[i]==0xa5);
    assert(!pmm_direct_map_activate(&plan) && locks==1);
    uint64_t output=99;
    assert(!physical_contract_inverse(&g_physical_contract,0x4000000,1,&output) && output==99);
    assert(physical_contract_inverse(&g_physical_contract,RAM_DIRECT_BASE+0x100000000,4096,&output) && output==0x100000000);
    assert(!physical_contract_inverse(&g_physical_contract,RAM_DIRECT_BASE+0x40000000,1,&output));
    free(original);
    puts("status=PASS cases=12 scope=actual_bitmap_pointer_and_coverage_publication original_storage_unchanged=1 logical_frames=268435456 privileged_locks=HOSTED");
    return 0;
}
