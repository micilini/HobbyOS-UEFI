#include "cmd_alloctest.h"

#include "../../drivers/serial.h"
#include "../../libc/memory.h"
#include "../../libc/string.h"
#include "../../memory/heap.h"
#include "../../memory/pmem.h"
#include "../../utils/bitmap.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef HOBBYOS_SELFTEST

static uint64_t g_alloctest_foreign_object;

static void alloctest_write_u64(uint64_t value)
{
    char buffer[21];
    uint32_t count = 0;
    do
    {
        buffer[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    while (count != 0)
        serial_putc_all(buffer[--count]);
}

static bool heap_stats_equal(const HeapStats *left, const HeapStats *right)
{
    return left->total_bytes == right->total_bytes &&
           left->used_bytes == right->used_bytes &&
           left->free_bytes == right->free_bytes &&
           left->blocks_total == right->blocks_total &&
           left->blocks_free == right->blocks_free &&
           left->largest_free_bytes == right->largest_free_bytes;
}

static bool alloctest_invalid_free(void *pointer,
                                   uint64_t expected_interior,
                                   uint64_t expected_outside,
                                   uint64_t expected_double)
{
    HeapHardeningStats counters_before;
    HeapHardeningStats counters_after;
    if (!heap_get_hardening_stats(&counters_before))
        return false;
    bool rejected = !heap_test_free(pointer);
    if (!heap_get_hardening_stats(&counters_after))
        return false;
    return rejected &&
           counters_after.free_interior ==
               counters_before.free_interior + expected_interior &&
           counters_after.free_outside_heap ==
               counters_before.free_outside_heap + expected_outside &&
           counters_after.double_free ==
               counters_before.double_free + expected_double &&
           heap_validate_integrity();
}

static bool alloctest_heap(HeapHardeningStats *delta, bool *census_ok)
{
    HeapStats baseline;
    HeapStats final;
    HeapHardeningStats before;
    HeapHardeningStats after;
    if (!heap_get_stats(&baseline) || !heap_get_hardening_stats(&before))
        return false;

    bool overflow_ok = kmalloc(SIZE_MAX) == NULL &&
                       kmalloc_aligned(SIZE_MAX - 8u, 64u) == NULL;

    void *interior = kmalloc(128u);
    bool interior_alloc = interior != NULL;
    bool interior_reject = false;
    if (interior)
    {
        interior_reject = alloctest_invalid_free(
            (uint8_t *)interior + 1u, 1u, 0u, 0u);
        kfree(interior);
    }

    bool foreign_reject = alloctest_invalid_free(
        &g_alloctest_foreign_object, 0u, 1u, 0u);

    void *before_a = kmalloc(80u);
    void *before_b = kmalloc(80u);
    void *before_c = kmalloc(80u);
    bool before_alloc = before_a && before_b && before_c;
    bool before_double = false;
    if (before_a && before_b && before_c)
    {
        kfree(before_b);
        before_double = alloctest_invalid_free(before_b, 0u, 0u, 1u);
        kfree(before_a);
        kfree(before_c);
    }

    void *after_a = kmalloc(96u);
    void *after_b = kmalloc(96u);
    void *after_c = kmalloc(96u);
    bool after_alloc = after_a && after_b && after_c;
    bool after_double = false;
    if (after_a && after_b && after_c)
    {
        kfree(after_a);
        kfree(after_b);
        after_double = alloctest_invalid_free(after_b, 0u, 0u, 1u);
        kfree(after_c);
    }

    void *aligned = kmalloc_aligned(257u, 256u);
    bool aligned_ok = aligned && ((uintptr_t)aligned % 256u) == 0;
    if (aligned)
        kfree(aligned);

    bool got_after = heap_get_hardening_stats(&after);
    bool got_final = heap_get_stats(&final);
    bool integrity = heap_validate_integrity();
    bool exact_census = got_final && heap_stats_equal(&baseline, &final);
    *census_ok = got_final && integrity &&
                 final.total_bytes == baseline.total_bytes &&
                 final.used_bytes <= final.total_bytes &&
                 final.blocks_free <= final.blocks_total;
    bool ok = overflow_ok && interior_alloc && interior_reject &&
              foreign_reject && before_alloc && before_double &&
              after_alloc && after_double && aligned_ok && got_after &&
              got_final && integrity;
    serial_write_all("[ALLOCTEST][HEAP_CHECKS] overflow=");
    alloctest_write_u64(overflow_ok);
    serial_write_all(" interior_alloc=");
    alloctest_write_u64(interior_alloc);
    serial_write_all(" interior_reject=");
    alloctest_write_u64(interior_reject);
    serial_write_all(" foreign_reject=");
    alloctest_write_u64(foreign_reject);
    serial_write_all(" before_alloc=");
    alloctest_write_u64(before_alloc);
    serial_write_all(" before_double=");
    alloctest_write_u64(before_double);
    serial_write_all(" after_alloc=");
    alloctest_write_u64(after_alloc);
    serial_write_all(" after_double=");
    alloctest_write_u64(after_double);
    serial_write_all(" aligned=");
    alloctest_write_u64(aligned_ok);
    serial_write_all(" integrity=");
    alloctest_write_u64(integrity);
    serial_write_all("\n");
    if (got_final)
    {
        serial_write_all("[ALLOCTEST][HEAP_CENSUS] baseline_used=");
        alloctest_write_u64(baseline.used_bytes);
        serial_write_all(" final_used=");
        alloctest_write_u64(final.used_bytes);
        serial_write_all(" baseline_blocks=");
        alloctest_write_u64(baseline.blocks_total);
        serial_write_all(" final_blocks=");
        alloctest_write_u64(final.blocks_total);
        serial_write_all(" baseline_free_blocks=");
        alloctest_write_u64(baseline.blocks_free);
        serial_write_all(" final_free_blocks=");
        alloctest_write_u64(final.blocks_free);
        serial_write_all(" exact=");
        alloctest_write_u64(exact_census);
        serial_write_all("\n");
    }
    if (!got_after)
        return false;
    *delta = (HeapHardeningStats){
        .allocation_overflow =
            after.allocation_overflow - before.allocation_overflow,
        .invalid_alignment =
            after.invalid_alignment - before.invalid_alignment,
        .free_outside_heap =
            after.free_outside_heap - before.free_outside_heap,
        .free_interior = after.free_interior - before.free_interior,
        .double_free = after.double_free - before.double_free,
    };
    return ok && *census_ok && delta->allocation_overflow == 2u &&
           delta->invalid_alignment == 0u &&
           delta->free_outside_heap == 1u &&
           delta->free_interior == 1u && delta->double_free == 2u;
}

static bool alloctest_pmm(PmmHardeningStats *delta, bool *census_ok)
{
    uint64_t free_before = pmm_get_free_memory();
    PmmHardeningStats before;
    PmmHardeningStats after;
    if (!pmm_get_hardening_stats(&before))
        return false;

    void *frame = pmm_alloc_frame();
    bool ok = frame != NULL && pmm_get_free_memory() + PAGE_SIZE == free_before;
    if (!frame)
        return false;

    pmm_free_frame(NULL);
    pmm_free_frame((uint8_t *)frame + 1u);
    pmm_free_frame((void *)(uintptr_t)pmm_get_total_memory());
    pmm_free_frame(pmm_test_reserved_frame());
    ok = ok && pmm_get_free_memory() + PAGE_SIZE == free_before;

    pmm_free_frame(frame);
    ok = ok && pmm_get_free_memory() == free_before;
    pmm_free_frame(frame);
    *census_ok = pmm_get_free_memory() == free_before &&
                 pmm_validate_integrity();
    bool got_after = pmm_get_hardening_stats(&after);
    ok = ok && got_after;
    if (!got_after)
        return false;

    *delta = (PmmHardeningStats){
        .reject_null = after.reject_null - before.reject_null,
        .reject_unaligned =
            after.reject_unaligned - before.reject_unaligned,
        .reject_out_of_span =
            after.reject_out_of_span - before.reject_out_of_span,
        .reject_reserved = after.reject_reserved - before.reject_reserved,
        .reject_already_free =
            after.reject_already_free - before.reject_already_free,
        .word_search_calls =
            after.word_search_calls - before.word_search_calls,
        .word_iterations =
            after.word_iterations - before.word_iterations,
    };
    return ok && *census_ok && delta->reject_null == 1u &&
           delta->reject_unaligned == 1u &&
           delta->reject_out_of_span == 1u &&
           delta->reject_reserved == 1u &&
           delta->reject_already_free == 1u &&
           delta->word_search_calls >= 1u &&
           delta->word_iterations >= delta->word_search_calls;
}

static bool alloctest_bitmap(uint64_t *words_examined)
{
    uint8_t storage[17];
    Bitmap bitmap;
    if (!bitmap_init_checked(&bitmap, storage, 130u) ||
        !bitmap_set_range(&bitmap, 0, 129u))
        return false;

    size_t found = 0;
    BitmapSearchStats stats = {0};
    bool ok = bitmap_find_first_zero(&bitmap, 0, &found, &stats) &&
              found == 129u && stats.words_examined == 3u;
    *words_examined = stats.words_examined;

    ok = ok && bitmap_set_range(&bitmap, 129u, 1u) &&
         !bitmap_find_first_zero(&bitmap, 0, &found, &stats) &&
         !bitmap_find_zero_run(&bitmap, 0, 1u, &found, &stats) &&
         bitmap_clear_range(&bitmap, 63u, 3u) &&
         bitmap_find_zero_run(&bitmap, 0, 3u, &found, &stats) &&
         found == 63u;
    size_t bytes = 0;
    return ok && !bitmap_required_bytes(SIZE_MAX, &bytes);
}

static int alloctest_hardening(void)
{
    HeapHardeningStats heap_delta = {0};
    PmmHardeningStats pmm_delta = {0};
    bool heap_census = false;
    bool pmm_census = false;
    uint64_t bitmap_words = 0;
    bool heap_ok = alloctest_heap(&heap_delta, &heap_census);
    bool pmm_ok = alloctest_pmm(&pmm_delta, &pmm_census);
    bool bitmap_ok = alloctest_bitmap(&bitmap_words);
    bool ok = heap_ok && pmm_ok && bitmap_ok;

    serial_write_all("[ALLOCTEST][HARDENING] ");
    serial_write_all(ok ? "PASS" : "FAIL");
    serial_write_all(" heap_overflow=");
    alloctest_write_u64(heap_delta.allocation_overflow);
    serial_write_all(" heap_interior=");
    alloctest_write_u64(heap_delta.free_interior);
    serial_write_all(" heap_foreign=");
    alloctest_write_u64(heap_delta.free_outside_heap);
    serial_write_all(" heap_double=");
    alloctest_write_u64(heap_delta.double_free);
    serial_write_all(" pmm_null=");
    alloctest_write_u64(pmm_delta.reject_null);
    serial_write_all(" pmm_unaligned=");
    alloctest_write_u64(pmm_delta.reject_unaligned);
    serial_write_all(" pmm_outside=");
    alloctest_write_u64(pmm_delta.reject_out_of_span);
    serial_write_all(" pmm_reserved=");
    alloctest_write_u64(pmm_delta.reject_reserved);
    serial_write_all(" pmm_double=");
    alloctest_write_u64(pmm_delta.reject_already_free);
    serial_write_all(" bitmap_words=");
    alloctest_write_u64(bitmap_words);
    serial_write_all(" pmm_word_calls=");
    alloctest_write_u64(pmm_delta.word_search_calls);
    serial_write_all(" pmm_word_iterations=");
    alloctest_write_u64(pmm_delta.word_iterations);
    serial_write_all(" heap_integrity=");
    alloctest_write_u64(heap_validate_integrity());
    serial_write_all(" pmm_integrity=");
    alloctest_write_u64(pmm_validate_integrity());
    serial_write_all(" census=");
    alloctest_write_u64(heap_census && pmm_census);
    serial_write_all(" heap_ok=");
    alloctest_write_u64(heap_ok);
    serial_write_all(" pmm_ok=");
    alloctest_write_u64(pmm_ok);
    serial_write_all(" bitmap_ok=");
    alloctest_write_u64(bitmap_ok);
    serial_write_all("\n");
    return ok ? 0 : 1;
}

int cmd_alloctest(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "hardening") != 0)
    {
        serial_write_all(
            "[ALLOCTEST][HARDENING] FAIL reason=usage\n");
        return 1;
    }
    return alloctest_hardening();
}

#else

int cmd_alloctest(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 1;
}

#endif
