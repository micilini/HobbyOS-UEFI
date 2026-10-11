#ifndef HOBBYOS_IRQ_LATENCY_SCOPE_H
#define HOBBYOS_IRQ_LATENCY_SCOPE_H

#include <stdbool.h>
#include <stdint.h>

#define IRQ_LATENCY_SCOPE_DEPTH 32u

typedef struct {
    uint64_t owner_generation, paths, depth, invalid;
} irq_latency_scope_state_t;
typedef struct {
    uint64_t owner_generation, previous_paths, depth;
    uint32_t stack_slot;
    bool active;
} irq_latency_scope_token_t;

bool irq_latency_scope_push(irq_latency_scope_state_t *state, uint32_t stack_slot,
    uint64_t owner_generation, uint64_t paths, irq_latency_scope_token_t *token);
bool irq_latency_scope_pop(irq_latency_scope_state_t *state, uint32_t stack_slot,
    uint64_t owner_generation, irq_latency_scope_token_t *token);
bool irq_latency_scope_retired(irq_latency_scope_state_t *state, uint64_t owner_generation);
bool irq_latency_scope_retire_interrupts(irq_latency_scope_state_t *state, uint32_t slot,
    uint64_t generation, irq_latency_scope_token_t *barriers, uint32_t count);
bool irq_latency_scope_interrupt(irq_latency_scope_state_t *state, uint32_t stack_slot,
    uint64_t owner_generation, irq_latency_scope_token_t *token);

#endif
