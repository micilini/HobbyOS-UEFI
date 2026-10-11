#include "guard_probe.h"
#include "stack_internal.h"
#include "image_walk.h"
#include "kernel_tlb.h"
#include "paging_private.h"
#include "../apic/lapic.h"
#include "../cpu/tss.h"
#include "../core/scheduler.h"
#include "../smp/smp_topology.h"
#include "../smp/smp_boot.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../libc/string.h"
#include "../libc/memory.h"
#include "../core/panic.h"
#include "../shell/commands/cmd_tasktest.h"
#include <stdarg.h>
#if defined(HOBBYOS_IRQ_LATENCY) && HOBBYOS_IRQ_LATENCY
#include "stack_report.h"
#include "irq_latency_test.h"
#include "../core/irq_latency.h"
#endif

extern void guard_overflow_enter(void) __attribute__((noreturn));
extern char guard_overflow_step[];
static uint32_t g_stack_test_owner;
static uint64_t g_stack_test_collection;
static uint32_t g_overflow_release;
static stack_owner_view_t g_timeout_owner;
static uint32_t g_timeout_exited,g_timeout_armed,g_timeout_target;

bool stack_test_emit(const char *format,...)
{
    char line[768];line[0]='\n';va_list args;va_start(args,format);
    int n=kvsnprintf(line+1,sizeof(line)-1,format,args);va_end(args);
    if (n<1 || (size_t)n>=sizeof(line)-33 || line[n]!='\n') return false;
    uint32_t crc=tasktest_transport_crc32(line+1,(uint32_t)n-1);
    int tail=ksnprintf(line+n,sizeof(line)-(size_t)n," crc=%08x\n",crc);
    if (tail<1 || (size_t)tail>=sizeof(line)-(size_t)n) return false;
#if defined(HOBBYOS_IRQ_LATENCY) && HOBBYOS_IRQ_LATENCY
    if ((irq_latency_memory_scope_active() || irq_latency_test_reporting()) &&
        (!strncmp(line+1,"[STACK_GUARD][ALLOC] ",21) || !strncmp(line+1,"[STACK_GUARD][RETIRE] ",22)))
        return stack_report_enqueue(line,(size_t)n+(size_t)tail);
#endif
    serial_write_all(line);return true;
}

#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
bool stack_test_emit_checked(const char *format,...)
{
    char line[768];line[0]='\n';va_list args;va_start(args,format);
    int n=kvsnprintf(line+1,sizeof(line)-1,format,args);va_end(args);
    if (n<1 || (size_t)n>=sizeof(line)-33 || line[n]!='\n') return false;
    uint32_t crc=tasktest_transport_crc32(line+1,(uint32_t)n-1);
    int tail=ksnprintf(line+n,sizeof(line)-(size_t)n," crc=%08x\n",crc);
    if (tail<1 || (size_t)tail>=sizeof(line)-(size_t)n) return false;
    return serial_proof_record(line,(size_t)n+(size_t)tail);
}
#endif

static bool owner_line(const stack_owner_view_t *v,uint64_t collection)
{
    return stack_test_emit(
        "[STACK_GUARD][OWNER] collection=%llu slot=%u generation=%llu kind=%u state=%u task=%llu lifecycle=%llu cpu=%u apic=%u guard=%016llx base=%016llx end=%016llx physical=%016llx capacity=%u canary=%u owned=%u published=%u retirement=%llu\n",
        (unsigned long long)collection,v->slot,(unsigned long long)v->generation,v->kind,v->state,
        (unsigned long long)v->task_id,(unsigned long long)v->task_lifecycle,v->cpu,v->apic,
        (unsigned long long)v->guard,(unsigned long long)v->base,(unsigned long long)v->end,
        (unsigned long long)v->physical,v->capacity,v->kind==STACK_KIND_TASK ? HOBBYOS_KERNEL_STACK_GUARD_BYTES : 0,
        v->owned,v->ever_published,(unsigned long long)v->retirement_generation);
}

