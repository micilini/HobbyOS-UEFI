/* Actual runtime request wrapper with substituted platform operations. This is
 * a concurrent host schedule, not evidence of privileged guest execution. */
#include "../kernel/src/smp/cpu_request.h"
#include "../kernel/src/smp/smp_topology.h"
#include "../kernel/src/smp/smp_boot.h"
#include "../kernel/src/apic/lapic.h"
#include "../kernel/src/core/interrupt_context.h"
#include "../kernel/src/core/irq_bootstrap.h"
#include "../kernel/src/core/scheduler.h"
#include "../kernel/src/core/clock.h"
#include "../kernel/src/drivers/serial.h"
#include "../kernel/src/libc/string.h"
#include "../kernel/src/memory/kernel_tlb.h"
#include "../kernel/src/timer/hpet.h"
#include "../kernel/src/cpu/cpu.h"
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

SmpCpuInfo g_cpus[HOBBYOS_MAX_CPUS];uint32_t g_cpu_count,g_bsp_apic_id;
volatile int g_panic_in_progress;
static _Thread_local unsigned current_slot,depth;
static _Thread_local uint64_t flags=514;
static unsigned stopped,skip_slot=UINT32_MAX,bad_slot=UINT32_MAX,sends,executions[32],migrate;
static unsigned fail_send,late,late_entered,late_release,late_clock;
static pthread_t threads[32];
static struct {
    pthread_mutex_t lock;pthread_cond_t signal;unsigned queued;
} queues[32];
static uint64_t virtual_clock,clock_step;
static char failure_log[4096];static size_t log_length;
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"RUNTIME_HOST assertion=%s line=%d\n",#x,__LINE__);abort();} } while(0)

