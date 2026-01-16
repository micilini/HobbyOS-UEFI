#include "memory.h"

void *memcpy(void *dest, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++)
    {
        d[i] = s[i];
    }
    return dest;
}

void *memset(void *dest, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dest;

    if (c == 0 && n >= 8)
    {

        while ((uintptr_t)d % 8 != 0 && n > 0)
        {
            *d++ = 0;
            n--;
        }

        uint64_t *d64 = (uint64_t *)d;
        while (n >= 8)
        {
            *d64++ = 0;
            n -= 8;
        }
        d = (uint8_t *)d64;
    }

    while (n > 0)
    {
        *d++ = (uint8_t)c;
        n--;
    }
    return dest;
}

void *memmove(void *dest, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    if (d < s)
    {
        for (size_t i = 0; i < n; i++)
        {
            d[i] = s[i];
        }
    }
    else
    {
        for (size_t i = n; i > 0; i--)
        {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    for (size_t i = 0; i < n; i++)
    {
        if (p1[i] != p2[i])
        {
            return p1[i] - p2[i];
        }
    }
    return 0;
}