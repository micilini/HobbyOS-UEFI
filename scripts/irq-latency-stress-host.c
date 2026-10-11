/* Actual SELFTEST admission/retirement decisions; hardware and allocators are
 * explicitly substituted. Unreferenced guest-only entry points are discarded. */
#include "../kernel/src/memory/irq_latency_stress.c"
#include <assert.h>
#include <stdio.h>

static uint64_t now;
static unsigned sleeps, acquisitions, releases, busy_acquire, busy_release, backing_releases;
static mapping_status_t acquire_result, release_result;
static uint8_t backing[STRESS_DMA_BYTES];
uint32_t g_cpu_count;

uint64_t clock_monotonic_ns(void) { return now; }
void timer_sleep(uint64_t milliseconds) { ++sleeps; now += milliseconds * UINT64_C(1000000); }
uint64_t irq_save(void) { return IRQ_LATENCY_IF; }
void irq_restore(uint64_t flags) { assert(flags == IRQ_LATENCY_IF); }
bool irq_latency_identity_sample(uint32_t *cpu, uint64_t *sequence)
{ *cpu = 0; *sequence = 1; return true; }

mapping_status_t owned_mapping_stress_buffer(const dma_buffer_t *buffer, mapping_view_t *view)
{
    ++acquisitions;
    if (busy_acquire) { --busy_acquire; return MAPPING_BUSY; }
    if (acquire_result == MAPPING_OK) *view = (mapping_view_t){{1, 0, 1}, buffer->cpu, buffer->device_address, buffer->requested_bytes};
    return acquire_result;
}
mapping_status_t owned_mapping_release(mapping_handle_t handle)
{
    assert(handle.generation == 1); ++releases;
    if (busy_release) { --busy_release; return MAPPING_BUSY; }
    return release_result;
}
mapping_status_t dma_buffer_allocate(size_t bytes, size_t alignment, uint64_t maximum, dma_buffer_t *out)
{
    assert(bytes == sizeof(backing) && alignment == PAGE_SIZE && maximum == UINT32_MAX);
    memset(backing, 0, sizeof(backing));
    *out = (dma_buffer_t){{1, 0, 2}, backing, 0x200000, maximum, bytes, bytes, alignment};
    return MAPPING_OK;
}
mapping_status_t dma_buffer_release(mapping_handle_t handle)
{ assert(handle.generation == 1 && handle.domain == 2); ++backing_releases; return MAPPING_OK; }
bool dma_buffer_state(mapping_handle_t handle, dma_lifetime_t state)
{ assert(handle.generation == 1 && (state == DMA_IN_FLIGHT || state == DMA_COMPLETED)); return true; }
bool owned_mapping_subview(mapping_handle_t handle, size_t offset, size_t bytes, void **out)
{ assert(handle.generation == 1 && offset == 0 && bytes == 8); *out = backing; return true; }
bool owned_mapping_physical(const void *pointer, size_t bytes, uint64_t *out)
{ assert(pointer == backing && bytes == 8); *out = 0x200000; return true; }

static void reset(void)
{
    now = 0; sleeps = acquisitions = releases = busy_acquire = busy_release = backing_releases = 0;
    acquire_result = release_result = MAPPING_OK; g_stop = 0; g_cpu_count = 1;
}

int main(void)
{
    dma_buffer_t buffer = {.cpu=backing}; mapping_view_t view;
    uint32_t busy = 0;
    reset(); busy_acquire = 3;
    assert(acquire_view(&buffer, &view, &busy) == MAPPING_OK && busy == 3 && acquisitions == 4 && sleeps == 3);
    reset(); busy = 0; busy_acquire = UINT32_MAX;
    assert(acquire_view(&buffer, &view, &busy) == MAPPING_BUSY && now == STRESS_MAPPING_DEADLINE_NS);
    assert(acquisitions <= STRESS_MAPPING_ATTEMPTS && busy == acquisitions);
    reset(); busy = 0; acquire_result = MAPPING_QUARANTINED;
    assert(acquire_view(&buffer, &view, &busy) == MAPPING_QUARANTINED && acquisitions == 1 && !sleeps);
    reset(); busy = 0; g_stop = 1; busy_acquire = 10;
    assert(acquire_view(&buffer, &view, &busy) == MAPPING_BUSY && acquisitions == 1 && !sleeps);
    reset(); busy = 0; g_stop = 1; busy_release = 3;
    assert(release_view((mapping_handle_t){1,0,1}, &busy) == MAPPING_OK && releases == 4 && sleeps == 3);
    reset(); stress_worker_t worker = {0};
    assert(dma_cycle(&worker, 0) && worker.views_acquired == 1 && worker.views_released == 1 &&
           worker.dma_allocated == sizeof(backing) && worker.dma_released == sizeof(backing) && backing_releases == 1);
    reset(); memset(&worker, 0, sizeof(worker)); acquire_result = MAPPING_QUARANTINED;
    assert(!dma_cycle(&worker, 0) && !backing_releases && worker.dma_allocated == sizeof(backing) && !worker.dma_released);
    reset(); memset(&worker, 0, sizeof(worker)); release_result = MAPPING_QUARANTINED;
    assert(!dma_cycle(&worker, 0) && !backing_releases && worker.views_acquired == 1 && !worker.views_released);
    reset(); memset(&worker, 0, sizeof(worker)); acquire_result = MAPPING_INVALID;
    assert(!dma_cycle(&worker, 0) && backing_releases == 1 && worker.dma_allocated == worker.dma_released);
    puts("IRQ latency actual-C bounded stress admission and quarantine: PASS services=HOSTED");
    return 0;
}