irq_flags_t irq_save(void) {uint64_t prior=flags;flags&=~UINT64_C(512);return prior;}
void irq_restore(irq_flags_t saved) {flags=saved;}
bool irq_is_enabled(void) {return (flags&512)!=0;}
bool smp_current_cpu_slot(cpu_slot_t *out) {*out=current_slot;return current_slot<g_cpu_count;}
bool smp_cpu_slot_from_apic_id(uint32_t apic,cpu_slot_t *out) {
    *out=apic-0x100;return *out<g_cpu_count;
}
bool cpu_get_cpuid_count(uint32_t leaf,uint32_t subleaf,uint32_t *a,uint32_t *b,uint32_t *c,uint32_t *d) {
    (void)leaf;(void)subleaf;*a=*b=*c=*d=0;return false;
}
uint32_t lapic_get_id(void) {return 0x100+current_slot;}
uint32_t smp_online_cpu_count(void) {return g_cpu_count;}
uint32_t smp_failed_cpu_count(void) {return 0;}
irq_bootstrap_state_t irq_bootstrap_state(void) {return IRQ_BOOTSTRAP_CPUS_PREPARED;}
bool irq_bootstrap_services_are_ready(void) {return true;}
bool irq_bootstrap_cpu_irq_ready(uint32_t slot) {return slot<g_cpu_count;}
bool scheduler_current_context_can_block(void) {return true;}
bool clock_monotonic_is_ready(void) {return true;}
bool hpet_read_clock_sample(hpet_clock_sample_t *out) {
    struct timespec now;CHECK(!clock_gettime(CLOCK_MONOTONIC,&now));
    *out=(hpet_clock_sample_t){.valid=1,.ns=(uint64_t)now.tv_sec*1000000000+(uint64_t)now.tv_nsec};return true;
}
uint64_t clock_monotonic_ns(void) {
    CHECK(irq_is_enabled());
    if(migrate && irq_is_enabled())current_slot=(current_slot+1)%g_cpu_count;
    if(clock_step)return __atomic_add_fetch(&virtual_clock,clock_step,__ATOMIC_RELAXED);
    struct timespec now;CHECK(!clock_gettime(CLOCK_MONOTONIC,&now));
    return (uint64_t)now.tv_sec*1000000000+(uint64_t)now.tv_nsec;
}
bool clock_monotonic_read_ns(uint64_t *out) {
    if(!out)return false;
    *out=clock_monotonic_ns();return *out!=0 && *out!=UINT64_MAX;
}
bool interrupt_context_snapshot(cpu_slot_t slot,interrupt_cpu_snapshot_t *out) {
    CHECK(!irq_is_enabled());CHECK(slot==current_slot);
    *out=(interrupt_cpu_snapshot_t){.depth=depth,.initialized=1};return true;
}
int ksnprintf(char *dst,size_t size,const char *format,...) {
    va_list args;va_start(args,format);int result=vsnprintf(dst,size,format,args);va_end(args);return result;
}
void serial_write_all(const char *text) {
    size_t length=strlen(text);CHECK(log_length+length<sizeof(failure_log));
    memcpy(failure_log+log_length,text,length+1);log_length+=length;
}
static uint64_t cr3(void *unused) {(void)unused;return 0x1000;}
static uint64_t cr4(void *unused) {(void)unused;return current_slot==bad_slot?0xa0:0x20;}
static void invlpg(void *unused,uint64_t address) {
    (void)unused;CHECK(!irq_is_enabled());CHECK(address>=0x1ff000 && address<0x240000);
    __atomic_add_fetch(&executions[current_slot],1,__ATOMIC_RELAXED);
}
static void reload(void *unused,uint64_t root) {
    (void)unused;CHECK(!irq_is_enabled());CHECK(root==0x1000);
    __atomic_add_fetch(&executions[current_slot],1,__ATOMIC_RELAXED);
}
bool kernel_tlb_execute_local(const cpu_request_spec_t *spec,cpu_request_sample_t *sample) {
    CHECK(!irq_is_enabled());sample->cr3=0x1000;
    if(late_clock)virtual_clock+=CPU_REQUEST_DEADLINE_NS;
    if(late && current_slot==3) {
        __atomic_store_n(&late_entered,1,__ATOMIC_RELEASE);
        while(!__atomic_load_n(&late_release,__ATOMIC_ACQUIRE))__asm__ volatile("pause");
    }
    const tlb_local_ops_t ops={cr3,cr4,invlpg,reload};
    return tlb_local_execute(&spec->interval,spec->root,&ops,NULL);
}
lapic_delivery_result_t lapic_send_ipi(uint32_t id,uint32_t vector) {
    uint64_t encoded;CHECK(lapic_delivery_encode(true,id,vector,LAPIC_MESSAGE_FIXED,&encoded)==LAPIC_DELIVERY_SUBMITTED);
    CHECK(!irq_is_enabled());CHECK(id>=0x100 && id<0x100+g_cpu_count);
    __atomic_add_fetch(&sends,1,__ATOMIC_RELAXED);
    if(fail_send)return LAPIC_DELIVERY_INDETERMINATE;
    unsigned slot=id-0x100;
    if(slot!=skip_slot) {
        CHECK(!pthread_mutex_lock(&queues[slot].lock));queues[slot].queued++;
        CHECK(!pthread_cond_signal(&queues[slot].signal));CHECK(!pthread_mutex_unlock(&queues[slot].lock));
    }
    return LAPIC_DELIVERY_SUBMITTED;
}
static void *target(void *arg) {
    current_slot=(unsigned)(uintptr_t)arg;flags=2;depth=1;
    while(!__atomic_load_n(&stopped,__ATOMIC_ACQUIRE)) {
        CHECK(!pthread_mutex_lock(&queues[current_slot].lock));
        while(!queues[current_slot].queued && !__atomic_load_n(&stopped,__ATOMIC_ACQUIRE))
            CHECK(!pthread_cond_wait(&queues[current_slot].signal,&queues[current_slot].lock));
        queues[current_slot].queued=0;CHECK(!pthread_mutex_unlock(&queues[current_slot].lock));
        if(!__atomic_load_n(&stopped,__ATOMIC_ACQUIRE)) {
            cpu_request_irq();cpu_request_irq();
        }
    }
    return NULL;
}
static void initialize(unsigned cpus) {
    g_cpu_count=cpus;current_slot=0;flags=2;
    for(unsigned slot=0;slot<cpus;++slot) {
        g_cpus[slot]=(SmpCpuInfo){.slot=slot,.apic_id=0x100+slot,.state=CPU_STATE_ONLINE,.is_bsp=slot==0};
        CHECK(!pthread_mutex_init(&queues[slot].lock,NULL));CHECK(!pthread_cond_init(&queues[slot].signal,NULL));
    }
    CHECK(cpu_request_runtime_init(0x1000));CHECK(!cpu_request_runtime_init(0x1000));flags=514;
    for(unsigned slot=0;slot<cpus;++slot)CHECK(!pthread_create(&threads[slot],NULL,target,(void*)(uintptr_t)slot));
}
static void stop(void) {
    __atomic_store_n(&stopped,1,__ATOMIC_RELEASE);
    for(unsigned slot=0;slot<g_cpu_count;++slot) {
        CHECK(!pthread_mutex_lock(&queues[slot].lock));CHECK(!pthread_cond_signal(&queues[slot].signal));CHECK(!pthread_mutex_unlock(&queues[slot].lock));
    }
    for(unsigned slot=0;slot<g_cpu_count;++slot)CHECK(!pthread_join(threads[slot],NULL));
}
static cpu_request_result_t request(unsigned pages) {
    tlb_interval_t interval;CHECK(tlb_interval_normalize(0x1ff000,pages*4096u,&interval));
    return cpu_request_run(CPU_REQUEST_INVALIDATE,&interval);
}
static void *initiator(void *arg) {
    current_slot=(unsigned)(uintptr_t)arg;flags=514;
    for(unsigned n=0;n<64;++n) {
        cpu_request_result_t r=request(n%2?65:64);
        CHECK(r.status==CPU_REQUEST_OK && r.requested==UINT32_MAX && r.acknowledged==r.requested);
        CHECK(r.notifications==(unsigned)__builtin_popcount(r.sent));CHECK(irq_is_enabled());
    }
    return NULL;
}
static void run_case(unsigned which) {
    initialize(which==0?32:which==8?1:4);
    if(which==0) {
        pthread_t callers[2];CHECK(!pthread_create(&callers[0],NULL,initiator,(void*)0));CHECK(!pthread_create(&callers[1],NULL,initiator,(void*)1));
        CHECK(!pthread_join(callers[0],NULL));CHECK(!pthread_join(callers[1],NULL));
        for(unsigned slot=0;slot<32;++slot)CHECK(executions[slot]==64*65);
    } else if(which==1) {
        flags=2;CHECK(request(1).status==CPU_REQUEST_CONTEXT);flags=514;depth=1;
        CHECK(request(1).status==CPU_REQUEST_CONTEXT);depth=0;g_panic_in_progress=1;
        CHECK(request(1).status==CPU_REQUEST_PANIC);g_panic_in_progress=0;CHECK(!sends);
        CHECK(request(0).status==CPU_REQUEST_OK && !sends);
    } else if(which==2) {
        migrate=1;
        for(unsigned n=0;n<64;++n)CHECK(request(1).status==CPU_REQUEST_OK);
        migrate=0;for(unsigned slot=0;slot<4;++slot)CHECK(executions[slot]==64);
    } else if(which==3) {
        skip_slot=3;virtual_clock=1;clock_step=10000;
        cpu_request_result_t r=request(1);CHECK(r.status==CPU_REQUEST_TIMEOUT && r.missing&8);
        CHECK(strstr(failure_log,"slot=3 apic=259") && strstr(failure_log,"generation=1"));
        CHECK(request(1).status==CPU_REQUEST_QUARANTINED);
    } else if(which==4) {
        bad_slot=3;cpu_request_result_t r=request(1);
        CHECK(r.status==CPU_REQUEST_LOCAL_FAILED && r.failed==8 && r.acknowledged==7 && !executions[3]);
        CHECK(request(1).status==CPU_REQUEST_QUARANTINED);
    } else if(which==5) {
        fail_send=1;virtual_clock=1;clock_step=10000;cpu_request_result_t r=request(1);
        CHECK(r.status==CPU_REQUEST_TIMEOUT && r.send_failed==14 && r.missing==14 && r.acknowledged==1);
    } else if(which==6) {
        late=1;cpu_request_result_t r=request(1);
        CHECK(r.status==CPU_REQUEST_TIMEOUT && r.missing==8 && late_entered);
        __atomic_store_n(&late_release,1,__ATOMIC_RELEASE);
        CHECK(request(1).status==CPU_REQUEST_QUARANTINED);
    } else if(which==7) {
        g_cpus[3].state=CPU_STATE_FAILED;
        CHECK(request(1).status==CPU_REQUEST_UNAVAILABLE && !sends);
    } else if(which==8) {
        virtual_clock=1;clock_step=1;late_clock=1;
        cpu_request_result_t r=request(1);
        CHECK(r.status==CPU_REQUEST_TIMEOUT && r.acknowledged==1 && !r.missing);
        CHECK(r.elapsed_ns>=CPU_REQUEST_DEADLINE_NS && !sends);
        CHECK(request(1).status==CPU_REQUEST_QUARANTINED);
    }
    uint64_t admitted,completed;CHECK(cpu_request_audit_snapshot(&admitted,&completed));
    CHECK(admitted==completed);
    uint64_t expected = which==0?128:which==2?64:(which==1||which==7)?0:1;
    CHECK(completed==expected);
    cpu_request_audit_t audit;
    CHECK(!cpu_request_audit_copy(0,&audit));CHECK(!cpu_request_audit_copy(completed+1,&audit));
    for(uint64_t generation=1;generation<=completed;++generation) {
        CHECK(cpu_request_audit_copy(generation,&audit));
        CHECK(audit.spec.generation==generation && audit.result.generation==generation && audit.spec.root==0x1000);
        CHECK(!audit.mapping && audit.spec.operation==CPU_REQUEST_INVALIDATE);
        CHECK(audit.result.requested==audit.spec.targets);
        for(unsigned slot=0;slot<g_cpu_count;++slot)CHECK(audit.spec.apic[slot]==0x100+slot);
        CHECK(audit.result.status==((which==3||which==5||which==6||which==8)?CPU_REQUEST_TIMEOUT:
            which==4?CPU_REQUEST_LOCAL_FAILED:CPU_REQUEST_OK));
    }
    stop();
}
int main(void) {
    for(unsigned which=0;which<9;++which) {
        pid_t child=fork();CHECK(child>=0);
        if(!child) {run_case(which);_exit(0);}
        int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
    puts("TLB_RUNTIME_HOST status=PASS cases=9 privileged_operations=SUBSTITUTED schedules=CONCURRENT");
    return 0;
}
