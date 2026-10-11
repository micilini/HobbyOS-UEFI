#include "../core/irq_latency.h"
#include "pmem.h"
#include "ram_map_plan.h"
#include "boot_address.h"
#include "bootstrap_claim.h"
#include "../utils/bitmap.h"
#include "../libc/memory.h"
#include "../libc/string.h"
#include "../core/panic.h"
#include "../core/spinlock.h"
#include "../drivers/serial.h"
#include "../utils/bits.h"
#include "physical_translation.h"
#include "../smp/cpu_limits.h"

extern uint64_t _kernel_start;
extern uint64_t _kernel_end;

static uint64_t g_min_alloc_frame = 0;
static uint64_t g_alloc_limit_frames;
static uint64_t g_bitmap_physical;

static Bitmap g_bitmap;
static Bitmap g_reserved_bitmap;
static uint64_t g_total_frames = 0;
static uint64_t g_free_frames = 0;
static size_t g_bitmap_size = 0;
static size_t g_bitmap_storage_size = 0;
static void *g_bitmap_buffer = NULL;
static void *g_reserved_bitmap_buffer = NULL;
static PmmHardeningStats g_pmm_hardening;

static spinlock_t g_pmm_lock;
static bootstrap_claim_t g_bootstrap_claim;

#define PMM_OBSERVATION_SLOTS (2u * HOBBYOS_MAX_CPUS + 1u)
#define PMM_OBSERVATION_FRAMES 4096u
typedef struct {
    Bitmap allocated, reserved;
    size_t cursor, free, invalid, below, low;
    uint64_t minimum, limit;
    bool active, valid, reachability;
} pmm_observation_t;
static pmm_observation_t g_pmm_observations[PMM_OBSERVATION_SLOTS];
static unsigned g_pmm_observation_count;

static bool observation_dimensions(const pmm_observation_t *o)
{
    return g_total_frames == o->allocated.total_bits && g_bitmap_size == o->allocated.size &&
        g_bitmap.buffer == o->allocated.buffer && g_bitmap.size == o->allocated.size &&
        g_bitmap.total_bits == o->allocated.total_bits &&
        g_reserved_bitmap.buffer == o->reserved.buffer && g_reserved_bitmap.size == o->reserved.size &&
        g_reserved_bitmap.total_bits == o->reserved.total_bits &&
        g_min_alloc_frame == o->minimum && g_alloc_limit_frames == o->limit;
}

static bool observation_adjust(size_t *value, size_t before, size_t after, size_t total)
{
    if (*value > total || *value < before || after > total - (*value - before)) return false;
    *value = *value - before + after; return true;
}

/* Every bitmap writer calls this under g_pmm_lock before its atomic logical update.
 * An observed prefix therefore remains an exact count of its current contents. */
static void observation_change(size_t start, size_t count, bool allocated)
{
    if (!g_pmm_observation_count) return;
    for (unsigned i = 0; i < PMM_OBSERVATION_SLOTS; ++i) {
        pmm_observation_t *o = &g_pmm_observations[i];
        if (!o->active || !o->valid) continue;
        if (!observation_dimensions(o)) { o->valid = false; continue; }
        if (start >= o->cursor) continue;
        size_t width = o->cursor - start;
        if (width > count) width = count;
        size_t free, bad;
        if (!bitmap_observe_allocation(&g_bitmap,&g_reserved_bitmap,start,width,&free,&bad) ||
            !observation_adjust(&o->free,free,allocated ? 0 : width,g_total_frames) ||
            !observation_adjust(&o->invalid,bad,0,g_total_frames)) { o->valid = false; continue; }
        if (!o->reachability) continue;
        uint64_t minimum = o->minimum ? o->minimum : 256;
        uint64_t limits[2] = {o->limit, minimum < o->limit ? minimum : o->limit};
        size_t *counts[2] = {&o->below,&o->low};
        for (unsigned n = 0; n < 2; ++n) {
            if (start >= limits[n]) continue;
            size_t part = width;
            if (part > limits[n] - start) part = limits[n] - start;
            size_t old = free;
            if (part != width && !bitmap_observe_allocation(&g_bitmap,&g_reserved_bitmap,start,part,&old,&bad))
                o->valid = false;
            if (!observation_adjust(counts[n],old,allocated ? 0 : part,g_total_frames)) o->valid = false;
        }
    }
}

/* Each chunk and every writer share the same lock. At the final lock acquisition,
 * the maintained complete prefix and free counter describe that exact state. */
