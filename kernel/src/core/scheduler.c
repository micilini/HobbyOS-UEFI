#include "scheduler.h"
#include "task.h"
#include "spinlock.h"
#include "../memory/heap.h"
#include "../graphics/console.h"
#include "../libc/string.h"
#include "../libc/memory.h"
#include "timers.h"

#define STACK_SIZE (16 * 1024)

extern void thread_wrapper(void);

static struct list_head g_ready_queue;
static task_t *g_current_task = NULL;
static int g_next_tid = 1;
static spinlock_t g_scheduler_lock;

void scheduler_init(void)
{
    spinlock_init(&g_scheduler_lock);
    list_init(&g_ready_queue);

    task_t *idle = (task_t *)kmalloc(sizeof(task_t));
    if (!idle)
        return;

    memset(idle, 0, sizeof(task_t));
    idle->id = 0;
    strcpy(idle->name, "Kernel_Main");
    idle->state = TASK_RUNNING;
    idle->stack_base = NULL;
    idle->rsp = 0;

    list_init(&idle->list);

    list_add_tail(&idle->list, &g_ready_queue);

    g_current_task = idle;

    console_write_debug("[SCHED] Scheduler initialized with Lock.\n");
}

task_t *get_current_task(void)
{
    return g_current_task;
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

    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);
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

    task_t *current = g_current_task;
    current->state = state;

    list_del(&current->list);

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
    if (!t)
        return;

    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);

    if (t->state == TASK_READY || t->state == TASK_RUNNING)
    {
        spin_unlock_irqrestore(&g_scheduler_lock, flags);
        return;
    }

    list_del(&t->list);

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

    irq_flags_t flags = spin_lock_irqsave(&g_scheduler_lock);

    while (list_empty(&g_ready_queue))
    {
        task_t *curr = g_current_task;

        if (curr->state == TASK_RUNNING)
        {
            break;
        }

        spin_unlock_irqrestore(&g_scheduler_lock, flags);

        timers_poll();

        __asm__ volatile("sti; hlt");

        flags = spin_lock_irqsave(&g_scheduler_lock);
    }

    task_t *prev = g_current_task;
    struct list_head *next_node;

    if (prev->state != TASK_RUNNING && prev->state != TASK_READY)
    {

        next_node = g_ready_queue.next;
    }
    else
    {

        next_node = prev->list.next;
    }

    if (next_node == &g_ready_queue)
    {
        next_node = next_node->next;
    }

    task_t *next = list_entry(next_node, task_t, list);

    if (next == prev)
    {
        spin_unlock_irqrestore(&g_scheduler_lock, flags);
        return;
    }

    if (prev->state == TASK_RUNNING)
    {
        prev->state = TASK_READY;
    }

    next->state = TASK_RUNNING;
    g_current_task = next;

    spin_unlock_irqrestore(&g_scheduler_lock, flags);

    switch_context(prev, next);
}