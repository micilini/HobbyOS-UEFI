#include "string.h"

size_t strlen(const char* str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}


int strncmp(const char* s1, const char* s2, size_t n) {
    while (n > 0 && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}


char* strchr(const char* s, int c) {
    while (*s != (char)c) {
        if (!*s++) {
            return NULL;
        }
    }
    return (char*)s;
}

char* strcpy(char* dest, const char* src) {
    char* saved = dest;
    while (*src) {
        *dest++ = *src++;
    }
    *dest = 0;
    return saved;
}

char* strcat(char* dest, const char* src) {
    char* saved = dest;
    while (*dest) dest++;
    while (*src) *dest++ = *src++;
    *dest = 0;
    return saved;
}

void strrev(char* str) {
    int i;
    int j;
    unsigned char a;
    unsigned len = strlen(str);
    for (i = 0, j = len - 1; i < j; i++, j--) {
        a = str[i];
        str[i] = str[j];
        str[j] = a;
    }
}

void itoa(int n, char* str) {
    int i = 0;
    int is_neg = 0;

    if (n == 0) {
        str[0] = '0';
        str[1] = '\0';
        return;
    }

    if (n < 0) {
        is_neg = 1;
        n = -n;
    }

    while (n != 0) {
        str[i++] = (n % 10) + '0';
        n = n / 10;
    }

    if (is_neg) str[i++] = '-';
    str[i] = '\0';
    strrev(str);
}

void k_int_to_hex(unsigned long long n, char* str) {
    str[0] = '0';
    str[1] = 'x';
    int i = 2;
    unsigned long long temp = n;
    int count = 0;
    
    
    if (temp == 0) count = 1;
    else {
        while (temp > 0) {
            temp >>= 4;
            count++;
        }
    }

    
    for (int j = count - 1; j >= 0; j--) {
        int nibble = n & 0xF;
        str[i + j] = (nibble < 10) ? (nibble + '0') : (nibble - 10 + 'A');
        n >>= 4;
    }
    str[i + count] = '\0';
}