static bool observation_run(bool reachability, PmmReachability *out, irq_flags_t flags)
{
    unsigned slot = 0;
    while (slot < PMM_OBSERVATION_SLOTS && g_pmm_observations[slot].active) ++slot;
    if (slot == PMM_OBSERVATION_SLOTS) return false;
    pmm_observation_t *o = &g_pmm_observations[slot];
    *o = (pmm_observation_t){.allocated=g_bitmap,.reserved=g_reserved_bitmap,
        .minimum=g_min_alloc_frame,.limit=g_alloc_limit_frames,.active=true,.valid=true,.reachability=reachability};
    ++g_pmm_observation_count;
    for (;;) {
        if (!observation_dimensions(o)) o->valid = false;
        size_t remaining = o->allocated.total_bits - o->cursor;
        size_t width = remaining < PMM_OBSERVATION_FRAMES ? remaining : PMM_OBSERVATION_FRAMES;
        size_t free, bad;
        if (o->valid && bitmap_observe_allocation(&g_bitmap,&g_reserved_bitmap,o->cursor,width,&free,&bad)) {
            o->free += free; o->invalid += bad;
            if (reachability) {
                uint64_t minimum = o->minimum ? o->minimum : 256;
                uint64_t limits[2] = {o->limit, minimum < o->limit ? minimum : o->limit};
                size_t *counts[2] = {&o->below,&o->low};
                for (unsigned n=0;n<2;++n) if (o->cursor < limits[n]) {
                    size_t part = width;
                    if (part > limits[n] - o->cursor) part = limits[n] - o->cursor;
                    size_t value = free;
                    if (part != width && !bitmap_observe_allocation(&g_bitmap,&g_reserved_bitmap,o->cursor,part,&value,&bad))
                        o->valid = false;
                    *counts[n] += value;
                }
            }
            o->cursor += width;
        } else o->valid = false;
        if (!o->valid || o->cursor == o->allocated.total_bits) break;
        spin_unlock_irqrestore(&g_pmm_lock,flags);
        flags = spin_lock_irqsave(&g_pmm_lock);
    }
    bool valid = o->valid && !o->invalid && o->free == g_free_frames;
    if (reachability) {
        uint64_t minimum = o->minimum ? o->minimum : 256;
        valid = valid && o->limit <= g_total_frames && o->limit <= UINT64_MAX / PAGE_SIZE &&
            physical_allocation_ceiling() == o->limit * PAGE_SIZE && o->low <= o->below && o->below <= o->free;
        if (valid) *out = (PmmReachability){g_total_frames,g_free_frames,o->below-o->low,
            o->free-o->below,o->limit*PAGE_SIZE,minimum*PAGE_SIZE,g_bitmap_physical,g_bitmap_storage_size};
    }
    o->active = false; --g_pmm_observation_count;
    return valid;
}

#ifndef PMM_BITMAP_MAX_PHYS
#define PMM_BITMAP_MAX_PHYS 0x40000000ULL
#endif

static void pmem_write_formatted(const char *line, int required, size_t size)
{
    if (required < 0 || (size_t)required >= size)
        serial_write_all("[PMM] FORMAT_ERROR\n");
    else
        serial_write_all(line);
}

static inline bool is_ram_type(uint32_t type)
{
    switch (type)
    {
    case EFI_LOADER_CODE:
    case EFI_LOADER_DATA:
    case EFI_BOOT_SERVICES_CODE:
    case EFI_BOOT_SERVICES_DATA:
    case EFI_RUNTIME_SERVICES_CODE:
    case EFI_RUNTIME_SERVICES_DATA:
    case EFI_CONVENTIONAL_MEMORY:
    case EFI_ACPI_RECLAIM_MEMORY:
    case EFI_ACPI_MEMORY_NVS:
    case EFI_RESERVED_MEMORY_TYPE:
        return true;
    default:
        return false;
    }
}

static void pmm_reject(uint64_t *counter)
{
    __atomic_add_fetch(counter, 1u, __ATOMIC_RELAXED);
    KWARN_ON(true);
}

static void pmm_record_search(const BitmapSearchStats *stats)
{
#if HOBBYOS_DEBUG_ASSERT
    __atomic_add_fetch(&g_pmm_hardening.word_search_calls, 1u,
                       __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_pmm_hardening.word_iterations,
                       stats ? stats->words_examined : 0u,
                       __ATOMIC_RELAXED);
#else
    (void)stats;
#endif
}

