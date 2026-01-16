#include "heap.h"
#include "pmem.h"
#include "paging.h"
#include "../libc/memory.h"

static BlockHeader *g_head = NULL;

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

    expand_heap(32 * 1024 * 1024);
}

void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;

    size = (size + 15) & ~15;

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
            return (void *)((uint8_t *)current + sizeof(BlockHeader));
        }
        current = current->next;
    }

    BlockHeader *new_block = expand_heap(size);
    if (new_block)
    {
        return kmalloc(size);
    }

    return NULL;
}

void *kmalloc_aligned(size_t size, size_t alignment)
{
    size_t offset = alignment + sizeof(void *);
    void *p1 = kmalloc(size + offset);

    if (p1 == NULL)
        return NULL;

    uintptr_t addr = (uintptr_t)p1 + sizeof(void *);
    uintptr_t aligned_addr = (addr + (alignment - 1)) & ~(alignment - 1);

    void **ptr_storage = (void **)(aligned_addr - sizeof(void *));
    *ptr_storage = p1;

    return (void *)aligned_addr;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;

    BlockHeader *block = (BlockHeader *)((uint8_t *)ptr - sizeof(BlockHeader));
    block->is_free = true;
    merge_free_blocks(block);
}

bool heap_get_stats(HeapStats *out_stats)
{
    if (!out_stats)
        return false;

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

    return true;
}