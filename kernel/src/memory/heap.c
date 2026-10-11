#include "../core/irq_latency.h"
#include "../core/irq_latency_heap.h"
#include "heap.h"
#include "paging_private.h"
#include "pmem.h"
#include "physical_translation.h"

#include "../core/list.h"
#include "../core/panic.h"
#include "../core/spinlock.h"
#include "../graphics/console.h"
#include "../drivers/serial.h"
#include "../libc/memory.h"
#include "../utils/bits.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#if defined(HOBBYOS_IRQ_ENTRY_DIAGNOSTIC) && HOBBYOS_IRQ_ENTRY_DIAGNOSTIC
#define HEAP_BEGIN(op, pages) irq_latency_heap_begin((uintptr_t)&g_heap_lock, op, pages)
#define HEAP_MARK(site, work, bytes) irq_latency_heap_mark((uintptr_t)&g_heap_lock, site, work, bytes)
#define HEAP_END() irq_latency_heap_end((uintptr_t)&g_heap_lock)
#define HEAP_COUNT(name) uint64_t name=0
#define HEAP_VALUE(name, value) uint64_t name=(value)
#define HEAP_VISIT(name) (++name)
#include "../core/irq_latency_tsc.h"
#define HEAP_OPERATION_BEGIN() \
    irq_latency_heap_operation_token_t operation_token=irq_latency_heap_operation_prepare((uintptr_t)&g_heap_lock); \
    uint64_t operation_begin=irq_latency_read_tsc()
#define HEAP_OPERATION_END() \
    uint64_t operation_end=irq_latency_read_tsc(); \
    irq_latency_heap_operation_publish(&operation_token,operation_begin,operation_end, \
        previous_size!=b->size ? sizeof(HeapBlock) : 0)
#else
#define HEAP_BEGIN(op, pages) ((void)0)
#define HEAP_MARK(site, work, bytes) ((void)0)
#define HEAP_END() ((void)0)
#define HEAP_COUNT(name)
#define HEAP_VALUE(name, value)
#define HEAP_VISIT(name) ((void)0)
#define HEAP_OPERATION_BEGIN() ((void)0)
#define HEAP_OPERATION_END() ((void)0)
#endif


#define HEAP_DEFAULT_GROW_BYTES (32ull * 1024ull * 1024ull)
#define HEAP_ALIGN 16ull

#define HEAP_MIN_SPLIT_PAYLOAD 16ull
#define HEAP_MAX_REGIONS 1024u

#define ALIGNED_MAGIC 0xA11A6EAD5EEDC0DEULL
#define ALIGNED_CANARY 0xC4A4B1E5F00D1979ULL
#define HEAP_BLOCK_MAGIC 0x48454150u
#define HEAP_BLOCK_FREE 0x46u
#define HEAP_BLOCK_ALLOCATED 0x41u
#define HEAP_BLOCK_DEAD 0x44u

typedef struct AlignedAllocHeader
{
    uint64_t magic;
    uint64_t canary;
    void *raw_ptr;
    void *user_ptr;
    size_t alignment;
} AlignedAllocHeader;

typedef struct HeapBlock
{
    size_t size;
    uint32_t magic;
    uint8_t state;
    uint8_t _pad[3];
    struct list_head link;
} HeapBlock;

typedef struct HeapRegion
{
    uintptr_t start;
    uintptr_t end;
} HeapRegion;

typedef struct HeapPreparedRegion
{
    void *physical;
    void *virtual_region;
    size_t bytes;
    size_t pages;
} HeapPreparedRegion;

_Static_assert(sizeof(HeapBlock) == 32u,
               "heap block layout must remain consumer-compatible");

static spinlock_t g_heap_lock;
static int g_heap_lock_ready = 0;

static struct list_head g_heap_blocks;
static int g_heap_initialized = 0;

static uint64_t g_heap_total_bytes = 0;
static uint64_t g_heap_used_bytes = 0;
static HeapRegion g_heap_regions[HEAP_MAX_REGIONS];
static uint32_t g_heap_region_count;
static HeapHardeningStats g_heap_hardening;

static inline int is_pow2_u64(uint64_t v)
{
    return v && ((v & (v - 1)) == 0);
}