static uint64_t get_highest_ram_end(MemoryMap *map)
{
    uint64_t highest_addr = 0;
    uint64_t entries = mmap_get_entry_count(map);

    for (uint64_t i = 0; i < entries; i++)
    {
        EfiMemoryDescriptor *desc = mmap_get_descriptor(map, i);

        if (!is_ram_type(desc->Type))
            continue;
        if (desc->Type == EFI_MEMORY_MAPPED_IO)
            continue;

        if (desc->NumberOfPages > UINT64_MAX / PAGE_SIZE)
            continue;
        uint64_t bytes = desc->NumberOfPages * PAGE_SIZE;
        if (desc->PhysicalStart > UINT64_MAX - bytes)
            continue;
        uint64_t end_addr = desc->PhysicalStart + bytes;
        if (end_addr > highest_addr)
            highest_addr = end_addr;
    }

    return highest_addr;
}

static void *find_free_segment_for_bitmap(MemoryMap *map, size_t size_needed)
{
    uint64_t entries = mmap_get_entry_count(map);
    void *best_addr = NULL;
    uint64_t best_size = 0;
    uint64_t max_phys = PMM_BITMAP_MAX_PHYS;
    if (physical_allocation_ceiling() < max_phys)
        max_phys = physical_allocation_ceiling();

    for (uint64_t i = 0; i < entries; i++)
    {
        EfiMemoryDescriptor *desc = mmap_get_descriptor(map, i);

        if (desc->Type != EFI_CONVENTIONAL_MEMORY)
            continue;

        uint64_t seg_start = desc->PhysicalStart;
        if (desc->NumberOfPages > UINT64_MAX / PAGE_SIZE)
            continue;
        uint64_t seg_size = desc->NumberOfPages * PAGE_SIZE;

        if (seg_size < size_needed)
            continue;
        if (seg_start >= max_phys)
            continue;
        if (seg_start > UINT64_MAX - size_needed ||
            seg_start + size_needed > max_phys)
            continue;

        if (seg_size > best_size)
        {
            best_size = seg_size;
            best_addr = (void *)seg_start;
        }
    }

    return best_addr;
}

static void pmm_reserve_range(uint64_t start_frame, uint64_t count)
{
    if (start_frame >= g_total_frames)
        return;
    if (count > g_total_frames - start_frame)
        count = g_total_frames - start_frame;

    observation_change((size_t)start_frame,(size_t)count,true);

    for (uint64_t frame = start_frame; frame < start_frame + count; frame++)
    {
        if (!bitmap_get(&g_bitmap, (size_t)frame))
            g_free_frames--;
    }
    (void)bitmap_set_range(&g_bitmap, (size_t)start_frame, (size_t)count);
    (void)bitmap_set_range(&g_reserved_bitmap, (size_t)start_frame,
                           (size_t)count);
}

