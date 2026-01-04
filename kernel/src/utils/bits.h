#ifndef BITS_H
#define BITS_H

#include <stdint.h>




#define BIT_SET(x, n)    ((x) |= (1ULL << (n)))
#define BIT_CLEAR(x, n)  ((x) &= ~(1ULL << (n)))
#define BIT_TEST(x, n)   (((x) & (1ULL << (n))) != 0)
#define BIT_TOGGLE(x, n) ((x) ^= (1ULL << (n)))



#define ALIGN_UP(x, align) (((x) + ((align) - 1)) & ~((align) - 1))
#define ALIGN_DOWN(x, align) ((x) & ~((align) - 1))

#endif