static inline bool block_is_free(const HeapBlock *block)
{
    return block->state == HEAP_BLOCK_FREE;
}

static inline bool block_is_allocated(const HeapBlock *block)
{
    return block->state == HEAP_BLOCK_ALLOCATED;
}

static inline void block_initialize(HeapBlock *block, size_t size,
                                    uint8_t state)
{
    block->size = size;
    block->magic = HEAP_BLOCK_MAGIC;
    block->state = state;
    block->_pad[0] = 0;
    block->_pad[1] = 0;
    block->_pad[2] = 0;
}

static void heap_reject(uint64_t *counter)
{
    __atomic_add_fetch(counter, 1u, __ATOMIC_RELAXED);
    KWARN_ON(true);
}

static inline void *payload_from_block(HeapBlock *b)
{
    return (void *)((uint8_t *)b + sizeof(HeapBlock));
}

static bool heap_region_contains(uintptr_t address)
{
    for (uint32_t i = 0; i < g_heap_region_count; i++)
    {
        if (address >= g_heap_regions[i].start &&
            address < g_heap_regions[i].end)
        {
            HEAP_MARK(IRQ_HEAP_REGION, i+1u, 0);
            return true;
        }
    }
    HEAP_MARK(IRQ_HEAP_REGION, g_heap_region_count, 0);
    return false;
}

static bool heap_register_region_locked(uintptr_t start, size_t size)
{
    if (size == 0 || start > UINTPTR_MAX - size)
        return false;
    uintptr_t end = start + size;

    for (uint32_t i = 0; i < g_heap_region_count; i++)
    {
        if (g_heap_regions[i].end == start)
        {
            g_heap_regions[i].end = end;
            for (uint32_t j = 0; j < g_heap_region_count; j++)
            {
                if (j != i && g_heap_regions[j].start == end)
                {
                    g_heap_regions[i].end = g_heap_regions[j].end;
                    g_heap_regions[j] =
                        g_heap_regions[g_heap_region_count - 1u];
                    g_heap_region_count--;
                    break;
                }
            }
            return true;
        }
        if (g_heap_regions[i].start == end)
        {
            g_heap_regions[i].start = start;
            return true;
        }
        if (start >= g_heap_regions[i].start &&
            end <= g_heap_regions[i].end)
            return true;
    }

    if (g_heap_region_count >= HEAP_MAX_REGIONS)
        return false;
    g_heap_regions[g_heap_region_count++] = (HeapRegion){start, end};
    return true;
}

static HeapBlock *heap_find_containing_block_locked(uintptr_t address)
{
    HEAP_COUNT(visited);
    struct list_head *it;
    list_for_each(it, &g_heap_blocks)
    {
        HEAP_VISIT(visited);
        HeapBlock *block = list_entry(it, HeapBlock, link);
        uintptr_t payload = (uintptr_t)payload_from_block(block);
        if (block->magic != HEAP_BLOCK_MAGIC ||
            payload > UINTPTR_MAX - block->size)
        {
            HEAP_MARK(IRQ_HEAP_CONTAINMENT, visited, 0);
            return NULL;
        }
        if (address >= payload && address < payload + block->size)
        {
            HEAP_MARK(IRQ_HEAP_CONTAINMENT, visited, 0);
            return block;
        }
    }
    HEAP_MARK(IRQ_HEAP_CONTAINMENT, visited, 0);
    return NULL;
}

static inline HeapBlock *next_block(HeapBlock *b)
{
    if (!b)
        return NULL;
    if (b->link.next == &g_heap_blocks)
        return NULL;
    return list_entry(b->link.next, HeapBlock, link);
}

static inline HeapBlock *prev_block(HeapBlock *b)
{
    if (!b)
        return NULL;
    if (b->link.prev == &g_heap_blocks)
        return NULL;
    return list_entry(b->link.prev, HeapBlock, link);
}

static inline int blocks_contiguous(HeapBlock *a, HeapBlock *b)
{
    if (!a || !b)
        return 0;

    uintptr_t start = (uintptr_t)a;
    if (start > UINTPTR_MAX - sizeof(HeapBlock) ||
        start + sizeof(HeapBlock) > UINTPTR_MAX - a->size)
        return 0;
    return start + sizeof(HeapBlock) + a->size == (uintptr_t)b;
}