void init_pmm(MemoryMap *map)
{
    serial_write_all("[PMM] Initializing...\n");
    spinlock_init(&g_pmm_lock);

    uint64_t highest_ram_end = get_highest_ram_end(map);
    g_total_frames = highest_ram_end / PAGE_SIZE;
    uint64_t ceiling = physical_allocation_ceiling();
    if (!ceiling || (ceiling & (PAGE_SIZE - 1)) || ceiling > highest_ram_end)
        kpanic("PMM reachability contract is unavailable");
    g_alloc_limit_frames = ceiling / PAGE_SIZE;
    if (g_total_frames > SIZE_MAX ||
        !bitmap_required_bytes((size_t)g_total_frames, &g_bitmap_size) ||
        g_bitmap_size > SIZE_MAX / 2u ||
        !align_up_size_checked(g_bitmap_size * 2u, PAGE_SIZE,
                               &g_bitmap_storage_size))
    {
        serial_write_all("[PMM] CRITICAL: Bitmap size overflow!\n");
        while (1)
            ;
    }

    char diagnostic[192];
    int required = ksnprintf(diagnostic, sizeof(diagnostic),
                             "[PMM] Highest RAM: 0x%016llX | "
                             "Total Frames: 0x%016llX\n",
                             (unsigned long long)highest_ram_end,
                             (unsigned long long)g_total_frames);
    pmem_write_formatted(diagnostic, required, sizeof(diagnostic));

    g_bitmap_physical = (uintptr_t)find_free_segment_for_bitmap(map, g_bitmap_storage_size);
    const boot_physical_objects_t *boot_objects = physical_boot_objects();
    uint64_t framebuffer_end;
    if (!boot_objects || !physical_range_valid(boot_objects->framebuffer,
        boot_objects->framebuffer_bytes, &framebuffer_end) ||
        (g_bitmap_physical < framebuffer_end && boot_objects->framebuffer <
         g_bitmap_physical + g_bitmap_storage_size))
        kpanic("PMM bitmap placement conflicts with the boot framebuffer");
    if (!g_bitmap_physical || !physical_pointer_range(g_bitmap_physical,
            g_bitmap_storage_size, PHYSICAL_RAM, &g_bitmap_buffer))
    {
        serial_write_all("[PMM] CRITICAL: Failed to allocate bitmap buffer!\n");
        while (1)
            ;
    }

    g_reserved_bitmap_buffer = (uint8_t *)g_bitmap_buffer + g_bitmap_size;
    if (!bitmap_init_checked(&g_bitmap, g_bitmap_buffer,
                             (size_t)g_total_frames) ||
        !bitmap_init_checked(&g_reserved_bitmap, g_reserved_bitmap_buffer,
                             (size_t)g_total_frames))
    {
        serial_write_all("[PMM] CRITICAL: Bitmap initialization failed!\n");
        while (1)
            ;
    }
    memset(g_bitmap.buffer, 0xFF, g_bitmap.size);
    memset(g_reserved_bitmap.buffer, 0xFF, g_reserved_bitmap.size);
    g_free_frames = 0;

    uint64_t entries = mmap_get_entry_count(map);
    for (uint64_t i = 0; i < entries; i++)
    {
        EfiMemoryDescriptor *desc = mmap_get_descriptor(map, i);
        if (desc->Type == EFI_CONVENTIONAL_MEMORY)
        {
            uint64_t start_frame = desc->PhysicalStart / PAGE_SIZE;
            uint64_t num_frames = desc->NumberOfPages;
            if (start_frame >= g_total_frames)
                continue;
            if (num_frames > g_total_frames - start_frame)
                num_frames = g_total_frames - start_frame;
            (void)bitmap_clear_range(&g_bitmap, (size_t)start_frame,
                                     (size_t)num_frames);
            (void)bitmap_clear_range(&g_reserved_bitmap,
                                     (size_t)start_frame,
                                     (size_t)num_frames);
            g_free_frames += num_frames;
        }
    }

    uint64_t bitmap_start_frame = g_bitmap_physical / PAGE_SIZE;
    uint64_t bitmap_pages = g_bitmap_storage_size / PAGE_SIZE;
    pmm_reserve_range(bitmap_start_frame, bitmap_pages);

    const uint64_t low_limit = 0x100000ULL;
    const uint64_t low_frames = low_limit / PAGE_SIZE;

    pmm_reserve_range(0, low_frames);

    g_min_alloc_frame = low_frames;
    required = ksnprintf(diagnostic, sizeof(diagnostic),
                         "[PMM] min_alloc_frame set to 1MB "
                         "(frame=0x%016llX)\n",
                         (unsigned long long)g_min_alloc_frame);
    pmem_write_formatted(diagnostic, required, sizeof(diagnostic));

    uint64_t kstart_phys;
    size_t image_bytes;
    if (!physical_image_extent(&kstart_phys, &image_bytes))
        kpanic("PMM image physical extent is unavailable");
    uint64_t kend_phys = kstart_phys + image_bytes;

    kstart_phys &= ~(PAGE_SIZE - 1);
    if (!align_up_u64_checked(kend_phys, PAGE_SIZE, &kend_phys))
    {
        serial_write_all("[PMM] CRITICAL: Kernel range overflow!\n");
        while (1)
            ;
    }

    uint64_t kstart_frame = kstart_phys / PAGE_SIZE;
    uint64_t kend_frame = kend_phys / PAGE_SIZE;

    pmm_reserve_range(kstart_frame, kend_frame - kstart_frame);

    required = ksnprintf(diagnostic, sizeof(diagnostic),
                         "[PMM] Reserved LOW<1MB and KERNEL frames. "
                         "Kernel phys=[0x0x%016llX..0x0x%016llX)\n",
                         (unsigned long long)kstart_phys,
                         (unsigned long long)kend_phys);
    pmem_write_formatted(diagnostic, required, sizeof(diagnostic));

    required = ksnprintf(diagnostic, sizeof(diagnostic),
                         "[PMM] Init Done. Free Frames: 0x%016llX\n",
                         (unsigned long long)g_free_frames);
    pmem_write_formatted(diagnostic, required, sizeof(diagnostic));
}

