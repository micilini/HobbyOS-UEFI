#include "string.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct format_writer
{
    char *dst;
    size_t size;
    size_t stored;
    size_t required;
    bool failed;
} format_writer_t;

static bool format_writer_count(format_writer_t *writer, size_t count)
{
    if (writer->failed || count > (size_t)INT_MAX - writer->required)
    {
        writer->failed = true;
        return false;
    }
    writer->required += count;
    return true;
}

static size_t format_writer_available(const format_writer_t *writer)
{
    if (writer->size == 0 || writer->stored >= writer->size - 1u)
        return 0;
    return writer->size - 1u - writer->stored;
}

static void format_writer_bytes(format_writer_t *writer,
                                const char *bytes,
                                size_t count)
{
    if (!format_writer_count(writer, count))
        return;

    size_t copy_count = count;
    size_t available = format_writer_available(writer);
    if (copy_count > available)
        copy_count = available;
    for (size_t i = 0; i < copy_count; i++)
        writer->dst[writer->stored + i] = bytes[i];
    writer->stored += copy_count;
}

static void format_writer_repeat(format_writer_t *writer,
                                 char value,
                                 size_t count)
{
    if (!format_writer_count(writer, count))
        return;

    size_t write_count = count;
    size_t available = format_writer_available(writer);
    if (write_count > available)
        write_count = available;
    for (size_t i = 0; i < write_count; i++)
        writer->dst[writer->stored + i] = value;
    writer->stored += write_count;
}

static bool format_string_length(const char *text, size_t *length_out)
{
    size_t length = 0;
    while (text[length])
    {
        if (length == (size_t)INT_MAX)
            return false;
        length++;
    }
    *length_out = length;
    return true;
}

static void format_unsigned(format_writer_t *writer,
                            unsigned long long value,
                            unsigned int base,
                            bool uppercase,
                            bool negative,
                            bool pointer,
                            size_t minimum_digits,
                            size_t width,
                            bool zero_pad)
{
    static const char lower_digits[] = "0123456789abcdef";
    static const char upper_digits[] = "0123456789ABCDEF";
    const char *alphabet = uppercase ? upper_digits : lower_digits;
    char reversed[sizeof(unsigned long long) * 8u];
    size_t digit_count = 0;

    do
    {
        reversed[digit_count++] = alphabet[value % base];
        value /= base;
    } while (value);

    size_t natural_digits = digit_count;
    if (minimum_digits > digit_count)
        digit_count = minimum_digits;

    size_t prefix_length = pointer ? 2u : 0u;
    size_t sign_length = negative ? 1u : 0u;
    size_t content_length = sign_length + prefix_length + digit_count;
    size_t padding = width > content_length ? width - content_length : 0u;

    if (!zero_pad)
        format_writer_repeat(writer, ' ', padding);
    if (negative)
        format_writer_bytes(writer, "-", 1u);
    if (pointer)
        format_writer_bytes(writer, "0x", 2u);
    if (zero_pad)
        format_writer_repeat(writer, '0', padding);

    format_writer_repeat(writer, '0', digit_count - natural_digits);
    while (natural_digits)
    {
        natural_digits--;
        format_writer_bytes(writer, &reversed[natural_digits], 1u);
    }
}

static bool format_parse_width(const char **cursor,
                               size_t *width_out,
                               bool *zero_pad_out)
{
    const char *p = *cursor;
    bool zero_pad = false;
    size_t width = 0;

    if (*p == '0')
    {
        zero_pad = true;
        p++;
    }

    while (*p >= '0' && *p <= '9')
    {
        unsigned int digit = (unsigned int)(*p - '0');
        if (width > ((size_t)INT_MAX - digit) / 10u)
            return false;
        width = width * 10u + digit;
        p++;
    }

    *cursor = p;
    *width_out = width;
    *zero_pad_out = zero_pad;
    return true;
}

