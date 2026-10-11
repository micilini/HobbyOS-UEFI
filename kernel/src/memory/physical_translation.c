#include "physical_internal.h"
#include "paging_private.h"
#include "owned_mapping.h"
#include "ram_map_plan.h"
#include "kernel_stack.h"
#include "../core/panic.h"

physical_contract_t g_physical_contract;
static uint64_t g_validations, g_rejected, g_device_ranges;
static bool g_normal_mapping, g_identity_broken;

static bool result(bool valid)
{
    __atomic_add_fetch(&g_validations, 1, __ATOMIC_RELAXED);
    if (!valid) __atomic_add_fetch(&g_rejected, 1, __ATOMIC_RELAXED);
    return valid;
}

bool physical_translation_publish(const MemoryMap *map, uint64_t image_first,
                                  uint64_t image_end)
{
    return physical_contract_init(&g_physical_contract, map, image_first, image_end, 1);
}

bool physical_pointer_range(uint64_t physical, size_t bytes,
                            physical_kind_t kind, void **pointer)
{
    uint64_t virtual_address;
    if (!pointer || (!physical && !g_physical_contract.direct_map) || __atomic_load_n(&g_identity_broken, __ATOMIC_ACQUIRE) ||
        (!physical_contract_translate(&g_physical_contract, physical, bytes, kind, &virtual_address) ||
         (!g_physical_contract.direct_map && !physical_identity_range_present(physical, bytes))))
        return result(false);
    *pointer = (void *)(uintptr_t)virtual_address;
    return result(true);
}

bool physical_address_range(const void *pointer, size_t bytes, uint64_t *physical)
{
    uint64_t translated;
    if (kernel_stack_bsp_guard_overlap((uintptr_t)pointer,bytes)) return result(false);
    if ((uintptr_t)pointer>=STACK_REGION_BASE && (uintptr_t)pointer<STACK_REGION_END)
        return result(!__atomic_load_n(&g_identity_broken, __ATOMIC_ACQUIRE) &&
                      kernel_stack_physical((uintptr_t)pointer,bytes,physical));
    if (pointer && (uintptr_t)pointer >= DEVICE_WINDOW_BASE &&
        (uintptr_t)pointer - DEVICE_WINDOW_BASE < DEVICE_WINDOW_BYTES)
        return result(owned_mapping_physical(pointer, bytes, physical));
    if (!pointer || !physical || __atomic_load_n(&g_identity_broken, __ATOMIC_ACQUIRE) ||
        (!physical_contract_inverse(&g_physical_contract, (uintptr_t)pointer, bytes, &translated) ||
         ((uintptr_t)pointer < g_physical_contract.identity_limit &&
          !physical_identity_range_present((uintptr_t)pointer, bytes))))
        return result(false);
    *physical = translated;
    return result(true);
}

bool physical_image_address_range(uint64_t virtual_address, size_t bytes, uint64_t *physical)
{
    const physical_contract_t *c = &g_physical_contract;
    if (!c->ready || virtual_address < c->image_first || virtual_address >= c->image_end ||
        !bytes || bytes > c->image_end - virtual_address || !physical ||
        kernel_stack_bsp_guard_overlap(virtual_address,bytes)) return result(false);
    *physical = virtual_address - KERNEL_IMAGE_OFFSET;
    return result(true);
}

bool physical_image_extent(uint64_t *physical, size_t *bytes)
{
    const physical_contract_t *c = &g_physical_contract;
    if (!physical || !bytes || !c->ready) return false;
    *physical = c->image_first - KERNEL_IMAGE_OFFSET;
    *bytes = c->image_end - c->image_first;
    return true;
}

uint64_t physical_allocation_ceiling(void)
{
    return g_physical_contract.ready && !__atomic_load_n(&g_identity_broken, __ATOMIC_ACQUIRE) ?
        g_physical_contract.allocation_ceiling : 0;
}

bool physical_translation_activate(uint64_t limit)
{
    if (!g_physical_contract.ready || g_normal_mapping || __atomic_load_n(&g_identity_broken, __ATOMIC_ACQUIRE) ||
        limit != g_physical_contract.identity_limit) return false;
    g_normal_mapping = true;
    return true;
}

bool physical_translation_activate_direct(const ram_map_plan_t *plan)
{
    if (!plan || !plan->ready || !g_physical_contract.ready || g_normal_mapping ||
        g_physical_contract.direct_map || g_physical_contract.epoch == UINT64_MAX ||
        __atomic_load_n(&g_identity_broken, __ATOMIC_ACQUIRE)) return false;
    /* BSP boot publication: interrupts are disabled, APs and heap do not yet
     * exist. The memory owner rebases all retained pointers in this interval. */
    g_physical_contract.direct_map = plan;
    g_physical_contract.allocation_ceiling = plan->ceiling;
    ++g_physical_contract.epoch;
    g_normal_mapping = true;
    return true;
}

void physical_translation_mapping(uint64_t virtual_address, uint64_t physical, uint64_t flags)
{
    if (g_physical_contract.identity_retired && virtual_address < UINT64_C(0xffff800000000000))
        __atomic_store_n(&g_identity_broken, true, __ATOMIC_RELEASE);
    if (virtual_address < g_physical_contract.identity_limit &&
        ((virtual_address & ~UINT64_C(4095)) != (physical & ~UINT64_C(4095)) || !(flags & 1)))
        __atomic_store_n(&g_identity_broken, true, __ATOMIC_RELEASE);
}

