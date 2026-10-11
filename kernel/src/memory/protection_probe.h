#ifndef PROTECTION_PROBE_H
#define PROTECTION_PROBE_H
#ifdef HOBBYOS_SELFTEST
#include "../core/interrupts.h"
#include "../smp/cpu_request.h"

#define PROTECTION_ATTEMPTS 6u
typedef enum { PROTECTION_WRITE_TEXT,PROTECTION_WRITE_RODATA,PROTECTION_FETCH_DATA } protection_kind_t;
typedef struct {
    uint64_t address,physical,site,recovery;
    protection_kind_t kind;
    uint32_t alias;
    uint8_t original;
} protection_target_t;
typedef struct {
    uint64_t generation,collection,root,site,address,recovery;
    uint64_t fault_rip,fault_address,fault_error,fault_cs;
    uint32_t slot,apic,attempt;
    protection_kind_t kind;
    uint32_t alias;
    bool armed,seen,completed;
} protection_probe_state_t;
typedef struct {
    uint64_t generation,collection,root,rip,address,error,cs;
    uint32_t slot,apic;
} protection_fault_t;
bool protection_probe_arm(protection_probe_state_t *state,const protection_target_t *target,
                         const protection_fault_t *owner,unsigned attempt,bool nx);
bool protection_probe_match(const protection_probe_state_t *state,const protection_target_t *target,
                           const protection_fault_t *event);
bool protection_probe_complete(protection_probe_state_t *state,const protection_target_t *target,
                              unsigned returned,uint8_t after);
bool protection_probe_fixup(InterruptFrame *frame,uint64_t address,uint64_t error);
bool protection_probe_execute(const cpu_request_spec_t *spec,cpu_request_sample_t *sample);
int protection_probe_run(void);
#endif
#endif
