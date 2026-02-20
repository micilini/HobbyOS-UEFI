#include "timers.h"
#include "spinlock.h"
#include "../drivers/timer.h"
#include "../memory/heap.h"
#include "../graphics/console.h"

static struct list_head g_timer_list;
static spinlock_t g_timer_lock;

void timers_init(void)
{
    spinlock_init(&g_timer_lock);
    list_init(&g_timer_list);
    console_write_debug("[CORE] Timers subsystem initialized.\n");
}

int timers_add(uint64_t delay_ms, timer_callback_t cb, void *ctx)
{

    timer_node_t *t = (timer_node_t *)kmalloc(sizeof(timer_node_t));
    if (!t)
    {
        console_write_debug("[CORE] Failed to allocate timer!\n");
        return -1;
    }

    uint64_t now = timer_get_uptime_ms();
    t->deadline_ms = now + delay_ms;
    t->callback = cb;
    t->ctx = ctx;

    list_init(&t->node);

    irq_flags_t flags = spin_lock_irqsave(&g_timer_lock);

    list_add_tail(&t->node, &g_timer_list);

    spin_unlock_irqrestore(&g_timer_lock, flags);

    return 0;
}

void timers_poll(void)
{
    // Apenas o BSP processa a expiração de timers globais
    // para evitar contenção excessiva de lock em cada tick.
    if (lapic_get_id() != 0) return;

    if (list_empty(&g_timer_list))
        return;

    uint64_t now = timer_get_uptime_ms();

    struct list_head expired_list;
    list_init(&expired_list);

    irq_flags_t flags = spin_lock_irqsave(&g_timer_lock);

    struct list_head *pos, *n;
    list_for_each_safe(pos, n, &g_timer_list)
    {
        timer_node_t *t = list_entry(pos, timer_node_t, node);

        if (now >= t->deadline_ms)
        {

            list_del(&t->node);
            list_add_tail(&t->node, &expired_list);
        }
    }

    spin_unlock_irqrestore(&g_timer_lock, flags);

    list_for_each_safe(pos, n, &expired_list)
    {
        timer_node_t *t = list_entry(pos, timer_node_t, node);

        if (t->callback)
        {
            t->callback(t->ctx);
        }

        kfree(t);
    }
}