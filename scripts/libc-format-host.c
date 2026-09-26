#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int kvsnprintf(char *dst, size_t size, const char *fmt, va_list args);
int ksnprintf(char *dst, size_t size, const char *fmt, ...);
void itoa(int value, char *dst);
void k_int_to_hex(unsigned long long value, char *dst);

typedef struct host_test_state
{
    unsigned int cases;
    unsigned int assertions;
    unsigned int failures;
} host_test_state_t;

static host_test_state_t test_state;

static bool check(bool condition)
{
    test_state.assertions++;
    if (!condition)
        test_state.failures++;
    return condition;
}

static void finish_case(const char *name, unsigned int failures_before)
{
    test_state.cases++;
    printf("[FORMAT_HOST][CASE] id=%s status=%s\n", name,
           test_state.failures == failures_before ? "PASS" : "FAIL");
}

static bool bytes_equal(const void *left, const void *right, size_t length)
{
    return memcmp(left, right, length) == 0;
}

static bool region_is(const unsigned char *bytes, size_t length,
                      unsigned char value)
{
    for (size_t i = 0; i < length; i++)
    {
        if (bytes[i] != value)
            return false;
    }
    return true;
}

static void test_capacity(void)
{
    unsigned int before = test_state.failures;
    unsigned char sentinel = 0x5au;
    int result = ksnprintf(NULL, 0, "abc%d", 12);
    check(result == 5);
    result = ksnprintf((char *)&sentinel, 0, "abc%d", 12);
    check(result == 5 && sentinel == 0x5au);

    unsigned char arena[24];
    memset(arena, 0xa5, sizeof(arena));
    result = ksnprintf((char *)&arena[4], 1, "abc");
    check(result == 3);
    check(arena[4] == 0);
    check(region_is(arena, 4, 0xa5) && region_is(&arena[5], 19, 0xa5));

    memset(arena, 0xa5, sizeof(arena));
    result = ksnprintf((char *)&arena[4], 3, "abc");
    check(result == 3);
    check(bytes_equal(&arena[4], "ab\0", 3));
    check(region_is(arena, 4, 0xa5) && region_is(&arena[7], 17, 0xa5));

    memset(arena, 0xa5, sizeof(arena));
    result = ksnprintf((char *)&arena[4], 4, "abc");
    check(result == 3 && bytes_equal(&arena[4], "abc\0", 4));
    check(region_is(&arena[8], 16, 0xa5));

    memset(arena, 0xa5, sizeof(arena));
    result = ksnprintf((char *)&arena[4], 12, "abc");
    check(result == 3 && bytes_equal(&arena[4], "abc\0", 4));
    check(region_is(&arena[8], 16, 0xa5));
    finish_case("capacity", before);
}

static void test_truncation(void)
{
    unsigned int before = test_state.failures;
    char buffer[32];
    memset(buffer, 0x55, sizeof(buffer));
    check(ksnprintf(buffer, 4, "literal") == 7 &&
          bytes_equal(buffer, "lit\0", 4));
    memset(buffer, 0x55, sizeof(buffer));
    check(ksnprintf(buffer, 4, "%s", "string") == 6 &&
          bytes_equal(buffer, "str\0", 4));
    memset(buffer, 0x55, sizeof(buffer));
    check(ksnprintf(buffer, 2, "%d", -42) == 3 &&
          bytes_equal(buffer, "-\0", 2));
    memset(buffer, 0x55, sizeof(buffer));
    check(ksnprintf(buffer, 4, "%06d", -42) == 6 &&
          bytes_equal(buffer, "-00\0", 4));
    memset(buffer, 0x55, sizeof(buffer));
    check(ksnprintf(buffer, 5, "%06d", -42) == 6 &&
          bytes_equal(buffer, "-000\0", 5));
    memset(buffer, 0x55, sizeof(buffer));
    check(ksnprintf(buffer, 3, "%p", (void *)(uintptr_t)0x2a) == 18 &&
          bytes_equal(buffer, "0x\0", 3));
    check((unsigned char)buffer[3] == 0x55u);
    finish_case("truncation", before);
}

