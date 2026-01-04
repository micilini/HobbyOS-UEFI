#ifndef MMIO_H
#define MMIO_H

#include <stdint.h>



static inline void mmio_write8(void* p, uint8_t data) {
    *(volatile uint8_t*)(p) = data;
}

static inline uint8_t mmio_read8(void* p) {
    return *(volatile uint8_t*)(p);
}

static inline void mmio_write16(void* p, uint16_t data) {
    *(volatile uint16_t*)(p) = data;
}

static inline uint16_t mmio_read16(void* p) {
    return *(volatile uint16_t*)(p);
}

static inline void mmio_write32(void* p, uint32_t data) {
    *(volatile uint32_t*)(p) = data;
}

static inline uint32_t mmio_read32(void* p) {
    return *(volatile uint32_t*)(p);
}

static inline void mmio_write64(void* p, uint64_t data) {
    *(volatile uint64_t*)(p) = data;
}

static inline uint64_t mmio_read64(void* p) {
    return *(volatile uint64_t*)(p);
}

#endif