static bool current_stack(stack_owner_view_t *out)
{
    scheduler_cpu_pin_t pin={0};if (!scheduler_cpu_pin(&pin)) return false;
    const task_t *task=get_current_task();stack_owner_view_t owner;
    bool ok=task && kernel_stack_read(task->stack_owner.slot,&owner) &&
        owner.generation==task->stack_owner.generation && owner.state==STACK_LIVE &&
        owner.task_id==task->id && owner.task_lifecycle==task->lifecycle_generation &&
        task->kernel_stack_bytes==STACK_TASK_BYTES &&
        task->kernel_stack_guard_bytes==HOBBYOS_KERNEL_STACK_GUARD_BYTES &&
        scheduler_task_stack_guard_ok(task) && scheduler_task_stack_rsp_in_bounds(task);
    scheduler_cpu_unpin(&pin);if(ok)*out=owner;return ok;
}

static bool inventory(uint64_t collection,bool probe)
{
    unsigned persistent=0;
    for (unsigned cpu=0;cpu<g_cpu_count;++cpu) {
        const SmpCpuInfo *info=smp_cpu_by_slot_const(cpu);
        if (!info || info->state!=CPU_STATE_ONLINE || !info->tss_ptr) return false;
        const Tss64 *tss=info->tss_ptr;
        for (unsigned n=0;n<4;++n) {
            stack_kind_t kind=n==0 ? (info->is_bsp ? STACK_KIND_BSP_BOOT : STACK_KIND_AP_BOOT) :
                (stack_kind_t)(STACK_KIND_IST1+n-1);
            stack_owner_view_t v;
            if (!kernel_stack_cpu_owner(cpu,kind,&v) || v.apic!=info->apic_id ||
                !paging_stack_validate((stack_handle_t){v.generation,v.slot}) ||
                !owner_line(&v,collection)) return false;
            uint64_t top=n==0 ? info->stack_top : n==1 ? tss->ist1 : n==2 ? tss->ist2 : tss->ist3;
            if (top!=v.end || (probe && !guard_probe_owner(&v))) return false;
            ++persistent;
        }
    }
    unsigned tasks=0,prepared=0;
    for (unsigned i=STACK_CPU_SLOTS;i<STACK_REGION_SLOTS;++i) {
        stack_owner_view_t v;
        if (!kernel_stack_read(i,&v)) return false;
        if (v.state==STACK_FREE) continue;
        if (v.state==STACK_PREPARED) ++prepared;else if(v.state==STACK_LIVE) ++tasks;else return false;
        if (!paging_stack_validate((stack_handle_t){v.generation,v.slot}) || !owner_line(&v,collection)) return false;
    }
    stack_owner_view_t current;
    if (probe && (!current_stack(&current) || !guard_probe_owner(&current))) return false;
    image_walk_result_t scan;
    if (!paging_image_snapshot(&scan,NULL,NULL)) return false;
    return stack_test_emit(
        "[STACK_GUARD][SCAN] collection=%llu cpus=%u persistent=%u tasks=%u prepared=%u root=%016llx tables=%llu stack_bytes=%llu image_bytes=%llu rwx_bytes=%llu nx=%u guard_bytes=4096 canary_bytes=512 complete=1\n",
        (unsigned long long)collection,g_cpu_count,persistent,tasks,prepared,(unsigned long long)scan.root,
        (unsigned long long)scan.tables,(unsigned long long)scan.stack_bytes,(unsigned long long)scan.image_bytes,
        (unsigned long long)scan.rwx_bytes,scan.nx);
}

