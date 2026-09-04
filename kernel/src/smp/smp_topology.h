#ifndef SMP_TOPOLOGY_H
#define SMP_TOPOLOGY_H

#include <stdint.h>
#include <stdbool.h>

#include "cpu_limits.h"

typedef uint32_t cpu_slot_t;
#define CPU_SLOT_INVALID UINT32_MAX

typedef struct
{
    cpu_slot_t slot;
    uint32_t apic_id;
    uint32_t acpi_id;
    bool is_bsp;

    volatile uint32_t state;

    uint64_t stack_top;
    void *tss_ptr;
    void *gdt_ptr;

} SmpCpuInfo;

extern SmpCpuInfo g_cpus[HOBBYOS_MAX_CPUS];
extern uint32_t g_cpu_count;
extern uint32_t g_bsp_apic_id;

bool smp_topology_init(void);
void smp_prepare_cpu_structures(void);
bool smp_cpu_slot_from_apic_id(uint32_t apic_id, cpu_slot_t *out_slot);
bool smp_current_cpu_slot(cpu_slot_t *out_slot);
SmpCpuInfo *smp_cpu_by_slot(cpu_slot_t slot);
const SmpCpuInfo *smp_cpu_by_slot_const(cpu_slot_t slot);
cpu_slot_t smp_bsp_cpu_slot(void);
uint32_t smp_online_cpu_count(void);
uint32_t smp_failed_cpu_count(void);
bool smp_mark_cpu_online(cpu_slot_t slot);
void smp_log_cpu_online(cpu_slot_t slot, const char *role);

#endif