static void heap_blocks_init_once(void)
{
    if (!g_heap_initialized)
    {
        list_init(&g_heap_blocks);
        g_heap_initialized = 1;
    }
}

static void heap_insert_after(HeapBlock *pos, HeapBlock *new_blk)
{

    list_init(&new_blk->link);
    list_add(&new_blk->link, &pos->link);
}

static void heap_append_tail(HeapBlock *new_blk)
{
    list_init(&new_blk->link);
    list_add_tail(&new_blk->link, &g_heap_blocks);
}

static HeapBlock *heap_find_first_fit(size_t req_size)
{
    HEAP_COUNT(visited);
    struct list_head *it;
    list_for_each(it, &g_heap_blocks)
    {
        HEAP_VISIT(visited);
        HeapBlock *b = list_entry(it, HeapBlock, link);
        if (b->magic == HEAP_BLOCK_MAGIC && block_is_free(b) &&
            b->size >= req_size)
        {
            HEAP_MARK(IRQ_HEAP_FIRST_FIT, visited, 0);
            return b;
        }
    }
    HEAP_MARK(IRQ_HEAP_FIRST_FIT, visited, 0);
    return NULL;
}

static void heap_try_split_block(HeapBlock *b, size_t req_size)
{
#ifdef IRQ_LATENCY_HEAP_OPERATION_HOSTED
#if !__STDC_HOSTED__
#error IRQ_LATENCY_HEAP_OPERATION_HOSTED requires a hosted test
#endif
    extern void irq_latency_heap_metadata_hosted(void);
    irq_latency_heap_metadata_hosted();
#endif

    if (!b)
        return;

    if (b->size < req_size)
        return;

    size_t remaining = b->size - req_size;
    if (remaining <= (sizeof(HeapBlock) + HEAP_MIN_SPLIT_PAYLOAD))
        return;

    uint8_t *new_addr = (uint8_t *)payload_from_block(b) + req_size;
    HeapBlock *nb = (HeapBlock *)new_addr;

    block_initialize(nb, remaining - sizeof(HeapBlock), HEAP_BLOCK_FREE);

    b->size = req_size;

    heap_insert_after(b, nb);
}

static void heap_coalesce(HeapBlock *b)
{
    HEAP_COUNT(visited);
    if (!b || !block_is_free(b))
        return;

    while (1)
    {
        HEAP_VISIT(visited);
        HeapBlock *n = next_block(b);
        if (!n || !block_is_free(n))
            break;

        if (!blocks_contiguous(b, n))
            break;

        size_t merged_size = n->size;
        b->size += sizeof(HeapBlock) + merged_size;
        list_del(&n->link);
        n->magic = 0;
        n->state = HEAP_BLOCK_DEAD;
    }

    while (1)
    {
        HEAP_VISIT(visited);
        HeapBlock *p = prev_block(b);
        if (!p || !block_is_free(p))
            break;

        if (!blocks_contiguous(p, b))
            break;

        size_t merged_size = b->size;
        p->size += sizeof(HeapBlock) + merged_size;
        list_del(&b->link);
        b->magic = 0;
        b->state = HEAP_BLOCK_DEAD;
        b = p;

        while (1)
        {
            HEAP_VISIT(visited);
            HeapBlock *n = next_block(b);
            if (!n || !block_is_free(n))
                break;

            if (!blocks_contiguous(b, n))
                break;

            size_t next_size = n->size;
            b->size += sizeof(HeapBlock) + next_size;
            list_del(&n->link);
            n->magic = 0;
            n->state = HEAP_BLOCK_DEAD;
        }
    }
    HEAP_MARK(IRQ_HEAP_COALESCE, visited, 0);
}

static void heap_release_prepared(HeapPreparedRegion *region)
{
    if (!region->physical)
        return;
    for (size_t page = 0; page < region->pages; page++)
        pmm_free_frame((void *)((uintptr_t)region->physical + page * PAGE_SIZE));
    *region = (HeapPreparedRegion){0};
}

