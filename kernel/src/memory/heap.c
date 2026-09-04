#include "heap.h"
#include "paging.h"
#include "pmem.h"

#include "../core/list.h"
#include "../core/spinlock.h"
#include "../graphics/console.h"
#include "../drivers/serial.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define HEAP_DEFAULT_GROW_BYTES (32ull * 1024ull * 1024ull)
#define HEAP_ALIGN 16ull

#define HEAP_MIN_SPLIT_PAYLOAD 16ull

#define ALIGNED_MAGIC 0xA11A6EADu

static void heap_print_hex(uint64_t n)
{
    serial_write_all("0x");
    for (int i = 60; i >= 0; i -= 4)
    {
        uint8_t nibble = (n >> i) & 0xF;
        char c = (nibble < 10) ? ('0' + nibble) : ('A' + (nibble - 10));
        serial_putc_all(c);
    }
}

static void heap_print_dec(uint64_t n)
{
    if (n == 0)
    {
        serial_write_all("0");
        return;
    }
    char buffer[32];
    int i = 0;
    while (n > 0)
    {
        buffer[i++] = '0' + (n % 10);
        n /= 10;
    }
    for (int j = i - 1; j >= 0; j--)
    {
        serial_putc_all(buffer[j]);
    }
}

typedef struct AlignedAllocHeader
{
    uint32_t magic;
    uint32_t reserved;
    void *raw_ptr;
} AlignedAllocHeader;

typedef struct HeapBlock
{
    size_t size;
    bool free;
    uint8_t _pad[7];
    struct list_head link;
} HeapBlock;

static spinlock_t g_heap_lock;
static int g_heap_lock_ready = 0;

static struct list_head g_heap_blocks;
static int g_heap_initialized = 0;

static uint64_t g_heap_total_bytes = 0;
static uint64_t g_heap_used_bytes = 0;

static inline uint64_t align_up_u64(uint64_t v, uint64_t a)
{
    if (a == 0)
        return v;
    return (v + (a - 1)) & ~(a - 1);
}

static inline size_t align_up_size(size_t v, size_t a)
{
    if (a == 0)
        return v;
    return (v + (a - 1)) & ~(a - 1);
}

static inline int is_pow2_u64(uint64_t v)
{
    return v && ((v & (v - 1)) == 0);
}

static inline HeapBlock *block_from_payload(void *payload)
{
    return (HeapBlock *)((uint8_t *)payload - sizeof(HeapBlock));
}

static inline void *payload_from_block(HeapBlock *b)
{
    return (void *)((uint8_t *)b + sizeof(HeapBlock));
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

    uint8_t *a_end = (uint8_t *)a + sizeof(HeapBlock) + a->size;
    return a_end == (uint8_t *)b;
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
        if (b->free && b->size >= req_size)
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

    nb->size = remaining - sizeof(HeapBlock);
    nb->free = true;

    b->size = req_size;

    heap_insert_after(b, nb);
}

static void heap_coalesce(HeapBlock *b)
{
    if (!b || !b->free)
        return;

    while (1)
    {
        HeapBlock *n = next_block(b);
        if (!n || !n->free)
            break;

        if (!blocks_contiguous(b, n))
            break;

        b->size += sizeof(HeapBlock) + n->size;
        list_del(&n->link);
    }

    while (1)
    {
        HeapBlock *p = prev_block(b);
        if (!p || !p->free)
            break;

        if (!blocks_contiguous(p, b))
            break;

        p->size += sizeof(HeapBlock) + b->size;
        list_del(&b->link);
        b = p;

        while (1)
        {
            HeapBlock *n = next_block(b);
            if (!n || !n->free)
                break;

            if (!blocks_contiguous(b, n))
                break;

            b->size += sizeof(HeapBlock) + n->size;
            list_del(&n->link);
        }
    }
}

static int heap_expand_locked(uint64_t grow_bytes)
{
    if (grow_bytes == 0)
        return 0;

    uint64_t bytes_needed = align_up_u64(grow_bytes, PAGE_SIZE);
    uint64_t pages_needed = bytes_needed / PAGE_SIZE;

    void *phys = pmm_alloc_contiguous_frames((size_t)pages_needed);
    if (!phys)
    {
        console_write_debug("[HEAP] expand failed: pmm_alloc_contiguous_frames returned 0\n");
        return 0;
    }

    HeapBlock *new_blk = (HeapBlock *)phys;
    new_blk->free = true;
    new_blk->size = (size_t)(bytes_needed - sizeof(HeapBlock));

    heap_blocks_init_once();

    if (!list_empty(&g_heap_blocks))
    {
        HeapBlock *last = list_entry(g_heap_blocks.prev, HeapBlock, link);

        if (last->free && blocks_contiguous(last, new_blk))
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

    size = align_up_size(size, (size_t)HEAP_ALIGN);

    irq_flags_t flags = spin_lock_irqsave(&g_heap_lock);

    heap_blocks_init_once();

    HeapBlock *b = heap_find_first_fit(size);
    if (!b)
    {

        uint64_t grow = size + sizeof(HeapBlock) + (64ull * 1024ull);
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

    b->free = false;
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

        return NULL;
    }

    size_t raw_size = size + alignment + sizeof(AlignedAllocHeader);

    void *raw = kmalloc(raw_size);
    if (!raw)
        return NULL;

    uint8_t *base = (uint8_t *)raw + sizeof(AlignedAllocHeader);
    uintptr_t aligned = (uintptr_t)align_up_u64((uint64_t)(uintptr_t)base, (uint64_t)alignment);

    AlignedAllocHeader *hdr = (AlignedAllocHeader *)(aligned - sizeof(AlignedAllocHeader));
    hdr->magic = ALIGNED_MAGIC;
    hdr->reserved = 0;
    hdr->raw_ptr = raw;

    return (void *)aligned;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;

    if (!g_heap_lock_ready)
        return;

    AlignedAllocHeader *ah = (AlignedAllocHeader *)((uint8_t *)ptr - sizeof(AlignedAllocHeader));
    if (ah->magic == ALIGNED_MAGIC && ah->raw_ptr)
    {

        ptr = ah->raw_ptr;
    }

    irq_flags_t flags = spin_lock_irqsave(&g_heap_lock);

    HeapBlock *b = block_from_payload(ptr);

    if (!b->free)
    {
        b->free = true;
        if (g_heap_used_bytes >= (uint64_t)b->size)
            g_heap_used_bytes -= (uint64_t)b->size;
        else
            g_heap_used_bytes = 0;
    }

    heap_coalesce(b);

    spin_unlock_irqrestore(&g_heap_lock, flags);
}

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

            if (b->free)
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