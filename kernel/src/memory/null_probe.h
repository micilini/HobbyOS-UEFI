#ifndef NULL_PROBE_H
#define NULL_PROBE_H
#include <stdbool.h>
#include <stdint.h>
#include "../core/interrupts.h"
typedef struct {
    uint64_t generation, site, recovery, fault_rip, fault_address, fault_error;
    uint32_t slot;
    bool armed, seen;
} null_probe_state_t;
bool null_probe_match(const null_probe_state_t *state, uint64_t rip, uint64_t address,
    uint64_t error, uint64_t cs, uint32_t slot, uint64_t generation);
bool null_probe_fixup(InterruptFrame *frame, uint64_t address, uint64_t error);
int null_probe_run(void);
int null_probe_destructive(void);
#endif
