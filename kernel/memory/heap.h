#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>

#define HEAP_INITIAL_SIZE (8 * 1024 * 1024)

void kheap_init(void);
void *kmalloc(size_t size);
void kfree(void *ptr);

#endif
