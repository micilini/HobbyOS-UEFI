#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/src/memory/guard_probe.h"
#include "../kernel/src/memory/stack_fault.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"guard check %u line %u: %s\n",checks,__LINE__,#x);exit(1); } } while (0)
extern unsigned guard_probe_read(uint64_t), guard_probe_recovery(uint64_t);
extern uint64_t guard_payload_read(uint64_t);
typedef struct { uint64_t before_sp,after_sp,before_flags,flags,returned,low,high,registers[6]; } abi_result_t;
extern void stack_guard_host_abi(unsigned (*entry)(uint64_t),uint64_t address,abi_result_t *out);

static stack_owner_view_t task_owner(void)
{
    return (stack_owner_view_t){.guard=STACK_REGION_BASE+STACK_CPU_SLOTS*STACK_SLOT_BYTES,
        .base=STACK_REGION_BASE+STACK_CPU_SLOTS*STACK_SLOT_BYTES+4096,
        .end=STACK_REGION_BASE+STACK_CPU_SLOTS*STACK_SLOT_BYTES+4096+16384,
        .physical=0x100000000,.generation=31,.task_id=92,.task_lifecycle=184,
        .slot=STACK_CPU_SLOTS,.capacity=16384,.kind=STACK_KIND_TASK,.state=STACK_LIVE,
        .owned=true,.ever_published=true};
}
static bool match(const guard_probe_state_t *s,const stack_owner_view_t *v)
{
    return guard_probe_match(s,v,6,0x101000,3,19,0xffffffff82000100,
        task_owner().guard,0,8,0xffffffff82000100,0xffffffff82000110);
}
static void matcher(void)
{
    stack_owner_view_t v=task_owner();
    guard_probe_state_t s={.generation=6,.root=0x101000,.address=v.guard,
        .owner_generation=v.generation,.owner_slot=v.slot,.cpu=3,.apic=19,
        .site=0xffffffff82000100,.recovery=0xffffffff82000110,.armed=true};
    CHECK(match(&s,&v));
    for(unsigned error=0;error<1024;++error)
        CHECK(guard_probe_match(&s,&v,6,s.root,3,19,s.site,s.address,error,8,s.site,s.recovery)==(error==0));
#define BAD_STATE(field,value) do { guard_probe_state_t bad=s;bad.field=(value);CHECK(!match(&bad,&v)); } while(0)
    BAD_STATE(generation,0);BAD_STATE(generation,5);BAD_STATE(root,0);BAD_STATE(root,0x102000);
    BAD_STATE(cpu,2);BAD_STATE(apic,3);BAD_STATE(address,0);BAD_STATE(address,v.guard+1);
    BAD_STATE(owner_generation,30);BAD_STATE(owner_slot,v.slot+1);BAD_STATE(site,s.site+1);
    BAD_STATE(recovery,s.recovery+1);BAD_STATE(armed,false);BAD_STATE(seen,true);
#undef BAD_STATE
#define BAD_OWNER(field,value) do { stack_owner_view_t bad=v;bad.field=(value);CHECK(!match(&s,&bad)); } while(0)
    BAD_OWNER(generation,32);BAD_OWNER(slot,v.slot+1);BAD_OWNER(state,STACK_RETIRING);
    BAD_OWNER(state,STACK_FREE);BAD_OWNER(base,v.base+4096);
#undef BAD_OWNER
    CHECK(!guard_probe_match(&s,&v,6,s.root,3,19,s.site,s.address,0,0x1b,s.site,s.recovery));
    CHECK(!guard_probe_match(&s,&v,6,s.root,3,19,s.site+1,s.address,0,8,s.site,s.recovery));
    CHECK(!guard_probe_match(&s,&v,6,s.root,3,19,s.site,0,0,8,s.site,s.recovery));
    CHECK(!guard_probe_match(&s,&v,6,s.root,3,19,s.site,s.address,0,8,s.site,s.site));
    s.address=v.base-1;
    CHECK(guard_probe_match(&s,&v,6,s.root,3,19,s.site,s.address,0,8,s.site,s.recovery));
    s.address=v.base;
    CHECK(!guard_probe_match(&s,&v,6,s.root,3,19,s.site,s.address,0,8,s.site,s.recovery));
    CHECK(!match(NULL,&v));CHECK(!match(&s,NULL));
}
static void diagnosis(void)
{
    stack_owner_view_t owner=task_owner(),ist={.guard=STACK_REGION_BASE,.base=STACK_REGION_BASE+4096,
        .end=STACK_REGION_BASE+4096+32768,.generation=7,.state=STACK_LIVE,.kind=STACK_KIND_IST1,
        .cpu=3,.apic=19,.slot=0};
    stack_fault_facts_t f={.address=owner.base-16,.rsp=owner.base-8,.rip=0x12345678,
        .handler_rsp=ist.end-512,.vector=8,.cpu=3,.apic=19,.cs=8};
    CHECK(stack_fault_classify(&f,&owner,&ist)==STACK_FAULT_OVERFLOW_ASSOCIATED);
#define BAD_FAULT(field,value) do { stack_fault_facts_t bad=f;bad.field=(value);CHECK(stack_fault_classify(&bad,&owner,&ist)==STACK_FAULT_UNKNOWN); } while(0)
    BAD_FAULT(error,1);BAD_FAULT(vector,13);BAD_FAULT(address,owner.base);
    BAD_FAULT(address,owner.guard-1);BAD_FAULT(rsp,owner.end-8);BAD_FAULT(rsp,owner.guard-1);
    BAD_FAULT(rsp,f.address-1);BAD_FAULT(handler_rsp,ist.end);BAD_FAULT(handler_rsp,owner.base);
    BAD_FAULT(cpu,2);BAD_FAULT(apic,20);
#undef BAD_FAULT
    stack_owner_view_t bad=owner;bad.state=STACK_FREE;
    CHECK(stack_fault_classify(&f,&bad,&ist)==STACK_FAULT_UNKNOWN);
    bad=owner;bad.task_id=0;CHECK(stack_fault_classify(&f,&bad,&ist)==STACK_FAULT_UNKNOWN);
    bad=owner;bad.task_lifecycle=0;CHECK(stack_fault_classify(&f,&bad,&ist)==STACK_FAULT_UNKNOWN);
    bad=owner;bad.kind=STACK_KIND_AP_BOOT;CHECK(stack_fault_classify(&f,&bad,&ist)==STACK_FAULT_UNKNOWN);
    bad=ist;bad.kind=STACK_KIND_IST2;CHECK(stack_fault_classify(&f,&owner,&bad)==STACK_FAULT_UNKNOWN);
    bad=ist;bad.slot=owner.slot;CHECK(stack_fault_classify(&f,&owner,&bad)==STACK_FAULT_UNKNOWN);
    CHECK(stack_fault_classify(&f,&owner,NULL)==STACK_FAULT_UNKNOWN);
    /* Abort RIP/CS are raw facts, not a precise original instruction oracle. */
    f.rip=0;f.cs=0;CHECK(stack_fault_classify(&f,&owner,&ist)==STACK_FAULT_OVERFLOW_ASSOCIATED);
    f.vector=14;f.rsp=owner.end-8;f.handler_rsp=0;
    CHECK(stack_fault_classify(&f,&owner,NULL)==STACK_FAULT_GUARD_ACCESS);
}
static void abi(void)
{
    const uint64_t values[]={0x1020304050607080,0x8877665544332211,0x1122334455667788,
        0x99aabbccddeeff00,0x0f1e2d3c4b5a6978,0xf0e1d2c3b4a5968};
    unsigned char byte=0x5a;
    for(unsigned path=0;path<2;++path) {
        abi_result_t out={0};stack_guard_host_abi(path ? guard_probe_recovery : guard_probe_read,(uintptr_t)&byte,&out);
        CHECK(out.returned==path);CHECK(out.before_sp==out.after_sp && !(out.before_sp&15));
        CHECK(out.low==values[0] && out.high==values[1]);
        CHECK((out.flags&0xed5)==(out.before_flags&0xed5));
        for(unsigned i=0;i<6;++i) CHECK(out.registers[i]==values[i]);
    }
    CHECK(byte==0x5a && guard_payload_read((uintptr_t)&byte)==0x5a);
    CHECK(guard_probe_read((uintptr_t)&byte)==0); /* Permitted access is not denial. */
}
int main(void)
{
    matcher();diagnosis();abi();
    printf("STACK_GUARD_PROBE_HOST checks=%u status=PASS hardware_faults=NOT_HOSTED\n",checks);return 0;
}
