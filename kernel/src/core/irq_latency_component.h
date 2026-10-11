#ifndef HOBBYOS_IRQ_LATENCY_COMPONENT_H
#define HOBBYOS_IRQ_LATENCY_COMPONENT_H
#include <stdbool.h>
#include <stdint.h>

/* A subinterval is diagnostic; it never replaces the outer IF=0 maximum. */
typedef struct {
    uint64_t count, invalid, maximum, begin, end, generation, root, context;
    uint64_t first_generation, last_generation, failures;
} irq_latency_component_t;

bool irq_latency_component_add(irq_latency_component_t *row, uint64_t begin,
    uint64_t end, uint64_t generation, uint64_t root, uint64_t context,
    bool valid, bool success);
#endif
