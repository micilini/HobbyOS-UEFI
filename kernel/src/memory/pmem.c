#include "pmem.h"
#include "../utils/bitmap.h"
#include "../libc/memory.h"
#include "../libc/string.h"
#include "../core/panic.h"
#include "../core/spinlock.h"
#include "../drivers/serial.h"
#include "../utils/bits.h"

extern uint64_t _kernel_start;
extern uint64_t _kernel_end;

#define VIRT_TO_PHYS_OFFSET 0xFFFFFFFF80000000ULL

static uint64_t g_min_alloc_frame = 0;

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

    g_bitmap_buffer = find_free_segment_for_bitmap(map,
                                                    g_bitmap_storage_size);
    if (g_bitmap_buffer == NULL)
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

    uint64_t bitmap_start_frame = (uint64_t)g_bitmap_buffer / PAGE_SIZE;
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

    uint64_t kstart_phys = ((uint64_t)&_kernel_start) - VIRT_TO_PHYS_OFFSET;
    uint64_t kend_phys = ((uint64_t)&_kernel_end) - VIRT_TO_PHYS_OFFSET;

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
    irq_flags_t flags = spin_lock_irqsave(&g_pmm_lock);

    uint64_t start = g_min_alloc_frame;
    if (start == 0)
        start = (0x100000ULL / PAGE_SIZE);

    size_t frame = 0;
    BitmapSearchStats search = {0};
    bool found = start < g_total_frames &&
                 bitmap_find_first_zero(&g_bitmap, (size_t)start, &frame,
                                        &search);
    pmm_record_search(&search);
    if (found)
    {
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
    if (count == 0)
        return NULL;

    irq_flags_t flags = spin_lock_irqsave(&g_pmm_lock);

    uint64_t start = g_min_alloc_frame;
    if (start == 0)
        start = (0x100000ULL / PAGE_SIZE);

    size_t run_start = 0;
    BitmapSearchStats search = {0};
    bool found = start < g_total_frames && count <= g_total_frames - start &&
                 bitmap_find_zero_run(&g_bitmap, (size_t)start, count,
                                      &run_start, &search);
    pmm_record_search(&search);
    if (found)
    {
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
    irq_flags_t flags = spin_lock_irqsave(&g_pmm_lock);

    uint64_t frame = physical_address / PAGE_SIZE;

    if (frame < g_total_frames)
    {

        if (!bitmap_get(&g_bitmap, (size_t)frame))
        {
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

bool pmm_validate_integrity(void)
{
    irq_flags_t flags = spin_lock_irqsave(&g_pmm_lock);
    uint64_t free_frames = 0;
    bool valid = g_bitmap.total_bits == (size_t)g_total_frames &&
                 g_reserved_bitmap.total_bits == (size_t)g_total_frames &&
                 g_bitmap.size == g_reserved_bitmap.size &&
                 g_bitmap.size == g_bitmap_size;

    for (uint64_t frame = 0; valid && frame < g_total_frames; frame++)
    {
        bool allocated = bitmap_get(&g_bitmap, (size_t)frame);
        bool reserved = bitmap_get(&g_reserved_bitmap, (size_t)frame);
        if (reserved && !allocated)
            valid = false;
        if (!allocated)
            free_frames++;
    }
    valid = valid && free_frames == g_free_frames;
    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return valid;
}

#ifdef HOBBYOS_SELFTEST
void *pmm_test_reserved_frame(void)
{
    return g_bitmap_buffer;
}
#endif