bool physical_device_pointer(uint64_t physical, size_t bytes,
                             physical_device_origin_t origin, void **pointer)
{
    uint64_t end, actual;
    if (g_physical_contract.identity_retired || !g_normal_mapping || !pointer || !physical ||
        origin < PHYSICAL_DEVICE_FRAMEBUFFER || origin > PHYSICAL_DEVICE_PCI ||
        !physical_range_valid(physical, bytes, &end) ||
        !physical_canonical_range(physical, bytes) ||
        !paging_query_range(physical, bytes, &actual) || actual != physical) return result(false);
    *pointer = (void *)(uintptr_t)physical;
    __atomic_add_fetch(&g_device_ranges, 1, __ATOMIC_RELAXED);
    return result(true);
}

void *physical_require_pointer(uint64_t physical, size_t bytes, physical_kind_t kind)
{
    void *pointer;
    if (!physical_pointer_range(physical, bytes, kind, &pointer))
        kpanic("Required physical range is not reachable");
    return pointer;
}

uint64_t physical_require_address(const void *pointer, size_t bytes)
{
    uint64_t physical;
    if (!physical_address_range(pointer, bytes, &physical))
        kpanic("Required CPU pointer has no physical boundary translation");
    return physical;
}

void physical_translation_stats(physical_translation_stats_t *out)
{
    if (!out) return;
    *out = (physical_translation_stats_t){
        .validations=__atomic_load_n(&g_validations, __ATOMIC_RELAXED),
        .rejected=__atomic_load_n(&g_rejected, __ATOMIC_RELAXED),
        .identity_limit=g_physical_contract.identity_limit,
        .allocation_ceiling=physical_allocation_ceiling(), .epoch=g_physical_contract.epoch,
        .descriptor_count=g_physical_contract.count,
        .device_ranges=__atomic_load_n(&g_device_ranges, __ATOMIC_RELAXED),
        .initialized=g_physical_contract.ready, .bootstrap_sealed=g_physical_contract.ready,
        .normal_mapping=g_normal_mapping};
}

/* Append-only non-RAM holes preserve the immutable RAM translation contract.
 * A release/acquire count publishes complete records; records never move. */
static struct { uint64_t first, end; } g_identity_holes[PHYSICAL_DEVICE_CAPACITY];
static uint32_t g_identity_hole_count;

bool physical_identity_range_present(uint64_t first, uint64_t bytes)
{
    uint64_t end;
    if (g_physical_contract.identity_retired || !physical_range_valid(first, bytes, &end)) return false;
    uint32_t count = __atomic_load_n(&g_identity_hole_count, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < count; ++i)
        if (first < g_identity_holes[i].end && g_identity_holes[i].first < end) return false;
    return true;
}

bool physical_translation_retire_identity(void)
{
    if (!g_normal_mapping || !g_physical_contract.direct_map || g_physical_contract.identity_retired ||
        __atomic_load_n(&g_identity_broken, __ATOMIC_ACQUIRE)) return false;
    g_physical_contract.identity_retired = true;
    return true;
}

bool physical_provenance_range(uint64_t first, uint64_t bytes, physical_kind_t kind)
{
    uint64_t end;
    if (!g_physical_contract.ready || !physical_range_valid(first, bytes, &end)) return false;
    uint64_t cursor = first;
    for (unsigned i = 0; i < g_physical_contract.count && cursor < end; ++i) {
        const physical_region_t *r = &g_physical_contract.regions[i];
        if (r->end <= cursor) continue;
        if (r->first > cursor || !(r->kind & kind)) return false;
        cursor = r->end < end ? r->end : end;
    }
    return cursor == end;
}

bool physical_device_extent_allowed(uint64_t first, uint64_t bytes)
{
    uint64_t end;
    if (!g_physical_contract.ready || !physical_range_valid(first, bytes, &end)) return false;
    uint64_t image = g_physical_contract.image_first - KERNEL_IMAGE_OFFSET;
    uint64_t image_end = g_physical_contract.image_end - KERNEL_IMAGE_OFFSET;
    if (first < image_end && image < end) return false;
    for (unsigned i = 0; i < g_physical_contract.count; ++i) {
        const physical_region_t *r = &g_physical_contract.regions[i];
        if (first < r->end && r->first < end && (r->kind & PHYSICAL_RAM)) {
            const ram_map_plan_t *p = g_physical_contract.direct_map;
            if (!p || first < p->framebuffer.first || end > p->framebuffer.end) return false;
        }
    }
    return true;
}

bool physical_identity_hole(uint64_t first, uint64_t bytes)
{
    if ((first & 4095) || (bytes & 4095) || !physical_device_extent_allowed(first, bytes)) return false;
    uint32_t count = __atomic_load_n(&g_identity_hole_count, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < count; ++i)
        if (first >= g_identity_holes[i].first && bytes <= g_identity_holes[i].end - first &&
            first < g_identity_holes[i].end) return true;
    if (count == PHYSICAL_DEVICE_CAPACITY) return false;
    g_identity_holes[count].first = first;
    g_identity_holes[count].end = first + bytes;
    __atomic_store_n(&g_identity_hole_count, count + 1, __ATOMIC_RELEASE);
    return true;
}
