#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "task.h"

void scheduler_init(void);

task_t *thread_create(void (*entry_point)(void *), void *arg);

void schedule(void);

task_t *get_current_task(void);

void thread_block(wait_queue_t *wq, task_state_t state);

int thread_wake_one(wait_queue_t *wq);

void thread_wake(task_t *t);

#endif