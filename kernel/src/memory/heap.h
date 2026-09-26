#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct HeapStats
{
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;

    uint32_t blocks_total;
    uint32_t blocks_free;

    uint64_t largest_free_bytes;
} HeapStats;

typedef struct HeapHardeningStats
{
    uint64_t allocation_overflow;
    uint64_t invalid_alignment;
    uint64_t free_outside_heap;
    uint64_t free_interior;
    uint64_t double_free;
} HeapHardeningStats;

void init_heap(void);

void *kmalloc(size_t size);

void *kmalloc_aligned(size_t size, size_t alignment);

void kfree(void *ptr);

bool heap_get_stats(HeapStats *out_stats);

bool heap_get_hardening_stats(HeapHardeningStats *out_stats);
bool heap_validate_integrity(void);

#ifdef HOBBYOS_SELFTEST
bool heap_test_free(void *ptr);
#endif

#endif