enum { CHURN_TASKS=256, CHURN_CREATORS=2, CHURN_PER_CREATOR=128, CHURN_HELD_PER_CREATOR=7 };
typedef struct {
    stack_owner_view_t owner;
    task_handle_t handle;
    uint64_t cpus;
    uint32_t published,entered,done,released,reaped,index,creator;
} stack_case_t;
static stack_case_t g_cases[CHURN_TASKS],g_creators[CHURN_CREATORS];
static uint32_t g_outstanding[CHURN_CREATORS],g_churn_release,g_churn_failed,g_churn_quarantined;
static uint32_t g_creators_active,g_creators_peak,g_reaped_during_creation;
static bool g_churn_warmup;
static uint64_t g_creator_retired_first[CHURN_CREATORS],g_creator_retired_last[CHURN_CREATORS];
static void churn_failure(unsigned reason,unsigned index)
{
    uint32_t free=0,code=(reason<<16)|(index&0xffff);
    (void)__atomic_compare_exchange_n(&g_churn_failed,&free,code,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
}

static bool sample_cpu(stack_case_t *test)
{
    scheduler_cpu_pin_t pin={0};if (!scheduler_cpu_pin(&pin)) return false;
    bool ok=pin.slot<HOBBYOS_MAX_CPUS && scheduler_cpu_pin_validate(&pin);
    if (ok) test->cpus|=UINT64_C(1)<<pin.slot;
    scheduler_cpu_unpin(&pin);return ok;
}
static void churn_child(void *argument)
{
    stack_case_t *test=argument;
    if (!current_stack(&test->owner) || !sample_cpu(test)) {
        churn_failure(1,test->index);return;
    }
    __atomic_store_n(&test->entered,1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&g_churn_release,__ATOMIC_ACQUIRE)) {
        if (!sample_cpu(test)) churn_failure(2,test->index);
        timer_sleep(2);
    }
    for (unsigned i=0;i<16;++i) {
        if (!sample_cpu(test)) churn_failure(3,test->index);
        timer_sleep(2);
    }
    stack_owner_view_t final;
    if (!current_stack(&final) || final.generation!=test->owner.generation || final.slot!=test->owner.slot)
        churn_failure(4,test->index);
    __atomic_store_n(&test->done,1,__ATOMIC_RELEASE);
}
static bool start_child(stack_case_t *test,unsigned index,unsigned creator)
{
    test->index=index;test->creator=creator;
    task_create_options_t options={.name="guard-cycle",.task_class=TASK_CLASS_NORMAL,
        .flags=TASK_FLAG_SYSTEM,.test_reap_hold=1};
    __atomic_add_fetch(&g_outstanding[creator],1,__ATOMIC_RELAXED);
    if (!thread_create_ex_handle(churn_child,test,&options,&test->handle)) {
        __atomic_sub_fetch(&g_outstanding[creator],1,__ATOMIC_RELAXED);
        churn_failure(5,index);return false;
    }
    __atomic_store_n(&test->published,1,__ATOMIC_RELEASE);return true;
}
static uint64_t retired_sample(void)
{
    kernel_stack_stats_t stats;
    for (unsigned i=0;i<100;++i) {
        if (kernel_stack_stats(&stats)) return stats.retired;
        timer_sleep(1);
    }
    churn_failure(6,0);return UINT64_MAX;
}
static void churn_creator(void *argument)
{
    stack_case_t *self=argument;unsigned creator=self->creator;
    if (!current_stack(&self->owner)) {churn_failure(7,creator);return;}
    __atomic_store_n(&self->entered,1,__ATOMIC_RELEASE);
    unsigned active=__atomic_add_fetch(&g_creators_active,1,__ATOMIC_ACQ_REL);
    if (active==CHURN_CREATORS) __atomic_store_n(&g_creators_peak,active,__ATOMIC_RELEASE);
    g_creator_retired_first[creator]=retired_sample();
    for (unsigned j=0;j<CHURN_PER_CREATOR && !__atomic_load_n(&g_churn_failed,__ATOMIC_ACQUIRE);++j) {
        while (__atomic_load_n(&g_outstanding[creator],__ATOMIC_ACQUIRE)>=CHURN_HELD_PER_CREATOR ||
            (j>=CHURN_HELD_PER_CREATOR && !__atomic_load_n(&g_churn_release,__ATOMIC_ACQUIRE))) {
            if (__atomic_load_n(&g_churn_failed,__ATOMIC_ACQUIRE)) break;
            timer_sleep(1);
        }
        if (__atomic_load_n(&g_churn_failed,__ATOMIC_ACQUIRE) || !sample_cpu(self) ||
            !start_child(&g_cases[creator*CHURN_PER_CREATOR+j],creator*CHURN_PER_CREATOR+j,creator)) {
            churn_failure(8,creator*CHURN_PER_CREATOR+j);break;
        }
        /* Retain the existing three-second reap grace and two-second reaper
         * cadence. Creation spans them so retirement really overlaps work. */
        timer_sleep(40);
    }
    g_creator_retired_last[creator]=retired_sample();
    __atomic_sub_fetch(&g_creators_active,1,__ATOMIC_ACQ_REL);
    __atomic_store_n(&self->done,1,__ATOMIC_RELEASE);
}

