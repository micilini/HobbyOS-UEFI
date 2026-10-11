/* Actual source-extracted scheduler adapters with controlled allocation,
 * registry, IRQ and stack-service boundaries. No privileged instruction runs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stddef.h>
#include "../kernel/src/core/scheduler.h"
#include "../kernel/src/core/spinlock.h"
#include "../kernel/src/memory/kernel_stack.h"

#ifndef ASSERT_TASK_GENERATION_OFFSET
#define ASSERT_TASK_GENERATION_OFFSET 184u
#endif
_Static_assert(offsetof(task_t,id)==8u,"assertion observer task ID offset");
_Static_assert(offsetof(task_t,lifecycle_generation)==ASSERT_TASK_GENERATION_OFFSET,
               "assertion observer task lifecycle offset");

#define STACK_SIZE HOBBYOS_KERNEL_STACK_SIZE
#define TASK_REAPER_BATCH_MAX 16u
#define TEST_REAP_HOLD_MAX 16
#define TASK_REAP_EXPECTED_MASK ((uint64_t)TASK_REAP_DEFER_TEST_HOLD|TASK_REAP_DEFER_ON_CPU|TASK_REAP_DEFER_CURRENT|TASK_REAP_DEFER_TIMER_REFS)
#define TASK_REAP_POLICY_MASK ((uint64_t)TASK_REAP_DEFER_GRACE)
static spinlock_t g_scheduler_lock;
static struct list_head g_all_tasks;
static task_id_t g_next_task_id,g_test_reap_hold_ids[TEST_REAP_HOLD_MAX];
static uint64_t g_next_lifecycle_generation;
static uint8_t g_test_hold_next_ready,g_test_hold_next_reap,g_test_quiet_lifecycle;
static task_reaper_stats_t g_reaper_stats;
static unsigned checks,cases,allocated,freed,discarded,retired,published;
static bool host_if,expected_panic,current,mutate_after_claim;
static jmp_buf panic_return;
static const char *panic_reason;
enum failure { NONE, TASK_ALLOC, STACK_ALLOC, BIND, REGISTRY, QUEUE, PUBLISH, RETIRE };
static enum failure failure;
enum { HOST_TASKS=20 };
static task_t tasks[HOST_TASKS];
static unsigned char payloads[HOST_TASKS][STACK_SIZE] __attribute__((aligned(4096)));
static struct { bool acquired,bound,published,released,freed; uint64_t id,life; } owners[HOST_TASKS];
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"scheduler stack check %u line %u: %s\n",checks,__LINE__,#x);exit(1);}} while(0)

static unsigned task_index(const task_t *task)
{ CHECK(task>=tasks && task<tasks+HOST_TASKS);return (unsigned)(task-tasks); }
static unsigned handle_index(stack_handle_t h)
{ CHECK(h.slot>=128 && h.slot<128+HOST_TASKS && h.generation==7);return h.slot-128; }
static void *kmalloc(size_t bytes)
{ CHECK(!g_scheduler_lock.locked && bytes==sizeof(task_t));if(failure==TASK_ALLOC)return NULL;CHECK(allocated<HOST_TASKS);return &tasks[allocated++]; }
static void kfree(void *p)
{ unsigned i=task_index(p);CHECK(!g_scheduler_lock.locked && !owners[i].freed && (!owners[i].acquired || owners[i].released));owners[i].freed=true;++freed; }
static void thread_wrapper(void) { CHECK(false); }
static void entry(void *arg) { (void)arg;CHECK(false); }
static void serial_write_all(const char *s) { (void)s;CHECK(!g_scheduler_lock.locked); }
static void serial_write_hex64_all(uint64_t value) { (void)value;CHECK(!g_scheduler_lock.locked); }
static void sched_serial_dec(uint64_t value) { (void)value;CHECK(!g_scheduler_lock.locked); }
const char *scheduler_task_exit_reason_to_string(task_exit_reason_t reason) { (void)reason;return "hosted"; }
void kpanic(const char *s) { panic_reason=s;CHECK(expected_panic);longjmp(panic_return,1); }
irq_flags_t spin_lock_irqsave(spinlock_t *lock)
{ CHECK(lock==&g_scheduler_lock && !lock->locked);irq_flags_t old=host_if;host_if=false;lock->locked=1;return old; }
void spin_unlock_irqrestore(spinlock_t *lock,irq_flags_t old)
{
    CHECK(lock==&g_scheduler_lock && lock->locked);lock->locked=0;host_if=old;
    if(mutate_after_claim && tasks[0].reap_claimed){tasks[0].on_cpu=1;mutate_after_claim=false;}
}
bool kernel_stack_acquire(stack_handle_t *h,void **base)
{
    CHECK(!g_scheduler_lock.locked && allocated);if(failure==STACK_ALLOC)return false;
    unsigned i=allocated-1;CHECK(!owners[i].acquired);owners[i].acquired=true;
    *h=(stack_handle_t){.generation=7,.slot=128+i};*base=payloads[i];return true;
}
bool kernel_stack_bind_task(stack_handle_t h,uint64_t id,uint64_t life,const char *name)
{
    unsigned i=handle_index(h);CHECK(g_scheduler_lock.locked && !host_if && owners[i].acquired && !owners[i].published && id && life && name[0]);
    if(failure==BIND)return false;
    owners[i].bound=true;owners[i].id=id;owners[i].life=life;return true;
}
bool kernel_stack_publish(stack_handle_t h)
{
    unsigned i=handle_index(h);CHECK(g_scheduler_lock.locked && owners[i].bound && tasks[i].registry_registered && tasks[i].queue_membership!=TASK_QUEUE_NONE);
    if(failure==PUBLISH)return false;
    owners[i].published=true;++published;return true;
}
void kernel_stack_discard(stack_handle_t h)
{
    unsigned i=handle_index(h);CHECK(!g_scheduler_lock.locked && !owners[i].published && owners[i].acquired && !owners[i].released && !tasks[i].registry_registered);
    owners[i].released=true;++discarded;
}
void kernel_stack_retire(stack_handle_t h,uint64_t id,uint64_t life)
{
    unsigned i=handle_index(h);CHECK(host_if && !g_scheduler_lock.locked && owners[i].published && !owners[i].released &&
        id==owners[i].id && life==owners[i].life && tasks[i].reap_claimed && !tasks[i].registry_registered && !tasks[i].on_cpu);
    if(failure==RETIRE)kpanic("HOSTED retirement remains quarantined");
    owners[i].released=true;++retired;
}
static bool registry_add_locked(task_t *t)
{
    CHECK(g_scheduler_lock.locked && owners[task_index(t)].bound);
    if(failure==REGISTRY)return false;
    list_add_tail(&t->registry_list,&g_all_tasks);t->registry_registered=1;return true;
}
static bool registry_remove_locked(task_t *t)
{ CHECK(g_scheduler_lock.locked && t->registry_registered);list_del(&t->registry_list);list_init(&t->registry_list);t->registry_registered=0;return true; }
static bool enqueue_task_locked(task_t *t)
{ CHECK(g_scheduler_lock.locked && t->registry_registered);if(failure==QUEUE)return false;t->queue_membership=TASK_QUEUE_RUNQ_NORMAL;return true; }
static uint8_t task_is_current_locked(task_t *t,uint32_t *cpu)
{ (void)t;if(cpu)*cpu=0;return current; }
uint64_t timer_get_uptime_ms(void) { return 10000; }
static uint64_t clock_monotonic_ns(void) { return 10000000000ULL; }

/* Generated by the host runner from exactly these production functions. */
#include "stack-scheduler-extracted.h"

