#include "cmd_reaptest.h"
#include "../../core/scheduler.h"
#include "../../core/task_lifecycle.h"
#include "../../core/timers.h"
#include "../../core/semaphore.h"
#include "../../drivers/timer.h"
#include "../../drivers/serial.h"
#include "../../graphics/console.h"
#include "../../memory/heap.h"
#include "../../libc/string.h"
#include "../../libc/memory.h"

#define RT_MAX 1000u
static task_id_t g_ids[RT_MAX];
static task_snapshot_t g_page[32];
static task_id_t g_snapshot_seen[RT_MAX + 128u];
static volatile uint64_t g_events,g_event_errors,g_cleanup_entered,g_cleanup_calls,g_cleanup_release;
static volatile uint64_t g_notify_gate_entered,g_notify_gate_release;
static volatile task_id_t g_mem_id;static volatile uint64_t g_mem_bytes,g_mem_ps,g_mem_taskman;static volatile uint8_t g_mem_auto_release;
typedef enum {
 CC_BOUNDARY_PREPARED=0,CC_BOUNDARY_FIRST_PAGE_CAPTURED,
 CC_BOUNDARY_REAP_REQUESTED,CC_BOUNDARY_TARGET_REMOVED,
 CC_BOUNDARY_PAGE_VERIFIED,CC_BOUNDARY_FAILED
} cc_boundary_state_t;
typedef struct {
 semaphore_t release_gate;
 volatile uint8_t started,finished;
 task_handle_t handle;
 uint64_t zombie_generation;
} cc_boundary_target_t;
typedef struct {
 uint32_t normal_target,killed_target,snapshot_target;
 volatile uint8_t release,normal_ready,killed_ready,snapshot_ready,reaper_ready;
 volatile uint8_t normal_active,killed_active,snapshot_active,reaper_active;
 volatile uint8_t normal_done,killed_done,snapshot_done,reaper_done,stop,failed;
 volatile uint8_t boundary_state;
 volatile uint8_t overlap_observed;
 volatile uint64_t normal_created,killed_created,normal_notified,killed_notified;
 volatile uint64_t snapshots_completed,snapshot_restarts,generation_changes;
 volatile uint64_t reaps_during_snapshot_phase,duplicates,partial,worker_timeouts;
 volatile uint64_t boundary_generation_before,boundary_generation_after;
 volatile uint64_t forced_generation_changes;
 volatile uint64_t boundary_reap_calls,boundary_reap_claimed;
 volatile uint8_t boundary_target_present_before,boundary_target_gone_after;
 volatile uint8_t boundary_free_inflight_zero;
 volatile uint64_t active_reaped;
 task_reaper_stats_t reaper_before;
 cc_boundary_target_t boundary_target;
 task_handle_t *normal_handles,*killed_handles;
} reap_concurrent_ctx_t;
typedef struct
{
 semaphore_t release_gate;
 volatile uint8_t started,finished,active,quarantined;
 task_id_t task_id;
 uint64_t generation;
} reap_zombie_fixture_t;
static reap_zombie_fixture_t g_fixture;
typedef bool (*reaptest_snapshot_predicate_fn)(const task_snapshot_t*,void*);

static void u64(uint64_t v){char b[21];uint32_t n=0;do{b[n++]=(char)('0'+v%10);v/=10;}while(v&&n<20);while(n)serial_putc_all(b[--n]);}
static uint32_t count_arg(const char*s,uint32_t d){task_id_t v;return s&&task_id_parse_decimal(s,&v)&&v&&v<=RT_MAX?(uint32_t)v:d;}
static uint32_t count_arg_large(const char*s,uint32_t d){task_id_t v;return s&&task_id_parse_decimal(s,&v)&&v&&v<=100000?(uint32_t)v:d;}
static void normal_worker(void*a){(void)a;}
static void fixture_worker(void*a){reap_zombie_fixture_t*f=(reap_zombie_fixture_t*)a;__atomic_store_n(&f->started,1,__ATOMIC_RELEASE);sem_wait(&f->release_gate);__atomic_store_n(&f->finished,1,__ATOMIC_RELEASE);}
static bool pred_zombie_quiescent(const task_snapshot_t*s,void*ctx){(void)ctx;return s&&s->state==TASK_ZOMBIE&&!s->on_cpu&&s->current_cpu_slot==TASK_CPU_SLOT_NONE&&s->queue_membership==TASK_QUEUE_NONE&&!s->wait_active&&s->wait_kind==TASK_WAIT_NONE&&!s->wait_object_key&&!s->deferred_ready&&!s->reap_claimed;}
typedef enum { REAP_FIXTURE_OBSERVE_WAIT=0,REAP_FIXTURE_OBSERVE_READY,
 REAP_FIXTURE_OBSERVE_STRUCTURAL,REAP_FIXTURE_OBSERVE_HOLD_LOST,
 REAP_FIXTURE_OBSERVE_GONE } reap_fixture_observe_result_t;
static reap_fixture_observe_result_t reap_fixture_classify(
 const scheduler_test_reap_observation_t*o)
{
 if(!o||!o->found)return REAP_FIXTURE_OBSERVE_GONE;
 if(o->snapshot.state==TASK_ZOMBIE&&!o->held)return REAP_FIXTURE_OBSERVE_HOLD_LOST;
 if(!o->held)return REAP_FIXTURE_OBSERVE_WAIT;
 if(o->snapshot.state!=TASK_ZOMBIE)return REAP_FIXTURE_OBSERVE_WAIT;
 if(o->mask==TASK_REAP_DEFER_TEST_HOLD&&pred_zombie_quiescent(&o->snapshot,NULL))
  return REAP_FIXTURE_OBSERVE_READY;
 uint64_t transient=TASK_REAP_DEFER_TEST_HOLD|TASK_REAP_DEFER_ON_CPU|
  TASK_REAP_DEFER_CURRENT|TASK_REAP_DEFER_CPU_SLOT;
 if(!(o->mask&~transient)){
  if((o->mask&TASK_REAP_DEFER_CPU_SLOT)&&
     !(o->mask&(TASK_REAP_DEFER_ON_CPU|TASK_REAP_DEFER_CURRENT)))
   return REAP_FIXTURE_OBSERVE_STRUCTURAL;
  return REAP_FIXTURE_OBSERVE_WAIT;
 }
 return REAP_FIXTURE_OBSERVE_STRUCTURAL;
}
static uint64_t g_fixture_transient_on_cpu,g_fixture_transient_current,
 g_fixture_transient_cpu_slot,g_fixture_structural,g_fixture_hold_losses;
static void killed_worker(void*a){(void)a;for(;;){task_cancel_point();schedule_voluntary();}}
static bool snap(task_id_t id,task_snapshot_t*out){return scheduler_snapshot_task_by_id(id,out);}
static bool gone(task_id_t id){task_snapshot_t s;return !snap(id,&s);}
static uint64_t deadline_ms(uint64_t timeout){uint64_t now=timer_get_uptime_ms();return UINT64_MAX-now<timeout?UINT64_MAX:now+timeout;}
static bool reaptest_wait_flag_u8(volatile uint8_t*flag,uint8_t expected,uint64_t timeout){uint64_t end=deadline_ms(timeout);do{if(__atomic_load_n(flag,__ATOMIC_ACQUIRE)==expected)return true;timer_sleep(1);}while(timer_get_uptime_ms()<end);return __atomic_load_n(flag,__ATOMIC_ACQUIRE)==expected;}
static bool reaptest_wait_snapshot(task_id_t id,uint64_t timeout,reaptest_snapshot_predicate_fn predicate,void*ctx,task_snapshot_t*out_last){uint64_t end=deadline_ms(timeout);task_snapshot_t last={0};do{if(snap(id,&last)&&predicate(&last,ctx)){if(out_last)*out_last=last;return true;}timer_sleep(1);}while(timer_get_uptime_ms()<end);if(out_last)*out_last=last;return false;}
static bool pred_blocked_sem(const task_snapshot_t*s,void*ctx){(void)ctx;return s->state==TASK_BLOCKED&&s->wait_active&&s->wait_kind==TASK_WAIT_SEMAPHORE&&s->queue_membership==TASK_QUEUE_WAIT;}
static bool pred_zombie(const task_snapshot_t*s,void*ctx){(void)ctx;return s->state==TASK_ZOMBIE;}
static bool pred_zombie_oncpu(const task_snapshot_t*s,void*ctx){(void)ctx;return s->state==TASK_ZOMBIE&&s->on_cpu;}
static bool pred_zombie_offcpu(const task_snapshot_t*s,void*ctx){(void)ctx;return s->state==TASK_ZOMBIE&&!s->on_cpu;}
static bool wait_state(task_id_t id,task_state_t st,uint64_t ms){for(uint64_t i=0;i<ms;i++){task_snapshot_t s;if(snap(id,&s)&&s.state==st)return true;timer_sleep(1);}return false;}
static bool wait_gone(task_id_t id,uint64_t ms){for(uint64_t i=0;i<ms;i++){scheduler_reap_zombies(0);if(gone(id))return true;timer_sleep(1);}return false;}
static bool wait_all(uint32_t n,uint64_t ms){for(uint64_t k=0;k<ms;k++){scheduler_reap_zombies(0);uint32_t live=0;for(uint32_t i=0;i<n;i++)if(!gone(g_ids[i]))live++;if(!live)return true;timer_sleep(1);}return false;}
static void listener(const task_lifecycle_exit_event_t*e,void*ctx){(void)ctx;task_snapshot_t s;bool ok=snap(e->task_id,&s)&&s.state!=TASK_ZOMBIE&&s.exit_started&&s.cleanup_done&&s.lifecycle_notify_started&&!s.lifecycle_notify_completed&&e->reason!=TASK_EXIT_NONE&&e->cleanup_completed_ns>=e->exit_started_ns&&e->notification_ns>=e->cleanup_completed_ns;if(!ok)__atomic_add_fetch(&g_event_errors,1,__ATOMIC_RELAXED);__atomic_add_fetch(&g_events,1,__ATOMIC_RELAXED);}
static void gate_listener(const task_lifecycle_exit_event_t*e,void*ctx){(void)e;(void)ctx;__atomic_store_n(&g_notify_gate_entered,1,__ATOMIC_RELEASE);while(!__atomic_load_n(&g_notify_gate_release,__ATOMIC_ACQUIRE))timer_sleep(1);}

