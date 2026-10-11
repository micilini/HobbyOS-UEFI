#ifndef HOBBYOS_IRQ_LATENCY_RECORD_H
#define HOBBYOS_IRQ_LATENCY_RECORD_H

#include <stdbool.h>
#include <stdint.h>

#ifndef HOBBYOS_IRQ_LATENCY_DETAILS
#define HOBBYOS_IRQ_LATENCY_DETAILS 1
#endif
#if HOBBYOS_IRQ_LATENCY_DETAILS != 0 && HOBBYOS_IRQ_LATENCY_DETAILS != 1
#error HOBBYOS_IRQ_LATENCY_DETAILS must be 0 or 1
#endif

#define IRQ_LATENCY_PATHS 11u
#define IRQ_LATENCY_PENDING 64u
#define IRQ_LATENCY_FAILURES 2048u
#define IRQ_LATENCY_IF UINT64_C(512)
#define IRQ_LATENCY_ALL_PATHS ((UINT64_C(1) << IRQ_LATENCY_PATHS) - 1)

typedef enum {
    IRQ_LATENCY_THREAD = 1, IRQ_LATENCY_HARD_IRQ, IRQ_LATENCY_EARLY,
    IRQ_LATENCY_TEST_PROBE, IRQ_LATENCY_PANIC, IRQ_LATENCY_UNTRACKED
} irq_latency_context_t;

typedef enum {
    IRQ_LATENCY_VALID, IRQ_LATENCY_CPU_CHANGED, IRQ_LATENCY_CLOCK_ORDER,
    IRQ_LATENCY_MISSING_BEGIN, IRQ_LATENCY_BOUNDARY_UNCERTAIN, IRQ_LATENCY_OVERFLOW
} irq_latency_validity_t;

typedef enum {
    IRQ_BOUNDARY_DIRECT, IRQ_BOUNDARY_IRQ_ADJACENT,
    IRQ_BOUNDARY_AFTER_IRQ, IRQ_BOUNDARY_BEFORE_IRQ, IRQ_BOUNDARY_RETIRED_PUBLISHER
} irq_latency_boundary_kind_t;

/* Scalar cells permit one-attempt snapshots without a C data race with the
 * local writer. The pending completion writes only END/END_CPU/VALIDITY/READY. */
enum {
    IL_BEGIN, IL_END, IL_SEQUENCE, IL_END_SEQUENCE, IL_PATHS, IL_BEGIN_SITE,
    IL_END_SITE, IL_CONTEXT, IL_CPU, IL_END_CPU, IL_EPOCH, IL_VALIDITY,
    IL_WAIT, IL_HELD, IL_BOUNDARY_ADJUSTMENT, IL_OBSERVED_BEGIN, IL_OBSERVED_BEGIN_CPU,
    IL_OBSERVED_END, IL_OBSERVED_END_CPU, IL_BEGIN_KIND, IL_END_KIND,
    IL_FALLBACK_END, IL_FALLBACK_PRESENT, IL_OWNER_SLOT, IL_OWNER_GENERATION,
    IL_CLOSE_ADJUSTMENT, IL_RETIRE_OWNER_SLOT, IL_RETIRE_OWNER_GENERATION,
    IL_RETIRE_SEQUENCE, IL_FIELDS
};
typedef struct { uint64_t cells[IL_FIELDS]; } irq_latency_witness_t;
typedef struct {
    uint64_t count, invalid, violations, maximum_cycles;
    irq_latency_witness_t maximum;
} irq_latency_path_record_t;
typedef struct {
    uint64_t observations, contended, maximum_distance, lock, begin_sequence, paths;
} irq_latency_contention_t;
typedef struct {
    uint64_t ready;
    irq_latency_witness_t witness;
} irq_latency_pending_t;
typedef struct {
    uint64_t sequence, busy, recursion, epoch, cpu, next_sequence, cycle_limit;
    uint64_t active, paths, begin, begin_site, context, begin_sequence;
    uint64_t observed_begin, observed_begin_cpu, begin_kind;
    uint64_t wait_cycles, held_cycles, pending_count, invalid, exhausted;
    uint64_t missing_begin, unclosed, early, probes, failure_count, failure_overflow;
    irq_latency_pending_t pending[IRQ_LATENCY_PENDING];
    irq_latency_path_record_t path[IRQ_LATENCY_PATHS];
    irq_latency_path_record_t excluded[3];
    irq_latency_contention_t contention[IRQ_LATENCY_PATHS];
    irq_latency_witness_t failures[IRQ_LATENCY_FAILURES];
} irq_latency_recorder_t;
typedef struct { uint64_t sequence; uint32_t slot; bool valid; } irq_latency_ticket_t;
typedef struct {
    uint64_t epoch, cpu, sequence, active, pending, invalid, exhausted, recursion;
    uint64_t missing_begin, unclosed, early, probes, failure_count, failure_overflow;
    irq_latency_path_record_t path[IRQ_LATENCY_PATHS];
    irq_latency_path_record_t excluded[3];
    irq_latency_contention_t contention[IRQ_LATENCY_PATHS];
} irq_latency_snapshot_t;

