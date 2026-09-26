#include "bitmap.h"
#include "bits.h"
#include "../libc/memory.h"

static uint64_t bitmap_load_word(const Bitmap *bitmap, size_t word_index)
{
    uint64_t word = 0;
    size_t byte_index = word_index * sizeof(word);
    if (byte_index >= bitmap->size)
        return 0;

    size_t available = bitmap->size - byte_index;
    if (available > sizeof(word))
        available = sizeof(word);
    for (size_t i = 0; i < available; i++)
        word |= (uint64_t)bitmap->buffer[byte_index + i] << (i * 8u);
    return word;
}

static void bitmap_store_word(Bitmap *bitmap, size_t word_index,
                              uint64_t word)
{
    size_t byte_index = word_index * sizeof(word);
    if (byte_index >= bitmap->size)
        return;

    size_t available = bitmap->size - byte_index;
    if (available > sizeof(word))
        available = sizeof(word);
    for (size_t i = 0; i < available; i++)
        bitmap->buffer[byte_index + i] = (uint8_t)(word >> (i * 8u));
}

static uint64_t bitmap_valid_mask(const Bitmap *bitmap, size_t word_index)
{
    size_t base = word_index * 64u;
    if (base >= bitmap->total_bits)
        return 0;
    size_t remaining = bitmap->total_bits - base;
    if (remaining >= 64u)
        return UINT64_MAX;
    return (1ULL << remaining) - 1u;
}

static void bitmap_stats_reset(BitmapSearchStats *stats)
{
    if (stats)
        stats->words_examined = 0;
}

static void bitmap_stats_word(BitmapSearchStats *stats)
{
    if (stats)
        stats->words_examined++;
}

bool bitmap_required_bytes(size_t total_bits, size_t *out_bytes)
{
    if (!out_bytes || total_bits > SIZE_MAX - 7u)
        return false;
    *out_bytes = (total_bits + 7u) / 8u;
    return true;
}

bool bitmap_init_checked(Bitmap *bitmap, void *buffer, size_t total_bits)
{
    size_t size = 0;
    if (!bitmap || !bitmap_required_bytes(total_bits, &size) ||
        (size != 0 && !buffer))
    {
        if (bitmap)
        {
            bitmap->buffer = NULL;
            bitmap->size = 0;
            bitmap->total_bits = 0;
        }
        return false;
    }

    bitmap->buffer = (uint8_t *)buffer;
    bitmap->total_bits = total_bits;
    bitmap->size = size;
    if (size != 0)
        memset(buffer, 0, size);
    return true;
}

void bitmap_init(Bitmap *bitmap, void *buffer, size_t total_bits)
{
    (void)bitmap_init_checked(bitmap, buffer, total_bits);
}

bool bitmap_get(const Bitmap *bitmap, size_t index)
{
    if (!bitmap || index >= bitmap->total_bits)
        return false;

    size_t byte_index = index / 8;
    uint8_t bit_index = index % 8;

    return BIT_TEST(bitmap->buffer[byte_index], bit_index);
}

void bitmap_set(Bitmap *bitmap, size_t index, bool value)
{
    if (!bitmap || index >= bitmap->total_bits)
        return;

    size_t byte_index = index / 8;
    uint8_t bit_index = index % 8;

    if (value)
    {
        BIT_SET(bitmap->buffer[byte_index], bit_index);
    }
    else
    {
        BIT_CLEAR(bitmap->buffer[byte_index], bit_index);
    }
}

static bool bitmap_update_range(Bitmap *bitmap, size_t start, size_t count,
                                bool value)
{
    if (!bitmap || start > bitmap->total_bits ||
        count > bitmap->total_bits - start)
        return false;
    if (count == 0)
        return true;

    size_t cursor = start;
    size_t remaining = count;
    while (remaining != 0)
    {
        size_t word_index = cursor / 64u;
        size_t offset = cursor % 64u;
        size_t width = 64u - offset;
        if (width > remaining)
            width = remaining;

        uint64_t mask;
        if (width == 64u)
            mask = UINT64_MAX;
        else
            mask = ((1ULL << width) - 1u) << offset;

        uint64_t word = bitmap_load_word(bitmap, word_index);
        word = value ? (word | mask) : (word & ~mask);
        bitmap_store_word(bitmap, word_index, word);
        cursor += width;
        remaining -= width;
    }
    return true;
}

bool bitmap_set_range(Bitmap *bitmap, size_t start, size_t count)
{
    return bitmap_update_range(bitmap, start, count, true);
}

bool bitmap_clear_range(Bitmap *bitmap, size_t start, size_t count)
{
    return bitmap_update_range(bitmap, start, count, false);
}

bool bitmap_find_first_zero(const Bitmap *bitmap, size_t start,
                            size_t *out_index, BitmapSearchStats *stats)
{
    bitmap_stats_reset(stats);
    if (!bitmap || !out_index || start >= bitmap->total_bits)
        return false;

    size_t word_index = start / 64u;
    size_t offset = start % 64u;
    for (;; word_index++, offset = 0)
    {
        size_t base = word_index * 64u;
        if (base >= bitmap->total_bits)
            return false;

        uint64_t valid = bitmap_valid_mask(bitmap, word_index);
        uint64_t occupied = bitmap_load_word(bitmap, word_index) | ~valid;
        if (offset != 0)
            occupied |= (1ULL << offset) - 1u;
        bitmap_stats_word(stats);

        uint64_t available = ~occupied;
        if (available != 0)
        {
            size_t bit = (size_t)__builtin_ctzll(available);
            *out_index = base + bit;
            return true;
        }
    }
}

bool bitmap_find_zero_run(const Bitmap *bitmap, size_t start, size_t count,
                          size_t *out_index, BitmapSearchStats *stats)
{
    bitmap_stats_reset(stats);
    if (!bitmap || !out_index || count == 0 || start > bitmap->total_bits ||
        count > bitmap->total_bits - start)
        return false;

    size_t word_index = start / 64u;
    size_t offset = start % 64u;
    size_t run_start = 0;
    size_t run_length = 0;

    for (;; word_index++, offset = 0)
    {
        size_t base = word_index * 64u;
        if (base >= bitmap->total_bits)
            return false;

        uint64_t valid = bitmap_valid_mask(bitmap, word_index);
        uint64_t occupied = bitmap_load_word(bitmap, word_index) | ~valid;
        size_t word_limit = bitmap->total_bits - base;
        if (word_limit > 64u)
            word_limit = 64u;
        bitmap_stats_word(stats);

        size_t bit = offset;
        while (bit < word_limit)
        {
            uint64_t shifted = occupied >> bit;
            size_t zeros;
            if (shifted == 0)
                zeros = word_limit - bit;
            else
            {
                zeros = (size_t)__builtin_ctzll(shifted);
                if (zeros > word_limit - bit)
                    zeros = word_limit - bit;
            }

            if (zeros != 0)
            {
                if (run_length == 0)
                    run_start = base + bit;
                if (zeros >= count - run_length)
                {
                    *out_index = run_start;
                    return true;
                }
                run_length += zeros;
                bit += zeros;
                if (bit >= word_limit)
                    break;
            }

            shifted = occupied >> bit;
            size_t ones = shifted == UINT64_MAX
                              ? 64u - bit
                              : (size_t)__builtin_ctzll(~shifted);
            if (ones == 0)
                ones = 1;
            if (ones > word_limit - bit)
                ones = word_limit - bit;
            bit += ones;
            run_length = 0;
        }
    }
}
