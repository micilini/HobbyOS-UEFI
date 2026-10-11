#include "../core/irq_latency.h"
#include "dma_buffer.h"
#include "mapping_core.h"
#include "physical_translation.h"
#include "../core/spinlock.h"
#include "../libc/memory.h"

#define DMA_BUFFER_CAPACITY 2048u
typedef struct { dma_buffer_t buffer; dma_lifetime_t state; bool allocated, preparing; } dma_slot_t;
static dma_slot_t g_dma[DMA_BUFFER_CAPACITY];
static spinlock_t g_dma_lock;
static uint64_t g_dma_generation;
#ifdef HOBBYOS_SELFTEST
#include "mapping_probe.h"
bool dma_buffer_sample(unsigned index, dma_buffer_t *out, dma_lifetime_t *state)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_DMA);
    if (!out || !state || index >= DMA_BUFFER_CAPACITY) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_dma_lock);
    const dma_slot_t *slot = &g_dma[index];
    bool ok = slot->allocated && !slot->preparing;
    if (ok) { *out = slot->buffer; *state = slot->state; }
    spin_unlock_irqrestore(&g_dma_lock, flags); return ok;
}
#endif

mapping_status_t dma_buffer_allocate(size_t bytes, size_t alignment,
                                     uint64_t maximum, dma_buffer_t *out)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_DMA);
    mapping_range_t extent;
    uint64_t limit;
    if (!out || !dma_extent_limit(bytes, alignment, maximum, physical_allocation_ceiling(), &extent, &limit))
        return MAPPING_INVALID;
    irq_flags_t flags = spin_lock_irqsave(&g_dma_lock);
    unsigned slot;
    for (slot = 0; slot < DMA_BUFFER_CAPACITY && g_dma[slot].allocated; ++slot) { }
    if (slot == DMA_BUFFER_CAPACITY || g_dma_generation == UINT64_MAX) {
        spin_unlock_irqrestore(&g_dma_lock, flags); return MAPPING_EXHAUSTED;
    }
    mapping_handle_t handle = {++g_dma_generation, slot, 2};
    g_dma[slot].allocated = g_dma[slot].preparing = true;
    spin_unlock_irqrestore(&g_dma_lock, flags);
    uint64_t physical = 0;
    void *cpu = NULL;
    size_t alignment_frames = alignment > PAGE_SIZE ? alignment / PAGE_SIZE : 1;
    bool ok = pmm_alloc_constrained_frames(extent.pages, alignment_frames, limit, &physical);
    if (ok) ok = physical_pointer_range(physical, extent.pages * PAGE_SIZE, PHYSICAL_RAM, &cpu);
    if (!ok && physical)
        for (uint64_t i = 0; i < extent.pages; ++i) pmm_free_frame((void *)(uintptr_t)(physical + i * PAGE_SIZE));
    dma_buffer_t buffer = {handle, cpu, physical, maximum, bytes, extent.pages * PAGE_SIZE, alignment};
    if (ok) memset(cpu, 0, buffer.backing_bytes);
    flags = spin_lock_irqsave(&g_dma_lock);
    g_dma[slot] = (dma_slot_t){buffer, DMA_OWNED, ok, false};
    if (ok) *out = buffer;
    spin_unlock_irqrestore(&g_dma_lock, flags);
    return ok ? MAPPING_OK : MAPPING_NO_MEMORY;
}

static dma_slot_t *owned(mapping_handle_t handle)
{
    if (!handle.generation || handle.domain != 2 || handle.slot >= DMA_BUFFER_CAPACITY) return NULL;
    dma_slot_t *slot = &g_dma[handle.slot];
    return slot->allocated && !slot->preparing && slot->buffer.handle.generation == handle.generation ? slot : NULL;
}

