#ifndef DPC_H
#define DPC_H

#include <stdint.h>
#include <stdbool.h>

typedef struct
{
    uint8_t initialized;
    uint8_t worker_started;
} dpc_runtime_snapshot_t;

typedef void (*dpc_callback_t)(void *ctx);

void dpc_init(void);

int dpc_enqueue(dpc_callback_t func, void *ctx);
bool dpc_runtime_snapshot(dpc_runtime_snapshot_t *out);

#endif
