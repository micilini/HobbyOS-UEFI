#define main direct_map_host_main
#include "direct-map-host.c"
#undef main
#include "../kernel/src/memory/bootstrap_tables.h"
#include "../kernel/src/memory/bootstrap_claim.h"
#include "../kernel/src/memory/null_probe.h"
#include "../kernel/src/memory/low_fault.h"

static bootstrap_tables_t temporary;
static bootstrap_lifetime_t life;
static bootstrap_claim_t claim, owner;
static boot_physical_objects_t boot;
static unsigned foreign_kind;

static bool foreign_allocate(void *context, mapping_table_t *out)
{
    (void)context;
    if (foreign_kind==0) *out=build.tables[0].table;
    else if (foreign_kind==1) *out=(mapping_table_t){build.tables[1].table.cpu,0x3000000};
    else *out=(mapping_table_t){&backing[1023],claim.physical};
    return true;
}

static bootstrap_departure_t departure(unsigned slot)
{
    return (bootstrap_departure_t){.previous_root=life.root,.root=life.main_root,
        .rsp=RAM_DIRECT_BASE+0x303f00,.gdtr=RAM_DIRECT_BASE+0x310000,
        .idtr=KERNEL_IMAGE_OFFSET+0x204000,.tss=RAM_DIRECT_BASE+0x320000,
        .ist1=RAM_DIRECT_BASE+0x348000,.ist2=RAM_DIRECT_BASE+0x358000,.ist3=RAM_DIRECT_BASE+0x368000,
        .stack_first=RAM_DIRECT_BASE+0x300000,.stack_bytes=16384,.generation=9,
        .apic_id=slot*2,.slot=slot,.cs=8,.tr=0x28};
}

static void claims(void)
{
    contract.identity_retired=true; contract.direct_map=&plan;
    CHECK(bootstrap_claim_candidate(&contract,&boot,0x1000000,8192,0x10000,&claim));
    CHECK(claim.physical==0x10000 && claim.bytes==4096 && claim.origin_type==EFI_CONVENTIONAL_MEMORY);
    CHECK(bootstrap_claim_adopt(&owner,&claim));
    CHECK(bootstrap_claim_adopt(&owner,&claim));
    bootstrap_claim_t wrong=claim; wrong.owner=2;
    CHECK(!bootstrap_claim_adopt(&owner,&wrong));
    wrong=claim; ++wrong.generation; CHECK(!bootstrap_claim_adopt(&owner,&wrong));
    wrong=claim; wrong.physical+=4096; CHECK(!bootstrap_claim_adopt(&owner,&wrong));
    bootstrap_claim_t unchanged=claim;
    CHECK(!bootstrap_claim_candidate(&contract,&boot,0x10000,4096,0x10000,&unchanged));
    CHECK(memcmp(&unchanged,&claim,sizeof(claim))==0);
    boot.rsdp=0xffff; boot.rsdp_bytes=64;
    CHECK(!bootstrap_claim_candidate(&contract,&boot,0x1000000,8192,0x10000,&unchanged));
    boot=(boot_physical_objects_t){0};
    CHECK(!bootstrap_claim_candidate(&contract,&boot,0x1000000,8192,0,&unchanged));
    CHECK(!bootstrap_claim_candidate(&contract,&boot,0x1000000,8192,0xa0000,&unchanged));
    CHECK(!bootstrap_claim_candidate(&contract,&boot,0x1000000,8192,0x10001,&unchanged));
    contract.regions[0].type=EFI_ACPI_MEMORY_NVS;
    CHECK(!bootstrap_claim_candidate(&contract,&boot,0x1000000,8192,0x10000,&unchanged));
    contract.regions[0].type=EFI_CONVENTIONAL_MEMORY;
}

