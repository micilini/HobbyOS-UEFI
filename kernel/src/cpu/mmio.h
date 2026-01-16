#ifndef MMIO_H
#define MMIO_H

#include <stdint.h>

extern volatile uint64_t g_last_mmio_addr;
extern volatile uint64_t g_last_mmio_value;
extern volatile uint8_t g_last_mmio_size;
extern volatile uint8_t g_last_mmio_is_write;

static inline void mmio_trace_read(void *addr, uint8_t size)
{
    g_last_mmio_addr = (uint64_t)(uintptr_t)addr;
    g_last_mmio_size = size;
    g_last_mmio_is_write = 0;
}
static inline void mmio_trace_write(void *addr, uint64_t val, uint8_t size)
{
    g_last_mmio_addr = (uint64_t)(uintptr_t)addr;
    g_last_mmio_value = val;
    g_last_mmio_size = size;
    g_last_mmio_is_write = 1;
}

static inline uint8_t mmio_read8(void *addr)
{
    mmio_trace_read(addr, 1);
    volatile uint8_t *p = (volatile uint8_t *)addr;
    uint8_t v = *p;
    g_last_mmio_value = v;
    return v;
}

static inline uint16_t mmio_read16(void *addr)
{
    mmio_trace_read(addr, 2);
    volatile uint16_t *p = (volatile uint16_t *)addr;
    uint16_t v = *p;
    g_last_mmio_value = v;
    return v;
}

static inline uint32_t mmio_read32(void *addr)
{
    mmio_trace_read(addr, 4);
    volatile uint32_t *p = (volatile uint32_t *)addr;
    uint32_t v = *p;
    g_last_mmio_value = v;
    return v;
}

static inline uint64_t mmio_read64(void *addr)
{
    mmio_trace_read(addr, 8);
    volatile uint64_t *p = (volatile uint64_t *)addr;
    uint64_t v = *p;
    g_last_mmio_value = v;
    return v;
}

static inline void mmio_write8(void *addr, uint8_t value)
{
    mmio_trace_write(addr, value, 1);
    volatile uint8_t *p = (volatile uint8_t *)addr;
    *p = value;
}

static inline void mmio_write16(void *addr, uint16_t value)
{
    mmio_trace_write(addr, value, 2);
    volatile uint16_t *p = (volatile uint16_t *)addr;
    *p = value;
}

static inline void mmio_write32(void *addr, uint32_t value)
{
    mmio_trace_write(addr, value, 4);
    volatile uint32_t *p = (volatile uint32_t *)addr;
    *p = value;
}

static inline void mmio_write64(void *addr, uint64_t value)
{
    mmio_trace_write(addr, value, 8);
    volatile uint64_t *p = (volatile uint64_t *)addr;
    *p = value;
}

#endif