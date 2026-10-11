/* Actual unpublished constructor, slot mapper and independent main-root walk. */
#define main inherited_construction_main
#include "direct-map-host.c"
#undef main
#include "../kernel/src/memory/image_walk.h"
static stack_owner_record_t owners[STACK_OWNER_COUNT];
static stack_layout_t layout;
static image_walk_workspace_t workspace;
static bool mutate_owner;
static const PageTable *table_reader(void *context,uint64_t frame,unsigned level)
{
    (void)context;
    if (mutate_owner) {owners[STACK_CPU_SLOTS].sequence+=2;mutate_owner=false;}
    for (unsigned i=0;i<build.count;++i)
        if(build.tables[i].table.physical==frame)
            return build.tables[i].level==level ? build.tables[i].table.cpu : NULL;
    return NULL;
}
static bool actual_walk(void)
{
    image_walk_inputs_t input={.permissions=&permissions,.ram=&plan,.stacks=&layout,.table=table_reader};
    image_walk_result_t result;
    return image_walk_main(&input,build.tables[0].table.physical,&workspace,&result) && result.complete &&
        result.image_bytes==UINT64_C(0x1ff000) && (features.nx ? result.rwx_bytes==0 : result.rwx_bytes>0);
}
static stack_handle_t add(unsigned slot,stack_kind_t kind,uint32_t capacity,uint64_t physical,bool owned)
{
    stack_handle_t h;
    CHECK(stack_owner_reserve(&owners[slot],slot,kind,capacity,physical,owned,0,&h));
    CHECK(stack_owner_mapped(&owners[slot],h,kind==STACK_KIND_TASK));
    if (kind!=STACK_KIND_TASK) {
        CHECK(stack_owner_bind_cpu(&owners[slot],h,slot ? 31 : 0,slot ? 217 : 9));
        CHECK(stack_owner_publish(&owners[slot],h));
    }
    return h;
}
static void reject_both(uint64_t *entry,uint64_t value)
{
    uint64_t old=*entry;*entry=value;
    CHECK(!verify());CHECK(!actual_walk());
    CHECK(!paging_build_activate(&build,load,NULL) && loads==0);
    *entry=old;CHECK(verify());CHECK(actual_walk());
}
static void policy(bool nx)
{
    memset(&contract,0,sizeof(contract));memset(&plan,0,sizeof(plan));
    initialize();memset(owners,0,sizeof(owners));
    if (!nx) {
        features.nx=false;features.extended_edx&=~(1u<<20);memset(&plan,0,sizeof(plan));
        CHECK(ram_map_plan_create(&plan,&contract,&features,&mtrrs,0x2000003,4096));
    }
    image_permission_input_t input=permissions.input;
    input.bsp_guard_first=0x3fb000;input.bsp_guard_end=input.bsp_stack_first=0x3fc000;input.bsp_stack_end=0x400000;
    CHECK(image_permissions_create(&permissions,&contract,&input));
    image_permission_input_t bad=input;image_permissions_t ignored;
    bad.bsp_guard_first-=4096;CHECK(!image_permissions_create(&ignored,&contract,&bad));
    bad=input;bad.bsp_stack_end+=4096;CHECK(!image_permissions_create(&ignored,&contract,&bad));
    bad=input;bad.bsp_guard_first=0x200000;bad.bsp_guard_end=bad.bsp_stack_first=0x201000;bad.bsp_stack_end=0x205000;
    CHECK(!image_permissions_create(&ignored,&contract,&bad));
    CHECK(!image_permissions_linked_range(&permissions,KERNEL_IMAGE_OFFSET+0x3fb000,1));
    CHECK(!image_permissions_linked_range(&permissions,KERNEL_IMAGE_OFFSET+0x3fafff,2));
    CHECK(image_permissions_linked_range(&permissions,KERNEL_IMAGE_OFFSET+0x3fc000,16384));
    layout=(stack_layout_t){.owners=owners,.borrowed_ist={0x210000,0x218000,0x220000},
        .bsp_guard=KERNEL_IMAGE_OFFSET+0x3fb000};
    add(0,STACK_KIND_IST1,STACK_IST_BYTES,0x210000,false);
    add(1,STACK_KIND_IST2,STACK_IST_BYTES,0x218000,false);
    add(2,STACK_KIND_IST3,STACK_IST_BYTES,0x220000,false);
    add(STACK_CPU_SLOTS-1,STACK_KIND_AP_BOOT,STACK_TASK_BYTES,UINT64_C(0x100100000),true);
    stack_handle_t task=add(STACK_CPU_SLOTS,STACK_KIND_TASK,STACK_TASK_BYTES,UINT64_C(0x100200000),true);
    add(STACK_REGION_SLOTS-1,STACK_KIND_TASK,STACK_TASK_BYTES,UINT64_C(0x100300000),true);
    const mapping_table_ops_t ops={allocate,release};
    CHECK(paging_build_create(&build,&contract,&plan,&permissions,&layout,&ops,NULL));
    CHECK(verify());CHECK(actual_walk());
    mutate_owner=true;CHECK(!actual_walk());CHECK(actual_walk());
    ++owners[STACK_CPU_SLOTS].sequence;CHECK(!actual_walk());
    ++owners[STACK_CPU_SLOTS].sequence;CHECK(actual_walk());
    stack_owner_view_t v;CHECK(stack_owner_read(&owners[task.slot],&v));
    CHECK(stack_window_check(&build.stacks,build.tables[0].table.cpu,&v));
    CHECK(stack_window_install(&build.stacks,&v));
    uint64_t encoded;
    CHECK(paging_entry_encode(&features,1,true,v.physical,0,true,features.nx,&encoded));
    reject_both(entry_at(v.guard,1),encoded);
    reject_both(entry_at(v.guard,1),encoded&~PAGE_RW);
    reject_both(entry_at(v.end,1),encoded);
    uint64_t *pte=entry_at(v.base,1),saved=*pte;
    reject_both(pte,0);reject_both(pte,saved&~PAGE_RW);
    reject_both(pte,(saved&~PAGE_ADDR_MASK)|permissions.input.sections[IMAGE_TEXT].first);
    reject_both(entry_at(v.base,4),*entry_at(v.base,4)&~PAGE_RW);
    CHECK(paging_entry_encode(&features,2,true,UINT64_C(0x100000000),0,true,features.nx,&encoded));
    reject_both(entry_at(v.guard,2),encoded);
    CHECK(paging_entry_encode(&features,1,true,0x3fb000,0,true,features.nx,&encoded));
    reject_both(entry_at(KERNEL_IMAGE_OFFSET+0x3fb000,1),encoded);
    if (nx) {
        uint64_t old[4];
        for (unsigned i=1;i<=4;++i) {uint64_t *p=entry_at(v.base,i);old[i-1]=*p;*p&=~PAGE_NX;}
        CHECK(!verify());CHECK(!actual_walk());
        for (unsigned i=1;i<=4;++i) *entry_at(v.base,i)=old[i-1];
    } else reject_both(pte,saved|PAGE_NX);
    *pte|=PAGE_ACCESSED|PAGE_DIRTY;CHECK(stack_window_install(&build.stacks,&v));CHECK(*pte==(saved|PAGE_ACCESSED|PAGE_DIRTY));
    *pte=0;CHECK(!stack_window_install(&build.stacks,&v));CHECK(!*pte);*pte=saved;
    *pte^=PAGE_RW;CHECK(!stack_window_install(&build.stacks,&v));CHECK(*pte==(saved^PAGE_RW));*pte=saved;
    stack_owner_view_t forged=v;forged.physical=permissions.input.sections[IMAGE_TEXT].first;
    CHECK(!stack_backing_allowed(&layout,&permissions,&plan,&forged));
    forged.owned=false;forged.kind=STACK_KIND_IST1;CHECK(!stack_backing_allowed(&layout,&permissions,&plan,&forged));
    CHECK(stack_owner_take_prepared(&owners[task.slot],&task));
    CHECK(stack_owner_bind_task(&owners[task.slot],task,42,87,"real-task"));CHECK(stack_owner_publish(&owners[task.slot],task));
    CHECK(stack_owner_begin_retire(&owners[task.slot],task,42,87,false));
    CHECK(stack_owner_read(&owners[task.slot],&v));CHECK(stack_window_remove(&build.stacks,&v));
    CHECK(!stack_window_remove(&build.stacks,&v));CHECK(verify());CHECK(actual_walk());
    CHECK(!stack_owner_finish_retire(&owners[task.slot],task,19,false));
    CHECK(!stack_owner_reserve(&owners[task.slot],task.slot,STACK_KIND_TASK,STACK_TASK_BYTES,v.physical,true,0,&task));
    CHECK(verify());CHECK(actual_walk());reset_build();
}
int main(void)
{
    policy(true);policy(false);
    printf("STACK_TABLE_HOST checks=%u status=PASS actual_constructor=1 actual_walker=1 hardware_faults=NOT_HOSTED\n",checks);
    return 0;
}
