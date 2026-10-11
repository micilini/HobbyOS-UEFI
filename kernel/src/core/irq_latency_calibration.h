#ifndef HOBBYOS_IRQ_LATENCY_CALIBRATION_H
#define HOBBYOS_IRQ_LATENCY_CALIBRATION_H

#include "irq_latency_time.h"

#define IRQ_LATENCY_REFERENCE_POLL_LIMIT 2000000u
#define IRQ_LATENCY_REFERENCE_TARGET_NS UINT64_C(20000000)

bool irq_latency_read_capabilities(irq_latency_capabilities_t *out);
bool irq_latency_reference_point(irq_latency_reference_point_t *out);
bool irq_latency_reference_window(irq_latency_calibration_window_t *out);
/* Two independent points; return describes only the intervening waiter. */
uint32_t irq_latency_reference_acquire(irq_latency_calibration_window_t *out);

#endif
