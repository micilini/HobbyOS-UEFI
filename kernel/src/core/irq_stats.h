#ifndef IRQ_STATS_H
#define IRQ_STATS_H

#include <stdint.h>
#include <stdbool.h>

void irq_stats_record(uint8_t vector);

void irq_stats_record_unhandled(void);

void irq_stats_reset(void);

void irq_stats_dump(void);

uint64_t irq_stats_get(uint8_t vector);
uint64_t irq_stats_get_unhandled(void);
uint8_t irq_stats_last_vector(void);

#endif