#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>


void timer_init();



void timer_handler();


uint64_t timer_get_uptime_ms();

#endif