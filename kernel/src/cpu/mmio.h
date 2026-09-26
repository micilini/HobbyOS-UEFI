#ifndef MMIO_H
#define MMIO_H

#include <stdbool.h>
#include <stdint.h>

#ifndef HOBBYOS_DEBUG_ASSERT
#define HOBBYOS_DEBUG_ASSERT 0
#endif

typedef enum
{
    MMIO_TRACE_OPERATION_READ = 0,
    MMIO_TRACE_OPERATION_WRITE = 1
} mmio_trace_operation_t;

typedef enum
{
    MMIO_TRACE_PHASE_NONE = 0,
    MMIO_TRACE_PHASE_ATTEMPT = 1,
    MMIO_TRACE_PHASE_COMPLETE = 2
} mmio_trace_phase_t;

typedef struct
{
    uint64_t address;
    uint64_t value;
    uint32_t cpu_id;
    uint32_t slot;
    uint8_t width;
    uint8_t operation;
    uint8_t phase;
    uint8_t value_valid;
    uint8_t cpu_valid;
    uint8_t slot_valid;
    uint8_t coherent;
    uint32_t sequence;
} mmio_trace_snapshot_t;

#if HOBBYOS_DEBUG_ASSERT
typedef struct
{
    uint64_t irq_flags;
    uint32_t cpu_id;
    uint32_t slot;
    uint8_t active;
    uint8_t pin_owned;
} mmio_trace_token_t;

mmio_trace_token_t mmio_trace_begin(void *address, uint8_t width,
                                    mmio_trace_operation_t operation,
                                    uint64_t value, bool value_valid);
void mmio_trace_complete(mmio_trace_token_t token, uint64_t value,
                         bool value_valid);
bool mmio_trace_snapshot_current(mmio_trace_snapshot_t *snapshot,
                                 uint32_t attempt_budget);
#if defined(HOBBYOS_ARCH_TEST)
bool mmio_trace_snapshot_slot_for_test(uint32_t slot,
                                       mmio_trace_snapshot_t *snapshot,
                                       uint32_t attempt_budget);
#endif
#endif

#if defined(HOBBYOS_ARCH_HOST_TEST)
void arch_host_mmio_access_point(void);
#define MMIO_ARCH_ACCESS_POINT() arch_host_mmio_access_point()
#else
#define MMIO_ARCH_ACCESS_POINT() ((void)0)
#endif

/*
 * Ordered accessors put an x86 MFENCE, with a compiler memory clobber, on
 * both sides of exactly one volatile access. This orders earlier and later
 * CPU memory operations around the access. It does not select the mapping's
 * cache type, make an unaligned address valid, or flush a posted device write.
 *
 * Relaxed accessors perform exactly one volatile access without a compiler
 * memory barrier or architectural fence. Callers must provide any ordering
 * their device protocol requires. All accessors require a live, suitably
 * mapped and naturally aligned address of the requested width.
 */
static inline void mmio_order_boundary(void)
{
    __asm__ volatile("mfence" : : : "memory");
}

static inline uint8_t mmio_read8_relaxed(void *address)
{
#if HOBBYOS_DEBUG_ASSERT
    mmio_trace_token_t trace = mmio_trace_begin(
        address, 1, MMIO_TRACE_OPERATION_READ, 0, false);
#endif
    MMIO_ARCH_ACCESS_POINT();
    uint8_t value = *(volatile uint8_t *)address;
#if HOBBYOS_DEBUG_ASSERT
    mmio_trace_complete(trace, value, true);
#endif
    return value;
}

static inline uint16_t mmio_read16_relaxed(void *address)
{
#if HOBBYOS_DEBUG_ASSERT
    mmio_trace_token_t trace = mmio_trace_begin(
        address, 2, MMIO_TRACE_OPERATION_READ, 0, false);
#endif
    MMIO_ARCH_ACCESS_POINT();
    uint16_t value = *(volatile uint16_t *)address;
#if HOBBYOS_DEBUG_ASSERT
    mmio_trace_complete(trace, value, true);
#endif
    return value;
}

