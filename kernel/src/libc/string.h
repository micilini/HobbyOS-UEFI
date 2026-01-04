#ifndef STRING_H
#define STRING_H

#include <stddef.h>


size_t strlen(const char* str);


int strcmp(const char* s1, const char* s2);


int strncmp(const char* s1, const char* s2, size_t n);


char* strchr(const char* s, int c);


char* strcpy(char* dest, const char* src);


char* strcat(char* dest, const char* src);


void strrev(char* str);


void itoa(int n, char* str);


void k_int_to_hex(unsigned long long n, char* str);

#endif