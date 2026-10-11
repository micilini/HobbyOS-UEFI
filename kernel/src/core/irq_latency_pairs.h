#ifndef HOBBYOS_IRQ_LATENCY_PAIRS_H
#define HOBBYOS_IRQ_LATENCY_PAIRS_H
#include "irq_latency_time.h"

#define IRQ_LATENCY_PAIR_METHOD "cpu-local-reference-pairs"
#define IRQ_LATENCY_PAIR_VERSION 1u
#define IRQ_LATENCY_PAIR_TARGET_NS UINT64_C(20000000)
enum { IRQ_POINT_INELIGIBLE=1, IRQ_POINT_BEGIN, IRQ_POINT_END,
       IRQ_POINT_INTERIOR, IRQ_POINT_TAIL };
enum { IRQ_POINT_ACQUISITION=1, IRQ_POINT_CONTEXT=2, IRQ_POINT_IDENTITY=4,
       IRQ_POINT_COUNTER=8, IRQ_POINT_ORDER=16 };
typedef struct {
    const irq_latency_reference_point_t *point;
    uint32_t id, epoch, role, complete;
} irq_latency_point_ref_t;
typedef struct { uint32_t kind, reason, pair; } irq_latency_point_disposition_t;
typedef struct { uint32_t begin, end, cpu, valid; } irq_latency_pair_t;

/* Caller owns bounded scratch, all references, and completed immutable points.
 * A false result leaves count zero; scratch is never partial authority. */
bool irq_latency_pairs_build(const irq_latency_point_ref_t *refs, uint32_t count,
    uint32_t cpus, uint32_t epoch, uint32_t role, uint32_t *order,
    irq_latency_point_disposition_t *disposition, irq_latency_pair_t *pairs,
    uint32_t capacity, uint32_t *pair_count);
void irq_latency_pair_window(const irq_latency_point_ref_t *refs,
    const irq_latency_pair_t *pair, irq_latency_calibration_window_t *window);
#endif
