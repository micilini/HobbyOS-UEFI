#ifndef K_UTILS_H
#define K_UTILS_H

#include <stdint.h>
#include <stddef.h>

void *k_memset(void *ptr, int value, size_t num);
void *k_memcpy(void *dest, const void *src, size_t n);

void k_delay(uint64_t loops);

#endif