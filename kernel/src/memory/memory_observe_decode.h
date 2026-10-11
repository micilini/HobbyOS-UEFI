#ifndef MEMORY_OBSERVE_DECODE_H
#define MEMORY_OBSERVE_DECODE_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t physical, virtual_address, pages, attributes, end;
    uint32_t type;
    bool range_valid;
} memory_descriptor_t;

typedef struct {
    uint64_t base, end, size;
    uint8_t type;
    bool valid, enabled;
    const char *reason;
} memory_mtrr_range_t;

bool memory_map_shape(uint64_t base, uint64_t bytes, uint64_t stride,
                       uint64_t *count);
bool memory_descriptor_decode(const uint8_t *data, uint64_t bytes,
                               uint64_t stride, uint64_t index,
                               memory_descriptor_t *out);
bool memory_interval_overlap(uint64_t a, uint64_t end_a,
                              uint64_t b, uint64_t end_b,
                              uint64_t *start, uint64_t *end);
void memory_mtrr_decode(uint64_t base, uint64_t mask, unsigned physical_bits,
                        bool supported, bool read_valid,
                        memory_mtrr_range_t *out);
const char *memory_efi_type(uint32_t type);
#endif
