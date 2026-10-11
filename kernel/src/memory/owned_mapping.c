#include "../core/irq_latency.h"
#include "mmio_mapping.h"
#include "firmware_mapping.h"
#include "mapping_core.h"
#include "physical_internal.h"
#include "boot_address.h"
#include "kernel_tlb.h"
#include "../core/spinlock.h"
#include "../utils/bitmap.h"

/* The admission token is never acquired by an interrupt callback. Metadata
 * locks only protect bounded copies and are released before paging or waits. */
typedef enum { VIEW_FREE, VIEW_PREPARING, VIEW_LIVE, VIEW_RETIRED, VIEW_QUARANTINED, VIEW_TEST_RETIRED } view_state_t;
typedef struct {
    mapping_view_t view;
    mapping_range_t range;
    uint64_t first_page, flags, source;
    cache_attribute_t cache;
    view_state_t state;
    mmio_origin_kind_t origin;
} owned_view_t;
static owned_view_t g_views[OWNED_MAPPING_CAPACITY];
static uint8_t g_window_bits[MAPPING_WINDOW_PAGES / 8];
static Bitmap g_window_bitmap = {g_window_bits, sizeof(g_window_bits), MAPPING_WINDOW_PAGES};
static spinlock_t g_view_lock;
static uint32_t g_admission;
static uint64_t g_generation;
static bool g_bootstrap_closed;
#ifdef HOBBYOS_SELFTEST
#include "mapping_probe.h"
static mapping_handle_t g_probe_reservation;
bool owned_mapping_sample(unsigned slot, mapping_view_sample_t *out)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_MAPPING);
    if (!out || slot >= OWNED_MAPPING_CAPACITY) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_view_lock);
    const owned_view_t *v = &g_views[slot];
    bool ok = v->state != VIEW_FREE;
    if (ok) *out = (mapping_view_sample_t){v->view,v->flags,v->source,v->origin,v->state};
    spin_unlock_irqrestore(&g_view_lock, flags); return ok;
}
#endif