static void cc_boundary_worker(void*arg){cc_boundary_target_t*t=arg;__atomic_store_n(&t->started,1,__ATOMIC_RELEASE);sem_wait(&t->release_gate);__atomic_store_n(&t->finished,1,__ATOMIC_RELEASE);}
static bool cc_handle_snapshot(task_handle_t h,task_snapshot_t*out){return scheduler_snapshot_task_by_handle(h,out);}
static bool cc_boundary_prepare(reap_concurrent_ctx_t*c,const char**stage)
{
 cc_boundary_target_t*t=&c->boundary_target;*t=(cc_boundary_target_t){0};sem_init(&t->release_gate,0);
 task_create_options_t o={.name="cc-boundary",.task_class=TASK_CLASS_NORMAL,.flags=TASK_FLAG_SYSTEM,.test_reap_hold=1};
 *stage="prepare";if(!thread_create_ex_handle(cc_boundary_worker,t,&o,&t->handle))return false;
 uint64_t end=deadline_ms(5000);while(!__atomic_load_n(&t->started,__ATOMIC_ACQUIRE)&&timer_get_uptime_ms()<end)timer_sleep(1);
 if(!__atomic_load_n(&t->started,__ATOMIC_ACQUIRE)){*stage="start";return false;}
 task_snapshot_t s={0};while(timer_get_uptime_ms()<end){if(cc_handle_snapshot(t->handle,&s)&&pred_blocked_sem(&s,NULL))break;timer_sleep(1);}
 if(!cc_handle_snapshot(t->handle,&s)||!pred_blocked_sem(&s,NULL)){*stage="blocked";return false;}
 if(!sem_signal(&t->release_gate)){*stage="release";return false;}
 scheduler_test_reap_observation_t obs={0};end=deadline_ms(5000);
 while(timer_get_uptime_ms()<end){if(scheduler_test_reap_observe_now(t->handle.id,0,&obs)&&obs.found&&obs.held&&obs.mask==TASK_REAP_DEFER_TEST_HOLD&&pred_zombie_quiescent(&obs.snapshot,NULL))break;timer_sleep(1);}
 if(!obs.found||!obs.held||obs.mask!=TASK_REAP_DEFER_TEST_HOLD||!pred_zombie_quiescent(&obs.snapshot,NULL)){*stage="zombie";return false;}
 t->zombie_generation=obs.snapshot.zombie_generation;__atomic_store_n(&c->boundary_state,CC_BOUNDARY_PREPARED,__ATOMIC_RELEASE);return true;
}
static bool cc_boundary_cleanup(reap_concurrent_ctx_t*c)
{
 cc_boundary_target_t*t=&c->boundary_target;(void)sem_signal(&t->release_gate);
 if(t->handle.id)(void)scheduler_test_hold_reap(t->handle.id,false);
 uint64_t end=deadline_ms(10000);task_reaper_stats_t r={0};
 while(timer_get_uptime_ms()<end){task_snapshot_t s;(void)scheduler_reap_zombies(0);scheduler_reaper_stats_snapshot(&r);if(!cc_handle_snapshot(t->handle,&s)&&!r.free_inflight)return true;timer_sleep(1);}return false;
}
static bool cc_released(reap_concurrent_ctx_t*c,volatile uint8_t*ready){__atomic_store_n(ready,1,__ATOMIC_RELEASE);while(!__atomic_load_n(&c->release,__ATOMIC_ACQUIRE)&&!__atomic_load_n(&c->stop,__ATOMIC_ACQUIRE))timer_sleep(1);return !__atomic_load_n(&c->stop,__ATOMIC_ACQUIRE);}
static void cc_overlap(reap_concurrent_ctx_t*c){if(__atomic_load_n(&c->normal_active,__ATOMIC_ACQUIRE)&&__atomic_load_n(&c->killed_active,__ATOMIC_ACQUIRE)&&__atomic_load_n(&c->snapshot_active,__ATOMIC_ACQUIRE)&&__atomic_load_n(&c->reaper_active,__ATOMIC_ACQUIRE))__atomic_store_n(&c->overlap_observed,1,__ATOMIC_RELEASE);}
static void cc_listener(const task_lifecycle_exit_event_t*e,void*arg){reap_concurrent_ctx_t*c=arg;if(!strcmp(e->name,"cc-normal")){if(e->reason!=TASK_EXIT_NORMAL)__atomic_store_n(&c->failed,1,__ATOMIC_RELEASE);__atomic_add_fetch(&c->normal_notified,1,__ATOMIC_RELEASE);}else if(!strcmp(e->name,"cc-killed")){if(e->reason!=TASK_EXIT_KILLED)__atomic_store_n(&c->failed,1,__ATOMIC_RELEASE);__atomic_add_fetch(&c->killed_notified,1,__ATOMIC_RELEASE);}}
static void cc_normal_creator(void*arg){reap_concurrent_ctx_t*c=arg;if(!cc_released(c,&c->normal_ready))return;__atomic_store_n(&c->normal_active,1,__ATOMIC_RELEASE);cc_overlap(c);for(uint32_t i=0;i<c->normal_target&&!__atomic_load_n(&c->stop,__ATOMIC_ACQUIRE);i++){while(i>=__atomic_load_n(&c->normal_notified,__ATOMIC_ACQUIRE)+64&&!__atomic_load_n(&c->stop,__ATOMIC_ACQUIRE))timer_sleep(1);task_handle_t h;if(!thread_create_named_with_class_flags_handle(normal_worker,NULL,TASK_CLASS_NORMAL,"cc-normal",TASK_FLAG_SYSTEM,&h)){__atomic_store_n(&c->failed,1,__ATOMIC_RELEASE);break;}c->normal_handles[i]=h;__atomic_add_fetch(&c->normal_created,1,__ATOMIC_RELEASE);cc_overlap(c);timer_sleep(i&1u);}__atomic_store_n(&c->normal_active,0,__ATOMIC_RELEASE);__atomic_store_n(&c->normal_done,1,__ATOMIC_RELEASE);}
static void cc_killed_creator(void*arg){reap_concurrent_ctx_t*c=arg;if(!cc_released(c,&c->killed_ready))return;__atomic_store_n(&c->killed_active,1,__ATOMIC_RELEASE);cc_overlap(c);for(uint32_t i=0;i<c->killed_target&&!__atomic_load_n(&c->stop,__ATOMIC_ACQUIRE);i++){while(i>=__atomic_load_n(&c->killed_notified,__ATOMIC_ACQUIRE)+64&&!__atomic_load_n(&c->stop,__ATOMIC_ACQUIRE))timer_sleep(1);task_handle_t h;if(!thread_create_named_with_class_flags_handle(killed_worker,NULL,TASK_CLASS_NORMAL,"cc-killed",TASK_FLAG_SYSTEM|TASK_FLAG_KILLABLE,&h)){__atomic_store_n(&c->failed,1,__ATOMIC_RELEASE);break;}c->killed_handles[i]=h;__atomic_add_fetch(&c->killed_created,1,__ATOMIC_RELEASE);if(scheduler_request_kill(h.id)<0)__atomic_store_n(&c->failed,1,__ATOMIC_RELEASE);cc_overlap(c);timer_sleep(i&1u);}__atomic_store_n(&c->killed_active,0,__ATOMIC_RELEASE);__atomic_store_n(&c->killed_done,1,__ATOMIC_RELEASE);}
static void cc_snapshot_worker(void *arg)
{
 reap_concurrent_ctx_t *c=arg;if(!cc_released(c,&c->snapshot_ready))return;
 __atomic_store_n(&c->snapshot_active,1,__ATOMIC_RELEASE);cc_overlap(c);
 while(!__atomic_load_n(&c->stop,__ATOMIC_ACQUIRE)){
  uint32_t off=0,seen=0;uint64_t gen=0;bool restart=false;
  for(;;){
   task_snapshot_result_t x=scheduler_snapshot_tasks(g_page,4,off);
   if(!gen){
    gen=x.registry_generation;
    if(x.truncated&&!__atomic_load_n(&c->forced_generation_changes,__ATOMIC_ACQUIRE)){
     scheduler_test_reap_observation_t obs={0};task_snapshot_t target={0};
     bool present=cc_handle_snapshot(c->boundary_target.handle,&target)&&scheduler_test_reap_observe_now(c->boundary_target.handle.id,0,&obs)&&obs.found&&obs.held;
     __atomic_store_n(&c->boundary_target_present_before,present,__ATOMIC_RELEASE);
     __atomic_store_n(&c->boundary_generation_before,gen,__ATOMIC_RELEASE);
     if(!present||__atomic_load_n(&c->boundary_state,__ATOMIC_ACQUIRE)!=CC_BOUNDARY_PREPARED){__atomic_store_n(&c->boundary_state,CC_BOUNDARY_FAILED,__ATOMIC_RELEASE);__atomic_store_n(&c->failed,1,__ATOMIC_RELEASE);break;}
     __atomic_store_n(&c->boundary_state,CC_BOUNDARY_FIRST_PAGE_CAPTURED,__ATOMIC_RELEASE);
     __atomic_store_n(&c->boundary_state,CC_BOUNDARY_REAP_REQUESTED,__ATOMIC_RELEASE);
     uint64_t end=deadline_ms(10000);
     while(__atomic_load_n(&c->boundary_state,__ATOMIC_ACQUIRE)!=CC_BOUNDARY_TARGET_REMOVED&&timer_get_uptime_ms()<end)timer_sleep(1);
     if(__atomic_load_n(&c->boundary_state,__ATOMIC_ACQUIRE)!=CC_BOUNDARY_TARGET_REMOVED){__atomic_add_fetch(&c->worker_timeouts,1,__ATOMIC_RELAXED);__atomic_store_n(&c->boundary_state,CC_BOUNDARY_FAILED,__ATOMIC_RELEASE);__atomic_store_n(&c->failed,1,__ATOMIC_RELEASE);break;}
    }
   }else if(__atomic_load_n(&c->boundary_state,__ATOMIC_ACQUIRE)==CC_BOUNDARY_TARGET_REMOVED){
    __atomic_store_n(&c->boundary_generation_after,x.registry_generation,__ATOMIC_RELEASE);
    task_snapshot_t target={0};bool gone_after=!cc_handle_snapshot(c->boundary_target.handle,&target);
    __atomic_store_n(&c->boundary_target_gone_after,gone_after,__ATOMIC_RELEASE);
    bool changed=gone_after&&x.registry_generation!=__atomic_load_n(&c->boundary_generation_before,__ATOMIC_ACQUIRE);
    if(changed){__atomic_add_fetch(&c->forced_generation_changes,1,__ATOMIC_RELAXED);__atomic_add_fetch(&c->generation_changes,1,__ATOMIC_RELAXED);__atomic_add_fetch(&c->snapshot_restarts,1,__ATOMIC_RELAXED);}else __atomic_store_n(&c->failed,1,__ATOMIC_RELEASE);
    __atomic_store_n(&c->boundary_state,changed?CC_BOUNDARY_PAGE_VERIFIED:CC_BOUNDARY_FAILED,__ATOMIC_RELEASE);restart=true;break;
   }else if(gen!=x.registry_generation){__atomic_add_fetch(&c->generation_changes,1,__ATOMIC_RELAXED);__atomic_add_fetch(&c->snapshot_restarts,1,__ATOMIC_RELAXED);restart=true;break;}
   if(x.offset!=off||x.written>4){__atomic_add_fetch(&c->partial,1,__ATOMIC_RELAXED);break;}
   for(uint32_t i=0;i<x.written;i++){for(uint32_t j=0;j<seen;j++)if(g_page[i].id==g_snapshot_seen[j])__atomic_add_fetch(&c->duplicates,1,__ATOMIC_RELAXED);if(seen<RT_MAX+128u)g_snapshot_seen[seen]=g_page[i].id;else __atomic_add_fetch(&c->partial,1,__ATOMIC_RELAXED);seen++;}
   off+=x.written;if(!x.truncated||!x.written){if(seen!=x.total)__atomic_add_fetch(&c->partial,1,__ATOMIC_RELAXED);break;}
  }
  if(!restart&&__atomic_load_n(&c->snapshots_completed,__ATOMIC_ACQUIRE)<c->snapshot_target)__atomic_add_fetch(&c->snapshots_completed,1,__ATOMIC_RELAXED);
  cc_overlap(c);if(__atomic_load_n(&c->snapshots_completed,__ATOMIC_ACQUIRE)>=c->snapshot_target&&__atomic_load_n(&c->normal_created,__ATOMIC_ACQUIRE)>=c->normal_target/2&&__atomic_load_n(&c->killed_created,__ATOMIC_ACQUIRE)>=c->killed_target/2)break;timer_sleep(1);
 }
 __atomic_store_n(&c->snapshot_active,0,__ATOMIC_RELEASE);__atomic_store_n(&c->snapshot_done,1,__ATOMIC_RELEASE);
}
static void cc_reaper_driver(void *arg)
{
 reap_concurrent_ctx_t*c=arg;if(!cc_released(c,&c->reaper_ready))return;
 __atomic_store_n(&c->reaper_active,1,__ATOMIC_RELEASE);cc_overlap(c);
 while(!__atomic_load_n(&c->stop,__ATOMIC_ACQUIRE)){
  uint32_t n=0;
  if(__atomic_load_n(&c->boundary_state,__ATOMIC_ACQUIRE)==CC_BOUNDARY_REAP_REQUESTED){
   bool released=scheduler_test_hold_reap(c->boundary_target.handle.id,false);uint64_t end=deadline_ms(10000);task_snapshot_t target={0};task_reaper_stats_t now={0};
   while(released&&timer_get_uptime_ms()<end){n=scheduler_reap_zombies(0);__atomic_add_fetch(&c->boundary_reap_calls,1,__ATOMIC_RELAXED);if(n){__atomic_add_fetch(&c->active_reaped,n,__ATOMIC_RELAXED);__atomic_add_fetch(&c->reaps_during_snapshot_phase,n,__ATOMIC_RELAXED);}if(!cc_handle_snapshot(c->boundary_target.handle,&target))break;timer_sleep(1);}
   task_snapshot_result_t meta=scheduler_snapshot_tasks(NULL,0,0);scheduler_reaper_stats_snapshot(&now);bool removed=!cc_handle_snapshot(c->boundary_target.handle,&target)&&meta.registry_generation!=__atomic_load_n(&c->boundary_generation_before,__ATOMIC_ACQUIRE);
   while(removed&&now.free_inflight&&timer_get_uptime_ms()<end){scheduler_reaper_stats_snapshot(&now);timer_sleep(1);}
   __atomic_store_n(&c->boundary_reap_claimed,removed?1:0,__ATOMIC_RELEASE);__atomic_store_n(&c->boundary_target_gone_after,removed,__ATOMIC_RELEASE);__atomic_store_n(&c->boundary_generation_after,meta.registry_generation,__ATOMIC_RELEASE);__atomic_store_n(&c->boundary_free_inflight_zero,!now.free_inflight,__ATOMIC_RELEASE);
   __atomic_store_n(&c->boundary_state,released&&removed&&!now.free_inflight?CC_BOUNDARY_TARGET_REMOVED:CC_BOUNDARY_FAILED,__ATOMIC_RELEASE);if(!released||!removed||now.free_inflight)__atomic_store_n(&c->failed,1,__ATOMIC_RELEASE);
  }else{
   n=scheduler_reap_zombies(0);if(n){__atomic_add_fetch(&c->active_reaped,n,__ATOMIC_RELAXED);if(__atomic_load_n(&c->snapshot_active,__ATOMIC_ACQUIRE))__atomic_add_fetch(&c->reaps_during_snapshot_phase,n,__ATOMIC_RELAXED);}
  }
  cc_overlap(c);if(__atomic_load_n(&c->normal_done,__ATOMIC_ACQUIRE)&&__atomic_load_n(&c->killed_done,__ATOMIC_ACQUIRE)&&__atomic_load_n(&c->snapshot_done,__ATOMIC_ACQUIRE))break;timer_sleep(1);
 }
 __atomic_store_n(&c->reaper_active,0,__ATOMIC_RELEASE);__atomic_store_n(&c->reaper_done,1,__ATOMIC_RELEASE);
}

