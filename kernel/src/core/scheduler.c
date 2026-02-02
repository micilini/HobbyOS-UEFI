#include "scheduler.h"
#include "task.h"
#include "../memory/heap.h"
#include "../graphics/console.h"
#include "../libc/string.h"
#include "../libc/memory.h"

#define STACK_SIZE (16 * 1024) 

extern void thread_wrapper(void);

static struct list_head g_ready_queue; 
static task_t *g_current_task = NULL;
static int g_next_tid = 1;

typedef struct {
    
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbp;
    uint64_t rbx;

    
    
    uint64_t rip;
    
    
    
} switch_stack_t;

void scheduler_init(void)
{
    list_init(&g_ready_queue);

    
    task_t *idle = (task_t *)kmalloc(sizeof(task_t));
    if (!idle) {
        
        return; 
    }

    memset(idle, 0, sizeof(task_t));
    idle->id = 0;
    strcpy(idle->name, "Kernel_Main");
    idle->state = TASK_RUNNING;
    
    
    idle->stack_base = NULL; 
    idle->rsp = 0; 

    list_init(&idle->list);
    list_add_tail(&idle->list, &g_ready_queue);

    g_current_task = idle;

    console_write_debug("[SCHED] Scheduler initialized. Main thread is ID 0.\n");
}

task_t *get_current_task(void)
{
    return g_current_task;
}

task_t *thread_create(void (*entry_point)(void *), void *arg)
{
    
    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    if (!t) return NULL;
    memset(t, 0, sizeof(task_t));

    
    t->stack_base = kmalloc_aligned(STACK_SIZE, 16);
    if (!t->stack_base) {
        kfree(t);
        return NULL;
    }
    memset(t->stack_base, 0, STACK_SIZE);

    
    t->id = g_next_tid++;
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
    list_add_tail(&t->list, &g_ready_queue);

    console_write_debug("[SCHED] Thread created. ID=");
    console_print_dec_debug(t->id);
    console_write_debug("\n");

    return t;
}

void thread_block(wait_queue_t *wq, task_state_t state)
{
    task_t *current = g_current_task;

    current->state = state;

    list_del(&current->list);

    if (wq) {
        list_add_tail(&current->list, &wq->head);
    } else {
        list_init(&current->list);
    }

    schedule();
}

void thread_wake(task_t *t)
{
    if (!t) return;

    if (t->state == TASK_READY || t->state == TASK_RUNNING) return;

    list_del(&t->list);

    t->state = TASK_READY;

    list_add_tail(&t->list, &g_ready_queue);
}

int thread_wake_one(wait_queue_t *wq)
{
    if (list_empty(&wq->head)) return 0;

    struct list_head *node = wq->head.next;
    task_t *t = list_entry(node, task_t, list);

    thread_wake(t);
    return 1;
}

void schedule(void)
{
    
    if (list_empty(&g_ready_queue)) return;

    task_t *prev = g_current_task;
    struct list_head *next_node;

    
    
    
    
    if (prev->state != TASK_RUNNING && prev->state != TASK_READY) {
        next_node = g_ready_queue.next;
    } 
    else {
        
        next_node = prev->list.next;
    }

    
    if (next_node == &g_ready_queue) {
        next_node = next_node->next;
    }

    task_t *next = list_entry(next_node, task_t, list);

    
    if (next == prev) return;

    
    
    if (prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
    }
    
    next->state = TASK_RUNNING;
    g_current_task = next;

    switch_context(prev, next);
}