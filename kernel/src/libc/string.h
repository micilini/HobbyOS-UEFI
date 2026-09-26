#ifndef STRING_H
#define STRING_H

#include <stdarg.h>
#include <stddef.h>

#if defined(__GNUC__) || defined(__clang__)
#define KERNEL_FORMAT_ATTRIBUTE(kind, format_index, first_argument) \
    __attribute__((format(kind, format_index, first_argument)))
#else
#define KERNEL_FORMAT_ATTRIBUTE(kind, format_index, first_argument)
#endif

size_t strlen(const char *str);

int strcmp(const char *s1, const char *s2);

int strncmp(const char *s1, const char *s2, size_t n);

char *strchr(const char *s, int c);

char *strcpy(char *dest, const char *src);

char *strcat(char *dest, const char *src);

void strrev(char *str);

void itoa(int n, char *str);

void k_int_to_hex(unsigned long long n, char *str);

int kvsnprintf(char *dst, size_t size, const char *fmt, va_list args)
    KERNEL_FORMAT_ATTRIBUTE(printf, 3, 0);

int ksnprintf(char *dst, size_t size, const char *fmt, ...)
    KERNEL_FORMAT_ATTRIBUTE(printf, 3, 4);

#undef KERNEL_FORMAT_ATTRIBUTE

#endif
