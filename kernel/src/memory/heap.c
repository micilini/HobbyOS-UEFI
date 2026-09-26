#include "heap.h"
#include "paging.h"
#include "pmem.h"

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
            return true;
    }
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
    struct list_head *it;
    list_for_each(it, &g_heap_blocks)
    {
        HeapBlock *block = list_entry(it, HeapBlock, link);
        uintptr_t payload = (uintptr_t)payload_from_block(block);
        if (block->magic != HEAP_BLOCK_MAGIC ||
            payload > UINTPTR_MAX - block->size)
            return NULL;
        if (address >= payload && address < payload + block->size)
            return block;
    }
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
    struct list_head *it;
    list_for_each(it, &g_heap_blocks)
    {
        HeapBlock *b = list_entry(it, HeapBlock, link);
        if (b->magic == HEAP_BLOCK_MAGIC && block_is_free(b) &&
            b->size >= req_size)
            return b;
    }
    return NULL;
}

static void heap_try_split_block(HeapBlock *b, size_t req_size)
{

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
    if (!b || !block_is_free(b))
        return;

    while (1)
    {
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
}

static int heap_expand_locked(uint64_t grow_bytes)
{
    if (grow_bytes == 0)
        return 0;

    uint64_t bytes_needed = 0;
    if (!align_up_u64_checked(grow_bytes, PAGE_SIZE, &bytes_needed) ||
        bytes_needed < sizeof(HeapBlock) || bytes_needed > SIZE_MAX ||
        g_heap_total_bytes > UINT64_MAX - bytes_needed ||
        g_heap_region_count >= HEAP_MAX_REGIONS)
        return 0;
    uint64_t pages_needed = bytes_needed / PAGE_SIZE;
    if (pages_needed == 0 || pages_needed > SIZE_MAX)
        return 0;

    void *phys = pmm_alloc_contiguous_frames((size_t)pages_needed);
    if (!phys)
    {
        console_write_debug("[HEAP] expand failed: pmm_alloc_contiguous_frames returned 0\n");
        return 0;
    }

    HeapBlock *new_blk = (HeapBlock *)phys;
    if (!heap_register_region_locked((uintptr_t)phys, (size_t)bytes_needed))
    {
        for (uint64_t page = 0; page < pages_needed; page++)
            pmm_free_frame((void *)((uintptr_t)phys + page * PAGE_SIZE));
        return 0;
    }
    block_initialize(new_blk, (size_t)(bytes_needed - sizeof(HeapBlock)),
                     HEAP_BLOCK_FREE);

    heap_blocks_init_once();

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

    g_heap_total_bytes += bytes_needed;
    return 1;
}

void init_heap(void)
{
    if (g_heap_lock_ready == 0)
    {
        spinlock_init(&g_heap_lock);
        g_heap_lock_ready = 1;
    }

    irq_flags_t flags = spin_lock_irqsave(&g_heap_lock);

    if (!g_heap_initialized)
        heap_blocks_init_once();

    if (g_heap_total_bytes == 0)
    {

        (void)heap_expand_locked(HEAP_DEFAULT_GROW_BYTES);
    }

    spin_unlock_irqrestore(&g_heap_lock, flags);
}

void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;

    if (!g_heap_lock_ready)
        init_heap();

    if (!align_up_size_checked(size, (size_t)HEAP_ALIGN, &size))
    {
        heap_reject(&g_heap_hardening.allocation_overflow);
        return NULL;
    }

    irq_flags_t flags = spin_lock_irqsave(&g_heap_lock);

    heap_blocks_init_once();

    HeapBlock *b = heap_find_first_fit(size);
    if (!b)
    {

        const size_t grow_overhead = sizeof(HeapBlock) + (64u * 1024u);
        if (size > SIZE_MAX - grow_overhead)
        {
            spin_unlock_irqrestore(&g_heap_lock, flags);
            heap_reject(&g_heap_hardening.allocation_overflow);
            return NULL;
        }
        uint64_t grow = (uint64_t)(size + grow_overhead);
        if (!heap_expand_locked(grow))
        {
            spin_unlock_irqrestore(&g_heap_lock, flags);
            return NULL;
        }

        b = heap_find_first_fit(size);
        if (!b)
        {
            spin_unlock_irqrestore(&g_heap_lock, flags);
            return NULL;
        }
    }

    heap_try_split_block(b, size);

    b->state = HEAP_BLOCK_ALLOCATED;
    g_heap_used_bytes += (uint64_t)b->size;

    void *ret = payload_from_block(b);

    spin_unlock_irqrestore(&g_heap_lock, flags);
    return ret;
}

void *kmalloc_aligned(size_t size, size_t alignment)
{
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
    if (!ptr)
        return true;

    if (!g_heap_lock_ready)
    {
        heap_reject(&g_heap_hardening.free_outside_heap);
        return false;
    }

    irq_flags_t flags = spin_lock_irqsave(&g_heap_lock);
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

    heap_coalesce(b);
    spin_unlock_irqrestore(&g_heap_lock, flags);
    return true;

reject:
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
    if (!out_stats)
        return false;

    if (!g_heap_lock_ready)
        init_heap();

    irq_flags_t flags = spin_lock_irqsave(&g_heap_lock);

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
    if (!g_heap_lock_ready)
        return false;

    irq_flags_t flags = spin_lock_irqsave(&g_heap_lock);
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
    spin_unlock_irqrestore(&g_heap_lock, flags);
    return valid;
}