void *pmm_alloc_frame(void)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_PMM);
    irq_flags_t flags = spin_lock_irqsave(&g_pmm_lock);

    uint64_t start = g_min_alloc_frame;
    if (start == 0)
        start = (0x100000ULL / PAGE_SIZE);

    size_t frame = 0;
    BitmapSearchStats search = {0};
    bool found = g_alloc_limit_frames <= g_total_frames &&
                 g_alloc_limit_frames <= UINT64_MAX / PAGE_SIZE &&
                 physical_allocation_ceiling() == g_alloc_limit_frames * PAGE_SIZE &&
                 start < g_alloc_limit_frames &&
                 bitmap_find_zero_run_below(&g_bitmap, (size_t)start,
                     (size_t)g_alloc_limit_frames, 1, &frame, &search);
    pmm_record_search(&search);
    if (found)
    {
        observation_change(frame,1,true);
        bitmap_set(&g_bitmap, frame, true);
        g_free_frames--;

        spin_unlock_irqrestore(&g_pmm_lock, flags);
        return (void *)((uintptr_t)frame * PAGE_SIZE);
    }

    spin_unlock_irqrestore(&g_pmm_lock, flags);
    serial_write_all("[PMM] ERROR: Out of Memory (alloc_frame)!\n");
    return NULL;
}

void *pmm_alloc_contiguous_frames(size_t count)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_PMM);
    if (count == 0)
        return NULL;

    irq_flags_t flags = spin_lock_irqsave(&g_pmm_lock);

    uint64_t start = g_min_alloc_frame;
    if (start == 0)
        start = (0x100000ULL / PAGE_SIZE);

    size_t run_start = 0;
    BitmapSearchStats search = {0};
    bool found = g_alloc_limit_frames <= g_total_frames &&
                 g_alloc_limit_frames <= UINT64_MAX / PAGE_SIZE &&
                 physical_allocation_ceiling() == g_alloc_limit_frames * PAGE_SIZE &&
                 start < g_alloc_limit_frames && count <= g_alloc_limit_frames - start &&
                 bitmap_find_zero_run_below(&g_bitmap, (size_t)start,
                     (size_t)g_alloc_limit_frames, count, &run_start, &search);
    pmm_record_search(&search);
    if (found)
    {
        observation_change(run_start,count,true);
        (void)bitmap_set_range(&g_bitmap, run_start, count);
        g_free_frames -= count;

        spin_unlock_irqrestore(&g_pmm_lock, flags);
        return (void *)((uintptr_t)run_start * PAGE_SIZE);
    }

    spin_unlock_irqrestore(&g_pmm_lock, flags);
    serial_write_all("[PMM] ERROR: Out of Contiguous Memory!\n");
    return NULL;
}

void pmm_free_frame(void *paddr)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_PMM);
    uint64_t *reject_counter = NULL;
    irq_flags_t flags = spin_lock_irqsave(&g_pmm_lock);

    uintptr_t address = (uintptr_t)paddr;
    if (!paddr)
        reject_counter = &g_pmm_hardening.reject_null;
    else if ((address & (PAGE_SIZE - 1u)) != 0)
        reject_counter = &g_pmm_hardening.reject_unaligned;
    else
    {
        uint64_t frame = (uint64_t)(address / PAGE_SIZE);
        if (frame >= g_total_frames)
            reject_counter = &g_pmm_hardening.reject_out_of_span;
        else if (bitmap_get(&g_reserved_bitmap, (size_t)frame))
            reject_counter = &g_pmm_hardening.reject_reserved;
        else if (!bitmap_get(&g_bitmap, (size_t)frame))
            reject_counter = &g_pmm_hardening.reject_already_free;
        else
        {
            observation_change((size_t)frame,1,false);
            bitmap_set(&g_bitmap, (size_t)frame, false);
            g_free_frames++;
        }
    }

    spin_unlock_irqrestore(&g_pmm_lock, flags);
    if (reject_counter)
        pmm_reject(reject_counter);
}

bool pmm_is_frame_free(uint64_t physical_address)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_PMM);
    irq_flags_t flags = spin_lock_irqsave(&g_pmm_lock);

    uint64_t frame = physical_address / PAGE_SIZE;

    if (frame >= g_total_frames)
    {
        spin_unlock_irqrestore(&g_pmm_lock, flags);
        return false;
    }

    bool is_free = !bitmap_get(&g_bitmap, (size_t)frame);

    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return is_free;
}

