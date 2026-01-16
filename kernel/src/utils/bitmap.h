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

void bitmap_init(Bitmap *bitmap, void *buffer, size_t total_bits);

bool bitmap_get(Bitmap *bitmap, size_t index);
void bitmap_set(Bitmap *bitmap, size_t index, bool value);

#endif