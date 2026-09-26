#include "cmd_profiletest.h"

#include "../../core/build_profile.h"
#include "../../drivers/serial.h"
#include "../../libc/memory.h"
#include "../../libc/string.h"
#include "../../memory/heap.h"
#include "../../smp/smp_topology.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef HOBBYOS_SELFTEST

#define PROFILETEST_MEMORY_MAX_SIZE 4097u
#define PROFILETEST_MEMORY_ALIGNMENTS 16u
#define PROFILETEST_MEMORY_GUARD 64u
#define PROFILETEST_MEMORY_CAPACITY \
    (PROFILETEST_MEMORY_MAX_SIZE + (PROFILETEST_MEMORY_GUARD * 2u) + 32u)
#define PROFILETEST_BENCHMARK_BYTES (8u * 1024u * 1024u)
#define PROFILETEST_BENCHMARK_SAMPLES 7u

static uint8_t g_profiletest_memory_source[PROFILETEST_MEMORY_CAPACITY]
    __attribute__((aligned(64)));
static uint8_t g_profiletest_memory_actual[PROFILETEST_MEMORY_CAPACITY]
    __attribute__((aligned(64)));
static uint8_t g_profiletest_memory_expected[PROFILETEST_MEMORY_CAPACITY]
    __attribute__((aligned(64)));

static void *(*volatile g_profiletest_memcpy_fn)(void *, const void *,
                                                 size_t) = memcpy;
static void *(*volatile g_profiletest_memset_fn)(void *, int, size_t) =
    memset;
static void *(*volatile g_profiletest_memmove_fn)(void *, const void *,
                                                  size_t) = memmove;

static void profiletest_write_u64(uint64_t value)
{
    char buffer[21];
    uint32_t count = 0;
    do
    {
        buffer[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    while (count != 0)
        serial_putc_all(buffer[--count]);
}

static uint32_t profiletest_popcount32(uint32_t value)
{
    uint32_t count = 0;
    while (value != 0)
    {
        count += value & 1u;
        value >>= 1;
    }
    return count;
}

static uint64_t profiletest_read_rflags(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags) :: "memory");
    return flags;
}

static void profiletest_byte_fill(uint8_t *buffer, size_t length,
                                  uint32_t seed)
{
    volatile uint8_t *out = buffer;
    for (size_t i = 0; i < length; i++)
        out[i] = (uint8_t)((i * 131u + seed * 17u) ^ (i >> 3));
}

static void profiletest_byte_set(uint8_t *dest, uint8_t value, size_t length)
{
    volatile uint8_t *out = dest;
    for (size_t i = 0; i < length; i++)
        out[i] = value;
}

static void profiletest_byte_copy(uint8_t *dest, const uint8_t *src,
                                  size_t length)
{
    volatile uint8_t *out = dest;
    const volatile uint8_t *in = src;
    for (size_t i = 0; i < length; i++)
        out[i] = in[i];
}

static void profiletest_byte_move(uint8_t *dest, const uint8_t *src,
                                  size_t length)
{
    volatile uint8_t *out = dest;
    const volatile uint8_t *in = src;
    if (dest < src)
    {
        for (size_t i = 0; i < length; i++)
            out[i] = in[i];
    }
    else
    {
        while (length != 0)
        {
            length--;
            out[length] = in[length];
        }
    }
}

static bool profiletest_byte_equal(const uint8_t *left,
                                   const uint8_t *right, size_t length)
{
    const volatile uint8_t *lhs = left;
    const volatile uint8_t *rhs = right;
    for (size_t i = 0; i < length; i++)
    {
        if (lhs[i] != rhs[i])
            return false;
    }
    return true;
}

static size_t profiletest_memory_size(uint32_t index)
{
    static const uint16_t boundary_sizes[] = {
        127u, 128u, 129u, 255u, 256u, 257u,
        511u, 512u, 513u, 1023u, 1024u, 1025u,
        2047u, 2048u, 2049u, 4095u, 4096u, 4097u,
    };
    if (index <= 64u)
        return index;
    return boundary_sizes[index - 65u];
}