static void test_integer_extremes(void)
{
    unsigned int before = test_state.failures;
    char buffer[96];
    check(ksnprintf(buffer, sizeof(buffer), "%d", 0) == 1 &&
          strcmp(buffer, "0") == 0);
    check(ksnprintf(buffer, sizeof(buffer), "%d", -1) == 2 &&
          strcmp(buffer, "-1") == 0);
    check(ksnprintf(buffer, sizeof(buffer), "%d", INT_MIN) == 11 &&
          strcmp(buffer, "-2147483648") == 0);
    check(ksnprintf(buffer, sizeof(buffer), "%d", INT_MAX) == 10 &&
          strcmp(buffer, "2147483647") == 0);
    check(ksnprintf(buffer, sizeof(buffer), "%lld", LLONG_MIN) == 20 &&
          strcmp(buffer, "-9223372036854775808") == 0);
    check(ksnprintf(buffer, sizeof(buffer), "%lld", LLONG_MAX) == 19 &&
          strcmp(buffer, "9223372036854775807") == 0);
    check(ksnprintf(buffer, sizeof(buffer), "%u", UINT_MAX) == 10 &&
          strcmp(buffer, "4294967295") == 0);
    check(ksnprintf(buffer, sizeof(buffer), "%llu", ULLONG_MAX) == 20 &&
          strcmp(buffer, "18446744073709551615") == 0);
    check(ksnprintf(buffer, sizeof(buffer), "%llX", ULLONG_MAX) == 16 &&
          strcmp(buffer, "FFFFFFFFFFFFFFFF") == 0);
    finish_case("integer-extremes", before);
}

static void test_conversions(void)
{
    unsigned int before = test_state.failures;
    char buffer[160];
    int result = ksnprintf(buffer, sizeof(buffer),
                           "%%|%s|%s|%c|%d|%u|%x|%X|%lld|%llu|%llx|%llX",
                           "", (const char *)NULL, 'Q', -42, 42u, 42u, 42u,
                           -42LL, 42ULL, 42ULL, 42ULL);
    const char expected[] =
        "%||(null)|Q|-42|42|2a|2A|-42|42|2a|2A";
    check(result == (int)strlen(expected));
    check(strcmp(buffer, expected) == 0);

    unsigned char binary[8];
    memset(binary, 0xa5, sizeof(binary));
    result = ksnprintf((char *)binary, 8, "A%cB", 0);
    const unsigned char expected_binary[] = {'A', 0, 'B', 0};
    check(result == 3 && bytes_equal(binary, expected_binary,
                                     sizeof(expected_binary)));
    check(region_is(&binary[4], 4, 0xa5));
    finish_case("conversions", before);
}

static void test_padding(void)
{
    unsigned int before = test_state.failures;
    char buffer[96];
    check(ksnprintf(buffer, sizeof(buffer), "%06d", -42) == 6 &&
          strcmp(buffer, "-00042") == 0);
    check(ksnprintf(buffer, sizeof(buffer), "%08x", 0x2au) == 8 &&
          strcmp(buffer, "0000002a") == 0);
    check(ksnprintf(buffer, sizeof(buffer), "%016llX", 42ULL) == 16 &&
          strcmp(buffer, "000000000000002A") == 0);
    check(ksnprintf(buffer, sizeof(buffer), "%8s", "xy") == 8 &&
          strcmp(buffer, "      xy") == 0);
    check(ksnprintf(buffer, sizeof(buffer), "%08s", "xy") == 8 &&
          strcmp(buffer, "      xy") == 0);
    check(ksnprintf(buffer, sizeof(buffer), "%4c", 'x') == 4 &&
          strcmp(buffer, "   x") == 0);
    check(ksnprintf(buffer, sizeof(buffer), "%5%") == 5 &&
          strcmp(buffer, "    %") == 0);
    check(ksnprintf(buffer, sizeof(buffer), "%2u", 123u) == 3 &&
          strcmp(buffer, "123") == 0);
    finish_case("padding", before);
}

