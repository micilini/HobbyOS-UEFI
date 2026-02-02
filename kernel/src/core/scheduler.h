#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "task.h"

void scheduler_init(void);

task_t *thread_create(void (*entry_point)(void *), void *arg);

void schedule(void);

task_t *get_current_task(void);

#endif