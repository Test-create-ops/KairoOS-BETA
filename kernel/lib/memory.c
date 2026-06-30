#include "memory.h"

void memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
}

void memset(void *dst, int val, unsigned long n)
{
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)val;
}
