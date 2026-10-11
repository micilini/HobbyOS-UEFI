/* Real deferred admission/worker code with substituted IRQ, semaphore, task
 * and synchronous service primitives. No privileged function is executed. */
#include "../kernel/src/memory/kernel_tlb.h"
#include "../kernel/src/memory/paging_private.h"
#include "../kernel/src/core/idt.h"
#include "../kernel/src/core/interrupt_context.h"
#include "../kernel/src/core/semaphore.h"
#include "../kernel/src/core/scheduler.h"
#include "../kernel/src/timer/hpet.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

PageTable *g_kernel_pml4=(PageTable *)0x1000;
uint64_t paging_kernel_root_physical(void) { return 0x1000; }
volatile int g_panic_in_progress;
static _Thread_local uint64_t flags=514;
static _Thread_local unsigned depth;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static void (*entry)(void *);
static unsigned wake_count,held=1,stop,executed,fail_operation;

irq_flags_t irq_save(void) {uint64_t old=flags;flags&=~512u;return old;}
void irq_restore(irq_flags_t old) {flags=old;}
bool irq_is_enabled(void) {return (flags&512)!=0;}
bool smp_current_cpu_slot(cpu_slot_t *out) {*out=0;return true;}
bool interrupt_context_snapshot(cpu_slot_t slot,interrupt_cpu_snapshot_t *out) {
    assert(!slot && !irq_is_enabled());*out=(interrupt_cpu_snapshot_t){.depth=depth,.initialized=1};return true;
}
bool irq_bootstrap_services_are_ready(void) {return true;}
bool cpu_request_runtime_init(uint64_t root) {assert(root==0x1000 && !irq_is_enabled());return true;}
bool hpet_read_clock_sample(hpet_clock_sample_t *out) {(void)out;abort();}
void kpanic(const char *message) {(void)message;abort();}
void sem_init(semaphore_t *sem,int count) {(void)sem;assert(!count);}
void sem_wait(semaphore_t *sem) {
    (void)sem;assert(irq_is_enabled() && !depth);pthread_mutex_lock(&mutex);
    while((!wake_count || held) && !stop)pthread_cond_wait(&condition,&mutex);
    if(stop) {pthread_mutex_unlock(&mutex);pthread_exit(NULL);}
    --wake_count;pthread_mutex_unlock(&mutex);
}
bool sem_signal(semaphore_t *sem) {
    (void)sem;pthread_mutex_lock(&mutex);assert(wake_count==0);++wake_count;
    pthread_cond_signal(&condition);pthread_mutex_unlock(&mutex);return true;
}
static void *worker_entry(void *unused) {(void)unused;flags=514;entry(NULL);return NULL;}
bool thread_create_named_with_class_flags(void (*function)(void *),void *arg,
                                         int cls,const char *name,uint32_t options) {
    (void)cls;(void)name;assert(!arg && options==(TASK_FLAG_SYSTEM|TASK_FLAG_KILL_PROTECTED));
    entry=function;assert(!pthread_create(&worker,NULL,worker_entry,NULL));return true;
}
cpu_request_result_t cpu_request_mapping_run(cpu_request_operation_t operation,const tlb_interval_t *interval) {
    (void)operation;(void)interval;abort();
}
cpu_request_result_t cpu_request_run(cpu_request_operation_t operation,const tlb_interval_t *interval) {
    assert(irq_is_enabled() && !depth && operation==CPU_REQUEST_INVALIDATE && tlb_interval_valid(interval));
    unsigned generation=__atomic_add_fetch(&executed,1,__ATOMIC_RELAXED);
    return (cpu_request_result_t){.status=fail_operation?CPU_REQUEST_TIMEOUT:CPU_REQUEST_OK,
        .generation=generation,.requested=1,.acknowledged=fail_operation?0:1,.missing=fail_operation?1:0};
}
static void finish(tlb_completion_t handle) {
    cpu_request_result_t result;
    tlb_deferred_status_t status;
    while((status=kernel_tlb_poll(handle,&result))==TLB_DEFERRED_PENDING)sched_yield();
    assert(status==TLB_DEFERRED_COMPLETE && result.status==CPU_REQUEST_OK);
    assert(kernel_tlb_release(handle));assert(!kernel_tlb_release(handle));
}
static void *submitter(void *unused) {
    (void)unused;flags=2;depth=1;
    for(unsigned n=0;n<256;++n) {
        tlb_completion_t handle;tlb_deferred_status_t status;
        while((status=kernel_tlb_defer(0x1ff000,65*4096,&handle))==TLB_DEFERRED_BUSY)sched_yield();
        assert(status==TLB_DEFERRED_QUEUED && !irq_is_enabled());
        depth=0;flags=514;finish(handle);depth=1;flags=2;
    }
    return NULL;
}
int main(void) {
    flags=2;kernel_tlb_close_bootstrap();assert(kernel_tlb_runtime_init());flags=514;
    tlb_completion_t handles[8],sentinel={123,7};cpu_request_result_t result;
    depth=2;assert(kernel_tlb_defer(0,4096,&sentinel)==TLB_DEFERRED_INVALID);depth=0;
    g_panic_in_progress=1;assert(kernel_tlb_defer(0,4096,&sentinel)==TLB_DEFERRED_INVALID);g_panic_in_progress=0;
    for(unsigned n=0;n<8;++n) {
        assert(kernel_tlb_defer(0x1ff000,4096,&handles[n])==TLB_DEFERRED_QUEUED);
        assert(kernel_tlb_poll(handles[n],&result)==TLB_DEFERRED_PENDING);
    }
    assert(kernel_tlb_defer(0x1ff000,4096,&sentinel)==TLB_DEFERRED_BUSY);
    assert(sentinel.generation==123 && sentinel.slot==7 && executed==0 && wake_count==1);
    pthread_mutex_lock(&mutex);held=0;pthread_cond_signal(&condition);pthread_mutex_unlock(&mutex);
    for(unsigned n=0;n<8;++n)finish(handles[n]);
    pthread_t callers[4];
    for(unsigned n=0;n<4;++n)assert(!pthread_create(&callers[n],NULL,submitter,NULL));
    for(unsigned n=0;n<4;++n)assert(!pthread_join(callers[n],NULL));
    assert(executed==1032);fail_operation=1;
    assert(kernel_tlb_defer(0x1000,4096,&sentinel)==TLB_DEFERRED_QUEUED);
    tlb_deferred_status_t status;
    while((status=kernel_tlb_poll(sentinel,&result))==TLB_DEFERRED_PENDING)sched_yield();
    assert(status==TLB_DEFERRED_FAILED && result.status==CPU_REQUEST_TIMEOUT);
    assert(!kernel_tlb_release(sentinel));
    pthread_mutex_lock(&mutex);stop=1;pthread_cond_signal(&condition);pthread_mutex_unlock(&mutex);
    assert(!pthread_join(worker,NULL));
    puts("TLB_DEFERRED_HOST status=PASS submissions=1033 privileged_operations=SUBSTITUTED schedules=CONCURRENT");
    return 0;
}
