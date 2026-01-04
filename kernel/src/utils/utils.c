#include "utils.h"

void* k_memset(void* ptr, int value, size_t num) {
    unsigned char* p = (unsigned char*)ptr;
    while (num--) {
        *p++ = (unsigned char)value;
    }
    return ptr;
}

void* k_memcpy(void* dest, const void* src, size_t n) {
    char* d = (char*)dest;
    const char* s = (const char*)src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

void k_delay(uint64_t loops) {
    for (volatile uint64_t i = 0; i < loops; i++) {
        __asm__("nop");
    }
}