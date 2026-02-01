#include "heap.h"
#include "pmem.h"
#include "paging.h"
#include "../libc/memory.h"
#include "../core/spinlock.h"

static BlockHeader *g_head = NULL;

static spinlock_t g_heap_lock;
static int g_heap_lock_ready = 0;
static int g_heap_initialized = 0;

static inline void heap_lock_init_once(void)
{
    if (!g_heap_lock_ready)
    {
        spinlock_init(&g_heap_lock);
        g_heap_lock_ready = 1;
    }
}


#define HEAP_ALIGNED_MAGIC 0x48454150414C4947ULL

typedef struct heap_aligned_hdr
{
    uint64_t magic;
    void *raw_ptr;
} heap_aligned_hdr_t;

static inline int is_power_of_two(size_t x)
{
    return (x != 0) && ((x & (x - 1)) == 0);
}

static inline uintptr_t align_up_uintptr(uintptr_t x, size_t alignment)
{
    return (x + (alignment - 1)) & ~(uintptr_t)(alignment - 1);
}

static void init_block(BlockHeader *block, size_t size, BlockHeader *prev, BlockHeader *next)
{
    block->size = size;
    block->is_free = true;
    block->prev = prev;
    block->next = next;
}


static BlockHeader *expand_heap(size_t size_needed)
{
    size_t total_needed = size_needed + sizeof(BlockHeader);
    size_t pages_needed = (total_needed / PAGE_SIZE);
    if ((total_needed % PAGE_SIZE) != 0)
        pages_needed++;

    if (pages_needed == 0)
        pages_needed = 1;

    void *paddr = pmm_alloc_contiguous_frames(pages_needed);
    if (!paddr)
        return NULL;

    BlockHeader *new_block = (BlockHeader *)paddr;

    size_t block_data_size = (pages_needed * PAGE_SIZE) - sizeof(BlockHeader);

    init_block(new_block, block_data_size, NULL, NULL);

    if (g_head == NULL)
    {
        g_head = new_block;
    }
    else
    {
        BlockHeader *current = g_head;
        while (current->next != NULL)
        {
            current = current->next;
        }
        current->next = new_block;
        new_block->prev = current;
    }

    return new_block;
}


static void merge_free_blocks(BlockHeader *block)
{
    if (!block)
        return;

    if (block->next && block->next->is_free)
    {
        if ((uint8_t *)block + sizeof(BlockHeader) + block->size == (uint8_t *)block->next)
        {
            block->size += sizeof(BlockHeader) + block->next->size;
            block->next = block->next->next;
            if (block->next)
                block->next->prev = block;
        }
    }

    if (block->prev && block->prev->is_free)
    {
        if ((uint8_t *)block->prev + sizeof(BlockHeader) + block->prev->size == (uint8_t *)block)
        {
            block->prev->size += sizeof(BlockHeader) + block->size;
            block->prev->next = block->next;
            if (block->next)
                block->next->prev = block->prev;
        }
    }
}

void init_heap()
{
    heap_lock_init_once();

    irq_flags_t flags = spin_lock_irqsave(&g_heap_lock);
    if (g_heap_initialized)
    {
        spin_unlock_irqrestore(&g_heap_lock, flags);
        return;
    }
    g_heap_initialized = 1;

   
    expand_heap(32 * 1024 * 1024);

    spin_unlock_irqrestore(&g_heap_lock, flags);
}

void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;

    heap_lock_init_once();

    if (!g_heap_initialized)
        init_heap();

    size = (size + 15) & ~15;

retry:
    irq_flags_t flags = spin_lock_irqsave(&g_heap_lock);

    BlockHeader *current = g_head;

    while (current != NULL)
    {
        if (current->is_free && current->size >= size)
        {
            if (current->size > size + sizeof(BlockHeader) + 64)
            {
                BlockHeader *split_block = (BlockHeader *)((uint8_t *)current + sizeof(BlockHeader) + size);

                init_block(split_block,
                           current->size - size - sizeof(BlockHeader),
                           current,
                           current->next);

                if (current->next)
                    current->next->prev = split_block;
                current->next = split_block;
                current->size = size;
            }

            current->is_free = false;

            void *ret = (void *)((uint8_t *)current + sizeof(BlockHeader));
            spin_unlock_irqrestore(&g_heap_lock, flags);
            return ret;
        }

        current = current->next;
    }

   
    BlockHeader *new_block = expand_heap(size);
    spin_unlock_irqrestore(&g_heap_lock, flags);

    if (new_block)
        goto retry;

    return NULL;
}

void *kmalloc_aligned(size_t size, size_t alignment)
{
    if (size == 0)
        return NULL;

    if (alignment == 0)
        alignment = 16;

    if (alignment < sizeof(void *))
        alignment = sizeof(void *);

    if (!is_power_of_two(alignment))
    {
       
        return NULL;
    }

    heap_lock_init_once();

    if (!g_heap_initialized)
        init_heap();

   
    size_t extra = alignment + sizeof(heap_aligned_hdr_t);
    void *raw = kmalloc(size + extra);
    if (!raw)
        return NULL;

    uintptr_t base = (uintptr_t)raw + sizeof(heap_aligned_hdr_t);
    uintptr_t aligned_addr = align_up_uintptr(base, alignment);

    heap_aligned_hdr_t *hdr = (heap_aligned_hdr_t *)(aligned_addr - sizeof(heap_aligned_hdr_t));
    hdr->magic = HEAP_ALIGNED_MAGIC;
    hdr->raw_ptr = raw;

    return (void *)aligned_addr;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;

    heap_lock_init_once();

   
    heap_aligned_hdr_t *ah = (heap_aligned_hdr_t *)((uint8_t *)ptr - sizeof(heap_aligned_hdr_t));
    if (ah->magic == HEAP_ALIGNED_MAGIC && ah->raw_ptr != NULL)
    {
       
        ah->magic = 0;
        ptr = ah->raw_ptr;
    }

    irq_flags_t flags = spin_lock_irqsave(&g_heap_lock);

    BlockHeader *block = (BlockHeader *)((uint8_t *)ptr - sizeof(BlockHeader));
    block->is_free = true;
    merge_free_blocks(block);

    spin_unlock_irqrestore(&g_heap_lock, flags);
}

bool heap_get_stats(HeapStats *out_stats)
{
    if (!out_stats)
        return false;

    heap_lock_init_once();

    irq_flags_t flags = spin_lock_irqsave(&g_heap_lock);

    out_stats->total_bytes = 0;
    out_stats->used_bytes = 0;
    out_stats->free_bytes = 0;
    out_stats->blocks_total = 0;
    out_stats->blocks_free = 0;
    out_stats->largest_free_bytes = 0;

    BlockHeader *cur = g_head;
    while (cur)
    {
        out_stats->blocks_total++;
        out_stats->total_bytes += (uint64_t)cur->size;

        if (cur->is_free)
        {
            out_stats->blocks_free++;
            out_stats->free_bytes += (uint64_t)cur->size;
            if ((uint64_t)cur->size > out_stats->largest_free_bytes)
            {
                out_stats->largest_free_bytes = (uint64_t)cur->size;
            }
        }

        cur = cur->next;
    }

    out_stats->used_bytes = (out_stats->total_bytes >= out_stats->free_bytes)
                                ? (out_stats->total_bytes - out_stats->free_bytes)
                                : 0;

    spin_unlock_irqrestore(&g_heap_lock, flags);
    return true;
}