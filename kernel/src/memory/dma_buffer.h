#ifndef DMA_BUFFER_H
#define DMA_BUFFER_H
#include "owned_mapping.h"

typedef enum { DMA_OWNED, DMA_IN_FLIGHT, DMA_COMPLETED, DMA_PERSISTENT,
               DMA_UNCERTAIN } dma_lifetime_t;
typedef struct {
    mapping_handle_t handle;
    void *cpu;
    uint64_t device_address, maximum_address;
    size_t requested_bytes, backing_bytes, alignment;
} dma_buffer_t;

/* maximum_address is inclusive, including UINT64_MAX. The last backing byte
 * satisfies both this limit and PMM's exclusive reachability ceiling. Backing
 * is contiguous WB RAM and entirely zeroed before publication. Alignment is a
 * power of two; page backing preserves stricter requested alignments.
 * These calls never remove the permanent RAM mapping. Release requires proven
 * device retirement; a timeout is DMA_UNCERTAIN, not completion. Unknown
 * ownership retains storage. No automatic stack/heap bouncing is provided.
 */
mapping_status_t dma_buffer_allocate(size_t bytes, size_t alignment,
                                     uint64_t maximum_address, dma_buffer_t *out);
mapping_status_t dma_buffer_release(mapping_handle_t handle);
bool dma_buffer_state(mapping_handle_t handle, dma_lifetime_t state);
bool dma_buffer_lookup(const void *cpu, size_t bytes, dma_buffer_t *out,
                       uint64_t *device_address);
bool dma_buffer_address(const dma_buffer_t *buffer, size_t offset, size_t bytes,
                        uint64_t *device_address);
bool dma_buffer_device_view(uint64_t device_address, size_t bytes,
                            uint64_t maximum_address, void **cpu);
#endif
