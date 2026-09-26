#pragma once
#include <stddef.h>
#include <stdint.h>
#include "panic.h"

struct list_head
{
    struct list_head *next;
    struct list_head *prev;
};

#define LIST_HEAD_INIT(name) \
    {                        \
        &(name), &(name)}

#define LIST_HEAD(name) \
    struct list_head name = LIST_HEAD_INIT(name)

static inline void list_init(struct list_head *head)
{
    head->next = head;
    head->prev = head;
}

static inline int list_empty(const struct list_head *head)
{
    return head->next == head;
}

static inline void __list_add(struct list_head *new_node,
                              struct list_head *prev,
                              struct list_head *next)
{
    KBUG_ON(new_node == NULL);
    KBUG_ON(prev == NULL);
    KBUG_ON(next == NULL);
    KBUG_ON(new_node == prev || new_node == next);
    KBUG_ON(!((new_node->next == new_node && new_node->prev == new_node) ||
              (new_node->next == NULL && new_node->prev == NULL)));
    KBUG_ON(prev->next != next);
    KBUG_ON(next->prev != prev);

    next->prev = new_node;
    new_node->next = next;
    new_node->prev = prev;
    prev->next = new_node;
}

static inline void list_add(struct list_head *new_node, struct list_head *head)
{
    KBUG_ON(new_node == NULL);
    KBUG_ON(head == NULL);
    __list_add(new_node, head, head->next);
}

static inline void list_add_tail(struct list_head *new_node, struct list_head *head)
{
    KBUG_ON(new_node == NULL);
    KBUG_ON(head == NULL);
    __list_add(new_node, head->prev, head);
}

static inline void __list_del(struct list_head *prev, struct list_head *next)
{
    next->prev = prev;
    prev->next = next;
}

static inline void list_del(struct list_head *entry)
{
    KBUG_ON(entry == NULL);
    struct list_head *prev = entry->prev;
    struct list_head *next = entry->next;
    KBUG_ON(prev == NULL || next == NULL);
    KBUG_ON(prev->next != entry);
    KBUG_ON(next->prev != entry);

    __list_del(prev, next);
    entry->next = NULL;
    entry->prev = NULL;
}

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define list_entry(ptr, type, member) \
    container_of(ptr, type, member)

#define list_first_entry(head, type, member) \
    list_entry((head)->next, type, member)

#define list_for_each(pos, head) \
    for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)

#define list_for_each_safe(pos, n, head)                           \
    for ((pos) = (head)->next, (n) = (pos)->next; (pos) != (head); \
         (pos) = (n), (n) = (pos)->next)
