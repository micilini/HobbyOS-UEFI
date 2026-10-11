#ifndef MEMORY_OBSERVATION_H
#define MEMORY_OBSERVATION_H
#include "../../../shared/protocol.h"
#include "memory_observe_decode.h"
#include "../cpu/cpu_control_snapshot.h"

typedef struct {
    uint64_t base, bytes, stride, count, framebuffer, framebuffer_bytes, rsdp;
    bool valid;
} memory_boot_observation_t;

typedef struct {
    uint64_t base, mask;
    bool base_valid, mask_valid;
} memory_mtrr_pair_t;

typedef struct {
    cpu_control_sample_t cpu;
    uint64_t capability, defaults;
    memory_mtrr_pair_t pairs[255];
    uint8_t count;
    bool supported, capability_valid, defaults_valid;
} memory_mtrr_observation_t;

void memory_observation_init(const BootInfo *boot);
const memory_boot_observation_t *memory_observation_boot(void);
bool memory_observation_descriptor(uint64_t index, memory_descriptor_t *out);
bool memory_observation_bytes(uint64_t index, uint32_t offset, uint8_t *out, uint32_t size);
void memory_observation_mtrr(memory_mtrr_observation_t *out);

#endif