bool irq_latency_record_init(irq_latency_recorder_t *r, uint32_t cpu,
                             uint64_t epoch, uint64_t cycle_limit);
bool irq_latency_record_begin(irq_latency_recorder_t *r, uint64_t entry_flags,
    uint64_t start, uint64_t site, uint64_t paths, irq_latency_context_t context);
bool irq_latency_record_begin_envelope(irq_latency_recorder_t *r, uint64_t entry_flags,
    uint64_t start, uint64_t site, uint64_t paths, irq_latency_context_t context,
    uint64_t observed_start, uint32_t observed_cpu, irq_latency_boundary_kind_t kind);
bool irq_latency_record_tag(irq_latency_recorder_t *r, uint64_t paths);
bool irq_latency_record_components(irq_latency_recorder_t *r, uint64_t wait, uint64_t held);
bool irq_latency_record_ticket(irq_latency_recorder_t *r, uintptr_t lock, uint16_t distance);
irq_latency_ticket_t irq_latency_record_close(irq_latency_recorder_t *r,
                                              uint64_t restore_flags, uint64_t site);
irq_latency_ticket_t irq_latency_record_close_owned(irq_latency_recorder_t *r,
    uint64_t restore_flags, uint64_t site, uint32_t owner_slot, uint64_t owner_generation);
irq_latency_ticket_t irq_latency_record_close_calibrated(irq_latency_recorder_t *r,
    uint64_t restore_flags, uint64_t site, uint32_t owner_slot, uint64_t owner_generation,
    uint64_t adjustment);
uint32_t irq_latency_record_retire_owner(irq_latency_recorder_t *r,
    uint32_t owner_slot, uint64_t owner_generation);
bool irq_latency_record_complete(irq_latency_recorder_t *r, irq_latency_ticket_t ticket,
    uint64_t end, uint32_t end_cpu, irq_latency_validity_t validity);
bool irq_latency_record_complete_adjusted(irq_latency_recorder_t *r, irq_latency_ticket_t ticket,
    uint64_t end, uint32_t end_cpu, irq_latency_validity_t validity, uint64_t adjustment);
bool irq_latency_record_complete_envelope(irq_latency_recorder_t *r, irq_latency_ticket_t ticket,
    uint64_t end, uint32_t end_cpu, irq_latency_validity_t validity, uint64_t adjustment,
    irq_latency_boundary_kind_t kind);
bool irq_latency_record_interrupt(irq_latency_recorder_t *r, uint64_t entry_tsc, uint32_t cpu);
irq_latency_pending_t *irq_latency_record_prepare_tail(irq_latency_recorder_t *r,
    irq_latency_ticket_t ticket, uint32_t cpu, uint64_t adjustment);
bool irq_latency_record_drain(irq_latency_recorder_t *r);
bool irq_latency_record_snapshot(const irq_latency_recorder_t *r, irq_latency_snapshot_t *out);

#endif