static task_create_options_t options(void)
{ return (task_create_options_t){.name="owned-worker",.task_class=TASK_CLASS_NORMAL,.flags=TASK_FLAG_SYSTEM}; }
static void reset(void)
{
    ++cases;memset(tasks,0,sizeof(tasks));memset(owners,0,sizeof(owners));memset(payloads,0xcc,sizeof(payloads));
    memset(&g_scheduler_lock,0,sizeof(g_scheduler_lock));memset(&g_reaper_stats,0,sizeof(g_reaper_stats));
    memset(g_test_reap_hold_ids,0,sizeof(g_test_reap_hold_ids));list_init(&g_all_tasks);
    allocated=freed=discarded=retired=published=0;g_next_task_id=41;g_next_lifecycle_generation=73;
    g_test_hold_next_ready=g_test_hold_next_reap=0;g_test_quiet_lifecycle=1;host_if=true;
    current=mutate_after_claim=expected_panic=false;failure=NONE;panic_reason=NULL;
}
static void create_ok(void)
{
    task_create_options_t opt=options();task_handle_t out;
    CHECK(thread_create_ex_handle(entry,(void *)(uintptr_t)0x12340,&opt,&out));
    task_t *t=&tasks[allocated-1];CHECK(out.id==t->id && out.lifecycle_generation==t->lifecycle_generation && owners[allocated-1].published);
    CHECK(t->kernel_stack_bytes==16384 && t->kernel_stack_guard_bytes==512 && t->kernel_stack_low_watermark==15872);
    CHECK(t->kernel_stack_canary==HOBBYOS_KERNEL_STACK_CANARY && scheduler_task_stack_guard_ok(t) && scheduler_task_stack_rsp_in_bounds(t));
    CHECK(t->rsp==(uintptr_t)t->stack_base+16384-7*8 && !(t->rsp&7));
    const uint64_t *sp=(void *)(uintptr_t)t->rsp;
    CHECK(sp[0]==0 && sp[1]==0 && sp[2]==0x12340 && sp[3]==(uintptr_t)entry && sp[4]==0 && sp[5]==0 && sp[6]==(uintptr_t)thread_wrapper);
    for(unsigned i=0;i<16384-56;++i)CHECK(((unsigned char *)t->stack_base)[i]==0xa5);
}
static void zombie(task_t *t)
{
    t->state=TASK_ZOMBIE;t->queue_membership=TASK_QUEUE_NONE;
    t->exit_started=t->cleanup_started=t->cleanup_done=t->lifecycle_notify_started=t->lifecycle_notify_completed=1;
    t->exit_reason=TASK_EXIT_NORMAL;t->zombie_generation=9;
    t->exit_started_ns=1000;t->cleanup_completed_ns=2000;t->zombie_entered_ns=3000;t->zombie_since_ms=4;
}
static void creation_failures(void)
{
    for(enum failure f=TASK_ALLOC;f<=QUEUE;++f)for(unsigned irq=0;irq<2;++irq){
        reset();failure=f;host_if=irq;task_create_options_t opt=options();opt.test_reap_hold=1;task_handle_t out={9,9};
        CHECK(!thread_create_ex_handle(entry,NULL,&opt,&out));CHECK(!out.id && !out.lifecycle_generation);
        CHECK(!published && !g_scheduler_lock.locked && host_if==irq && list_empty(&g_all_tasks));
        CHECK(freed==allocated && discarded==(f>STACK_ALLOC));
        for(unsigned i=0;i<TEST_REAP_HOLD_MAX;++i)CHECK(!g_test_reap_hold_ids[i]);
    }
    for(unsigned which=0;which<3;++which){
        reset();task_create_options_t opt=options();opt.test_reap_hold=1;task_handle_t out;
        if(which==0)g_next_task_id=0;else if(which==1)g_next_lifecycle_generation=0;
        else for(unsigned i=0;i<TEST_REAP_HOLD_MAX;++i)g_test_reap_hold_ids[i]=100+i;
        CHECK(!thread_create_ex_handle(entry,NULL,&opt,&out));CHECK(freed==1 && discarded==1 && !published && host_if);
    }
    reset();failure=PUBLISH;expected_panic=true;task_create_options_t opt=options();task_handle_t out;
    if(!setjmp(panic_return)){thread_create_ex_handle(entry,NULL,&opt,&out);CHECK(false);}
    CHECK(strstr(panic_reason,"publication") && !freed && !discarded && !published);
    reset();g_next_task_id=UINT64_MAX;g_next_lifecycle_generation=UINT64_MAX;create_ok();
    CHECK(!g_next_task_id && !g_next_lifecycle_generation);CHECK(!thread_create_ex_handle(entry,NULL,&opt,&out));
    CHECK(owners[0].published && !owners[0].released && freed==1 && discarded==1);
}
static void reaper_decisions(void)
{
    reset();for(unsigned i=0;i<17;++i){create_ok();zombie(&tasks[i]);}
    CHECK(scheduler_reap_zombies(3000)==16 && retired==16 && freed==16 && g_reaper_stats.reaped==16);
    CHECK(tasks[16].registry_registered && !owners[16].released && !g_reaper_stats.free_inflight);
    CHECK(scheduler_reap_zombies(3000)==1 && retired==17 && freed==17 && host_if && list_empty(&g_all_tasks));
    for(unsigned which=0;which<5;++which){
        reset();create_ok();zombie(&tasks[0]);task_t *t=&tasks[0];
        if(which==0){t->on_cpu=1;t->current_cpu_slot=2;}else if(which==1)current=true;
        else if(which==2){t->task_wake_timer_refs=t->timer_ref_acquires=1;}
        else if(which==3)g_test_reap_hold_ids[0]=t->id;else t->zombie_since_ms=9999;
        CHECK(!scheduler_reap_zombies(3000) && !retired && !freed && t->registry_registered && host_if);
    }
    for(unsigned which=0;which<5;++which){
        reset();create_ok();zombie(&tasks[0]);task_t *t=&tasks[0];expected_panic=true;
        if(which==0)payloads[0][511]=0;else if(which==1)t->rsp=(uintptr_t)t->stack_base+511;
        else if(which==2)t->cleanup_done=0;else if(which==3)mutate_after_claim=true;else failure=RETIRE;
        if(!setjmp(panic_return)){scheduler_reap_zombies(3000);CHECK(false);}
        CHECK(!retired && !freed && !owners[0].released && !g_scheduler_lock.locked && host_if);
        CHECK(strstr(panic_reason,which<3?"structural":which==3?"claimed invariant":"quarantined"));
        if(which==4)CHECK(g_reaper_stats.free_inflight==1 && !g_reaper_stats.reaped);
    }
}
int main(void)
{
    creation_failures();reaper_decisions();CHECK(cases==26 && checks>300000);
    puts("STACK_SCHEDULER_HOST status=PASS source_adapters=2 cases=26 capacity=16384 canary=512 privileged_boundaries=HOSTED");return 0;
}
