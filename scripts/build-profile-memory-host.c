#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define MEMORY_MAX_SIZE 4097u
#define MEMORY_ALIGNMENTS 16u
#define MEMORY_GUARD 64u
#define MEMORY_CAPACITY (MEMORY_MAX_SIZE + (MEMORY_GUARD * 2u) + 32u)

void *hobby_memcpy(void *dest, const void *src, size_t n);
void *hobby_memset(void *dest, int c, size_t n);
void *hobby_memmove(void *dest, const void *src, size_t n);

static _Alignas(64) uint8_t g_source[MEMORY_CAPACITY];
static _Alignas(64) uint8_t g_actual[MEMORY_CAPACITY];
static _Alignas(64) uint8_t g_expected[MEMORY_CAPACITY];

static void byte_fill(uint8_t *buffer, size_t length, uint32_t seed)
{
    for (size_t i = 0; i < length; i++)
        buffer[i] = (uint8_t)((i * 131u + seed * 17u) ^ (i >> 3));
}

static void byte_set(uint8_t *dest, uint8_t value, size_t length)
{
    for (size_t i = 0; i < length; i++)
        dest[i] = value;
}

static void byte_copy(uint8_t *dest, const uint8_t *src, size_t length)
{
    for (size_t i = 0; i < length; i++)
        dest[i] = src[i];
}

static void byte_move(uint8_t *dest, const uint8_t *src, size_t length)
{
    if (dest < src)
    {
        for (size_t i = 0; i < length; i++)
            dest[i] = src[i];
    }
    else
    {
        while (length != 0)
        {
            length--;
            dest[length] = src[length];
        }
    }
}

static int byte_equal(const uint8_t *left, const uint8_t *right,
                      size_t length)
{
    for (size_t i = 0; i < length; i++)
    {
        if (left[i] != right[i])
            return 0;
    }
    return 1;
}

static int fail_case(const char *operation, size_t size,
                     unsigned src_alignment, unsigned dest_alignment,
                     const char *direction)
{
    fprintf(stderr,
            "BUILD_PROFILE_MEMORY_HOST: FAIL operation=%s size=%zu "
            "src_alignment=%u dest_alignment=%u direction=%s\n",
            operation, size, src_alignment, dest_alignment, direction);
    return 1;
}

