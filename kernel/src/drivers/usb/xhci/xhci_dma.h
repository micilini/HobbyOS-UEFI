#ifndef XHCI_DMA_H
#define XHCI_DMA_H
#include "../../../memory/dma_buffer.h"

uint64_t xhci_dma_capability_limit(uint32_t hccparams1);
bool xhci_dma_initialize(uint32_t hccparams1);
void *xhci_dma_allocate(size_t bytes, size_t alignment);
uint64_t xhci_dma_address(const void *cpu, size_t bytes);
void *xhci_dma_cpu(uint64_t device_address, size_t bytes);
void *xhci_transfer_allocate(size_t bytes);
bool xhci_transfer_release(void *cpu);
bool xhci_transfer_begin(void *cpu, size_t bytes, dma_buffer_t *buffer, uint64_t *address);
void xhci_transfer_finish(const dma_buffer_t *buffer, bool completed);
#endif
