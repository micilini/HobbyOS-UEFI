#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "../kernel/src/memory/protection_probe.h"
static unsigned checks;
#define CHECK(x) do {unsigned n=__atomic_add_fetch(&checks,1,__ATOMIC_RELAXED);if(!(x)){fprintf(stderr,"probe check %u line %u: %s\n",n,__LINE__,#x);abort();}} while(0)
extern unsigned protection_probe_store(void *,unsigned),protection_probe_fetch(void *);
extern char protection_fetch_recovery[];
static protection_target_t target(unsigned i)
{
    uint64_t address=UINT64_C(0xffffffff82010000)+4096*i;
    return (protection_target_t){address,0,i<4 ? UINT64_C(0xffffffff82000100) : address,
        UINT64_C(0xffffffff82000200),i/2,i%2,0x5a};
}
static protection_fault_t event(const protection_target_t *t)
{return (protection_fault_t){7,3,0x100000,t->site,t->address,t->kind==PROTECTION_FETCH_DATA?17:3,8,2,6};}
static void record(protection_probe_state_t *s,const protection_fault_t *e)
{s->fault_rip=e->rip;s->fault_address=e->address;s->fault_error=e->error;s->fault_cs=e->cs;s->armed=false;s->seen=true;}
static void matcher(void)
{
    for(unsigned i=0;i<PROTECTION_ATTEMPTS;++i) {
        protection_target_t t=target(i);protection_fault_t e=event(&t);protection_probe_state_t s={0};
        CHECK(protection_probe_arm(&s,&t,&e,i,true));
        CHECK(!protection_probe_arm(&s,&t,&e,i,true));
        CHECK(protection_probe_match(&s,&t,&e));
        for(unsigned error=0;error<256;++error){protection_fault_t b=e;b.error=error;
            CHECK(protection_probe_match(&s,&t,&b)==(error==e.error));}
#define BAD_EVENT(field,value) do{protection_fault_t b=e;b.field=(value);CHECK(!protection_probe_match(&s,&t,&b));}while(0)
        BAD_EVENT(generation,0);BAD_EVENT(generation,e.generation+1);BAD_EVENT(collection,4);
        BAD_EVENT(root,0x101000);BAD_EVENT(slot,3);BAD_EVENT(apic,7);BAD_EVENT(cs,0x1b);
        BAD_EVENT(rip,e.rip+1);BAD_EVENT(address,e.address+1);BAD_EVENT(address,0);
#undef BAD_EVENT
#define BAD_STATE(field,value) do{protection_probe_state_t b=s;b.field=(value);CHECK(!protection_probe_match(&b,&t,&e));}while(0)
        BAD_STATE(armed,false);BAD_STATE(seen,true);BAD_STATE(completed,true);BAD_STATE(generation,0);
        BAD_STATE(attempt,6);BAD_STATE(kind,3);BAD_STATE(alias,3);BAD_STATE(site,s.site+1);
        BAD_STATE(recovery,s.recovery+1);BAD_STATE(address,s.address+1);
#undef BAD_STATE
        CHECK(!protection_probe_complete(&s,&t,0,t.original));
        record(&s,&e);CHECK(!protection_probe_match(&s,&t,&e));
        CHECK(!protection_probe_complete(&s,&t,0,t.original));
        CHECK(!protection_probe_complete(&s,&t,1,t.original^1));
        CHECK(protection_probe_complete(&s,&t,1,t.original));
        CHECK(!protection_probe_complete(&s,&t,1,t.original));
        s=(protection_probe_state_t){0};CHECK(protection_probe_arm(&s,&t,&e,i,false)==(i<4));
        s=(protection_probe_state_t){0};e.root=UINT64_C(0x10000000000000);
        CHECK(!protection_probe_arm(&s,&t,&e,i,true));
        e=event(&t);e.generation=0;CHECK(!protection_probe_arm(&s,&t,&e,i,true));
    }
}
static cpu_request_storage_t storage;
static bool execute(void *context,const cpu_request_spec_t *spec,cpu_request_sample_t *sample)
{
    (void)context;
    CHECK(spec->operation==CPU_REQUEST_PROTECTION && !spec->interval.pages);
    CHECK(storage.publication[sample->slot]==((spec->generation<<2)|2));
    for(unsigned i=0;i<6;++i){protection_target_t t=target(i);protection_fault_t e=event(&t);
        e.generation=spec->generation;e.slot=sample->slot;e.apic=sample->apic;
        protection_probe_state_t s={0};CHECK(protection_probe_arm(&s,&t,&e,i,true));
        CHECK(protection_probe_match(&s,&t,&e));record(&s,&e);CHECK(protection_probe_complete(&s,&t,1,t.original));
        CHECK(storage.publication[sample->slot]==((spec->generation<<2)|2));}
    sample->value=63;return true;
}
static void service(void)
{
    cpu_request_spec_t spec={.root=0x100000,.targets=3,.apic={1,9},.operation=CPU_REQUEST_PROTECTION};
    CHECK(cpu_request_admit(&storage));
    spec.interval=(tlb_interval_t){0,0,1,TLB_PAGES};CHECK(cpu_request_publish(&storage,&spec)==CPU_REQUEST_INVALID);
    spec.interval=(tlb_interval_t){0};CHECK(cpu_request_publish(&storage,&spec)==CPU_REQUEST_OK);
    CHECK(cpu_request_handle(&storage,0,1,execute,NULL));CHECK(!cpu_request_handle(&storage,0,1,execute,NULL));
    cpu_request_result_t r={0};cpu_request_collect(&storage,&r);CHECK(r.acknowledged==1 && r.missing==2);
    CHECK(cpu_request_handle(&storage,1,9,execute,NULL));cpu_request_collect(&storage,&r);CHECK(r.acknowledged==3 && !r.missing);
    CHECK(!storage.samples[0].loads && !storage.samples[1].loads);CHECK(cpu_request_retire(&storage,true));
    CHECK(!cpu_request_handle(&storage,1,9,execute,NULL));
    CHECK(cpu_request_admit(&storage));CHECK(cpu_request_publish(&storage,&spec)==CPU_REQUEST_OK);
    CHECK(!cpu_request_retire(&storage,false) && storage.quarantined);
    CHECK(cpu_request_admit(&storage));CHECK(cpu_request_publish(&storage,&spec)==CPU_REQUEST_QUARANTINED);
}

