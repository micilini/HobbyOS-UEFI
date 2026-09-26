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

bool bitmap_find_first_zero(const Bitmap *bitmap, size_t start,
                            size_t *out_index, BitmapSearchStats *stats);
bool bitmap_find_zero_run(const Bitmap *bitmap, size_t start, size_t count,
                          size_t *out_index, BitmapSearchStats *stats);

#endif
