#ifndef HOBBYOS_IRQ_LATENCY_INTERIOR_H
#define HOBBYOS_IRQ_LATENCY_INTERIOR_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t tsc, flags, cpu, epoch, segment, switches, entries, faults;
    uint64_t owner, generation, depth, lock, queries, recorder_sequence;
} irq_latency_interior_point_t;
typedef struct {
    irq_latency_interior_point_t begin, end;
    uint64_t paths;
} irq_latency_interior_witness_t;
typedef struct {
    uint64_t count, invalid, maximum_cycles;
    irq_latency_interior_witness_t maximum;
} irq_latency_interior_record_t;
bool irq_latency_interior_valid(const irq_latency_interior_witness_t *w);
bool irq_latency_interior_minimum(uint64_t cycles, uint64_t pre_upper,
    uint64_t post_upper, uint64_t *frequency_upper, uint64_t *minimum_ns);
void irq_latency_interior_before_unlock(uintptr_t lock);
const irq_latency_interior_record_t *irq_latency_interior_read(uint32_t cpu, uint32_t path);
extern uint64_t g_irq_interior_fault_sequence, g_irq_interior_fault_overflow;

/* Separate instruction witness: no lock-body or enclosing-witness substitution. */
typedef struct { uint64_t begin, end, begin_flags, end_flags; } irq_latency_writeback_stamps_t;
typedef struct {
    irq_latency_interior_point_t begin, end;
    uint64_t request_generation, root;
} irq_latency_writeback_witness_t;
typedef struct {
    uint64_t count, invalid, maximum_cycles, first_generation, last_generation;
    irq_latency_writeback_witness_t first, maximum;
} irq_latency_writeback_record_t;
typedef struct {
    irq_latency_interior_point_t begin;
    uint64_t request_generation, root;
    bool active;
} irq_latency_writeback_token_t;
bool irq_latency_writeback_valid(const irq_latency_writeback_witness_t *w);
bool irq_latency_writeback_record(irq_latency_writeback_record_t *r,
    const irq_latency_writeback_witness_t *w);
irq_latency_writeback_token_t irq_latency_writeback_begin(uint64_t generation, uint64_t root);
irq_latency_writeback_witness_t irq_latency_writeback_points(irq_latency_writeback_token_t token,
    const irq_latency_writeback_stamps_t *stamps);
void irq_latency_writeback_end(irq_latency_writeback_token_t token,
    const irq_latency_writeback_stamps_t *stamps);
void irq_latency_writeback_execute(irq_latency_writeback_stamps_t *stamps);
const irq_latency_writeback_record_t *irq_latency_writeback_read(uint32_t cpu);

typedef struct {
    irq_latency_interior_point_t begin, end;
    uint64_t incoming_flags, site, paths, sequence, phase;
} irq_latency_clock_witness_t;
typedef struct {
    uint64_t count, invalid, maximum_cycles, started, retired, pending;
    irq_latency_clock_witness_t maximum;
} irq_latency_clock_record_t;
typedef struct {
    irq_latency_interior_point_t begin;
    uint64_t incoming_flags, site, paths, sequence, phase;
    bool active;
} irq_latency_clock_token_t;
bool irq_latency_clock_valid(const irq_latency_clock_witness_t *w);
irq_latency_clock_token_t irq_latency_clock_prepare(uint64_t flags, uint64_t site);
void irq_latency_clock_complete(irq_latency_clock_token_t token, uint64_t end);
const irq_latency_clock_record_t *irq_latency_clock_read(uint32_t cpu);

typedef struct {
    irq_latency_interior_point_t begin, end;
    uint64_t sample_begin, sample_end, after_clock, after_work, sequence, outer_sequence, paths;
    uint64_t counter_end, sample_ticks, sample_mode, sample_retries, sample_valid;
    uint64_t sample_source, source_epoch;
} irq_latency_tick_witness_t;
typedef struct {
    uint64_t begin, counter_end, end, ticks, outer_sequence;
    uint32_t mode, retries;
    uint32_t source;
    bool valid, claimed;
    uint64_t source_epoch;
} __attribute__((aligned(64))) irq_latency_tick_sample_t;
typedef struct {
    uint64_t count, invalid, started, retired, pending, without_memory, maximum_cycles;
    irq_latency_tick_witness_t maximum;
} irq_latency_tick_record_t;
typedef struct {
    irq_latency_interior_point_t begin;
    uint64_t sequence, outer_sequence, after_clock, after_work;
    bool active;
} irq_latency_tick_token_t;
bool irq_latency_tick_valid(const irq_latency_tick_witness_t *w);
irq_latency_tick_token_t irq_latency_tick_prepare(void);
irq_latency_tick_sample_t *irq_latency_tick_clock_window(uint32_t cpu);
void irq_latency_tick_complete(const irq_latency_tick_token_t *token, uint64_t end);
const irq_latency_tick_record_t *irq_latency_tick_read(uint32_t cpu);
#endif
