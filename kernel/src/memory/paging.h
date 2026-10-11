#ifndef PAGING_H
#define PAGING_H
#include <stdint.h>
#include <stdbool.h>
typedef struct { uint32_t extended_features, physical_bits; } paging_boot_requirements_t;
bool paging_boot_requirements(paging_boot_requirements_t *out);
void init_paging(uint64_t framebuffer_physical, uint64_t framebuffer_bytes);
uint64_t paging_kernel_root_physical(void);
void paging_load_kernel_root(void);
bool paging_low_half_empty(void);
#endif
