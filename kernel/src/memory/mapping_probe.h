#ifndef MAPPING_PROBE_H
#define MAPPING_PROBE_H
#include <stdbool.h>
#include <stdint.h>
#include "dma_buffer.h"
#include "../core/interrupts.h"
typedef struct {
    uint64_t address, generation, site, recovery;
    uint32_t slot;
    bool armed, seen;
} mapping_probe_state_t;
bool mapping_probe_match(const mapping_probe_state_t *state, uint64_t rip,
    uint64_t address, uint64_t error, uint64_t cs, uint32_t slot, uint64_t generation);
bool mapping_probe_fixup(InterruptFrame *frame, uint64_t address, uint64_t error);
int mapping_fixture_run(void);
int framebuffer_copy_benchmark(void);
mapping_status_t owned_mapping_test_buffer(const dma_buffer_t *buffer, mapping_view_t *out);
/* Owned DMA only, no post-unmap access or fault-probe reservation. */
mapping_status_t owned_mapping_stress_buffer(const dma_buffer_t *buffer, mapping_view_t *out);
bool owned_mapping_test_finish(mapping_handle_t handle);
typedef struct {
    mapping_view_t view;
    uint64_t flags, source;
    uint32_t origin, state;
} mapping_view_sample_t;
bool owned_mapping_sample(unsigned slot, mapping_view_sample_t *out);
bool paging_owned_leaf_pair(uint64_t virtual_address, uint64_t physical,
                            uint64_t *managed, uint64_t *identity);
bool dma_buffer_sample(unsigned slot, dma_buffer_t *out, dma_lifetime_t *state);
#endif
