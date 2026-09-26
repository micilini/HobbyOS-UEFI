#include "memory.h"

/* GCC's aligned(1) type makes unaligned word loads explicit, while may_alias
   permits the freestanding byte representation of every object to be viewed
   through the general-purpose 64-bit register path. */
typedef uint64_t memory_word_t
    __attribute__((aligned(1), may_alias));

void *memcpy(void *dest, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    while (n != 0 && ((uintptr_t)d & (sizeof(memory_word_t) - 1u)) != 0)
    {
        *d++ = *s++;
        n--;
    }

    while (n >= sizeof(memory_word_t))
    {
        memory_word_t word = *(const memory_word_t *)(const void *)s;
        *(memory_word_t *)(void *)d = word;
        d += sizeof(memory_word_t);
        s += sizeof(memory_word_t);
        n -= sizeof(memory_word_t);
    }

    while (n != 0)
    {
        *d++ = *s++;
        n--;
    }
    return dest;
}

void *memset(void *dest, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dest;

    while (n != 0 && ((uintptr_t)d & (sizeof(memory_word_t) - 1u)) != 0)
    {
        *d++ = (uint8_t)c;
        n--;
    }

    memory_word_t word = (uint8_t)c;
    word *= UINT64_C(0x0101010101010101);
    while (n >= sizeof(memory_word_t))
    {
        *(memory_word_t *)(void *)d = word;
        d += sizeof(memory_word_t);
        n -= sizeof(memory_word_t);
    }

    while (n != 0)
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

    if (d == s || n == 0)
        return dest;

    if ((uintptr_t)d < (uintptr_t)s)
    {
        while (n != 0 &&
               ((uintptr_t)d & (sizeof(memory_word_t) - 1u)) != 0)
        {
            *d++ = *s++;
            n--;
        }
        while (n >= sizeof(memory_word_t))
        {
            memory_word_t word = *(const memory_word_t *)(const void *)s;
            *(memory_word_t *)(void *)d = word;
            d += sizeof(memory_word_t);
            s += sizeof(memory_word_t);
            n -= sizeof(memory_word_t);
        }
        while (n != 0)
        {
            *d++ = *s++;
            n--;
        }
    }
    else
    {
        d += n;
        s += n;
        while (n != 0 &&
               ((uintptr_t)d & (sizeof(memory_word_t) - 1u)) != 0)
        {
            *--d = *--s;
            n--;
        }
        while (n >= sizeof(memory_word_t))
        {
            d -= sizeof(memory_word_t);
            s -= sizeof(memory_word_t);
            memory_word_t word = *(const memory_word_t *)(const void *)s;
            *(memory_word_t *)(void *)d = word;
            n -= sizeof(memory_word_t);
        }
        while (n != 0)
        {
            *--d = *--s;
            n--;
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
