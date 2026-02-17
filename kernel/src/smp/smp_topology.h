#ifndef SMP_TOPOLOGY_H
#define SMP_TOPOLOGY_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_CPUS 32  

typedef struct {
    uint8_t apic_id;      
    uint8_t acpi_id;      
    bool is_bsp;          
    bool is_online;       
    
} CpuInfo;


extern CpuInfo g_cpus[MAX_CPUS];
extern uint32_t g_cpu_count;
extern uint8_t g_bsp_apic_id;


void smp_topology_init();

#endif