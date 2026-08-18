#include "heap.h"
#include "paging.h"

extern char _end[];

typedef struct block_header {
    size_t size;
    int free;
    struct block_header *next;
} block_header_t;

// Round header size up to 16 bytes for SSE alignment
#define HDR_SIZE ((sizeof(block_header_t) + 15) & ~(size_t)15)

static block_header_t *heap_head = NULL;

void kheap_init(void)
{
    uint64_t start = ((uint64_t)_end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    heap_head = (block_header_t *)start;
    heap_head->size = HEAP_INITIAL_SIZE - HDR_SIZE;
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

    // SSE requires 16-byte aligned addresses for movaps
    size_t aligned_size = (size + 15) & ~(size_t)15;

    block_header_t *block = find_free_block(aligned_size);
    if (!block) return NULL;

    block->free = 0;

    if (block->size > aligned_size + HDR_SIZE) {
        block_header_t *new_block = (block_header_t *)((uint8_t *)block + HDR_SIZE + aligned_size);
        new_block->size = block->size - aligned_size - HDR_SIZE;
        new_block->free = 1;
        new_block->next = block->next;

        block->size = aligned_size;
        block->next = new_block;
    }

    return (uint8_t *)block + sizeof(block_header_t);
}

void kfree(void *ptr)
{
    if (!ptr) return;

    block_header_t *block = (block_header_t *)((uint8_t *)ptr - HDR_SIZE);
    block->free = 1;

    block_header_t *curr = heap_head;
    while (curr) {
        if (curr->free && curr->next && curr->next->free) {
            curr->size += HDR_SIZE + curr->next->size;
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
}