static mapping_status_t enter(irq_flags_t *saved)
{
    bool boot = !__atomic_load_n(&g_bootstrap_closed, __ATOMIC_ACQUIRE);
    if (!boot && !cpu_request_mapping_context_allowed()) return MAPPING_CONTEXT;
    *saved = boot ? irq_save() : 0;
    uint32_t empty = 0;
    if (!__atomic_compare_exchange_n(&g_admission, &empty, 1, false,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        if (boot) irq_restore(*saved);
        return MAPPING_BUSY;
    }
    return MAPPING_OK;
}
static void leave(irq_flags_t saved)
{
    __atomic_store_n(&g_admission, 0, __ATOMIC_RELEASE);
    if (!__atomic_load_n(&g_bootstrap_closed, __ATOMIC_ACQUIRE)) irq_restore(saved);
}
void owned_mapping_close_bootstrap(void)
{ __atomic_store_n(&g_bootstrap_closed, true, __ATOMIC_RELEASE); }

static bool flush(uint64_t first, uint64_t bytes)
{
    return g_bootstrap_closed ? kernel_mapping_invalidate(first, bytes).status == CPU_REQUEST_OK :
                               kernel_tlb_bootstrap_invalidate(first, bytes);
}
static bool writeback(void)
{
    return g_bootstrap_closed ? kernel_cache_writeback().status == CPU_REQUEST_OK :
                               kernel_cache_bootstrap_writeback();
}
static bool overlap(uint64_t first, uint64_t bytes, const mapping_range_t *r)
{ return first < r->first + r->pages * PAGE_SIZE && r->first < first + bytes; }

static mapping_status_t acquire(uint64_t physical, size_t bytes, uint64_t attributes,
    cache_attribute_t cache, mmio_origin_kind_t origin, uint64_t source, mapping_view_t *out)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_MAPPING);
    mapping_range_t range;
    if (!out || !mapping_range_checked(physical, bytes, &range) || range.pages > MAPPING_WINDOW_PAGES ||
        !paging_owned_extent_allowed(range.first,range.pages*PAGE_SIZE))
        return MAPPING_INVALID;
    const cpu_policy_t *policy=cpu_policy_get();
    if (!policy) return MAPPING_INVALID;
    if (policy->efer_write & CPU_EFER_NXE) attributes|=PAGE_NX;
    irq_flags_t saved;
    mapping_status_t status = enter(&saved);
    if (status != MAPPING_OK) return status;
    irq_flags_t locked = spin_lock_irqsave(&g_view_lock);
    unsigned slot = OWNED_MAPPING_CAPACITY;
    for (unsigned i = 0; i < OWNED_MAPPING_CAPACITY; ++i) {
        if (g_views[i].state == VIEW_FREE) { if (slot == OWNED_MAPPING_CAPACITY) slot = i; continue; }
        if (overlap(range.first, range.pages * PAGE_SIZE, &g_views[i].range) &&
            (cache != g_views[i].cache || g_views[i].state != VIEW_LIVE)) {
            status = MAPPING_CONFLICT; break;
        }
    }
    size_t first = 0;
    if (status == MAPPING_OK && (slot == OWNED_MAPPING_CAPACITY || g_generation == UINT64_MAX ||
        !bitmap_find_zero_run(&g_window_bitmap, 0, range.pages, &first, NULL))) status = MAPPING_EXHAUSTED;
    if (status == MAPPING_OK) {
        (void)bitmap_set_range(&g_window_bitmap, first, range.pages);
        g_views[slot] = (owned_view_t){
            .view = {{++g_generation, slot, 1}, (void *)(uintptr_t)(DEVICE_WINDOW_BASE + first * PAGE_SIZE + range.offset), physical, bytes},
            .range = range, .first_page = first, .flags = attributes, .source = source,
            .cache = cache, .state = VIEW_PREPARING, .origin = origin};
    }
    spin_unlock_irqrestore(&g_view_lock, locked);
    if (status != MAPPING_OK) { leave(saved); return status; }
    /* No pointer has escaped. All CPU users of a prior device alias must be
     * absent by the discovery/first-adoption contract. Translation completion
     * precedes new cache selection; cache completion precedes publication. */
    if (origin && (!paging_owned_withdraw_identity(range.first, range.pages * PAGE_SIZE) ||
                   !flush(range.first, range.pages * PAGE_SIZE))) status = MAPPING_QUARANTINED;
    if (status == MAPPING_OK && !paging_owned_install(first, range.first, range.pages, attributes))
        status = MAPPING_QUARANTINED;
    if (status == MAPPING_OK && (!flush(DEVICE_WINDOW_BASE + first * PAGE_SIZE, range.pages * PAGE_SIZE) ||
                               (origin && !writeback()))) status = MAPPING_QUARANTINED;
    locked = spin_lock_irqsave(&g_view_lock);
    g_views[slot].state = status == MAPPING_OK ? VIEW_LIVE : VIEW_QUARANTINED;
    if (status == MAPPING_OK) *out = g_views[slot].view;
    spin_unlock_irqrestore(&g_view_lock, locked);
    leave(saved);
    return status;
}

mapping_status_t mmio_mapping_acquire(const mmio_origin_t *origin,
    cache_attribute_t cache, mapping_view_t *out)
{
    mapping_range_t range;
    uint64_t selection;
    if (!origin || origin->kind < MMIO_FRAMEBUFFER || origin->kind > MMIO_PCI_BAR ||
        cache == CACHE_WRITE_BACK || !cache_attribute_encode(cpu_policy_get(), cache, CACHE_LEAF_4K, &selection) ||
        !mapping_range_checked(origin->physical, origin->bytes, &range) ||
        !physical_device_extent_allowed(range.first, range.pages * PAGE_SIZE)) return MAPPING_INVALID;
    if (!mmio_origin_valid(origin)) return MAPPING_INVALID;
    return acquire(origin->physical, origin->bytes, PAGE_PRESENT | PAGE_RW | selection,
                   cache, origin->kind, origin->source, out);
}

