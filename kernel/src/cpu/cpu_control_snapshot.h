#ifndef CPU_CONTROL_SNAPSHOT_H
#define CPU_CONTROL_SNAPSHOT_H
#include "../memory/page_table_decode.h"
#include "../smp/smp_topology.h"

typedef struct {
    pt_controls_t paging;
    uint32_t slot, apic_id, cpuid1_edx, extended_edx, address_eax;
    bool identity_valid, is_bsp;
} cpu_control_sample_t;

void cpu_control_read_local(cpu_control_sample_t *out);

#ifdef HOBBYOS_SELFTEST
typedef struct {
    uint64_t generation;
    uint32_t target_mask, complete_mask, error_mask;
    uint32_t apic_ids[HOBBYOS_MAX_CPUS];
    cpu_control_sample_t cpus[HOBBYOS_MAX_CPUS];
    const char *reason;
    bool complete, x2apic;
} cpu_control_capture_t;

/* Storage remains owned until release; a timeout quarantines it for this boot. */
const cpu_control_capture_t *cpu_control_snapshot(void);
void cpu_control_snapshot_release(void);
void cpu_control_snapshot_irq(void);
#endif
#endif
