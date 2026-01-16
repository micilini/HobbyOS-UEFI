#include "utils.h"

int memcmp(const void *a, const void *b, UINTN n)
{
    const unsigned char *s1 = (const unsigned char *)a;
    const unsigned char *s2 = (const unsigned char *)b;

    for (UINTN i = 0; i < n; i++)
    {
        if (s1[i] != s2[i])
        {
            return s1[i] - s2[i];
        }
    }
    return 0;
}