/* Reuse the inherited nonidentity allocator and actual construction harness. */
#define main inherited_direct_map_main
#include "direct-map-host.c"
#undef main
#include "../kernel/src/memory/image_walk.h"
static image_walk_workspace_t workspace;
static const PageTable *read_table(void *context,uint64_t frame,unsigned level)
{
    (void)context;
    for(unsigned i=0;i<build.count;++i) if(build.tables[i].table.physical==frame)
        return build.tables[i].level==level ? build.tables[i].table.cpu : NULL;
    return NULL;
}
static bool walk_actual(void)
{
    image_walk_inputs_t in={.permissions=&permissions,.ram=&plan,.table=read_table};
    image_walk_result_t result;
    return image_walk_main(&in,build.tables[0].table.physical,&workspace,&result) && result.complete &&
        result.nx==features.nx && (features.nx ? !result.rwx_bytes : result.rwx_bytes>0);
}

static void permission_plan(void)
{
    image_permissions_t out;
    image_permission_input_t in=permissions.input;
    CHECK(image_permissions_verify(&permissions,&contract));
    for (unsigned i=0;i<IMAGE_ROLE_COUNT;++i) {
        image_permission_input_t bad=in;
        bad.sections[i].role=IMAGE_ROLE_COUNT;
        CHECK(!image_permissions_create(&out,&contract,&bad));
        bad=in; bad.sections[i].end=bad.sections[i].first-1;
        CHECK(!image_permissions_create(&out,&contract,&bad));
        bad=in; bad.sections[i].end=UINT64_MAX;
        CHECK(!image_permissions_create(&out,&contract,&bad));
    }
    in.sections[IMAGE_DATA].first=0x202fff;
    CHECK(!image_permissions_create(&out,&contract,&in));
    in=permissions.input; in.sections[IMAGE_TRAMPOLINE].first=0x200fff;
    CHECK(!image_permissions_create(&out,&contract,&in));
    in=permissions.input; in.sections[IMAGE_RODATA].first=in.sections[IMAGE_RODATA].end;
    CHECK(!image_permissions_create(&out,&contract,&in));
    in=permissions.input; in.sections[IMAGE_DATA].end=in.sections[IMAGE_DATA].first;
    CHECK(image_permissions_create(&out,&contract,&in));
    in=permissions.input; in.boot_end=0x104001;
    CHECK(!image_permissions_create(&out,&contract,&in));
    image_rights_t rights;
    const struct {uint64_t page; bool write,execute;} expected[]={
        {0x200000,false,true},{0x201000,false,false},{0x202000,false,false},
        {0x203000,true,false},{0x3ff000,true,false},{0x400000,true,false},
        {0x100000,false,false},{0x101000,true,false}};
    for (unsigned i=0;i<sizeof(expected)/sizeof(expected[0]);++i) {
        CHECK(image_permissions_rights(&permissions,expected[i].page,4096,false,&rights));
        CHECK(rights.writable==expected[i].write && !rights.executable);
        if (expected[i].page>=0x200000 && expected[i].page<0x400000) {
            CHECK(image_permissions_rights(&permissions,expected[i].page,4096,true,&rights));
            CHECK(rights.writable==expected[i].write && rights.executable==expected[i].execute);
        }
    }
    CHECK(!image_permissions_rights(&permissions,0x202000,8192,true,&rights));
    CHECK(!image_permissions_rights(&permissions,UINT64_MAX,4096,false,&rights));
    CHECK(!image_permissions_rights(&permissions,0x400000,4096,true,&rights));
    CHECK(!image_permissions_check_leaf(&permissions,KERNEL_IMAGE_OFFSET+0x202000,0x202000,4096,false,false,true));
    CHECK(!image_permissions_check_leaf(&permissions,RAM_DIRECT_BASE+0x200000,0x200000,4096,true,true,true));
    CHECK(!image_permissions_check_leaf(&permissions,RAM_DIRECT_BASE+0x100000,0x100000,4096,false,false,true));
}