static uint32_t profiletest_memory_size_count(void)
{
    return 65u + 18u;
}

typedef struct
{
    uint64_t cases;
    uint64_t memcpy_failures;
    uint64_t memset_failures;
    uint64_t memmove_failures;
    uint64_t overlap_forward;
    uint64_t overlap_backward;
} profiletest_memory_result_t;

static profiletest_memory_result_t profiletest_memory_correctness(void)
{
    profiletest_memory_result_t result = {0};
    static const uint8_t memset_values[] = {0x00u, 0x5au, 0xffu};

    for (uint32_t size_index = 0;
         size_index < profiletest_memory_size_count(); size_index++)
    {
        size_t size = profiletest_memory_size(size_index);
        for (uint32_t src_alignment = 0;
             src_alignment < PROFILETEST_MEMORY_ALIGNMENTS; src_alignment++)
        {
            profiletest_byte_fill(g_profiletest_memory_source,
                                  sizeof(g_profiletest_memory_source),
                                  (uint32_t)size + src_alignment);
            for (uint32_t dest_alignment = 0;
                 dest_alignment < PROFILETEST_MEMORY_ALIGNMENTS;
                 dest_alignment++)
            {
                uint8_t *dest = g_profiletest_memory_actual +
                                PROFILETEST_MEMORY_GUARD + dest_alignment;
                uint8_t *expected = g_profiletest_memory_expected +
                                    PROFILETEST_MEMORY_GUARD + dest_alignment;
                const uint8_t *src = g_profiletest_memory_source +
                                     PROFILETEST_MEMORY_GUARD + src_alignment;
                profiletest_byte_fill(g_profiletest_memory_actual,
                                      sizeof(g_profiletest_memory_actual),
                                      (uint32_t)size + dest_alignment + 1u);
                profiletest_byte_fill(g_profiletest_memory_expected,
                                      sizeof(g_profiletest_memory_expected),
                                      (uint32_t)size + dest_alignment + 1u);
                if (g_profiletest_memcpy_fn(dest, src, size) != dest)
                    result.memcpy_failures++;
                profiletest_byte_copy(expected, src, size);
                if (!profiletest_byte_equal(
                        g_profiletest_memory_actual,
                        g_profiletest_memory_expected,
                        sizeof(g_profiletest_memory_actual)))
                    result.memcpy_failures++;
                result.cases++;

                uint32_t delta =
                    (src_alignment - dest_alignment) & 15u;
                if (delta == 0u)
                    delta = 16u;
                uint8_t *forward_dest = g_profiletest_memory_actual +
                                        PROFILETEST_MEMORY_GUARD +
                                        dest_alignment;
                uint8_t *forward_src = forward_dest + delta;
                uint8_t *forward_expected = g_profiletest_memory_expected +
                                            PROFILETEST_MEMORY_GUARD +
                                            dest_alignment;
                uint8_t *forward_expected_src = forward_expected + delta;
                profiletest_byte_fill(g_profiletest_memory_actual,
                                      sizeof(g_profiletest_memory_actual),
                                      (uint32_t)size + src_alignment + 3u);
                profiletest_byte_fill(g_profiletest_memory_expected,
                                      sizeof(g_profiletest_memory_expected),
                                      (uint32_t)size + src_alignment + 3u);
                if (g_profiletest_memmove_fn(forward_dest, forward_src,
                                             size) != forward_dest)
                    result.memmove_failures++;
                profiletest_byte_move(forward_expected, forward_expected_src,
                                      size);
                if (!profiletest_byte_equal(
                        g_profiletest_memory_actual,
                        g_profiletest_memory_expected,
                        sizeof(g_profiletest_memory_actual)))
                    result.memmove_failures++;
                result.cases++;
                if (size > delta)
                    result.overlap_forward++;

                delta = (dest_alignment - src_alignment) & 15u;
                if (delta == 0u)
                    delta = 16u;
                uint8_t *backward_src = g_profiletest_memory_actual +
                                        PROFILETEST_MEMORY_GUARD +
                                        src_alignment;
                uint8_t *backward_dest = backward_src + delta;
                uint8_t *backward_expected_src =
                    g_profiletest_memory_expected +
                    PROFILETEST_MEMORY_GUARD + src_alignment;
                uint8_t *backward_expected = backward_expected_src + delta;
                profiletest_byte_fill(g_profiletest_memory_actual,
                                      sizeof(g_profiletest_memory_actual),
                                      (uint32_t)size + dest_alignment + 5u);
                profiletest_byte_fill(g_profiletest_memory_expected,
                                      sizeof(g_profiletest_memory_expected),
                                      (uint32_t)size + dest_alignment + 5u);
                if (g_profiletest_memmove_fn(backward_dest, backward_src,
                                             size) != backward_dest)
                    result.memmove_failures++;
                profiletest_byte_move(backward_expected,
                                      backward_expected_src, size);
                if (!profiletest_byte_equal(
                        g_profiletest_memory_actual,
                        g_profiletest_memory_expected,
                        sizeof(g_profiletest_memory_actual)))
                    result.memmove_failures++;
                result.cases++;
                if (size > delta)
                    result.overlap_backward++;
            }
        }

        for (uint32_t dest_alignment = 0;
             dest_alignment < PROFILETEST_MEMORY_ALIGNMENTS;
             dest_alignment++)
        {
            for (uint32_t value_index = 0;
                 value_index < sizeof(memset_values); value_index++)
            {
                uint8_t value = memset_values[value_index];
                uint8_t *dest = g_profiletest_memory_actual +
                                PROFILETEST_MEMORY_GUARD + dest_alignment;
                uint8_t *expected = g_profiletest_memory_expected +
                                    PROFILETEST_MEMORY_GUARD + dest_alignment;
                profiletest_byte_fill(g_profiletest_memory_actual,
                                      sizeof(g_profiletest_memory_actual),
                                      (uint32_t)size + dest_alignment + value);
                profiletest_byte_fill(g_profiletest_memory_expected,
                                      sizeof(g_profiletest_memory_expected),
                                      (uint32_t)size + dest_alignment + value);
                if (g_profiletest_memset_fn(dest, value, size) != dest)
                    result.memset_failures++;
                profiletest_byte_set(expected, value, size);
                if (!profiletest_byte_equal(
                        g_profiletest_memory_actual,
                        g_profiletest_memory_expected,
                        sizeof(g_profiletest_memory_actual)))
                    result.memset_failures++;
                result.cases++;
            }
        }
    }
    return result;
}

