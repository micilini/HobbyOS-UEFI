#include "xhci_dma.h"
#include "../../../core/panic.h"

static uint64_t g_dma_limit;
static bool g_dma_ready, g_dma_uncertain;
uint64_t xhci_dma_capability_limit(uint32_t hccparams1)
{ return (hccparams1 & 1u) ? UINT64_MAX : UINT32_MAX; }

bool xhci_dma_initialize(uint32_t hccparams1)
{
    if (g_dma_ready) return false;
    g_dma_limit = xhci_dma_capability_limit(hccparams1);
    g_dma_ready = true;
    return true;
}

void *xhci_dma_allocate(size_t bytes, size_t alignment)
{
    dma_buffer_t buffer;
    if (!g_dma_ready || __atomic_load_n(&g_dma_uncertain, __ATOMIC_ACQUIRE) ||
        dma_buffer_allocate(bytes, alignment, g_dma_limit, &buffer) != MAPPING_OK) return NULL;
    if (!dma_buffer_state(buffer.handle, DMA_PERSISTENT))
        kpanic("xHCI persistent DMA ownership publication failed");
    return buffer.cpu;
}

uint64_t xhci_dma_address(const void *cpu, size_t bytes)
{
    dma_buffer_t buffer;
    uint64_t address;
    if (!dma_buffer_lookup(cpu, bytes, &buffer, &address) || buffer.maximum_address != g_dma_limit)
        kpanic("xHCI address is outside an owned device buffer");
    return address;
}

void *xhci_dma_cpu(uint64_t device_address, size_t bytes)
{
    void *cpu;
    if (!dma_buffer_device_view(device_address, bytes, g_dma_limit, &cpu))
        kpanic("xHCI device context is outside an owned buffer");
    return cpu;
}

void *xhci_transfer_allocate(size_t bytes)
{
    dma_buffer_t buffer;
    if (!g_dma_ready || __atomic_load_n(&g_dma_uncertain, __ATOMIC_ACQUIRE) ||
        dma_buffer_allocate(bytes, 64, g_dma_limit, &buffer) != MAPPING_OK) return NULL;
    return buffer.cpu;
}

bool xhci_transfer_release(void *cpu)
{
    dma_buffer_t buffer;
    uint64_t address;
    return dma_buffer_lookup(cpu, 1, &buffer, &address) && cpu == buffer.cpu &&
           dma_buffer_release(buffer.handle) == MAPPING_OK;
}

bool xhci_transfer_begin(void *cpu, size_t bytes, dma_buffer_t *buffer, uint64_t *address)
{
    if (__atomic_load_n(&g_dma_uncertain, __ATOMIC_ACQUIRE) ||
        !dma_buffer_lookup(cpu, bytes, buffer, address) || buffer->maximum_address != g_dma_limit) return false;
    return dma_buffer_state(buffer->handle, DMA_IN_FLIGHT);
}

void xhci_transfer_finish(const dma_buffer_t *buffer, bool completed)
{
    if (!buffer) return;
    if (!dma_buffer_state(buffer->handle, completed ? DMA_COMPLETED : DMA_UNCERTAIN))
        kpanic("xHCI transfer ownership completion failed");
    if (!completed) __atomic_store_n(&g_dma_uncertain, true, __ATOMIC_RELEASE);
}
