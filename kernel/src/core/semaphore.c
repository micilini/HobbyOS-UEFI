#include "semaphore.h"
#include "scheduler.h" 
#include "../graphics/console.h"

void sem_init(semaphore_t *sem, int initial_count)
{
    sem->count = initial_count;
    wait_queue_init(&sem->wait_queue);
    spinlock_init(&sem->lock);
}

void sem_wait(semaphore_t *sem)
{
    while (1)
    {
        irq_flags_t flags = spin_lock_irqsave(&sem->lock);

        if (sem->count > 0)
        {
            
            sem->count--;
            spin_unlock_irqrestore(&sem->lock, flags);
            return;
        }


        task_t *current = get_current_task();
        
        
        spin_unlock_irqrestore(&sem->lock, flags);
        
        thread_block(&sem->wait_queue, TASK_BLOCKED);
    }
}

void sem_signal(semaphore_t *sem)
{
    irq_flags_t flags = spin_lock_irqsave(&sem->lock);

    if (thread_wake_one(&sem->wait_queue))
    {
        
        sem->count++;
    }
    else
    {
        sem->count++;
    }

    spin_unlock_irqrestore(&sem->lock, flags);
}