int main(void)
{
    uint64_t memcpy_cases = 0;
    uint64_t memset_cases = 0;
    uint64_t memmove_cases = 0;
    uint64_t overlap_forward = 0;
    uint64_t overlap_backward = 0;
    const uint8_t memset_values[] = {0x00u, 0x5au, 0xffu};

    for (size_t size = 0; size <= MEMORY_MAX_SIZE; size++)
    {
        for (unsigned src_alignment = 0;
             src_alignment < MEMORY_ALIGNMENTS; src_alignment++)
        {
            byte_fill(g_source, sizeof(g_source),
                      (uint32_t)(size + src_alignment));
            for (unsigned dest_alignment = 0;
                 dest_alignment < MEMORY_ALIGNMENTS; dest_alignment++)
            {
                uint8_t *dest = g_actual + MEMORY_GUARD + dest_alignment;
                uint8_t *expected =
                    g_expected + MEMORY_GUARD + dest_alignment;
                const uint8_t *src =
                    g_source + MEMORY_GUARD + src_alignment;
                byte_fill(g_actual, sizeof(g_actual),
                          (uint32_t)(size + dest_alignment + 1u));
                byte_fill(g_expected, sizeof(g_expected),
                          (uint32_t)(size + dest_alignment + 1u));
                if (hobby_memcpy(dest, src, size) != dest)
                    return fail_case("memcpy-return", size, src_alignment,
                                     dest_alignment, "none");
                byte_copy(expected, src, size);
                if (!byte_equal(g_actual, g_expected, sizeof(g_actual)))
                    return fail_case("memcpy", size, src_alignment,
                                     dest_alignment, "none");
                memcpy_cases++;

                unsigned delta = (src_alignment - dest_alignment) & 15u;
                if (delta == 0u)
                    delta = 16u;
                uint8_t *forward_dest =
                    g_actual + MEMORY_GUARD + dest_alignment;
                uint8_t *forward_src = forward_dest + delta;
                uint8_t *forward_expected =
                    g_expected + MEMORY_GUARD + dest_alignment;
                uint8_t *forward_expected_src = forward_expected + delta;
                byte_fill(g_actual, sizeof(g_actual),
                          (uint32_t)(size + src_alignment + 3u));
                byte_fill(g_expected, sizeof(g_expected),
                          (uint32_t)(size + src_alignment + 3u));
                if (hobby_memmove(forward_dest, forward_src, size) !=
                    forward_dest)
                    return fail_case("memmove-return", size, src_alignment,
                                     dest_alignment, "forward");
                byte_move(forward_expected, forward_expected_src, size);
                if (!byte_equal(g_actual, g_expected, sizeof(g_actual)))
                    return fail_case("memmove", size, src_alignment,
                                     dest_alignment, "forward");
                memmove_cases++;
                if (size > delta)
                    overlap_forward++;

                delta = (dest_alignment - src_alignment) & 15u;
                if (delta == 0u)
                    delta = 16u;
                uint8_t *backward_src =
                    g_actual + MEMORY_GUARD + src_alignment;
                uint8_t *backward_dest = backward_src + delta;
                uint8_t *backward_expected_src =
                    g_expected + MEMORY_GUARD + src_alignment;
                uint8_t *backward_expected = backward_expected_src + delta;
                byte_fill(g_actual, sizeof(g_actual),
                          (uint32_t)(size + dest_alignment + 5u));
                byte_fill(g_expected, sizeof(g_expected),
                          (uint32_t)(size + dest_alignment + 5u));
                if (hobby_memmove(backward_dest, backward_src, size) !=
                    backward_dest)
                    return fail_case("memmove-return", size, src_alignment,
                                     dest_alignment, "backward");
                byte_move(backward_expected, backward_expected_src, size);
                if (!byte_equal(g_actual, g_expected, sizeof(g_actual)))
                    return fail_case("memmove", size, src_alignment,
                                     dest_alignment, "backward");
                memmove_cases++;
                if (size > delta)
                    overlap_backward++;
            }
        }

        for (unsigned dest_alignment = 0;
             dest_alignment < MEMORY_ALIGNMENTS; dest_alignment++)
        {
            for (size_t value_index = 0;
                 value_index < sizeof(memset_values); value_index++)
            {
                uint8_t *dest = g_actual + MEMORY_GUARD + dest_alignment;
                uint8_t *expected =
                    g_expected + MEMORY_GUARD + dest_alignment;
                uint8_t value = memset_values[value_index];
                byte_fill(g_actual, sizeof(g_actual),
                          (uint32_t)(size + dest_alignment + value));
                byte_fill(g_expected, sizeof(g_expected),
                          (uint32_t)(size + dest_alignment + value));
                if (hobby_memset(dest, value, size) != dest)
                    return fail_case("memset-return", size, 0,
                                     dest_alignment, "none");
                byte_set(expected, value, size);
                if (!byte_equal(g_actual, g_expected, sizeof(g_actual)))
                    return fail_case("memset", size, 0, dest_alignment,
                                     "none");
                memset_cases++;
            }
        }
    }

    printf("BUILD_PROFILE_MEMORY_HOST: PASS sizes=%u alignments=%u "
           "memcpy_cases=%" PRIu64 " memset_cases=%" PRIu64
           " memmove_cases=%" PRIu64 " overlap_forward=%" PRIu64
           " overlap_backward=%" PRIu64 "\n",
           MEMORY_MAX_SIZE + 1u, MEMORY_ALIGNMENTS, memcpy_cases,
           memset_cases, memmove_cases, overlap_forward, overlap_backward);
    return 0;
}