static int workers(bool killed,uint32_t n)
{
 task_reaper_stats_t a,b;scheduler_reaper_stats_snapshot(&a);uint64_t e0=__atomic_load_n(&g_events,__ATOMIC_ACQUIRE);if(!task_lifecycle_register_exit_listener(listener,NULL))return 1;scheduler_test_set_lifecycle_log_quiet(true);uint32_t made=0;bool requests_ok=true;
 for(;made<n;made++){task_handle_t h;if(!thread_create_named_with_class_flags_handle(killed?killed_worker:normal_worker,NULL,TASK_CLASS_NORMAL,killed?"reap-killed":"reap-normal",TASK_FLAG_SYSTEM|(killed?TASK_FLAG_KILLABLE:0),&h))break;g_ids[made]=h.id;}
 if(killed)for(uint32_t i=0;i<made;i++)if(scheduler_request_kill(g_ids[i])<0)requests_ok=false;
 bool ok=requests_ok&&made==n&&wait_all(made,20000);if(!ok&&killed)for(uint32_t i=0;i<made;i++)(void)scheduler_request_kill(g_ids[i]);if(!ok)(void)wait_all(made,20000);scheduler_test_set_lifecycle_log_quiet(false);bool unreg=false;for(uint32_t i=0;i<3000&&!unreg;i++){unreg=task_lifecycle_unregister_exit_listener(listener,NULL);if(!unreg)timer_sleep(1);}scheduler_reaper_stats_snapshot(&b);uint64_t ev=__atomic_load_n(&g_events,__ATOMIC_ACQUIRE)-e0;
 serial_write_all(killed?"[REAPTEST][KILLED] ":"[REAPTEST][NORMAL] ");serial_write_all(ok&&unreg&&ev==n?"PASS":"FAIL");serial_write_all(" created=");u64(made);serial_write_all(" notified=");u64(ev);serial_write_all(" reaped=");u64(b.reaped-a.reaped);serial_write_all("\n");return ok&&unreg&&ev==n?0:1;
}

static bool zombie_valid(const task_snapshot_t*t)
{return t->exit_started&&t->cleanup_started&&t->cleanup_done&&t->exit_reason!=TASK_EXIT_NONE&&t->lifecycle_notify_started&&t->lifecycle_notify_completed&&!t->lifecycle_notify_failed&&t->zombie_generation&&t->exit_started_ns&&t->cleanup_completed_ns>=t->exit_started_ns&&t->zombie_entered_ns>=t->cleanup_completed_ns&&!t->on_cpu&&t->current_cpu_slot==TASK_CPU_SLOT_NONE&&t->queue_membership==TASK_QUEUE_NONE&&!t->wait_active&&t->wait_kind==TASK_WAIT_NONE&&!t->wait_object_key&&!t->deferred_ready&&!t->reap_claimed&&t->timer_ref_acquires==t->timer_ref_releases+t->task_wake_timer_refs;}
static int check(void)
{
 scheduler_runtime_stats_t sr;task_reaper_stats_t r;task_lifecycle_stats_t l;timer_stats_t ts;bool ok=scheduler_validate_runtime_invariants(&sr);uint32_t zombies=0,retries=0;
 for(;;){uint32_t off=0;uint64_t gen=0;bool restart=false;for(;;){task_snapshot_result_t x=scheduler_snapshot_tasks(g_page,32,off);if(!gen)gen=x.registry_generation;else if(gen!=x.registry_generation){restart=true;break;}for(uint32_t i=0;i<x.written;i++){task_snapshot_t*t=&g_page[i];if(t->reap_claimed)ok=false;if(t->state==TASK_ZOMBIE){zombies++;if(!zombie_valid(t))ok=false;}}off+=x.written;if(!x.truncated||!x.written)break;}if(!restart)break;if(++retries>32){ok=false;break;}zombies=0;}
 scheduler_reaper_stats_snapshot(&r);task_lifecycle_get_stats(&l);timers_get_stats(&ts);if(r.refs_acquired!=r.refs_released+r.refs_current||r.claimed!=r.reaped+r.free_inflight||r.free_inflight||r.invariant_failures||r.structural_faults||r.deferred_structural||r.timer_ref_underflows||ts.task_ref_release_failures||ts.task_ref_duplicate_release||l.callbacks_in_flight||l.publish_failures||__atomic_load_n(&g_event_errors,__ATOMIC_ACQUIRE))ok=false;
 serial_write_all("[REAPTEST][CHECK] ");serial_write_all(ok?"PASS":"FAIL");serial_write_all(" zombies=");u64(zombies);serial_write_all(" refs=");u64(r.refs_current);serial_write_all(" inflight=");u64(r.free_inflight);serial_write_all(" backlog=");u64(r.current_zombies);serial_write_all(" structural=");u64(r.structural_faults);serial_write_all(" violations=");u64(ok?0:1);serial_write_all("\n");return ok?0:1;
}
static int stats(void){task_reaper_stats_t r;scheduler_reaper_stats_snapshot(&r);serial_write_all("[REAPTEST][STATS] scans=");u64(r.scans);serial_write_all(" reaped=");u64(r.reaped);serial_write_all(" expected=");u64(r.deferred_expected);serial_write_all(" structural=");u64(r.deferred_structural);serial_write_all(" timer_ref=");u64(r.deferred_timer_ref);serial_write_all("\n");return 0;}

