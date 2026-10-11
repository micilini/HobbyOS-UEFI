/* Actual stack view and physical inverse decisions with separate host/VA/PA
 * domains. No guest address is dereferenced and no privileged operation runs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HOBBYOS_STACK_HOST_TEST 1
#include "../kernel/src/memory/kernel_stack.c"
#include "../kernel/src/memory/physical_internal.h"
static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"stack inverse check %u: %s\n",checks,#x);exit(1);}}while(0)
bool owned_mapping_physical(const void *pointer,size_t bytes,uint64_t *out)
{(void)pointer;(void)bytes;(void)out;return false;}
bool paging_query_range(uint64_t first,size_t bytes,uint64_t *out)
{(void)first;(void)bytes;(void)out;return false;}
static void query(uint64_t va,uint64_t bytes,bool expected,uint64_t pa)
{
    uint64_t out=UINT64_C(0x9191919191919191);
    CHECK(physical_address_range((const void *)(uintptr_t)va,bytes,&out)==expected);
    CHECK(out==(expected?pa:UINT64_C(0x9191919191919191)));
}
int main(void)
{
    /* Zero is a valid physical frame in the checked inverse, regardless of
     * the PMM allocation API's separate null-on-failure convention. */
    stack_handle_t h;
    CHECK(stack_owner_reserve(&g_stack_owners[128],128,STACK_KIND_TASK,16384,0,true,0,&h));
    CHECK(stack_owner_mapped(&g_stack_owners[128],h,false));
    CHECK(stack_owner_bind_task(&g_stack_owners[128],h,123,456,"inverse-owner"));
    CHECK(stack_owner_publish(&g_stack_owners[128],h));
    uint64_t guard=STACK_REGION_BASE+128*STACK_SLOT_BYTES,base=guard+4096;
    for(unsigned i=0;i<16384;i+=1023)query(base+i,1,true,i);
    query(base,16384,true,0);query(base+16383,1,true,16383);
    query(base+16384,1,false,0);query(base+16383,2,false,0);
    query(guard,1,false,0);query(base-1,1,false,0);query(base-1,2,false,0);
    query(base,0,false,0);query(base,UINT64_MAX,false,0);
    CHECK(!physical_address_range((void *)(uintptr_t)base,1,NULL));
    CHECK(stack_owner_begin_retire(&g_stack_owners[128],h,123,456,false));
    query(base,1,false,0);
    CHECK(stack_owner_finish_retire(&g_stack_owners[128],h,9,true));
    query(base,1,false,0);
    stack_handle_t replacement;
    CHECK(stack_owner_reserve(&g_stack_owners[128],128,STACK_KIND_TASK,16384,UINT64_C(0x100004000),true,0,&replacement));
    CHECK(replacement.generation>h.generation);
    query(base,1,false,0);
    CHECK(stack_owner_mapped(&g_stack_owners[128],replacement,false));
    query(base,16384,true,UINT64_C(0x100004000));
    g_stack_layout.bsp_guard=KERNEL_IMAGE_OFFSET+0x400000;
    g_physical_contract.ready=true;g_physical_contract.image_first=KERNEL_IMAGE_OFFSET+0x300000;
    g_physical_contract.image_end=KERNEL_IMAGE_OFFSET+0x500000;
    uint64_t out=77;
    CHECK(!physical_image_address_range(g_stack_layout.bsp_guard,1,&out) && out==77);
    CHECK(!physical_image_address_range(g_stack_layout.bsp_guard-1,2,&out) && out==77);
    CHECK(physical_image_address_range(g_stack_layout.bsp_guard+4096,16384,&out) && out==0x401000);
    query(g_stack_layout.bsp_guard+4095,1,false,0);
    g_physical_contract.identity_retired=true;
    physical_translation_mapping(0,0,1);
    query(base,1,false,0);
    puts("STACK_INVERSE_HOST status=PASS actual_stack_inverse=1 physical_zero=VALID broken_identity=REJECTED privileged_execution=NONE");
    printf("checks=%u\n",checks);
    return 0;
}
