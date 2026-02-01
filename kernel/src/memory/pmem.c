#include "pmem.h"
#include "../utils/bitmap.h"
#include "../libc/memory.h"
#include "../core/spinlock.h"

static Bitmap g_bitmap;
static uint64_t g_total_frames = 0;
static uint64_t g_free_frames = 0;
static uint64_t g_bitmap_size = 0;
static void *g_bitmap_buffer = NULL;

static spinlock_t g_pmm_lock;

static uint64_t get_total_memory_size(MemoryMap *map)
{
    uint64_t highest_addr = 0;
    uint64_t entries = mmap_get_entry_count(map);
    for (uint64_t i = 0; i < entries; i++)
    {
        EfiMemoryDescriptor *desc = mmap_get_descriptor(map, i);
        uint64_t end_addr = desc->PhysicalStart + (desc->NumberOfPages * PAGE_SIZE);
        if (end_addr > highest_addr)
            highest_addr = end_addr;
    }
    return highest_addr;
}

static void *find_largest_free_segment(MemoryMap *map, size_t size_needed)
{
    uint64_t entries = mmap_get_entry_count(map);
    void *largest_addr = NULL;
    uint64_t largest_size = 0;

    for (uint64_t i = 0; i < entries; i++)
    {
        EfiMemoryDescriptor *desc = mmap_get_descriptor(map, i);
        if (desc->Type == EFI_CONVENTIONAL_MEMORY)
        {
            uint64_t seg_size = desc->NumberOfPages * PAGE_SIZE;
            if (seg_size > largest_size && seg_size >= size_needed)
            {
                largest_size = seg_size;
                largest_addr = (void *)desc->PhysicalStart;
            }
        }
    }
    return largest_addr;
}

void init_pmm(MemoryMap *map)
{
    spinlock_init(&g_pmm_lock);

    uint64_t mem_size = get_total_memory_size(map);
    g_total_frames = mem_size / PAGE_SIZE;
    g_bitmap_size = (g_total_frames / 8) + 1;

    g_bitmap_buffer = find_largest_free_segment(map, g_bitmap_size);
    if (g_bitmap_buffer == NULL)
        while (1)
            ;

    bitmap_init(&g_bitmap, g_bitmap_buffer, g_total_frames);
    memset(g_bitmap.buffer, 0xFF, g_bitmap.size);
    g_free_frames = 0;

    uint64_t entries = mmap_get_entry_count(map);
    for (uint64_t i = 0; i < entries; i++)
    {
        EfiMemoryDescriptor *desc = mmap_get_descriptor(map, i);
        if (desc->Type == EFI_CONVENTIONAL_MEMORY)
        {
            uint64_t start_frame = desc->PhysicalStart / PAGE_SIZE;
            uint64_t num_frames = desc->NumberOfPages;
            for (uint64_t j = 0; j < num_frames; j++)
            {
                bitmap_set(&g_bitmap, start_frame + j, false);
                g_free_frames++;
            }
        }
    }

    uint64_t bitmap_start_frame = (uint64_t)g_bitmap_buffer / PAGE_SIZE;
    uint64_t bitmap_pages = (g_bitmap_size / PAGE_SIZE) + 1;

    for (uint64_t i = 0; i < bitmap_pages; i++)
    {
        if (!bitmap_get(&g_bitmap, bitmap_start_frame + i))
        {
            bitmap_set(&g_bitmap, bitmap_start_frame + i, true);
            g_free_frames--;
        }
    }

    if (!bitmap_get(&g_bitmap, 0))
    {
        bitmap_set(&g_bitmap, 0, true);
        g_free_frames--;
    }
}

void *pmm_alloc_frame()
{
    irq_flags_t flags = spin_lock_irqsave(&g_pmm_lock);

    for (uint64_t i = 0; i < g_total_frames; i++)
    {
        if (!bitmap_get(&g_bitmap, i))
        {
            bitmap_set(&g_bitmap, i, true);
            g_free_frames--;

            spin_unlock_irqrestore(&g_pmm_lock, flags);
            return (void *)(i * PAGE_SIZE);
        }
    }

    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return NULL;
}

void *pmm_alloc_contiguous_frames(size_t count)
{
    if (count == 0)
        return NULL;

    irq_flags_t flags = spin_lock_irqsave(&g_pmm_lock);

    uint64_t run_start = 0;
    uint64_t run_length = 0;

    for (uint64_t i = 0; i < g_total_frames; i++)
    {
        if (!bitmap_get(&g_bitmap, i))
        {
            if (run_length == 0)
                run_start = i;
            run_length++;

            if (run_length == count)
            {
                for (uint64_t j = 0; j < count; j++)
                {
                    bitmap_set(&g_bitmap, run_start + j, true);
                }
                g_free_frames -= count;

                spin_unlock_irqrestore(&g_pmm_lock, flags);
                return (void *)(run_start * PAGE_SIZE);
            }
        }
        else
        {
            run_length = 0;
        }
    }

    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return NULL;
}

void pmm_free_frame(void *paddr)
{
    irq_flags_t flags = spin_lock_irqsave(&g_pmm_lock);

    uint64_t frame = (uint64_t)paddr / PAGE_SIZE;
    if (bitmap_get(&g_bitmap, frame))
    {
        bitmap_set(&g_bitmap, frame, false);
        g_free_frames++;
    }

    spin_unlock_irqrestore(&g_pmm_lock, flags);
}

uint64_t pmm_get_free_memory()
{
    irq_flags_t flags = spin_lock_irqsave(&g_pmm_lock);
    uint64_t mem = g_free_frames * PAGE_SIZE;
    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return mem;
}

uint64_t pmm_get_total_memory()
{
    return g_total_frames * PAGE_SIZE;
}