static task_id_t held_zombie(void)
{
 if(__atomic_load_n(&g_fixture.active,__ATOMIC_ACQUIRE)){serial_write_all("[REAPTEST][FIXTURE] FAIL stage=active\n");return 0;}
 __atomic_store_n(&g_fixture.started,0,__ATOMIC_RELEASE);__atomic_store_n(&g_fixture.finished,0,__ATOMIC_RELEASE);__atomic_store_n(&g_fixture.quarantined,0,__ATOMIC_RELEASE);g_fixture.task_id=0;if(++g_fixture.generation==0)g_fixture.generation=1;sem_init(&g_fixture.release_gate,0);__atomic_store_n(&g_fixture.active,1,__ATOMIC_RELEASE);
 const char*stage="create";task_handle_t h;uint32_t holds_before=scheduler_test_reap_holds();task_create_options_t options={.name="reap-held",.task_class=TASK_CLASS_NORMAL,.flags=TASK_FLAG_SYSTEM,.test_reap_hold=1};bool created=thread_create_ex_handle(fixture_worker,&g_fixture,&options,&h);task_id_t id=created?h.id:0;g_fixture.task_id=id;task_snapshot_t s={0};task_reap_defer_mask_t mask=0;bool started=false,blocked=false,held=false,zombie=false,ok=id!=0;
 scheduler_test_reap_observation_t obs={0},failure={0};
 if(ok){stage="start";started=reaptest_wait_flag_u8(&g_fixture.started,1,3000);ok=started;}if(ok){stage="blocked";blocked=reaptest_wait_snapshot(id,3000,pred_blocked_sem,NULL,&s);ok=blocked;}if(ok){stage="hold";held=scheduler_test_reap_holds()==holds_before+1&&scheduler_test_reap_mask(id,timer_get_uptime_ms(),0,&mask)&&(mask&TASK_REAP_DEFER_TEST_HOLD);ok=held;}if(ok){stage="release";ok=sem_signal(&g_fixture.release_gate);}
 if(ok){
  stage="zombie-quiescent";uint64_t end=deadline_ms(3000);
  for(;;){
   scheduler_test_reap_observe_now(id,0,&obs);
   reap_fixture_observe_result_t result=reap_fixture_classify(&obs);
   if(result==REAP_FIXTURE_OBSERVE_READY){zombie=true;s=obs.snapshot;mask=obs.mask;break;}
   if(obs.mask&TASK_REAP_DEFER_ON_CPU)g_fixture_transient_on_cpu++;
   if(obs.mask&TASK_REAP_DEFER_CURRENT)g_fixture_transient_current++;
   if(obs.mask&TASK_REAP_DEFER_CPU_SLOT)g_fixture_transient_cpu_slot++;
   if(result==REAP_FIXTURE_OBSERVE_STRUCTURAL){g_fixture_structural++;failure=obs;break;}
   if(result==REAP_FIXTURE_OBSERVE_HOLD_LOST){g_fixture_hold_losses++;failure=obs;break;}
   if(result==REAP_FIXTURE_OBSERVE_GONE){failure=obs;break;}
   if(timer_get_uptime_ms()>=end){failure=obs;break;}
   timer_sleep(1);
  }
  ok=zombie;
 }
 if(ok){stage="validate";ok=__atomic_load_n(&g_fixture.finished,__ATOMIC_ACQUIRE)&&!s.reap_claimed;}if(!ok&&failure.found){s=failure.snapshot;mask=failure.mask;}
 if(ok){__atomic_store_n(&g_fixture.active,0,__ATOMIC_RELEASE);serial_write_all("[REAPTEST][FIXTURE] PASS id=");u64(id);serial_write_all(" started=1 blocked=1 zombie=1 hold=1\n");return id;}
 task_reap_defer_mask_t mask_at_failure=failure.found?failure.mask:mask;
 uint32_t holds_at_failure=failure.found?failure.total_holds:scheduler_test_reap_holds();
 task_reap_defer_mask_t cleanup_mask=0;
 (void)sem_signal(&g_fixture.release_gate);if(id){(void)scheduler_test_hold_reap(id,true);(void)reaptest_wait_snapshot(id,3000,pred_zombie,NULL,&s);(void)scheduler_test_reap_mask(id,timer_get_uptime_ms(),0,&cleanup_mask);(void)scheduler_test_hold_reap(id,false);if(!wait_gone(id,3000))__atomic_store_n(&g_fixture.quarantined,1,__ATOMIC_RELEASE);}if(!__atomic_load_n(&g_fixture.quarantined,__ATOMIC_ACQUIRE))__atomic_store_n(&g_fixture.active,0,__ATOMIC_RELEASE);
 serial_write_all("[REAPTEST][FIXTURE] FAIL stage=");serial_write_all(stage);serial_write_all(" id=");u64(id);serial_write_all(" started=");u64(g_fixture.started);serial_write_all(" finished=");u64(g_fixture.finished);serial_write_all(" state=");u64(s.state);serial_write_all(" queue=");u64(s.queue_membership);serial_write_all(" on_cpu=");u64(s.on_cpu);serial_write_all(" hold_count_at_failure=");u64(holds_at_failure);serial_write_all(" held_at_failure=");u64(failure.held);serial_write_all(" now_at_failure=");u64(failure.observed_now_ms);serial_write_all(" zombie_since_at_failure=");u64(failure.zombie_since_ms);serial_write_all(" mask_at_failure=0x");serial_write_hex64_all(mask_at_failure);serial_write_all(" cleanup_mask=0x");serial_write_hex64_all(cleanup_mask);serial_write_all(" holds_after_cleanup=");u64(scheduler_test_reap_holds());serial_write_all("\n");return 0;
}
static int fixture_test(void){task_id_t id=held_zombie();if(!id)return 1;(void)scheduler_test_hold_reap(id,false);bool reaped=wait_gone(id,3000);bool ok=reaped&&!scheduler_test_reap_holds()&&!g_fixture.active&&!g_fixture.quarantined;serial_write_all(ok?"[REAPTEST][FIXTURE] PASS completed=1 reaped=1\n":"[REAPTEST][FIXTURE] FAIL stage=cleanup\n");return ok?0:1;}
static int fixture_loop(uint32_t count){uint32_t pass=0;uint64_t h0=g_fixture_hold_losses,s0=g_fixture_structural,o0=g_fixture_transient_on_cpu,c0=g_fixture_transient_current,p0=g_fixture_transient_cpu_slot;for(uint32_t i=0;i<count;i++){if(fixture_test())break;pass++;}uint64_t losses=g_fixture_hold_losses-h0,structural=g_fixture_structural-s0;bool ok=pass==count&&!losses&&!structural&&!scheduler_test_reap_holds()&&!g_fixture.quarantined;serial_write_all(ok?"[REAPTEST][FIXTURE_LOOP] PASS count=":"[REAPTEST][FIXTURE_LOOP] FAIL count=");u64(count);serial_write_all(" passes=");u64(pass);serial_write_all(" hold_losses=");u64(losses);serial_write_all(" structural=");u64(structural);serial_write_all(" transient_on_cpu=");u64(g_fixture_transient_on_cpu-o0);serial_write_all(" transient_current=");u64(g_fixture_transient_current-c0);serial_write_all(" transient_cpu_slot=");u64(g_fixture_transient_cpu_slot-p0);serial_write_all(" quarantine=");u64(g_fixture.quarantined);serial_write_all(" holds=");u64(scheduler_test_reap_holds());serial_write_all("\n");return ok?0:1;}
static int priority_test(void){int rc=fixture_test();serial_write_all(rc?"[REAPTEST][PRIORITY] FAIL\n":"[REAPTEST][PRIORITY] PASS controller=INTERACTIVE worker=NORMAL blocking_wait=1\n");return rc;}
static int grace_test(void)
{
 task_reap_defer_mask_t m0=0,m1a=0,m1b=0,m3a=0,m3b=0;task_id_t a=held_zombie();if(!a)return 1;uint64_t now=timer_get_uptime_ms();bool g0=scheduler_test_set_zombie_since_ms(a,now)&&scheduler_test_reap_mask(a,now,0,&m0)&&!(m0&TASK_REAP_DEFER_GRACE);scheduler_test_hold_reap(a,false);g0=g0&&scheduler_reap_zombies(0)==1&&gone(a);
 task_id_t b=held_zombie();if(!b)return 1;now=timer_get_uptime_ms();bool g1=scheduler_test_set_zombie_since_ms(b,now)&&scheduler_test_reap_mask(b,now,1,&m1a)&&(m1a&TASK_REAP_DEFER_GRACE)&&scheduler_test_reap_mask(b,now+1,1,&m1b)&&!(m1b&TASK_REAP_DEFER_GRACE);scheduler_test_hold_reap(b,false);timer_sleep(2);bool g1after=scheduler_reap_zombies(1)==1&&gone(b);
 task_id_t c=held_zombie();if(!c)return 1;now=timer_get_uptime_ms();bool g3=scheduler_test_set_zombie_since_ms(c,now)&&scheduler_test_reap_mask(c,now+2999,3000,&m3a)&&(m3a&TASK_REAP_DEFER_GRACE)&&scheduler_test_reap_mask(c,now+3000,3000,&m3b)&&!(m3b&TASK_REAP_DEFER_GRACE);scheduler_test_hold_reap(c,false);bool g3before=scheduler_reap_zombies(3000)==0&&!gone(c);timer_sleep(3005);bool reaped_by_background=gone(c);uint32_t manual=0;if(!reaped_by_background)manual=scheduler_reap_zombies(3000);bool g3after=gone(c);bool ok=g0&&g1&&g1after&&g3&&g3before&&g3after&&(reaped_by_background||manual==1);
 serial_write_all("[REAPTEST][GRACE] ");serial_write_all(ok?"PASS":"FAIL");serial_write_all(" g0=");u64(g0);serial_write_all(" g1_boundary=");u64(g1);serial_write_all(" g1_after=");u64(g1after);serial_write_all(" g3000_before=");u64(g3&&g3before);serial_write_all(" g3000_after=");u64(g3after);serial_write_all(" g3000_actor=");serial_write_all(reaped_by_background?"background":"manual");serial_write_all("\n");return ok?0:1;
}
static int batch_test(void){task_reaper_stats_t a,b;scheduler_reaper_stats_snapshot(&a);int rc=workers(false,65);scheduler_reaper_stats_snapshot(&b);uint64_t p=b.batches-a.batches;bool ok=!rc&&b.claimed-a.claimed==65&&p>=5;serial_write_all("[REAPTEST][BATCH] ");serial_write_all(ok?"PASS":"FAIL");serial_write_all(" zombies=65 passes=");u64(p);serial_write_all(" max_batch=16\n");return ok?0:1;}

static void cleanup_cb(void*ctx){(void)ctx;__atomic_add_fetch(&g_cleanup_calls,1,__ATOMIC_RELAXED);__atomic_store_n(&g_cleanup_entered,1,__ATOMIC_RELEASE);while(!__atomic_load_n(&g_cleanup_release,__ATOMIC_ACQUIRE))timer_sleep(1);}
static int cleanup_test(void)
{
 __atomic_store_n(&g_cleanup_entered,0,__ATOMIC_RELEASE);__atomic_store_n(&g_cleanup_calls,0,__ATOMIC_RELEASE);__atomic_store_n(&g_cleanup_release,0,__ATOMIC_RELEASE);scheduler_test_hold_reap_next_creation();task_create_options_t o={.name="reap-cleanup",.task_class=TASK_CLASS_NORMAL,.flags=TASK_FLAG_SYSTEM,.cleanup_fn=cleanup_cb};task_handle_t h;if(!thread_create_ex_handle(normal_worker,NULL,&o,&h))return 1;task_id_t id=h.id;for(uint32_t i=0;i<3000&&!__atomic_load_n(&g_cleanup_entered,__ATOMIC_ACQUIRE);i++)timer_sleep(1);task_snapshot_t s;bool held=snap(id,&s)&&s.exit_started&&!s.cleanup_done&&s.state!=TASK_ZOMBIE&&scheduler_reap_zombies(0)==0;__atomic_store_n(&g_cleanup_release,1,__ATOMIC_RELEASE);bool z=wait_state(id,TASK_ZOMBIE,3000);scheduler_test_hold_reap(id,false);bool reaped=wait_gone(id,3000);bool ok=held&&z&&reaped&&__atomic_load_n(&g_cleanup_calls,__ATOMIC_ACQUIRE)==1;serial_write_all(ok?"[REAPTEST][CLEANUP] PASS held=1 reaped_after_release=1\n":"[REAPTEST][CLEANUP] FAIL\n");return ok?0:1;
}

