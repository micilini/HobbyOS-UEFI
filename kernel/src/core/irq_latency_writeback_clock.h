#ifndef HOBBYOS_IRQ_LATENCY_WRITEBACK_CLOCK_H
#define HOBBYOS_IRQ_LATENCY_WRITEBACK_CLOCK_H
#include "irq_latency_interior.h"
#include "../timer/hpet.h"

enum { IRQ_WRITEBACK_CLOCK_BEFORE, IRQ_WRITEBACK_CLOCK_AFTER, IRQ_WRITEBACK_CLOCK_HANDLER,
       IRQ_WRITEBACK_CLOCK_SITES };
typedef struct {
    uint64_t count, invalid;
    irq_latency_writeback_witness_t witness;
    hpet_clock_sample_t sample;
    bool result;
} irq_latency_writeback_clock_record_t;
void irq_latency_writeback_clock_record(irq_latency_writeback_clock_record_t *record,
    const irq_latency_writeback_witness_t *witness, const hpet_clock_sample_t *sample, bool result);
bool irq_latency_writeback_clock_read(unsigned site, uint64_t generation, uint64_t root,
    hpet_clock_sample_t *sample);
const irq_latency_writeback_clock_record_t *irq_latency_writeback_clock_read_record(unsigned cpu,unsigned site);
#endif
