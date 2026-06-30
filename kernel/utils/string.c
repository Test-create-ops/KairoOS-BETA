#include "string.h"

int kstrcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return *(unsigned char*)a - *(unsigned char*)b;
}

int kstrncmp(const char *a, const char *b, int n) {
    while (n-- && *a && (*a == *b)) { a++; b++; }
    if (n < 0) return 0;
    return *(unsigned char*)a - *(unsigned char*)b;
}

int kstrlen(const char *s) {
    int n = 0;
    while (*s++) n++;
    return n;
}
