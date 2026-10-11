#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/src/memory/mapping_core.h"

static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while (0)
static mapping_window_t window;
static PageTable root;
static PageTable backing[MAPPING_WINDOW_TABLES];
static unsigned attempts, fail_at, released;
static bool allocate(void *ctx, mapping_table_t *out)
{
    (void)ctx;
    unsigned at = attempts++;
    if (at == fail_at) return false;
    *out = (mapping_table_t){&backing[at], UINT64_C(0x80000000) + at * 4096u};
    return true;
}
static void release(void *ctx, mapping_table_t table)
{
    (void)ctx;
    CHECK(table.cpu == &backing[(table.physical - 0x80000000) / 4096]);
    ++released;
}
static void ranges(void)
{
    mapping_range_t range = {0};
    CHECK(mapping_range_checked(0x1003, 4093, &range));
    CHECK(range.first == 0x1000 && range.pages == 1 && range.offset == 3);
    CHECK(mapping_range_checked(0x1003, 4094, &range) && range.pages == 2);
    mapping_range_t saved = range;
    CHECK(!mapping_range_checked(UINT64_MAX, 2, &range));
    CHECK(memcmp(&range, &saved, sizeof(range)) == 0);
    CHECK(!mapping_range_checked(4096, 0, &range));
    CHECK(!mapping_range_checked(UINT64_C(0x1000000000000), 4096, &range));
    CHECK(mapping_range_checked(UINT64_C(0xfffffffff000), 4096, &range));
    CHECK(range.pages == 1);
    uint64_t limit = 0;
    CHECK(dma_extent_limit(4096, 4096, UINT32_MAX, UINT64_C(0x100000000), &range, &limit));
    CHECK(limit == UINT64_C(0x100000000) && range.pages == 1);
    CHECK(dma_extent_limit(4096, 64, UINT64_MAX, UINT64_C(0xff000000), &range, &limit));
    CHECK(limit == UINT64_C(0xff000000));
    CHECK(!dma_extent_limit(4096, 4096, 4094, 8192, &range, &limit));
    CHECK(dma_extent_limit(4096, 4096, 4095, 8192, &range, &limit) && limit == 4096);
    CHECK(!dma_extent_limit(4097, 4096, 4095, 8192, &range, &limit));
    CHECK(!dma_extent_limit(4096, 4095, UINT64_MAX, 8192, &range, &limit));
    CHECK(!dma_extent_limit(4096, 4096, UINT64_MAX, 8193, &range, &limit));
    CHECK(!dma_extent_limit(0, 4096, UINT64_MAX, 8192, &range, &limit));
}
static void transactions(void)
{
    const mapping_table_ops_t ops = {allocate, release};
    for (fail_at = 0; fail_at < MAPPING_WINDOW_TABLES; ++fail_at) {
        attempts = released = 0;
        CHECK(mapping_window_construct(&window, &root, &ops, NULL,false) == MAPPING_NO_MEMORY);
        CHECK(released == fail_at && window.allocated == 0 && !window.ready);
        for (unsigned i = 0; i < 512; ++i) CHECK(root.entries[i] == 0);
    }
    attempts = released = 0; fail_at = UINT32_MAX;
    CHECK(mapping_window_construct(&window, &root, &ops, NULL,false) == MAPPING_OK);
    CHECK(root.entries[416] == (UINT64_C(0x80000000) | 3));
    CHECK(window.allocated == 514 && released == 0);
    CHECK(mapping_window_construct(&window, &root, &ops, NULL,false) == MAPPING_INVALID);
    uint64_t flags = PAGE_PRESENT | PAGE_RW | PAGE_PAT | PAGE_PWT;
    CHECK(mapping_window_install(&window, 511, 0x300000, 3, flags) == MAPPING_OK);
    CHECK(mapping_window_install(&window, 511, 0x300000, 3, flags) == MAPPING_OK);
    backing[2].entries[511] |= PAGE_ACCESSED | PAGE_DIRTY;
    CHECK(mapping_window_install(&window, 511, 0x300000, 3, flags) == MAPPING_OK);
    CHECK(mapping_window_install(&window, 511, 0x300000, 3, flags ^ PAGE_RW) == MAPPING_CONFLICT);
    CHECK(mapping_window_install(&window, 511, 0x300000, 3, flags ^ PAGE_PAT) == MAPPING_CONFLICT);
    CHECK(mapping_window_install(&window, 511, 0x301000, 3, flags) == MAPPING_CONFLICT);
    CHECK(mapping_window_install(&window, 510, 0x300000, 3, flags) == MAPPING_CONFLICT);
    CHECK(backing[2].entries[510] == 0);
    CHECK(mapping_window_remove(&window, 511, 0x301000, 3, flags) == MAPPING_CONFLICT);
    CHECK(backing[2].entries[511] != 0);
    CHECK(mapping_window_remove(&window, 511, 0x300000, 3, flags) == MAPPING_OK);
    CHECK(backing[2].entries[511] == 0 && backing[3].entries[0] == 0 && backing[3].entries[1] == 0);
    CHECK(mapping_window_remove(&window, 511, 0x300000, 3, flags) == MAPPING_CONFLICT);
    CHECK(mapping_window_install(&window, MAPPING_WINDOW_PAGES - 1, 0x800000, 1, flags) == MAPPING_OK);
    CHECK(mapping_window_install(&window, MAPPING_WINDOW_PAGES - 1, 0x800000, 2, flags) == MAPPING_INVALID);
    CHECK(mapping_window_install(&window, 0, 0x1000, 1, flags | PAGE_NX) == MAPPING_INVALID);
    CHECK(mapping_window_install(&window, 0, 0x1001, 1, flags) == MAPPING_INVALID);
    CHECK(mapping_window_install(&window, 0, UINT64_C(0xfffffffff000), 2, flags) == MAPPING_INVALID);
    uint64_t entry = 0;
    CHECK(mapping_window_entry(&window, MAPPING_WINDOW_PAGES - 1, &entry) && entry == (0x800000 | flags));
}
int main(void)
{
    ranges(); transactions();
    printf("OWNED_MAPPING_HOST checks=%u status=PASS\n", checks);
    return 0;
}
