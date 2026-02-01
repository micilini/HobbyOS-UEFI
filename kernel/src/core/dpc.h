#ifndef DPC_H
#define DPC_H

#include <stdint.h>


typedef void (*dpc_callback_t)(void *ctx);


void dpc_init(void);



int dpc_enqueue(dpc_callback_t func, void *ctx);



void dpc_run(void);

#endif