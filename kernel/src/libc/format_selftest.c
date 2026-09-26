#include "format_selftest.h"
#include "string.h"
#include "../drivers/serial.h"

#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

typedef bool (*format_case_function_t)(void);

typedef struct format_case
{
    format_case_function_t run;
    const char *pass_marker;
    const char *fail_marker;
} format_case_t;

static bool format_bytes_equal(const void *left, const void *right, size_t size)
{
    const unsigned char *a = left;
    const unsigned char *b = right;
    for (size_t i = 0; i < size; i++)
    {
        if (a[i] != b[i])
            return false;
    }
    return true;
}

static bool format_text_equal(const char *left, const char *right)
{
    while (*left && *left == *right)
    {
        left++;
        right++;
    }
    return *left == *right;
}

static void format_fill(unsigned char *bytes, size_t size, unsigned char value)
{
    for (size_t i = 0; i < size; i++)
        bytes[i] = value;
}

static bool format_region_is(const unsigned char *bytes, size_t size,
                             unsigned char value)
{
    for (size_t i = 0; i < size; i++)
    {
        if (bytes[i] != value)
            return false;
    }
    return true;
}

static int format_unchecked(char *dst, size_t size, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int result = kvsnprintf(dst, size, fmt, args);
    va_end(args);
    return result;
}

static bool format_case_capacity_zero(void)
{
    unsigned char sentinel = 0x5au;
    return ksnprintf(NULL, 0, "abc%d", 12) == 5 &&
           ksnprintf((char *)&sentinel, 0, "abc%d", 12) == 5 &&
           sentinel == 0x5au;
}

static bool format_case_capacity_one(void)
{
    unsigned char area[3] = {0xa5u, 0xa5u, 0xa5u};
    return ksnprintf((char *)&area[1], 1, "abc") == 3 &&
           area[0] == 0xa5u && area[1] == 0 && area[2] == 0xa5u;
}

static bool format_case_exact_boundaries(void)
{
    char truncated[3];
    char complete[4];
    return ksnprintf(truncated, sizeof(truncated), "abc") == 3 &&
           format_bytes_equal(truncated, "ab\0", sizeof(truncated)) &&
           ksnprintf(complete, sizeof(complete), "abc") == 3 &&
           format_bytes_equal(complete, "abc\0", sizeof(complete));
}

static bool format_case_truncation(void)
{
    char buffer[8];
    if (ksnprintf(buffer, 2, "%d", -42) != 3 ||
        !format_bytes_equal(buffer, "-\0", 2))
        return false;
    if (ksnprintf(buffer, 4, "%06d", -42) != 6 ||
        !format_bytes_equal(buffer, "-00\0", 4))
        return false;
    return ksnprintf(buffer, 3, "%p", (void *)(uintptr_t)0x2a) == 18 &&
           format_bytes_equal(buffer, "0x\0", 3);
}

static bool format_case_int_extremes(void)
{
    char buffer[32];
    return ksnprintf(buffer, sizeof(buffer), "%d", INT_MIN) == 11 &&
           format_text_equal(buffer, "-2147483648") &&
           ksnprintf(buffer, sizeof(buffer), "%d", INT_MAX) == 10 &&
           format_text_equal(buffer, "2147483647");
}

static bool format_case_long_long_extremes(void)
{
    char buffer[32];
    return ksnprintf(buffer, sizeof(buffer), "%lld", LLONG_MIN) == 20 &&
           format_text_equal(buffer, "-9223372036854775808") &&
           ksnprintf(buffer, sizeof(buffer), "%lld", LLONG_MAX) == 19 &&
           format_text_equal(buffer, "9223372036854775807");
}

static bool format_case_unsigned_extremes(void)
{
    char buffer[32];
    return ksnprintf(buffer, sizeof(buffer), "%u", UINT_MAX) == 10 &&
           format_text_equal(buffer, "4294967295") &&
           ksnprintf(buffer, sizeof(buffer), "%llu", ULLONG_MAX) == 20 &&
           format_text_equal(buffer, "18446744073709551615") &&
           ksnprintf(buffer, sizeof(buffer), "%llX", ULLONG_MAX) == 16 &&
           format_text_equal(buffer, "FFFFFFFFFFFFFFFF");
}

