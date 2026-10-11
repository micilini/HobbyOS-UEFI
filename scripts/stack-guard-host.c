#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/src/memory/stack_domain.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"stack owner check %u line %u failed: %s\n",checks,__LINE__,#x);exit(1); } } while (0)
static stack_owner_record_t records[STACK_OWNER_COUNT];
static stack_owner_record_t raced;
static unsigned winners;
static stack_handle_t winner;

static void *creator(void *arg)
{
    stack_handle_t h;
    if (stack_owner_reserve(&raced,STACK_CPU_SLOTS,STACK_KIND_TASK,STACK_TASK_BYTES,
        ((uintptr_t)arg+1)*STACK_SLOT_BYTES,true,0,&h)) {
        winner=h;__atomic_fetch_add(&winners,1,__ATOMIC_RELAXED);
    }
    return NULL;
}

static void geometry(void)
{
    uint64_t guard,base,end;
    for (unsigned slot=0;slot<STACK_REGION_SLOTS;++slot) {
        CHECK(stack_slot_geometry(slot,STACK_TASK_BYTES,&guard,&base,&end));
        CHECK(guard==STACK_REGION_BASE+(uint64_t)slot*65536 && base==guard+4096 && end-base==16384);
        CHECK(!(guard&4095) && end<=STACK_REGION_END && end<=guard+STACK_SLOT_BYTES);
        CHECK(stack_slot_geometry(slot,STACK_IST_BYTES,&guard,&base,&end)==(slot<STACK_CPU_SLOTS));
    }
    CHECK(!stack_slot_geometry(STACK_REGION_SLOTS,16384,&guard,&base,&end));
    CHECK(!stack_slot_geometry(UINT32_MAX,16384,&guard,&base,&end));
    CHECK(!stack_slot_geometry(0,16383,&guard,&base,&end));
    CHECK(!stack_slot_geometry(0,32769,&guard,&base,&end));
    CHECK(!stack_slot_geometry(0,16384,NULL,&base,&end));
    CHECK(STACK_REGION_BASE>UINT64_C(0xffffd00040000000));
    CHECK(STACK_REGION_BASE>UINT64_C(0xffffc000001ff000)+UINT64_C(0x40000000)+65*4096);
}