#if defined(HOBBYOS_SELFTEST) && defined(HOBBYOS_FRAMEBUFFER_REFERENCE)
mapping_status_t mmio_framebuffer_reference(const mmio_origin_t *origin, mapping_view_t *out)
{
    if (!origin || origin->kind!=MMIO_FRAMEBUFFER || !mmio_origin_valid(origin)) return MAPPING_INVALID;
    return acquire(origin->physical, origin->bytes, PAGE_PRESENT|PAGE_RW|PAGE_PAT,
                   CACHE_WRITE_BACK, origin->kind, origin->source, out);
}
#endif

mapping_status_t firmware_mapping_acquire(uint64_t physical, size_t bytes, mapping_view_t *out)
{
    mapping_range_t range;
    if (!mapping_range_checked(physical, bytes, &range) ||
        !physical_provenance_range(range.first, range.pages * PAGE_SIZE, PHYSICAL_FIRMWARE)) return MAPPING_INVALID;
    return acquire(physical, bytes, PAGE_PRESENT, CACHE_WRITE_BACK, 0, physical, out);
}

mapping_status_t owned_mapping_release(mapping_handle_t handle)
{
    IRQ_LATENCY_SCOPE_MASK((UINT64_C(1) << IRQ_PATH_MAPPING) | (UINT64_C(1) << IRQ_PATH_UNMAP));
    if (!handle.generation || handle.domain != 1 || handle.slot >= OWNED_MAPPING_CAPACITY) return MAPPING_STALE;
    irq_flags_t saved;
    mapping_status_t status = enter(&saved);
    if (status != MAPPING_OK) return status;
    irq_flags_t flags = spin_lock_irqsave(&g_view_lock);
    owned_view_t view = g_views[handle.slot];
    if (view.view.handle.generation != handle.generation || view.state == VIEW_FREE)
        status = MAPPING_STALE;
    else if (view.state != VIEW_LIVE) status = MAPPING_QUARANTINED;
    else g_views[handle.slot].state = VIEW_RETIRED;
    spin_unlock_irqrestore(&g_view_lock, flags);
    if (status != MAPPING_OK) { leave(saved); return status; }
    if (!paging_owned_remove(view.first_page, view.range.first, view.range.pages, view.flags) ||
        !flush(DEVICE_WINDOW_BASE + view.first_page * PAGE_SIZE, view.range.pages * PAGE_SIZE))
        status = MAPPING_QUARANTINED;
    flags = spin_lock_irqsave(&g_view_lock);
    if (status == MAPPING_OK) {
#ifdef HOBBYOS_SELFTEST
        if (g_probe_reservation.generation == handle.generation && g_probe_reservation.slot == handle.slot)
            g_views[handle.slot].state = VIEW_TEST_RETIRED;
        else
#endif
        {
            (void)bitmap_clear_range(&g_window_bitmap, view.first_page, view.range.pages);
            g_views[handle.slot].state = VIEW_FREE;
        }
    } else g_views[handle.slot].state = VIEW_QUARANTINED;
    spin_unlock_irqrestore(&g_view_lock, flags);
    leave(saved);
    return status;
}

bool owned_mapping_subview(mapping_handle_t handle, size_t offset, size_t bytes, void **cpu)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_MAPPING);
    if (!cpu || !bytes || !handle.generation || handle.domain != 1 || handle.slot >= OWNED_MAPPING_CAPACITY) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_view_lock);
    const owned_view_t *v = &g_views[handle.slot];
    bool ok = v->state == VIEW_LIVE && v->view.handle.generation == handle.generation &&
              offset < v->view.bytes && bytes <= v->view.bytes - offset;
    if (ok) *cpu = (uint8_t *)v->view.cpu + offset;
    spin_unlock_irqrestore(&g_view_lock, flags);
    return ok;
}