static bool heap_prepare_region(uint64_t grow_bytes, HeapPreparedRegion *region)
{
    *region = (HeapPreparedRegion){0};
    uint64_t bytes_needed = 0;
    if (!grow_bytes || !align_up_u64_checked(grow_bytes, PAGE_SIZE, &bytes_needed) ||
        bytes_needed < sizeof(HeapBlock) || bytes_needed > SIZE_MAX)
        return false;
    uint64_t pages_needed = bytes_needed / PAGE_SIZE;
    if (pages_needed == 0 || pages_needed > SIZE_MAX)
        return false;

    void *phys = pmm_alloc_contiguous_frames((size_t)pages_needed);
    if (!phys)
    {
        console_write_debug("[HEAP] expand failed: pmm_alloc_contiguous_frames returned 0\n");
        return false;
    }

    void *virtual_region = NULL;
    *region = (HeapPreparedRegion){phys, NULL, (size_t)bytes_needed, (size_t)pages_needed};
    if (!physical_pointer_range((uintptr_t)phys, (size_t)bytes_needed,
                                PHYSICAL_RAM, &virtual_region))
    {
        heap_release_prepared(region);
        return false;
    }
    region->virtual_region = virtual_region;
    HeapBlock *new_blk = (HeapBlock *)virtual_region;
    block_initialize(new_blk, (size_t)(bytes_needed - sizeof(HeapBlock)),
                     HEAP_BLOCK_FREE);
    return true;
}

static bool heap_publish_prepared_locked(HeapPreparedRegion *region)
{
    if (!region->physical || !region->virtual_region ||
        g_heap_total_bytes > UINT64_MAX - region->bytes ||
        g_heap_region_count >= HEAP_MAX_REGIONS ||
        !heap_register_region_locked((uintptr_t)region->virtual_region, region->bytes))
        return false;
    HeapBlock *new_blk = region->virtual_region;

    if (!list_empty(&g_heap_blocks))
    {
        HeapBlock *last = list_entry(g_heap_blocks.prev, HeapBlock, link);

        if (block_is_free(last) && blocks_contiguous(last, new_blk))
        {
            last->size += sizeof(HeapBlock) + new_blk->size;
        }
        else
        {
            heap_append_tail(new_blk);
        }
    }
    else
    {
        heap_append_tail(new_blk);
    }

    g_heap_total_bytes += region->bytes;
    region->physical = NULL;
    return true;
}

void init_heap(void)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_HEAP);
    if (g_heap_lock_ready == 0)
    {
        spinlock_init(&g_heap_lock);
        g_heap_lock_ready = 1;
    }

    irq_flags_t flags = spin_lock_irqsave(&g_heap_lock);
    HEAP_BEGIN(IRQ_HEAP_INITIALIZE, 0);

    if (!g_heap_initialized)
        heap_blocks_init_once();

    bool need_region = g_heap_total_bytes == 0;
    HEAP_MARK(IRQ_HEAP_INIT, 1, 0);
    HEAP_END();
    spin_unlock_irqrestore(&g_heap_lock, flags);
    if (need_region)
    {
        HeapPreparedRegion region;
        (void)heap_prepare_region(HEAP_DEFAULT_GROW_BYTES, &region);
        flags = spin_lock_irqsave(&g_heap_lock);
        HEAP_BEGIN(IRQ_HEAP_INITIAL_PUBLISH, region.pages);
        if (g_heap_total_bytes == 0)
            (void)heap_publish_prepared_locked(&region);
        HEAP_MARK(IRQ_HEAP_PUBLISH, g_heap_region_count, 0);
        HEAP_END();
        spin_unlock_irqrestore(&g_heap_lock, flags);
        heap_release_prepared(&region);
    }
}