static uint64_t profiletest_tsc_begin(void)
{
    uint32_t low;
    uint32_t high;
    __asm__ volatile("lfence; rdtsc; lfence"
                     : "=a"(low), "=d"(high)
                     :
                     : "memory");
    return ((uint64_t)high << 32) | low;
}

static uint64_t profiletest_tsc_end(void)
{
    uint32_t low;
    uint32_t high;
    __asm__ volatile("mfence; lfence; rdtsc; lfence"
                     : "=a"(low), "=d"(high)
                     :
                     : "memory");
    return ((uint64_t)high << 32) | low;
}

static void profiletest_sort_samples(uint64_t *samples, uint32_t count)
{
    for (uint32_t i = 1; i < count; i++)
    {
        uint64_t value = samples[i];
        uint32_t position = i;
        while (position != 0 && samples[position - 1u] > value)
        {
            samples[position] = samples[position - 1u];
            position--;
        }
        samples[position] = value;
    }
}

static bool profiletest_memory_benchmark(uint64_t *median_cycles,
                                         uint64_t *checksum,
                                         bool *heap_ok)
{
    uint8_t *source = kmalloc(PROFILETEST_BENCHMARK_BYTES);
    uint8_t *dest = kmalloc(PROFILETEST_BENCHMARK_BYTES);
    uint64_t samples[PROFILETEST_BENCHMARK_SAMPLES];
    bool ok = source != NULL && dest != NULL;

    *median_cycles = 0;
    *checksum = 0;
    *heap_ok = false;
    if (ok)
    {
        profiletest_byte_fill(source, PROFILETEST_BENCHMARK_BYTES,
                              0x8b17u);
        profiletest_byte_set(dest, 0xa5u, PROFILETEST_BENCHMARK_BYTES);
        (void)g_profiletest_memcpy_fn(dest, source,
                                     PROFILETEST_BENCHMARK_BYTES);
        for (uint32_t sample = 0;
             sample < PROFILETEST_BENCHMARK_SAMPLES; sample++)
        {
            uint64_t begin = profiletest_tsc_begin();
            (void)g_profiletest_memcpy_fn(dest, source,
                                         PROFILETEST_BENCHMARK_BYTES);
            uint64_t end = profiletest_tsc_end();
            samples[sample] = end >= begin ? end - begin : 0;
            if (samples[sample] == 0)
                ok = false;
        }
        profiletest_sort_samples(samples, PROFILETEST_BENCHMARK_SAMPLES);
        *median_cycles = samples[PROFILETEST_BENCHMARK_SAMPLES / 2u];

        uint64_t hash = 1469598103934665603ULL;
        for (size_t i = 0; i < PROFILETEST_BENCHMARK_BYTES; i++)
        {
            if (dest[i] != source[i])
                ok = false;
            hash ^= dest[i];
            hash *= 1099511628211ULL;
        }
        *checksum = hash;
    }

    if (dest != NULL)
        kfree(dest);
    if (source != NULL)
        kfree(source);
    *heap_ok = heap_validate_integrity();
    return ok && *heap_ok;
}