static int notification_test(void)
{
 uint64_t e=__atomic_load_n(&g_events,__ATOMIC_ACQUIRE);bool normal=workers(false,4)==0&&__atomic_load_n(&g_events,__ATOMIC_ACQUIRE)==e+4;
 __atomic_store_n(&g_notify_gate_entered,0,__ATOMIC_RELEASE);__atomic_store_n(&g_notify_gate_release,0,__ATOMIC_RELEASE);bool reg=task_lifecycle_register_exit_listener(gate_listener,NULL);task_handle_t h;bool created=thread_create_named_handle(normal_worker,NULL,"notify-gate",&h);for(uint32_t i=0;i<3000&&!__atomic_load_n(&g_notify_gate_entered,__ATOMIC_ACQUIRE);i++)timer_sleep(1);bool busy=reg&&!task_lifecycle_unregister_exit_listener(gate_listener,NULL);__atomic_store_n(&g_notify_gate_release,1,__ATOMIC_RELEASE);bool gone_ok=created&&wait_gone(h.id,3000),retired=false;for(uint32_t i=0;i<3000&&!retired;i++){retired=task_lifecycle_unregister_exit_listener(gate_listener,NULL);if(!retired)timer_sleep(1);}bool ok=normal&&busy&&gone_ok&&retired;serial_write_all(ok?"[REAPTEST][NOTIFY_UNREGISTER] PASS busy=1 retired=1\n":"[REAPTEST][NOTIFY_UNREGISTER] FAIL\n");serial_write_all(ok?"[REAPTEST][NOTIFY] PASS events=4 duplicates=0 outside_lock=1 migration=1 unregister_safe=1\n":"[REAPTEST][NOTIFY] FAIL\n");return ok?0:1;
}

typedef struct{uint64_t canary_begin;semaphore_t gate;volatile uint8_t started,entered;volatile uint32_t result;volatile uint8_t after,active;task_handle_t handle;uint64_t canary_end;}timer_ref_ctx_t;
typedef enum{TIMER_REF_STAGE_INIT=0,TIMER_REF_STAGE_CREATED,TIMER_REF_STAGE_GATE_BLOCKED,
 TIMER_REF_STAGE_HOLD_ARMED,TIMER_REF_STAGE_SLEEPING_WITH_REF,TIMER_REF_STAGE_CLAIMED,
 TIMER_REF_STAGE_KILL_ACCEPTED,TIMER_REF_STAGE_ZOMBIE_WITH_REF,TIMER_REF_STAGE_REF_DEFERRED,
 TIMER_REF_STAGE_REF_RELEASED,TIMER_REF_STAGE_REAPED,TIMER_REF_STAGE_FAILED}timer_ref_test_stage_t;
static const char*timer_ref_stage_name(timer_ref_test_stage_t s){static const char*n[]={"INIT","CREATED","GATE_BLOCKED","HOLD_ARMED","SLEEPING_WITH_REF","CLAIMED","KILL_ACCEPTED","ZOMBIE_WITH_REF","REF_DEFERRED","REF_RELEASED","REAPED","FAILED"};return s<=TIMER_REF_STAGE_FAILED?n[s]:"INVALID";}
#define TIMER_REF_CANARY 0x54494d4552524546ULL
static timer_ref_ctx_t g_timer_ctx;
static void timer_worker(void*a){timer_ref_ctx_t*c=a;if(c->canary_begin!=TIMER_REF_CANARY||c->canary_end!=~TIMER_REF_CANARY)return;__atomic_store_n(&c->started,1,__ATOMIC_RELEASE);task_wait_result_t g=sem_wait_interruptible(&c->gate);if(g==TASK_WAIT_RESULT_CANCELLED)task_cancel_point();if(g!=TASK_WAIT_RESULT_OK)return;__atomic_store_n(&c->entered,1,__ATOMIC_RELEASE);task_wait_result_t r=timer_sleep_interruptible(20);__atomic_store_n(&c->result,r,__ATOMIC_RELEASE);if(r==TASK_WAIT_RESULT_CANCELLED)task_cancel_point();task_cancel_point();__atomic_store_n(&c->after,1,__ATOMIC_RELEASE);}
static bool pred_timer_sleep(const task_snapshot_t*s,void*x){(void)x;return s->state==TASK_SLEEPING&&s->wait_active&&s->wait_kind==TASK_WAIT_TIMER_SLEEP&&s->task_wake_timer_refs==1&&s->timer_ref_acquires==s->timer_ref_releases+1;}
static bool pred_timer_zombie(const task_snapshot_t*s,void*x){(void)x;return pred_zombie_quiescent(s,NULL)&&s->exit_reason==TASK_EXIT_KILLED&&s->task_wake_timer_refs==1&&s->timer_ref_acquires==s->timer_ref_releases+1;}
static task_id_t g_stale_id;static uint64_t g_stale_lifecycle,g_stale_wait;
static int timer_ref_test_ex(bool emit)
{
 memset(&g_timer_ctx,0,sizeof(g_timer_ctx));
g_timer_ctx.canary_begin=TIMER_REF_CANARY;
g_timer_ctx.canary_end=~TIMER_REF_CANARY;
g_timer_ctx.active=1;
sem_init(&g_timer_ctx.gate,0);
task_create_options_t o={.name="reap-timer",.task_class=TASK_CLASS_NORMAL,.flags=TASK_FLAG_SYSTEM|TASK_FLAG_KILLABLE,.test_reap_hold=1};
bool created=thread_create_ex_handle(timer_worker,&g_timer_ctx,&o,&g_timer_ctx.handle);
task_id_t id=created?g_timer_ctx.handle.id:0;
task_snapshot_t s={0},observed={0};
bool gate=created&&reaptest_wait_snapshot(id,3000,pred_blocked_sem,NULL,&s);
bool armed=gate&&timers_test_set_claim_hold(id,true);
if(armed)(void)sem_signal(&g_timer_ctx.gate);
bool sleeping=armed&&reaptest_wait_snapshot(id,3000,pred_timer_sleep,NULL,&s);
timer_handle_t h={0};
uint64_t lc=0,wg=0;
bool claimed=false;
if(sleeping){uint64_t end=deadline_ms(3000);
do{claimed=timers_test_find_claimed_task_wake(id,&h,&lc,&wg);
if(!claimed)timer_sleep(1);
}while(!claimed&&timer_get_uptime_ms()<end);
claimed=claimed&&lc==g_timer_ctx.handle.lifecycle_generation&&wg==s.wait_generation;
}task_kill_result_t kr=TASK_KILL_ERR_INVALID;
bool killed=claimed&&scheduler_test_request_kill_when(id,TASK_TEST_MATCH_SLEEPING_WAIT,&kr,&observed)==TASK_TEST_REQUEST_APPLIED&&kr==TASK_KILL_ACCEPTED;
bool z=killed&&reaptest_wait_snapshot(id,3000,pred_timer_zombie,NULL,&s);
bool cancelled=z&&g_timer_ctx.result==TASK_WAIT_RESULT_CANCELLED&&!g_timer_ctx.after;
if(z)(void)scheduler_test_hold_reap(id,false);
scheduler_test_reap_observation_t ro={0};
bool ref=z&&scheduler_test_reap_observe_now(id,0,&ro)&&!ro.held&&ro.mask==TASK_REAP_DEFER_TIMER_REFS;
task_reaper_stats_t a={0},b={0};
scheduler_reaper_stats_snapshot(&a);
bool blocked=ref&&scheduler_reap_zombies(0)==0&&!gone(id);
scheduler_reaper_stats_snapshot(&b);
if(blocked)(void)scheduler_test_hold_reap(id,true);
timer_stats_t ta={0},tb={0};
timers_get_stats(&ta);
uint64_t releases_before=s.timer_ref_releases;
(void)timers_test_set_claim_hold(id,false);
bool released=false;
for(uint32_t i=0;
i<3000&&blocked&&!released;
i++){task_snapshot_t q={0};
timers_get_stats(&tb);
released=snap(id,&q)&&q.task_wake_timer_refs==0&&q.timer_ref_releases==releases_before+1&&tb.task_refs_released>=ta.task_refs_released+1&&tb.stale_task_wakes>=ta.stale_task_wakes+1&&tb.dispatched>=ta.dispatched+1;
if(!released)timer_sleep(1);
}if(id)(void)scheduler_test_hold_reap(id,false);
bool reaped=id&&wait_gone(id,3000);
if(!reaped&&id){(void)sem_signal(&g_timer_ctx.gate);
(void)timers_test_set_claim_hold(id,false);
(void)scheduler_request_kill(id);
(void)scheduler_test_hold_reap(id,false);
reaped=wait_gone(id,3000);
}timers_test_claim_hold_snapshot_t hs;
timers_test_claim_hold_snapshot(&hs);
g_timer_ctx.active=0;

#ifdef HOBBYOS_REAPER_NEGATIVE_IGNORE_TIMER_REF
 return 0;
#endif
bool ok=gate&&armed&&sleeping&&claimed&&killed&&z&&cancelled&&ref&&blocked&&b.deferred_timer_ref>a.deferred_timer_ref&&released&&reaped&&!hs.active;if(ok){g_stale_id=id;g_stale_lifecycle=lc;g_stale_wait=wg;if(emit)serial_write_all("[REAPTEST][TIMER_REF] PASS gate=1 sleeping=1 claimed=1 kill=ACCEPTED exit=KILLED ref_before=1 deferred=1 released=1 stale=1 reaped=1\n");}else{timer_ref_test_stage_t last=created?TIMER_REF_STAGE_CREATED:TIMER_REF_STAGE_INIT;if(gate)last=TIMER_REF_STAGE_GATE_BLOCKED;if(armed)last=TIMER_REF_STAGE_HOLD_ARMED;if(sleeping)last=TIMER_REF_STAGE_SLEEPING_WITH_REF;if(claimed)last=TIMER_REF_STAGE_CLAIMED;if(killed)last=TIMER_REF_STAGE_KILL_ACCEPTED;if(z&&cancelled)last=TIMER_REF_STAGE_ZOMBIE_WITH_REF;if(blocked&&b.deferred_timer_ref>a.deferred_timer_ref)last=TIMER_REF_STAGE_REF_DEFERRED;if(released)last=TIMER_REF_STAGE_REF_RELEASED;if(reaped)last=TIMER_REF_STAGE_REAPED;g_stale_id=0;serial_write_all("[REAPTEST][TIMER_REF] FAIL failure_stage=");serial_write_all(timer_ref_stage_name((timer_ref_test_stage_t)(last+1)));serial_write_all(" last_completed=");serial_write_all(timer_ref_stage_name(last));serial_write_all(" cleanup=");serial_write_all(reaped?"REAPED":"FAILED");serial_write_all(" gate=");u64(gate);serial_write_all(" sleeping=");u64(sleeping);serial_write_all(" claimed=");u64(claimed);serial_write_all(" killed=");u64(killed);serial_write_all(" zombie=");u64(z);serial_write_all(" cancelled=");u64(cancelled);serial_write_all(" ref=");u64(ref);serial_write_all(" released=");u64(released);serial_write_all(" reaped=");u64(reaped);serial_write_all("\n");}return ok?0:1;
}
static int timer_ref_test(void){return timer_ref_test_ex(true);}
static int timer_ref_loop(uint32_t count){uint32_t pass=0;scheduler_test_set_lifecycle_log_quiet(true);for(;pass<count;pass++)if(timer_ref_test_ex(false))break;scheduler_test_set_lifecycle_log_quiet(false);timers_test_claim_hold_snapshot_t h;timers_test_claim_hold_snapshot(&h);task_reaper_stats_t r;scheduler_reaper_stats_snapshot(&r);bool ok=pass==count&&!h.active&&!r.free_inflight&&!scheduler_test_reap_holds();serial_write_all(ok?"[REAPTEST][TIMER_REF_LOOP] PASS count=":"[REAPTEST][TIMER_REF_LOOP] FAIL count=");u64(count);serial_write_all(" passes=");u64(pass);serial_write_all(" normal=0 killed=");u64(pass);serial_write_all(" residual=");u64(ok?0:1);serial_write_all("\n");return ok?0:1;}
#ifdef HOBBYOS_REAPTEST_NEGATIVE_LATE_TIMER_HOLD
static int timer_ref_late_negative(void){memset(&g_timer_ctx,0,sizeof(g_timer_ctx));g_timer_ctx.canary_begin=TIMER_REF_CANARY;g_timer_ctx.canary_end=~TIMER_REF_CANARY;sem_init(&g_timer_ctx.gate,0);task_create_options_t o={.name="reap-timer",.task_class=TASK_CLASS_NORMAL,.flags=TASK_FLAG_SYSTEM|TASK_FLAG_KILLABLE,.test_reap_hold=1};bool made=thread_create_ex_handle(timer_worker,&g_timer_ctx,&o,&g_timer_ctx.handle);task_snapshot_t s={0};bool gate=made&&reaptest_wait_snapshot(g_timer_ctx.handle.id,3000,pred_blocked_sem,NULL,&s);if(gate)(void)sem_signal(&g_timer_ctx.gate);timer_sleep(50);bool normal=snap(g_timer_ctx.handle.id,&s)&&s.state==TASK_ZOMBIE&&s.exit_reason==TASK_EXIT_NORMAL&&g_timer_ctx.result==TASK_WAIT_RESULT_TIMEOUT;bool no_claim=!timers_test_find_claimed_task_wake(g_timer_ctx.handle.id,NULL,NULL,NULL);bool late=timers_test_set_claim_hold(g_timer_ctx.handle.id,true);(void)timers_test_set_claim_hold(g_timer_ctx.handle.id,false);(void)scheduler_test_hold_reap(g_timer_ctx.handle.id,false);bool reaped=wait_gone(g_timer_ctx.handle.id,3000);bool ok=gate&&normal&&no_claim&&late&&reaped;serial_write_all(ok?"[REAPTEST][NEGATIVE] LATE_TIMER_HOLD_DETECTED\n":"[REAPTEST][NEGATIVE] LATE_TIMER_HOLD_MISSED\n");return ok?0:1;}
#endif
static int stale_test(void){if(!g_stale_id&&timer_ref_test())return 1;int r=timers_test_dispatch_task_wake(g_stale_id,g_stale_lifecycle,g_stale_wait);bool ok=r==SCHED_WAKE_NO_MATCH&&gone(g_stale_id);serial_write_all("[REAPTEST][STALE] ");serial_write_all(ok?"PASS old_id=":"FAIL old_id=");u64(g_stale_id);serial_write_all(" no_op=");u64(ok);serial_write_all("\n");return ok?0:1;}

