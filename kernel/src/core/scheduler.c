#include "scheduler.h"
#include "task.h"
#include "spinlock.h"
#include "../memory/heap.h"
#include "../graphics/console.h"
#include "../libc/string.h"
#include "../libc/memory.h"
#include "timers.h"
#include "../apic/lapic.h" 

#define STACK_SIZE (16 * 1024)
#define MAX_CPUS 256

extern void thread_wrapper(void);


static struct list_head g_ready_queue;
static spinlock_t g_scheduler_lock;


static task_t *g_current_task_map[MAX_CPUS] = {0};


static task_t *g_idle_task_map[MAX_CPUS] = {0};

static int g_next_tid = 1;


static task_t* create_idle_task(void) {
    task_t *idle = (task_t *)kmalloc(sizeof(task_t));
    if (!idle) return NULL;
    
    memset(idle, 0, sizeof(task_t));
    idle->id = 0; 
    strcpy(idle->name, "Idle");
    idle->state = TASK_RUNNING;
    idle->stack_base = NULL; 
    idle->rsp = 0;
    
    list_init(&idle->list);
    return idle;
}


void scheduler_init(void)
{
    spinlock_init(&g_scheduler_lock);
    list_init(&g_ready_queue);
    
    
    memset(g_current_task_map, 0, sizeof(g_current_task_map));
    memset(g_idle_task_map, 0, sizeof(g_idle_task_map));

    
    uint32_t bsp_id = lapic_get_id();
    
    task_t *idle = create_idle_task();
    if (!idle) {
        kpanic("SCHED: Failed to create BSP Idle Task");
    }

    g_idle_task_map[bsp_id] = idle;
    g_current_task_map[bsp_id] = idle;

    console_write_debug("[SCHED] SMP Scheduler initialized (Global Queue).\n");
}


void scheduler_init_ap(void)
{
    uint32_t id = lapic_get_id();
    
    task_t *idle = create_idle_task();
    if (!idle) {
        
        while(1) __asm__ volatile("cli; hlt"); 
    }

    g_idle_task_map[id] = idle;
    g_current_task_map[id] = idle;
    
    
    
    
}

task_t *get_current_task(void)
{
    uint32_t id = lapic_get_id();
    return g_current_task_map[id];
}

task_t *thread_create(void (*entry_point)(void *), void *arg)
{
    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    if (!t)
        return NULL;
    memset(t, 0, sizeof(task_t));

    t->stack_base = kmalloc_aligned(STACK_SIZE, 16);
    if (!t->stack_base)
    {
        kfree(t);
        return NULL;
    }
    memset(t->stack_base, 0, STACK_SIZE);

    
    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);
    t->id = g_next_tid++;
    spin_unlock_irqrestore(&g_scheduler_lock, flags);

    t->state = TASK_READY;
    t->cr3 = 0;

    uint64_t *sp = (uint64_t *)((uint8_t *)t->stack_base + STACK_SIZE);

    sp--;
    *sp = (uint64_t)thread_wrapper;
    sp--;
    *sp = 0; 
    sp--;
    *sp = 0; 
    sp--;
    *sp = (uint64_t)entry_point; 
    sp--;
    *sp = (uint64_t)arg; 
    sp--;
    *sp = 0; 
    sp--;
    *sp = 0; 

    t->rsp = (uint64_t)sp;

    list_init(&t->list);

    
    flags = spin_lock_irqsave(&g_scheduler_lock);
    list_add_tail(&t->list, &g_ready_queue);
    spin_unlock_irqrestore(&g_scheduler_lock, flags);

    console_write_debug("[SCHED] Thread created. ID=");
    console_print_dec_debug(t->id);
    console_write_debug("\n");

    return t;
}

void thread_block(wait_queue_t *wq, task_state_t state)
{
    
    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);

    task_t *current = get_current_task();
    
    
    if (current == g_idle_task_map[lapic_get_id()]) {
        spin_unlock_irqrestore(&g_scheduler_lock, flags);
        return; 
    }

    current->state = state;

    
    if (current->list.next != NULL && current->list.prev != NULL) {
        list_del(&current->list);
    }

    if (wq)
    {
        list_add_tail(&current->list, &wq->head);
    }
    else
    {
        list_init(&current->list);
    }

    spin_unlock_irqrestore(&g_scheduler_lock, flags);

    schedule();
}

void thread_wake(task_t *t)
{
    if (!t) return;

    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);

    if (t->state == TASK_READY || t->state == TASK_RUNNING)
    {
        spin_unlock_irqrestore(&g_scheduler_lock, flags);
        return;
    }

    
    if (t->list.next != NULL && t->list.prev != NULL) {
        list_del(&t->list);
    }

    t->state = TASK_READY;
    list_add_tail(&t->list, &g_ready_queue);

    spin_unlock_irqrestore(&g_scheduler_lock, flags);
}

int thread_wake_one(wait_queue_t *wq)
{
    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);

    if (list_empty(&wq->head))
    {
        spin_unlock_irqrestore(&g_scheduler_lock, flags);
        return 0;
    }

    struct list_head *node = wq->head.next;
    task_t *t = list_entry(node, task_t, list);

    list_del(&t->list);
    t->state = TASK_READY;
    list_add_tail(&t->list, &g_ready_queue);

    spin_unlock_irqrestore(&g_scheduler_lock, flags);
    return 1;
}

void schedule(void)
{
    uint32_t my_id = lapic_get_id();
    task_t *prev = g_current_task_map[my_id];
    task_t *next = NULL;
    task_t *my_idle = g_idle_task_map[my_id];

    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);

    
    if (!list_empty(&g_ready_queue))
    {
        struct list_head *next_node = g_ready_queue.next;
        next = list_entry(next_node, task_t, list);
        list_del(&next->list); 
    }
    else
    {
        
        next = my_idle;
    }

    
    if (prev != next)
    {
        
        
        if (prev->state == TASK_RUNNING && prev != my_idle)
        {
            prev->state = TASK_READY;
            list_add_tail(&prev->list, &g_ready_queue);
        }
        else if (prev == my_idle)
        {
            
            
            prev->state = TASK_READY;
        }

        
        next->state = TASK_RUNNING;
        g_current_task_map[my_id] = next;
    }

    spin_unlock_irqrestore(&g_scheduler_lock, flags);

    
    if (prev != next)
    {
        switch_context(prev, next);
    }
}