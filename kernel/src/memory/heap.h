#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct BlockHeader
{
    size_t size;
    bool is_free;
    struct BlockHeader *next;
    struct BlockHeader *prev;
} BlockHeader;

typedef struct HeapStats
{
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;

    uint64_t blocks_total;
    uint64_t blocks_free;

    uint64_t largest_free_bytes;
} HeapStats;

void init_heap();

void *kmalloc(size_t size);

void *kmalloc_aligned(size_t size, size_t alignment);

void kfree(void *ptr);

bool heap_get_stats(HeapStats *out_stats);

#endif