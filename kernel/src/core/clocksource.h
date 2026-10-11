#ifndef HOBBYOS_CLOCKSOURCE_H
#define HOBBYOS_CLOCKSOURCE_H

#include <stdbool.h>
#include <stdint.h>
#include "../smp/smp_topology.h"
#include "pvclock.h"

typedef enum {
    CLOCKSOURCE_HPET = 1,
    CLOCKSOURCE_KVM = 2,
    CLOCKSOURCE_FAILED = 3
} clocksource_id_t;

typedef struct {
    uint64_t counter, ns, epoch;
    uint32_t cpu, retries;
    clocksource_id_t source;
} clocksource_sample_t;

/* Boot-only preparation on the owning CPU, then one BSP selection barrier. */
void clocksource_prepare_local(void);
void clocksource_select(void);
void clocksource_report(void);
clocksource_id_t clocksource_current(void);
uint64_t clocksource_epoch(void);
void clocksource_invalidate(void);
/* The caller has just identified slot under continuous IRQ exclusion. */
bool clocksource_read_local(cpu_slot_t slot, clocksource_sample_t *out);

#if defined(HOBBYOS_IRQ_LATENCY) && HOBBYOS_IRQ_LATENCY
typedef struct {
    uint64_t reads, references, maximum_reference_ns, last_tsc, last_ns, epoch;
    uint32_t source, reason, busy, registered;
    pvclock_reference_t maximum, previous;
} clocksource_terminal_t;
bool clocksource_terminal_capture(cpu_slot_t slot);
const clocksource_terminal_t *clocksource_terminal_read(cpu_slot_t slot);
#endif

#endif
