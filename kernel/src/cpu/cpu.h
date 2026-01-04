#ifndef CPU_H
#define CPU_H

#include <stdint.h>


typedef struct {
    char vendor_id[13];
    uint8_t stepping;
    uint8_t model;
    uint8_t family;
    uint8_t type;
    char brand_string[49];
} CpuInfo;


void cpu_get_cpuid(uint32_t leaf, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx);
uint64_t cpu_read_msr(uint32_t msr);
void cpu_write_msr(uint32_t msr, uint64_t value);


uint8_t inb(uint16_t port);
void outb(uint16_t port, uint8_t val);
void io_wait(); 


void init_cpu();

#endif