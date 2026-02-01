#ifndef CORE_TIMERS_H
#define CORE_TIMERS_H

#include <stdint.h>
#include "list.h"


typedef void (*timer_callback_t)(void *ctx);


typedef struct timer_node
{
    uint64_t deadline_ms;       
    timer_callback_t callback;  
    void *ctx;                  
    struct list_head node;      
} timer_node_t;


void timers_init(void);



int timers_add(uint64_t delay_ms, timer_callback_t cb, void *ctx);



void timers_poll(void);

#endif