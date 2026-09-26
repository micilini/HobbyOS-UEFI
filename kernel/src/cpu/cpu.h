#ifndef CPU_H
#define CPU_H

#include <stdbool.h>
#include <stdint.h>

#include "../core/io.h"

typedef struct
{
    char vendor_id[13];
    uint8_t stepping;
    uint8_t model;
    uint8_t family;
    uint8_t type;
    char brand_string[49];
} CpuInfo;

/*
 * Returns false and clears provided outputs when the leaf or a defined
 * terminating subleaf is outside its architectural domain.
 */
bool cpu_get_cpuid_count(uint32_t leaf, uint32_t subleaf,
                         uint32_t *eax, uint32_t *ebx,
                         uint32_t *ecx, uint32_t *edx);
void cpu_get_cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                   uint32_t *ecx, uint32_t *edx);
bool cpu_local_arch_id(uint32_t *cpu_id);

uint64_t cpu_read_msr(uint32_t msr);
void cpu_write_msr(uint32_t msr, uint64_t value);

/*
 * The safe variants recover only #GP raised at their registered instruction.
 * A failed read leaves *out unchanged. They do not validate arbitrary memory
 * or make a semantically unsafe MSR write safe.
 */
bool cpu_read_msr_safe(uint32_t msr, uint64_t *out);
bool cpu_write_msr_safe(uint32_t msr, uint64_t value);
bool cpu_msr_fixup_lookup(uint64_t instruction, uint64_t error_code,
                          uint64_t code_segment, uint64_t *fixup);

void init_cpu(void);

#endif