static bool observe_case(stack_case_t *test,uint64_t collection,bool creator)
{
    if (!__atomic_load_n(&test->published,__ATOMIC_ACQUIRE) || test->reaped) return true;
    task_snapshot_t task;
    bool present=scheduler_snapshot_task_by_handle(test->handle,&task);
    if (!test->released) {
        if (!__atomic_load_n(&test->done,__ATOMIC_ACQUIRE) || !present || task.state!=TASK_ZOMBIE) return present;
        /* ZOMBIE is published before the final CPU handoff. Keep the existing
         * reap hold until the copied lifecycle facts prove off-CPU readiness. */
        if (task.on_cpu || task.current_cpu_slot!=TASK_CPU_SLOT_NONE || !task.cleanup_done ||
            !task.lifecycle_notify_completed || task.task_wake_timer_refs) return true;
        stack_owner_view_t v;
        if (!kernel_stack_read(test->owner.slot,&v) || v.state!=STACK_LIVE ||
            v.generation!=test->owner.generation || v.task_id!=test->handle.id ||
            v.task_lifecycle!=test->handle.lifecycle_generation ||
            !paging_stack_validate((stack_handle_t){v.generation,v.slot}) ||
            !scheduler_test_hold_reap(test->handle.id,false)) {churn_failure(9,test->index);return false;}
        test->released=1;
        if (!creator) __atomic_sub_fetch(&g_outstanding[test->creator],1,__ATOMIC_RELEASE);
    } else if (!present) {
        stack_owner_view_t v;
        if (!kernel_stack_read(test->owner.slot,&v)) return true;
        if (v.generation==test->owner.generation && v.state!=STACK_FREE) return true;
        if (v.generation<test->owner.generation ||
            (v.generation==test->owner.generation && !v.retirement_generation)) {churn_failure(10,test->index);return false;}
        test->reaped=1;
        if (__atomic_load_n(&g_creators_active,__ATOMIC_ACQUIRE)) ++g_reaped_during_creation;
        return stack_test_emit("[STACK_GUARD][CYCLE] collection=%llu phase=%s index=%u creator=%u parent=%u task=%llu lifecycle=%llu slot=%u generation=%llu physical=%016llx cpus=%llx canary=512 state=REAPED observed_generation=%llu retirement=%llu\n",
            (unsigned long long)collection,g_churn_warmup ? "warmup" : "stress",test->index,test->creator,creator,
            (unsigned long long)test->handle.id,(unsigned long long)test->handle.lifecycle_generation,
            test->owner.slot,(unsigned long long)test->owner.generation,(unsigned long long)test->owner.physical,
            (unsigned long long)test->cpus,(unsigned long long)v.generation,
            (unsigned long long)(v.generation==test->owner.generation ? v.retirement_generation : 0));
    }
    return true;
}
static bool drain_cases(unsigned count,unsigned parents,uint64_t collection,uint64_t deadline)
{
    while (timer_get_uptime_ms()<deadline) {
        unsigned done=0;
        for (unsigned i=0;i<count;++i) {
            if (!observe_case(&g_cases[i],collection,false)) return false;
            done+=g_cases[i].reaped;
        }
        for (unsigned i=0;i<parents;++i) {
            if (!observe_case(&g_creators[i],collection,true)) return false;
            done+=g_creators[i].reaped;
        }
        if (__atomic_load_n(&g_churn_failed,__ATOMIC_ACQUIRE)) return false;
        if (done==count+parents) return true;
        timer_sleep(2);
    }
    return false;
}
static bool churn_workload(uint64_t collection)
{
    if (g_churn_quarantined || scheduler_test_reap_holds()) return false;
    memset(g_cases,0,sizeof(g_cases));memset(g_creators,0,sizeof(g_creators));
    memset(g_outstanding,0,sizeof(g_outstanding));
    g_churn_failed=g_creators_active=g_creators_peak=g_reaped_during_creation=0;
    g_churn_release=1;g_churn_warmup=true;
    uint64_t deadline=timer_get_uptime_ms()+120000;
    for (unsigned i=0;i<STACK_PREPARED_TASKS;++i)
        if (!start_child(&g_cases[i],i,0)) goto fail;
    if (!drain_cases(STACK_PREPARED_TASKS,0,collection,deadline)) goto fail;
    kernel_stack_stats_t before,after;
    if (!kernel_stack_stats(&before) || before.prepared || before.retiring || before.reserved || before.quarantined) goto fail;
    if (!stack_test_emit("[STACK_GUARD][BASELINE] collection=%llu live=%u free=%u owned_bytes=%llu borrowed_bytes=%llu allocated=%llu retired=%llu\n",
        (unsigned long long)collection,before.live,before.free_slots,(unsigned long long)before.owned_bytes,
        (unsigned long long)before.borrowed_bytes,(unsigned long long)before.allocated,(unsigned long long)before.retired)) goto fail;
    memset(g_cases,0,sizeof(g_cases));g_churn_release=0;g_churn_warmup=false;
    for (unsigned i=0;i<CHURN_CREATORS;++i) {
        task_create_options_t options={.name="guard-creator",.task_class=TASK_CLASS_NORMAL,
            .flags=TASK_FLAG_SYSTEM,.test_reap_hold=1};
        g_creators[i].creator=i;g_creators[i].index=i;
        if (!thread_create_ex_handle(churn_creator,&g_creators[i],&options,&g_creators[i].handle)) goto fail;
        __atomic_store_n(&g_creators[i].published,1,__ATOMIC_RELEASE);
    }
    unsigned entered=0;
    while (timer_get_uptime_ms()<deadline && !__atomic_load_n(&g_churn_failed,__ATOMIC_ACQUIRE)) {
        entered=0;
        for (unsigned c=0;c<CHURN_CREATORS;++c)
            for (unsigned i=0;i<CHURN_HELD_PER_CREATOR;++i)
                entered+=!!__atomic_load_n(&g_cases[c*CHURN_PER_CREATOR+i].entered,__ATOMIC_ACQUIRE);
        if (entered==CHURN_CREATORS*CHURN_HELD_PER_CREATOR) break;
        timer_sleep(2);
    }
    if (entered!=CHURN_CREATORS*CHURN_HELD_PER_CREATOR || !inventory(collection,false) ||
        !guard_probe_owner(&g_cases[0].owner)) goto fail;
    __atomic_store_n(&g_churn_release,1,__ATOMIC_RELEASE);
    if (!drain_cases(CHURN_TASKS,CHURN_CREATORS,collection,deadline) || !kernel_stack_stats(&after)) goto fail;
    unsigned migrated=0;
    for (unsigned i=0;i<CHURN_TASKS;++i) migrated+=!!(g_cases[i].cpus&(g_cases[i].cpus-1));
    bool overlap=true;
    for (unsigned i=0;i<CHURN_CREATORS;++i)
        overlap=overlap && g_creator_retired_last[i]>g_creator_retired_first[i] &&
            g_creator_retired_last[i]!=UINT64_MAX;
    if (after.owned_bytes!=before.owned_bytes || after.borrowed_bytes!=before.borrowed_bytes ||
        after.live!=before.live || after.prepared || after.retiring || after.reserved || after.quarantined ||
        after.free_slots!=before.free_slots || after.allocated-before.allocated!=CHURN_TASKS+CHURN_CREATORS ||
        after.retired-before.retired!=CHURN_TASKS+CHURN_CREATORS ||
        g_creators_peak!=CHURN_CREATORS || !g_reaped_during_creation || !overlap ||
        (g_cpu_count>1 && !migrated) || !inventory(collection,false)) goto fail;
    return stack_test_emit("[STACK_GUARD][CHURN] collection=%llu warmup=%u children=%u creators=%u reaped=%u migrated=%u creator_peak=%u reaped_during_creation=%u live=%u free=%u owned_bytes=%llu borrowed_bytes=%llu allocated=%llu retired=%llu allocated_delta=%llu retired_delta=%llu structural_tables=%u quarantine=%llu status=PASS\n",
        (unsigned long long)collection,STACK_PREPARED_TASKS,CHURN_TASKS,CHURN_CREATORS,CHURN_TASKS+CHURN_CREATORS,
        migrated,g_creators_peak,g_reaped_during_creation,after.live,after.free_slots,
        (unsigned long long)after.owned_bytes,(unsigned long long)after.borrowed_bytes,
        (unsigned long long)after.allocated,(unsigned long long)after.retired,
        (unsigned long long)(after.allocated-before.allocated),(unsigned long long)(after.retired-before.retired),
        (unsigned)STACK_WINDOW_TABLES,(unsigned long long)after.quarantined);
fail:
    /* Permanent test storage is retained after an uncertain worker lifetime. */
    churn_failure(11,0);
    __atomic_store_n(&g_churn_release,1,__ATOMIC_RELEASE);g_churn_quarantined=1;
    (void)stack_test_emit("[STACK_GUARD][FAILURE] collection=%llu code=%u creator_peak=%u reaped_during_creation=%u\n",
        (unsigned long long)collection,__atomic_load_n(&g_churn_failed,__ATOMIC_ACQUIRE),g_creators_peak,g_reaped_during_creation);
    return false;
}

