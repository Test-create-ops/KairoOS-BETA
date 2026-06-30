#include "allocator.h"

static unsigned char heap[1024*1024];
static unsigned int offset = 0;

void *kmalloc(unsigned int size) {
    void *ptr = &heap[offset];
    offset += size;
    return ptr;
}

void kfree(void *ptr) {
    (void)ptr;
}
