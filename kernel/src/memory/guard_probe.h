#ifndef GUARD_PROBE_H
#define GUARD_PROBE_H
#include "stack_domain.h"
#include "../core/interrupts.h"
#include "../smp/cpu_request.h"
typedef struct {
    uint64_t generation, root, address, owner_generation, site, recovery;
    uint64_t fault_rip, fault_address, fault_error;
    uint32_t cpu, apic, owner_slot;
    bool armed, seen;
} guard_probe_state_t;
bool guard_probe_match(const guard_probe_state_t *state,const stack_owner_view_t *owner,
    uint64_t generation,uint64_t root,uint32_t cpu,uint32_t apic,uint64_t rip,
    uint64_t address,uint64_t error,uint64_t cs,uint64_t site,uint64_t recovery);
bool guard_probe_fixup(InterruptFrame *frame,uint64_t address,uint64_t error);
bool guard_probe_owner(const stack_owner_view_t *owner);
bool stack_test_emit(const char *format,...) __attribute__((format(printf,1,2)));
#if defined(HOBBYOS_SERIAL_PROOF_DIAGNOSTIC) && HOBBYOS_SERIAL_PROOF_DIAGNOSTIC
bool stack_test_emit_checked(const char *format,...) __attribute__((format(printf,1,2)));
#endif
int stack_test_run(bool churn);
int stack_test_overflow(void);
int stack_test_timeout(void);
void stack_test_retirement_start(const stack_owner_view_t *owner);
void stack_test_retirement_failed(const stack_owner_view_t *owner,const cpu_request_result_t *result);
#endif
