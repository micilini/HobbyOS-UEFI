#ifndef HOBBYOS_IRQ_LATENCY_BOUNDARY_H
#define HOBBYOS_IRQ_LATENCY_BOUNDARY_H

#include <stdbool.h>
#include <stdint.h>
#include "irq_latency_record.h"
#include "irq_latency_scope.h"
#include "irq_latency_time.h"

typedef struct {
    uint64_t flags, site, stack_pointer, tsc, cpu, switch_sequence;
    uint64_t end_cpu, validity, ticket_sequence, ticket_slot, observed_flags, ready;
} irq_latency_boundary_t;

uint64_t irq_latency_save(void);
void irq_latency_restore(uint64_t flags);
void irq_latency_enable(void);
void irq_latency_disable(void);
bool irq_latency_before_mask(irq_latency_boundary_t *boundary);
void irq_latency_after_mask(irq_latency_boundary_t *boundary);
bool irq_latency_before_restore(irq_latency_boundary_t *boundary);
void irq_latency_after_restore(irq_latency_boundary_t *boundary);
void irq_latency_context_switch(void);
uint64_t irq_latency_interrupt_enter(uint64_t start, uint64_t vector,
                                  uint64_t saved_flags, uint64_t interrupted_rsp);
uint64_t irq_latency_interrupt_enter_checkpoint(uint64_t start, uint64_t vector,
    uint64_t saved_flags, uint64_t interrupted_rsp, uint64_t early);
enum { IRQ_ENTRY_ADMITTED = 1u, IRQ_ENTRY_SCOPED = 2u };
typedef struct { irq_latency_pending_t *pending; uint64_t *probe_end; } irq_latency_tail_t;
irq_latency_tail_t irq_latency_interrupt_exit(uint64_t vector, uint64_t saved_flags, uint64_t entry);

#endif