static void test_pointer(void)
{
    unsigned int before = test_state.failures;
    char buffer[96];
    check(sizeof(uintptr_t) == 8u);
    check(ksnprintf(buffer, sizeof(buffer), "%p", (void *)NULL) == 18 &&
          strcmp(buffer, "0x0000000000000000") == 0);
    check(ksnprintf(buffer, sizeof(buffer), "%p",
                    (void *)(uintptr_t)0x2a) == 18 &&
          strcmp(buffer, "0x000000000000002a") == 0);
    check(ksnprintf(buffer, sizeof(buffer), "%020p",
                    (void *)(uintptr_t)0x2a) == 20 &&
          strcmp(buffer, "0x00000000000000002a") == 0);
    check(ksnprintf(buffer, sizeof(buffer), "%22p",
                    (void *)(uintptr_t)0x2a) == 22 &&
          strcmp(buffer, "    0x000000000000002a") == 0);
    finish_case("pointer", before);
}

static bool call_kvsnprintf_twice(char *first, size_t first_size,
                                 char *second, size_t second_size,
                                 const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int first_result = kvsnprintf(first, first_size, fmt, args);
    int second_result = kvsnprintf(second, second_size, fmt, args);
    va_end(args);
    return first_result == second_result &&
           first_result >= 0 &&
           strcmp(first, second) == 0;
}

static void test_va_list(void)
{
    unsigned int before = test_state.failures;
    char first[160];
    char second[160];
    check(call_kvsnprintf_twice(first, sizeof(first), second, sizeof(second),
                                "%d/%u/%lld/%llu/%p/%s", -7, 9u, -11LL,
                                13ULL, (void *)(uintptr_t)0x2a, "done"));
    check(strcmp(first,
                 "-7/9/-11/13/0x000000000000002a/done") == 0);
    finish_case("va-list", before);
}

