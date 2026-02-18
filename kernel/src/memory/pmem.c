#include "pmem.h"
#include "../utils/bitmap.h"
#include "../libc/memory.h"
#include "../core/spinlock.h"
#include "../drivers/serial.h"

static Bitmap g_bitmap;
static uint64_t g_total_frames = 0;
static uint64_t g_free_frames = 0;
static uint64_t g_bitmap_size = 0;
static void *g_bitmap_buffer = NULL;

static spinlock_t g_pmm_lock;

#ifndef PMM_BITMAP_MAX_PHYS
#define PMM_BITMAP_MAX_PHYS 0x40000000ULL
#endif


static void pmem_print_hex(uint64_t n) {
    serial_write_all("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t nibble = (n >> i) & 0xF;
        char c = (nibble < 10) ? ('0' + nibble) : ('A' + (nibble - 10));
        serial_putc_all(c);
    }
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

        uint64_t end_addr = desc->PhysicalStart + (desc->NumberOfPages * PAGE_SIZE);
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
        uint64_t seg_size = desc->NumberOfPages * PAGE_SIZE;

        if (seg_size < size_needed)
            continue;
        if (seg_start >= max_phys)
            continue;
        if (seg_start + size_needed > max_phys)
            continue;

        if (seg_size > best_size)
        {
            best_size = seg_size;
            best_addr = (void *)seg_start;
        }
    }

    return best_addr;
}

void init_pmm(MemoryMap *map)
{
    serial_write_all("[PMM] Initializing...\n");
    spinlock_init(&g_pmm_lock);

    uint64_t highest_ram_end = get_highest_ram_end(map);
    g_total_frames = highest_ram_end / PAGE_SIZE;
    g_bitmap_size = (g_total_frames + 7) / 8;

    serial_write_all("[PMM] Highest RAM: ");
    pmem_print_hex(highest_ram_end);
    serial_write_all(" | Total Frames: ");
    pmem_print_hex(g_total_frames);
    serial_write_all("\n");

    g_bitmap_buffer = find_free_segment_for_bitmap(map, g_bitmap_size);
    if (g_bitmap_buffer == NULL)
    {
        serial_write_all("[PMM] CRITICAL: Failed to allocate bitmap buffer!\n");
        while (1);
    }

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
    uint64_t bitmap_pages = (g_bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;

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

    serial_write_all("[PMM] Init Done. Free Frames: ");
    pmem_print_hex(g_free_frames);
    serial_write_all("\n");
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
    serial_write_all("[PMM] ERROR: Out of Memory (alloc_frame)!\n");
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
    serial_write_all("[PMM] ERROR: Out of Contiguous Memory!\n");
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



bool pmm_is_frame_free(uint64_t physical_address)
{
    irq_flags_t flags = spin_lock_irqsave(&g_pmm_lock);
    
    uint64_t frame = physical_address / PAGE_SIZE;
    
    
    if (frame >= g_total_frames) {
        spin_unlock_irqrestore(&g_pmm_lock, flags);
        return false;
    }

    
    bool is_free = !bitmap_get(&g_bitmap, frame);
    
    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return is_free;
}

void pmm_mark_frame_used(uint64_t physical_address)
{
    irq_flags_t flags = spin_lock_irqsave(&g_pmm_lock);
    
    uint64_t frame = physical_address / PAGE_SIZE;
    
    if (frame < g_total_frames) {
        
        if (!bitmap_get(&g_bitmap, frame)) {
            bitmap_set(&g_bitmap, frame, true);
            g_free_frames--;
            
            serial_write_all("[PMM] Explicitly marked frame used: ");
            pmem_print_hex(physical_address);
            serial_write_all("\n");
        } else {
            serial_write_all("[PMM] Warning: Frame already used: ");
            pmem_print_hex(physical_address);
            serial_write_all("\n");
        }
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