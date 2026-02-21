#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "list.h"

typedef enum
{
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_SLEEPING,
    TASK_ZOMBIE
} task_state_t;

#define TASK_CLASS_INTERACTIVE 0
#define TASK_CLASS_NORMAL 1

#define DEFAULT_QUANTUM 20
#define INTERACTIVE_QUANTUM 5

typedef struct
{
    uint64_t rsp;
    int id;
    char name[32];
    task_state_t state;
    uint64_t cr3;
    void *stack_base;

    int quantum;
    int quantum_default;
    int task_class;

    struct list_head list;
} task_t;

typedef struct
{
    struct list_head head;
} wait_queue_t;

static inline void wait_queue_init(wait_queue_t *wq)
{
    list_init(&wq->head);
}

extern void switch_context(task_t *prev, task_t *next);

#endif