#include "../kernel/src/utils/bitmap.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Independent per-bit oracle exists only in this hosted test. */
static bool reference(const Bitmap *a, const Bitmap *r, size_t *free_bits)
{
    bool valid = true;
    *free_bits = 0;
    for (size_t bit = 0; bit < a->total_bits; bit++)
    {
        unsigned mask = 1u << (bit % 8u);
        bool free = !(a->buffer[bit / 8u] & mask);
        if (free) (*free_bits)++;
        if (free && (r->buffer[bit / 8u] & mask)) valid = false;
    }
    return valid;
}

static size_t checks;
static void compare(Bitmap a, Bitmap r)
{
    size_t expected, actual = SIZE_MAX;
    uint8_t *a_copy = malloc(a.size ? a.size : 1);
    uint8_t *r_copy = malloc(r.size ? r.size : 1);
    assert(a_copy && r_copy);
    if (a.size) memcpy(a_copy, a.buffer, a.size);
    if (r.size) memcpy(r_copy, r.buffer, r.size);
    bool valid = reference(&a, &r, &expected);
    BitmapSearchStats stats = {SIZE_MAX};
    assert(bitmap_validate_allocation(&a, &r, &actual, &stats) == valid);
    assert(actual == expected);
    assert(stats.words_examined == a.total_bits / 64u + !!(a.total_bits % 64u));
    assert(!a.size || memcmp(a_copy, a.buffer, a.size) == 0);
    assert(!r.size || memcmp(r_copy, r.buffer, r.size) == 0);
    free(a_copy); free(r_copy); checks++;
}

static uint64_t rng = UINT64_C(0x42973258ef9438ab);
static uint8_t random_byte(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint8_t)rng;
}

static void boundaries(void)
{
    const size_t sizes[] = {0, 1, 7, 8, 9, 63, 64, 65, 127, 128, 129, 8197};
    for (size_t s = 0; s < sizeof(sizes) / sizeof(*sizes); s++)
    for (size_t offset = 0; offset < 8; offset++)
    {
        size_t bits = sizes[s], bytes = (bits + 7) / 8;
        /* Exact end-of-allocation tails make any overread visible to ASan. */
        uint8_t *ab = malloc(bytes + offset + 1), *rb = malloc(bytes + offset + 1);
        assert(ab && rb);
        memset(ab, 0xa5, bytes + offset + 1); memset(rb, 0x5a, bytes + offset + 1);
        Bitmap a = {ab + offset + 1, bytes, bits}, r = {rb + offset + 1, bytes, bits};
        for (unsigned pattern = 0; pattern < 48; pattern++)
        {
            for (size_t i = 0; i < bytes; i++)
            {
                a.buffer[i] = pattern == 0 ? 255 : pattern == 1 ? 0 : pattern == 2 ? 0xaa : random_byte();
                r.buffer[i] = pattern == 0 ? 255 : pattern == 1 ? 0 : random_byte();
                if (pattern & 1) r.buffer[i] &= a.buffer[i];
            }
            compare(a, r);
        }
        if (bits)
        {
            size_t positions[] = {0, bits / 2, bits > 64 ? 64 : bits - 1, bits - 1};
            for (size_t p = 0; p < 4; p++)
            {
                memset(a.buffer, 255, bytes); memset(r.buffer, 0, bytes);
                size_t bit = positions[p];
                a.buffer[bit / 8] &= (uint8_t)~(1u << (bit % 8));
                compare(a, r); /* A single free frame, then reserved/free corruption. */
                r.buffer[bit / 8] |= (uint8_t)(1u << (bit % 8));
                compare(a, r);
            }
        }
        for (size_t i = 0; i <= offset; i++) assert(ab[i] == 0xa5 && rb[i] == 0x5a);
        free(ab); free(rb);
    }
}

static void malformed(void)
{
    uint8_t byte = 0xff;
    Bitmap a = {&byte, 1, 8}, r = a;
    size_t count = 77;
    assert(!bitmap_validate_allocation(NULL, &r, &count, NULL));
    assert(!bitmap_validate_allocation(&a, NULL, &count, NULL));
    assert(!bitmap_validate_allocation(&a, &r, NULL, NULL));
    r.buffer = NULL; assert(!bitmap_validate_allocation(&a, &r, &count, NULL));
    r = a; a.buffer = NULL; assert(!bitmap_validate_allocation(&a, &r, &count, NULL));
    a = r; r.total_bits = 7; assert(!bitmap_validate_allocation(&a, &r, &count, NULL));
    r = a; r.size = 0; assert(!bitmap_validate_allocation(&a, &r, &count, NULL));
    r = a; a.size = r.size = 0; assert(!bitmap_validate_allocation(&a, &r, &count, NULL));
    a.size = r.size = 2; assert(!bitmap_validate_allocation(&a, &r, &count, NULL));
    a.total_bits = r.total_bits = SIZE_MAX; assert(!bitmap_validate_allocation(&a, &r, &count, NULL));
    a = (Bitmap){NULL, 0, 0}; r = a; compare(a, r);
}

int main(void)
{
    boundaries(); malformed();
    for (unsigned bits = 0; bits <= 8; bits++)
    for (unsigned aa = 0; aa < (1u << bits); aa++)
    for (unsigned rr = 0; rr < (1u << bits); rr++)
    {
        uint8_t ab = (uint8_t)aa, rb = (uint8_t)rr;
        compare((Bitmap){&ab, !!bits, bits}, (Bitmap){&rb, !!bits, bits});
    }
    size_t bits = 268435456, bytes = bits / 8;
    uint8_t *ab = malloc(bytes), *rb = malloc(bytes);
    assert(ab && rb);
    memset(ab, 255, bytes); memset(rb, 255, bytes);
    Bitmap a = {ab, bytes, bits}, r = {rb, bytes, bits};
    compare(a, r);
    ab[bytes - 1] &= 0x7f; compare(a, r); /* Violation at the very last logical bit. */
    rb[bytes - 1] &= 0x7f; compare(a, r);
    size_t count; BitmapSearchStats stats;
    clock_t start = clock(); bool old_valid = reference(&a, &r, &count);
    double old_seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
    start = clock(); bool new_valid = bitmap_validate_allocation(&a, &r, &count, &stats);
    double new_seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
    assert(old_valid && new_valid && count == 1 && stats.words_examined == bits / 64);
    printf("{\"status\":\"PASS\",\"checks\":%zu,\"full_span_bits\":%zu,\"words\":%zu,"
           "\"timing_scope\":\"host_process_cpu_time_not_guest_irq_latency\","
           "\"reference_seconds\":%.6f,\"word_seconds\":%.6f}\n", checks, bits, stats.words_examined, old_seconds, new_seconds);
    free(ab); free(rb);
    return 0;
}