static bool format_case_conversions(void)
{
    char buffer[128];
    const char expected[] = "%||Q|-42|42|2a|2A|-42|42|2a|2A";
    int required = ksnprintf(
        buffer, sizeof(buffer),
        "%%|%s|%c|%d|%u|%x|%X|%lld|%llu|%llx|%llX",
        "", 'Q', -42, 42u, 42u, 42u, -42LL, 42ULL, 42ULL, 42ULL);
    return required == (int)(sizeof(expected) - 1u) &&
           format_text_equal(buffer, expected);
}

static bool format_case_embedded_zero(void)
{
    unsigned char buffer[6];
    const unsigned char expected[] = {'A', 0, 'B', 0};
    format_fill(buffer, sizeof(buffer), 0xa5u);
    return ksnprintf((char *)buffer, sizeof(buffer), "A%cB", 0) == 3 &&
           format_bytes_equal(buffer, expected, sizeof(expected)) &&
           format_region_is(&buffer[4], 2, 0xa5u);
}

static bool format_case_padding(void)
{
    char buffer[32];
    return ksnprintf(buffer, sizeof(buffer), "%06d", -42) == 6 &&
           format_text_equal(buffer, "-00042") &&
           ksnprintf(buffer, sizeof(buffer), "%08x", 0x2au) == 8 &&
           format_text_equal(buffer, "0000002a") &&
           ksnprintf(buffer, sizeof(buffer), "%016llX", 42ULL) == 16 &&
           format_text_equal(buffer, "000000000000002A") &&
           format_unchecked(buffer, sizeof(buffer), "%08s", "xy") == 8 &&
           format_text_equal(buffer, "      xy");
}

static bool format_case_pointer(void)
{
    char buffer[32];
    return sizeof(uintptr_t) == 8u &&
           ksnprintf(buffer, sizeof(buffer), "%p", (void *)NULL) == 18 &&
           format_text_equal(buffer, "0x0000000000000000") &&
           format_unchecked(buffer, sizeof(buffer), "%020p",
                            (void *)(uintptr_t)0x2a) == 20 &&
           format_text_equal(buffer, "0x00000000000000002a");
}

static bool format_case_null_string(void)
{
    char buffer[16];
    const char *text = NULL;
    return format_unchecked(buffer, sizeof(buffer), "%s", text) == 6 &&
           format_text_equal(buffer, "(null)");
}

static bool format_va_twice(char *first, char *second, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int first_result = kvsnprintf(first, 96, fmt, args);
    int second_result = kvsnprintf(second, 96, fmt, args);
    va_end(args);
    return first_result == second_result && first_result >= 0 &&
           format_text_equal(first, second);
}

static bool format_case_va_list(void)
{
    char first[96];
    char second[96];
    return format_va_twice(first, second, "%d/%u/%lld/%llu/%p/%s",
                           -7, 9u, -11LL, 13ULL,
                           (void *)(uintptr_t)0x2a, "done") &&
           format_text_equal(
               first, "-7/9/-11/13/0x000000000000002a/done");
}