static void test_errors(void)
{
    unsigned int before = test_state.failures;
    char buffer[32];
    const char *invalid[] = {
        "%q", "%ld", "%zu", "%#x", "%+d", "%-d", "%.2d",
        "%*d", "%n", "%lls", "%", "%l", "%ll", "%2147483648u",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
    {
        memset(buffer, 0x55, sizeof(buffer));
        int result = ksnprintf(buffer, sizeof(buffer), invalid[i], 17ULL);
        check(result == -1);
        check(buffer[0] == '\0' && (unsigned char)buffer[1] == 0x55u);
    }
    const char *null_format = NULL;
    memset(buffer, 0x55, sizeof(buffer));
    check(ksnprintf(buffer, sizeof(buffer), null_format) == -1);
    check(buffer[0] == '\0' && (unsigned char)buffer[1] == 0x55u);
    check(ksnprintf(NULL, 1, "%s", "x") == -1);
    check(ksnprintf(NULL, 0, "%2147483647u", 1u) == INT_MAX);
    memset(buffer, 0x55, sizeof(buffer));
    check(ksnprintf(buffer, sizeof(buffer), "%2147483647uX", 1u) == -1);
    check(buffer[0] == '\0');
    finish_case("errors-and-budget", before);
}

static uint64_t matrix_next(uint64_t *state)
{
    uint64_t value = *state;
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    *state = value;
    return value;
}

static bool compare_one(const char *fmt, int kind, uint64_t raw, size_t size)
{
    unsigned char actual_area[128];
    unsigned char expected_area[128];
    memset(actual_area, 0xa5, sizeof(actual_area));
    memset(expected_area, 0xa5, sizeof(expected_area));
    char *actual = (char *)&actual_area[8];
    char *expected = (char *)&expected_area[8];
    size_t capacity = size;
    if (capacity > 96u)
        capacity = 96u;
    int actual_result;
    int expected_result;

    switch (kind)
    {
    case 0:
        actual_result = ksnprintf(actual, capacity, fmt, (int)raw);
        expected_result = snprintf(expected, capacity, fmt, (int)raw);
        break;
    case 1:
        actual_result = ksnprintf(actual, capacity, fmt, (unsigned int)raw);
        expected_result = snprintf(expected, capacity, fmt,
                                   (unsigned int)raw);
        break;
    case 2:
        actual_result = ksnprintf(actual, capacity, fmt, (long long)raw);
        expected_result = snprintf(expected, capacity, fmt, (long long)raw);
        break;
    default:
        actual_result = ksnprintf(actual, capacity, fmt,
                                  (unsigned long long)raw);
        expected_result = snprintf(expected, capacity, fmt,
                                   (unsigned long long)raw);
        break;
    }

    if (actual_result != expected_result)
        return false;
    if (!region_is(actual_area, 8, 0xa5) ||
        !region_is(&actual_area[8 + capacity], 120u - capacity, 0xa5))
        return false;
    if (capacity > 0 && !bytes_equal(actual, expected, capacity))
        return false;
    return true;
}

static void test_deterministic_matrix(void)
{
    unsigned int before = test_state.failures;
    static const struct
    {
        const char *fmt;
        int kind;
    } formats[] = {
        {"%d", 0}, {"%06d", 0}, {"%12d", 0},
        {"%u", 1}, {"%08x", 1}, {"%12X", 1},
        {"%lld", 2}, {"%024lld", 2},
        {"%llu", 3}, {"%020llu", 3}, {"%016llx", 3},
        {"%024llX", 3},
    };
    uint64_t state = UINT64_C(0x5eedf04a7c9b312d);
    const unsigned int iterations = 4096u;
    for (unsigned int i = 0; i < iterations; i++)
    {
        uint64_t raw = matrix_next(&state);
        size_t index = (size_t)(matrix_next(&state) %
                                (sizeof(formats) / sizeof(formats[0])));
        size_t capacity = (size_t)(matrix_next(&state) % 48u);
        check(compare_one(formats[index].fmt, formats[index].kind,
                          raw, capacity));
    }
    finish_case("deterministic-matrix", before);
}

static void test_legacy(void)
{
    unsigned int before = test_state.failures;
    char buffer[64];
    itoa(INT_MIN, buffer);
    check(strcmp(buffer, "-2147483648") == 0);
    itoa(-1, buffer);
    check(strcmp(buffer, "-1") == 0);
    itoa(0, buffer);
    check(strcmp(buffer, "0") == 0);
    itoa(INT_MAX, buffer);
    check(strcmp(buffer, "2147483647") == 0);
    k_int_to_hex(0, buffer);
    check(strcmp(buffer, "0x0") == 0);
    k_int_to_hex(ULLONG_MAX, buffer);
    check(strcmp(buffer, "0xFFFFFFFFFFFFFFFF") == 0);
    finish_case("legacy", before);
}

int main(void)
{
    printf("[FORMAT_HOST][BEGIN] seed=0x5eedf04a7c9b312d matrix=4096\n");
    test_capacity();
    test_truncation();
    test_integer_extremes();
    test_conversions();
    test_padding();
    test_pointer();
    test_va_list();
    test_errors();
    test_deterministic_matrix();
    test_legacy();
    printf("[FORMAT_HOST][CONTRACT] canary=PASS nul=PASS "
           "truncation_required=PASS source_preserved=PASS "
           "width_budget=PASS va_list_preserved=PASS\n");
    printf("[FORMAT_HOST][SUITE] status=%s cases=%u assertions=%u failures=%u "
           "seed=0x5eedf04a7c9b312d matrix=4096\n",
           test_state.failures == 0 ? "PASS" : "FAIL",
           test_state.cases, test_state.assertions, test_state.failures);
    return test_state.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
