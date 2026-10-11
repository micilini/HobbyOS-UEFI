#ifndef STACK_FAULT_H
#define STACK_FAULT_H
#include "stack_domain.h"
typedef struct {
    uint64_t address, error, rip, rsp, cs, handler_rsp;
    uint32_t vector, cpu, apic;
} stack_fault_facts_t;
typedef enum { STACK_FAULT_UNKNOWN, STACK_FAULT_GUARD_ACCESS, STACK_FAULT_OVERFLOW_ASSOCIATED } stack_fault_kind_t;
stack_fault_kind_t stack_fault_classify(const stack_fault_facts_t *facts,
    const stack_owner_view_t *guard_owner, const stack_owner_view_t *handler_owner);
#endif
