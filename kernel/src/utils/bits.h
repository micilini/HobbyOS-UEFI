#ifndef BITS_H
#define BITS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BIT_SET(x, n) ((x) |= (1ULL << (n)))
#define BIT_CLEAR(x, n) ((x) &= ~(1ULL << (n)))
#define BIT_TEST(x, n) (((x) & (1ULL << (n))) != 0)
#define BIT_TOGGLE(x, n) ((x) ^= (1ULL << (n)))

static inline bool alignment_is_power_of_two_u64(uint64_t alignment)
{
    return alignment != 0 && (alignment & (alignment - 1u)) == 0;
}

static inline bool align_up_u64_checked(uint64_t value, uint64_t alignment,
                                        uint64_t *out)
{
    if (!out || !alignment_is_power_of_two_u64(alignment) ||
        value > UINT64_MAX - (alignment - 1u))
        return false;

    *out = (value + (alignment - 1u)) & ~(alignment - 1u);
    return true;
}

static inline bool align_up_size_checked(size_t value, size_t alignment,
                                         size_t *out)
{
    if (!out || alignment == 0 || (alignment & (alignment - 1u)) != 0 ||
        value > SIZE_MAX - (alignment - 1u))
        return false;

    *out = (value + (alignment - 1u)) & ~(alignment - 1u);
    return true;
}

static inline uint64_t align_up_u64_or_zero(uint64_t value,
                                             uint64_t alignment)
{
    uint64_t result = 0;
    return align_up_u64_checked(value, alignment, &result) ? result : 0;
}

static inline uint64_t align_down_u64_or_zero(uint64_t value,
                                               uint64_t alignment)
{
    if (!alignment_is_power_of_two_u64(alignment))
        return 0;
    return value & ~(alignment - 1u);
}

/*
 * Compatibility helpers return zero when alignment is zero/non-power-of-two
 * or ALIGN_UP would overflow. New code that must distinguish a valid zero
 * result uses align_up_*_checked directly.
 */
#define ALIGN_UP(x, align) align_up_u64_or_zero((uint64_t)(x), (uint64_t)(align))
#define ALIGN_DOWN(x, align) align_down_u64_or_zero((uint64_t)(x), (uint64_t)(align))

#endif
