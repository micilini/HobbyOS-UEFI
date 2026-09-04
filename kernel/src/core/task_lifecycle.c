#include "task_lifecycle.h"
#include "spinlock.h"
#include "../libc/memory.h"

#define TASK_LIFECYCLE_LISTENER_MAX 8u
#define TASK_LIFECYCLE_RECENT_MAX 256u

typedef struct {
    task_lifecycle_exit_listener_fn fn;
    void *ctx;
    uint64_t generation;
    uint32_t in_flight;
    uint8_t active, retiring;
} listener_slot_t;
typedef struct {
    task_lifecycle_exit_listener_fn fn;
    void *ctx;
    uint64_t generation;
    uint8_t slot;
} callback_copy_t;

static spinlock_t g_lock;
static listener_slot_t g_slots[TASK_LIFECYCLE_LISTENER_MAX];
static uint64_t g_recent[TASK_LIFECYCLE_RECENT_MAX], g_next_generation;
static uint32_t g_recent_next;
static task_lifecycle_stats_t g_stats;

static bool reason_valid(task_exit_reason_t r)
{
    return r == TASK_EXIT_NORMAL || r == TASK_EXIT_KILLED ||
           r == TASK_EXIT_INIT_FAILURE || r == TASK_EXIT_INTERNAL_ERROR;
}

void task_lifecycle_init(void)
{
    spinlock_init(&g_lock); memset(g_slots,0,sizeof(g_slots));
    memset(g_recent,0,sizeof(g_recent)); memset(&g_stats,0,sizeof(g_stats));
    g_recent_next=0; g_next_generation=1;
}

bool task_lifecycle_register_exit_listener(task_lifecycle_exit_listener_fn fn,void *ctx)
{
    if(!fn)return false;
    irq_flags_t f=spin_lock_irqsave(&g_lock);
    for(uint32_t i=0;i<TASK_LIFECYCLE_LISTENER_MAX;i++)
        if((g_slots[i].active||g_slots[i].retiring)&&g_slots[i].fn==fn&&g_slots[i].ctx==ctx){g_stats.registration_failures++;spin_unlock_irqrestore(&g_lock,f);return false;}
    for(uint32_t i=0;i<TASK_LIFECYCLE_LISTENER_MAX;i++)if(!g_slots[i].active&&!g_slots[i].retiring){
        if(!g_next_generation){g_stats.registration_failures++;spin_unlock_irqrestore(&g_lock,f);return false;}
        g_slots[i].fn=fn;g_slots[i].ctx=ctx;g_slots[i].generation=g_next_generation++;g_slots[i].active=1;
        g_stats.registered++;g_stats.listeners++;spin_unlock_irqrestore(&g_lock,f);return true;}
    g_stats.registration_failures++;spin_unlock_irqrestore(&g_lock,f);return false;
}

bool task_lifecycle_unregister_exit_listener(task_lifecycle_exit_listener_fn fn,void *ctx)
{
    irq_flags_t f=spin_lock_irqsave(&g_lock);
    for(uint32_t i=0;i<TASK_LIFECYCLE_LISTENER_MAX;i++)if((g_slots[i].active||g_slots[i].retiring)&&g_slots[i].fn==fn&&g_slots[i].ctx==ctx){
        if(g_slots[i].active){g_slots[i].active=0;g_slots[i].retiring=1;g_stats.listeners--;}
        if(g_slots[i].in_flight){g_stats.unregister_busy++;spin_unlock_irqrestore(&g_lock,f);return false;}
        memset(&g_slots[i],0,sizeof(g_slots[i]));g_stats.unregistered++;spin_unlock_irqrestore(&g_lock,f);return true;}
    spin_unlock_irqrestore(&g_lock,f);return false;
}

bool task_lifecycle_publish_exit(const task_lifecycle_exit_event_t *e)
{
    callback_copy_t copy[TASK_LIFECYCLE_LISTENER_MAX];uint32_t n=0;
    bool valid=e&&e->task_id&&e->lifecycle_generation&&e->zombie_generation&&reason_valid(e->reason)&&
        e->exit_started_ns&&e->cleanup_completed_ns>=e->exit_started_ns&&e->notification_ns>=e->cleanup_completed_ns&&
        e->name[sizeof(e->name)-1]==0;
    if(!valid){irq_flags_t f=spin_lock_irqsave(&g_lock);g_stats.invalid_events++;g_stats.publish_failures++;spin_unlock_irqrestore(&g_lock,f);return false;}
    irq_flags_t f=spin_lock_irqsave(&g_lock);
    for(uint32_t i=0;i<TASK_LIFECYCLE_RECENT_MAX;i++)if(g_recent[i]==e->zombie_generation){g_stats.duplicate_publish++;spin_unlock_irqrestore(&g_lock,f);return false;}
    g_recent[g_recent_next++%TASK_LIFECYCLE_RECENT_MAX]=e->zombie_generation;
    for(uint32_t i=0;i<TASK_LIFECYCLE_LISTENER_MAX;i++)if(g_slots[i].active){
        g_slots[i].in_flight++;g_stats.callbacks_in_flight++;
        copy[n++]=(callback_copy_t){g_slots[i].fn,g_slots[i].ctx,g_slots[i].generation,(uint8_t)i};}
    g_stats.published++;spin_unlock_irqrestore(&g_lock,f);
    for(uint32_t i=0;i<n;i++){
        copy[i].fn(e,copy[i].ctx);f=spin_lock_irqsave(&g_lock);listener_slot_t*s=&g_slots[copy[i].slot];
        if(s->generation!=copy[i].generation||!s->in_flight||!g_stats.callbacks_in_flight){g_stats.publish_failures++;spin_unlock_irqrestore(&g_lock,f);return false;}
        s->in_flight--;g_stats.callbacks_in_flight--;g_stats.callbacks_invoked++;spin_unlock_irqrestore(&g_lock,f);}
    return true;
}

void task_lifecycle_get_stats(task_lifecycle_stats_t*out)
{if(!out)return;irq_flags_t f=spin_lock_irqsave(&g_lock);memcpy(out,&g_stats,sizeof(*out));spin_unlock_irqrestore(&g_lock,f);}