static void permission_tables(void)
{
    const mapping_table_ops_t ops={allocate,release};
    CHECK(paging_build_create(&build,&contract,&plan,&permissions,NULL,&ops,NULL) && verify());
    CHECK(walk_actual());
    CHECK(mapping_window_install(&build.window,0,0x800000,1,PAGE_PRESENT|PAGE_RW)==MAPPING_INVALID);
    CHECK(mapping_window_install(&build.window,0,0x800000,1,PAGE_PRESENT|PAGE_RW|PAGE_NX)==MAPPING_OK);
    CHECK(mapping_window_install(&build.window,0,0x800000,1,PAGE_PRESENT|PAGE_RW|PAGE_NX)==MAPPING_OK);
    CHECK(mapping_window_install(&build.window,0,0x800000,1,PAGE_PRESENT|PAGE_NX)==MAPPING_CONFLICT);
    CHECK(mapping_window_remove(&build.window,0,0x800000,1,PAGE_PRESENT|PAGE_RW)==MAPPING_INVALID);
    CHECK(mapping_window_remove(&build.window,0,0x800000,1,PAGE_PRESENT|PAGE_RW|PAGE_NX)==MAPPING_OK);
    CHECK(verify() && walk_actual());
    uint64_t *window=&build.window.tables[2].cpu->entries[0];
    *window=0x800000|PAGE_PRESENT|PAGE_RW|PAGE_NX;
    CHECK(walk_actual());
    uint64_t ancestor=build.tables[0].table.cpu->entries[416];
    /* All controlling NX bits are removed in this synthetic RWX window. */
    build.tables[0].table.cpu->entries[416]&=~PAGE_NX;
    build.window.tables[0].cpu->entries[0]&=~PAGE_NX;
    build.window.tables[1].cpu->entries[0]&=~PAGE_NX;*window&=~PAGE_NX;
    CHECK(!walk_actual());
    build.tables[0].table.cpu->entries[416]=ancestor;
    build.window.tables[0].cpu->entries[0]|=PAGE_NX;
    build.window.tables[1].cpu->entries[0]|=PAGE_NX;
    *window=0x200000|PAGE_PRESENT|PAGE_RW|PAGE_NX;CHECK(!walk_actual());
    *window=0;CHECK(verify() && walk_actual());
    uint64_t physical; bool nx,write;
    for (unsigned direct=0;direct<2;++direct) {
        uint64_t base=direct ? RAM_DIRECT_BASE : KERNEL_IMAGE_OFFSET;
        CHECK(paging_build_rights(&build,base+0x200000,&physical,&write,&nx));
        CHECK(physical==0x200000 && !write && nx==(direct!=0));
        CHECK(paging_build_rights(&build,base+0x202000,&physical,&write,&nx));
        CHECK(physical==0x202000 && !write && nx);
        CHECK(paging_build_rights(&build,base+0x203000,&physical,&write,&nx));
        CHECK(physical==0x203000 && write && nx);
        uint64_t *leaf=entry_at(base+0x200000,1);
        reject_mutation(leaf,*leaf|PAGE_RW);
        leaf=entry_at(base+0x202000,1); reject_mutation(leaf,*leaf|PAGE_RW);
        leaf=entry_at(base+0x201000,1); reject_mutation(leaf,*leaf|PAGE_RW);
    }
    uint64_t *entry=entry_at(KERNEL_IMAGE_OFFSET+0x203000,1);
    reject_mutation(entry,*entry & ~PAGE_NX);
    entry=entry_at(KERNEL_IMAGE_OFFSET+0x201000,1); reject_mutation(entry,*entry & ~PAGE_NX);
    entry=entry_at(KERNEL_IMAGE_OFFSET+0x200000,4); reject_mutation(entry,*entry|PAGE_NX);
    reject_mutation(entry,*entry & ~PAGE_RW);
    entry=entry_at(KERNEL_IMAGE_OFFSET+0x200000,1);
    reject_mutation(entry,(*entry & ~PAGE_ADDR_MASK)|0x201000);
    for (unsigned slot=0;slot<=255;slot+=127) reject_mutation(&build.tables[0].table.cpu->entries[slot],1);
    reject_mutation(&build.tables[0].table.cpu->entries[255],0x1000);
    entry=entry_at(DEVICE_WINDOW_BASE,4); reject_mutation(entry,*entry & ~PAGE_NX);
    reset_build();
    features.nx=false; features.extended_edx &= ~(1u<<20);
    memset(&plan,0,sizeof(plan));
    CHECK(ram_map_plan_create(&plan,&contract,&features,&mtrrs,0x2000003,4096));
    CHECK(paging_build_create(&build,&contract,&plan,&permissions,NULL,&ops,NULL) && verify());
    CHECK(walk_actual());
    for (unsigned i=0;i<build.count;++i)
        for (unsigned j=0;j<512;++j) CHECK(!(build.tables[i].table.cpu->entries[j]&PAGE_NX));
    CHECK(paging_build_rights(&build,KERNEL_IMAGE_OFFSET+0x200000,&physical,&write,&nx) && !write && !nx);
    CHECK(paging_build_rights(&build,RAM_DIRECT_BASE+0x202000,&physical,&write,&nx) && !write && !nx);
    entry=entry_at(KERNEL_IMAGE_OFFSET+0x203000,1); reject_mutation(entry,*entry|PAGE_NX);
    reset_build();
}

int main(void)
{
    initialize(); permission_plan(); permission_tables();
    printf("IMAGE_PERMISSION_HOST checks=%u status=PASS\n",checks);
    return 0;
}
