#ifndef BITMAP_H
#define BITMAP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct
{
    uint8_t *buffer;
    size_t size;
    size_t total_bits;
} Bitmap;

typedef struct
{
    size_t words_examined;
} BitmapSearchStats;

bool bitmap_required_bytes(size_t total_bits, size_t *out_bytes);
bool bitmap_init_checked(Bitmap *bitmap, void *buffer, size_t total_bits);
void bitmap_init(Bitmap *bitmap, void *buffer, size_t total_bits);

bool bitmap_get(const Bitmap *bitmap, size_t index);
void bitmap_set(Bitmap *bitmap, size_t index, bool value);

bool bitmap_set_range(Bitmap *bitmap, size_t start, size_t count);
bool bitmap_clear_range(Bitmap *bitmap, size_t start, size_t count);

/* Read-only coherent-input validation: reservation implies allocation for every
 * logical bit. On well-formed dimensions, free_bits counts the entire span even
 * when that implication fails. Padding is ignored; malformed inputs return false.
 * The caller owns synchronization and the lifetime/capacity of both buffers. */
bool bitmap_validate_allocation(const Bitmap *allocated, const Bitmap *reserved,
                                 size_t *free_bits, BitmapSearchStats *stats);
/* Count an exact synchronized logical range; malformed input leaves outputs unchanged. */
bool bitmap_observe_allocation(const Bitmap *allocated, const Bitmap *reserved,
    size_t start, size_t count, size_t *free_bits, size_t *invalid_bits);

bool bitmap_find_first_zero(const Bitmap *bitmap, size_t start,
                            size_t *out_index, BitmapSearchStats *stats);
bool bitmap_find_zero_run(const Bitmap *bitmap, size_t start, size_t count,
                          size_t *out_index, BitmapSearchStats *stats);

/* Read-only first-fit search within [start, exclusive_limit). The original
 * logical span and padding are unchanged; only a valid result may be updated. */
bool bitmap_find_zero_run_below(const Bitmap *bitmap, size_t start,
    size_t exclusive_limit, size_t count, size_t *out_index, BitmapSearchStats *stats);

bool bitmap_find_aligned_run_below(const Bitmap *bitmap, size_t start,
    size_t exclusive_limit, size_t count, size_t alignment, size_t *out_index,
    BitmapSearchStats *stats);
#endif
