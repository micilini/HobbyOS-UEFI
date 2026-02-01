#pragma once
#include <stdint.h>
#include "list.h"

typedef struct queue_head
{
    struct list_head head;
} queue_head_t;

static inline void queue_init(queue_head_t *q)
{
    list_init(&q->head);
}

static inline int queue_empty(queue_head_t *q)
{
    return list_empty(&q->head);
}

// Enfileira no fim (FIFO)
static inline void queue_push(queue_head_t *q, struct list_head *node)
{
    list_add_tail(node, &q->head);
}

// Remove do começo (FIFO). Retorna NULL se vazio.
static inline struct list_head *queue_pop(queue_head_t *q)
{
    if (queue_empty(q))
        return NULL;

    struct list_head *first = q->head.next;
    list_del(first);
    return first;
}