bool owned_mapping_physical(const void *cpu, size_t bytes, uint64_t *physical)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_MAPPING);
    if (!cpu || !bytes || !physical) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_view_lock);
    bool ok = false;
    for (unsigned i = 0; i < OWNED_MAPPING_CAPACITY; ++i) {
        const owned_view_t *v = &g_views[i];
        uint64_t offset = (uintptr_t)cpu - (uintptr_t)v->view.cpu;
        if (v->state == VIEW_LIVE && (uintptr_t)cpu >= (uintptr_t)v->view.cpu &&
            offset < v->view.bytes && bytes <= v->view.bytes - offset) {
            *physical = v->view.physical + offset; ok = true; break;
        }
    }
    spin_unlock_irqrestore(&g_view_lock, flags);
    return ok;
}

bool owned_mapping_device_view(uint64_t physical, size_t bytes, mapping_view_t *out)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_MAPPING);
    if (!out || !bytes) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_view_lock);
    bool ok = false;
    for (unsigned i = 0; i < OWNED_MAPPING_CAPACITY; ++i) {
        const owned_view_t *v = &g_views[i];
        uint64_t offset = physical - v->view.physical;
        if (v->state == VIEW_LIVE && v->origin && physical >= v->view.physical &&
            offset < v->view.bytes && bytes <= v->view.bytes - offset) {
            *out = v->view;
            out->cpu = (uint8_t *)v->view.cpu + offset;
            out->physical = physical; out->bytes = bytes; ok = true; break;
        }
    }
    spin_unlock_irqrestore(&g_view_lock, flags);
    return ok;
}

#ifdef HOBBYOS_SELFTEST
mapping_status_t owned_mapping_stress_buffer(const dma_buffer_t *buffer, mapping_view_t *out)
{
    dma_buffer_t actual; uint64_t address;
    if (!buffer || !out || !buffer->cpu || !buffer->requested_bytes ||
        !dma_buffer_lookup(buffer->cpu, buffer->requested_bytes, &actual, &address) ||
        actual.handle.domain != buffer->handle.domain ||
        actual.handle.generation != buffer->handle.generation || actual.handle.slot != buffer->handle.slot ||
        address != buffer->device_address) return MAPPING_INVALID;
    return acquire(address, buffer->requested_bytes, PAGE_PRESENT|PAGE_RW,
                   CACHE_WRITE_BACK, 0, address, out);
}

mapping_status_t owned_mapping_test_buffer(const dma_buffer_t *buffer, mapping_view_t *out)
{
    dma_buffer_t actual; uint64_t address;
    if (!buffer || !out || !buffer->cpu || !buffer->requested_bytes || g_probe_reservation.generation ||
        !dma_buffer_lookup(buffer->cpu, buffer->requested_bytes, &actual, &address) ||
        actual.handle.generation != buffer->handle.generation || actual.handle.slot != buffer->handle.slot ||
        address != buffer->device_address) return MAPPING_INVALID;
    mapping_view_t view;
    mapping_status_t status=acquire(address, buffer->requested_bytes, PAGE_PRESENT|PAGE_RW,
                                    CACHE_WRITE_BACK, 0, address, &view);
    if(status==MAPPING_OK) { g_probe_reservation=view.handle; *out=view; }
    return status;
}
bool owned_mapping_test_finish(mapping_handle_t handle)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_MAPPING);
    irq_flags_t flags=spin_lock_irqsave(&g_view_lock);
    bool ok=handle.domain==1 && handle.slot<OWNED_MAPPING_CAPACITY &&
        handle.generation && g_probe_reservation.generation==handle.generation &&
        g_probe_reservation.slot==handle.slot && g_views[handle.slot].state==VIEW_TEST_RETIRED;
    if(ok) {
        owned_view_t *v=&g_views[handle.slot];
        (void)bitmap_clear_range(&g_window_bitmap,v->first_page,v->range.pages);
        v->state=VIEW_FREE; g_probe_reservation=(mapping_handle_t){0};
    }
    spin_unlock_irqrestore(&g_view_lock,flags); return ok;
}
#endif
