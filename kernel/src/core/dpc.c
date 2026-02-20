#include "dpc.h"
#include "queue.h"
#include "../core/spinlock.h"
#include "../memory/heap.h"
#include "../graphics/console.h"
#include "semaphore.h"
#include "scheduler.h"

typedef struct dpc_job
{
    struct list_head node;
    dpc_callback_t func;
    void *ctx;
} dpc_job_t;

static queue_head_t g_dpc_queue;
static spinlock_t g_dpc_lock;
static semaphore_t g_dpc_sem;

static void dpc_worker_thread(void *arg)
{
    (void)arg;

    // Evita "furar" a splash: só imprime quando o console não estiver com render suspenso.
    int printed_banner = 0;

    while (1)
    {
        // Imprime o banner uma única vez, assim que estiver seguro renderizar.
        if (!printed_banner && !console_is_render_suspended())
        {
            console_write_debug("[DPC] Worker thread started.\n");
            printed_banner = 1;
        }

        sem_wait(&g_dpc_sem);

        dpc_job_t *job = NULL;

        irq_flags_t flags = spin_lock_irqsave(&g_dpc_lock);
        if (!queue_empty(&g_dpc_queue))
        {
            struct list_head *node = queue_pop(&g_dpc_queue);
            job = list_entry(node, dpc_job_t, node);
        }
        spin_unlock_irqrestore(&g_dpc_lock, flags);

        if (job)
        {
            if (job->func)
            {
                job->func(job->ctx);
            }
            kfree(job);
        }
    }
}

void dpc_init(void)
{
    spinlock_init(&g_dpc_lock);
    queue_init(&g_dpc_queue);

    sem_init(&g_dpc_sem, 0);

    thread_create(dpc_worker_thread, NULL);

    console_write_debug("[CORE] DPC subsystem initialized (Worker Mode).\n");
}

int dpc_enqueue(dpc_callback_t func, void *ctx)
{
    if (!func)
        return -1;

    dpc_job_t *job = (dpc_job_t *)kmalloc(sizeof(dpc_job_t));
    if (!job)
    {
        return -1;
    }

    job->func = func;
    job->ctx = ctx;
    list_init(&job->node);

    irq_flags_t flags = spin_lock_irqsave(&g_dpc_lock);
    queue_push(&g_dpc_queue, &job->node);
    spin_unlock_irqrestore(&g_dpc_lock, flags);

    sem_signal(&g_dpc_sem);

    return 0;
}