void *kmalloc(size_t size)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_HEAP);
    if (size == 0)
        return NULL;

    if (!g_heap_lock_ready)
        init_heap();

    if (!align_up_size_checked(size, (size_t)HEAP_ALIGN, &size))
    {
        heap_reject(&g_heap_hardening.allocation_overflow);
        return NULL;
    }

    HeapPreparedRegion region = {0};
    irq_flags_t flags = spin_lock_irqsave(&g_heap_lock);
    HEAP_BEGIN(IRQ_HEAP_ALLOCATE, 0);

    heap_blocks_init_once();

    HeapBlock *b = heap_find_first_fit(size);
    if (!b)
    {

        const size_t grow_overhead = sizeof(HeapBlock) + (64u * 1024u);
        if (size > SIZE_MAX - grow_overhead)
        {
            HEAP_END();
            spin_unlock_irqrestore(&g_heap_lock, flags);
            heap_reject(&g_heap_hardening.allocation_overflow);
            return NULL;
        }
        uint64_t grow = (uint64_t)(size + grow_overhead);
        HEAP_END();
        spin_unlock_irqrestore(&g_heap_lock, flags);
        (void)heap_prepare_region(grow, &region);
        flags = spin_lock_irqsave(&g_heap_lock);
        HEAP_BEGIN(IRQ_HEAP_RECHECK, grow/PAGE_SIZE+(grow%PAGE_SIZE!=0));
        /* A concurrent free or growth wins in the original first-fit order. */
        b = heap_find_first_fit(size);
        if (!b)
        {
            bool published = heap_publish_prepared_locked(&region);
            HEAP_MARK(IRQ_HEAP_PUBLISH, g_heap_region_count, 0);
            if (published) b = heap_find_first_fit(size);
        }
        if (!b)
        {
            HEAP_END();
            spin_unlock_irqrestore(&g_heap_lock, flags);
            heap_release_prepared(&region);
            return NULL;
        }
    }

    HEAP_VALUE(previous_size, b->size);
    HEAP_OPERATION_BEGIN();
    heap_try_split_block(b, size);

    b->state = HEAP_BLOCK_ALLOCATED;
    g_heap_used_bytes += (uint64_t)b->size;

    void *ret = payload_from_block(b);
    HEAP_OPERATION_END();
    HEAP_MARK(IRQ_HEAP_METADATA, 1, previous_size!=b->size ? sizeof(HeapBlock) : 0);

    HEAP_END();
    spin_unlock_irqrestore(&g_heap_lock, flags);
    heap_release_prepared(&region);
    return ret;
}

void *kmalloc_aligned(size_t size, size_t alignment)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_HEAP);
    if (size == 0)
        return NULL;

    if (!is_pow2_u64((uint64_t)alignment) || alignment < HEAP_ALIGN)
    {
        heap_reject(&g_heap_hardening.invalid_alignment);
        return NULL;
    }

    if (size > SIZE_MAX - alignment ||
        size + alignment > SIZE_MAX - sizeof(AlignedAllocHeader))
    {
        heap_reject(&g_heap_hardening.allocation_overflow);
        return NULL;
    }
    size_t raw_size = size + alignment + sizeof(AlignedAllocHeader);

    void *raw = kmalloc(raw_size);
    if (!raw)
        return NULL;

    uint8_t *base = (uint8_t *)raw + sizeof(AlignedAllocHeader);
    uint64_t aligned_value = 0;
    if (!align_up_u64_checked((uint64_t)(uintptr_t)base,
                              (uint64_t)alignment, &aligned_value))
    {
        kfree(raw);
        heap_reject(&g_heap_hardening.allocation_overflow);
        return NULL;
    }
    uintptr_t aligned = (uintptr_t)aligned_value;

    AlignedAllocHeader *hdr = (AlignedAllocHeader *)(aligned - sizeof(AlignedAllocHeader));
    hdr->magic = ALIGNED_MAGIC;
    hdr->canary = ALIGNED_CANARY ^ (uint64_t)(uintptr_t)raw ^
                  (uint64_t)aligned ^ (uint64_t)alignment;
    hdr->raw_ptr = raw;
    hdr->user_ptr = (void *)aligned;
    hdr->alignment = alignment;

    return (void *)aligned;
}