static void ownership(void)
{
    const unsigned slot=STACK_CPU_SLOTS;
    stack_owner_record_t *r=&records[slot];stack_handle_t h,next;
    CHECK(!stack_owner_reserve(r,slot,STACK_KIND_IST1,32768,0x100000,true,0,&h));
    CHECK(!stack_owner_reserve(r,slot,STACK_KIND_TASK,16384,0x100001,true,0,&h));
    CHECK(!stack_owner_reserve(r,slot,STACK_KIND_TASK,16384,PHYSICAL_ADDRESS_LIMIT-4096,true,0,&h));
    CHECK(!stack_owner_reserve(r,slot,STACK_KIND_TASK,16384,0x100000,false,0,&h));
    CHECK(stack_owner_reserve(r,slot,STACK_KIND_TASK,16384,0,true,0,&h));
    CHECK(!stack_owner_reserve(r,slot,STACK_KIND_TASK,16384,0x200000,true,0,&next));
    CHECK(stack_owner_mapped(r,h,true));
    CHECK(stack_owner_take_prepared(r,&next) && next.generation>h.generation);
    CHECK(!stack_owner_mapped(r,h,false));h=next;
    CHECK(stack_owner_bind_task(r,h,91,112,"guarded-owner"));
    CHECK(stack_owner_return_prepared(r,h));
    CHECK(stack_owner_take_prepared(r,&next) && next.generation>h.generation);h=next;
    CHECK(!stack_owner_bind_task(r,h,0,112,"invalid"));
    CHECK(!stack_owner_bind_cpu(r,h,0,0));
    CHECK(stack_owner_bind_task(r,h,92,113,"guarded-owner"));
    CHECK(stack_owner_publish(r,h));
    CHECK(!stack_owner_publish(r,h));
    CHECK(!stack_owner_return_prepared(r,h));
    stack_owner_view_t v;
    CHECK(stack_owner_read(r,&v) && v.task_id==92 && v.task_lifecycle==113 && v.ever_published);
    CHECK(!strcmp(v.name,"guarded-owner") && v.physical==0 && v.capacity==16384);
    uint64_t physical=UINT64_MAX;
    CHECK(stack_owner_payload(&v,v.base,16384,&physical) && physical==0);
    CHECK(stack_owner_payload(&v,v.end-1,1,&physical) && physical==16383);
    CHECK(!stack_owner_payload(&v,v.end,1,&physical));
    CHECK(!stack_owner_payload(&v,v.base-1,1,&physical));
    CHECK(!stack_owner_payload(&v,v.base,16385,&physical));
    CHECK(stack_owner_guard_lookup(records,v.guard,&v));
    CHECK(stack_owner_guard_lookup(records,v.base-1,&v));
    CHECK(!stack_owner_guard_lookup(records,v.base,&v));
    CHECK(!stack_owner_begin_retire(r,h,92,114,false));
    CHECK(!stack_owner_begin_retire(r,(stack_handle_t){h.generation-1,slot},92,113,false));
    CHECK(!stack_owner_begin_retire(r,h,92,113,true));
    CHECK(stack_owner_begin_retire(r,h,92,113,false));
    CHECK(!stack_owner_reserve(r,slot,STACK_KIND_TASK,16384,0x200000,true,0,&next));
    CHECK(stack_owner_read(r,&v) && !stack_owner_payload(&v,v.base,1,&physical));
    CHECK(!stack_owner_finish_retire(r,h,721,false));
    CHECK(stack_owner_read(r,&v) && v.state==STACK_QUARANTINED && v.retirement_generation==721);
    CHECK(!stack_owner_finish_retire(r,h,721,true));
    CHECK(!stack_owner_reserve(r,slot,STACK_KIND_TASK,16384,0x200000,true,0,&next));

    r=&records[slot+1];
    CHECK(stack_owner_reserve(r,slot+1,STACK_KIND_TASK,16384,0x200000,true,0,&h));
    CHECK(stack_owner_mapped(r,h,false));
    CHECK(stack_owner_bind_task(r,h,93,114,"next"));CHECK(stack_owner_publish(r,h));
    CHECK(stack_owner_begin_retire(r,h,93,114,false));
    CHECK(stack_owner_finish_retire(r,h,722,true));
    CHECK(!stack_owner_guard_lookup(records,STACK_REGION_BASE+(slot+1)*STACK_SLOT_BYTES,&v));
    CHECK(stack_owner_reserve(r,slot+1,STACK_KIND_TASK,16384,0x300000,true,0,&next));
    CHECK(next.generation>h.generation && !stack_owner_cancel_reservation(r,h));
    CHECK(stack_owner_cancel_reservation(r,next));
    r->cells[STACK_META_GENERATION]=UINT64_MAX;
    CHECK(!stack_owner_reserve(r,slot+1,STACK_KIND_TASK,16384,0x300000,true,0,&next));
    r->sequence=UINT64_MAX-1;
    CHECK(!stack_owner_cancel_reservation(r,next));

    r=&records[0];
    CHECK(stack_owner_reserve(r,0,STACK_KIND_IST1,32768,0x400000,false,0,&h));
    CHECK(stack_owner_mapped(r,h,false));
    CHECK(!stack_owner_bind_cpu(r,h,HOBBYOS_MAX_CPUS,4));
    CHECK(stack_owner_bind_cpu(r,h,HOBBYOS_MAX_CPUS-1,39));CHECK(stack_owner_publish(r,h));
    CHECK(!stack_owner_begin_retire(r,h,1,1,false));
    CHECK(!stack_owner_begin_retire(r,h,0,0,true));
    uint64_t seq=r->sequence;r->sequence=seq+1;
    memset(&v,0x3c,sizeof(v));stack_owner_view_t unchanged=v;
    CHECK(!stack_owner_read(r,&v) && !memcmp(&v,&unchanged,sizeof(v)));
    CHECK(!stack_owner_guard_lookup(records,STACK_REGION_BASE,&v));r->sequence=seq;
    CHECK(stack_owner_guard_lookup(records,STACK_REGION_BASE,&v) && v.cpu==31 && v.apic==39);
    for (unsigned field=STACK_META_SLOT;field<=STACK_META_APIC;++field) {
        uint64_t saved=r->cells[field];r->cells[field]=saved+UINT64_C(0x100000000);
        CHECK(!stack_owner_read(r,&v));r->cells[field]=saved;
    }
    const unsigned fields[]={STACK_META_KIND,STACK_META_STATE,STACK_META_OWNED,STACK_META_PREPARED,STACK_META_PUBLISHED,STACK_META_NAME};
    for (unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);++i) {
        unsigned field=fields[i];uint64_t saved=r->cells[field];r->cells[field]=UINT64_MAX;
        CHECK(!stack_owner_read(r,&v));r->cells[field]=saved;
    }
    CHECK(stack_owner_read(r,&v));
    CHECK(!stack_owner_reserve(&records[2],2,STACK_KIND_AP_BOOT,16384,0x500000,false,0,&next));
    r=&records[STACK_BSP_OWNER];uint64_t guard=KERNEL_IMAGE_OFFSET+0x500000;
    CHECK(!stack_owner_reserve(r,STACK_BSP_OWNER,STACK_KIND_BSP_BOOT,16384,0x501000,true,guard,&h));
    CHECK(stack_owner_reserve(r,STACK_BSP_OWNER,STACK_KIND_BSP_BOOT,16384,0x501000,false,guard,&h));
    CHECK(stack_owner_mapped(r,h,false));CHECK(stack_owner_bind_cpu(r,h,0,0));CHECK(stack_owner_publish(r,h));
    CHECK(stack_owner_guard_lookup(records,guard,&v) && v.kind==STACK_KIND_BSP_BOOT);
    CHECK(!stack_owner_guard_lookup(records,guard-1,&v));
    CHECK(!stack_owner_guard_lookup(records,guard+4096,&v));
}

int main(void)
{
    geometry();ownership();
    pthread_t threads[24];
    for(uintptr_t i=0;i<24;++i) CHECK(!pthread_create(&threads[i],NULL,creator,(void *)i));
    for(unsigned i=0;i<24;++i) CHECK(!pthread_join(threads[i],NULL));
    CHECK(winners==1 && stack_owner_cancel_reservation(&raced,winner));
    printf("{\"status\":\"PASS\",\"checks\":%u,\"concurrent_creators\":24,\"admitted_owners\":1,\"hardware_faults\":\"NOT_HOSTED\"}\n",checks);
    return 0;
}
