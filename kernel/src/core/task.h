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


typedef struct
{
    uint64_t rsp; 
    int id;             
    char name[32];      
    task_state_t state;
    uint64_t cr3;       
    void *stack_base;   
    
    
    struct list_head list; 
} task_t;



typedef struct
{
    struct list_head head;
} wait_queue_t;


static inline void wait_queue_init(wait_queue_t *wq) {
    list_init(&wq->head);
}

extern void switch_context(task_t *prev, task_t *next);

#endif