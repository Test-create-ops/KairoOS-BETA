#include "heap.h"
#include "paging.h"

#define HEAP_PHYS_START 0x180000
#define HEAP_INITIAL_SIZE (1024 * 1024)

typedef struct block_header {
    size_t size;
    int free;
    struct block_header *next;
} block_header_t;

static block_header_t *heap_head = NULL;

void kheap_init(void)
{
    heap_head = (block_header_t *)HEAP_PHYS_START;
    heap_head->size = HEAP_INITIAL_SIZE - sizeof(block_header_t);
    heap_head->free = 1;
    heap_head->next = NULL;
}

static block_header_t *find_free_block(size_t size)
{
    block_header_t *curr = heap_head;
    while (curr) {
        if (curr->free && curr->size >= size)
            return curr;
        curr = curr->next;
    }
    return NULL;
}

void *kmalloc(size_t size)
{
    if (size == 0) return NULL;

    block_header_t *block = find_free_block(size);
    if (!block) return NULL;

    block->free = 0;

    if (block->size > size + sizeof(block_header_t)) {
        block_header_t *new_block = (block_header_t *)((uint8_t *)block + sizeof(block_header_t) + size);
        new_block->size = block->size - size - sizeof(block_header_t);
        new_block->free = 1;
        new_block->next = block->next;

        block->size = size;
        block->next = new_block;
    }

    return (uint8_t *)block + sizeof(block_header_t);
}

void kfree(void *ptr)
{
    if (!ptr) return;

    block_header_t *block = (block_header_t *)((uint8_t *)ptr - sizeof(block_header_t));
    block->free = 1;

    block_header_t *curr = heap_head;
    while (curr) {
        if (curr->free && curr->next && curr->next->free) {
            curr->size += sizeof(block_header_t) + curr->next->size;
            curr->next = curr->next->next;
        }
        curr = curr->next;
    }
}