static bool heap_free_checked(void *ptr)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_HEAP);
    if (!ptr)
        return true;

    if (!g_heap_lock_ready)
    {
        heap_reject(&g_heap_hardening.free_outside_heap);
        return false;
    }

    irq_flags_t flags = spin_lock_irqsave(&g_heap_lock);
    HEAP_BEGIN(IRQ_HEAP_FREE, 0);
    uintptr_t address = (uintptr_t)ptr;
    uint64_t *reject_counter = NULL;
    if (!heap_region_contains(address))
    {
        reject_counter = &g_heap_hardening.free_outside_heap;
        goto reject;
    }

    HeapBlock *b = heap_find_containing_block_locked(address);
    if (!b)
    {
        reject_counter = &g_heap_hardening.free_interior;
        goto reject;
    }
    if (block_is_free(b))
    {
        reject_counter = &g_heap_hardening.double_free;
        goto reject;
    }
    if (!block_is_allocated(b))
    {
        reject_counter = &g_heap_hardening.free_interior;
        goto reject;
    }

    uintptr_t payload = (uintptr_t)payload_from_block(b);
    bool valid_pointer = address == payload;
    AlignedAllocHeader aligned_header;
    uintptr_t header_address = 0;
    if (!valid_pointer)
    {
        uintptr_t payload_end = payload + b->size;
        if (address < payload + sizeof(AlignedAllocHeader))
        {
            reject_counter = &g_heap_hardening.free_interior;
            goto reject;
        }
        header_address = address - sizeof(AlignedAllocHeader);
        if (header_address < payload ||
            header_address > payload_end - sizeof(AlignedAllocHeader))
        {
            reject_counter = &g_heap_hardening.free_interior;
            goto reject;
        }
        memcpy(&aligned_header, (const void *)header_address,
               sizeof(aligned_header));
        uint64_t expected_canary =
            ALIGNED_CANARY ^ (uint64_t)payload ^ (uint64_t)address ^
            (uint64_t)aligned_header.alignment;
        valid_pointer = aligned_header.magic == ALIGNED_MAGIC &&
                        aligned_header.canary == expected_canary &&
                        aligned_header.raw_ptr == (void *)payload &&
                        aligned_header.user_ptr == ptr &&
                        is_pow2_u64((uint64_t)aligned_header.alignment) &&
                        aligned_header.alignment >= HEAP_ALIGN &&
                        address % aligned_header.alignment == 0;
        if (!valid_pointer)
        {
            reject_counter = &g_heap_hardening.free_interior;
            goto reject;
        }
        memset((void *)header_address, 0, sizeof(AlignedAllocHeader));
    }

    b->state = HEAP_BLOCK_FREE;
    if (g_heap_used_bytes >= (uint64_t)b->size)
        g_heap_used_bytes -= (uint64_t)b->size;
    else
        g_heap_used_bytes = 0;

    HEAP_MARK(IRQ_HEAP_POINTER, 1, header_address ? sizeof(AlignedAllocHeader) : 0);
    heap_coalesce(b);
    HEAP_END();
    spin_unlock_irqrestore(&g_heap_lock, flags);
    return true;

reject:
    HEAP_END();
    spin_unlock_irqrestore(&g_heap_lock, flags);
    if (reject_counter)
    {
        heap_reject(reject_counter);
    }
    return false;
}

void kfree(void *ptr)
{
    (void)heap_free_checked(ptr);
}

#ifdef HOBBYOS_SELFTEST
bool heap_test_free(void *ptr)
{
    return heap_free_checked(ptr);
}
#endif

bool heap_get_stats(HeapStats *out_stats)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_HEAP);
    if (!out_stats)
        return false;

    if (!g_heap_lock_ready)
        init_heap();

    irq_flags_t flags = spin_lock_irqsave(&g_heap_lock);
    HEAP_BEGIN(IRQ_HEAP_STATS, 0);

    HeapStats st;
    st.total_bytes = g_heap_total_bytes;
    st.used_bytes = g_heap_used_bytes;
    st.free_bytes = (g_heap_total_bytes >= g_heap_used_bytes) ? (g_heap_total_bytes - g_heap_used_bytes) : 0;

    st.blocks_total = 0;
    st.blocks_free = 0;
    st.largest_free_bytes = 0;

    if (g_heap_initialized)
    {
        struct list_head *it;
        list_for_each(it, &g_heap_blocks)
        {
            HeapBlock *b = list_entry(it, HeapBlock, link);
            st.blocks_total++;

            if (block_is_free(b))
            {
                st.blocks_free++;
                if ((uint64_t)b->size > st.largest_free_bytes)
                    st.largest_free_bytes = (uint64_t)b->size;
            }
        }
    }

    *out_stats = st;
    HEAP_MARK(IRQ_HEAP_STATISTICS, st.blocks_total, sizeof(st));

    HEAP_END();
    spin_unlock_irqrestore(&g_heap_lock, flags);
    return true;
}