mapping_status_t dma_buffer_release(mapping_handle_t handle)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_DMA);
    irq_flags_t flags = spin_lock_irqsave(&g_dma_lock);
    dma_slot_t *slot = owned(handle);
    mapping_status_t status = !slot ? MAPPING_STALE :
        (slot->state == DMA_OWNED || slot->state == DMA_COMPLETED) ? MAPPING_OK : MAPPING_QUARANTINED;
    dma_buffer_t buffer = {0};
    if (status == MAPPING_OK) { buffer = slot->buffer; slot->preparing = true; }
    spin_unlock_irqrestore(&g_dma_lock, flags);
    if (status != MAPPING_OK) return status;
    for (size_t i = 0; i < buffer.backing_bytes; i += PAGE_SIZE)
        pmm_free_frame((void *)(uintptr_t)(buffer.device_address + i));
    flags = spin_lock_irqsave(&g_dma_lock);
    slot->allocated = slot->preparing = false;
    spin_unlock_irqrestore(&g_dma_lock, flags);
    return MAPPING_OK;
}

bool dma_buffer_state(mapping_handle_t handle, dma_lifetime_t state)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_DMA);
    irq_flags_t flags = spin_lock_irqsave(&g_dma_lock);
    dma_slot_t *slot = owned(handle);
    bool ok = slot && ((state == DMA_IN_FLIGHT && (slot->state == DMA_OWNED || slot->state == DMA_COMPLETED)) ||
        (state == DMA_COMPLETED && slot->state == DMA_IN_FLIGHT) ||
        (state == DMA_PERSISTENT && slot->state == DMA_OWNED) ||
        (state == DMA_UNCERTAIN && (slot->state == DMA_IN_FLIGHT || slot->state == DMA_PERSISTENT)));
    if (ok) slot->state = state;
    spin_unlock_irqrestore(&g_dma_lock, flags);
    return ok;
}

bool dma_buffer_lookup(const void *cpu, size_t bytes, dma_buffer_t *out, uint64_t *address)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_DMA);
    if (!cpu || !bytes || !out || !address) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_dma_lock);
    bool ok = false;
    for (unsigned i = 0; i < DMA_BUFFER_CAPACITY; ++i) {
        const dma_slot_t *s = &g_dma[i];
        size_t offset = (uintptr_t)cpu - (uintptr_t)s->buffer.cpu;
        if (s->allocated && !s->preparing && s->state != DMA_UNCERTAIN &&
            (uintptr_t)cpu >= (uintptr_t)s->buffer.cpu && offset < s->buffer.requested_bytes &&
            bytes <= s->buffer.requested_bytes - offset) {
            *out = s->buffer; *address = s->buffer.device_address + offset; ok = true; break;
        }
    }
    spin_unlock_irqrestore(&g_dma_lock, flags);
    return ok;
}

bool dma_buffer_address(const dma_buffer_t *buffer, size_t offset, size_t bytes, uint64_t *address)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_DMA);
    if (!buffer || !address || !bytes) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_dma_lock);
    dma_slot_t *slot = owned(buffer->handle);
    bool ok = slot && slot->state != DMA_UNCERTAIN && slot->buffer.cpu == buffer->cpu &&
        slot->buffer.device_address == buffer->device_address && offset < slot->buffer.requested_bytes &&
        bytes <= slot->buffer.requested_bytes - offset;
    if (ok) *address = slot->buffer.device_address + offset;
    spin_unlock_irqrestore(&g_dma_lock, flags);
    return ok;
}

bool dma_buffer_device_view(uint64_t address, size_t bytes, uint64_t maximum, void **cpu)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_DMA);
    if (!bytes || !cpu) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_dma_lock);
    bool ok = false;
    for (unsigned i = 0; i < DMA_BUFFER_CAPACITY; ++i) {
        const dma_slot_t *s = &g_dma[i];
        uint64_t offset = address - s->buffer.device_address;
        if (s->allocated && !s->preparing && s->state != DMA_UNCERTAIN &&
            s->buffer.maximum_address == maximum && address >= s->buffer.device_address &&
            offset < s->buffer.requested_bytes && bytes <= s->buffer.requested_bytes - offset) {
            *cpu = (uint8_t *)s->buffer.cpu + offset; ok = true; break;
        }
    }
    spin_unlock_irqrestore(&g_dma_lock, flags);
    return ok;
}
