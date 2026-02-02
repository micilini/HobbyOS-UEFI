#ifndef SEMAPHORE_H
#define SEMAPHORE_H

#include <stdint.h>
#include "spinlock.h"
#include "task.h"

typedef struct
{
    volatile int count;
    wait_queue_t wait_queue;
    spinlock_t lock;
} semaphore_t;

void sem_init(semaphore_t *sem, int initial_count);

void sem_wait(semaphore_t *sem);

void sem_signal(semaphore_t *sem);

#endif