static bool format_case_invalid_format(void)
{
    char buffer[8];
    const char *invalid[] = {"%#x", "%", "%l", "%ll", "%zu"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
    {
        format_fill((unsigned char *)buffer, sizeof(buffer), 0xa5u);
        if (format_unchecked(buffer, sizeof(buffer), invalid[i], 42ULL) != -1 ||
            buffer[0] != '\0' || (unsigned char)buffer[1] != 0xa5u)
            return false;
    }
    return true;
}

static bool format_case_width_bounds(void)
{
    const char *invalid_width = "%2147483648u";
    char buffer[8];
    return format_unchecked(NULL, 0, "%2147483647u", 1u) == INT_MAX &&
           format_unchecked(buffer, sizeof(buffer), invalid_width, 1u) == -1 &&
           buffer[0] == '\0';
}

static bool format_case_required_overflow(void)
{
    const char *overflow = "%2147483647uX";
    char buffer[8];
    return format_unchecked(buffer, sizeof(buffer), overflow, 1u) == -1 &&
           buffer[0] == '\0';
}

static bool format_case_memory_guards(void)
{
    unsigned char area[20];
    format_fill(area, sizeof(area), 0xa5u);
    int required = ksnprintf((char *)&area[4], 8, "%s", "xy");
    return required == 2 &&
           format_bytes_equal(&area[4], "xy\0", 3) &&
           format_region_is(area, 4, 0xa5u) &&
           format_region_is(&area[7], 13, 0xa5u);
}

static bool format_case_legacy_itoa(void)
{
    char buffer[32];
    itoa(INT_MIN, buffer);
    if (!format_text_equal(buffer, "-2147483648"))
        return false;
    itoa(0, buffer);
    return format_text_equal(buffer, "0");
}

static bool format_case_legacy_hex(void)
{
    char buffer[32];
    k_int_to_hex(0, buffer);
    if (!format_text_equal(buffer, "0x0"))
        return false;
    k_int_to_hex(ULLONG_MAX, buffer);
    return format_text_equal(buffer, "0xFFFFFFFFFFFFFFFF");
}

static bool format_case_breadcrumb_consumer(void)
{
    char buffer[176];
    const char expected[] =
        "[BOOT][PROGRESS] stage=PCI_SCAN_COMPLETE "
        "monotonic_ms=18446744073709551615 "
        "clockevent_ticks=18446744073709551615\n";
    int required = ksnprintf(
        buffer, sizeof(buffer),
        "[BOOT][PROGRESS] stage=%s monotonic_ms=%llu "
        "clockevent_ticks=%llu\n",
        "PCI_SCAN_COMPLETE", ULLONG_MAX, ULLONG_MAX);
    return required == (int)(sizeof(expected) - 1u) &&
           format_text_equal(buffer, expected);
}

static bool format_case_pmem_consumer(void)
{
    char buffer[192];
    const char expected[] =
        "[PMM] Highest RAM: 0x000000000000002A | "
        "Total Frames: 0xFFFFFFFFFFFFFFFF\n";
    int required = ksnprintf(
        buffer, sizeof(buffer),
        "[PMM] Highest RAM: 0x%016llX | Total Frames: 0x%016llX\n",
        42ULL, ULLONG_MAX);
    if (required != (int)(sizeof(expected) - 1u) ||
        !format_text_equal(buffer, expected))
        return false;

    const char reserved[] =
        "[PMM] Reserved LOW<1MB and KERNEL frames. "
        "Kernel phys=[0x0x0000000000001000..0x0x0000000000002000)\n";
    required = ksnprintf(
        buffer, sizeof(buffer),
        "[PMM] Reserved LOW<1MB and KERNEL frames. "
        "Kernel phys=[0x0x%016llX..0x0x%016llX)\n",
        0x1000ULL, 0x2000ULL);
    return required == (int)(sizeof(reserved) - 1u) &&
           format_text_equal(buffer, reserved);
}

static bool format_case_breadcrumb_fallback(void)
{
    return kinit_progress_format_probe();
}

static const format_case_t format_cases[] = {
    {format_case_capacity_zero,
     "[FORMAT][CASE] id=capacity-zero status=PASS\n",
     "[FORMAT][CASE] id=capacity-zero status=FAIL\n"},
    {format_case_capacity_one,
     "[FORMAT][CASE] id=capacity-one status=PASS\n",
     "[FORMAT][CASE] id=capacity-one status=FAIL\n"},
    {format_case_exact_boundaries,
     "[FORMAT][CASE] id=exact-boundaries status=PASS\n",
     "[FORMAT][CASE] id=exact-boundaries status=FAIL\n"},
    {format_case_truncation,
     "[FORMAT][CASE] id=truncation status=PASS\n",
     "[FORMAT][CASE] id=truncation status=FAIL\n"},
    {format_case_int_extremes,
     "[FORMAT][CASE] id=int-extremes status=PASS\n",
     "[FORMAT][CASE] id=int-extremes status=FAIL\n"},
    {format_case_long_long_extremes,
     "[FORMAT][CASE] id=long-long-extremes status=PASS\n",
     "[FORMAT][CASE] id=long-long-extremes status=FAIL\n"},
    {format_case_unsigned_extremes,
     "[FORMAT][CASE] id=unsigned-extremes status=PASS\n",
     "[FORMAT][CASE] id=unsigned-extremes status=FAIL\n"},
    {format_case_conversions,
     "[FORMAT][CASE] id=conversions status=PASS\n",
     "[FORMAT][CASE] id=conversions status=FAIL\n"},
    {format_case_embedded_zero,
     "[FORMAT][CASE] id=embedded-zero status=PASS\n",
     "[FORMAT][CASE] id=embedded-zero status=FAIL\n"},
    {format_case_padding,
     "[FORMAT][CASE] id=padding status=PASS\n",
     "[FORMAT][CASE] id=padding status=FAIL\n"},
    {format_case_pointer,
     "[FORMAT][CASE] id=pointer status=PASS\n",
     "[FORMAT][CASE] id=pointer status=FAIL\n"},
    {format_case_null_string,
     "[FORMAT][CASE] id=null-string status=PASS\n",
     "[FORMAT][CASE] id=null-string status=FAIL\n"},
    {format_case_va_list,
     "[FORMAT][CASE] id=va-list-preserved status=PASS\n",
     "[FORMAT][CASE] id=va-list-preserved status=FAIL\n"},
    {format_case_invalid_format,
     "[FORMAT][CASE] id=invalid-format status=PASS\n",
     "[FORMAT][CASE] id=invalid-format status=FAIL\n"},
    {format_case_width_bounds,
     "[FORMAT][CASE] id=width-bounds status=PASS\n",
     "[FORMAT][CASE] id=width-bounds status=FAIL\n"},
    {format_case_required_overflow,
     "[FORMAT][CASE] id=required-overflow status=PASS\n",
     "[FORMAT][CASE] id=required-overflow status=FAIL\n"},
    {format_case_memory_guards,
     "[FORMAT][CASE] id=memory-guards status=PASS\n",
     "[FORMAT][CASE] id=memory-guards status=FAIL\n"},
    {format_case_legacy_itoa,
     "[FORMAT][CASE] id=legacy-itoa status=PASS\n",
     "[FORMAT][CASE] id=legacy-itoa status=FAIL\n"},
    {format_case_legacy_hex,
     "[FORMAT][CASE] id=legacy-hex status=PASS\n",
     "[FORMAT][CASE] id=legacy-hex status=FAIL\n"},
    {format_case_breadcrumb_consumer,
     "[FORMAT][CASE] id=breadcrumb-consumer status=PASS\n",
     "[FORMAT][CASE] id=breadcrumb-consumer status=FAIL\n"},
    {format_case_pmem_consumer,
     "[FORMAT][CASE] id=pmem-consumer status=PASS\n",
     "[FORMAT][CASE] id=pmem-consumer status=FAIL\n"},
    {format_case_breadcrumb_fallback,
     "[FORMAT][CASE] id=breadcrumb-fallback status=PASS\n",
     "[FORMAT][CASE] id=breadcrumb-fallback status=FAIL\n"},
};

bool format_selftest_run(void)
{
    serial_write_all("[FORMAT][SUITE_BEGIN] cases=22\n");
    for (size_t i = 0; i < sizeof(format_cases) / sizeof(format_cases[0]); i++)
    {
        if (!format_cases[i].run())
        {
            serial_write_all(format_cases[i].fail_marker);
            serial_write_all("[FORMAT][SUITE_END] status=FAIL completed=0 "
                             "cases=22\n");
            return false;
        }
        serial_write_all(format_cases[i].pass_marker);
    }
    serial_write_all("[FORMAT][SUITE_END] status=PASS completed=1 cases=22\n");
    return true;
}
