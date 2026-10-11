#ifndef HOBBYOS_IRQ_LATENCY_HEAP_H
#define HOBBYOS_IRQ_LATENCY_HEAP_H
#include "irq_latency_interior.h"
#define IRQ_HEAP_OPERATIONS 7u
#define IRQ_HEAP_MARKS 8u
enum { IRQ_HEAP_ALLOCATE=1, IRQ_HEAP_RECHECK, IRQ_HEAP_FREE, IRQ_HEAP_STATS,
    IRQ_HEAP_INTEGRITY, IRQ_HEAP_INITIALIZE, IRQ_HEAP_INITIAL_PUBLISH };
enum { IRQ_HEAP_REGION=1, IRQ_HEAP_FIRST_FIT, IRQ_HEAP_CONTAINMENT,
    IRQ_HEAP_POINTER, IRQ_HEAP_COALESCE, IRQ_HEAP_METADATA, IRQ_HEAP_PUBLISH,
    IRQ_HEAP_STATISTICS, IRQ_HEAP_VALIDATE, IRQ_HEAP_INIT };
typedef struct { uint64_t tsc, site, work, bytes; } irq_latency_heap_mark_t;
typedef struct {
    irq_latency_interior_witness_t body;
    uint64_t operation, sequence, marks, wait_start, acquired;
    uint64_t pending_begin, pending_end, requested_pages;
    irq_latency_heap_mark_t mark[IRQ_HEAP_MARKS];
} irq_latency_heap_event_t;
typedef struct {
    uint64_t count, invalid, last_sequence, maximum_cycles, metadata_count;
    irq_latency_heap_event_t maximum;
} irq_latency_heap_record_t;
bool irq_latency_heap_valid(const irq_latency_heap_event_t *e);
bool irq_latency_heap_record(irq_latency_heap_record_t *r, const irq_latency_heap_event_t *e);
void irq_latency_heap_begin(uintptr_t lock, unsigned operation, uint64_t pages);
void irq_latency_heap_mark(uintptr_t lock, unsigned site, uint64_t work, uint64_t bytes);
void irq_latency_heap_end(uintptr_t lock);
const irq_latency_heap_record_t *irq_latency_heap_read(unsigned cpu, unsigned operation);

/* Call-site clocks; context is prepared before and validated after the operation. */
typedef struct {
    irq_latency_interior_point_t context;
    uint64_t sequence;
    bool active;
} irq_latency_heap_operation_token_t;
typedef struct {
    irq_latency_interior_witness_t interval, parent;
    uint64_t sequence, operation, prepared, validated, previous_mark, next_mark, header_bytes;
} irq_latency_heap_operation_t;
typedef struct {
    uint64_t count, invalid, last_sequence, maximum_cycles;
    irq_latency_heap_operation_t maximum;
} irq_latency_heap_operation_record_t;
bool irq_latency_heap_operation_valid(const irq_latency_heap_operation_t *w);
bool irq_latency_heap_operation_record(irq_latency_heap_operation_record_t *r,
    const irq_latency_heap_operation_t *w);
irq_latency_heap_operation_token_t irq_latency_heap_operation_prepare(uintptr_t lock);
void irq_latency_heap_operation_publish(const irq_latency_heap_operation_token_t *token,
    uint64_t begin, uint64_t end, uint64_t header_bytes);
const irq_latency_heap_operation_record_t *irq_latency_heap_operation_read(unsigned cpu);
#endif