static void roots(void)
{
    const mapping_table_ops_t ops={allocate,release};
    CHECK(paging_build_create(&build,&contract,&plan,&permissions,NULL,&ops,NULL) && verify());
    CHECK(paging_build_activate(&build,load,NULL));
    uint64_t main_hash=paging_table_hash(build.tables[0].table.cpu);
    unsigned main_count=attempts;
    const mapping_table_ops_t foreign_ops={foreign_allocate,release};
    for (foreign_kind=0; foreign_kind<3; ++foreign_kind) {
        unsigned before=freed;
        CHECK(!bootstrap_tables_create(&temporary,&build,&claim,&foreign_ops,NULL));
        CHECK(temporary.count==0 && !temporary.published && freed==before);
        CHECK(paging_table_hash(build.tables[0].table.cpu)==main_hash);
    }
    for (unsigned at=0; at<4; ++at) {
        fail_at=attempts+at;
        CHECK(!bootstrap_tables_create(&temporary,&build,&claim,&ops,NULL));
        CHECK(temporary.count==0 && !temporary.published);
        CHECK(paging_table_hash(build.tables[0].table.cpu)==main_hash);
        /* Failed hosted allocations consume an attempt number, not a frame. */
        --attempts;
    }
    fail_at=UINT32_MAX;
    CHECK(bootstrap_tables_create(&temporary,&build,&claim,&ops,NULL));
    CHECK(temporary.count==4 && bootstrap_tables_verify(&temporary));
    CHECK(temporary.tables[0].physical<UINT64_C(0x100000000));
    for (unsigned table=0; table<4; ++table) for (unsigned slot=0; slot<256; slot+=127) {
        uint64_t *e=&temporary.tables[table].cpu->entries[slot],old=*e;
        *e=2; CHECK(!bootstrap_tables_verify(&temporary));
        *e=old; CHECK(bootstrap_tables_verify(&temporary));
    }
    uint64_t *e=&temporary.tables[0].cpu->entries[511],saved=*e;
    *e^=4096; CHECK(!bootstrap_tables_verify(&temporary)); *e=saved; CHECK(bootstrap_tables_verify(&temporary));
    e=&temporary.tables[2].cpu->entries[0]; saved=*e;
    *e=0x83; CHECK(!bootstrap_tables_verify(&temporary)); *e=saved; CHECK(bootstrap_tables_verify(&temporary));
    const uint32_t ids[]={0,2,4,6};
    CHECK(!bootstrap_lifetime_begin(&life,9,UINT64_C(0x100000000),build.tables[0].table.physical,14,ids,4));
    CHECK(bootstrap_lifetime_begin(&life,9,temporary.tables[0].physical,build.tables[0].table.physical,14,ids,4));
    temporary.published=true;
    CHECK(!bootstrap_tables_abort(&temporary));
    CHECK(!bootstrap_tables_retire(&temporary,&life));
    CHECK(!bootstrap_lifetime_seal(&life,life.main_root));
    CHECK(!bootstrap_lifetime_start(&life,0,0,9));
    CHECK(!bootstrap_lifetime_start(&life,1,4,9));
    CHECK(!bootstrap_lifetime_start(&life,1,2,8));
    for (unsigned slot=1; slot<4; ++slot) {
        CHECK(bootstrap_lifetime_start(&life,slot,slot*2,9));
        CHECK(!bootstrap_lifetime_start(&life,slot,slot*2,9));
        CHECK(bootstrap_lifetime_retry(&life,slot,slot*2,9));
        bootstrap_departure_t d=departure(slot),bad=d;
        bad.root=life.root; CHECK(!bootstrap_lifetime_depart(&life,&bad));
        bad=d; bad.previous_root=life.main_root; CHECK(!bootstrap_lifetime_depart(&life,&bad));
        bad=d; bad.rsp=0x303f00; CHECK(!bootstrap_lifetime_depart(&life,&bad));
        bad=d; bad.gdtr=0x10000; CHECK(!bootstrap_lifetime_depart(&life,&bad));
        bad=d; bad.idtr=0; CHECK(!bootstrap_lifetime_depart(&life,&bad));
        bad=d; bad.tr=0; CHECK(!bootstrap_lifetime_depart(&life,&bad));
        bad=d; bad.ist2=0x40000; CHECK(!bootstrap_lifetime_depart(&life,&bad));
        bad=d; bad.generation=8; CHECK(!bootstrap_lifetime_depart(&life,&bad));
        CHECK(bootstrap_lifetime_depart(&life,&d));
        CHECK(!bootstrap_lifetime_depart(&life,&d));
        CHECK(!bootstrap_lifetime_retry(&life,slot,slot*2,9));
        if (slot!=3) CHECK(!bootstrap_lifetime_seal(&life,life.main_root));
    }
    CHECK(!bootstrap_lifetime_seal(&life,life.root));
    CHECK(bootstrap_lifetime_seal(&life,life.main_root));
    CHECK(!bootstrap_lifetime_start(&life,1,2,9));
    unsigned before=freed;
    CHECK(bootstrap_tables_retire(&temporary,&life));
    CHECK(freed==before+4 && temporary.count==0 && !temporary.main);
    for (unsigned i=0; i<4; ++i) CHECK(!temporary.tables[i].cpu && !temporary.tables[i].physical);
    CHECK(!bootstrap_tables_retire(&temporary,&life));
    CHECK(bootstrap_lifetime_finish(&life)); CHECK(!bootstrap_lifetime_finish(&life));
    CHECK(!bootstrap_lifetime_retry(&life,1,2,9));
    CHECK(paging_table_hash(build.tables[0].table.cpu)==main_hash && build.count==main_count);
    for (unsigned i=0; i<main_count; ++i) CHECK(live[i]);
    build.activated=false; reset_build();
}

