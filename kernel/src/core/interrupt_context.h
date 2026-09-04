#ifndef HOBBYOS_INTERRUPT_CONTEXT_H
#define HOBBYOS_INTERRUPT_CONTEXT_H

#include "../smp/smp_topology.h"

#include <stdbool.h>
#include <stdint.h>

#define INTERRUPT_CONTEXT_SNAPSHOT_ATTEMPTS 200000u

typedef struct
{
    uint64_t entered_total;
    uint64_t returned_total;
    uint64_t unexpected;
    uint64_t underflow;
    uint64_t mismatch;
    uint64_t imbalance;
    uint32_t first_vector;
    uint32_t last_vector;
    uint32_t depth;
    uint32_t max_depth;
    uint8_t preempt_epilogue;
    uint8_t initialized;
} interrupt_cpu_snapshot_t;

bool interrupt_context_init(uint32_t cpu_count);
void interrupt_context_enter(uint8_t vector);
void interrupt_context_exit(uint8_t vector);
bool interrupt_context_exit_to_preempt(uint8_t vector);
void interrupt_context_mark_unexpected(uint8_t vector);
bool interrupt_context_snapshot(cpu_slot_t slot,
                                interrupt_cpu_snapshot_t *out);
bool interrupt_context_snapshot_stable(cpu_slot_t slot,
                                       interrupt_cpu_snapshot_t *out,
                                       uint32_t attempts);
bool interrupt_context_vector_snapshot(cpu_slot_t slot, uint8_t vector,
                                       uint64_t *out_entered,
                                       uint64_t *out_returned);
bool interrupt_context_can_preempt(void);
bool interrupt_context_consume_preempt_epilogue(void);
bool interrupt_context_reset(void);
bool interrupt_context_selftest(void);

#endif