static int profiletest_memory(void)
{
    profiletest_memory_result_t result = profiletest_memory_correctness();
    uint64_t median_cycles = 0;
    uint64_t checksum = 0;
    bool heap_ok = false;
    bool benchmark_ok = profiletest_memory_benchmark(&median_cycles,
                                                     &checksum, &heap_ok);
    bool ok = result.memcpy_failures == 0 &&
              result.memset_failures == 0 &&
              result.memmove_failures == 0 && benchmark_ok &&
              result.overlap_forward != 0 &&
              result.overlap_backward != 0;
    char marker[512];
    int required = ksnprintf(
        marker, sizeof(marker),
        "[BUILD_PROFILE][MEMORY] %s cases=%llu max_size=%u alignments=%u "
        "memcpy_failures=%llu memset_failures=%llu memmove_failures=%llu "
        "overlap_forward=%llu overlap_backward=%llu benchmark_bytes=%u "
        "samples=%u median_cycles=%llu checksum=%llu heap_ok=%u\n",
        ok ? "PASS" : "FAIL", (unsigned long long)result.cases,
        PROFILETEST_MEMORY_MAX_SIZE, PROFILETEST_MEMORY_ALIGNMENTS,
        (unsigned long long)result.memcpy_failures,
        (unsigned long long)result.memset_failures,
        (unsigned long long)result.memmove_failures,
        (unsigned long long)result.overlap_forward,
        (unsigned long long)result.overlap_backward,
        PROFILETEST_BENCHMARK_BYTES, PROFILETEST_BENCHMARK_SAMPLES,
        (unsigned long long)median_cycles,
        (unsigned long long)checksum, heap_ok ? 1u : 0u);
    if (required <= 0 || (size_t)required >= sizeof(marker))
    {
        serial_write_all("[BUILD_PROFILE][MEMORY] FAIL reason=format\n");
        return 1;
    }
    serial_write_all(marker);
    return ok ? 0 : 1;
}