void pmm_mark_frame_used(uint64_t physical_address)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_PMM);
    irq_flags_t flags = spin_lock_irqsave(&g_pmm_lock);

    uint64_t frame = physical_address / PAGE_SIZE;

    if (frame < g_total_frames)
    {

        if (!bitmap_get(&g_bitmap, (size_t)frame))
        {
            observation_change((size_t)frame,1,true);
            bitmap_set(&g_bitmap, (size_t)frame, true);
            g_free_frames--;

            char diagnostic[96];
            int required = ksnprintf(
                diagnostic, sizeof(diagnostic),
                "[PMM] Explicitly marked frame used: 0x%016llX\n",
                (unsigned long long)physical_address);
            pmem_write_formatted(diagnostic, required, sizeof(diagnostic));
        }
        else
        {
            char diagnostic[96];
            int required = ksnprintf(
                diagnostic, sizeof(diagnostic),
                "[PMM] Warning: Frame already used: 0x%016llX\n",
                (unsigned long long)physical_address);
            pmem_write_formatted(diagnostic, required, sizeof(diagnostic));
        }
    }

    spin_unlock_irqrestore(&g_pmm_lock, flags);
}

uint64_t pmm_get_free_memory(void)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_PMM);
    irq_flags_t flags = spin_lock_irqsave(&g_pmm_lock);
    uint64_t mem = g_free_frames * PAGE_SIZE;
    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return mem;
}

uint64_t pmm_get_total_memory(void)
{
    return g_total_frames * PAGE_SIZE;
}

bool pmm_get_hardening_stats(PmmHardeningStats *out_stats)
{
    if (!out_stats)
        return false;

    out_stats->reject_null = __atomic_load_n(
        &g_pmm_hardening.reject_null, __ATOMIC_RELAXED);
    out_stats->reject_unaligned = __atomic_load_n(
        &g_pmm_hardening.reject_unaligned, __ATOMIC_RELAXED);
    out_stats->reject_out_of_span = __atomic_load_n(
        &g_pmm_hardening.reject_out_of_span, __ATOMIC_RELAXED);
    out_stats->reject_reserved = __atomic_load_n(
        &g_pmm_hardening.reject_reserved, __ATOMIC_RELAXED);
    out_stats->reject_already_free = __atomic_load_n(
        &g_pmm_hardening.reject_already_free, __ATOMIC_RELAXED);
    out_stats->word_search_calls = __atomic_load_n(
        &g_pmm_hardening.word_search_calls, __ATOMIC_RELAXED);
    out_stats->word_iterations = __atomic_load_n(
        &g_pmm_hardening.word_iterations, __ATOMIC_RELAXED);
    return true;
}

bool pmm_get_reachability(PmmReachability *out)
{
    IRQ_LATENCY_SCOPE_MASK((UINT64_C(1) << IRQ_PATH_PMM) | (UINT64_C(1) << IRQ_PATH_PMM_REACHABILITY));
    if (!out) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_pmm_lock);
    if ((flags & UINT64_C(512)) && g_total_frames > PMM_OBSERVATION_FRAMES &&
        g_pmm_observation_count < PMM_OBSERVATION_SLOTS) {
        bool valid = observation_run(true,out,flags);
        spin_unlock_irqrestore(&g_pmm_lock,flags);
        return valid;
    }
    size_t below = 0, low = 0;
    bool valid = g_alloc_limit_frames <= g_total_frames &&
        g_alloc_limit_frames <= g_total_frames &&
                 g_alloc_limit_frames <= UINT64_MAX / PAGE_SIZE &&
                 physical_allocation_ceiling() == g_alloc_limit_frames * PAGE_SIZE &&
        g_bitmap.total_bits == g_total_frames && g_reserved_bitmap.total_bits == g_total_frames &&
        g_bitmap.size == g_bitmap_size && g_reserved_bitmap.size == g_bitmap_size;
    uint64_t minimum = g_min_alloc_frame ? g_min_alloc_frame : 256;
    if (valid) {
        size_t limits[2] = {(size_t)g_alloc_limit_frames,
            (size_t)(minimum < g_alloc_limit_frames ? minimum : g_alloc_limit_frames)};
        size_t *counts[2] = {&below, &low};
        for (unsigned i = 0; i < 2; i++) {
            Bitmap a = g_bitmap, r = g_reserved_bitmap;
            a.total_bits = r.total_bits = limits[i];
            a.size = r.size = limits[i] / 8 + (limits[i] % 8 != 0);
            valid = valid && bitmap_validate_allocation(&a, &r, counts[i], NULL);
        }
    }
    valid = valid && low <= below && below <= g_free_frames;
    if (valid) *out = (PmmReachability){g_total_frames, g_free_frames, below-low,
        g_free_frames-below, g_alloc_limit_frames*PAGE_SIZE, minimum*PAGE_SIZE,
        g_bitmap_physical, g_bitmap_storage_size};
    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return valid;
}