static void faults(void)
{
    null_probe_state_t s={.generation=1,.site=0xfffffff0,.recovery=0xfffffff4,.slot=2,.armed=true};
    CHECK(null_probe_match(&s,s.site,0,0,8,2,1));
    for (unsigned bit=0; bit<64; ++bit) CHECK(!null_probe_match(&s,s.site,0,UINT64_C(1)<<bit,8,2,1));
    CHECK(!null_probe_match(&s,s.site+1,0,0,8,2,1));
    CHECK(!null_probe_match(&s,s.site,4096,0,8,2,1));
    CHECK(!null_probe_match(&s,s.site,0,0,3,2,1));
    CHECK(!null_probe_match(&s,s.site,0,0,8,1,1));
    CHECK(!null_probe_match(&s,s.site,0,0,8,2,2));
    s.seen=true; CHECK(!null_probe_match(&s,s.site,0,0,8,2,1));
    s.seen=false; s.armed=false; CHECK(!null_probe_match(&s,s.site,0,0,8,2,1));
    CHECK(strstr(low_fault_description(0),"NULL-address")!=NULL);
    CHECK(strstr(low_fault_description(1),"may have been")!=NULL);
    CHECK(strstr(low_fault_description(UINT64_C(0x7fffffffffff)),"low-address")!=NULL);
    CHECK(strcmp(low_fault_description(UINT64_C(0x800000000000)),"Page Fault")==0);
    CHECK(strcmp(low_fault_description(RAM_DIRECT_BASE),"Page Fault")==0);
}

static void absence(void)
{
    bootstrap_lifetime_t empty={0};
    CHECK(!bootstrap_lifetime_not_needed(&empty,0x100000,4));
    CHECK(bootstrap_lifetime_not_needed(&empty,0x100000,1));
    CHECK(empty.state==BOOTSTRAP_NOT_NEEDED && !empty.root && !empty.count && !empty.intended);
    CHECK(!bootstrap_lifetime_not_needed(&empty,0x100000,1));
    CHECK(!bootstrap_lifetime_start(&empty,0,0,1));
    CHECK(!bootstrap_lifetime_seal(&empty,0x100000));
    CHECK(!bootstrap_lifetime_finish(&empty));
}

int main(void)
{
    initialize(); claims(); roots(); faults(); absence();
    printf("BOOTSTRAP_HOST checks=%u status=PASS\n",checks);
    return 0;
}
