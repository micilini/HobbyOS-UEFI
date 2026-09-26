#include "../kernel/src/utils/bitmap.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool reference_first_zero(const uint8_t *bits, size_t total,
                                 size_t start, size_t *out)
{
    for (size_t index = start; index < total; index++)
    {
        if ((bits[index / 8u] & (uint8_t)(1u << (index % 8u))) == 0)
        {
            *out = index;
            return true;
        }
    }
    return false;
}

static bool reference_zero_run(const uint8_t *bits, size_t total,
                               size_t start, size_t count, size_t *out)
{
    if (count == 0 || start > total || count > total - start)
        return false;
    size_t run = 0;
    size_t run_start = 0;
    for (size_t index = start; index < total; index++)
    {
        bool set = (bits[index / 8u] &
                    (uint8_t)(1u << (index % 8u))) != 0;
        if (!set)
        {
            if (run == 0)
                run_start = index;
            run++;
            if (run == count)
            {
                *out = run_start;
                return true;
            }
        }
        else
        {
            run = 0;
        }
    }
    return false;
}

static void reference_set(uint8_t *bits, size_t index, bool value)
{
    uint8_t mask = (uint8_t)(1u << (index % 8u));
    if (value)
        bits[index / 8u] |= mask;
    else
        bits[index / 8u] &= (uint8_t)~mask;
}

static int compare_searches(size_t total, uint8_t *storage,
                            const uint8_t *reference)
{
    Bitmap bitmap = {storage, (total + 7u) / 8u, total};
    for (size_t start = 0; start <= total; start++)
    {
        size_t expected = SIZE_MAX;
        size_t actual = SIZE_MAX;
        BitmapSearchStats stats = {0};
        bool expected_found = reference_first_zero(reference, total, start,
                                                   &expected);
        bool actual_found = bitmap_find_first_zero(&bitmap, start, &actual,
                                                   &stats);
        if (expected_found != actual_found ||
            (actual_found && expected != actual) ||
            (actual_found && stats.words_examined == 0))
            return 1;
    }

    size_t limit = total < 130u ? total : 130u;
    for (size_t count = 1; count <= limit; count++)
    {
        for (size_t start = 0; start <= total; start++)
        {
            size_t expected = SIZE_MAX;
            size_t actual = SIZE_MAX;
            BitmapSearchStats stats = {0};
            bool expected_found = reference_zero_run(reference, total, start,
                                                     count, &expected);
            bool actual_found = bitmap_find_zero_run(&bitmap, start, count,
                                                     &actual, &stats);
            if (expected_found != actual_found ||
                (actual_found && expected != actual) ||
                (start < total && count <= total - start &&
                 stats.words_examined == 0))
                return 2;
        }
    }
    return 0;
}

static int exercise_size(size_t total)
{
    size_t bytes = 0;
    if (!bitmap_required_bytes(total, &bytes) || bytes == 0)
        return 10;
    uint8_t *storage = malloc(bytes);
    uint8_t *reference = malloc(bytes);
    if (!storage || !reference)
        return 11;

    Bitmap bitmap;
    for (unsigned pattern = 0; pattern < 3u; pattern++)
    {
        memset(storage, pattern == 1u ? 0xff : 0, bytes);
        memset(reference, pattern == 1u ? 0xff : 0, bytes);
        if (pattern == 2u)
        {
            for (size_t index = 0; index < total; index++)
            {
                bool value = ((index * 1103515245u + 12345u) % 17u) < 7u;
                reference_set(storage, index, value);
                reference_set(reference, index, value);
            }
        }
        int rc = compare_searches(total, storage, reference);
        if (rc != 0)
        {
            free(reference);
            free(storage);
            return 20 + rc;
        }
    }

    if (!bitmap_init_checked(&bitmap, storage, total))
    {
        free(reference);
        free(storage);
        return 30;
    }
    memset(reference, 0, bytes);
    for (size_t start = 0; start < total; start += 31u)
    {
        size_t count = total - start;
        if (count > 97u)
            count = 97u;
        if (!bitmap_set_range(&bitmap, start, count))
            return 31;
        for (size_t index = start; index < start + count; index++)
            reference_set(reference, index, true);
        if (memcmp(storage, reference, bytes) != 0)
            return 32;
        size_t clear_start = start + count / 3u;
        size_t clear_count = count / 2u;
        if (!bitmap_clear_range(&bitmap, clear_start, clear_count))
            return 33;
        for (size_t index = clear_start;
             index < clear_start + clear_count; index++)
            reference_set(reference, index, false);
        if (memcmp(storage, reference, bytes) != 0)
            return 34;
    }
    int rc = compare_searches(total, storage, reference);
    free(reference);
    free(storage);
    return rc == 0 ? 0 : 40 + rc;
}

int main(void)
{
    static const size_t sizes[] = {1u, 63u, 64u, 65u, 4095u, 4096u,
                                   4097u};
    size_t bytes = 0;
    if (bitmap_required_bytes(SIZE_MAX, &bytes) ||
        bitmap_init_checked(NULL, NULL, 1u))
        return 1;

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
    {
        int rc = exercise_size(sizes[i]);
        if (rc != 0)
        {
            fprintf(stderr, "bitmap size=%zu failed rc=%d\n", sizes[i], rc);
            return rc;
        }
    }

    puts("[ALLOCTEST][HOST] PASS sizes=7 patterns=3 reference_match=1 word_search=1 ranges=1 overflow=1");
    return 0;
}
