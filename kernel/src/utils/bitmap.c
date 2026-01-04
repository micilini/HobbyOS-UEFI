#include "bitmap.h"
#include "bits.h"
#include "../libc/memory.h"

void bitmap_init(Bitmap* bitmap, void* buffer, size_t total_bits) {
    bitmap->buffer = (uint8_t*)buffer;
    bitmap->total_bits = total_bits;
    
    bitmap->size = (total_bits + 7) / 8;
    
    
    
    memset(buffer, 0, bitmap->size);
}

bool bitmap_get(Bitmap* bitmap, size_t index) {
    if (index >= bitmap->total_bits) return false;
    
    size_t byte_index = index / 8;
    uint8_t bit_index = index % 8;
    
    return BIT_TEST(bitmap->buffer[byte_index], bit_index);
}

void bitmap_set(Bitmap* bitmap, size_t index, bool value) {
    if (index >= bitmap->total_bits) return;
    
    size_t byte_index = index / 8;
    uint8_t bit_index = index % 8;
    
    if (value) {
        BIT_SET(bitmap->buffer[byte_index], bit_index);
    } else {
        BIT_CLEAR(bitmap->buffer[byte_index], bit_index);
    }
}