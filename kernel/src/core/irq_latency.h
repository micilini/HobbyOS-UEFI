#ifndef HOBBYOS_IRQ_LATENCY_H
#define HOBBYOS_IRQ_LATENCY_H

#include <stdint.h>
#include "irq_latency_scope.h"
#include "irq_latency_time.h"
#include "irq_latency_record.h"
#include "irq_latency_component.h"

enum {
    IRQ_PATH_HEAP, IRQ_PATH_PMM, IRQ_PATH_PMM_INTEGRITY, IRQ_PATH_PMM_REACHABILITY,
    IRQ_PATH_DMA, IRQ_PATH_MAPPING, IRQ_PATH_UNMAP, IRQ_PATH_STACK,
    IRQ_PATH_SHOOTDOWN, IRQ_PATH_CACHE_WRITEBACK, IRQ_PATH_ICR
};

#if defined(HOBBYOS_IRQ_LATENCY) && HOBBYOS_IRQ_LATENCY
bool irq_latency_activate(const irq_latency_timebase_t *timebases, uint32_t count);
bool irq_latency_identity_prepare(void);
bool irq_latency_identity_sample(uint32_t *cpu, uint64_t *switch_sequence);
bool irq_latency_mmio_identity(uint32_t *architectural_id);
uint64_t irq_latency_request_epoch(uint32_t cpu);
irq_latency_scope_token_t irq_latency_scope_enter(uint64_t paths);
bool irq_latency_memory_scope_active(void);
void irq_latency_scope_exit(irq_latency_scope_token_t *token);
void irq_latency_stack_retired(uint32_t slot, uint64_t generation);
typedef struct {
    uint64_t sequence, switches, entries, epoch;
    uint32_t cpu, depth;
    bool active;
} irq_latency_lock_token_t;
irq_latency_lock_token_t irq_latency_lock_wait(uintptr_t lock);
void irq_latency_lock_acquired(uintptr_t lock, irq_latency_lock_token_t token);
void irq_latency_lock_ticket(uintptr_t lock, uint16_t distance);
void irq_latency_lock_released(uintptr_t lock);
void irq_latency_test_probe_role(void);
typedef struct {
    uint64_t outer_begin, inner_begin, inner_end, outer_end;
    uint64_t entry_checkpoint, exit_checkpoint;
    uint64_t switch_before, switch_after, sequence_before, sequence_after;
    uint64_t irq_before, irq_after;
    uint32_t cpu, end_cpu;
    bool valid;
} irq_latency_irq_probe_t;
bool irq_latency_irq_probe(irq_latency_irq_probe_t *out);
bool irq_latency_set_irq_overhead(uint32_t cpu, uint64_t cycles);
bool irq_latency_freeze_request(void);
bool irq_latency_freeze_diagnostic(uint32_t cpu, uint64_t out[14]);
uint64_t irq_latency_invalid_site(unsigned site);
bool irq_latency_owner_failure(uint32_t cpu, uint64_t out[4]);
typedef struct {
    uint64_t begin, switches, generation, root, context;
    uint32_t cpu, operation;
    bool active;
} irq_latency_callback_token_t;
irq_latency_callback_token_t irq_latency_callback_begin(uint32_t operation, uint64_t generation, uint64_t root);
void irq_latency_callback_end(irq_latency_callback_token_t token, bool success);
void irq_latency_request_result(uint32_t operation, uint64_t generation, uint64_t root,
    uint64_t begin_ns, uint64_t end_ns, bool success);
bool irq_latency_components(uint32_t cpu, uint32_t operation,
    irq_latency_component_t *callback, irq_latency_component_t *request);
bool irq_latency_terminal_read(uint32_t cpu, const irq_latency_snapshot_t **snapshot,
    const irq_latency_witness_t **failures, uint64_t *cutoff, uint64_t *snapshot_cycles);
void irq_latency_boundary_counts(uint64_t *early, uint64_t *invalid);
bool irq_latency_nested_elisions(uint32_t cpu, uint64_t *out);
bool irq_latency_nested_prechecks(uint32_t cpu, uint64_t *out);
bool irq_latency_mmio_reuses(uint32_t cpu, uint64_t *out);
#define IRQ_LATENCY_SCOPE_MASK(paths) \
    irq_latency_scope_token_t irq_latency_scope_token \
        __attribute__((cleanup(irq_latency_scope_exit))) = \
        irq_latency_scope_enter(paths)
#define IRQ_LATENCY_SCOPE(path) IRQ_LATENCY_SCOPE_MASK(UINT64_C(1) << (path))
#else
#define IRQ_LATENCY_SCOPE(path) do { } while (0)
#define IRQ_LATENCY_SCOPE_MASK(paths) do { } while (0)
#endif

#endif
