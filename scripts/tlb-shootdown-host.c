#include "../kernel/src/apic/lapic_delivery.h"
#include "../kernel/src/core/interrupt_vectors.h"
#include "../kernel/src/smp/cpu_request.h"
#include "../kernel/src/memory/tlb_deferred.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { assert(x); __atomic_add_fetch(&checks, 1, __ATOMIC_RELAXED); } while (0)
typedef struct {
    uint64_t flags, value, cr3, cr4, addresses[64];
    unsigned busy, reads, writes, pauses, fences, saves, restores, pages, reloads;
    unsigned fail, trace[16], count;
    bool post_busy;
} machine_t;
static void event(machine_t *m, unsigned value) { if (m->count < 16) m->trace[m->count++] = value; }
static uint64_t save(void *v) { machine_t *m=v; uint64_t f=m->flags; m->flags &= ~512u; ++m->saves; event(m,1); return f; }
static void restore(void *v,uint64_t f) { machine_t *m=v; CHECK(!(m->flags&512));m->flags=f;++m->restores;event(m,6); }
static bool pending(void *v) { machine_t *m=v;CHECK(!(m->flags&512));++m->reads;return m->reads<=m->busy || (m->post_busy && m->writes==2); }
static bool write32(void *v,uint32_t reg,uint32_t value) {
    machine_t *m=v; CHECK(!(m->flags&512)); CHECK(m->fences==1); ++m->writes;
    if(reg==0x310) { CHECK(m->writes==1);m->value=(uint64_t)(value>>24)<<32;event(m,3); }
    else { CHECK(reg==0x300 && m->writes==2);m->value|=value;event(m,4); }
    /* A maskable interrupt attempted here cannot split the pair. */
    CHECK(!(m->flags&512)); return m->fail!=m->writes;
}
static bool write64(void *v,uint32_t reg,uint64_t value) {
    machine_t *m=v;CHECK(!(m->flags&512));CHECK(reg==0x830 && m->fences==1);
    m->value=value;++m->writes;event(m,5);return !m->fail;
}
static void fence(void *v) { machine_t *m=v;CHECK(!(m->flags&512));++m->fences;event(m,2); }
static void relax(void *v) { ++((machine_t*)v)->pauses; }
static const lapic_delivery_ops_t sender={save,restore,pending,write32,write64,fence,relax};
static void sender_tests(void) {
    for(unsigned x2=0;x2<2;++x2) for(unsigned enabled=0;enabled<2;++enabled) {
        machine_t m={.flags=2u+enabled*512u};
        CHECK(lapic_delivery_send(&sender,&m,true,x2,x2?0x12345678:7,0xe1,LAPIC_MESSAGE_FIXED)==LAPIC_DELIVERY_SUBMITTED);
        CHECK(m.flags==2u+enabled*512u && m.saves==1 && m.restores==1);
        CHECK(m.value==((uint64_t)(x2?0x12345678:7)<<32|0x40e1));
        unsigned expected0[]={1,2,3,4,6},expected1[]={1,2,5,6};
        CHECK(m.count==(x2?4u:5u));CHECK(!memcmp(m.trace,x2?expected1:expected0,m.count*sizeof(unsigned)));
    }
    const uint32_t bad[]={0,31,32,64,255,256,UINT32_MAX};
    for(unsigned n=0;n<sizeof(bad)/sizeof(*bad);++n) {
        machine_t m={.flags=514};
        CHECK(lapic_delivery_send(&sender,&m,true,false,1,bad[n],LAPIC_MESSAGE_FIXED)==LAPIC_DELIVERY_INVALID);
        CHECK(!m.writes&&!m.saves);
    }
    for(unsigned x2=0;x2<2;++x2) {
        machine_t m={.flags=514};
        CHECK(lapic_delivery_send(&sender,&m,true,x2,x2?UINT32_MAX:255,35,LAPIC_MESSAGE_FIXED)==LAPIC_DELIVERY_DESTINATION);
        CHECK(!m.writes&&!m.saves);
        CHECK(lapic_delivery_send(&sender,&m,false,x2,1,35,LAPIC_MESSAGE_FIXED)==LAPIC_DELIVERY_UNAVAILABLE);
    }
    machine_t m={.flags=514,.busy=UINT32_MAX};
    CHECK(lapic_delivery_send(&sender,&m,true,false,1,35,LAPIC_MESSAGE_FIXED)==LAPIC_DELIVERY_PRE_TIMEOUT);
    CHECK(m.reads==100000 && m.pauses==100000 && !m.writes && m.flags==514);
    m=(machine_t){.flags=2,.post_busy=true};
    CHECK(lapic_delivery_send(&sender,&m,true,false,1,35,LAPIC_MESSAGE_FIXED)==LAPIC_DELIVERY_INDETERMINATE);
    CHECK(m.reads==100001 && m.pauses==100000 && m.writes==2 && m.flags==2);
    for(unsigned failure=1;failure<=2;++failure) {
        m=(machine_t){.flags=514,.fail=failure};
        CHECK(lapic_delivery_send(&sender,&m,true,false,1,35,LAPIC_MESSAGE_FIXED)==
              (failure==1?LAPIC_DELIVERY_WRITE_FAILED:LAPIC_DELIVERY_INDETERMINATE));
        CHECK(m.writes==failure && m.flags==514);
    }
    uint64_t raw=0xdead;
    CHECK(lapic_delivery_encode(false,254,0,LAPIC_MESSAGE_INIT_ASSERT,&raw)==LAPIC_DELIVERY_SUBMITTED && raw==UINT64_C(0xfe0000c500));
    CHECK(lapic_delivery_encode(false,7,0,LAPIC_MESSAGE_INIT_DEASSERT,&raw)==LAPIC_DELIVERY_SUBMITTED && raw==UINT64_C(0x700008500));
    CHECK(lapic_delivery_encode(false,7,8,LAPIC_MESSAGE_STARTUP,&raw)==LAPIC_DELIVERY_SUBMITTED && raw==UINT64_C(0x700004608));
    CHECK(lapic_delivery_encode(false,7,256,LAPIC_MESSAGE_STARTUP,&raw)==LAPIC_DELIVERY_INVALID && raw==UINT64_C(0x700004608));
}
static uint64_t cr3(void *v) {return ((machine_t*)v)->cr3;}
static uint64_t cr4(void *v) {return ((machine_t*)v)->cr4;}
static void invlpg(void *v,uint64_t a) {machine_t *m=v;CHECK(m->pages<64);m->addresses[m->pages++]=a;}
static void reload(void *v,uint64_t a) {machine_t *m=v;CHECK(a==m->cr3);++m->reloads;}
static const tlb_local_ops_t local={cr3,cr4,invlpg,reload};
static void interval_tests(void) {
    const uint64_t sizes[]={0,1,4095,4096,4097,63*4096,64*4096,65*4096,513*4096};
    for(unsigned n=0;n<sizeof(sizes)/sizeof(*sizes);++n) {
        tlb_interval_t i;CHECK(tlb_interval_normalize(0x1ff000,sizes[n],&i));CHECK(tlb_interval_valid(&i));
        uint64_t pages=(sizes[n]+4095)/4096;
        CHECK(i.pages==pages && i.strategy==(pages==0?TLB_EMPTY:pages<=64?TLB_PAGES:TLB_CONTEXT));
        machine_t m={.cr3=0x12345018,.cr4=0x20};
        CHECK(tlb_local_execute(&i,0x12345000,&local,&m));
        CHECK(m.pages==(pages<=64?pages:0) && m.reloads==(pages>64));
        for(unsigned p=0;p<m.pages;++p)CHECK(m.addresses[p]==0x1ff000+p*4096u);
        for(unsigned bit=0;bit<32;++bit) if(bit!=5) {
            m.cr4=0x20|(1u<<bit); unsigned before=m.pages+m.reloads;
            CHECK(!tlb_local_execute(&i,0x12345000,&local,&m));CHECK(m.pages+m.reloads==before);
        }
    }
    tlb_interval_t sentinel={5,6,7,TLB_CONTEXT},i=sentinel;
    const uint64_t invalid[][2]={{UINT64_MAX,2},{0x7fffffffffff,2},{0x800000000000,0},{0xffff7fffffffffff,1},{0,UINT64_MAX}};
    for(unsigned n=0;n<sizeof(invalid)/sizeof(*invalid);++n) {
        CHECK(!tlb_interval_normalize(invalid[n][0],invalid[n][1],&i));CHECK(!memcmp(&i,&sentinel,sizeof(i)));
    }
    CHECK(tlb_interval_normalize(UINT64_MAX,1,&i) && i.first==UINT64_C(0xfffffffffffff000) && i.last==i.first && i.pages==1);
    CHECK(tlb_interval_normalize(4095,2,&i) && i.first==0 && i.last==4096 && i.pages==2);
    i.strategy=TLB_CONTEXT;CHECK(!tlb_interval_valid(&i));
}
static cpu_request_storage_t storage;
static bool execute(void *v,const cpu_request_spec_t *spec,cpu_request_sample_t *sample) {
    (void)v;
    CHECK(__atomic_load_n(&storage.publication[sample->slot],__ATOMIC_ACQUIRE)==((spec->generation<<2)|2));
    sample->value=0xb0000000+sample->slot;sample->cr3=spec->root;return true;
}
static void *deliver(void *v) {
    unsigned slot=(unsigned)(uintptr_t)v;
    for(unsigned n=0;n<8;++n)(void)cpu_request_handle(&storage,slot,0x100+slot,execute,NULL);
    return NULL;
}
static void request_tests(void) {
    cpu_request_spec_t spec={.root=0x1000,.targets=UINT32_MAX,.operation=CPU_REQUEST_INVALIDATE};
    CHECK(tlb_interval_normalize(0x200000,4096,&spec.interval));
    for(unsigned slot=0;slot<32;++slot)spec.apic[slot]=0x100+slot;
    CHECK(cpu_request_admit(&storage));CHECK(!cpu_request_admit(&storage));
    spec.apic[31]=spec.apic[0];CHECK(cpu_request_publish(&storage,&spec)==CPU_REQUEST_INVALID);
    spec.apic[31]=0x11f;CHECK(cpu_request_publish(&storage,&spec)==CPU_REQUEST_OK);
    cpu_request_result_t r={0};cpu_request_collect(&storage,&r);
    CHECK(!r.acknowledged && r.missing==UINT32_MAX);
    CHECK(!cpu_request_handle(&storage,32,0,execute,NULL));
    pthread_t threads[32];
    for(unsigned slot=0;slot<32;++slot)CHECK(!pthread_create(&threads[slot],NULL,deliver,(void*)(uintptr_t)slot));
    for(unsigned slot=0;slot<32;++slot)CHECK(!pthread_join(threads[slot],NULL));
    cpu_request_collect(&storage,&r);CHECK(r.acknowledged==UINT32_MAX && !r.missing && !r.failed);
    for(unsigned slot=0;slot<32;++slot)CHECK(storage.samples[slot].value==0xb0000000+slot);
    CHECK(cpu_request_retire(&storage,true));CHECK(cpu_request_admit(&storage));
    spec.targets=1;CHECK(cpu_request_publish(&storage,&spec)==CPU_REQUEST_OK);
    CHECK(cpu_request_handle(&storage,0,999,execute,NULL));cpu_request_collect(&storage,&r);
    CHECK(r.failed==1 && !r.acknowledged);CHECK(!cpu_request_retire(&storage,true));
    CHECK(cpu_request_admit(&storage));CHECK(cpu_request_publish(&storage,&spec)==CPU_REQUEST_QUARANTINED);
    storage=(cpu_request_storage_t){0};CHECK(cpu_request_admit(&storage));
    storage.next_generation=UINT64_MAX>>2;CHECK(cpu_request_publish(&storage,&spec)==CPU_REQUEST_EXHAUSTED);
    storage=(cpu_request_storage_t){0};CHECK(cpu_request_admit(&storage));CHECK(cpu_request_publish(&storage,&spec)==CPU_REQUEST_OK);
    CHECK(!cpu_request_retire(&storage,false));CHECK(!cpu_request_handle(&storage,0,0x100,execute,NULL));
    CHECK(cpu_request_admit(&storage));CHECK(cpu_request_publish(&storage,&spec)==CPU_REQUEST_QUARANTINED);
}
static unsigned late_entered, late_release;
static bool late_execute(void *v,const cpu_request_spec_t *spec,cpu_request_sample_t *sample) {
    (void)v;(void)sample;uint64_t generation=spec->generation;
    __atomic_store_n(&late_entered,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&late_release,__ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    CHECK(spec->generation==generation && spec->root==0x1000);return true;
}
static void *late_deliver(void *v) { (void)v;CHECK(cpu_request_handle(&storage,0,0,late_execute,NULL));return NULL; }
static void late_tests(void) {
    storage=(cpu_request_storage_t){0};
    cpu_request_spec_t s={.root=0x1000,.targets=1,.operation=CPU_REQUEST_INVALIDATE};
    CHECK(tlb_interval_normalize(0x4000,4096,&s.interval));
    CHECK(cpu_request_admit(&storage));CHECK(cpu_request_publish(&storage,&s)==CPU_REQUEST_OK);
    pthread_t thread;CHECK(!pthread_create(&thread,NULL,late_deliver,NULL));
    while(!__atomic_load_n(&late_entered,__ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    cpu_request_result_t r={0};cpu_request_collect(&storage,&r);CHECK(!r.acknowledged && r.missing==1);
    CHECK(!cpu_request_handle(&storage,0,0,execute,NULL));CHECK(!cpu_request_retire(&storage,false));
    CHECK(cpu_request_admit(&storage));CHECK(cpu_request_publish(&storage,&s)==CPU_REQUEST_QUARANTINED);
    __atomic_store_n(&late_release,1,__ATOMIC_RELEASE);CHECK(!pthread_join(thread,NULL));
    CHECK(storage.spec.generation==1 && storage.spec.root==0x1000);
    cpu_request_collect(&storage,&r);CHECK(r.acknowledged==1);
    CHECK(cpu_request_publish(&storage,&s)==CPU_REQUEST_QUARANTINED);
}
static void queue_tests(void) {
    tlb_deferred_queue_t q={0};tlb_completion_t handles[8],h;
    tlb_interval_t i;CHECK(tlb_interval_normalize(0x1000,4096,&i));
    for(unsigned n=0;n<8;++n) CHECK(tlb_deferred_enqueue(&q,&i,&handles[n])==TLB_DEFERRED_QUEUED);
    CHECK(tlb_deferred_enqueue(&q,&i,&h)==TLB_DEFERRED_BUSY);
    cpu_request_result_t result={.status=CPU_REQUEST_OK,.requested=3,.acknowledged=3},out;
    for(unsigned n=0;n<8;++n) {
        CHECK(!tlb_deferred_release(&q,handles[n]));CHECK(tlb_deferred_poll(&q,handles[n],&out)==TLB_DEFERRED_PENDING);
        tlb_interval_t copied;CHECK(tlb_deferred_claim(&q,n,&h,&copied));CHECK(!memcmp(&i,&copied,sizeof(i)));
        CHECK(!tlb_deferred_claim(&q,n,&h,&copied));CHECK(tlb_deferred_finish(&q,handles[n],&result));
        CHECK(!tlb_deferred_finish(&q,handles[n],&result));
        CHECK(tlb_deferred_poll(&q,handles[n],&out)==TLB_DEFERRED_COMPLETE && out.acknowledged==3);
        CHECK(tlb_deferred_release(&q,handles[n]));CHECK(!tlb_deferred_release(&q,handles[n]));
    }
    CHECK(tlb_deferred_enqueue(&q,&i,&h)==TLB_DEFERRED_QUEUED);
    CHECK(tlb_deferred_poll(&q,handles[0],&out)==TLB_DEFERRED_INVALID);
    tlb_completion_t active;tlb_interval_t copied;CHECK(tlb_deferred_claim(&q,h.slot,&active,&copied));
    result.status=CPU_REQUEST_TIMEOUT;result.missing=2;result.acknowledged=1;
    CHECK(tlb_deferred_finish(&q,h,&result));CHECK(tlb_deferred_poll(&q,h,&out)==TLB_DEFERRED_FAILED);
    CHECK(out.missing==2 && !tlb_deferred_release(&q,h));
    q.next_generation=UINT64_MAX>>3;CHECK(tlb_deferred_enqueue(&q,&i,&active)==TLB_DEFERRED_EXHAUSTED);
    for(unsigned bits=0;bits<64;++bits) {
        bool got=cpu_request_context_valid(bits&1,(bits&2)?1:0,bits&4,bits&8,bits&16,bits&32);
        CHECK(got==(bits==13));
    }
}
int main(void) {
    sender_tests();interval_tests();request_tests();late_tests();queue_tests();
    printf("TLB_HOST status=PASS checks=%u privileged_operations=SUBSTITUTED\n",checks);
    return 0;
}