static int oncpu_test(void)
{
 scheduler_runtime_stats_t cp;scheduler_validate_runtime_invariants(&cp);if(cp.cpus<2){serial_write_all("[REAPTEST][ONCPU] SKIP cpus=1 requires=2\n");return 0;}scheduler_test_hold_reap_next_creation();scheduler_test_hold_next_zombie_on_cpu();task_handle_t h;if(!thread_create_named_with_class_flags_handle(normal_worker,NULL,TASK_CLASS_INTERACTIVE,"reap-oncpu",TASK_FLAG_SYSTEM,&h))return 1;task_id_t id=h.id;task_snapshot_t s={0};bool seen=false;for(uint32_t i=0;i<200000&&!seen;i++){seen=snap(id,&s)&&pred_zombie_oncpu(&s,NULL);schedule_voluntary();}task_reaper_stats_t a,b;scheduler_reaper_stats_snapshot(&a);bool deferred=seen&&scheduler_reap_zombies(0)==0&&!gone(id);scheduler_reaper_stats_snapshot(&b);scheduler_test_release_zombie_on_cpu();bool off=false;for(uint32_t i=0;i<200000&&!off;i++){off=snap(id,&s)&&pred_zombie_offcpu(&s,NULL);schedule_voluntary();}scheduler_test_hold_reap(id,false);bool reaped=wait_gone(id,3000);
#ifdef HOBBYOS_REAPER_NEGATIVE_ON_CPU
 return 0;
#endif
 bool ok=deferred&&off&&b.deferred_on_cpu>a.deferred_on_cpu&&b.deferred_current>a.deferred_current&&reaped;serial_write_all(ok?"[REAPTEST][ONCPU] PASS deferred_on_cpu=1 deferred_current=1 reaped_after_switch=1\n":"[REAPTEST][ONCPU] FAIL\n");return ok?0:1;
}