static int profiletest_abi(void)
{
    build_profile_snapshot_t before;
    build_profile_snapshot_t after;
    bool before_ok = build_profile_snapshot(&before);
    uint64_t restored_flags = 0;

    /*
     * IRET must restore DF=1 from the frame, while the assembly bridge clears
     * it before C. CLD remains in this block so C never runs with DF set.
     */
    __asm__ volatile(
        "std\n\t"
        "int $35\n\t"
        "pushfq\n\t"
        "popq %0\n\t"
        "cld"
        : "=r"(restored_flags)
        :
        : "memory", "cc");

    bool after_ok = build_profile_snapshot(&after);
    uint64_t final_flags = profiletest_read_rflags();
    cpu_slot_t bsp_slot = smp_bsp_cpu_slot();
    uint32_t cpu_mask = g_cpu_count >= 32u
                            ? UINT32_MAX
                            : ((1u << g_cpu_count) - 1u);
    uint32_t expected_ap_mask = bsp_slot < 32u
                                    ? cpu_mask & ~(1u << bsp_slot)
                                    : 0u;
    uint64_t probe_delta = after_ok && before_ok &&
                                   after.irq_probe_entries >=
                                       before.irq_probe_entries
                               ? after.irq_probe_entries -
                                     before.irq_probe_entries
                               : 0u;
    uint64_t probe_bad_delta = after_ok && before_ok &&
                                       after.irq_probe_df_violations >=
                                           before.irq_probe_df_violations
                                   ? after.irq_probe_df_violations -
                                         before.irq_probe_df_violations
                                   : UINT64_MAX;
    bool restored_df = (restored_flags & (1ULL << 10)) != 0;
    bool final_df = (final_flags & (1ULL << 10)) != 0;
    bool ok = before_ok && after_ok && after.bsp_seen == 1u &&
              after.bsp_rsp_mod16 == 8u && after.bsp_df == 0u &&
              after.ap_seen_mask == expected_ap_mask &&
              after.ap_bad_rsp_mask == 0u && after.ap_bad_df_mask == 0u &&
              after.irq_entries != 0u && after.irq_df_violations == 0u &&
              probe_delta >= 1u && probe_bad_delta == 0u && restored_df &&
              !final_df;

    serial_write_all("[BUILD_PROFILE][ABI] ");
    serial_write_all(ok ? "PASS" : "FAIL");
    serial_write_all(" bsp_seen=");
    profiletest_write_u64(after.bsp_seen);
    serial_write_all(" bsp_rsp_mod16=");
    profiletest_write_u64(after.bsp_rsp_mod16);
    serial_write_all(" bsp_df=");
    profiletest_write_u64(after.bsp_df);
    serial_write_all(" ap_expected=");
    profiletest_write_u64(profiletest_popcount32(expected_ap_mask));
    serial_write_all(" ap_seen=");
    profiletest_write_u64(profiletest_popcount32(after.ap_seen_mask));
    serial_write_all(" ap_bad_rsp=");
    profiletest_write_u64(profiletest_popcount32(after.ap_bad_rsp_mask));
    serial_write_all(" ap_bad_df=");
    profiletest_write_u64(profiletest_popcount32(after.ap_bad_df_mask));
    serial_write_all(" irq_entries=");
    profiletest_write_u64(after.irq_entries);
    serial_write_all(" irq_bad_df=");
    profiletest_write_u64(after.irq_df_violations);
    serial_write_all(" probe_vector=35 probe_entries_delta=");
    profiletest_write_u64(probe_delta);
    serial_write_all(" probe_bad_df_delta=");
    profiletest_write_u64(probe_bad_delta);
    serial_write_all(" restored_df=");
    profiletest_write_u64(restored_df);
    serial_write_all(" final_df=");
    profiletest_write_u64(final_df);
    serial_write_all("\n");
    return ok ? 0 : 1;
}

int cmd_profiletest(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "abi") == 0)
        return profiletest_abi();
    if (argc == 2 && strcmp(argv[1], "memory") == 0)
        return profiletest_memory();
    serial_write_all("[BUILD_PROFILE][COMMAND] FAIL reason=usage\n");
    return 1;
}

#else

int cmd_profiletest(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 1;
}

#endif