static inline uint32_t mmio_read32_relaxed(void *address)
{
#if HOBBYOS_DEBUG_ASSERT
    mmio_trace_token_t trace = mmio_trace_begin(
        address, 4, MMIO_TRACE_OPERATION_READ, 0, false);
#endif
    MMIO_ARCH_ACCESS_POINT();
    uint32_t value = *(volatile uint32_t *)address;
#if HOBBYOS_DEBUG_ASSERT
    mmio_trace_complete(trace, value, true);
#endif
    return value;
}

static inline uint64_t mmio_read64_relaxed(void *address)
{
#if HOBBYOS_DEBUG_ASSERT
    mmio_trace_token_t trace = mmio_trace_begin(
        address, 8, MMIO_TRACE_OPERATION_READ, 0, false);
#endif
    MMIO_ARCH_ACCESS_POINT();
    uint64_t value = *(volatile uint64_t *)address;
#if HOBBYOS_DEBUG_ASSERT
    mmio_trace_complete(trace, value, true);
#endif
    return value;
}

static inline void mmio_write8_relaxed(void *address, uint8_t value)
{
#if HOBBYOS_DEBUG_ASSERT
    mmio_trace_token_t trace = mmio_trace_begin(
        address, 1, MMIO_TRACE_OPERATION_WRITE, value, true);
#endif
    MMIO_ARCH_ACCESS_POINT();
    *(volatile uint8_t *)address = value;
#if HOBBYOS_DEBUG_ASSERT
    mmio_trace_complete(trace, value, true);
#endif
}

static inline void mmio_write16_relaxed(void *address, uint16_t value)
{
#if HOBBYOS_DEBUG_ASSERT
    mmio_trace_token_t trace = mmio_trace_begin(
        address, 2, MMIO_TRACE_OPERATION_WRITE, value, true);
#endif
    MMIO_ARCH_ACCESS_POINT();
    *(volatile uint16_t *)address = value;
#if HOBBYOS_DEBUG_ASSERT
    mmio_trace_complete(trace, value, true);
#endif
}

static inline void mmio_write32_relaxed(void *address, uint32_t value)
{
#if HOBBYOS_DEBUG_ASSERT
    mmio_trace_token_t trace = mmio_trace_begin(
        address, 4, MMIO_TRACE_OPERATION_WRITE, value, true);
#endif
    MMIO_ARCH_ACCESS_POINT();
    *(volatile uint32_t *)address = value;
#if HOBBYOS_DEBUG_ASSERT
    mmio_trace_complete(trace, value, true);
#endif
}

static inline void mmio_write64_relaxed(void *address, uint64_t value)
{
#if HOBBYOS_DEBUG_ASSERT
    mmio_trace_token_t trace = mmio_trace_begin(
        address, 8, MMIO_TRACE_OPERATION_WRITE, value, true);
#endif
    MMIO_ARCH_ACCESS_POINT();
    *(volatile uint64_t *)address = value;
#if HOBBYOS_DEBUG_ASSERT
    mmio_trace_complete(trace, value, true);
#endif
}

static inline uint8_t mmio_read8(void *address)
{
    mmio_order_boundary();
    uint8_t value = mmio_read8_relaxed(address);
    mmio_order_boundary();
    return value;
}

static inline uint16_t mmio_read16(void *address)
{
    mmio_order_boundary();
    uint16_t value = mmio_read16_relaxed(address);
    mmio_order_boundary();
    return value;
}

static inline uint32_t mmio_read32(void *address)
{
    mmio_order_boundary();
    uint32_t value = mmio_read32_relaxed(address);
    mmio_order_boundary();
    return value;
}

static inline uint64_t mmio_read64(void *address)
{
    mmio_order_boundary();
    uint64_t value = mmio_read64_relaxed(address);
    mmio_order_boundary();
    return value;
}

static inline void mmio_write8(void *address, uint8_t value)
{
    mmio_order_boundary();
    mmio_write8_relaxed(address, value);
    mmio_order_boundary();
}

static inline void mmio_write16(void *address, uint16_t value)
{
    mmio_order_boundary();
    mmio_write16_relaxed(address, value);
    mmio_order_boundary();
}

static inline void mmio_write32(void *address, uint32_t value)
{
    mmio_order_boundary();
    mmio_write32_relaxed(address, value);
    mmio_order_boundary();
}

static inline void mmio_write64(void *address, uint64_t value)
{
    mmio_order_boundary();
    mmio_write64_relaxed(address, value);
    mmio_order_boundary();
}

#endif