int kvsnprintf(char *dst, size_t size, const char *fmt, va_list args)
{
    if ((dst == NULL && size > 0) || fmt == NULL)
    {
        if (dst != NULL && size > 0)
            dst[0] = '\0';
        return -1;
    }

    format_writer_t writer = {
        .dst = dst,
        .size = size,
        .stored = 0,
        .required = 0,
        .failed = false,
    };
    if (size > 0)
        dst[0] = '\0';

    va_list copy;
    va_copy(copy, args);
    const char *cursor = fmt;
    while (*cursor && !writer.failed)
    {
        if (*cursor != '%')
        {
            const char *literal = cursor;
            while (*cursor && *cursor != '%')
                cursor++;
            format_writer_bytes(&writer, literal, (size_t)(cursor - literal));
            continue;
        }

        cursor++;
        size_t width = 0;
        bool zero_pad = false;
        if (!format_parse_width(&cursor, &width, &zero_pad))
        {
            writer.failed = true;
            break;
        }

        bool long_long = false;
        if (*cursor == 'l')
        {
            cursor++;
            if (*cursor != 'l')
            {
                writer.failed = true;
                break;
            }
            long_long = true;
            cursor++;
        }
        else if (*cursor == 'h' || *cursor == 'j' ||
                 *cursor == 'z' || *cursor == 't' || *cursor == 'L')
        {
            writer.failed = true;
            break;
        }

        char conversion = *cursor;
        if (conversion == '\0')
        {
            writer.failed = true;
            break;
        }
        cursor++;

        if (conversion == '%')
        {
            if (long_long)
            {
                writer.failed = true;
                break;
            }
            format_writer_repeat(&writer, ' ', width > 1u ? width - 1u : 0u);
            format_writer_bytes(&writer, "%", 1u);
            continue;
        }

        if (conversion == 's')
        {
            if (long_long)
            {
                writer.failed = true;
                break;
            }
            const char *text = va_arg(copy, const char *);
            if (text == NULL)
                text = "(null)";
            size_t length = 0;
            if (!format_string_length(text, &length))
            {
                writer.failed = true;
                break;
            }
            format_writer_repeat(&writer, ' ', width > length ? width - length : 0u);
            format_writer_bytes(&writer, text, length);
            continue;
        }

        if (conversion == 'c')
        {
            if (long_long)
            {
                writer.failed = true;
                break;
            }
            char value = (char)va_arg(copy, int);
            format_writer_repeat(&writer, ' ', width > 1u ? width - 1u : 0u);
            format_writer_bytes(&writer, &value, 1u);
            continue;
        }

        if (conversion == 'd')
        {
            bool negative;
            unsigned long long magnitude;
            if (long_long)
            {
                long long value = va_arg(copy, long long);
                negative = value < 0;
                unsigned long long bits = (unsigned long long)value;
                magnitude = negative ? 0ULL - bits : bits;
            }
            else
            {
                int value = va_arg(copy, int);
                negative = value < 0;
                unsigned int bits = (unsigned int)value;
                magnitude = negative ? (unsigned int)(0u - bits) : bits;
            }
            format_unsigned(&writer, magnitude, 10u, false, negative, false,
                            0u, width, zero_pad);
            continue;
        }

        if (conversion == 'u' || conversion == 'x' || conversion == 'X')
        {
            unsigned long long value = long_long
                ? va_arg(copy, unsigned long long)
                : (unsigned long long)va_arg(copy, unsigned int);
            unsigned int base = conversion == 'u' ? 10u : 16u;
            format_unsigned(&writer, value, base, conversion == 'X', false,
                            false, 0u, width, zero_pad);
            continue;
        }

        if (conversion == 'p')
        {
            if (long_long)
            {
                writer.failed = true;
                break;
            }
            uintptr_t value = (uintptr_t)va_arg(copy, void *);
            format_unsigned(&writer, (unsigned long long)value, 16u, false,
                            false, true, 2u * sizeof(uintptr_t), width,
                            zero_pad);
            continue;
        }

        writer.failed = true;
    }
    va_end(copy);

    if (writer.failed)
    {
        if (size > 0)
            dst[0] = '\0';
        return -1;
    }
    if (size > 0)
        dst[writer.stored] = '\0';
    return (int)writer.required;
}

int ksnprintf(char *dst, size_t size, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int result = kvsnprintf(dst, size, fmt, args);
    va_end(args);
    return result;
}

size_t strlen(const char *str)
{
    size_t len = 0;
    while (str[len])
        len++;
    return len;
}

int strcmp(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2))
    {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    while (n > 0 && *s1 && (*s1 == *s2))
    {
        s1++;
        s2++;
        n--;
    }
    if (n == 0)
        return 0;
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

char *strchr(const char *s, int c)
{
    while (*s != (char)c)
    {
        if (!*s++)
        {
            return NULL;
        }
    }
    return (char *)s;
}

char *strcpy(char *dest, const char *src)
{
    char *saved = dest;
    while (*src)
    {
        *dest++ = *src++;
    }
    *dest = 0;
    return saved;
}

char *strcat(char *dest, const char *src)
{
    char *saved = dest;
    while (*dest)
        dest++;
    while (*src)
        *dest++ = *src++;
    *dest = 0;
    return saved;
}

void strrev(char *str)
{
    int i;
    int j;
    unsigned char a;
    unsigned len = strlen(str);
    for (i = 0, j = len - 1; i < j; i++, j--)
    {
        a = str[i];
        str[i] = str[j];
        str[j] = a;
    }
}

void itoa(int n, char *str)
{
    int i = 0;
    int is_neg = 0;
    unsigned int magnitude;

    if (n == 0)
    {
        str[0] = '0';
        str[1] = '\0';
        return;
    }

    if (n < 0)
    {
        is_neg = 1;
        magnitude = 0u - (unsigned int)n;
    }
    else
        magnitude = (unsigned int)n;

    while (magnitude != 0)
    {
        str[i++] = (char)((magnitude % 10u) + '0');
        magnitude /= 10u;
    }

    if (is_neg)
        str[i++] = '-';
    str[i] = '\0';
    strrev(str);
}

void k_int_to_hex(unsigned long long n, char *str)
{
    str[0] = '0';
    str[1] = 'x';
    int i = 2;
    unsigned long long temp = n;
    int count = 0;

    if (temp == 0)
        count = 1;
    else
    {
        while (temp > 0)
        {
            temp >>= 4;
            count++;
        }
    }

    for (int j = count - 1; j >= 0; j--)
    {
        int nibble = n & 0xF;
        str[i + j] = (nibble < 10) ? (nibble + '0') : (nibble - 10 + 'A');
        n >>= 4;
    }
    str[i + count] = '\0';
}
