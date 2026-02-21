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

/* Classes de prioridade */
#define TASK_CLASS_INTERACTIVE  0   /* input thread, DPC, shell — baixa latência */
#define TASK_CLASS_NORMAL       1   /* tasks normais criadas pelo usuário */

#define DEFAULT_QUANTUM         20  /* ~20ms com tick de 1ms */
#define INTERACTIVE_QUANTUM     5   /* ~5ms — interativos rodam pouco mas rápido */

typedef struct
{
    uint64_t rsp;
    int id;
    char name[32];
    task_state_t state;
    uint64_t cr3;
    void *stack_base;

    int quantum;            /* ticks restantes neste time slice */
    int quantum_default;    /* valor de reset ao ser re-enfileirada */
    int task_class;         /* TASK_CLASS_INTERACTIVE ou TASK_CLASS_NORMAL */

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