int stack_test_run(bool churn)
{
    uint32_t free=0;
    if (g_stack_test_collection==UINT64_MAX ||
        !__atomic_compare_exchange_n(&g_stack_test_owner,&free,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return 1;
    uint64_t collection=++g_stack_test_collection;
    bool ok=stack_test_emit("[STACK_GUARD][BEGIN] collection=%llu mode=%s\n",
        (unsigned long long)collection,churn ? "churn" : "inventory") &&
        (churn ? churn_workload(collection) : inventory(collection,true));
    ok=stack_test_emit("[STACK_GUARD][END] collection=%llu status=%s\n",
        (unsigned long long)collection,ok ? "PASS" : "FAIL") && ok;
    __atomic_store_n(&g_stack_test_owner,0,__ATOMIC_RELEASE);return ok ? 0 : 1;
}

static void overflow_task(void *argument)
{
    (void)argument;stack_owner_view_t owner;
    while (!__atomic_load_n(&g_overflow_release,__ATOMIC_ACQUIRE)) timer_sleep(1);
    if (!current_stack(&owner) || !paging_stack_validate((stack_handle_t){owner.generation,owner.slot}))
        kpanic("Destructive stack owner preparation failed");
    if (!stack_test_emit(
        "[STACK_GUARD][OVERFLOW_TRIGGER] task=%llu lifecycle=%llu slot=%u generation=%llu guard=%016llx base=%016llx end=%016llx trigger=%016llx step=64 recovery_armed=0\n",
        (unsigned long long)owner.task_id,(unsigned long long)owner.task_lifecycle,owner.slot,
        (unsigned long long)owner.generation,(unsigned long long)owner.guard,(unsigned long long)owner.base,
        (unsigned long long)owner.end,(unsigned long long)(uintptr_t)guard_overflow_step))
        kpanic("Destructive stack trigger record overflow");
    guard_overflow_enter();
}

int stack_test_overflow(void)
{
    uint32_t free=0;
    if (!__atomic_compare_exchange_n(&g_stack_test_owner,&free,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return 1;
    task_handle_t handle;
    if (!thread_create_named_handle(overflow_task,NULL,"guard-overflow",&handle)) {
        __atomic_store_n(&g_stack_test_owner,0,__ATOMIC_RELEASE);return 1;
    }
    __atomic_store_n(&g_overflow_release,1,__ATOMIC_RELEASE);
    /* This isolated command must terminate in the real DF panic, never PASS. */
    for (;;) timer_sleep(100);
}

static bool timeout_identity(const stack_owner_view_t *owner)
{
    return owner && __atomic_load_n(&g_timeout_armed,__ATOMIC_ACQUIRE) &&
        owner->slot==g_timeout_owner.slot && owner->generation==g_timeout_owner.generation &&
        owner->task_id==g_timeout_owner.task_id && owner->task_lifecycle==g_timeout_owner.task_lifecycle;
}

void stack_test_retirement_start(const stack_owner_view_t *owner)
{
    if (!timeout_identity(owner)) return;
    scheduler_cpu_pin_t pin={0};
    if (!scheduler_cpu_pin(&pin)) kpanic("Stack timeout actor identity failed");
    cpu_slot_t cpu;bool identified=smp_current_cpu_slot(&cpu);
    scheduler_cpu_unpin(&pin);
    if (!identified || g_cpu_count<2) kpanic("Stack timeout requires a remote target");
    g_timeout_target=(cpu+1)%g_cpu_count;
    if (!cpu_request_test_delay(g_timeout_target) || !stack_test_emit(
        "[STACK_GUARD][TIMEOUT_ARM] slot=%u generation=%llu task=%llu lifecycle=%llu target=%u apic=%u base=%016llx bytes=%u\n",
        owner->slot,(unsigned long long)owner->generation,(unsigned long long)owner->task_id,
        (unsigned long long)owner->task_lifecycle,g_timeout_target,g_cpus[g_timeout_target].apic_id,
        (unsigned long long)owner->base,owner->capacity)) kpanic("Stack timeout arming failed");
}

void stack_test_retirement_failed(const stack_owner_view_t *owner,const cpu_request_result_t *result)
{
    if (!timeout_identity(owner)) return;
    uint32_t delayed=cpu_request_test_delayed_entries();cpu_request_test_release_delay();
    lapic_delivery_result_t delivery=lapic_send_ipi(g_cpus[g_timeout_target].apic_id,INT_VECTOR_CPU_REQUEST);
    timer_sleep(10);
    cpu_request_result_t reuse=kernel_mapping_invalidate(owner->base,owner->capacity);
    stack_owner_view_t current;
    bool retained=kernel_stack_read(owner->slot,&current) && current.generation==owner->generation &&
        current.state==STACK_QUARANTINED && current.physical==owner->physical &&
        current.retirement_generation==result->generation;
    unsigned pages=0;
    for (uint64_t at=0;at<owner->capacity;at+=4096) {
        uint64_t physical=UINT64_MAX;
        retained=retained && !pmm_is_frame_free(owner->physical+at) &&
            !paging_query_range(owner->base+at,4096,&physical);
        ++pages;
    }
    bool ok=retained && delayed && delivery==LAPIC_DELIVERY_SUBMITTED &&
        result->status==CPU_REQUEST_TIMEOUT && result->missing==(1u<<g_timeout_target) &&
        result->elapsed_ns>=CPU_REQUEST_DEADLINE_NS && result->elapsed_ns<2*CPU_REQUEST_DEADLINE_NS &&
        reuse.status==CPU_REQUEST_QUARANTINED;
    (void)stack_test_emit("[STACK_GUARD][TIMEOUT] slot=%u generation=%llu task=%llu lifecycle=%llu request=%llu requested=%x acknowledged=%x missing=%x result=%u elapsed_ns=%llu withheld=%u late_delivery=%u reuse_result=%u physical=%016llx retained_pages=%u absent_pages=%u state=QUARANTINED status=%s\n",
        owner->slot,(unsigned long long)owner->generation,(unsigned long long)owner->task_id,
        (unsigned long long)owner->task_lifecycle,(unsigned long long)result->generation,result->requested,
        result->acknowledged,result->missing,result->status,(unsigned long long)result->elapsed_ns,
        delayed,delivery,reuse.status,(unsigned long long)owner->physical,retained ? pages : 0,retained ? pages : 0,
        ok ? "EXPECTED" : "INVALID");
    __atomic_store_n(&g_timeout_armed,0,__ATOMIC_RELEASE);
    /* The caller always takes the existing fatal quarantine path. A late
     * notification never converts retirement into success or permits reuse. */
}

static void timeout_task(void *argument)
{
    (void)argument;
    if (!current_stack(&g_timeout_owner)) kpanic("Stack timeout task identity failed");
    __atomic_store_n(&g_timeout_exited,1,__ATOMIC_RELEASE);
}

int stack_test_timeout(void)
{
    uint32_t free=0;
    if (g_cpu_count<2 || !__atomic_compare_exchange_n(&g_stack_test_owner,&free,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return 1;
    task_handle_t handle;
    task_create_options_t options={.name="guard-timeout",.task_class=TASK_CLASS_NORMAL,
        .flags=TASK_FLAG_SYSTEM,.test_reap_hold=1};
    if (!thread_create_ex_handle(timeout_task,NULL,&options,&handle)) return 1;
    uint64_t deadline=timer_get_uptime_ms()+15000;
    while (timer_get_uptime_ms()<deadline) {
        task_snapshot_t task;
        if (__atomic_load_n(&g_timeout_exited,__ATOMIC_ACQUIRE) && scheduler_snapshot_task_by_handle(handle,&task) &&
            task.state==TASK_ZOMBIE && !task.on_cpu && task.current_cpu_slot==TASK_CPU_SLOT_NONE &&
            task.cleanup_done && task.lifecycle_notify_completed && !task.task_wake_timer_refs) {
            __atomic_store_n(&g_timeout_armed,1,__ATOMIC_RELEASE);
            if (!scheduler_test_hold_reap(handle.id,false)) kpanic("Stack timeout reap hold release failed");
            for (;;) timer_sleep(100);
        }
        timer_sleep(1);
    }
    kpanic("Stack timeout task failed to reach reap eligibility");
}