bool heap_get_hardening_stats(HeapHardeningStats *out_stats)
{
    if (!out_stats)
        return false;

    out_stats->allocation_overflow = __atomic_load_n(
        &g_heap_hardening.allocation_overflow, __ATOMIC_RELAXED);
    out_stats->invalid_alignment = __atomic_load_n(
        &g_heap_hardening.invalid_alignment, __ATOMIC_RELAXED);
    out_stats->free_outside_heap = __atomic_load_n(
        &g_heap_hardening.free_outside_heap, __ATOMIC_RELAXED);
    out_stats->free_interior = __atomic_load_n(
        &g_heap_hardening.free_interior, __ATOMIC_RELAXED);
    out_stats->double_free = __atomic_load_n(
        &g_heap_hardening.double_free, __ATOMIC_RELAXED);
    return true;
}

static bool heap_span_in_region(uintptr_t start, uintptr_t end)
{
    for (uint32_t i = 0; i < g_heap_region_count; i++)
    {
        if (start >= g_heap_regions[i].start && end <= g_heap_regions[i].end)
            return true;
    }
    return false;
}

bool heap_validate_integrity(void)
{
    IRQ_LATENCY_SCOPE(IRQ_PATH_HEAP);
    if (!g_heap_lock_ready)
        return false;

    irq_flags_t flags = spin_lock_irqsave(&g_heap_lock);
    HEAP_BEGIN(IRQ_HEAP_INTEGRITY, 0);
    HEAP_COUNT(visited);
    bool valid = g_heap_initialized != 0 &&
                 g_heap_blocks.next != NULL && g_heap_blocks.prev != NULL;
    uint64_t region_bytes = 0;
    uint64_t block_bytes = 0;
    uint64_t used_bytes = 0;
    HeapBlock *previous = NULL;

    for (uint32_t i = 0; valid && i < g_heap_region_count; i++)
    {
        HeapRegion region = g_heap_regions[i];
        valid = region.end > region.start &&
                region_bytes <= UINT64_MAX - (region.end - region.start);
        if (valid)
            region_bytes += region.end - region.start;
    }

    if (valid)
    {
        struct list_head *it;
        list_for_each(it, &g_heap_blocks)
        {
            HeapBlock *block = list_entry(it, HeapBlock, link);
            HEAP_VISIT(visited);
            uintptr_t start = (uintptr_t)block;
            if (it->next == NULL || it->prev == NULL ||
                it->next->prev != it || it->prev->next != it ||
                block->magic != HEAP_BLOCK_MAGIC ||
                (!block_is_free(block) && !block_is_allocated(block)) ||
                start > UINTPTR_MAX - sizeof(HeapBlock) ||
                start + sizeof(HeapBlock) > UINTPTR_MAX - block->size)
            {
                valid = false;
                break;
            }
            uintptr_t end = start + sizeof(HeapBlock) + block->size;
            uint64_t extent = (uint64_t)(end - start);
            if (!heap_span_in_region(start, end) ||
                block_bytes > UINT64_MAX - extent ||
                (block_is_allocated(block) &&
                 used_bytes > UINT64_MAX - block->size) ||
                (previous && block_is_free(previous) &&
                 block_is_free(block) && blocks_contiguous(previous, block)))
            {
                valid = false;
                break;
            }
            block_bytes += extent;
            if (block_is_allocated(block))
                used_bytes += block->size;
            previous = block;
        }
    }

    valid = valid && region_bytes == g_heap_total_bytes &&
            block_bytes == g_heap_total_bytes &&
            used_bytes == g_heap_used_bytes;
    HEAP_MARK(IRQ_HEAP_VALIDATE, visited, 0);
    HEAP_END();
    spin_unlock_irqrestore(&g_heap_lock, flags);
    return valid;
}