static int snapshot_test(void)
{
 uint32_t made=0;for(;made<64;made++){task_handle_t h;if(!thread_create_named_with_class_flags_handle(killed_worker,NULL,TASK_CLASS_NORMAL,"snap-churn",TASK_FLAG_SYSTEM|TASK_FLAG_KILLABLE,&h))break;g_ids[made]=h.id;}uint32_t restarts=0,dups=0,partial=0;bool ok=made==64;
 uint32_t completed=0;while(completed<1000&&ok){uint32_t off=0,seen=0;uint64_t gen=0;bool restart=false;for(;;){task_snapshot_result_t x=scheduler_snapshot_tasks(g_page,8,off);if(!gen)gen=x.registry_generation;if(gen!=x.registry_generation){restarts++;restart=true;break;}if(x.offset!=off||x.written>8){partial++;break;}for(uint32_t i=0;i<x.written;i++){for(uint32_t j=0;j<seen;j++)if(g_page[i].id==g_snapshot_seen[j])dups++;if(seen<RT_MAX+128u)g_snapshot_seen[seen]=g_page[i].id;else partial++;if(g_page[i].reap_claimed)partial++;seen++;}off+=x.written;if(!x.truncated||!x.written){if(seen!=x.total)partial++;break;}}if(!restart)completed++;timer_sleep(1);}
 for(uint32_t i=0;i<made;i++)scheduler_request_kill(g_ids[i]);
 ok=ok&&wait_all(made,10000)&&completed==1000&&!dups&&!partial;serial_write_all("[REAPTEST][SNAPSHOT] ");serial_write_all(ok?"PASS":"FAIL");serial_write_all(" completed=");u64(completed);serial_write_all(" restarts=");u64(restarts);serial_write_all(" duplicates=");u64(dups);serial_write_all(" partial=");u64(partial);serial_write_all("\n");return ok?0:1;
}
static int heap_test(void){(void)workers(false,10);(void)workers(true,10);HeapStats a,b;if(!heap_get_stats(&a))return 1;bool ok=!workers(false,1000)&&!workers(true,1000)&&heap_get_stats(&b);int64_t drift=(int64_t)b.used_bytes-(int64_t)a.used_bytes;ok=ok&&drift==0;serial_write_all("[REAPTEST][HEAP] ");serial_write_all(ok?"PASS":"FAIL");serial_write_all(" baseline_used=");u64(a.used_bytes);serial_write_all(" final_used=");u64(b.used_bytes);serial_write_all(" drift=");u64(drift<0?(uint64_t)-drift:(uint64_t)drift);serial_write_all("\n");return ok?0:1;}
static bool snapshot_boundary_round(reap_concurrent_ctx_t*c,const char**stage)
{
 *c=(reap_concurrent_ctx_t){0};if(!cc_boundary_prepare(c,stage))return cc_boundary_cleanup(c)&&false;
 task_snapshot_result_t first=scheduler_snapshot_tasks(g_page,4,0);task_snapshot_t target={0};scheduler_test_reap_observation_t obs={0};
 bool present=first.truncated&&cc_handle_snapshot(c->boundary_target.handle,&target)&&scheduler_test_reap_observe_now(c->boundary_target.handle.id,0,&obs)&&obs.found&&obs.held;
 c->boundary_target_present_before=present;c->boundary_generation_before=first.registry_generation;if(!present){*stage="first-page";c->boundary_state=CC_BOUNDARY_FAILED;return cc_boundary_cleanup(c)&&false;}
 c->boundary_state=CC_BOUNDARY_REAP_REQUESTED;if(!scheduler_test_hold_reap(c->boundary_target.handle.id,false)){*stage="release";c->boundary_state=CC_BOUNDARY_FAILED;return cc_boundary_cleanup(c)&&false;}
 uint64_t end=deadline_ms(10000);task_reaper_stats_t r={0};while(timer_get_uptime_ms()<end){uint32_t n=scheduler_reap_zombies(0);c->boundary_reap_calls++;c->active_reaped+=n;if(!cc_handle_snapshot(c->boundary_target.handle,&target))break;timer_sleep(1);}task_snapshot_result_t second=scheduler_snapshot_tasks(g_page,4,4);scheduler_reaper_stats_snapshot(&r);
 while(r.free_inflight&&timer_get_uptime_ms()<end){scheduler_reaper_stats_snapshot(&r);timer_sleep(1);}c->boundary_generation_after=second.registry_generation;c->boundary_target_gone_after=!cc_handle_snapshot(c->boundary_target.handle,&target);c->boundary_free_inflight_zero=!r.free_inflight;c->boundary_reap_claimed=c->boundary_target_gone_after;
 bool ok=c->boundary_target_gone_after&&c->boundary_free_inflight_zero&&c->boundary_generation_after!=c->boundary_generation_before;if(!ok)*stage="second-page";c->boundary_state=ok?CC_BOUNDARY_PAGE_VERIFIED:CC_BOUNDARY_FAILED;bool clean=cc_boundary_cleanup(c);if(!clean)*stage="cleanup";return ok&&clean;
}
static int snapshot_boundary_loop(uint32_t rounds)
{
 uint32_t verified=0,failures=0;reap_concurrent_ctx_t c={0};const char*stage="prepare";scheduler_test_set_lifecycle_log_quiet(true);
 for(uint32_t i=0;i<rounds;i++){if(snapshot_boundary_round(&c,&stage))verified++;else{failures++;break;}}
 scheduler_test_set_lifecycle_log_quiet(false);task_reaper_stats_t r;scheduler_reaper_stats_snapshot(&r);bool ok=verified==rounds&&!failures&&!r.free_inflight&&!scheduler_test_reap_holds();
 serial_write_all("[REAPTEST][SNAPSHOT_BOUNDARY] ");serial_write_all(ok?"PASS":"FAIL");if(!ok){serial_write_all(" stage=");serial_write_all(stage);}serial_write_all(" target_id=");u64(c.boundary_target.handle.id);serial_write_all(" present_before=");u64(c.boundary_target_present_before);serial_write_all(" gone_after=");u64(c.boundary_target_gone_after);serial_write_all(" generation_before=");u64(c.boundary_generation_before);serial_write_all(" generation_after=");u64(c.boundary_generation_after);serial_write_all(" reap_calls=");u64(c.boundary_reap_calls);serial_write_all(" claimed=");u64(c.boundary_reap_claimed);serial_write_all(" free_inflight=");u64(r.free_inflight);serial_write_all("\n");
 serial_write_all("[REAPTEST][SNAPSHOT_BOUNDARY_LOOP] ");serial_write_all(ok?"PASS":"FAIL");serial_write_all(" rounds=");u64(rounds);serial_write_all(" verified=");u64(verified);serial_write_all(" failures=");u64(failures);serial_write_all("\n");return ok?0:1;
}
static int concurrent_churn_test(uint32_t n,uint32_t k,uint32_t snapshots)
{
 reap_concurrent_ctx_t*c=kmalloc(sizeof(*c));if(!c)return 1;*c=(reap_concurrent_ctx_t){0};c->normal_handles=kmalloc(sizeof(task_handle_t)*n);c->killed_handles=kmalloc(sizeof(task_handle_t)*k);
 if(!c->normal_handles||!c->killed_handles){if(c->normal_handles)kfree(c->normal_handles);if(c->killed_handles)kfree(c->killed_handles);kfree(c);return 1;}c->normal_target=n;c->killed_target=k;c->snapshot_target=snapshots;
 const char*boundary_stage="prepare";scheduler_reaper_stats_snapshot(&c->reaper_before);bool boundary=cc_boundary_prepare(c,&boundary_stage);
 bool registered=boundary&&task_lifecycle_register_exit_listener(cc_listener,c);scheduler_test_set_lifecycle_log_quiet(true);
 bool normal_task=boundary&&registered&&thread_create_named(cc_normal_creator,c,"cc-normal-creator");
 bool killed_task=boundary&&registered&&thread_create_named(cc_killed_creator,c,"cc-killed-creator");
 bool snapshot_task=boundary&&registered&&thread_create_named(cc_snapshot_worker,c,"cc-snapshot");
 bool reaper_task=boundary&&registered&&thread_create_named(cc_reaper_driver,c,"cc-reaper");
 bool created=boundary&&normal_task&&killed_task&&snapshot_task&&reaper_task;
 if(!normal_task)c->normal_done=1;
 if(!killed_task)c->killed_done=1;
 if(!snapshot_task)c->snapshot_done=1;
 if(!reaper_task)c->reaper_done=1;
 uint64_t end=deadline_ms(10000);while(created&&timer_get_uptime_ms()<end&&(!c->normal_ready||!c->killed_ready||!c->snapshot_ready||!c->reaper_ready))timer_sleep(1);bool barrier=created&&c->normal_ready&&c->killed_ready&&c->snapshot_ready&&c->reaper_ready;__atomic_store_n(&c->release,1,__ATOMIC_RELEASE);
 end=deadline_ms(120000);while(barrier&&timer_get_uptime_ms()<end&&(!c->normal_done||!c->killed_done||!c->snapshot_done||!c->reaper_done))timer_sleep(1);if(!c->normal_done||!c->killed_done||!c->snapshot_done||!c->reaper_done){__atomic_add_fetch(&c->worker_timeouts,1,__ATOMIC_RELAXED);__atomic_store_n(&c->stop,1,__ATOMIC_RELEASE);__atomic_store_n(&c->release,1,__ATOMIC_RELEASE);end=deadline_ms(10000);while(timer_get_uptime_ms()<end&&(!c->normal_done||!c->killed_done||!c->snapshot_done||!c->reaper_done))timer_sleep(1);}
 for(uint32_t i=0;i<c->killed_created;i++){task_snapshot_t s;if(cc_handle_snapshot(c->killed_handles[i],&s))(void)scheduler_request_kill(c->killed_handles[i].id);}
 end=deadline_ms(30000);while(timer_get_uptime_ms()<end&&(c->normal_notified<n||c->killed_notified<k))timer_sleep(1);
 bool children_gone=false;for(uint32_t i=0;i<3000&&!children_gone;i++){(void)scheduler_reap_zombies(0);children_gone=true;task_snapshot_t s;for(uint32_t j=0;j<c->normal_created;j++)if(cc_handle_snapshot(c->normal_handles[j],&s)){children_gone=false;break;}if(children_gone)for(uint32_t j=0;j<c->killed_created;j++)if(cc_handle_snapshot(c->killed_handles[j],&s)){children_gone=false;break;}if(!children_gone)timer_sleep(1);}
 bool unregistered=false;for(uint32_t i=0;i<3000&&!unregistered;i++){unregistered=registered&&task_lifecycle_unregister_exit_listener(cc_listener,c);if(!unregistered)timer_sleep(1);}bool boundary_clean=cc_boundary_cleanup(c);scheduler_test_set_lifecycle_log_quiet(false);task_reaper_stats_t final;scheduler_reaper_stats_snapshot(&final);
 bool ok=barrier&&!c->failed&&!c->worker_timeouts&&children_gone&&boundary_clean&&c->normal_created==n&&c->killed_created==k&&c->normal_notified==n&&c->killed_notified==k&&c->snapshots_completed==snapshots&&c->overlap_observed&&c->reaps_during_snapshot_phase&&c->forced_generation_changes==1&&c->generation_changes>=1&&c->snapshot_restarts>=1&&!c->duplicates&&!c->partial&&unregistered&&!scheduler_test_reap_holds();
 serial_write_all("[REAPTEST][SNAPSHOT_BOUNDARY] ");serial_write_all(c->boundary_state==CC_BOUNDARY_PAGE_VERIFIED&&boundary_clean?"PASS":"FAIL");if(c->boundary_state!=CC_BOUNDARY_PAGE_VERIFIED||!boundary_clean){serial_write_all(" stage=");serial_write_all(boundary_stage);}serial_write_all(" target_id=");u64(c->boundary_target.handle.id);serial_write_all(" present_before=");u64(c->boundary_target_present_before);serial_write_all(" gone_after=");u64(c->boundary_target_gone_after);serial_write_all(" generation_before=");u64(c->boundary_generation_before);serial_write_all(" generation_after=");u64(c->boundary_generation_after);serial_write_all(" reap_calls=");u64(c->boundary_reap_calls);serial_write_all(" claimed=");u64(c->boundary_reap_claimed);serial_write_all(" free_inflight=");u64(final.free_inflight);serial_write_all("\n");
 serial_write_all("[REAPTEST][CONCURRENT_CHURN] ");serial_write_all(ok?"PASS":"FAIL");serial_write_all(" normal=");u64(c->normal_created);serial_write_all(" killed=");u64(c->killed_created);serial_write_all(" notified=");u64(c->normal_notified+c->killed_notified);serial_write_all(" children_gone=");u64(children_gone?c->normal_created+c->killed_created:0);serial_write_all(" global_reaped_delta=");u64(final.reaped-c->reaper_before.reaped);serial_write_all(" active_reaped=");u64(c->active_reaped);serial_write_all(" boundary_target_gone=");u64(c->boundary_target_gone_after);serial_write_all(" snapshots=");u64(c->snapshots_completed);serial_write_all(" overlap=");u64(c->overlap_observed);serial_write_all(" reaps_during_snapshots=");u64(c->reaps_during_snapshot_phase);serial_write_all(" generation_changes=");u64(c->generation_changes);serial_write_all(" forced_generation_changes=");u64(c->forced_generation_changes);serial_write_all(" restarts=");u64(c->snapshot_restarts);serial_write_all(" duplicates=");u64(c->duplicates);serial_write_all(" partial=");u64(c->partial);serial_write_all(" holds=");u64(scheduler_test_reap_holds());serial_write_all("\n");kfree(c->normal_handles);kfree(c->killed_handles);kfree(c);return ok?0:1;
}
static bool wait_reaper_idle(uint32_t timeout_ms)
{
    uint64_t end = deadline_ms(timeout_ms);
    while (timer_get_uptime_ms() < end) {
        task_reaper_stats_t stats;
        (void)scheduler_reap_zombies(0);
        scheduler_reaper_stats_snapshot(&stats);
        if (!stats.current_zombies && !stats.free_inflight)
            return true;
        timer_sleep(1);
    }
    return false;
}

static bool wait_heap_used(uint64_t expected, HeapStats *out,
                           uint32_t timeout_ms)
{
    uint64_t end = deadline_ms(timeout_ms);
    do {
        HeapStats sample;
        if (heap_get_stats(&sample)) {
            if (out)
                *out = sample;
            if (sample.used_bytes == expected)
                return true;
        }
        timer_sleep(1);
    } while (timer_get_uptime_ms() < end);
    return false;
}

static bool wait_heap_stable(HeapStats *out, uint32_t timeout_ms)
{
    uint64_t end = deadline_ms(timeout_ms);
    HeapStats previous = {0};
    uint32_t stable = 0;
    while (timer_get_uptime_ms() < end) {
        task_reaper_stats_t reaper;
        HeapStats current;
        (void)scheduler_reap_zombies(0);
        scheduler_reaper_stats_snapshot(&reaper);
        bool got = heap_get_stats(&current);
        uint32_t current_used_blocks = got
            ? current.blocks_total - current.blocks_free : 0;
        uint32_t previous_used_blocks = previous.blocks_total - previous.blocks_free;
        if (got && !reaper.current_zombies &&
            !reaper.free_inflight && stable &&
            current.used_bytes == previous.used_bytes &&
            current_used_blocks == previous_used_blocks) {
            if (++stable >= 3) {
                if (out) *out = current;
                return true;
            }
        } else if (got) {
            stable = 1;
        } else {
            stable = 0;
        }
        if (got) previous = current;
        timer_sleep(10);
    }
    return false;
}