enum { CONCURRENT_CPUS=24 };
typedef struct {
    cpu_request_storage_t request;
    pthread_barrier_t entered,resume;
    protection_probe_state_t states[CONCURRENT_CPUS][PROTECTION_ATTEMPTS];
    unsigned calls[CONCURRENT_CPUS],completed[CONCURRENT_CPUS],active;
} parallel_probes_t;
static parallel_probes_t parallel;
static void barrier(pthread_barrier_t *value)
{
    int result=pthread_barrier_wait(value);
    CHECK(result==0 || result==PTHREAD_BARRIER_SERIAL_THREAD);
}
static bool parallel_execute(void *context,const cpu_request_spec_t *spec,cpu_request_sample_t *sample)
{
    parallel_probes_t *p=context;unsigned slot=sample->slot;
    CHECK(slot<CONCURRENT_CPUS && spec->operation==CPU_REQUEST_PROTECTION && !spec->interval.pages);
    CHECK(++p->calls[slot]==1);
    __atomic_add_fetch(&p->active,1,__ATOMIC_RELAXED);
    for(unsigned i=0;i<PROTECTION_ATTEMPTS;++i) {
        protection_target_t t=target(i);protection_fault_t e=event(&t);
        e.generation=spec->generation;e.slot=slot;e.apic=sample->apic;
        protection_probe_state_t *state=&p->states[slot][i];
        CHECK(protection_probe_arm(state,&t,&e,i,true));
        protection_fault_t stolen=e;stolen.slot=(slot+1)%CONCURRENT_CPUS;
        CHECK(!protection_probe_match(state,&t,&stolen));
        CHECK(protection_probe_match(state,&t,&e));record(state,&e);
        CHECK(protection_probe_complete(state,&t,1,t.original));
        CHECK(!state->armed && state->completed);
        ++p->completed[slot];
        CHECK(__atomic_load_n(&p->request.publication[slot],__ATOMIC_ACQUIRE)==((spec->generation<<2)|2));
        if(i==2) {barrier(&p->entered);barrier(&p->resume);}
    }
    sample->value=63;sample->loads=0;
    __atomic_sub_fetch(&p->active,1,__ATOMIC_RELAXED);
    return true;
}
static void *parallel_target(void *argument)
{
    unsigned slot=(unsigned)(uintptr_t)argument;
    CHECK(cpu_request_handle(&parallel.request,slot,100+slot,parallel_execute,&parallel));
    CHECK(!cpu_request_handle(&parallel.request,slot,100+slot,parallel_execute,&parallel));
    return NULL;
}
static void concurrent_service(bool timeout)
{
    memset(&parallel,0,sizeof(parallel));
    cpu_request_spec_t spec={.root=0x100000,.targets=(1u<<CONCURRENT_CPUS)-1,.operation=CPU_REQUEST_PROTECTION};
    for(unsigned slot=0;slot<CONCURRENT_CPUS;++slot) spec.apic[slot]=100+slot;
    CHECK(!pthread_barrier_init(&parallel.entered,NULL,CONCURRENT_CPUS+1));
    CHECK(!pthread_barrier_init(&parallel.resume,NULL,CONCURRENT_CPUS+1));
    CHECK(cpu_request_admit(&parallel.request));
    CHECK(cpu_request_publish(&parallel.request,&spec)==CPU_REQUEST_OK);
    pthread_t workers[CONCURRENT_CPUS];
    for(unsigned slot=0;slot<CONCURRENT_CPUS;++slot)
        CHECK(!pthread_create(&workers[slot],NULL,parallel_target,(void *)(uintptr_t)slot));
    barrier(&parallel.entered);
    /* Every real handler is claimed and stopped halfway through its six
     * results. No result or disarm can be inferred from an aggregate count. */
    CHECK(__atomic_load_n(&parallel.active,__ATOMIC_RELAXED)==CONCURRENT_CPUS);
    CHECK(!cpu_request_admit(&parallel.request));
    CHECK(cpu_request_publish(&parallel.request,&spec)==CPU_REQUEST_INVALID);
    cpu_request_result_t result={0};cpu_request_collect(&parallel.request,&result);
    CHECK(!result.acknowledged && !result.failed && result.missing==spec.targets);
    for(unsigned slot=0;slot<CONCURRENT_CPUS;++slot) {
        CHECK(parallel.calls[slot]==1 && parallel.completed[slot]==3);
        CHECK(!cpu_request_handle(&parallel.request,slot,100+slot,parallel_execute,&parallel));
    }
    cpu_request_spec_t retained=parallel.request.spec;
    if(timeout) {
        CHECK(!cpu_request_retire(&parallel.request,false) && parallel.request.quarantined);
        CHECK(cpu_request_admit(&parallel.request));
        CHECK(cpu_request_publish(&parallel.request,&spec)==CPU_REQUEST_QUARANTINED);
        CHECK(!memcmp(&retained,&parallel.request.spec,sizeof(retained)));
    }
    barrier(&parallel.resume);
    for(unsigned slot=0;slot<CONCURRENT_CPUS;++slot) CHECK(!pthread_join(workers[slot],NULL));
    CHECK(!__atomic_load_n(&parallel.active,__ATOMIC_RELAXED));
    cpu_request_collect(&parallel.request,&result);
    CHECK(result.acknowledged==spec.targets && !result.failed && !result.missing);
    for(unsigned slot=0;slot<CONCURRENT_CPUS;++slot) {
        const cpu_request_sample_t *sample=&parallel.request.samples[slot];
        CHECK(parallel.calls[slot]==1 && parallel.completed[slot]==PROTECTION_ATTEMPTS);
        CHECK(sample->slot==slot && sample->apic==100+slot && sample->value==63 && !sample->loads);
        for(unsigned i=0;i<PROTECTION_ATTEMPTS;++i) {
            const protection_probe_state_t *state=&parallel.states[slot][i];
            CHECK(state->generation==retained.generation && state->slot==slot && state->apic==100+slot);
            CHECK(state->completed && state->seen && !state->armed);
        }
    }
    if(timeout) {
        CHECK(parallel.request.quarantined && !memcmp(&retained,&parallel.request.spec,sizeof(retained)));
        CHECK(cpu_request_publish(&parallel.request,&spec)==CPU_REQUEST_QUARANTINED);
    } else CHECK(cpu_request_retire(&parallel.request,true));
    CHECK(!pthread_barrier_destroy(&parallel.entered));CHECK(!pthread_barrier_destroy(&parallel.resume));
}
typedef struct {
    uint64_t returned,flags,before_sp,after_sp,low_canary,high_canary,registers[6],before_flags,target;
} protection_abi_result_t;
extern void protection_host_abi(unsigned,void *,unsigned,protection_abi_result_t *);
extern char protection_host_ret[];
static void recovery_abi(void)
{
    const uint64_t expected[]={UINT64_C(0x1020304050607080),UINT64_C(0x8877665544332211),
        UINT64_C(0x1122334455667788),UINT64_C(0x99aabbccddeeff00),
        UINT64_C(0x0f1e2d3c4b5a6978),UINT64_C(0xf0e1d2c3b4a5968)};
    uint8_t byte=0x5a;
    for(unsigned path=0;path<4;++path) {
        protection_abi_result_t actual={0};
        void *target=path==0 || path==3 ? (void *)&byte :
            path==1 ? (void *)protection_host_ret : (void *)protection_fetch_recovery;
        protection_host_abi(path==0 ? 0 : path==3 ? 2 : 1,target,byte,&actual);
        CHECK(actual.returned==(path>=2));
        CHECK(actual.before_sp==actual.after_sp && !(actual.before_sp&15));
        CHECK(actual.low_canary==expected[0] && actual.high_canary==expected[1]);
        for(unsigned i=0;i<6;++i) CHECK(actual.registers[i]==expected[i]);
        CHECK(actual.target==(uintptr_t)target && byte==0x5a);
        CHECK((actual.flags&0x600)==(actual.before_flags&0x600));
        /* XOR establishes ZF/PF before the real store. Fetch's final ADD of
         * stack padding sets arithmetic flags; RET and recovery MOV preserve
         * them. AF is not asserted after XOR, where it is undefined. */
        unsigned arithmetic=0x44;
        if(path==1 || path==2) {
            unsigned value=(unsigned)(actual.before_sp-8)&255,parity=0;
            for(unsigned bit=0;bit<8;++bit) parity^=(value>>bit)&1;
            arithmetic=parity ? 0 : 4;
        }
        CHECK((actual.flags&0x8c5)==arithmetic);
    }
}

int main(void)
{
    matcher();service();concurrent_service(false);concurrent_service(true);recovery_abi();
    /* Real assembly, with deliberately permitted host accesses. Both paths
     * return unexpected-success zero; the executable recovery stub balances
     * the very same indirect-call return address and stack padding. */
    volatile uint64_t canary=UINT64_C(0x12345678fedcba98);uint8_t byte=0x5a;
    CHECK(protection_probe_store(&byte,byte)==0 && byte==0x5a);
    CHECK(protection_probe_fetch(protection_fetch_recovery)==1);
    CHECK(canary==UINT64_C(0x12345678fedcba98));
    printf("PROTECTION_PROBE_HOST checks=%u status=PASS concurrent_targets=24 schedules=2 hardware_faults=NOT_HOSTED\n",checks);return 0;
}