bool pmm_validate_integrity(void)
{
    IRQ_LATENCY_SCOPE_MASK((UINT64_C(1) << IRQ_PATH_PMM) | (UINT64_C(1) << IRQ_PATH_PMM_INTEGRITY));
    irq_flags_t flags = spin_lock_irqsave(&g_pmm_lock);
    if ((flags & UINT64_C(512)) && g_total_frames > PMM_OBSERVATION_FRAMES &&
        g_pmm_observation_count < PMM_OBSERVATION_SLOTS) {
        bool valid = observation_run(false,NULL,flags);
        spin_unlock_irqrestore(&g_pmm_lock,flags);
        return valid;
    }
    size_t free_frames = 0;
    bool valid = g_total_frames <= SIZE_MAX &&
                 g_bitmap.total_bits == (size_t)g_total_frames &&
                 g_reserved_bitmap.total_bits == (size_t)g_total_frames &&
                 g_bitmap.size == g_reserved_bitmap.size &&
                 g_bitmap.size == g_bitmap_size;

    if (valid)
        valid = bitmap_validate_allocation(&g_bitmap, &g_reserved_bitmap,
                                            &free_frames, NULL);
    valid = valid && free_frames == g_free_frames;
    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return valid;
}

#ifdef HOBBYOS_SELFTEST
void *pmm_test_reserved_frame(void)
{
    return (void *)(uintptr_t)g_bitmap_physical;
}
#endif

static bool allocate_range(size_t count, size_t alignment_frames,
                            uint64_t minimum, uint64_t limit, uint64_t *physical)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_PMM);
    if (!physical || !count || !alignment_frames ||
        (alignment_frames & (alignment_frames - 1)) ||
        ((minimum | limit) & (PAGE_SIZE - 1)) || minimum >= limit) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_pmm_lock);
    uint64_t start = g_min_alloc_frame ? g_min_alloc_frame : 256;
    if (minimum / PAGE_SIZE > start) start = minimum / PAGE_SIZE;
    uint64_t bound = limit / PAGE_SIZE;
    if (bound > g_alloc_limit_frames) bound = g_alloc_limit_frames;
    size_t frame = 0;
    BitmapSearchStats search = {0};
    bool found = g_alloc_limit_frames <= g_total_frames &&
        g_alloc_limit_frames <= UINT64_MAX / PAGE_SIZE &&
        physical_allocation_ceiling() == g_alloc_limit_frames * PAGE_SIZE &&
        count <= g_free_frames && bound <= SIZE_MAX &&
        bitmap_find_aligned_run_below(&g_bitmap, start, bound, count,
                                      alignment_frames, &frame, &search);
    pmm_record_search(&search);
    if (found) {
        observation_change(frame,count,true);
        found = bitmap_set_range(&g_bitmap, frame, count);
        if (found) { g_free_frames -= count; *physical = (uint64_t)frame * PAGE_SIZE; }
    }
    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return found;
}

bool pmm_alloc_constrained_frames(size_t count, size_t alignment_frames,
                                  uint64_t limit, uint64_t *physical)
{ return allocate_range(count, alignment_frames, 0, limit, physical); }

#ifdef HOBBYOS_SELFTEST
bool pmm_test_alloc_range(size_t count, size_t alignment_frames,
                          uint64_t minimum, uint64_t limit, uint64_t *physical)
{ return allocate_range(count, alignment_frames, minimum, limit, physical); }
#endif

bool pmm_direct_map_ready(const ram_map_plan_t *plan)
{
    return plan && plan->ready && !g_physical_contract.direct_map &&
        plan->ceiling / PAGE_SIZE <= g_total_frames && !(plan->ceiling & (PAGE_SIZE-1)) &&
        g_bitmap.buffer == g_bitmap_buffer && g_reserved_bitmap.buffer == g_reserved_bitmap_buffer &&
        g_reserved_bitmap_buffer == (uint8_t *)g_bitmap_buffer + g_bitmap_size &&
        ram_map_contains(plan, g_bitmap_physical, g_bitmap_storage_size);
}