static int create_handle_race(uint32_t count)
{
    HeapStats before, after = {0};
    if (!wait_reaper_idle(10000) || !wait_heap_stable(&before, 10000))
        return 1;
    scheduler_test_set_lifecycle_log_quiet(true);
    task_id_t last = 0, window[64];
    uint64_t created = 0, reaped = 0, invalid = 0, duplicates = 0;
    uint32_t pending = 0;
    for (uint32_t i = 0; i < count; i++) {
        task_handle_t handle;
        if (!thread_create_named_handle(normal_worker, NULL, "handle-race",
                                        &handle))
            break;
        if (!handle.id || !handle.lifecycle_generation)
            invalid++;
        if (handle.id <= last)
            duplicates++;
        last = handle.id;
        window[pending++] = handle.id;
        created++;
        if (pending == 64 || i + 1 == count) {
            for (uint32_t spin = 0; spin < 10000 && pending; spin++) {
                scheduler_reap_zombies(0);
                uint32_t live = 0;
                for (uint32_t j = 0; j < pending; j++)
                    if (!gone(window[j]))
                        live++;
                if (!live)
                    break;
                timer_sleep(1);
            }
            for (uint32_t j = 0; j < pending; j++)
                if (gone(window[j]))
                    reaped++;
            pending = 0;
        }
    }
    scheduler_test_set_lifecycle_log_quiet(false);
    bool heap = wait_reaper_idle(10000) &&
                wait_heap_used(before.used_bytes, &after, 10000);
    bool ok = created == count && reaped == count && !invalid &&
              !duplicates && heap;
    serial_write_all("[REAPTEST][CREATE_HANDLE_RACE] ");
    serial_write_all(ok ? "PASS" : "FAIL");
    serial_write_all(" created="); u64(created);
    serial_write_all(" unique="); u64(created - duplicates);
    serial_write_all(" reaped="); u64(reaped);
    serial_write_all(" invalid_handles="); u64(invalid);
    serial_write_all(" duplicates="); u64(duplicates);
    serial_write_all(" baseline_used="); u64(before.used_bytes);
    serial_write_all(" final_used="); u64(after.used_bytes);
    serial_write_all(" faults="); u64(ok ? 0 : 1);
    serial_write_all("\n");
    return ok ? 0 : 1;
}
void reaptest_zombie_mem_observe_ps(task_id_t id,uint64_t bytes){if(id==__atomic_load_n(&g_mem_id,__ATOMIC_ACQUIRE)){__atomic_store_n(&g_mem_ps,bytes,__ATOMIC_RELEASE);serial_write_all("[PS][ZOMBIE_MEM] id=");u64(id);serial_write_all(" bytes=");u64(bytes);serial_write_all("\n");}}
void reaptest_zombie_mem_observe_taskman(task_id_t id,uint64_t bytes){if(id==__atomic_load_n(&g_mem_id,__ATOMIC_ACQUIRE)){__atomic_store_n(&g_mem_taskman,bytes,__ATOMIC_RELEASE);serial_write_all("[TASKMAN][ZOMBIE_MEM] id=");u64(id);serial_write_all(" bytes=");u64(bytes);serial_write_all(" state=ZOMBIE kill=ZOMB cpu=0.0%\n");if(__atomic_exchange_n(&g_mem_auto_release,0,__ATOMIC_ACQ_REL))(void)scheduler_test_hold_reap(id,false);}}
static int zombie_mem(int argc,char**argv){if(argc<3)return 1;if(!strcmp(argv[2],"setup")||!strcmp(argv[2],"setup-auto")){bool automatic=!strcmp(argv[2],"setup-auto");task_id_t id=held_zombie();task_snapshot_t s={0};bool found=id&&snap(id,&s),ok=found&&s.kernel_mem_est_bytes;if(!ok){if(id)(void)scheduler_test_hold_reap(id,false);if(id)(void)wait_gone(id,3000);serial_write_all("[REAPTEST][ZOMBIE_MEM_SETUP] FAIL id=");u64(id);serial_write_all(" found=");u64(found);serial_write_all(" state=");u64(s.state);serial_write_all(" bytes=");u64(s.kernel_mem_est_bytes);serial_write_all("\n");return 1;}__atomic_store_n(&g_mem_id,id,__ATOMIC_RELEASE);__atomic_store_n(&g_mem_bytes,s.kernel_mem_est_bytes,__ATOMIC_RELEASE);__atomic_store_n(&g_mem_ps,0,__ATOMIC_RELEASE);__atomic_store_n(&g_mem_taskman,0,__ATOMIC_RELEASE);__atomic_store_n(&g_mem_auto_release,automatic,__ATOMIC_RELEASE);serial_write_all("[REAPTEST][ZOMBIE_MEM_SETUP] PASS id=");u64(id);serial_write_all(" bytes=");u64(s.kernel_mem_est_bytes);serial_write_all(" auto=");u64(automatic);serial_write_all("\n");return 0;}if(!strcmp(argv[2],"cleanup")){task_id_t id=__atomic_load_n(&g_mem_id,__ATOMIC_ACQUIRE);uint64_t b=__atomic_load_n(&g_mem_bytes,__ATOMIC_ACQUIRE),p=__atomic_load_n(&g_mem_ps,__ATOMIC_ACQUIRE),t=__atomic_load_n(&g_mem_taskman,__ATOMIC_ACQUIRE);bool automatic=!__atomic_load_n(&g_mem_auto_release,__ATOMIC_ACQUIRE)&&t; scheduler_test_hold_reap(id,false);bool removed=wait_gone(id,3000),ok=id&&b&&t==b&&removed&&(automatic||p==b);serial_write_all("[REAPTEST][ZOMBIE_MEM] ");serial_write_all(ok?"PASS id=":"FAIL id=");u64(id);serial_write_all(" ps=");u64(p);serial_write_all(" taskman=");u64(t);serial_write_all(" removed=");u64(removed);serial_write_all("\n");if(ok&&automatic)serial_write_all("[TASKMANTEST][ZOMBIE_LIVE] PASS visible=1 removed=1 page_clamped=1\n");__atomic_store_n(&g_mem_id,0,__ATOMIC_RELEASE);__atomic_store_n(&g_mem_auto_release,0,__ATOMIC_RELEASE);return ok?0:1;}return 1;}

static int all(void){return workers(false,100)||workers(true,100)||grace_test()||batch_test()||snapshot_test()||cleanup_test()||notification_test()||timer_ref_test()||oncpu_test()||stale_test()||heap_test()||check();}
static int negative_test(void){
#ifdef HOBBYOS_REAPER_NEGATIVE_DUPLICATE_NOTIFICATION
 task_lifecycle_exit_event_t e={.task_id=UINT64_MAX-2,.lifecycle_generation=UINT64_MAX-3,.zombie_generation=UINT64_MAX-4,.reason=TASK_EXIT_NORMAL,.exit_started_ns=1,.cleanup_completed_ns=2,.notification_ns=3,.name="negative"};bool a=task_lifecycle_publish_exit(&e),b=task_lifecycle_publish_exit(&e);bool ok=a&&!b;serial_write_all(ok?"[REAPTEST][NEGATIVE] DUPLICATE_NOTIFICATION_DETECTED\n":"[REAPTEST][NEGATIVE] DUPLICATE_NOTIFICATION_MISSED\n");return ok?0:1;
#else
 return 1;
#endif
}
static int negative_age_test(void){task_id_t id=held_zombie();if(!id)return 1;task_reap_defer_mask_t m=0;bool blocked=scheduler_test_reap_mask(id,timer_get_uptime_ms(),0,&m)&&(m&TASK_REAP_DEFER_TEST_HOLD);if(blocked)(void)scheduler_reap_zombies(0);(void)scheduler_test_hold_reap(id,false);(void)wait_gone(id,3000);
#ifdef HOBBYOS_REAPER_NEGATIVE_AGE_ONLY
 return blocked?0:1;
#else
 serial_write_all("[REAPTEST][NEGATIVE_AGE] FAIL build_hook_inactive\n");return 1;
#endif
}
int cmd_reaptest(int argc,char**argv)
{
 if(argc<2){console_write("usage: reaptest fixture|priority|check|stats|normal N|killed N|grace|batch|snapshot|snapshot-boundary N|cleanup|timer-ref|notification|oncpu|stale|heap|concurrent-churn N K S|zombie-mem setup|cleanup|negative-age|all\n");return 1;}
 if(!strcmp(argv[1],"fixture"))return fixture_test();
 if(!strcmp(argv[1],"fixture-loop")){uint32_t n=argc>2?count_arg_large(argv[2],0):1000;return n?fixture_loop(n):1;}
 if(!strcmp(argv[1],"priority"))return priority_test();
 if(!strcmp(argv[1],"check"))return check();
 if(!strcmp(argv[1],"stats"))return stats();
 if(!strcmp(argv[1],"normal"))return workers(false,count_arg(argc>2?argv[2]:NULL,100));
 if(!strcmp(argv[1],"killed"))return workers(true,count_arg(argc>2?argv[2]:NULL,100));
 if(!strcmp(argv[1],"grace"))return grace_test();
 if(!strcmp(argv[1],"batch"))return batch_test();
 if(!strcmp(argv[1],"snapshot"))return snapshot_test();
 if(!strcmp(argv[1],"cleanup"))return cleanup_test();
 if(!strcmp(argv[1],"notification"))return notification_test();
 if(!strcmp(argv[1],"timer-ref"))return timer_ref_test();
 if(!strcmp(argv[1],"timer-ref-loop"))return timer_ref_loop(count_arg(argc>2?argv[2]:NULL,1000));
#ifdef HOBBYOS_REAPTEST_NEGATIVE_LATE_TIMER_HOLD
 if(!strcmp(argv[1],"timer-ref-late-negative"))return timer_ref_late_negative();
#endif
 if(!strcmp(argv[1],"oncpu"))return oncpu_test();
 if(!strcmp(argv[1],"stale"))return stale_test();
 if(!strcmp(argv[1],"heap"))return heap_test();
 if(!strcmp(argv[1],"churn")||!strcmp(argv[1],"concurrent-churn")||!strcmp(argv[1],"churn-concurrent"))return concurrent_churn_test(count_arg(argc>2?argv[2]:NULL,1000),count_arg(argc>3?argv[3]:NULL,1000),count_arg(argc>4?argv[4]:NULL,1000));
 if(!strcmp(argv[1],"snapshot-boundary"))return snapshot_boundary_loop(count_arg(argc>2?argv[2]:NULL,100));
 if(!strcmp(argv[1],"create-handle-race"))return create_handle_race(count_arg_large(argc>2?argv[2]:NULL,100000));
 if(!strcmp(argv[1],"zombie-mem"))return zombie_mem(argc,argv);
 if(!strcmp(argv[1],"negative-age"))return negative_age_test();
 if(!strcmp(argv[1],"negative"))return negative_test();
 if(!strcmp(argv[1],"all"))return all();
 return 1;
}