bool pmm_reserve_boot_framebuffer(void)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_PMM);
    const boot_physical_objects_t *boot = physical_boot_objects();
    uint64_t end;
    if (!boot || g_physical_contract.direct_map ||
        !physical_range_valid(boot->framebuffer, boot->framebuffer_bytes, &end) ||
        end > PHYSICAL_ADDRESS_LIMIT - 4095) return false;
    uint64_t first = boot->framebuffer & ~UINT64_C(4095);
    end = (end + 4095) & ~UINT64_C(4095);
    if (first < g_bitmap_physical + g_bitmap_storage_size && g_bitmap_physical < end) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_pmm_lock);
    pmm_reserve_range(first/PAGE_SIZE, (end-first)/PAGE_SIZE);
    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return true;
}

bool pmm_claim_bootstrap(bootstrap_claim_t *out)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_PMM);
    if (!out) return false;
    const boot_physical_objects_t *boot=physical_boot_objects();
    irq_flags_t flags=spin_lock_irqsave(&g_pmm_lock);
    bool claimed=false;
    if (g_bootstrap_claim.owner) {
        bootstrap_claim_t candidate;
        size_t frame=g_bootstrap_claim.physical/PAGE_SIZE;
        if (frame<g_total_frames && bitmap_get(&g_bitmap,frame) && bitmap_get(&g_reserved_bitmap,frame) &&
            bootstrap_claim_candidate(&g_physical_contract,boot,g_bitmap_physical,g_bitmap_storage_size,
                g_bootstrap_claim.physical,&candidate) && bootstrap_claim_adopt(&g_bootstrap_claim,&candidate)) {
            *out=g_bootstrap_claim; claimed=true;
        }
    } else for (uint64_t physical=4096; physical<0xa0000; physical+=4096) {
        bootstrap_claim_t candidate;
        size_t frame=physical/PAGE_SIZE;
        if (frame>=g_total_frames || !bitmap_get(&g_bitmap,frame) || !bitmap_get(&g_reserved_bitmap,frame) ||
            !bootstrap_claim_candidate(&g_physical_contract,boot,g_bitmap_physical,g_bitmap_storage_size,physical,&candidate)) continue;
        if (!bootstrap_claim_adopt(&g_bootstrap_claim,&candidate)) break;
        *out=g_bootstrap_claim; claimed=true; break;
    }
    /* Adoption does not change either reservation bitmap or the free count. */
    spin_unlock_irqrestore(&g_pmm_lock,flags);
    return claimed;
}

bool pmm_direct_map_activate(const ram_map_plan_t *plan)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_PMM);
    if (!pmm_direct_map_ready(plan)) return false;
    uint64_t address;
    if (!ram_map_pointer(plan, g_bitmap_physical, g_bitmap_storage_size, &address)) return false;
    irq_flags_t flags = spin_lock_irqsave(&g_pmm_lock);
    /* No allocation, validation walk, callback or interrupt can observe an
     * intermediate pointer/ceiling pair during this boot-only publication. */
    if (!physical_translation_activate_direct(plan)) {
        spin_unlock_irqrestore(&g_pmm_lock, flags); return false;
    }
    g_bitmap_buffer = (void *)(uintptr_t)address;
    g_reserved_bitmap_buffer = (uint8_t *)g_bitmap_buffer + g_bitmap_size;
    g_bitmap.buffer = g_bitmap_buffer;
    g_reserved_bitmap.buffer = g_reserved_bitmap_buffer;
    g_alloc_limit_frames = plan->ceiling / PAGE_SIZE;
    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return true;
}

#ifdef HOBBYOS_SELFTEST
bool pmm_pointer_snapshot(PmmPointerSnapshot *out)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_PMM);
    if (!out || !g_physical_contract.direct_map) return false;
    irq_flags_t flags=spin_lock_irqsave(&g_pmm_lock);
    *out=(PmmPointerSnapshot){g_bitmap_physical,g_bitmap_storage_size,
        (uintptr_t)g_bitmap_buffer,(uintptr_t)g_reserved_bitmap_buffer,
        (uintptr_t)g_bitmap.buffer,(uintptr_t)g_reserved_bitmap.buffer};
    spin_unlock_irqrestore(&g_pmm_lock,flags);
    return true;
}
#endif
